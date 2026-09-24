[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string] $Architecture = "x64"
)

$ErrorActionPreference = "Stop"
$matterWinRtRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$matterSdkRoot = Join-Path $matterWinRtRoot "matterforwindows"
$setupScript = Join-Path $matterSdkRoot "scripts\setup\windows.ps1"
$gn = Join-Path $matterSdkRoot ".environment\windows\gn\gn.exe"
$ninja = Join-Path $matterSdkRoot ".environment\windows\ninja\ninja.exe"
$pythonEnvironment = Join-Path $matterSdkRoot ".environment\windows\python"

if (-not (Test-Path $setupScript)) {
    throw "The Matter SDK submodule is not initialized. Run 'git submodule update --init --recursive'."
}

$environmentReady =
    $env:VSCMD_ARG_TGT_ARCH -eq $Architecture -and
    $env:VIRTUAL_ENV -eq $pythonEnvironment -and
    (Test-Path $gn) -and
    (Test-Path $ninja)
if (-not $environmentReady) {
    . $setupScript -Architecture $Architecture
}

$targetDirectory = Join-Path $matterSdkRoot "out\matter-winrt-sdk-target"
$outputDirectory = Join-Path $matterSdkRoot "out\matter-winrt-sdk-$Architecture"
$dotfile = Join-Path $targetDirectory ".gn"
$library = Join-Path $outputDirectory "obj\out\matter-winrt-sdk-target\matter-controller-sdk.lib"

New-Item -ItemType Directory -Force $targetDirectory | Out-Null

$utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    $dotfile,
    "buildconfig = `"//build/config/BUILDCONFIG.gn`"`nroot = `"//out/matter-winrt-sdk-target:matter-controller-sdk`"`n",
    $utf8WithoutBom)
[System.IO.File]::WriteAllText(
    (Join-Path $targetDirectory "BUILD.gn"),
    @'
static_library("matter-controller-sdk") {
  complete_static_lib = true
  sources = [
    "anchor.cpp",
    "//src/app/server-cluster/testing/EmptyProvider.cpp",
  ]
  deps = [
    "//src/app/data-model-provider",
    "//src/controller",
    "//src/credentials:default_attestation_verifier",
    "//src/platform",
    "//src/platform/logging:default",
  ]
}
'@,
    $utf8WithoutBom)
[System.IO.File]::WriteAllText(
    (Join-Path $targetDirectory "anchor.cpp"),
    "namespace matter_winrt {`nvoid MatterControllerSdkAnchor() {}`n} // namespace matter_winrt`n",
    $utf8WithoutBom)

$gnArgs = @(
    'target_os="win"'
    "target_cpu=`"$Architecture`""
    'chip_device_platform="windows"'
    'chip_windows_enable_cxx20=true'
    'chip_with_nlfaultinjection=false'
    'chip_build_tests=false'
    'chip_build_tools=false'
    'chip_caller_handles_critical_failure=true'
    'is_debug=false'
)
if (Get-Command sccache.exe -ErrorAction SilentlyContinue) {
    $gnArgs += 'cc_wrapper="sccache"'
}
$gnArgs = $gnArgs -join " "

& $gn gen $outputDirectory "--root=$matterSdkRoot" "--dotfile=$dotfile" "--args=$gnArgs"
if ($LASTEXITCODE -ne 0) {
    throw "Matter SDK GN generation failed for $Architecture."
}

& $ninja -C $outputDirectory matter-controller-sdk
if ($LASTEXITCODE -ne 0) {
    throw "Matter controller SDK build failed for $Architecture."
}

if (-not (Test-Path $library)) {
    throw "Matter controller SDK archive was not produced: $library"
}

Write-Host "Built Matter controller SDK: $library"
