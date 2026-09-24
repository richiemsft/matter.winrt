using MatterControllerApp.Models;
using MatterControllerApp.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace MatterControllerApp.Pages;

public sealed partial class DeviceDetailsPage : Page
{
    public DeviceDetailsViewModel ViewModel { get; } = new(App.ControllerSession);

    public DeviceDetailsPage()
    {
        InitializeComponent();
    }

    public static Visibility When(bool value) =>
        value ? Visibility.Visible : Visibility.Collapsed;

    public static string OnOffText(bool isOn) =>
        isOn ? "On" : "Off";

    protected override async void OnNavigatedTo(NavigationEventArgs args)
    {
        base.OnNavigatedTo(args);
        if (args.Parameter is not KnownDevice device)
        {
            throw new InvalidOperationException("Device details require a known device.");
        }
        ViewModel.Initialize(device);
        await ViewModel.LoadAsync();
    }

    private void OnBackClicked(object sender, RoutedEventArgs args)
    {
        if (Frame.CanGoBack)
        {
            Frame.GoBack();
        }
    }

    private async void OnEndpointToggleClicked(object sender, RoutedEventArgs args)
    {
        if (sender is Button { Tag: OnOffEndpointViewModel endpoint })
        {
            await ViewModel.ToggleEndpointAsync(endpoint);
        }
    }

    private async void OnWidgetSelectionToggled(object sender, RoutedEventArgs args)
    {
        if (sender is ToggleSwitch
            {
                Tag: OnOffEndpointViewModel endpoint
            } toggleSwitch)
        {
            await ViewModel.SetWidgetSelectionAsync(endpoint, toggleSwitch.IsOn);
        }
    }

    private async void OnRemoveDeviceClicked(object sender, RoutedEventArgs args)
    {
        ContentDialog confirmation = new()
        {
            XamlRoot = XamlRoot,
            Title = $"Remove {ViewModel.DisplayName}?",
            Content = "This device will be removed from the controller fabric.",
            PrimaryButtonText = "Remove",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close
        };
        if (await confirmation.ShowAsync() == ContentDialogResult.Primary &&
            await ViewModel.RemoveAsync() &&
            Frame.CanGoBack)
        {
            Frame.GoBack();
        }
    }
}
