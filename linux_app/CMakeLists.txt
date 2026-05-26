# linux_app 构建脚本：按功能目录编译大核协议转换应用及其公共依赖。
cmake_minimum_required(VERSION 3.16)

project(linux_app C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

option(LINUX_APP_BUILD_TESTS "Build linux_app host tests" ON)

add_library(common_crc STATIC
    ../common/src/crc16.c
)
target_include_directories(common_crc PUBLIC
    ../common/include
)

add_library(linux_shm_ipc STATIC
    ipc/linux_shm_ipc.c
    ipc/linux_shm_platform.c
)
target_include_directories(linux_shm_ipc PUBLIC
    ipc
    ../common/include
)
target_link_libraries(linux_shm_ipc PUBLIC
    common_crc
)
target_compile_definitions(linux_shm_ipc PRIVATE
    _POSIX_C_SOURCE=200809L
)

add_executable(linux_app
    main.c
    core/app_config.c
)

target_include_directories(linux_app PRIVATE
    core
    ../common/include
)

target_link_libraries(linux_app PRIVATE
    common_crc
    Threads::Threads
)

target_compile_definitions(linux_app PRIVATE
    _POSIX_C_SOURCE=200809L
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(linux_app PRIVATE -Wall -Wextra)
    target_compile_options(common_crc PRIVATE -Wall -Wextra)
    target_compile_options(linux_shm_ipc PRIVATE -Wall -Wextra)
endif()

if(LINUX_APP_BUILD_TESTS)
    enable_testing()

    add_executable(linux_shm_ipc_test
        test/linux_shm_ipc_test.c
    )
    target_link_libraries(linux_shm_ipc_test PRIVATE
        linux_shm_ipc
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(linux_shm_ipc_test PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME linux_shm_ipc_test COMMAND linux_shm_ipc_test)
endif()
