# SPDX-License-Identifier: AGPL-3.0-only

Set-StrictMode -Version Latest

function Get-RelativePathCompat([string]$Root, [string]$Path) {
    # Windows PowerShell 5.1 (.NET Framework) lacks [System.IO.Path]::GetRelativePath.
    # Same contract for same-volume paths (backslash separators); falls back to a
    # URI-based relative path otherwise. Shared via dot-sourcing by build-installer,
    # test-installer and test-payload-policy.
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $pathFull = [System.IO.Path]::GetFullPath($Path)
    if ($pathFull.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $pathFull.Substring($rootFull.Length)
    }
    return [System.Uri]::UnescapeDataString(
        ([System.Uri]$rootFull).MakeRelativeUri([System.Uri]$pathFull).ToString())
}

function Get-ForbiddenPayloadFiles([string]$Root) {
    # Development inputs may exist in the repository but must never cross into MSI staging.
    $allowedExecutablePaths = @(
        'bin\NexoAIVisionLauncher.exe',
        'bin\NexoAIVision.exe',
        'bin\engine-builder\trtexec.exe'
    )
    $allowedModelPaths = @(
        'bin\models\ppe.engine',
        'bin\models\pose.engine',
        'bin\models\ppe.onnx',
        'bin\models\ppe.onnx.manifest.json',
        'bin\models\pose.onnx',
        'bin\models\pose.onnx.manifest.json',
        'bin\engine-builder\ppe.onnx',
        'bin\engine-builder\pose.onnx'
    )
    $patterns = @(
        '(^|[\\/])\.env($|\.)',
        '\.(pt|pth|engine|plan|onnx|safetensors|csv|xlsx|xls|parquet|arrow|feather|bin|pkl|pickle|weights|whl|py|pyc|pyo|pyd|jsonl)$',
        '(^|[\\/])(data|datasets?|fixtures?|weights?|models?|engines?|parity(?:-receipts?)?|\.atl|caches?|__pycache__|site-packages|dist-packages|cvat|supervision|ultralytics|torch)([\\/]|$)',
        '(^|[\\/])python[^\\/]*\.exe$',
        '(^|[\\/])python[^\\/]*\.dll$',
        '(^|[\\/])python[^\\/]*\.zip$',
        '(^|[\\/])Lib[\\/](encodings|importlib|asyncio|collections|site-packages)([\\/]|$)',
        'engine[^\\/]*manifest[^\\/]*\.json$',
        'parity[^\\/]*\.json$',
        '\.(cpp|cxx|cc|h|hpp|lib|pdb|obj|pfx|p12|pem|key|cer)$'
    )
    $violations = foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        $relative = Get-RelativePathCompat $Root $file.FullName
        if ($file.Extension -ieq '.exe' -and $relative -notin $allowedExecutablePaths) {
            $relative
            continue
        }
        if ($relative -in $allowedModelPaths) {
            continue
        }
        foreach ($pattern in $patterns) {
            if ($relative -match $pattern) {
                $relative
                break
            }
        }
    }
    @($violations | Sort-Object -Unique)
}

function Assert-NoForbiddenPayloadFiles([string]$Root, [string]$Context = "payload") {
    $violations = @(Get-ForbiddenPayloadFiles $Root)
    if ($violations.Count -gt 0) {
        throw "Forbidden development/QA files entered the $Context`: $($violations -join ', ')"
    }
}
