using MatterControllerApp.Dialogs;
using MatterControllerApp.Models;
using MatterControllerApp.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MatterControllerApp.Pages;

public sealed partial class DevicesPage : Page
{
    public DevicesPageViewModel ViewModel { get; } = new(App.ControllerSession);

    public DevicesPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
    }

    public static Visibility When(bool value) =>
        value ? Visibility.Visible : Visibility.Collapsed;

    private async void OnLoaded(object sender, RoutedEventArgs args) =>
        await ViewModel.LoadAsync();

    private async void OnAddDeviceClicked(object sender, RoutedEventArgs args)
    {
        AddDeviceDialog dialog = new()
        {
            XamlRoot = XamlRoot
        };
        try
        {
            await dialog.ShowAsync();
            if (dialog.Result is not null)
            {
                ViewModel.ShowAddedDevice(dialog.Result);
            }
        }
        finally
        {
            dialog.Dispose();
        }
    }

    private void OnDeviceClicked(object sender, ItemClickEventArgs args)
    {
        if (args.ClickedItem is KnownDevice device)
        {
            Frame.Navigate(typeof(DeviceDetailsPage), device);
        }
    }
}
