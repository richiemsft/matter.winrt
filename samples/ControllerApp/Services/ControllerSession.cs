using Matter.Windows.Controller;
using MatterControllerApp.Models;
using System.Text.Json;

namespace MatterControllerApp.Services;

public sealed class ControllerSession
{
    private readonly SemaphoreSlim lifecycleGate = new(1, 1);
    private readonly string storagePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "MatterControllerApp");
    private readonly Dictionary<string, string> deviceNames = [];
    private MatterControllerCommissioning? detailedCommissioning;

    public MatterController? Controller { get; private set; }
    public string? InitializationError { get; private set; }

    public event EventHandler<string>? CommissioningProgress;
    public event EventHandler? NodesChanged;
    public event EventHandler? StateChanged;

    public bool IsInitialized => Controller is not null;

    public async Task InitializeAsync()
    {
        await lifecycleGate.WaitAsync();
        try
        {
            if (Controller is not null)
            {
                return;
            }

            try
            {
                LoadDeviceNames();
                ControllerOptions options = new()
                {
                    StoragePath = storagePath,
                    AllowTestAttestation = true
                };
                Controller = await MatterController.CreateAsync(options);
                Controller.CommissioningProgress += OnCommissioningProgress;
                detailedCommissioning = new MatterControllerCommissioning(Controller);
                detailedCommissioning.ProgressChanged += OnDetailedCommissioningProgress;
                InitializationError = null;
                NodesChanged?.Invoke(this, EventArgs.Empty);
            }
            catch (Exception exception)
            {
                InitializationError = exception.Message;
            }
            finally
            {
                StateChanged?.Invoke(this, EventArgs.Empty);
            }
        }
        finally
        {
            lifecycleGate.Release();
        }
    }

    public IReadOnlyList<KnownDevice> GetKnownDevices()
    {
        MatterController controller = RequireController();
        return controller.CommissionedNodes
            .Select(CreateKnownDevice)
            .OrderBy(device => device.DisplayName, StringComparer.CurrentCultureIgnoreCase)
            .ToList();
    }

    public async Task<AddedDeviceResult> CommissionOnNetworkAsync(
        string setupCode,
        uint setupPinCode,
        ushort longDiscriminator)
    {
        MatterController controller = RequireController();
        ulong nodeId = AllocateNodeId(controller);
        CommissionedNode node;
        try
        {
            node = await controller.CommissionOnNetworkAsync(new OnNetworkCommissioningParameters
            {
                NodeId = nodeId,
                SetupCode = setupCode.Trim(),
                SetupPinCode = setupPinCode,
                LongDiscriminator = longDiscriminator
            });
        }
        catch (Exception exception) when (
            exception.Message.Contains("already exists", StringComparison.OrdinalIgnoreCase))
        {
            node = await RecoverAfterDuplicateFabricAsync(controller, nodeId, exception);
        }
        return await RegisterCommissionedNodeAsync(node);
    }

    public async Task<AddedDeviceResult> CommissionBleAsync(
        uint setupPinCode,
        ushort longDiscriminator,
        string wiFiSsid,
        string wiFiPassphrase)
    {
        MatterController controller = RequireController();
        ulong nodeId = AllocateNodeId(controller);
        CommissionedNode node;
        try
        {
            MatterCommissioningResult result = await RequireDetailedCommissioning().CommissionBleAsync(
                new BleNetworkCommissioningParameters
            {
                NodeId = nodeId,
                SetupPinCode = setupPinCode,
                LongDiscriminator = longDiscriminator,
                WiFi = new WiFiNetworkCredentials(wiFiSsid, wiFiPassphrase)
            },
                new MatterNetworkInterfaceSelection(MatterNetworkInterfaceSelectionMode.Automatic, 0));
            if (!result.Succeeded || result.Node is null)
            {
                throw new InvalidOperationException(result.DiagnosticMessage);
            }
            node = result.Node;
        }
        catch (Exception exception) when (
            exception.Message.Contains("already exists", StringComparison.OrdinalIgnoreCase))
        {
            node = await RecoverAfterDuplicateFabricAsync(controller, nodeId, exception);
        }
        return await RegisterCommissionedNodeAsync(node);
    }

    public async Task<AddedDeviceResult> RecoverNodeAsync(ulong nodeId)
    {
        CommissionedNode node = await new MatterControllerRecovery(RequireController()).RecoverNodeAsync(nodeId);
        return await RegisterCommissionedNodeAsync(node);
    }

    public async Task<AddedDeviceResult> RenameDeviceAsync(
        KnownDevice device,
        string displayName)
    {
        string name = displayName.Trim();
        if (name.Length == 0)
        {
            throw new InvalidOperationException("Enter a device name.");
        }

        deviceNames[GetDeviceKey(device.FabricIndex, device.NodeId)] = name;
        string? warning = null;
        try
        {
            await SaveDeviceNamesAsync();
        }
        catch (Exception exception)
        {
            warning =
                $"The display name could not be saved and may be lost when the app closes: {exception.Message}";
        }

        KnownDevice renamedDevice = device with { DisplayName = name };
        NodesChanged?.Invoke(this, EventArgs.Empty);
        return new AddedDeviceResult(renamedDevice, warning);
    }

    public async Task RemoveNodeAsync(ulong nodeId)
    {
        MatterController controller = RequireController();
        CommissionedNode? node = controller.CommissionedNodes.FirstOrDefault(candidate => candidate.NodeId == nodeId);
        try
        {
            await controller.RemoveNodeAsync(nodeId);
            if (node is not null)
            {
                deviceNames.Remove(GetDeviceKey(node.FabricIndex, node.NodeId));
                await SaveDeviceNamesAsync();
                new WidgetSelectionStore().RemoveNode(node.FabricIndex, node.NodeId);
            }
        }
        finally
        {
            NodesChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public async Task CloseAsync()
    {
        await lifecycleGate.WaitAsync();
        try
        {
            if (Controller is null)
            {
                return;
            }

            Controller.CommissioningProgress -= OnCommissioningProgress;
            if (detailedCommissioning is not null)
            {
                detailedCommissioning.ProgressChanged -= OnDetailedCommissioningProgress;
                detailedCommissioning = null;
            }
            await Controller.CloseAsync();
            Controller = null;
            NodesChanged?.Invoke(this, EventArgs.Empty);
            StateChanged?.Invoke(this, EventArgs.Empty);
        }
        finally
        {
            lifecycleGate.Release();
        }
    }

    public MatterController RequireController() =>
        Controller ?? throw new InvalidOperationException("The Matter controller is not initialized.");

    private void OnCommissioningProgress(MatterController sender, CommissioningProgressEventArgs args) =>
        CommissioningProgress?.Invoke(this, $"{args.Stage}: {args.Message}");

    private void OnDetailedCommissioningProgress(
        MatterControllerCommissioning sender,
        MatterCommissioningProgress args) =>
        CommissioningProgress?.Invoke(
            this,
            $"{args.Stage}: {args.DisplayMessage} ({args.ElapsedTime.TotalSeconds:F1}s)");

    private MatterControllerCommissioning RequireDetailedCommissioning() =>
        detailedCommissioning ?? throw new InvalidOperationException("The Matter controller is not initialized.");

    private static ulong AllocateNodeId(MatterController controller)
    {
        HashSet<ulong> existing = controller.CommissionedNodes.Select(node => node.NodeId).ToHashSet();
        for (ulong candidate = 1; candidate != 0; candidate++)
        {
            if (!existing.Contains(candidate))
            {
                return candidate;
            }
        }
        throw new InvalidOperationException("No Matter node identifiers are available.");
    }

    private KnownDevice CreateKnownDevice(CommissionedNode node)
    {
        string key = GetDeviceKey(node.FabricIndex, node.NodeId);
        string name = deviceNames.GetValueOrDefault(key, "Matter device");
        return new KnownDevice(node.NodeId, node.FabricIndex, name);
    }

    private async Task<AddedDeviceResult> RegisterCommissionedNodeAsync(CommissionedNode node)
    {
        string? warning = null;
        string displayName;
        try
        {
            BasicInformation information =
                await RequireController().GetBasicInformationCluster(node.NodeId, 0).ReadAsync();
            displayName = FirstNonEmpty(information.NodeLabel, information.ProductName, "Matter device");
        }
        catch (Exception exception)
        {
            displayName = "Matter device";
            warning = $"The device was added, but its name could not be read: {exception.Message}";
        }

        deviceNames[GetDeviceKey(node.FabricIndex, node.NodeId)] = displayName;
        try
        {
            await SaveDeviceNamesAsync();
        }
        catch (Exception exception)
        {
            warning = AppendWarning(
                warning,
                $"The display name could not be saved and may be lost when the app closes: {exception.Message}");
        }

        KnownDevice device = new(node.NodeId, node.FabricIndex, displayName);
        NodesChanged?.Invoke(this, EventArgs.Empty);
        return new AddedDeviceResult(device, warning);
    }

    private static async Task<CommissionedNode> RecoverAfterDuplicateFabricAsync(
        MatterController controller,
        ulong nodeId,
        Exception commissioningException)
    {
        try
        {
            return await new MatterControllerRecovery(controller).RecoverNodeAsync(nodeId);
        }
        catch (Exception recoveryException)
        {
            throw new InvalidOperationException(
                $"The device is already on this controller fabric, but node {nodeId} " +
                $"could not be restored: {recoveryException.Message}",
                new AggregateException(commissioningException, recoveryException));
        }
    }

    private void LoadDeviceNames()
    {
        deviceNames.Clear();
        string path = GetDeviceNamesPath();
        if (!File.Exists(path))
        {
            return;
        }

        Dictionary<string, string>? storedNames =
            JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(path));
        if (storedNames is null)
        {
            throw new InvalidDataException($"Device name storage is invalid: {path}");
        }

        foreach ((string key, string value) in storedNames)
        {
            if (!string.IsNullOrWhiteSpace(value))
            {
                deviceNames[key] = value;
            }
        }
    }

    private async Task SaveDeviceNamesAsync()
    {
        Directory.CreateDirectory(storagePath);
        string path = GetDeviceNamesPath();
        string temporaryPath = path + ".tmp";
        string json = JsonSerializer.Serialize(deviceNames, new JsonSerializerOptions { WriteIndented = true });
        await File.WriteAllTextAsync(temporaryPath, json);
        File.Move(temporaryPath, path, true);
    }

    private string GetDeviceNamesPath() => Path.Combine(storagePath, "device-names.json");

    private static string GetDeviceKey(ushort fabricIndex, ulong nodeId) =>
        $"{fabricIndex}:{nodeId}";

    private static string FirstNonEmpty(params string[] values) =>
        values.First(value => !string.IsNullOrWhiteSpace(value));

    private static string AppendWarning(string? current, string warning) =>
        string.IsNullOrEmpty(current) ? warning : $"{current} {warning}";
}
