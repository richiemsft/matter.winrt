using Microsoft.UI.Xaml;
using MatterControllerApp.Pages;
using WinUIEx;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace MatterControllerApp;

public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        AppWindow.SetIcon("Assets/AppIcon.ico");
        WindowManager windowManager = WindowManager.Get(this);
        windowManager.Width = 1200;
        windowManager.Height = 800;
        windowManager.MinWidth = 900;
        windowManager.MinHeight = 640;
        windowManager.PersistenceId = "MatterControllerMainWindow";
        AppWindow.Closing += OnWindowClosing;

        RootFrame.Navigate(typeof(DevicesPage));
    }

    private bool closeControllerComplete;

    private async void OnWindowClosing(Microsoft.UI.Windowing.AppWindow sender,
                                       Microsoft.UI.Windowing.AppWindowClosingEventArgs args)
    {
        if (closeControllerComplete)
        {
            return;
        }

        args.Cancel = true;
        try
        {
            await App.ControllerSession.CloseAsync();
        }
        finally
        {
            closeControllerComplete = true;
            Close();
        }
    }

}
