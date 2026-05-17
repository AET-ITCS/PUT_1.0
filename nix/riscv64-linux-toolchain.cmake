# CMake toolchain file for RISC-V 64-bit Linux cross-compilation
# Target: Milk-V Duo 256M (Sophgo SG2002 - C906 RISC-V core)
#
# Usage:
#   cmake -B build_linux -S linux_app \
#         -DCMAKE_TOOLCHAIN_FILE=../nix/riscv64-linux-toolchain.cmake
#
# This file expects the Nix development environment to be active,
# which exports $RISCV64_LINUX_CC pointing to the cross-compiler.

# Target system
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

# Cross-compiler executables
if(DEFINED ENV{RISCV64_LINUX_CC})
    get_filename_component(TOOLCHAIN_DIR $ENV{RISCV64_LINUX_CC} DIRECTORY)

    set(CMAKE_C_COMPILER    "$ENV{RISCV64_LINUX_CC}")
    set(CMAKE_CXX_COMPILER  "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-g++")
    set(CMAKE_ASM_COMPILER  "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-gcc")
    set(CMAKE_LINKER        "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-ld")
    set(CMAKE_AR            "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-ar")
    set(CMAKE_RANLIB        "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-ranlib")
    set(CMAKE_OBJCOPY       "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-objcopy")
    set(CMAKE_OBJDUMP       "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-objdump")
    set(CMAKE_STRIP         "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-strip")
    set(CMAKE_SIZE          "${TOOLCHAIN_DIR}/riscv64-unknown-linux-gnu-size")
else()
    message(FATAL_ERROR "RISCV64_LINUX_CC is not set! Use 'nix develop' to enter the Nix dev shell.")
endif()

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# For libraries and headers, only search the target sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
