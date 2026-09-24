using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MatterControllerApp.Models;
using MatterControllerApp.Services;
using Microsoft.UI.Dispatching;
using System.Collections.ObjectModel;

namespace MatterControllerApp.ViewModels;

public partial class DevicesPageViewModel : ObservableObject
{
    private readonly ControllerSession session;
    private readonly DispatcherQueue dispatcherQueue = DispatcherQueue.GetForCurrentThread();

    public DevicesPageViewModel(ControllerSession session)
    {
        this.session = session;
        session.NodesChanged += OnNodesChanged;
        session.StateChanged += OnStateChanged;
    }

    public ObservableCollection<KnownDevice> Devices { get; } = [];

    private bool isLoading = true;
    public bool IsLoading
    {
        get => isLoading;
        private set
        {
            if (SetProperty(ref isLoading, value))
            {
                NotifyStateChanged();
            }
        }
    }

    private string? errorMessage;
    public string? ErrorMessage
    {
        get => errorMessage;
        private set
        {
            if (SetProperty(ref errorMessage, value))
            {
                NotifyStateChanged();
            }
        }
    }

    private string? statusMessage;
    public string? StatusMessage
    {
        get => statusMessage;
        private set
        {
            if (SetProperty(ref statusMessage, value))
            {
                OnPropertyChanged(nameof(HasStatus));
            }
        }
    }

    public bool HasDevices => !IsLoading && ErrorMessage is null && Devices.Count > 0;
    public bool IsEmpty => !IsLoading && ErrorMessage is null && Devices.Count == 0;
    public bool HasError => !IsLoading && ErrorMessage is not null;
    public bool HasStatus => !string.IsNullOrWhiteSpace(StatusMessage);

    public async Task LoadAsync()
    {
        IsLoading = true;
        await App.ControllerInitialization;
        RefreshDevices();
    }

    public void ShowAddedDevice(AddedDeviceResult result)
    {
        RefreshDevices();
        StatusMessage = result.Warning ?? $"{result.Device.DisplayName} was added.";
    }

    [RelayCommand]
    private async Task RetryAsync()
    {
        IsLoading = true;
        await session.InitializeAsync();
        RefreshDevices();
    }

    private void RefreshDevices()
    {
        Devices.Clear();
        ErrorMessage = session.InitializationError;
        if (session.IsInitialized)
        {
            foreach (KnownDevice device in session.GetKnownDevices())
            {
                Devices.Add(device);
            }
        }
        IsLoading = false;
        NotifyStateChanged();
    }

    private void NotifyStateChanged()
    {
        OnPropertyChanged(nameof(HasDevices));
        OnPropertyChanged(nameof(IsEmpty));
        OnPropertyChanged(nameof(HasError));
    }

    private void OnNodesChanged(object? sender, EventArgs args) => Dispatch(RefreshDevices);

    private void OnStateChanged(object? sender, EventArgs args) => Dispatch(RefreshDevices);

    private void Dispatch(Action action)
    {
        if (dispatcherQueue.HasThreadAccess)
        {
            action();
        }
        else
        {
            dispatcherQueue.TryEnqueue(() => action());
        }
    }
}
