# Matter WinRT

Native C++/WinRT Matter controller APIs and a WinUI 3 sample for x64 and
ARM64 Windows. The Matter SDK is pinned as the `matterforwindows` submodule so
the component and client UX can evolve independently from the SDK fork.

## Application-facing API

`Matter.Windows.Controller` is the application-facing Windows API over the
native Matter SDK. Applications consume its WinRT metadata and
architecture-specific DLL instead of linking to the Matter SDK's internal C++
types or depending on their ABI.

The API is device-type agnostic. Matter device types are compositions of
clusters on endpoints, so the generic `ReadAttributeAsync`,
`WriteAttributeAsync`, `InvokeCommandAsync`, and `SubscribeAttributeAsync`
methods accept arbitrary endpoint, cluster, attribute, and command identifiers.
`ReadEventsAsync` and `SubscribeEventAsync` provide the corresponding generic
event access required by event-driven devices. Together, these operations work
across every device type represented by the pinned Matter data model.
`BasicInformationCluster`, `OnOffCluster`, and `LevelControlCluster` are typed
conveniences, not a device-type support list.

Use `WriteAttributeTimedAsync` and `InvokeCommandTimedAsync` with
`TimedInteractionOptions` for attributes and commands that require a timed
interaction, including Door Lock, Energy EVSE, Administrator Commissioning,
and Thread management operations.

Generic values use `IPropertySet`: decimal context-tag strings identify
structure fields, `value` wraps a scalar root, inspectable vectors represent
Matter lists, and byte vectors represent octet strings. Responses use the same
recursive representation.

BLE network commissioning can report native stage transitions, elapsed time,
transport changes, retries, and structured failures through the additive
`MatterControllerCommissioning` class. Its `ProgressChanged` handlers run in
order on a thread-pool thread rather than the UI thread. The returned
`MatterCommissioningResult` distinguishes success, timeout, the failed stage,
and common failure categories without requiring log parsing. Canceling the
returned `IAsyncOperation` stops native pairing and completes the operation in
the canceled state.

`MatterNetworkInterfaceProvider.GetEligibleNetworkInterfacesAsync` enumerates
interfaces that can participate in operational Matter discovery. Pass a
`MatterNetworkInterfaceSelection` to choose automatic routing, prefer one
interface with fallback, or require one interface without fallback. Interface
identifiers are valid only while the adapter exists; enumerate again before a
later commissioning operation. Interface selection is captured when
commissioning starts and applies only to operational DNS-SD, address
resolution, CASE, and commissioning-complete traffic. It does not affect BLE.

```csharp
var commissioning = new MatterControllerCommissioning(controller);
commissioning.ProgressChanged += (_, progress) =>
    Console.WriteLine($"{progress.Stage}: {progress.DiagnosticMessage}");

var selection = new MatterNetworkInterfaceSelection(
    MatterNetworkInterfaceSelectionMode.Automatic, 0);
MatterCommissioningResult result =
    await commissioning.CommissionBleAsync(parameters, selection);
if (!result.Succeeded)
{
    throw new InvalidOperationException(result.DiagnosticMessage);
}
```

This package is a development preview. Its WinRT contract is the intended
application boundary, but preview releases do not yet guarantee ABI
compatibility. Applications must deploy the native DLL from the same package
version used at build time.

## Versioning and compatibility

- The complete policy is documented in
  [VERSIONING.md](VERSIONING.md).
- The NuGet package follows semantic versioning. Preview suffixes identify
  pre-stable contracts and may contain breaking API or ABI changes.
- A future stable release will preserve existing WinRT metadata within a major
  version. Additive APIs increment the minor version; breaking changes require
  a new major version.
- Stable API removal requires deprecation for at least one minor release unless
  an immediate security fix makes that impossible.
- The public binary contract is the WinRT metadata plus the standard activation
  exports. Matter SDK C++ headers, classes, STL types, and internal DLL symbols
  are not public ABI.
- Each package pins one Matter SDK commit. Updating that pin requires x64 and
  ARM64 component builds, sample builds against the produced package, and a
  package version change.
- The Windows workflow compares generated WinMD metadata with the adopted
  release baseline and validates the x64 and ARM64 native export tables.

## Clone and build

```powershell
git clone --recurse-submodules https://github.com/dotMorten/matter.winrt
cd matter.winrt
.\tools\build.ps1
```

The native build has two explicit stages. The first uses the Matter SDK's own
GN/Ninja build inside the submodule and produces one private controller SDK
archive:

```powershell
.\tools\build-matter.ps1 -Architecture x64
```

The second builds the C++/WinRT component directly with the MSVC and Windows
SDK tools, using the generated Matter headers and archive at their known
submodule-relative paths:

```powershell
.\tools\build-winrt.ps1 -Architecture x64
```

`build.ps1` runs both stages for x64 and ARM64 before packaging. The outer
repository is not a GN source tree and does not create directory junctions to
the SDK.

The build produces
`artifacts\Matter.Windows.Controller.0.1.0-preview.10.nupkg`. The package
contains a WinMD plus architecture-specific native DLLs for `win-x64` and
`win-arm64`.

Build the sample:

```powershell
dotnet build .\samples\ControllerApp\MatterControllerApp.csproj `
    -p:Platform=ARM64 -p:PlatformTarget=ARM64
```

The sample initializes its persisted controller fabric when the app launches
and closes it with the window. Its landing page presents known devices as a
grid and provides an empty-state call to action when none are commissioned.
**Add device** opens a manual sharing-code, camera QR-scanning, or Bluetooth LE
workflow; after connection, the app suggests the device-reported name and lets
the user edit it before finishing. Selecting a device opens its details page
with independently controllable rows for every discovered On/Off endpoint,
Level controls, and generic attribute queries.
Devices that already contain this controller fabric but are missing from the
local node index can be verified and restored by node ID without repeating
AddNOC.

The packaged sample also provides one shared **Matter Controls** Windows
widget. On a device details page, turn on **Matter Controls widget** for any
endpoint that exposes the On/Off cluster. Turn the switch off again to remove
that endpoint. Open the Windows Widgets board, choose **Add widgets**, and pin
**Matter Controls** once; every selected switch then appears in that medium or
large widget and can be toggled without opening the app. Windows does not
permit apps to pin widgets programmatically. The widget provider is an MSIX
app extension, so it is not available from the unpackaged sample profile.

This is a development preview. Operational credentials are file-backed and
test device attestation is enabled by default.
