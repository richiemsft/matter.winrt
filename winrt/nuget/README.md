# Matter.Windows.Controller

Development-preview C++/WinRT Matter controller for x64 and ARM64 Windows apps.
The package includes a WinMD reference and selects the matching native component
DLL from `runtimes/win-x64/native` or `runtimes/win-arm64/native`.
Managed .NET projects must also reference `Microsoft.Windows.CsWinRT` 2.3.1 or
later so the WinMD is projected into C# at build time.

Set `ControllerOptions.StoragePath` to an absolute, app-owned directory before
calling `MatterController.CreateAsync`. Only one controller may be active in a
process, and the storage directory is exclusively owned while it is open.

This preview uses file-backed operational credentials and allows test device
attestation by default. It is not production security infrastructure.

For multi-admin commissioning, request a sharing code from the device's
existing controller and set `OnNetworkCommissioningParameters.SetupCode` to
the manual code or `MT:` QR payload before calling `CommissionOnNetworkAsync`.

For factory-reset Wi-Fi or Thread devices, use
`MatterControllerCommissioning.CommissionBleAsync`. Its ordered
`ProgressChanged` event runs on a worker thread and reports typed stages,
elapsed time, retries, transport, and the selected operational interface.
The returned `MatterCommissioningResult` contains structured failure details.
Canceling the WinRT asynchronous operation stops native pairing.

Use `MatterNetworkInterfaceProvider.GetEligibleNetworkInterfacesAsync` before
commissioning when an application needs to select an adapter. Automatic mode
retains normal Windows routing. Prefer mode tries the selected interface first
and may fall back; Require mode does not fall back and validates that the
adapter is connected and supports IPv6 multicast. Adapter identifiers are
ephemeral and should be enumerated again before a later operation.
