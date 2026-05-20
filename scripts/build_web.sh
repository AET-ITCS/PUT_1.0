#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${WEB_TARGET:-riscv64gc-unknown-linux-musl}"

npm --prefix "$repo_root/web/frontend" ci
npm --prefix "$repo_root/web/frontend" run build
cargo test --manifest-path "$repo_root/web/backend/Cargo.toml"
cargo build --manifest-path "$repo_root/web/backend/Cargo.toml" --release --target "$target"
