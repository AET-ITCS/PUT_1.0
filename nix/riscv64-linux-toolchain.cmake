# CMake toolchain file for RISC-V 64-bit Linux cross-compilation
# Target: Milk-V Duo 256M (Sophgo SG2002 - C906 RISC-V core)
#
# Usage:
#   cmake -B build_linux -S linux_app \
#         -DCMAKE_TOOLCHAIN_FILE=../nix/riscv64-linux-toolchain.cmake
#
# Prefer the compiler path exported by the Nix development shell. If it is not
# present, fall back to a RISC-V Linux compiler available on PATH.

# Target system
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

# Cross-compiler executables
set(RISCV64_LINUX_CC_PATH "")

if(DEFINED ENV{RISCV64_LINUX_CC} AND NOT "$ENV{RISCV64_LINUX_CC}" STREQUAL "")
    set(RISCV64_LINUX_CC_PATH "$ENV{RISCV64_LINUX_CC}")
else()
    find_program(RISCV64_LINUX_CC_PATH
        NAMES
            riscv64-unknown-linux-gnu-gcc
            riscv64-linux-gnu-gcc
    )
endif()

if(NOT RISCV64_LINUX_CC_PATH)
    message(FATAL_ERROR "RISCV64_LINUX_CC is not set and no RISC-V Linux gcc was found on PATH. Use 'nix develop' or install riscv64-unknown-linux-gnu-gcc.")
endif()

get_filename_component(TOOLCHAIN_DIR "${RISCV64_LINUX_CC_PATH}" DIRECTORY)
get_filename_component(TOOLCHAIN_PREFIX "${RISCV64_LINUX_CC_PATH}" NAME)
string(REGEX REPLACE "gcc$" "" TOOLCHAIN_PREFIX "${TOOLCHAIN_PREFIX}")

set(CMAKE_C_COMPILER    "${RISCV64_LINUX_CC_PATH}")
set(CMAKE_CXX_COMPILER  "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}g++")
set(CMAKE_ASM_COMPILER  "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_LINKER        "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}ld")
set(CMAKE_AR            "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}ar")
set(CMAKE_RANLIB        "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}ranlib")
set(CMAKE_OBJCOPY       "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}objcopy")
set(CMAKE_OBJDUMP       "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}objdump")
set(CMAKE_STRIP         "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}strip")
set(CMAKE_SIZE          "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}size")

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# For libraries and headers, only search the target sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
