# SPDX-License-Identifier: AGPL-3.0-only

function Remove-VerifiedInstallerHistory {
    param(
        [Parameter(Mandatory)][string]$ToolRoot,
        [Parameter(Mandatory)][string]$SupersededRoot,
        [Parameter(Mandatory)][string]$VerificationRoot,
        [Parameter(Mandatory)][string]$CurrentInstaller,
        [Parameter(Mandatory)][string]$VerifiedRunRoot
    )

    $fullToolRoot = [System.IO.Path]::GetFullPath($ToolRoot).TrimEnd('\')
    $fullSuperseded = [System.IO.Path]::GetFullPath($SupersededRoot).TrimEnd('\')
    $fullVerification = [System.IO.Path]::GetFullPath($VerificationRoot).TrimEnd('\')
    $fullInstaller = [System.IO.Path]::GetFullPath($CurrentInstaller)
    $fullRun = [System.IO.Path]::GetFullPath($VerifiedRunRoot).TrimEnd('\')
    foreach ($root in @($fullSuperseded, $fullVerification)) {
        if (-not $root.StartsWith("$fullToolRoot\", [StringComparison]::OrdinalIgnoreCase)) {
            throw "Retention directory escaped the native tool root: $root"
        }
    }
    if ((Split-Path -Parent $fullRun) -ine $fullVerification -or
        (Split-Path -Leaf $fullRun) -notmatch '^run-[A-Za-z0-9-]+$') {
        throw "Verified run is not a direct managed verification child: $fullRun"
    }

    $resultPath = Join-Path $fullRun 'verification-result.json'
    $imagePath = Join-Path $fullRun 'administrative-image'
    if (-not (Test-Path -LiteralPath $fullInstaller -PathType Leaf) -or
        -not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        throw 'Retention requires the current MSI and its completed verification result'
    }

    # Validate the verifier receipt and bind it to the current MSI bytes before deletion.
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -ErrorAction Stop
    $installerHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $fullInstaller).Hash.ToLowerInvariant()
    if ([System.IO.Path]::GetFullPath([string]$result.installer) -ine $fullInstaller -or
        [System.IO.Path]::GetFullPath([string]$result.acceptanceRoot).TrimEnd('\') -ine $fullRun -or
        [string]$result.installerSha256 -cne $installerHash -or
        [string]$result.msiDatabaseOpen -cne 'passed' -or
        [string]$result.wixMsiValidation -cne 'passed' -or
        [bool]$result.fastPreview -or
        [int]$result.administrativeExtractionExitCode -ne 0 -or
        [string]$result.thirdPartyBytePreservation -cne 'passed' -or
        [bool]$result.liveInstallPerformed) {
        throw 'Verification evidence is unsuccessful or does not match the current MSI; installer history retained'
    }

    # Identify only dated superseded archives and prior run-* verification directories.
    $removeCandidates = @()
    foreach ($candidate in Get-ChildItem -LiteralPath $fullSuperseded -Directory -Force -ErrorAction Stop) {
        if ($candidate.Name -match '^\d{8}-\d{9}-.+$') { $removeCandidates += $candidate }
    }
    foreach ($candidate in Get-ChildItem -LiteralPath $fullVerification -Directory -Filter 'run-*' -Force -ErrorAction Stop) {
        if ($candidate.FullName -ine $fullRun) { $removeCandidates += $candidate }
    }
    if (Test-Path -LiteralPath $imagePath) { $removeCandidates += Get-Item -LiteralPath $imagePath -Force }

    # Refuse all deletion when any ancestor, managed directory, or candidate descendant is a reparse point.
    $guardedPaths = @(
        $fullToolRoot, (Join-Path $fullToolRoot 'installer'),
        $fullSuperseded, $fullVerification, $fullRun, $fullInstaller, $resultPath
    )
    $ancestor = $fullToolRoot
    while ($ancestor) {
        $guardedPaths += $ancestor
        $parent = Split-Path -Parent $ancestor
        if ($parent -ieq $ancestor) { break }
        $ancestor = $parent
    }
    foreach ($path in $guardedPaths | Select-Object -Unique) {
        $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Retention refuses reparse points: $path"
        }
    }
    foreach ($candidate in $removeCandidates) {
        if (($candidate.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Retention refuses reparse points: $($candidate.FullName)"
        }
        $pending = [System.Collections.Generic.Stack[string]]::new()
        $pending.Push($candidate.FullName)
        while ($pending.Count -gt 0) {
            $current = $pending.Pop()
            $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Retention refuses reparse points: $current"
            }
            if ($item.PSIsContainer) {
                foreach ($child in Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop) {
                    $pending.Push($child.FullName)
                }
            }
        }
    }

    foreach ($candidate in $removeCandidates) {
        Remove-Item -LiteralPath $candidate.FullName -Recurse -Force -ErrorAction Stop
    }
}
