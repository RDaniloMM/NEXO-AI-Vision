#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Public Ubuntu/DGX OS installation entry point.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "${script_dir}/../.." && pwd)"
build_script="${script_dir}/build-dist.sh"
package_path=""
version="0.1.0"
assume_yes=0
no_install=0
with_tensorrt=0
temporary_paths=()

cleanup() {
    local path
    for path in "${temporary_paths[@]}"; do
        [[ -e "$path" ]] && rm -rf -- "$path"
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

usage() {
    cat <<'EOF'
Usage: install.sh [options]

Build and install Nexo AI Vision on compatible Ubuntu/DGX OS x86_64 hosts.

Options:
  -y, --yes             Do not ask for apt confirmation
      --version X.Y.Z   Package version (default: 0.1.0)
      --no-install      Build the .deb but do not install it
      --package PATH    Install an existing .deb without compiling
      --with-tensorrt   Explicitly build with the target TensorRT/CUDA SDKs
  -h, --help            Show this help
EOF
}

die() {
    echo "install.sh: $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -y|--yes) assume_yes=1; shift ;;
        --version)
            [[ $# -ge 2 ]] || die "missing value for --version"
            version="$2"
            shift 2
            ;;
        --no-install) no_install=1; shift ;;
        --package)
            [[ $# -ge 2 ]] || die "missing value for --package"
            package_path="$2"
            shift 2
            ;;
        --with-tensorrt) with_tensorrt=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
done

[[ "$no_install" -eq 0 || -z "$package_path" ]] || die "--no-install cannot be combined with --package"
[[ "$with_tensorrt" -eq 0 || -z "$package_path" ]] || die "--with-tensorrt only applies when building a package"
[[ "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] \
    || die "--version must be Debian-compatible major.minor.patch"

host_arch="$(uname -m)"
[[ "$host_arch" == "x86_64" ]] || die "only x86_64 Linux is supported (detected ${host_arch})"
[[ -r /etc/os-release ]] || die "cannot identify the operating system: /etc/os-release is missing"
# shellcheck disable=SC1091
source /etc/os-release
os_label="${PRETTY_NAME:-${ID:-unknown}}"
id_like=" ${ID:-} ${ID_LIKE:-} "
is_dgx=0
if [[ -f /etc/dgx-release ]] || [[ "$os_label" == *DGX* ]] || [[ "$id_like" == *dgx* ]]; then
    is_dgx=1
fi
if [[ "$is_dgx" -eq 0 && "$id_like" != *" ubuntu "* ]]; then
    die "unsupported operating system: ${os_label}; use Ubuntu 22.04+ or DGX OS"
fi
if [[ "$is_dgx" -eq 0 ]]; then
    ubuntu_major="${VERSION_ID%%.*}"
    [[ "$ubuntu_major" =~ ^[0-9]+$ && "$ubuntu_major" -ge 22 ]] \
        || die "Ubuntu 22.04 or newer is required (detected ${VERSION_ID:-unknown})"
fi

apt_packages=(
    build-essential cmake ninja-build pkg-config dpkg-dev libopencv-dev
    qt6-base-dev git patch unzip curl ca-certificates
)
if [[ "$EUID" -eq 0 ]]; then
    if command -v sudo >/dev/null 2>&1; then
        sudo_cmd=(sudo)
    else
        echo "Running as root; sudo is unavailable, so apt commands will run directly." >&2
        sudo_cmd=()
    fi
else
    command -v sudo >/dev/null 2>&1 || die "sudo is required; rerun with sudo or install sudo"
    sudo_cmd=(sudo)
fi

echo "Detected compatible host: ${os_label} (${host_arch})"
echo "The installer will use sudo for these apt packages: ${apt_packages[*]}"
if [[ "$assume_yes" -eq 0 ]]; then
    read -r -p "Continue? [y/N] " answer
    [[ "$answer" =~ ^[Yy]([Ee][Ss])?$ ]] || { echo "Installation cancelled."; exit 0; }
fi

"${sudo_cmd[@]}" apt-get update
"${sudo_cmd[@]}" apt-get install -y "${apt_packages[@]}"

show_gpu_status() {
    local has_nvidia_smi=0 has_libcuda=0
    command -v nvidia-smi >/dev/null 2>&1 && has_nvidia_smi=1
    if command -v ldconfig >/dev/null 2>&1 \
        && ldconfig -p 2>/dev/null | grep -q 'libcuda\.so\.1'; then
        has_libcuda=1
    fi
    if [[ "$has_nvidia_smi" -eq 1 || "$has_libcuda" -eq 1 ]]; then
        echo "GPU status: NVIDIA driver signal detected; no driver changes will be made."
    else
        echo "GPU status: nvidia-smi/libcuda.so.1 not detected; CPU mode remains available."
        echo "GPU drivers are not installed or replaced by this script."
    fi
}
show_gpu_status

if [[ -n "$package_path" ]]; then
    [[ -f "$package_path" ]] || die "package does not exist: $package_path"
    command -v dpkg-deb >/dev/null 2>&1 || die "dpkg-deb is required to inspect an existing package"
    package_path="$(readlink -f "$package_path")"
    package_version="$(dpkg-deb -W -f "$package_path" Version)" \
        || die "not a readable Debian package: $package_path"
    echo "Installing existing package: ${package_path} (version ${package_version})"
else
    build_args=(--with-qt --version "$version")
    [[ "$with_tensorrt" -eq 1 ]] && build_args+=(--with-tensorrt)
    bash "$build_script" "${build_args[@]}"
    package_path="${project_root}/.tools/native/dist/nexoai-vision-${version}-linux-x86_64.deb"
    [[ -f "$package_path" ]] || die "build completed without the expected package: $package_path"
    package_version="$version"
    echo "Built package: ${package_path} (version ${package_version})"
fi

if [[ "$no_install" -eq 1 ]]; then
    echo "Build-only mode: package left at ${package_path} (version ${package_version})"
    exit 0
fi

"${sudo_cmd[@]}" apt-get install -y "$package_path"
launcher="/opt/nexoai-vision/bin/NexoAIVisionLauncher"
command_alias="/usr/local/bin/nexoai-vision"
if [[ -x "$launcher" ]]; then
    if [[ -e "$command_alias" && ! -L "$command_alias" ]]; then
        echo "Notice: ${command_alias} exists and is not a symlink; leaving it unchanged." >&2
    else
        "${sudo_cmd[@]}" ln -sfn -- "$launcher" "$command_alias"
        echo "Created command: ${command_alias} -> ${launcher}"
    fi
else
    echo "Notice: launcher not found at ${launcher}; desktop entry was left as packaged." >&2
fi

echo "Installed Nexo AI Vision ${package_version} from ${package_path}"
echo "Start it from the applications menu or run: nexoai-vision"
if [[ "$with_tensorrt" -eq 1 ]]; then
    echo "TensorRT was explicitly requested; host CUDA/TensorRT paths were used without installing drivers."
fi
