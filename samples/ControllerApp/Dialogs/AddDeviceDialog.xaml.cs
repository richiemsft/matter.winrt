using MatterControllerApp.Models;
using MatterControllerApp.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MatterControllerApp.Dialogs;

public sealed partial class AddDeviceDialog : ContentDialog, IDisposable
{
    public AddDeviceDialogViewModel ViewModel { get; } = new(App.ControllerSession);

    public AddDeviceDialog()
    {
        InitializeComponent();
    }

    public AddedDeviceResult? Result { get; private set; }

    public static Visibility When(bool value) =>
        value ? Visibility.Visible : Visibility.Collapsed;

    public void Dispose() => ViewModel.Dispose();

    private async void OnLoaded(object sender, RoutedEventArgs args) =>
        await ViewModel.LoadCamerasAsync();

    private async void OnScanQrCodeClicked(object sender, RoutedEventArgs args)
    {
        if (ViewModel.IsScanning)
        {
            ViewModel.CancelQrScan();
            return;
        }

        IsPrimaryButtonEnabled = false;
        StatusInfoBar.Severity = InfoBarSeverity.Informational;
        try
        {
            await ViewModel.ScanQrCodeAsync();
        }
        catch
        {
            StatusInfoBar.Severity = InfoBarSeverity.Error;
        }
        finally
        {
            IsPrimaryButtonEnabled = true;
        }
    }

    private async void OnPrimaryButtonClick(
        ContentDialog sender,
        ContentDialogButtonClickEventArgs args)
    {
        ContentDialogButtonClickDeferral deferral = args.GetDeferral();
        args.Cancel = true;
        IsPrimaryButtonEnabled = false;
        StatusInfoBar.Severity = InfoBarSeverity.Informational;
        try
        {
            if (ViewModel.IsConnecting)
            {
                Result = await ViewModel.ConnectAsync();
                PrimaryButtonText = "Save";
                CloseButtonText = "Keep suggested name";
            }
            else
            {
                Result = await ViewModel.SaveNameAsync();
                args.Cancel = false;
            }
        }
        catch
        {
            StatusInfoBar.Severity = InfoBarSeverity.Error;
        }
        finally
        {
            IsPrimaryButtonEnabled = true;
            deferral.Complete();
        }
    }

    private void OnClosing(ContentDialog sender, ContentDialogClosingEventArgs args)
    {
        if (ViewModel.IsBusy)
        {
            ViewModel.CancelQrScan();
            args.Cancel = true;
        }
    }
}
