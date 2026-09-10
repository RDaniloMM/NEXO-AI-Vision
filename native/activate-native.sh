#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# Linux native build environment (mirror of activate-native.ps1).
#
# System prerequisites (Ubuntu 24.04 / DGX OS 7, GCC 13 default toolchain):
#   sudo apt update
#   sudo apt install -y build-essential cmake ninja-build pkg-config \
#       libopencv-dev git patch unzip curl
# Fase 2 (Qt6 GUI) additionally needs: qt6-base-dev
# Fase 3 (CUDA/TensorRT) additionally needs: NVIDIA CUDA + TensorRT SDKs
#
# Usage:
#   source native/activate-native.sh [--cpu-only]
# (--cpu-only is accepted for parity with the .ps1; Linux Fase 1 builds the
# POSIX runtime CPU-only: no CUDA/TensorRT until Fase 3.)

if [ -n "${BASH_SOURCE:-}" ] && [ "${BASH_SOURCE[0]}" != "$0" ]; then
    _CUAJONE_SOURCED=1
else
    echo "This script must be sourced: source native/activate-native.sh" >&2
    exit 1
fi

_CUAJONE_CPU_ONLY=0
for _cuajone_arg in "$@"; do
    case "${_cuajone_arg}" in
        --cpu-only) _CUAJONE_CPU_ONLY=1 ;;
        *) echo "activate-native.sh: unknown argument '${_cuajone_arg}'" >&2; return 1 ;;
    esac
done

_CUAJONE_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_CUAJONE_PROJECT_ROOT="$(dirname "${_CUAJONE_SCRIPT_DIR}")"
_CUAJONE_TOOL_ROOT="${_CUAJONE_PROJECT_ROOT}/.tools/native"

# --- Toolchain locations (override by exporting before sourcing) ---
# ONNX Runtime: Linux 1.25.0 package (Fase 1 runtime); provision it with the
# 'Provision ONNX Runtime' step of .github/workflows/linux-build.yml.
: "${ONNXRUNTIME_ROOT:=${_CUAJONE_TOOL_ROOT}/linux-onnxruntime-1.25.0}"
# OpenCV: empty means CMake system search, which finds apt libopencv-dev
# (/usr/lib/x86_64-linux-gnu/cmake/opencv4). Point at a custom build if needed.
: "${OpenCV_DIR:=}"
# TensorRT / CUDA: Fase 3 only; defaults match the official .deb/tarball layouts.
: "${TENSORRT_ROOT:=/opt/tensorrt}"
: "${CUDA_ROOT:=/usr/local/cuda}"
export ONNXRUNTIME_ROOT OpenCV_DIR TENSORRT_ROOT CUDA_ROOT

# --- cmake / ninja on PATH ---
if [ -z "${CUAJONE_CMAKE_BIN:-}" ]; then
    if command -v cmake >/dev/null 2>&1; then
        CUAJONE_CMAKE_BIN="$(dirname "$(command -v cmake)")"
    else
        echo "activate-native.sh: cmake not found. Install it (sudo apt install cmake) or set CUAJONE_CMAKE_BIN." >&2
        return 1
    fi
fi
export CUAJONE_CMAKE_BIN
case ":${PATH}:" in
    *":${CUAJONE_CMAKE_BIN}:"*) ;;
    *) export PATH="${CUAJONE_CMAKE_BIN}:${PATH}" ;;
esac

if [ "${_CUAJONE_CPU_ONLY}" -eq 1 ]; then
    unset TENSORRT_ROOT CUDA_ROOT
fi

# --- Fase 1 dependency warnings (non-fatal; CI installs these via apt) ---
_cuaje_warn_missing() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "activate-native.sh: warning: '$1' not found ($2)" >&2
    fi
}
_cuaje_warn_missing gcc "sudo apt install build-essential"
_cuaje_warn_missing ninja "sudo apt install ninja-build"
if [ ! -d /usr/lib/x86_64-linux-gnu/cmake/opencv4 ] && [ -z "${OpenCV_DIR}" ]; then
    echo "activate-native.sh: warning: system OpenCV not found (sudo apt install libopencv-dev) and OpenCV_DIR is empty" >&2
fi
if [ ! -f "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h" ]; then
    echo "activate-native.sh: warning: Linux ONNX Runtime 1.25.0 not found under ${ONNXRUNTIME_ROOT}" >&2
    echo "  Replicate the 'Provision ONNX Runtime' step of .github/workflows/linux-build.yml" >&2
fi
if [ ! -f "${_CUAJONE_TOOL_ROOT}/dependencies/byte-track-eigen-a865158906f6138465668810a98ffd918d95f9a3/.cuajone-source-receipt.json" ]; then
    echo "activate-native.sh: warning: pinned ByteTrack/Eigen sources are missing under .tools/native/dependencies" >&2
    echo "  Replicate the 'Provision tracking dependencies' step of .github/workflows/linux-build.yml" >&2
fi
unset -f _cuaje_warn_missing

echo "Nexo AI Vision Linux native environment activated from ${_CUAJONE_TOOL_ROOT} (CPU only: ${_CUAJONE_CPU_ONLY})"
echo "  ONNXRUNTIME_ROOT=${ONNXRUNTIME_ROOT} (Fase 1)"
echo "  OpenCV_DIR=${OpenCV_DIR:-<system search>}"
echo "  TENSORRT_ROOT=${TENSORRT_ROOT:-<unset>} (Fase 3)"
echo "  CUDA_ROOT=${CUDA_ROOT:-<unset>} (Fase 3)"
echo "  CUAJONE_CMAKE_BIN=${CUAJONE_CMAKE_BIN}"
unset _CUAJONE_CPU_ONLY _CUAJONE_SCRIPT_DIR _CUAJONE_PROJECT_ROOT _CUAJONE_TOOL_ROOT _cuajone_arg
