using CommunityToolkit.Mvvm.ComponentModel;

namespace MatterControllerApp.ViewModels;

public sealed class OnOffEndpointViewModel : ObservableObject
{
    private bool isOn;
    private bool isInWidget;

    public OnOffEndpointViewModel(
        ushort endpointId,
        bool initialIsOn,
        bool initialIsInWidget)
    {
        EndpointId = endpointId;
        isOn = initialIsOn;
        isInWidget = initialIsInWidget;
    }

    public ushort EndpointId { get; }
    public string DisplayName => $"Light {EndpointId}";
    public string ToggleAutomationId => $"ToggleLight{EndpointId}Button";
    public string WidgetAutomationId => $"WidgetLight{EndpointId}Toggle";

    public bool IsOn
    {
        get => isOn;
        set => SetProperty(ref isOn, value);
    }

    public bool IsInWidget
    {
        get => isInWidget;
        set => SetProperty(ref isInWidget, value);
    }

    public void RestoreWidgetSelection() =>
        OnPropertyChanged(nameof(IsInWidget));
}
