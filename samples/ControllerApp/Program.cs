using MatterControllerApp.Widgets;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace MatterControllerApp;

public static class Program
{
    private const string WidgetServerArgument = "-RegisterProcessAsComServer";
    private const string WidgetShutdownRequestName =
        @"Local\MatterControllerApp.WidgetProviderShutdownRequest";
    private const string WidgetShutdownCompleteName =
        @"Local\MatterControllerApp.WidgetProviderShutdownComplete";

    public static bool IsWidgetProviderProcess { get; private set; }

    [MTAThread]
    public static void Main(string[] args)
    {
        if (args.Contains(WidgetServerArgument, StringComparer.OrdinalIgnoreCase))
        {
            IsWidgetProviderProcess = true;
            RunWidgetProvider();
            return;
        }

        StopWidgetProvider();
        Thread uiThread = new(RunApplication);
        uiThread.SetApartmentState(ApartmentState.STA);
        uiThread.Start();
        uiThread.Join();
    }

    private static void RunApplication()
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();
        using WidgetProviderRegistration registration = WidgetProviderRegistration.Register();
        Application.Start(_ =>
        {
            DispatcherQueueSynchronizationContext context =
                new(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            App.XamlGeneratedCreateApplicationInstance();
        });
    }

    private static void RunWidgetProvider()
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();
        using EventWaitHandle shutdownRequest = new(
            false,
            EventResetMode.ManualReset,
            WidgetShutdownRequestName);
        using EventWaitHandle shutdownComplete = new(
            false,
            EventResetMode.ManualReset,
            WidgetShutdownCompleteName);
        shutdownRequest.Reset();
        shutdownComplete.Reset();
        using WidgetProviderRegistration registration = WidgetProviderRegistration.Register();
        shutdownRequest.WaitOne();
        try
        {
            MatterWidgetController.CloseAsync().GetAwaiter().GetResult();
        }
        finally
        {
            shutdownComplete.Set();
        }
    }

    private static void StopWidgetProvider()
    {
        try
        {
            using EventWaitHandle shutdownRequest =
                EventWaitHandle.OpenExisting(WidgetShutdownRequestName);
            using EventWaitHandle shutdownComplete =
                EventWaitHandle.OpenExisting(WidgetShutdownCompleteName);
            shutdownRequest.Set();
            shutdownComplete.WaitOne(TimeSpan.FromSeconds(15));
        }
        catch (WaitHandleCannotBeOpenedException)
        {
        }
    }
}
