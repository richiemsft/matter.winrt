[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CurrentWinmd,
    [Parameter(Mandatory)]
    [string] $BaselineWinmd,
    [Parameter(Mandatory)]
    [string[]] $NativeDll,
    [string] $EquivalentWinmd,
    [string] $ExpectedExports = (Join-Path $PSScriptRoot "..\abi\public-exports.txt")
)

$ErrorActionPreference = "Stop"

function Resolve-WindowsSdkTool {
    param([Parameter(Mandatory)][string] $Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $sdkBin = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $tool = Get-ChildItem -LiteralPath $sdkBin -Filter $Name -Recurse -File |
        Where-Object {
            $_.Directory.Name -eq "x64" -and
            $_.Directory.Parent.Name -match "^\d+\.\d+\.\d+\.\d+$"
        } |
        Sort-Object @{ Expression = { [Version]$_.Directory.Parent.Name }; Descending = $true } |
        Select-Object -First 1
    if (-not $tool) {
        throw "$Name was not found in PATH or the Windows SDK."
    }
    return $tool.FullName
}

function Convert-WinmdToIdl {
    param(
        [Parameter(Mandatory)][string] $Winmd,
        [Parameter(Mandatory)][string] $OutputDirectory,
        [Parameter(Mandatory)][string] $WinmdIdl
    )

    $toolVersion = Split-Path (Split-Path (Split-Path $WinmdIdl -Parent) -Parent) -Leaf
    $metadataDirectory = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\UnionMetadata\$toolVersion"
    if (-not (Test-Path $metadataDirectory -PathType Container)) {
        $metadataDirectory = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\UnionMetadata"
    }

    New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
    & $WinmdIdl /nologo /utf8 "/metadata_dir:$metadataDirectory" "/outdir:$OutputDirectory" $Winmd
    if ($LASTEXITCODE -ne 0) {
        throw "winmdidl failed for $Winmd."
    }

    $idl = Get-ChildItem -LiteralPath $OutputDirectory -Filter "*.idl" -File
    if (@($idl).Count -ne 1) {
        throw "Expected one generated IDL in $OutputDirectory."
    }
    return $idl.FullName
}

function Normalize-IdlLine {
    param([AllowEmptyString()][string] $Line)
    return ($Line.Trim() -replace "\s+", " ")
}

function Get-WinmdDeclarations {
    param([Parameter(Mandatory)][string] $Idl)

    $lines = Get-Content -LiteralPath $Idl
    $typeDefinition = [Array]::IndexOf($lines, "// Type definition")
    if ($typeDefinition -lt 0) {
        throw "Generated IDL does not contain a type-definition section: $Idl"
    }

    $declarations = @{}
    $attributes = [System.Collections.Generic.List[string]]::new()
    $namespaceStack = [System.Collections.Generic.List[string]]::new()
    $pendingNamespace = $null
    for ($index = $typeDefinition + 1; $index -lt $lines.Count; ++$index) {
        $line = Normalize-IdlLine $lines[$index]
        if (-not $line) {
            continue
        }
        if ($line -match "^namespace\s+([A-Za-z_][A-Za-z0-9_.]*)$") {
            $pendingNamespace = $Matches[1]
            continue
        }
        if ($line -eq "{" -and $pendingNamespace) {
            $namespaceStack.Add($pendingNamespace)
            $pendingNamespace = $null
            continue
        }
        if ($line -eq "}" -and $namespaceStack.Count -gt 0) {
            $namespaceStack.RemoveAt($namespaceStack.Count - 1)
            continue
        }
        if ($line.StartsWith("[")) {
            $attributes.Add($line)
            continue
        }
        if ($line -notmatch "^(enum|interface|runtimeclass)\s+([A-Za-z_][A-Za-z0-9_]*)") {
            if ($line -notin @("{", "}")) {
                $attributes.Clear()
            }
            continue
        }

        $kind = $Matches[1]
        $name = $Matches[2]
        $block = [System.Collections.Generic.List[string]]::new()
        $block.AddRange($attributes)
        $attributes.Clear()
        $block.Add($line)
        $braceDepth = ([regex]::Matches($line, "\{")).Count - ([regex]::Matches($line, "\}")).Count
        $openedBrace = $braceDepth -gt 0
        while ((($openedBrace -and $braceDepth -ne 0) -or
                (-not $openedBrace -and -not $line.EndsWith(";"))) -and
               ++$index -lt $lines.Count) {
            $line = Normalize-IdlLine $lines[$index]
            if (-not $line) {
                continue
            }
            $block.Add($line)
            $braceDepth += ([regex]::Matches($line, "\{")).Count - ([regex]::Matches($line, "\}")).Count
            $openedBrace = $openedBrace -or $line.Contains("{")
        }
        $qualifiedName = (@($namespaceStack) + $name) -join "."
        $declarations["$kind $qualifiedName"] = @($block)
    }
    return $declarations
}

function Compare-WinmdDeclarations {
    param(
        [Parameter(Mandatory)][hashtable] $Baseline,
        [Parameter(Mandatory)][hashtable] $Current
    )

    $errors = [System.Collections.Generic.List[string]]::new()
    foreach ($key in $Baseline.Keys | Sort-Object) {
        if (-not $Current.ContainsKey($key)) {
            $errors.Add("Removed public declaration: $key")
            continue
        }

        $baselineBlock = @($Baseline[$key])
        $currentBlock = @($Current[$key])
        if ($key.StartsWith("interface ")) {
            $baselineSignature = @($baselineBlock | Where-Object { -not $_.StartsWith("[deprecated(") })
            $currentSignature = @($currentBlock | Where-Object { -not $_.StartsWith("[deprecated(") })
            if (($baselineSignature -join "`n") -ne ($currentSignature -join "`n")) {
                $errors.Add("Changed existing WinRT interface: $key")
            }
            foreach ($deprecation in $baselineBlock | Where-Object { $_.StartsWith("[deprecated(") }) {
                if ($deprecation -notin $currentBlock) {
                    $errors.Add("Removed deprecation metadata from ${key}: $deprecation")
                }
            }
            continue
        }

        foreach ($line in $baselineBlock) {
            if ($line -notin $currentBlock) {
                $errors.Add("Removed or changed metadata in ${key}: $line")
            }
        }
    }
    return @($errors)
}

function Get-NativeExports {
    param(
        [Parameter(Mandatory)][string] $Dll,
        [Parameter(Mandatory)][string] $Dumpbin
    )

    $output = & $Dumpbin /nologo /exports $Dll
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $Dll."
    }
    $exports = [System.Collections.Generic.List[string]]::new()
    foreach ($row in $output | Where-Object { $_ -match "^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+" }) {
        if ($row -notmatch "^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+?)(?:\s+=.*)?\s*$") {
            throw "Unable to parse dumpbin export row: $row"
        }
        $exports.Add($Matches[1])
    }
    return @($exports | Sort-Object -Unique)
}

