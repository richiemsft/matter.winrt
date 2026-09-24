using MatterControllerApp.Models;
using System.Text.Json;

namespace MatterControllerApp.Services;

public sealed class WidgetSelectionStore
{
    private const string MutexName = @"Local\MatterControllerApp.WidgetSelections";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true
    };

    private readonly string storagePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "MatterControllerApp");

    public IReadOnlyList<WidgetDevice> GetDevices() =>
        WithLock(ReadDevices);

    public bool Contains(ulong nodeId, ushort endpointId) =>
        GetDevices().Any(device =>
            device.NodeId == nodeId && device.EndpointId == endpointId);

    public void Add(WidgetDevice device)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(device.DisplayName);
        WithLock(() =>
        {
            List<WidgetDevice> devices = ReadDevices();
            int existingIndex = devices.FindIndex(candidate => candidate.Key == device.Key);
            if (existingIndex >= 0)
            {
                device = device with { IsOn = devices[existingIndex].IsOn };
                devices[existingIndex] = device;
            }
            else
            {
                devices.Add(device);
            }
            WriteDevices(devices);
        });
    }

    public void UpdateState(string key, bool isOn)
    {
        WithLock(() =>
        {
            List<WidgetDevice> devices = ReadDevices();
            int index = devices.FindIndex(device => device.Key == key);
            if (index < 0)
            {
                return;
            }
            devices[index] = devices[index] with { IsOn = isOn };
            WriteDevices(devices);
        });
    }

    public void Remove(ushort fabricIndex, ulong nodeId, ushort endpointId)
    {
        WithLock(() =>
        {
            List<WidgetDevice> devices = ReadDevices();
            if (devices.RemoveAll(device =>
                    device.FabricIndex == fabricIndex &&
                    device.NodeId == nodeId &&
                    device.EndpointId == endpointId) > 0)
            {
                WriteDevices(devices);
            }
        });
    }

    public void RemoveNode(ushort fabricIndex, ulong nodeId)
    {
        WithLock(() =>
        {
            List<WidgetDevice> devices = ReadDevices();
            if (devices.RemoveAll(device =>
                    device.FabricIndex == fabricIndex && device.NodeId == nodeId) > 0)
            {
                WriteDevices(devices);
            }
        });
    }

    private List<WidgetDevice> ReadDevices()
    {
        string path = GetPath();
        if (!File.Exists(path))
        {
            return [];
        }

        List<WidgetDevice>? devices =
            JsonSerializer.Deserialize<List<WidgetDevice>>(File.ReadAllText(path));
        return devices ??
            throw new InvalidDataException($"Widget device storage is invalid: {path}");
    }

    private void WriteDevices(IReadOnlyList<WidgetDevice> devices)
    {
        Directory.CreateDirectory(storagePath);
        string path = GetPath();
        string temporaryPath = path + ".tmp";
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(devices, JsonOptions));
        File.Move(temporaryPath, path, true);
    }

    private string GetPath() => Path.Combine(storagePath, "widget-devices.json");

    private static T WithLock<T>(Func<T> action)
    {
        using Mutex mutex = new(false, MutexName);
        bool lockTaken;
        try
        {
            lockTaken = mutex.WaitOne(TimeSpan.FromSeconds(5));
        }
        catch (AbandonedMutexException)
        {
            lockTaken = true;
        }
        if (!lockTaken)
        {
            throw new TimeoutException("Timed out waiting for widget device storage.");
        }

        try
        {
            return action();
        }
        finally
        {
            mutex.ReleaseMutex();
        }
    }

    private static void WithLock(Action action) =>
        WithLock(() =>
        {
            action();
            return true;
        });
}
