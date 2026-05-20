# linux_app 构建脚本：按功能目录编译大核协议转换应用及其公共依赖。
cmake_minimum_required(VERSION 3.16)

project(linux_app C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(common_crc STATIC
    ../common/src/crc16.c
)
target_include_directories(common_crc PUBLIC
    ../common/include
)

add_executable(linux_app
    main.c
    core/frame_packer.c
    core/protocol_manager.c
    ipc/ipc_to_rtos.c
    以太网/ethernet_udp.c
)

target_include_directories(linux_app PRIVATE
    core
    ipc
    以太网
    ../common/include
)

target_link_libraries(linux_app PRIVATE
    common_crc
)

target_compile_definitions(linux_app PRIVATE
    _POSIX_C_SOURCE=200809L
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(linux_app PRIVATE -Wall -Wextra)
    target_compile_options(common_crc PRIVATE -Wall -Wextra)
endif()
