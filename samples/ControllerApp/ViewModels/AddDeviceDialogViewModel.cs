using CommunityToolkit.Mvvm.ComponentModel;
using MatterControllerApp.Models;
using MatterControllerApp.Services;
using Microsoft.UI.Dispatching;
using System.Collections.ObjectModel;

namespace MatterControllerApp.ViewModels;

public sealed class AddDeviceDialogViewModel : ObservableObject, IDisposable
{
    private readonly ControllerSession session;
    private readonly DispatcherQueue dispatcherQueue = DispatcherQueue.GetForCurrentThread();
    private AddedDeviceResult? connectedDevice;
    private CancellationTokenSource? scanCancellation;
    private bool camerasLoaded;

    public AddDeviceDialogViewModel(ControllerSession session)
    {
        this.session = session;
        session.CommissioningProgress += OnCommissioningProgress;
    }

    private int connectionMethodIndex;
    public int ConnectionMethodIndex
    {
        get => connectionMethodIndex;
        set
        {
            if (SetProperty(ref connectionMethodIndex, value))
            {
                OnPropertyChanged(nameof(UsesSharingCode));
                OnPropertyChanged(nameof(UsesBluetooth));
            }
        }
    }

    private string deviceName = string.Empty;
    public string DeviceName
    {
        get => deviceName;
        set => SetProperty(ref deviceName, value);
    }

    private string sharingCode = string.Empty;
    public string SharingCode
    {
        get => sharingCode;
        set => SetProperty(ref sharingCode, value);
    }

    private uint setupPinCode = 20202021;
    public uint SetupPinCode
    {
        get => setupPinCode;
        set => SetProperty(ref setupPinCode, value);
    }

    private ushort longDiscriminator = 3840;
    public ushort LongDiscriminator
    {
        get => longDiscriminator;
        set => SetProperty(ref longDiscriminator, value);
    }

    private string wiFiSsid = string.Empty;
    public string WiFiSsid
    {
        get => wiFiSsid;
        set => SetProperty(ref wiFiSsid, value);
    }

    private string wiFiPassphrase = string.Empty;
    public string WiFiPassphrase
    {
        get => wiFiPassphrase;
        set => SetProperty(ref wiFiPassphrase, value);
    }

    private string status = "Ready to connect.";
    public string Status
    {
        get => status;
        private set => SetProperty(ref status, value);
    }

    private bool isBusy;
    public bool IsBusy
    {
        get => isBusy;
        private set
        {
            if (SetProperty(ref isBusy, value))
            {
                OnPropertyChanged(nameof(CanChangeConnection));
                OnPropertyChanged(nameof(CanStartScan));
            }
        }
    }

    private QrCamera? selectedCamera;
    public QrCamera? SelectedCamera
    {
        get => selectedCamera;
        set
        {
            if (SetProperty(ref selectedCamera, value))
            {
                OnPropertyChanged(nameof(CanStartScan));
            }
        }
    }

    public ObservableCollection<QrCamera> Cameras { get; } = [];

    public bool UsesSharingCode => ConnectionMethodIndex == 0;
    public bool UsesBluetooth => ConnectionMethodIndex == 1;
    public bool IsConnecting => connectedDevice is null;
    public bool IsNaming => connectedDevice is not null;
    public bool IsScanning => scanCancellation is not null;
    public bool CanChangeConnection => !IsBusy;
    public bool CanStartScan => IsScanning || (!IsBusy && SelectedCamera is not null);
    public string ScanButtonText => IsScanning ? "Cancel scan" : "Scan Matter QR code";

