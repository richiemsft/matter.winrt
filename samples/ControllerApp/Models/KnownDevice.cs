namespace MatterControllerApp.Models;

public sealed record KnownDevice(
    ulong NodeId,
    ushort FabricIndex,
    string DisplayName);
