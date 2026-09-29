# SPDX-License-Identifier: AGPL-3.0-only

[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'output-retention.ps1')

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('nexoai-retention-' + [Guid]::NewGuid().ToString('N'))
try {
    $toolRoot = Join-Path $testRoot 'native'
    $verification = Join-Path $toolRoot 'installer\msi-verification'
    $superseded = Join-Path $toolRoot 'installer\superseded'
    $output = Join-Path $toolRoot 'installer\output'
    $currentMsi = Join-Path $output 'NexoAIVision-1.0.0-x64.msi'
    $currentRun = Join-Path $verification 'run-20260102-010101'
    $image = Join-Path $currentRun 'administrative-image'
    $oldHistory = Join-Path $superseded '20260101-010101001-0.1.0'
    $unrelatedHistory = Join-Path $superseded 'operator-notes'
    $oldRun = Join-Path $verification 'run-20260101-010101'
    New-Item -ItemType Directory -Path $image,$output,$oldHistory,$unrelatedHistory,$oldRun -Force | Out-Null
    Set-Content -LiteralPath $currentMsi -Value 'current MSI'
    Set-Content -LiteralPath (Join-Path $image 'large-payload.bin') -Value 'verified image'
    Set-Content -LiteralPath (Join-Path $currentRun 'administrative-extraction.log') -Value 'diagnostics'
    Set-Content -LiteralPath (Join-Path $oldHistory 'old.msi') -Value 'old MSI'
    Set-Content -LiteralPath (Join-Path $unrelatedHistory 'notes.txt') -Value 'keep'
    Set-Content -LiteralPath (Join-Path $oldRun 'verification-result.json') -Value '{}'
    Set-Content -LiteralPath (Join-Path $output 'NexoAIVision-1.0.0-x64.cab') -Value 'active CAB'

    function Write-VerificationResult([string]$Run, [string]$Installer, [bool]$Success = $true) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Installer).Hash.ToLowerInvariant()
        $result = [ordered]@{
            installer = [IO.Path]::GetFullPath($Installer)
            acceptanceRoot = [IO.Path]::GetFullPath($Run)
            installerSha256 = if ($Success) { $hash } else { ('0' * 64) }
            msiDatabaseOpen = 'passed'
            wixMsiValidation = 'passed'
            fastPreview = $false
            administrativeExtractionExitCode = 0
            thirdPartyBytePreservation = 'passed'
            liveInstallPerformed = $false
        }
        $result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run 'verification-result.json')
    }

    Write-VerificationResult $currentRun $currentMsi
    Remove-VerifiedInstallerHistory -ToolRoot $toolRoot -SupersededRoot $superseded -VerificationRoot $verification `
        -CurrentInstaller $currentMsi -VerifiedRunRoot $currentRun
    if ((Test-Path $image) -or -not (Test-Path (Join-Path $currentRun 'verification-result.json')) -or
        -not (Test-Path (Join-Path $currentRun 'administrative-extraction.log')) -or
        -not (Test-Path $currentMsi) -or -not (Test-Path (Join-Path $output 'NexoAIVision-1.0.0-x64.cab')) -or
        (Test-Path $oldHistory) -or (Test-Path $oldRun) -or -not (Test-Path $unrelatedHistory)) {
        throw 'Successful retention did not prune only managed prior history and the current extraction image'
    }

    $failedRun = Join-Path $verification 'run-20260103-010101'
    $failureHistory = Join-Path $superseded '20260103-010101001-0.1.1'
    $failureOldRun = Join-Path $verification 'run-20251231-010101'
    New-Item -ItemType Directory -Path $failureHistory,$failureOldRun -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $failureHistory 'old.msi') -Value 'keep on failure'
    New-Item -ItemType Directory -Path (Join-Path $failedRun 'administrative-image') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $failedRun 'administrative-image\sentinel.bin') -Value 'keep'
    Write-VerificationResult $failedRun $currentMsi $false
    $rejected = $false
    try {
        Remove-VerifiedInstallerHistory -ToolRoot $toolRoot -SupersededRoot $superseded -VerificationRoot $verification `
            -CurrentInstaller $currentMsi -VerifiedRunRoot $failedRun
    } catch { $rejected = $true }
    if (-not $rejected -or -not (Test-Path (Join-Path $failedRun 'administrative-image\sentinel.bin')) -or
        -not (Test-Path $failureHistory) -or -not (Test-Path $failureOldRun)) {
        throw 'Failed verification pruned history or accepted MSI-mismatched evidence'
    }

    $outside = Join-Path $testRoot 'outside'
    New-Item -ItemType Directory -Path $outside | Out-Null
    $rejected = $false
    try {
        Remove-VerifiedInstallerHistory -ToolRoot $toolRoot -SupersededRoot $superseded -VerificationRoot $verification `
            -CurrentInstaller $currentMsi -VerifiedRunRoot $outside
    } catch { $rejected = $true }
    if (-not $rejected) { throw 'Retention accepted a run outside the managed verification directory' }

    $missingEvidenceRun = Join-Path $verification 'run-20260104-010101'
    New-Item -ItemType Directory -Path (Join-Path $missingEvidenceRun 'administrative-image') -Force | Out-Null
    $rejected = $false
    try {
        Remove-VerifiedInstallerHistory -ToolRoot $toolRoot -SupersededRoot $superseded -VerificationRoot $verification `
            -CurrentInstaller $currentMsi -VerifiedRunRoot $missingEvidenceRun
    } catch { $rejected = $true }
    if (-not $rejected -or -not (Test-Path (Join-Path $missingEvidenceRun 'administrative-image'))) {
        throw 'Retention accepted missing verification evidence'
    }
    Write-Host 'Installer output retention tests passed.'
} finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
