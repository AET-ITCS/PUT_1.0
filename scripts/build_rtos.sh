#!/usr/bin/env bash
# 构建 rtos_firmware 小核固件骨架。
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

: "${CCACHE_DISABLE:=1}"
export CCACHE_DISABLE

cmake -S "$repo_root/rtos_firmware" -B "$repo_root/build/rtos_firmware" "$@"
cmake --build "$repo_root/build/rtos_firmware"
