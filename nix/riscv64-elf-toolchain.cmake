# CMake toolchain file for RISC-V 64-bit bare-metal cross-compilation
# Target: Milk-V Duo 256M small core RTOS firmware
#
# Usage:
#   cmake -B build_rtos -S rtos_firmware \
#         -DCMAKE_TOOLCHAIN_FILE=../nix/riscv64-elf-toolchain.cmake
#
# This file expects the Nix development environment to be active,
# which exports $RISCV64_ELF_CC pointing to the cross-compiler.

# Target system (bare-metal / no OS)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

# Cross-compiler executables
if(DEFINED ENV{RISCV64_ELF_CC})
    get_filename_component(TOOLCHAIN_DIR "$ENV{RISCV64_ELF_CC}" DIRECTORY)
    get_filename_component(TOOLCHAIN_PREFIX "$ENV{RISCV64_ELF_CC}" NAME)
    string(REGEX REPLACE "gcc$" "" TOOLCHAIN_PREFIX "${TOOLCHAIN_PREFIX}")

    set(CMAKE_C_COMPILER    "$ENV{RISCV64_ELF_CC}")
    set(CMAKE_CXX_COMPILER  "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}g++")
    set(CMAKE_ASM_COMPILER  "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}gcc")
    set(CMAKE_LINKER        "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}ld")
    set(CMAKE_AR            "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}ar")
    set(CMAKE_RANLIB        "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}ranlib")
    set(CMAKE_OBJCOPY       "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}objcopy")
    set(CMAKE_OBJDUMP       "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}objdump")
    set(CMAKE_STRIP         "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}strip")
    set(CMAKE_SIZE          "${TOOLCHAIN_DIR}/${TOOLCHAIN_PREFIX}size")
else()
    message(FATAL_ERROR "RISCV64_ELF_CC is not set! Use 'nix develop' to enter the Nix dev shell.")
endif()

# Search for programs in the build host directories
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# For libraries and headers, only search the target sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Bare-metal flags
set(CMAKE_C_FLAGS_INIT "-march=rv64gc -mabi=lp64d -ffunction-sections -fdata-sections" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_INIT "-march=rv64gc -mabi=lp64d -ffunction-sections -fdata-sections" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,-gc-sections" CACHE STRING "" FORCE)

# No executable prefix for bare-metal
set(CMAKE_EXECUTABLE_SUFFIX ".elf")
