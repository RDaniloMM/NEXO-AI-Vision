# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [string]$TensorRtSource = (Join-Path $env:USERPROFILE 'Downloads\TensorRT-Enterprise-11.1.0.106\TensorRT-11.1.0.106'),
    [switch]$SkipWix
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$toolRoot = Join-Path $projectRoot '.tools\native'
$vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$cmakeBin = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaBin = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$downloads = Join-Path $toolRoot 'downloads'

function Assert-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Description was not found: $Path" }
}

function Ensure-Junction([string]$Path, [string]$Target) {
    if (Test-Path -LiteralPath $Path) { return }
    New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
}

function Download-File([string]$Uri, [string]$Path, [string]$ExpectedSha256) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Invoke-WebRequest -Uri $Uri -OutFile $Path }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -cne $ExpectedSha256) { throw "Hash mismatch for $Path" }
}

Assert-File (Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat') 'Visual Studio 2022 Build Tools'
Assert-File (Join-Path $vsRoot 'VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\dumpbin.exe') 'MSVC 14.44.35207'
Assert-File (Join-Path $cmakeBin 'cmake.exe') 'CMake supplied by Build Tools'
Assert-File (Join-Path $ninjaBin 'ninja.exe') 'Ninja supplied by Build Tools'
Assert-File (Join-Path $TensorRtSource 'include\NvInfer.h') 'TensorRT 11.1 header'
Assert-File (Join-Path $TensorRtSource 'lib\nvinfer_11.lib') 'TensorRT 11.1 import library'

New-Item -ItemType Directory -Force -Path $toolRoot, $downloads | Out-Null
Ensure-Junction (Join-Path $toolRoot 'vs') $vsRoot
Ensure-Junction (Join-Path $toolRoot 'ninja') $ninjaBin
New-Item -ItemType Directory -Force -Path (Join-Path $toolRoot 'tensorrt') | Out-Null
Ensure-Junction (Join-Path $toolRoot 'tensorrt\TensorRT-11.1.0.106') $TensorRtSource

$onnxArchive = Join-Path $downloads 'onnxruntime-win-x64-gpu-1.25.0.zip'
Download-File 'https://github.com/microsoft/onnxruntime/releases/download/v1.25.0/onnxruntime-win-x64-gpu-1.25.0.zip' $onnxArchive '125c9fe408f41b9ae1ad7138dac5ebb19a85e65438d1e368d21b50e6abb32f4e'
$onnxRoot = Join-Path $toolRoot 'onnxruntime-win-x64-gpu-1.25.0'
if (-not (Test-Path -LiteralPath (Join-Path $onnxRoot 'lib\onnxruntime.dll'))) { Expand-Archive -LiteralPath $onnxArchive -DestinationPath $toolRoot -Force }
Ensure-Junction (Join-Path $toolRoot 'onnxruntime-win-x64-1.25.0') $onnxRoot

$opencvArchive = Join-Path $downloads 'opencv-4.12.0-windows.exe'
if (-not (Test-Path -LiteralPath $opencvArchive -PathType Leaf)) {
    Invoke-WebRequest -Uri 'https://github.com/opencv/opencv/releases/download/4.12.0/opencv-4.12.0-windows.exe' -OutFile $opencvArchive
}
$opencvDll = Join-Path $toolRoot 'opencv\opencv\build\x64\vc16\bin\opencv_world4120.dll'
if (-not (Test-Path -LiteralPath $opencvDll -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path (Join-Path $toolRoot 'opencv') | Out-Null
    & $opencvArchive "-o$toolRoot\opencv" -y
    if ($LASTEXITCODE -ne 0) { throw "OpenCV extraction failed with exit code $LASTEXITCODE" }
}

$wheels = @(
    'nvidia-cuda-runtime-cu12==12.9.79', 'nvidia-cuda-nvcc-cu12==12.9.86',
    'nvidia-cuda-cccl-cu12==12.9.27', 'nvidia-cublas-cu12==12.9.2.10',
    'nvidia-cudnn-cu12==9.24.0.43', 'nvidia-cufft-cu12==11.4.1.4'
)
& py -m pip download --no-deps --dest $downloads $wheels
if ($LASTEXITCODE -ne 0) { throw 'CUDA dependency download failed' }

$wheelLayouts = @{
    'nvidia_cuda_runtime_cu12-12.9.79-py3-none-win_amd64.whl' = 'cuda-runtime'
    'nvidia_cuda_nvcc_cu12-12.9.86-py3-none-win_amd64.whl' = 'cuda-nvcc'
    'nvidia_cuda_cccl_cu12-12.9.27-py3-none-win_amd64.whl' = 'cuda-cccl'
}
foreach ($entry in $wheelLayouts.GetEnumerator()) {
    $wheel = Join-Path $downloads $entry.Key
    Assert-File $wheel "CUDA package $($entry.Key)"
    $target = Join-Path $toolRoot $entry.Value
    if (-not (Test-Path -LiteralPath $target)) { Expand-Archive -LiteralPath $wheel -DestinationPath $target -Force }
}

& (Join-Path $PSScriptRoot 'Provision-TrackingDependencies.ps1')
& (Join-Path $PSScriptRoot 'Provision-Resvg.ps1')
if (-not $SkipWix) {
    $dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
    if ($null -eq $dotnet) { throw 'Install .NET SDK 8.0.423 or invoke this script with -SkipWix.' }
    $sdks = @(& $dotnet.Source --list-sdks)
    if ($LASTEXITCODE -ne 0 -or $sdks -notmatch '^8\.0\.423') {
        throw 'Install .NET SDK 8.0.423 or invoke this script with -SkipWix.'
    }
    & (Join-Path $projectRoot 'installer\native\Provision-Wix.ps1') -DotnetPath $dotnet.Source
}

Write-Host "Native build environment initialized under $toolRoot"
