using Windows.Devices.Enumeration;
using Windows.Devices.PointOfService;
using Windows.Security.Cryptography;

namespace MatterControllerApp.Services;

public sealed class QrCodeScanner
{
    public async Task<IReadOnlyList<QrCamera>> GetCamerasAsync()
    {
        string selector = BarcodeScanner.GetDeviceSelector(PosConnectionTypes.Local);
        DeviceInformationCollection devices = await DeviceInformation.FindAllAsync(selector);
        List<QrCamera> cameras = [];
        foreach (DeviceInformation device in devices)
        {
            using BarcodeScanner? scanner = await BarcodeScanner.FromIdAsync(device.Id);
            if (!string.IsNullOrWhiteSpace(scanner?.VideoDeviceId))
            {
                DeviceInformation camera =
                    await DeviceInformation.CreateFromIdAsync(scanner.VideoDeviceId);
                string displayName = string.IsNullOrWhiteSpace(camera.Name)
                    ? device.Name
                    : camera.Name;
                cameras.Add(new QrCamera(device.Id, displayName));
            }
        }

        return cameras;
    }

    public async Task<string> ScanAsync(
        string scannerId,
        IProgress<string>? scanProgress = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(scannerId);
        using BarcodeScanner? scanner = await BarcodeScanner.FromIdAsync(scannerId);
        if (scanner is null || string.IsNullOrWhiteSpace(scanner.VideoDeviceId))
        {
            throw new InvalidOperationException(
                "The selected camera is no longer available.");
        }

        using ClaimedBarcodeScanner? claimedScanner = await scanner.ClaimScannerAsync();
        if (claimedScanner is null)
        {
            throw new InvalidOperationException("The camera barcode scanner could not be claimed.");
        }

        TaskCompletionSource<string> completion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        int scannerClosed = 0;
        bool previewShown = false;
        bool softwareTriggerStarted = false;

        void OnDataReceived(
            ClaimedBarcodeScanner sender,
            BarcodeScannerDataReceivedEventArgs args)
        {
            string value = CryptographicBuffer.ConvertBinaryToString(
                BinaryStringEncoding.Utf8,
                args.Report.ScanDataLabel);
            value = value.TrimEnd('\0', '\r', '\n');
            if (!completion.Task.IsCompleted)
            {
                scanProgress?.Report(value);
                completion.TrySetResult(value);
            }
        }

        void OnClosed(
            ClaimedBarcodeScanner sender,
            ClaimedBarcodeScannerClosedEventArgs args)
        {
            Interlocked.Exchange(ref scannerClosed, 1);
            completion.TrySetCanceled();
        }

        claimedScanner.IsDecodeDataEnabled = true;
        claimedScanner.IsDisabledOnDataReceived = true;
        claimedScanner.DataReceived += OnDataReceived;
        claimedScanner.Closed += OnClosed;
        using CancellationTokenRegistration cancellation =
            cancellationToken.Register(() => completion.TrySetCanceled(cancellationToken));

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            await claimedScanner.EnableAsync();
            cancellationToken.ThrowIfCancellationRequested();
            if (scanner.Capabilities.IsSoftwareTriggerSupported)
            {
                await claimedScanner.StartSoftwareTriggerAsync();
                softwareTriggerStarted = true;
            }
            cancellationToken.ThrowIfCancellationRequested();
            await claimedScanner.ShowVideoPreviewAsync();
            previewShown = true;
            return await completion.Task;
        }
        finally
        {
            claimedScanner.DataReceived -= OnDataReceived;
            claimedScanner.Closed -= OnClosed;
            if (Volatile.Read(ref scannerClosed) == 0)
            {
                if (previewShown)
                {
                    claimedScanner.HideVideoPreview();
                }
                if (softwareTriggerStarted)
                {
                    await claimedScanner.StopSoftwareTriggerAsync();
                }
                if (claimedScanner.IsEnabled)
                {
                    await claimedScanner.DisableAsync();
                }
            }
        }
    }
}

public sealed record QrCamera(string ScannerId, string DisplayName);
