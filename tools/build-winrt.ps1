[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string] $Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$matterWinRtRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$matterSdkRoot = Join-Path $matterWinRtRoot "matterforwindows"
$winrtRoot = Join-Path $matterWinRtRoot "winrt"
$sdkOutputDirectory = Join-Path $matterSdkRoot "out\matter-winrt-sdk-$Architecture"
$matterLibrary = Join-Path $sdkOutputDirectory "obj\out\matter-winrt-sdk-target\matter-controller-sdk.lib"
$outputDirectory = Join-Path $matterWinRtRoot "out\win-winrt-$Architecture"
$generatedDirectory = Join-Path $outputDirectory "generated"
$objectDirectory = Join-Path $outputDirectory "obj"

if (-not (Test-Path $matterLibrary)) {
    throw "The Matter controller SDK is not built for $Architecture. Run '.\tools\build-matter.ps1 -Architecture $Architecture' first."
}

$environmentReady =
    $env:VSCMD_ARG_TGT_ARCH -eq $Architecture -and
    -not [string]::IsNullOrEmpty($env:WindowsSDKVersion) -and
    -not [string]::IsNullOrEmpty($env:WindowsSdkDir) -and
    $null -ne (Get-Command cl.exe -ErrorAction SilentlyContinue) -and
    $null -ne (Get-Command link.exe -ErrorAction SilentlyContinue)
if (-not $environmentReady) {
    . (Join-Path $matterSdkRoot "scripts\setup\windows.ps1") -Architecture $Architecture
}

$windowsSdkVersion = $env:WindowsSDKVersion.TrimEnd("\")
$windowsSdkRoot = $env:WindowsSdkDir.TrimEnd("\")
$windowsSdkBin = Join-Path $windowsSdkRoot "bin\$windowsSdkVersion\x64"
$windowsSdkMetadata = Join-Path $windowsSdkRoot "UnionMetadata\$windowsSdkVersion"
$midlrt = Join-Path $windowsSdkBin "midlrt.exe"
$cppwinrt = Join-Path $windowsSdkBin "cppwinrt.exe"
$winmd = Join-Path $outputDirectory "Matter.Windows.Controller.winmd"
$dll = Join-Path $outputDirectory "Matter.Windows.Controller.dll"

$python = Get-Command python3.exe -ErrorAction SilentlyContinue
if ($null -eq $python) {
    $python = Get-Command python.exe -ErrorAction Stop
}

New-Item -ItemType Directory -Force $outputDirectory, $generatedDirectory, $objectDirectory | Out-Null

& $python.Source (Join-Path $winrtRoot "generate_projection.py") `
    --midlrt $midlrt `
    --cppwinrt $cppwinrt `
    --sdk-metadata $windowsSdkMetadata `
    --idl (Join-Path $winrtRoot "Matter.Windows.Controller.idl") `
    --winmd $winmd `
    --generated $generatedDirectory
if ($LASTEXITCODE -ne 0) {
    throw "C++/WinRT projection generation failed for $Architecture."
}

$includeDirectories = @(
    $winrtRoot
    $generatedDirectory
    (Join-Path $matterSdkRoot "src\include")
    (Join-Path $matterSdkRoot "src")
    (Join-Path $sdkOutputDirectory "gen\include")
    (Join-Path $matterSdkRoot "config\standalone")
    (Join-Path $matterSdkRoot "zzz_generated\app-common")
    (Join-Path $matterSdkRoot "third_party\nlio\repo\include")
    (Join-Path $matterSdkRoot "third_party\boringssl\repo\src\include")
)
$definitions = @(
    "CHIP_STATIC_LIBRARY=1"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
    "__LITTLE_ENDIAN__=1"
    "_CRT_SECURE_NO_WARNINGS"
    "_SILENCE_CXX20_IS_POD_DEPRECATION_WARNING"
    "CHIP_HAVE_CONFIG_H=1"
    "OPENSSL_NO_ASM=1"
    "CHIP_DEVICE_LAYER_TARGET=Windows"
    "CHIP_DEVICE_PLATFORM_CONFIG_INCLUDE=<platform/Windows/CHIPDevicePlatformConfig.h>"
    "_HAS_EXCEPTIONS=1"
)
$compilerArguments = @(
    "/nologo"
    "/W4"
    "/WX"
    "/wd4068"
    "/wd4065"
    "/wd4100"
    "/wd4127"
    "/wd4130"
    "/wd4146"
    "/wd4244"
    "/wd4245"
    "/wd4267"
    "/wd4324"
    "/wd4505"
    "/wd4702"
    "/wd5054"
    "/utf-8"
    "/Zc:__cplusplus"
    "/Zc:preprocessor"
    "/permissive-"
    "/EHsc"
    "/GR"
    "/Gy"
    "/Gw"
    "/MD"
    "/O2"
    "/std:c++20"
    "/FI$(Join-Path $matterSdkRoot 'src\platform\Windows\MsvcCompilerCompatibility.h')"
)
$compilerArguments += $definitions | ForEach-Object { "/D$_" }
$compilerArguments += $includeDirectories | ForEach-Object { "/I$_" }

$sources = @(
    (Join-Path $generatedDirectory "module.g.cpp")
    (Join-Path $winrtRoot "AttributePath.cpp")
    (Join-Path $winrtRoot "BleCommissioningParameters.cpp")
    (Join-Path $winrtRoot "CommandPath.cpp")
    (Join-Path $winrtRoot "ControllerOptions.cpp")
    (Join-Path $winrtRoot "EventPath.cpp")
    (Join-Path $winrtRoot "MatterController.cpp")
    (Join-Path $winrtRoot "OnNetworkCommissioningParameters.cpp")
    (Join-Path $winrtRoot "Projection.cpp")
    (Join-Path $winrtRoot "TimedInteractionOptions.cpp")
)
$objects = @()
$sccache = Get-Command sccache.exe -ErrorAction SilentlyContinue
foreach ($source in $sources) {
    $objectName = [System.IO.Path]::GetFileNameWithoutExtension($source) + ".obj"
    $object = Join-Path $objectDirectory $objectName
    if ($sccache) {
        & $sccache.Source cl.exe @compilerArguments /c $source "/Fo$object"
    } else {
        & cl.exe @compilerArguments /c $source "/Fo$object"
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed for $source."
    }
    $objects += $object
}

$machine = if ($Architecture -eq "arm64") { "ARM64" } else { "X64" }
$linkArguments = @(
    "/nologo"
    "/DLL"
    "/MACHINE:$machine"
    "/OUT:$dll"
    "/IMPLIB:$(Join-Path $outputDirectory 'Matter.Windows.Controller.lib')"
    "/PDB:$(Join-Path $outputDirectory 'Matter.Windows.Controller.pdb')"
    "/DEF:$(Join-Path $winrtRoot 'module.def')"
    "/OPT:REF"
    "/OPT:ICF"
)
$linkArguments += $objects
$linkArguments += @(
    $matterLibrary
    "WindowsApp.lib"
    "ws2_32.lib"
    "iphlpapi.lib"
    "Advapi32.lib"
    "Ole32.lib"
    "Shell32.lib"
    "dnsapi.lib"
)

& link.exe @linkArguments
if ($LASTEXITCODE -ne 0) {
    throw "WinRT component link failed for $Architecture."
}

if (-not (Test-Path $dll) -or -not (Test-Path $winmd)) {
    throw "The WinRT component outputs were not produced for $Architecture."
}

Write-Host "Built WinRT component: $dll"
