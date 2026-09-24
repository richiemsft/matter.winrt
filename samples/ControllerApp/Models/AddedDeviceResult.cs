namespace MatterControllerApp.Models;

public sealed record AddedDeviceResult(
    KnownDevice Device,
    string? Warning);
