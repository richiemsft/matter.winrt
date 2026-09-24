# Matter WinRT controller sample

This diagnostic WinUI 3 app consumes the local `Matter.Windows.Controller`
NuGet package. It initializes and closes the controller with the application
window. The landing page shows known devices in a grid. The add-device dialog
accepts a manual multi-admin sharing code, scans a Matter QR code with a
selected camera-backed barcode scanner, or commissions over Bluetooth LE. After
connection it suggests the device-reported name and allows editing before the
device appears in the list. Selecting a device opens generic attribute state,
Basic Information, independently controllable rows for every On/Off endpoint,
and available Level Control commands.

If commissioning reports that the NOC or fabric already exists, the app
automatically verifies operational access with the attempted node ID and
restores the device to the local node index.

To add a device that is already owned by another Matter controller, open the
device's commissioning window in that controller and paste its manual sharing
code, paste its `MT:` QR setup payload, or select **Scan QR code** and point the
camera preview at it. The sample uses that code for multi-admin on-network
commissioning, then persists the assigned node and can read Basic Information,
On/Off, and Level Control state.

## Windows widget

The packaged app registers one shared **Matter Controls** widget. To add a
switch:

1. Open the device details page and select the endpoint that exposes On/Off.
2. Turn on **Matter Controls widget** in the On/Off card. Turn it off to
   remove that endpoint later.
3. Open Windows Widgets, choose **Add widgets**, and pin **Matter Controls**.

Windows requires the user to pin a widget and does not expose an API for the
app to do so. Pin the widget only once; all selected On/Off endpoints appear
in the same widget. The widget supports medium and large sizes, refreshes the
current switch state when activated, and can issue On/Off toggles while the
main app is closed. Connection failures are shown in the widget and the action
can be retried.

Windows Widgets requires the packaged MSIX profile. It is unavailable from the
unpackaged profile because that profile does not register the widget provider
app extension.

Create the package from the repository root:

```powershell
.\tools\build.ps1
```

Build or run the native ARM64 app:

```powershell
dotnet build .\samples\ControllerApp\MatterControllerApp.csproj `
    -p:Platform=ARM64 -p:PlatformTarget=ARM64

winapp run .\samples\ControllerApp\MatterControllerApp.csproj `
    --arch arm64
```

For an unpackaged build, add
`-p:WindowsPackageType=None -p:Unpackaged=true`. The project includes
registration-free WinRT manifest entries and copies the architecture-matched
component beside the app.

The component is a development preview. It uses file-backed operational keys
and permits test device attestation by default.
