# SPDX-License-Identifier: AGPL-3.0-only

[CmdletBinding()]
param(
    [string]$ToolRoot,
    [string]$DotnetPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$localToolRoot = Join-Path $projectRoot ".tools\native"
if ([string]::IsNullOrWhiteSpace($ToolRoot)) { $ToolRoot = $localToolRoot }
$fullToolRoot = [System.IO.Path]::GetFullPath($ToolRoot).TrimEnd('\')
$expectedToolRoot = [System.IO.Path]::GetFullPath($localToolRoot).TrimEnd('\')
if ($fullToolRoot -cne $expectedToolRoot) {
    throw "WiX must remain under the repository-local .tools/native cache: $fullToolRoot"
}

$wixVersion = "6.0.2"
$wixToolRoot = Join-Path $ToolRoot "wix"
$wix = Join-Path $wixToolRoot "wix.exe"
$localDotnet = Join-Path $ToolRoot "dotnet-sdk\dotnet.exe"
if ([string]::IsNullOrWhiteSpace($DotnetPath)) {
    if (Test-Path -LiteralPath $localDotnet -PathType Leaf) {
        $DotnetPath = $localDotnet
    } else {
        $dotnetCommand = Get-Command dotnet -ErrorAction SilentlyContinue
        if ($null -eq $dotnetCommand) {
            throw "dotnet was not found. Install the .NET SDK or provide -DotnetPath."
        }
        $DotnetPath = $dotnetCommand.Source
    }
}
if (-not (Test-Path -LiteralPath $DotnetPath -PathType Leaf)) {
    throw "dotnet executable was not found: $DotnetPath"
}

New-Item -ItemType Directory -Path $wixToolRoot -Force | Out-Null

$installedVersion = ""
if (Test-Path -LiteralPath $wix -PathType Leaf) {
    $installedVersion = (& $wix --version).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Could not query the local WiX CLI at $wix"
    }
}

if ($installedVersion -notmatch "^$([regex]::Escape($wixVersion))(?:\+|$)") {
    if ([string]::IsNullOrWhiteSpace($installedVersion)) {
        & $DotnetPath tool install wix --tool-path $wixToolRoot --version $wixVersion
    } else {
        & $DotnetPath tool update wix --tool-path $wixToolRoot --version $wixVersion
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to provision WiX $wixVersion under $wixToolRoot"
    }
}

if (-not (Test-Path -LiteralPath $wix -PathType Leaf)) {
    throw "WiX $wixVersion did not provide wix.exe at $wix"
}
$reportedVersion = (& $wix --version).Trim()
if ($LASTEXITCODE -ne 0 -or $reportedVersion -notmatch "^$([regex]::Escape($wixVersion))(?:\+|$)") {
    throw "Expected WiX $wixVersion at $wix; got '$reportedVersion'"
}

Push-Location $wixToolRoot
try {
    $requiredExtensions = @(
        "WixToolset.UI.wixext/$wixVersion",
        "WixToolset.Util.wixext/$wixVersion"
    )
    foreach ($extension in $requiredExtensions) {
        $extensionName = $extension.Replace('/', ' ')
        $installedExtensions = @(& $wix extension list)
        if ($LASTEXITCODE -ne 0) {
            throw "Could not list local WiX extensions"
        }
        if ($installedExtensions -notcontains $extensionName) {
            & $wix extension add $extension
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to provision required WiX extension: $extension"
            }
        }
    }

    $installedExtensions = @(& $wix extension list)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not verify local WiX extensions"
    }
} finally {
    Pop-Location
}

foreach ($extension in @("WixToolset.UI.wixext $wixVersion", "WixToolset.Util.wixext $wixVersion")) {
    if ($installedExtensions -notcontains $extension) {
        throw "Required local WiX extension is missing: $extension"
    }
}

Write-Host "WiX $wixVersion and its required extensions are ready under $wixToolRoot"