    public async Task LoadCamerasAsync()
    {
        if (camerasLoaded || IsBusy)
        {
            return;
        }

        camerasLoaded = true;
        IsBusy = true;
        Status = "Finding cameras…";
        try
        {
            IReadOnlyList<QrCamera> cameras = await new QrCodeScanner().GetCamerasAsync();
            Cameras.Clear();
            foreach (QrCamera camera in cameras)
            {
                Cameras.Add(camera);
            }
            SelectedCamera = Cameras.FirstOrDefault();
            Status = SelectedCamera is null
                ? "No camera-backed barcode scanner is available on this computer."
                : "Ready to scan or connect.";
        }
        catch (Exception exception)
        {
            Status = $"Could not find cameras: {exception.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    public async Task<AddedDeviceResult> ConnectAsync()
    {
        if (IsBusy)
        {
            throw new InvalidOperationException("A device connection is already in progress.");
        }
        if (UsesSharingCode)
        {
            SharingCode = NormalizeSetupCode(SharingCode);
            if (SharingCode.Length == 0)
            {
                throw new InvalidOperationException("Enter the device's sharing code.");
            }
        }
        else if (string.IsNullOrWhiteSpace(WiFiSsid))
        {
            throw new InvalidOperationException("Enter the Wi-Fi network name.");
        }
        IsBusy = true;
        Status = UsesSharingCode
            ? "Connecting with the sharing code…"
            : "Connecting over Bluetooth LE…";
        try
        {
            await App.ControllerInitialization;
            connectedDevice = UsesSharingCode
                ? await session.CommissionOnNetworkAsync(
                    SharingCode,
                    SetupPinCode,
                    LongDiscriminator)
                : await session.CommissionBleAsync(
                    SetupPinCode,
                    LongDiscriminator,
                    WiFiSsid,
                    WiFiPassphrase);
            DeviceName = connectedDevice.Device.DisplayName;
            OnPropertyChanged(nameof(IsConnecting));
            OnPropertyChanged(nameof(IsNaming));
            Status = "Connected. Keep the suggested name or edit it.";
            return connectedDevice;
        }
        catch (Exception exception)
        {
            Status = $"Could not add the device: {exception.Message}";
            throw;
        }
        finally
        {
            if (UsesBluetooth)
            {
                WiFiPassphrase = string.Empty;
            }
            IsBusy = false;
        }
    }

    public async Task<bool> ScanQrCodeAsync()
    {
        if (IsBusy)
        {
            return false;
        }
        if (SelectedCamera is null)
        {
            throw new InvalidOperationException("Select a camera before scanning.");
        }

        IsBusy = true;
        scanCancellation = new CancellationTokenSource();
        OnPropertyChanged(nameof(IsScanning));
        OnPropertyChanged(nameof(CanStartScan));
        OnPropertyChanged(nameof(ScanButtonText));
        Status = "Point the camera at the device's Matter QR code.";
        try
        {
            Progress<string> scanProgress = new(value =>
            {
                if (value.StartsWith("MT:", StringComparison.Ordinal))
                {
                    ApplyScannedCode(value);
                }
            });
            string value = await new QrCodeScanner().ScanAsync(
                SelectedCamera.ScannerId,
                scanProgress,
                scanCancellation.Token);
            if (!value.StartsWith("MT:", StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    "The scanned QR code is not a Matter setup payload.");
            }
            ApplyScannedCode(value);
            return true;
        }
        catch (OperationCanceledException)
        {
            Status = "QR scanning was canceled.";
            return false;
        }
        catch (Exception exception)
        {
            Status = $"Could not scan a QR code: {exception.Message}";
            throw;
        }
        finally
        {
            scanCancellation.Dispose();
            scanCancellation = null;
            OnPropertyChanged(nameof(IsScanning));
            OnPropertyChanged(nameof(CanStartScan));
            OnPropertyChanged(nameof(ScanButtonText));
            IsBusy = false;
        }
    }

    public async Task<AddedDeviceResult> SaveNameAsync()
    {
        if (connectedDevice is null)
        {
            throw new InvalidOperationException("Connect the device before naming it.");
        }
        if (string.IsNullOrWhiteSpace(DeviceName))
        {
            throw new InvalidOperationException("Enter a device name.");
        }

        IsBusy = true;
        Status = "Saving device name…";
        try
        {
            connectedDevice =
                await session.RenameDeviceAsync(connectedDevice.Device, DeviceName);
            Status = $"{connectedDevice.Device.DisplayName} was added.";
            return connectedDevice;
        }
        catch (Exception exception)
        {
            Status = $"Could not save the device name: {exception.Message}";
            throw;
        }
        finally
        {
            IsBusy = false;
        }
    }

    public void CancelQrScan() => scanCancellation?.Cancel();

    public void Dispose()
    {
        CancelQrScan();
        session.CommissioningProgress -= OnCommissioningProgress;
    }

    private void ApplyScannedCode(string value)
    {
        SharingCode = value;
        Status = "Matter QR code scanned. Select Connect to continue.";
    }

    private static string NormalizeSetupCode(string value)
    {
        string trimmed = value.Trim();
        if (trimmed.StartsWith("MT:", StringComparison.OrdinalIgnoreCase))
        {
            return string.Concat(trimmed.Where(character => !char.IsWhiteSpace(character)));
        }

        return string.Concat(trimmed.Where(character =>
            character != '-' && !char.IsWhiteSpace(character)));
    }

    private void OnCommissioningProgress(object? sender, string message)
    {
        if (dispatcherQueue.HasThreadAccess)
        {
            Status = message;
        }
        else
        {
            dispatcherQueue.TryEnqueue(() => Status = message);
        }
    }
}
