# SPDX-License-Identifier: AGPL-3.0-only

function Assert-LauncherBuildVersion {
    param([string]$LauncherExecutable, [string]$FileVersion)
    if ($FileVersion -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw 'FileVersion must be four canonical decimal components'
    }
    $parts = @($FileVersion.Split('.') | ForEach-Object {
        if ($_.Length -gt 5 -or [int]$_ -gt 65535) { throw 'FileVersion components must be in [0, 65535]' }
        [int]$_
    })
    if ($parts[0] -gt 255 -or $parts[1] -gt 255) {
        throw 'FileVersion major and minor must fit Windows Installer ranges [0, 255]'
    }
    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($LauncherExecutable)
    $actual = '{0}.{1}.{2}.{3}' -f $info.FileMajorPart, $info.FileMinorPart, $info.FileBuildPart, $info.FilePrivatePart
    if ([string]::IsNullOrWhiteSpace($info.FileVersion) -or $info.IsPrivateBuild -or $actual -cne $FileVersion) {
        throw "Launcher version '$($info.FileVersion)' does not match -FileVersion $FileVersion. Reconfigure native with -DCUAJONE_FILE_VERSION=$FileVersion and rebuild before packaging."
    }
    # Preserve the existing upgrade-version mapping: major.minor.revision.
    return '{0}.{1}.{2}' -f $parts[0], $parts[1], $parts[3]
}

function Resolve-NextFileVersion {
    param(
        [ValidateRange(0, 255)][int]$Major = 0,
        [ValidateRange(0, 255)][int]$Minor = 1,
        [ValidateRange(0, 65535)][int]$Build = 0,
        [Parameter(Mandatory)][string]$StatePath
    )
    $state = $null
    if (Test-Path -LiteralPath $StatePath -PathType Leaf) {
        $raw = Get-Content -LiteralPath $StatePath -Raw -Encoding UTF8
        $state = $raw | ConvertFrom-Json
    }
    if ($null -eq $state) {
        # First use: bootstrap the counter at revision 0 for this major.minor line.
        $state = [pscustomobject]@{ major = $Major; minor = $Minor; lastRevision = 0 }
        $revision = 0
    } elseif ([int]$state.major -eq $Major -and [int]$state.minor -eq $Minor) {
        $revision = [int]$state.lastRevision + 1
    } else {
        # New major.minor line: reset the revision counter.
        $state = [pscustomobject]@{ major = $Major; minor = $Minor; lastRevision = 0 }
        $revision = 0
    }
    if ($revision -gt 65535) {
        throw "Revision counter overflow for $Major.$Minor (lastRevision=$($state.lastRevision))"
    }
    $state = [pscustomobject]@{ major = $Major; minor = $Minor; lastRevision = $revision }
    $state | ConvertTo-Json -Compress | Set-Content -LiteralPath $StatePath -Encoding UTF8 -NoNewline
    $fileVersion = '{0}.{1}.{2}.{3}' -f $Major, $Minor, $Build, $revision
    $version = '{0}.{1}.{2}-internal.{3}' -f $Major, $Minor, $Build, $revision
    return [pscustomobject]@{ FileVersion = $fileVersion; Version = $version; Revision = $revision }
}
