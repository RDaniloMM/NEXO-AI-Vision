# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory)][string]$LauncherExecutable, [Parameter(Mandatory)][string]$ExpectedVersion)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version-policy.ps1')
function Assert-Rejected([string]$Path, [string]$Version) {
    try { $null = Assert-LauncherBuildVersion $Path $Version }
    catch { return }
    throw "Accepted invalid launcher/version pair: $Version"
}
if ($ExpectedVersion.EndsWith('-dev')) {
    Assert-Rejected $LauncherExecutable '0.0.0.0'
} else {
    $actual = Assert-LauncherBuildVersion $LauncherExecutable $ExpectedVersion
    $parts = $ExpectedVersion.Split('.')
    if ($actual -cne "$($parts[0]).$($parts[1]).$($parts[3])") { throw 'MSI mapping changed' }
}
foreach ($invalid in @('1.2.3', '01.2.3.4', '1.2.65536.4', '256.2.3.4', '1.256.3.4', '1.2.3.999999999999999999', '-1.2.3.4', '1.2.3.4-preview')) {
    Assert-Rejected $LauncherExecutable $invalid
}
$mismatch = if ($ExpectedVersion -ceq '9.8.7.6') { '9.8.7.5' } else { '9.8.7.6' }
Assert-Rejected $LauncherExecutable $mismatch
Assert-Rejected (Join-Path $PSScriptRoot 'missing-launcher.exe') '1.2.3.4'
Assert-Rejected $PSCommandPath '0.0.0.0'
Write-Output "PASS: installer version policy ($ExpectedVersion)"
