namespace MatterControllerApp.Models;

using System.Text.Json.Serialization;

public sealed record WidgetDevice(
    ulong NodeId,
    ushort FabricIndex,
    ushort EndpointId,
    string DisplayName,
    bool? IsOn = null)
{
    [JsonIgnore]
    public string Key => $"{FabricIndex}:{NodeId}:{EndpointId}";

    [JsonIgnore]
    public string? ErrorMessage { get; init; }
}
