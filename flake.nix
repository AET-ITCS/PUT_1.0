{
  description = "Milk-V Duo 256M 多协议统一终端 (Multi-Protocol Unified Terminal)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config = {
            allowUnsupportedSystem = true;
          };
        };

        # RISC-V 64-bit cross-compilation for Milk-V Duo big core (Linux userspace)
        pkgsCrossRiscV64 = import nixpkgs {
          inherit system;
          crossSystem = nixpkgs.lib.systems.examples.riscv64;
        };

        # RISC-V 64-bit bare-metal/embedded toolchain for RTOS firmware
        pkgsCrossRiscV64Embedded = import nixpkgs {
          inherit system;
          crossSystem = nixpkgs.lib.systems.examples.riscv64-embedded;
        };

        # ARM cross-compilation (alternative big core target)
        pkgsCrossAarch64 = import nixpkgs {
          inherit system;
          crossSystem = nixpkgs.lib.systems.examples.aarch64-multiplatform;
        };

        # ARM 32-bit cross-compilation
        pkgsCrossArmHF = import nixpkgs {
          inherit system;
          crossSystem = nixpkgs.lib.systems.examples.armv7l-hf-multiplatform;
        };

        # Python environment with tools dependencies
        pythonEnv = pkgs.python3.withPackages (ps: with ps; [
          pyserial
          pyyaml
          pytest
        ]);

        # ── Rust toolchain ───────────────────────────────────────────
        # Use rustup from nixpkgs to manage Rust versions & targets
        rustPkgs = with pkgs; [
          rustup       # Rust toolchain manager (supports `rustup target add`)
          cargo        # Rust package manager
          rustc        # Rust compiler (native)
          rust-analyzer # LSP server
          clippy       # Rust linter
          rustfmt      # Rust code formatter
        ];

        # ── Rust cross-compilation target triples ─────────────────────
        #   riscv64gc-unknown-linux-gnu    → 大核 Linux app
        #   riscv64gc-unknown-none-elf     → 小核 RTOS firmware (bare-metal)
        #   aarch64-unknown-linux-gnu      → 备选: ARM64 Linux
        #   aarch64-unknown-none-elf       → 备选: ARM64 bare-metal
        rustCrossTargets = [
          "riscv64gc-unknown-linux-gnu"
          "riscv64gc-unknown-none-elf"
          "aarch64-unknown-linux-gnu"
          "aarch64-unknown-none-elf"
        ];

        # Paths to Rust cross linkers
        riscv64LinuxLinker = "${pkgsCrossRiscV64.buildPackages.gcc}/bin/${pkgsCrossRiscV64.stdenv.cc.targetPrefix}gcc";
        riscv64ElfLinker   = "${pkgsCrossRiscV64Embedded.buildPackages.gcc}/bin/${pkgsCrossRiscV64Embedded.stdenv.cc.targetPrefix}gcc";
        aarch64Linker      = "${pkgsCrossAarch64.buildPackages.gcc}/bin/${pkgsCrossAarch64.stdenv.cc.targetPrefix}gcc";
      in
      {
        # ============================================================
        # Development Shells
        # ============================================================

        devShells = {
          # ── Main development shell ──
          default = pkgs.mkShell {
            buildInputs = with pkgs; [
              # ---- Build essentials ----
              cmake
              gnumake
              pkg-config
              gcc
              gdb

              # ---- RISC-V cross toolchain (big core Linux target) ----
              pkgsCrossRiscV64.buildPackages.gcc
              pkgsCrossRiscV64.buildPackages.binutils
              pkgsCrossRiscV64.buildPackages.gdb

              # ---- RISC-V bare-metal toolchain (small core RTOS target) ----
              pkgsCrossRiscV64Embedded.buildPackages.gcc
              pkgsCrossRiscV64Embedded.buildPackages.binutils

              # ---- ARM cross toolchain (alternative big core target) ----
              pkgsCrossAarch64.buildPackages.gcc
              pkgsCrossAarch64.buildPackages.binutils

              # ---- C51 / 8051 compiler ----
              sdcc

              # ---- Rust toolchain (native + cross) ----
            ] ++ rustPkgs ++ [

              # ---- Python for tools ----
              pythonEnv

              # ---- Code analysis & formatting ----
              clang-tools
              cppcheck
              cmake-format
              shellcheck

              # ---- Debug & analysis ----
              lttng-ust
              ltrace
              strace

              # ---- Version control ----
              git

              # ---- Misc utilities ----
              ripgrep
              fd
              jq
              file
              xxd
              hexdump
              unzip
              wget
              curl
              which
              tree
            ];

            # ── Set up Rust cross-compilation targets & Cargo config ──
            shellHook = ''
              echo ""
              echo "╔══════════════════════════════════════════════════════════╗"
              echo "║  Milk-V Duo 256M 多协议统一终端 开发环境              ║"
              echo "║  Multi-Protocol Unified Terminal Dev Environment        ║"
              echo "╚══════════════════════════════════════════════════════════╝"
              echo ""

              # ── Repository layout reminder ──
              echo "  项目目录结构:"
              echo "    Linux App   : ./linux_app/     (大核 - RISC-V)"
              echo "    RTOS Firmware: ./rtos_firmware/ (小核 - RISC-V bare-metal)"
              echo "    C51 Low Power: ./c51_low_power/ (C51 单片机)"
              echo "    Common      : ./common/         (公共代码)"
              echo "    Tools       : ./tools/          (Python 工具)"
              echo ""

              # ── C/C++ toolchain aliases ──
              echo "  C/C++ 工具链快捷方式:"
              echo "    riscv64-linux-gcc     - RISC-V Linux 交叉编译器 (大核)"
              echo "    riscv64-elf-gcc       - RISC-V 裸机交叉编译器 (小核)"
              echo "    aarch64-linux-gcc     - ARM64 交叉编译器 (备选)"
              echo "    sdcc                  - C51 单片机编译器"
              echo ""

              # ── Rust toolchain info ──
              echo "  Rust 工具链:"
              echo "    rustc: $(rustc --version 2>/dev/null || echo 'not configured')"
              echo "    cargo: $(cargo --version 2>/dev/null || echo 'not configured')"
              echo ""

              # ── Set up Rust cross-compilation targets ──
              echo "  正在配置 Rust 交叉编译目标..."

              # Initialize rustup if needed (first time setup)
              export RUSTUP_HOME="$HOME/.rustup"
              export CARGO_HOME="$HOME/.cargo"

              # Ensure stable toolchain is installed
              rustup toolchain install stable 2>/dev/null || true

              # Install cross-compilation targets
              for target in ${builtins.toString rustCrossTargets}; do
                if rustup target add "$target" --toolchain stable 2>/dev/null; then
                  echo "    ✓ $target"
                else
                  echo "    ✗ $target (install failed)"
                fi
              done

              # ── Write Cargo cross-compilation config ──
              mkdir -p .cargo
              cat > .cargo/config.toml << 'CARGO_EOF'
# ─────────────────────────────────────────────────────────────────────
# Cargo 交叉编译配置 — Multi-Protocol Unified Terminal
# ─────────────────────────────────────────────────────────────────────
# 由 flake.nix shellHook 自动生成。其他配置项也可手工添加到此文件。
# ─────────────────────────────────────────────────────────────────────

[target.riscv64gc-unknown-linux-gnu]
linker = "${riscv64LinuxLinker}"

[target.riscv64gc-unknown-none-elf]
linker = "${riscv64ElfLinker}"

[target.aarch64-unknown-linux-gnu]
linker = "${aarch64Linker}"

[target.aarch64-unknown-none-elf]
linker = "${aarch64Linker}"

# Aliases for shorter names
[alias]
build-riscv64-linux  = "build --target riscv64gc-unknown-linux-gnu"
build-riscv64-elf    = "build --target riscv64gc-unknown-none-elf"
build-aarch64-linux  = "build --target aarch64-unknown-linux-gnu"
build-aarch64-elf    = "build --target aarch64-unknown-none-elf"
check-riscv64-linux  = "check --target riscv64gc-unknown-linux-gnu"
check-riscv64-elf    = "check --target riscv64gc-unknown-none-elf"
check-aarch64-linux  = "check --target aarch64-unknown-linux-gnu"
check-aarch64-elf    = "check --target aarch64-unknown-none-elf"
CARGO_EOF
              echo "    ✓ .cargo/config.toml 已生成 (含交叉编译 linker 配置)"
              echo ""

              # ── Export toolchain prefixes ──
              export RISCV64_LINUX_CROSS="${pkgsCrossRiscV64.stdenv.cc.targetPrefix}"
              export RISCV64_ELF_CROSS="${pkgsCrossRiscV64Embedded.stdenv.cc.targetPrefix}"
              export AARCH64_LINUX_CROSS="${pkgsCrossAarch64.stdenv.cc.targetPrefix}"
              export ARMHF_LINUX_CROSS="${pkgsCrossArmHF.stdenv.cc.targetPrefix}"

              export RISCV64_LINUX_CC="${pkgsCrossRiscV64.buildPackages.gcc}/bin/${pkgsCrossRiscV64.stdenv.cc.targetPrefix}gcc"
              export RISCV64_ELF_CC="${pkgsCrossRiscV64Embedded.buildPackages.gcc}/bin/${pkgsCrossRiscV64Embedded.stdenv.cc.targetPrefix}gcc"
              export AARCH64_LINUX_CC="${pkgsCrossAarch64.buildPackages.gcc}/bin/${pkgsCrossAarch64.stdenv.cc.targetPrefix}gcc"

              # ── Export Rust cross-compilation variables ──
              export CARGO_HOME="$HOME/.cargo"
              export RUSTUP_HOME="$HOME/.rustup"
              export RUST_TARGET_RISCV64_LINUX="riscv64gc-unknown-linux-gnu"
              export RUST_TARGET_RISCV64_ELF="riscv64gc-unknown-none-elf"
              export RUST_TARGET_AARCH64_LINUX="aarch64-unknown-linux-gnu"
              export RUST_TARGET_AARCH64_ELF="aarch64-unknown-none-elf"

              # ── CMake toolchain file path hints ──
              echo "  导出变量 (可在 CMake / Cargo 中使用):"
              echo "    \$RISCV64_LINUX_CC  = ${pkgsCrossRiscV64.buildPackages.gcc}/bin/${pkgsCrossRiscV64.stdenv.cc.targetPrefix}gcc"
              echo "    \$RISCV64_ELF_CC    = ${pkgsCrossRiscV64Embedded.buildPackages.gcc}/bin/${pkgsCrossRiscV64Embedded.stdenv.cc.targetPrefix}gcc"
              echo "    \$AARCH64_LINUX_CC  = ${pkgsCrossAarch64.buildPackages.gcc}/bin/${pkgsCrossAarch64.stdenv.cc.targetPrefix}gcc"
              echo "    \$RUST_TARGET_RISCV64_LINUX = $RUST_TARGET_RISCV64_LINUX"
              echo "    \$RUST_TARGET_RISCV64_ELF   = $RUST_TARGET_RISCV64_ELF"
              echo ""

              # ── Python tools path ──
              export PYTHON_TOOLS_DIR="$(pwd)/tools"
              echo "  Python 工具目录: \$PYTHON_TOOLS_DIR"
              echo ""

              # ── Build helpers ──
              echo "  常用构建命令:"
              echo "    ── C/C++ ──"
              echo "    cmake -B build_linux -S linux_app                                           - 配置大核 Linux 项目"
              echo "    cmake -B build_rtos  -S rtos_firmware                                       - 配置小核 RTOS 项目"
              echo "    cmake -B build_c51   -S c51_low_power                                       - 配置 C51 项目"
              echo "    ── Rust ──"
              echo "    cargo build                                                                  - 编译 (native)"
              echo "    cargo build-riscv64-linux                                                    - 编译 (RISC-V Linux)"
              echo "    cargo build-riscv64-elf                                                      - 编译 (RISC-V bare-metal)"
              echo "    cargo build-aarch64-linux                                                    - 编译 (ARM64 Linux)"
              echo "    cargo check-riscv64-linux                                                    - 检查 (RISC-V Linux)"
              echo ""

              # ── Verify all toolchains ──
              echo "  检查关键工具链..."
              echo "  ── 基础工具 ──"
              for cmd in cmake make git; do
                if command -v "$cmd" &>/dev/null; then
                  echo "    ✓ $cmd: $(command -v $cmd)"
                else
                  echo "    ✗ $cmd: NOT FOUND"
                fi
              done

              echo "  ── C/C++ 编译器 ──"
              if command -v "${pkgsCrossRiscV64.stdenv.cc.targetPrefix}gcc" &>/dev/null; then
                echo "    ✓ riscv64-linux-gcc: $(${pkgsCrossRiscV64.stdenv.cc.targetPrefix}gcc --version | head -n1)"
              else
                echo "    ✗ riscv64-linux-gcc: NOT FOUND"
              fi
              if command -v "${pkgsCrossRiscV64Embedded.stdenv.cc.targetPrefix}gcc" &>/dev/null; then
                echo "    ✓ riscv64-elf-gcc: $(${pkgsCrossRiscV64Embedded.stdenv.cc.targetPrefix}gcc --version | head -n1)"
              else
                echo "    ✗ riscv64-elf-gcc: NOT FOUND"
              fi
              if command -v sdcc &>/dev/null; then
                echo "    ✓ sdcc: $(sdcc --version | head -n1)"
              else
                echo "    ✗ sdcc: NOT FOUND"
              fi

              echo "  ── Rust ──"
              if command -v rustc &>/dev/null; then
                echo "    ✓ rustc: $(rustc --version)"
              else
                echo "    ✗ rustc: NOT FOUND"
              fi
              if command -v cargo &>/dev/null; then
                echo "    ✓ cargo: $(cargo --version)"
              else
                echo "    ✗ cargo: NOT FOUND"
              fi

              echo "  ── Rust Cross Targets ──"
              for target in ${builtins.toString rustCrossTargets}; do
                if rustc --print target-list 2>/dev/null | grep -q "$target"; then
                  echo "    ✓ $target (available)"
                else
                  # Check by trying to get target spec
                  if rustc +stable --target "$target" --print target-spec-json 2>/dev/null > /dev/null; then
                    echo "    ✓ $target (available)"
                  else
                    echo "    ✗ $target (not installed, run: rustup target add $target)"
                  fi
                fi
              done

              echo ""
              echo "  环境就绪！Happy Hacking 🚀"
              echo ""
            '';
          };

          # ── Minimal shell (faster to load) ──
          minimal = pkgs.mkShell {
            buildInputs = with pkgs; [
              cmake
              gnumake
              gcc
              git
              pkgsCrossRiscV64.buildPackages.gcc
              pkgsCrossRiscV64Embedded.buildPackages.gcc
              sdcc
              rustup
              cargo
              rustc
            ];

            shellHook = ''
              echo "Milk-V Duo 256M 最小开发环境已激活"
              echo "  * Native:  gcc $(gcc --version | head -n1 | grep -oP '\d+\.\d+\.\d+')"
              echo "  * RISC-V Linux: ${pkgsCrossRiscV64.buildPackages.gcc}/bin/${pkgsCrossRiscV64.stdenv.cc.targetPrefix}gcc"
              echo "  * RISC-V ELF:   ${pkgsCrossRiscV64Embedded.buildPackages.gcc}/bin/${pkgsCrossRiscV64Embedded.stdenv.cc.targetPrefix}gcc"
              echo "  * C51/SDCC: sdcc $(sdcc --version | head -n1)"
              echo "  * Rust:    $(rustc --version 2>/dev/null || echo 'run `rustup toolchain install stable`')"
              echo ""
              echo "  Rust 交叉编译目标安装:"
              echo "    rustup target add riscv64gc-unknown-linux-gnu"
              echo "    rustup target add riscv64gc-unknown-none-elf"
            '';
          };
        };

        # ============================================================
        # Packages - Build individual components
        # ============================================================

        packages = {
          # ── Cross-compilation environment for the RISC-V big core ──
          riscv64-linux-toolchain = pkgsCrossRiscV64.buildPackages.gcc;
          riscv64-elf-toolchain = pkgsCrossRiscV64Embedded.buildPackages.gcc;
          aarch64-linux-toolchain = pkgsCrossAarch64.buildPackages.gcc;

          # ── toolchain wrapper providing all cross compilers ──
          milkv-toolchain = pkgs.buildEnv {
            name = "milkv-toolchain";
            paths = with pkgs; [
              pkgsCrossRiscV64.buildPackages.gcc
              pkgsCrossRiscV64.buildPackages.binutils
              pkgsCrossRiscV64Embedded.buildPackages.gcc
              pkgsCrossRiscV64Embedded.buildPackages.binutils
              sdcc
              cmake
              gnumake
              gcc
              rustup
              cargo
              rustc
            ];
          };
        };

        # ============================================================
        # Legacy shell.nix compatibility
        # ============================================================

        # `nix build .#` will build the default package (or use the default shell)
        defaultPackage = self.packages.${system}.milkv-toolchain;
      });
}
