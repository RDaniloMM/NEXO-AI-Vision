#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Thin Linux wrapper: the install manifest and CPack rules live in
# native/cmake/Packaging.cmake. This script provisions verified native inputs,
# prepares a build directory, and invokes CMake install/CPack.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build-dist.sh [--build-dir DIR] [--output-dir DIR] [--version X.Y.Z]
                      [--with-tensorrt]

The package always uses the Qt6 launcher and inference viewer.
TensorRT is opt-in and requires TENSORRT_ROOT (default: /opt/tensorrt) and a
host CUDA toolkit (default: /usr/local/cuda). Engines are never built or bundled.
EOF
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/../.." && pwd)"
# shellcheck disable=SC1091
source "${script_dir}/dependency-lock.env"
chmod +x "${script_dir}/run.sh" "${script_dir}/postinst"
build_dir="${project_root}/.tools/native/build/linux-package"
output_dir="${project_root}/.tools/native/dist"
version="0.1.0"
with_tensorrt=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) [[ $# -ge 2 ]] || { echo "Missing value for --build-dir" >&2; exit 2; }; build_dir="$2"; shift 2 ;;
        --output-dir) [[ $# -ge 2 ]] || { echo "Missing value for --output-dir" >&2; exit 2; }; output_dir="$2"; shift 2 ;;
        --version) [[ $# -ge 2 ]] || { echo "Missing value for --version" >&2; exit 2; }; version="$2"; shift 2 ;;
        --with-tensorrt) with_tensorrt=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "--version must be Debian-compatible major.minor.patch" >&2
    exit 2
fi

if [[ -n "${NEXOAI_CMAKE_BIN:-}" ]]; then
    if [[ -x "${NEXOAI_CMAKE_BIN}/cmake" ]]; then
        cmake_bin="${NEXOAI_CMAKE_BIN}/cmake"
    elif [[ -x "${NEXOAI_CMAKE_BIN}" ]]; then
        cmake_bin="${NEXOAI_CMAKE_BIN}"
    else
        echo "NEXOAI_CMAKE_BIN does not point to cmake or its directory: ${NEXOAI_CMAKE_BIN}" >&2
        exit 1
    fi
elif command -v cmake >/dev/null 2>&1; then
    cmake_bin="$(command -v cmake)"
else
    echo "cmake not found; install it or set NEXOAI_CMAKE_BIN" >&2
    exit 1
fi

echo "Provisioning verified Linux native dependencies..."
bash "${script_dir}/provision-deps.sh" --project-root "$project_root"
onnxruntime_root="${project_root}/.tools/native/linux-onnxruntime-${ONNX_RUNTIME_VERSION}"

if [[ "$with_tensorrt" -eq 1 ]]; then
    trt_root="${TENSORRT_ROOT:-/opt/tensorrt}"
    cuda_root="${CUDA_ROOT:-/usr/local/cuda}"
    [[ -d "$trt_root" ]] || { echo "TensorRT root not found: $trt_root" >&2; exit 1; }
    [[ -d "$cuda_root" ]] || { echo "CUDA root not found: $cuda_root" >&2; exit 1; }
else
    trt_root=""
    cuda_root=""
fi

build_dir="$(mkdir -p "$build_dir" && cd "$build_dir" && pwd)"
output_dir="$(mkdir -p "$output_dir" && cd "$output_dir" && pwd)"
install_stage="${build_dir}/install-stage"
rm -rf "$install_stage"
mkdir -p "$install_stage"

trt_args=(-DNEXOAI_ENABLE_TENSORRT=OFF -DTENSORRT_ROOT=)
if [[ "$with_tensorrt" -eq 1 ]]; then
    trt_args=(-DNEXOAI_ENABLE_TENSORRT=ON "-DTENSORRT_ROOT=${trt_root}" "-DCUDAToolkit_ROOT=${cuda_root}")
fi

"$cmake_bin" -S "${project_root}/native" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEXOAI_BUILD_RUNTIME=ON \
    -DNEXOAI_BUILD_TESTS=OFF \
    "-DNEXOAI_FILE_VERSION=${version}.0" \
    "-DNEXOAI_PRODUCT_VERSION=${version}" \
    "-DNEXOAI_PACKAGE_VERSION=${version}" \
    "-DONNXRUNTIME_ROOT=${onnxruntime_root}" \
    "${trt_args[@]}"
"$cmake_bin" --build "$build_dir" --parallel
"$cmake_bin" --install "$build_dir" --prefix "$install_stage" --config Release

if command -v cpack >/dev/null 2>&1; then
    cpack_bin="$(command -v cpack)"
elif [[ -x "$(dirname "$cmake_bin")/cpack" ]]; then
    cpack_bin="$(dirname "$cmake_bin")/cpack"
else
    echo "cpack not found beside CMake or on PATH" >&2
    exit 1
fi
"$cpack_bin" --config "${build_dir}/CPackConfig.cmake" -G DEB -B "$output_dir"
"$cpack_bin" --config "${build_dir}/CPackConfig.cmake" -G TGZ -B "$output_dir"

echo "Created Linux packages in ${output_dir}"
echo "  Debian: ${output_dir}/nexoai-vision-${version}-linux-x86_64.deb"
echo "  Tarball: ${output_dir}/nexoai-vision-${version}-linux-x86_64.tar.gz"
