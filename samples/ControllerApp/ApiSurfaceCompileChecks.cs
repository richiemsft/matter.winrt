using Matter.Windows.Controller;
using System.Threading.Tasks;
using Windows.Foundation.Collections;

namespace MatterControllerApp;

internal static class ApiSurfaceCompileChecks
{
    internal static async Task ExerciseGenericInteractionsAsync(MatterController controller, ulong nodeId)
    {
        var value = new PropertySet { ["value"] = true };
        var arguments = new PropertySet();
        var timed = new TimedInteractionOptions(5_000);
        var attribute = new AttributePath(1, 0x0006, 0x0000);
        var command = new CommandPath(1, 0x0006, 0x0001);
        var eventPath = new EventPath(1, 0x0003, 0x0000, false);

        await controller.ReadAttributeAsync(nodeId, attribute);
        await controller.WriteAttributeAsync(nodeId, attribute, value);
        await controller.WriteAttributeTimedAsync(nodeId, attribute, value, timed);
        await controller.InvokeCommandAsync(nodeId, command, arguments);
        await controller.InvokeCommandTimedAsync(nodeId, command, arguments, timed);

        AttributeSubscription attributeSubscription =
            await controller.SubscribeAttributeAsync(nodeId, attribute, 1, 60);
        await attributeSubscription.CloseAsync();

        await controller.ReadEventsAsync(nodeId, eventPath, 0);
        EventSubscription eventSubscription =
            await controller.SubscribeEventAsync(nodeId, eventPath, 0, 1, 60);
        await eventSubscription.CloseAsync();
    }

    internal static async Task ExerciseNetworkCommissioningAsync(MatterController controller)
    {
        IReadOnlyList<MatterNetworkInterface> interfaces =
            await MatterNetworkInterfaceProvider.GetEligibleNetworkInterfacesAsync();
        MatterNetworkInterfaceSelection automatic = new(
            MatterNetworkInterfaceSelectionMode.Automatic,
            0);
        var commissioning = new MatterControllerCommissioning(controller);
        commissioning.ProgressChanged += (_, progress) =>
        {
            MatterCommissioningStage stage = progress.Stage;
            MatterCommissioningTransport transport = progress.Transport;
            _ = (stage, transport, progress.LastCompletedStage, progress.NativeStageId,
                progress.ElapsedTime, progress.DiagnosticMessage, progress.DisplayMessage,
                progress.NetworkInterfaceId, progress.NetworkInterfaceName,
                progress.AttemptNumber, progress.IsRetrying);
        };

        var wiFi = new BleNetworkCommissioningParameters
        {
            NodeId = 1,
            SetupPinCode = 20202021,
            LongDiscriminator = 3840,
            WiFi = new WiFiNetworkCredentials("network", "passphrase")
        };
        MatterCommissioningResult result = await commissioning.CommissionBleAsync(wiFi, automatic);
        _ = (result.Succeeded, result.Outcome, result.FailureKind, result.FailedStage,
            result.LastCompletedStage, result.NativeStageId, result.NativeErrorCode,
            result.DiagnosticMessage, result.NetworkInterfaceId, result.Node);

        var dataset = new Windows.Storage.Streams.Buffer(16) { Length = 16 };
        var thread = new BleNetworkCommissioningParameters
        {
            NodeId = 2,
            SetupPinCode = 20202021,
            LongDiscriminator = 3840,
            Thread = new ThreadNetworkCredentials(dataset)
        };
        await commissioning.CommissionBleAsync(thread, automatic);

        foreach (MatterNetworkInterface networkInterface in interfaces)
        {
            _ = (networkInterface.Id, networkInterface.InterfaceIndex, networkInterface.Name,
                networkInterface.Type, networkInterface.IsConnected, networkInterface.SupportsIpv6,
                networkInterface.SupportsMulticast, networkInterface.IsVirtual);
        }
    }
}
