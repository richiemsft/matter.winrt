using MatterControllerApp.Models;
using Microsoft.Windows.Widgets.Providers;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace MatterControllerApp.Widgets;

[ComVisible(true)]
[ComDefaultInterface(typeof(IWidgetProvider))]
[Guid("A4EC39EB-5440-4B4E-828F-2AF7D8D8CE51")]
public sealed partial class MatterWidgetProvider : IWidgetProvider
{
    public const string DefinitionId = "Matter_Controls_Widget";
    private static readonly object WidgetIdsLock = new();
    private static readonly HashSet<string> WidgetIds = [];
    private static string? template;

    public MatterWidgetProvider()
    {
        lock (WidgetIdsLock)
        {
            foreach (WidgetInfo info in WidgetManager.GetDefault().GetWidgetInfos())
            {
                if (info.WidgetContext.DefinitionId == DefinitionId)
                {
                    WidgetIds.Add(info.WidgetContext.Id);
                }
            }
        }
    }

    public void CreateWidget(WidgetContext widgetContext)
    {
        lock (WidgetIdsLock)
        {
            WidgetIds.Add(widgetContext.Id);
        }
        UpdateFromCache(widgetContext.Id);
        Observe(RefreshAllAsync());
    }

    public void DeleteWidget(string widgetId, string customState)
    {
        lock (WidgetIdsLock)
        {
            WidgetIds.Remove(widgetId);
        }
    }

    public void Activate(WidgetContext widgetContext)
    {
        lock (WidgetIdsLock)
        {
            WidgetIds.Add(widgetContext.Id);
        }
        Observe(RefreshAllAsync());
    }

    public void Deactivate(string widgetId)
    {
    }

    public void OnWidgetContextChanged(WidgetContextChangedArgs contextChangedArgs) =>
        UpdateFromCache(contextChangedArgs.WidgetContext.Id);

    public void OnActionInvoked(WidgetActionInvokedArgs actionInvokedArgs)
    {
        if (actionInvokedArgs.Verb != "toggle")
        {
            return;
        }

        using JsonDocument data = JsonDocument.Parse(actionInvokedArgs.Data);
        string deviceKey = data.RootElement.GetProperty("deviceKey").GetString() ??
            throw new InvalidDataException("The widget action did not include a device key.");
        Observe(ToggleAndRefreshAsync(deviceKey));
    }

    public static async Task RefreshAllAsync()
    {
        IReadOnlyList<WidgetDevice> devices =
            await MatterWidgetController.ReadStatesAsync();
        UpdateAllWidgets(devices);
    }

    private static async Task ToggleAndRefreshAsync(string deviceKey)
    {
        try
        {
            await MatterWidgetController.ToggleAsync(deviceKey);
            await RefreshAllAsync();
        }
        catch (Exception exception)
        {
            IReadOnlyList<WidgetDevice> devices = new Services.WidgetSelectionStore()
                .GetDevices()
                .Select(device => device.Key == deviceKey
                    ? device with { ErrorMessage = exception.Message }
                    : device)
                .ToList();
            UpdateAllWidgets(devices);
        }
    }

    private static void Observe(Task operation) => _ = ObserveAsync(operation);

    private static async Task ObserveAsync(Task operation)
    {
        try
        {
            await operation;
        }
        catch (Exception exception)
        {
            IReadOnlyList<WidgetDevice> devices = new Services.WidgetSelectionStore()
                .GetDevices()
                .Select(device => device with { ErrorMessage = exception.Message })
                .ToList();
            UpdateAllWidgets(devices);
        }
    }

    private static void UpdateFromCache(string widgetId) =>
        UpdateWidget(widgetId, new Services.WidgetSelectionStore().GetDevices());

    private static void UpdateWidget(string widgetId, IReadOnlyList<WidgetDevice> devices)
    {
        object[] switches = devices.Select(device => new
        {
            key = device.Key,
            name = device.DisplayName,
            stateLabel = device.ErrorMessage is not null
                ? $"Unavailable: {device.ErrorMessage}"
                : device.IsOn switch
            {
                true => "On",
                false => "Off",
                null => "State unavailable"
            },
            actionLabel = device.IsOn == true ? "Turn off" : "Turn on"
        }).ToArray();
        string data = JsonSerializer.Serialize(new
        {
            showEmptyMessage = switches.Length == 0,
            emptyMessage = "Add On/Off devices from Matter Controller.",
            switches
        });

        WidgetUpdateRequestOptions options = new(widgetId)
        {
            Template = GetTemplate(),
            Data = data,
            CustomState = string.Empty
        };
        WidgetManager.GetDefault().UpdateWidget(options);
    }

    private static void UpdateAllWidgets(IReadOnlyList<WidgetDevice> devices)
    {
        foreach (string widgetId in GetWidgetIds())
        {
            UpdateWidget(widgetId, devices);
        }
    }

    private static IReadOnlyList<string> GetWidgetIds()
    {
        lock (WidgetIdsLock)
        {
            foreach (WidgetInfo info in WidgetManager.GetDefault().GetWidgetInfos())
            {
                if (info.WidgetContext.DefinitionId == DefinitionId)
                {
                    WidgetIds.Add(info.WidgetContext.Id);
                }
            }
            return WidgetIds.ToList();
        }
    }

    private static string GetTemplate() =>
        template ??= File.ReadAllText(
            Path.Combine(AppContext.BaseDirectory, "Widgets", "MatterControlsTemplate.json"));
}
