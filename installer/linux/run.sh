#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${root_dir}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-${root_dir}/config}"
export XDG_DATA_HOME="${XDG_DATA_HOME:-${root_dir}/data}"
export XDG_STATE_HOME="${XDG_STATE_HOME:-${root_dir}/state}"
export NEXOAI_COMPUTE_MODE="${NEXOAI_COMPUTE_MODE:-auto}"

mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME"
exec "${root_dir}/bin/NexoAIVisionLauncher" "$@"
