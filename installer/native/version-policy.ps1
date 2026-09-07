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
