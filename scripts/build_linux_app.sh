#!/usr/bin/env bash
# 构建 linux_app 大核协议转换应用。
set -euo pipefail

cmake -S linux_app -B build/linux_app "$@"
cmake --build build/linux_app
