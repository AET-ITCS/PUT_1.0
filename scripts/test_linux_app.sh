#!/usr/bin/env bash
# 构建并运行 linux_app 协议转换层单元测试。
set -euo pipefail

cmake -S tests/linux_app_test -B build/linux_app_test "$@"
cmake --build build/linux_app_test
ctest --test-dir build/linux_app_test --output-on-failure
