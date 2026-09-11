#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Provision the verified Linux native dependencies used by CMake and CI.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${script_dir}/dependency-lock.env"

usage() {
    cat <<'EOF'
Usage: provision-deps.sh [--project-root DIR]

Download and verify the pinned ByteTrack-Eigen/Eigen sources and the Linux
ONNX Runtime package into the repository-local .tools/native cache.
EOF
}

project_root="$(cd "${script_dir}/../.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --project-root)
            [[ $# -ge 2 ]] || { echo "Missing value for --project-root" >&2; exit 2; }
            project_root="$(cd "$2" && pwd)"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

tool_root="${project_root}/.tools/native"
download_root="${tool_root}/downloads"
dependency_root="${tool_root}/dependencies"
ort_root="${tool_root}/linux-onnxruntime-${ONNX_RUNTIME_VERSION}"
patch_path="${project_root}/native/third_party/byte-track-eigen-cuajone.patch"

for command_name in curl sha256sum unzip tar git; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "${command_name} is required to provision Linux native dependencies" >&2
        exit 1
    fi
done
[[ -f "$patch_path" ]] || { echo "Dependency patch not found: $patch_path" >&2; exit 1; }

mkdir -p "$download_root" "$dependency_root" "${tool_root}/temp"
temporary_root="$(mktemp -d "${tool_root}/temp/provision-deps.XXXXXX")"
cleanup() {
    rm -rf -- "$temporary_root"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

patch_sha256="$(sha256sum "$patch_path" | cut -d' ' -f1)"

fetch_verified() {
    local url="$1" archive="$2" expected_sha256="$3" description="$4"
    if [[ ! -f "$archive" ]]; then
        local partial="${temporary_root}/$(basename "$archive").part"
        curl -fL --retry 3 --retry-delay 1 --silent --show-error \
            -o "$partial" "$url"
        mv -- "$partial" "$archive"
    fi
    local actual_sha256
    actual_sha256="$(sha256sum "$archive" | cut -d' ' -f1)"
    if [[ "$actual_sha256" != "$expected_sha256" ]]; then
        echo "$description archive SHA-256 mismatch: expected $expected_sha256, got $actual_sha256" >&2
        exit 1
    fi
}

provision_tracking_dependency() {
    local archive="$1" target="$2" expected_root="$3" marker="$4" apply_patch="$5"
    local receipt="${target}/.cuajone-source-receipt.json"
    local archive_sha256
    archive_sha256="$(sha256sum "$archive" | cut -d' ' -f1)"

    if [[ -f "${target}/${marker}" && -f "$receipt" ]] \
        && grep -qF "\"archive_sha256\":\"${archive_sha256}\"" "$receipt" \
        && grep -qF "\"patch_sha256\":\"${patch_sha256}\"" "$receipt" \
        && { [[ "$apply_patch" -eq 0 ]] || grep -qF "retained_track_count" "${target}/${marker}"; }; then
        echo "Tracking dependency cache hit: ${target}"
        return 0
    fi

    rm -rf -- "$target"
    local extraction_root="${temporary_root}/$(basename "$target")"
    mkdir -p "$extraction_root"
    unzip -q "$archive" -d "$extraction_root"
    local expanded="${extraction_root}/${expected_root}"
    [[ -d "$expanded" ]] || {
        echo "Verified dependency archive did not contain expected root: ${expected_root}" >&2
        exit 1
    }
    if [[ "$apply_patch" -eq 1 ]]; then
        (
            cd "$expanded"
            GIT_CEILING_DIRECTORIES="$extraction_root" git apply --no-index --check "$patch_path"
            GIT_CEILING_DIRECTORIES="$extraction_root" git apply --no-index "$patch_path"
        )
    fi
    mv -- "$expanded" "$target"
    printf '{"archive_sha256":"%s","patch_sha256":"%s"}' \
        "$archive_sha256" "$patch_sha256" > "${target}/.cuajone-source-receipt.json"
}

byte_track_archive="${download_root}/byte-track-eigen-${BYTE_TRACK_COMMIT}.zip"
eigen_archive="${download_root}/eigen-${EIGEN_COMMIT}.zip"
fetch_verified \
    "https://codeload.github.com/cj-mills/byte-track-eigen/zip/${BYTE_TRACK_COMMIT}" \
    "$byte_track_archive" "$BYTE_TRACK_SHA256" "ByteTrack-Eigen"
fetch_verified \
    "https://gitlab.com/libeigen/eigen/-/archive/${EIGEN_COMMIT}/eigen-${EIGEN_COMMIT}.zip" \
    "$eigen_archive" "$EIGEN_SHA256" "Eigen"

provision_tracking_dependency \
    "$byte_track_archive" \
    "${dependency_root}/byte-track-eigen-${BYTE_TRACK_COMMIT}" \
    "byte-track-eigen-${BYTE_TRACK_COMMIT}" \
    "include/BYTETracker.h" 1
provision_tracking_dependency \
    "$eigen_archive" \
    "${dependency_root}/eigen-${EIGEN_COMMIT}" \
    "eigen-${EIGEN_COMMIT}" \
    "Eigen/Dense" 0

ort_archive="${download_root}/onnxruntime-linux-x64-${ONNX_RUNTIME_VERSION}.tgz"
# Microsoft does not publish a checksum file for this asset. Do not invent one:
# the helper validates the expected archive layout, and CMake validates the same
# headers and shared library before configuring.
if [[ ! -f "${ort_archive}" ]]; then
    partial="${temporary_root}/$(basename "$ort_archive").part"
    curl -fL --retry 3 --retry-delay 1 --silent --show-error -o "$partial" \
        "https://github.com/microsoft/onnxruntime/releases/download/v${ONNX_RUNTIME_VERSION}/onnxruntime-linux-x64-${ONNX_RUNTIME_VERSION}.tgz"
    mv -- "$partial" "$ort_archive"
fi

if [[ ! -f "${ort_root}/include/onnxruntime_cxx_api.h" \
    || ! -e "${ort_root}/lib/libonnxruntime.so" ]]; then
    rm -rf -- "$ort_root"
    ort_extraction_root="${temporary_root}/onnxruntime"
    mkdir -p "$ort_extraction_root"
    tar -xzf "$ort_archive" -C "$ort_extraction_root" --strip-components=1
    [[ -f "${ort_extraction_root}/include/onnxruntime_cxx_api.h" \
        && -e "${ort_extraction_root}/lib/libonnxruntime.so" ]] || {
        echo "ONNX Runtime archive has an unexpected layout; expected include/ and lib/libonnxruntime.so" >&2
        exit 1
    }
    mv -- "$ort_extraction_root" "$ort_root"
fi

[[ -f "${ort_root}/include/onnxruntime_cxx_api.h" \
    && -e "${ort_root}/lib/libonnxruntime.so" ]] || {
    echo "ONNX Runtime verification failed under ${ort_root}" >&2
    exit 1
}

echo "Verified Linux native dependencies under ${tool_root}"
echo "  ONNX_RUNTIME_ROOT=${ort_root}"