$currentWinmdPath = (Resolve-Path $CurrentWinmd).Path
$baselineWinmdPath = (Resolve-Path $BaselineWinmd).Path
$expectedExportNames = @(
    Get-Content -LiteralPath (Resolve-Path $ExpectedExports) |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith("#") } |
        Sort-Object -Unique
)
if ($expectedExportNames.Count -eq 0) {
    throw "The expected export list is empty."
}

$equivalentWinmdPath = if ($EquivalentWinmd) { (Resolve-Path $EquivalentWinmd).Path } else { $null }

$winmdIdl = Resolve-WindowsSdkTool "winmdidl.exe"
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    throw "dumpbin.exe was not found in PATH. Run the Windows build environment setup first."
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) "matter-winrt-abi-$([guid]::NewGuid())"
try {
    $baselineIdl = Convert-WinmdToIdl $baselineWinmdPath (Join-Path $temporaryRoot "baseline") $winmdIdl
    $currentIdl = Convert-WinmdToIdl $currentWinmdPath (Join-Path $temporaryRoot "current") $winmdIdl
    $baselineDeclarations = Get-WinmdDeclarations $baselineIdl
    $currentDeclarations = Get-WinmdDeclarations $currentIdl
    Write-Verbose "Comparing $($baselineDeclarations.Count) baseline declarations with $($currentDeclarations.Count) current declarations."
    $compatibilityErrors = @(Compare-WinmdDeclarations $baselineDeclarations $currentDeclarations)
    if ($compatibilityErrors.Count -ne 0) {
        throw "WinMD compatibility validation failed:`n - $($compatibilityErrors -join "`n - ")"
    }

    if ($equivalentWinmdPath) {
        $equivalentIdl = Convert-WinmdToIdl $equivalentWinmdPath (Join-Path $temporaryRoot "equivalent") $winmdIdl
        $equivalentDeclarations = Get-WinmdDeclarations $equivalentIdl
        $architectureErrors = @(
            Compare-WinmdDeclarations $currentDeclarations $equivalentDeclarations
            Compare-WinmdDeclarations $equivalentDeclarations $currentDeclarations
        )
        if ($architectureErrors.Count -ne 0) {
            throw "The architecture-specific WinMD metadata differs:`n - $($architectureErrors -join "`n - ")"
        }
    }

    foreach ($dllPath in $NativeDll) {
        $resolvedDll = (Resolve-Path $dllPath).Path
        $actualExports = Get-NativeExports $resolvedDll $dumpbin.Source
        $unexpected = @($actualExports | Where-Object { $_ -notin $expectedExportNames })
        $missing = @($expectedExportNames | Where-Object { $_ -notin $actualExports })
        if ($unexpected.Count -ne 0 -or $missing.Count -ne 0) {
            throw "Native export validation failed for ${resolvedDll}. Missing: $($missing -join ', '); unexpected: $($unexpected -join ', ')."
        }
    }
}
finally {
    if (Test-Path $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host "WinMD compatibility and native export validation passed."
