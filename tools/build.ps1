[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64", "all")]
    [string] $Architecture = "all",
    [string] $Version = "0.1.0-preview.10",
    [string] $OutputDirectory = "artifacts",
    [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"
$outerRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$architectures = if ($Architecture -eq "all") { @("x64", "arm64") } else { @($Architecture) }
$sdkRoot = Join-Path $outerRoot "matterforwindows"

foreach ($targetArchitecture in $architectures) {
    $outputName = "win-winrt-$targetArchitecture"
    $outputPath = Join-Path $outerRoot "out\$outputName"
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot "build-matter.ps1") -Architecture $targetArchitecture
        if ($LASTEXITCODE -ne 0) { throw "Matter SDK build failed for $targetArchitecture." }
        & (Join-Path $PSScriptRoot "build-winrt.ps1") -Architecture $targetArchitecture
        if ($LASTEXITCODE -ne 0) { throw "WinRT component build failed for $targetArchitecture." }
    }

    foreach ($asset in @("Matter.Windows.Controller.dll", "Matter.Windows.Controller.winmd")) {
        if (-not (Test-Path (Join-Path $outputPath $asset))) {
            throw "Missing $targetArchitecture output: $asset"
        }
    }
}

if (@($architectures).Count -ne 2) {
    Write-Host "Built $Architecture. Select -Architecture all to create the dual-architecture package."
    return
}

$artifactPath = Join-Path $outerRoot $OutputDirectory
$stagingPath = Join-Path $artifactPath "Matter.Windows.Controller.$Version"
$packagePath = Join-Path $artifactPath "Matter.Windows.Controller.$Version.nupkg"
New-Item -ItemType Directory -Force $artifactPath | Out-Null
if (Test-Path $stagingPath) {
    Remove-Item -Recurse -Force $stagingPath
}
New-Item -ItemType Directory -Force `
    (Join-Path $stagingPath "winmd"), `
    (Join-Path $stagingPath "buildTransitive"), `
    (Join-Path $stagingPath "runtimes\win-x64\native"), `
    (Join-Path $stagingPath "runtimes\win-arm64\native") | Out-Null

Copy-Item (Join-Path $outerRoot "out\win-winrt-x64\Matter.Windows.Controller.dll") `
    (Join-Path $stagingPath "runtimes\win-x64\native")
Copy-Item (Join-Path $outerRoot "out\win-winrt-arm64\Matter.Windows.Controller.dll") `
    (Join-Path $stagingPath "runtimes\win-arm64\native")
Copy-Item (Join-Path $outerRoot "out\win-winrt-x64\Matter.Windows.Controller.winmd") `
    (Join-Path $stagingPath "winmd")
Copy-Item (Join-Path $outerRoot "winrt\Matter.Windows.Controller.xml") `
    (Join-Path $stagingPath "winmd")
Copy-Item (Join-Path $outerRoot "winrt\nuget\Matter.Windows.Controller.props") `
    (Join-Path $stagingPath "buildTransitive")
Copy-Item (Join-Path $outerRoot "winrt\nuget\Matter.Windows.Controller.targets") `
    (Join-Path $stagingPath "buildTransitive")
Copy-Item (Join-Path $outerRoot "winrt\nuget\README.md") $stagingPath
Copy-Item (Join-Path $outerRoot "matterforwindows\LICENSE") $stagingPath

$nuspec = @"
<?xml version="1.0"?>
<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd">
  <metadata>
    <id>Matter.Windows.Controller</id>
    <version>$Version</version>
    <authors>Matter Windows contributors</authors>
    <license type="file">LICENSE</license>
    <readme>README.md</readme>
    <projectUrl>https://github.com/dotMorten/matter.winrt</projectUrl>
    <repository type="git" url="https://github.com/dotMorten/matter.winrt.git" />
    <description>Development-preview native C++/WinRT Matter controller for x64 and ARM64 Windows applications.</description>
    <tags>matter winrt windows iot controller</tags>
    <dependencies>
      <group targetFramework="net8.0-windows10.0.19041.0">
        <dependency id="Microsoft.Windows.CsWinRT" version="[2.3.1]" />
      </group>
    </dependencies>
  </metadata>
</package>
"@
Set-Content -Path (Join-Path $stagingPath "Matter.Windows.Controller.nuspec") -Value $nuspec -Encoding utf8
if (Test-Path $packagePath) {
    Remove-Item -Force $packagePath
}
Compress-Archive -Path (Join-Path $stagingPath "*") -DestinationPath ($packagePath + ".zip")
Move-Item ($packagePath + ".zip") $packagePath
Remove-Item -Recurse -Force $stagingPath

Write-Host "Created $packagePath"
