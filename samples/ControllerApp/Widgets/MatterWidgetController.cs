using MatterControllerApp.Models;
using MatterControllerApp.Services;

namespace MatterControllerApp.Widgets;

internal static class MatterWidgetController
{
    private static readonly SemaphoreSlim OperationGate = new(1, 1);

    public static async Task<IReadOnlyList<WidgetDevice>> ReadStatesAsync()
    {
        await OperationGate.WaitAsync();
        try
        {
            WidgetSelectionStore store = new();
            List<WidgetDevice> devices = store.GetDevices().ToList();
            try
            {
                await UseControllerAsync(async controller =>
                {
                    for (int index = 0; index < devices.Count; index++)
                    {
                        WidgetDevice device = devices[index];
                        try
                        {
                            bool isOn = await controller.GetOnOffCluster(
                                device.NodeId,
                                device.EndpointId).ReadAsync();
                            devices[index] = device with
                            {
                                IsOn = isOn,
                                ErrorMessage = null
                            };
                            store.UpdateState(device.Key, isOn);
                        }
                        catch (Exception exception)
                        {
                            devices[index] = device with
                            {
                                ErrorMessage = exception.Message
                            };
                        }
                    }
                });
            }
            catch (Exception exception)
            {
                devices = devices.Select(device => device with
                {
                    ErrorMessage = exception.Message
                }).ToList();
            }
            return devices;
        }
        finally
        {
            OperationGate.Release();
        }
    }

    public static async Task ToggleAsync(string deviceKey)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(deviceKey);
        await OperationGate.WaitAsync();
        try
        {
            WidgetSelectionStore store = new();
            WidgetDevice device = store.GetDevices().FirstOrDefault(
                candidate => candidate.Key == deviceKey) ??
                throw new InvalidOperationException("The selected widget device no longer exists.");

            await UseControllerAsync(async controller =>
            {
                Matter.Windows.Controller.OnOffCluster cluster =
                    controller.GetOnOffCluster(device.NodeId, device.EndpointId);
                await cluster.ToggleAsync();
                store.UpdateState(device.Key, await cluster.ReadAsync());
            });
        }
        finally
        {
            OperationGate.Release();
        }
    }

    public static async Task CloseAsync()
    {
        await OperationGate.WaitAsync();
        try
        {
            await App.ControllerSession.CloseAsync();
        }
        finally
        {
            OperationGate.Release();
        }
    }

    private static async Task UseControllerAsync(
        Func<Matter.Windows.Controller.MatterController, Task> action)
    {
        ControllerSession session = App.ControllerSession;
        if (Program.IsWidgetProviderProcess)
        {
            await session.InitializeAsync();
        }
        else
        {
            await App.ControllerInitialization;
        }

        if (!session.IsInitialized)
        {
            throw new InvalidOperationException(
                session.InitializationError ?? "The Matter controller could not be initialized.");
        }

        await action(session.RequireController());
    }
}
