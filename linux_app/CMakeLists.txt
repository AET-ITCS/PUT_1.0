# linux_app 构建脚本：按功能目录编译大核协议转换应用及其公共依赖。
cmake_minimum_required(VERSION 3.16)

project(linux_app C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_library(linux_app_core STATIC
    core/app_config.c
    core/status_collector.c
    egress/egress_manager.c
    egress/egress_worker.c
    adapters/bluetooth_adapter.c
    adapters/can_adapter.c
    adapters/ethernet_adapter.c
    adapters/four_g_adapter.c
    adapters/rs485_adapter.c
    adapters/wifi_adapter.c
)
target_include_directories(linux_app_core PUBLIC
    core
    egress
    adapters
    ipc
    ../common/include
)
target_link_libraries(linux_app_core PUBLIC
    linux_shm_ipc
    Threads::Threads
)
target_compile_definitions(linux_app_core PRIVATE
    _POSIX_C_SOURCE=200809L
)

add_executable(linux_app
    main.c
)

target_link_libraries(linux_app PRIVATE
    linux_app_core
)

target_compile_definitions(linux_app PRIVATE
    _POSIX_C_SOURCE=200809L
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(common_crc PRIVATE -Wall -Wextra)
    target_compile_options(linux_shm_ipc PRIVATE -Wall -Wextra)
    target_compile_options(linux_app_core PRIVATE -Wall -Wextra)
    target_compile_options(linux_app PRIVATE -Wall -Wextra)
endif()

if(LINUX_APP_BUILD_TESTS)
    enable_testing()

    add_executable(linux_shm_ipc_test
        test/linux_shm_ipc_test.c
    )
    target_link_libraries(linux_shm_ipc_test PRIVATE
        linux_shm_ipc
    )

    add_executable(ethernet_adapter_test
        test/ethernet_adapter_test.c
    )
    target_link_libraries(ethernet_adapter_test PRIVATE
        linux_app_core
    )

    add_executable(can_adapter_test
        test/can_adapter_test.c
    )
    target_link_libraries(can_adapter_test PRIVATE
        linux_app_core
    )

    add_executable(wifi_adapter_test
        test/wifi_adapter_test.c
    )
    target_link_libraries(wifi_adapter_test PRIVATE
        linux_app_core
    )

    add_executable(four_g_adapter_test
        test/four_g_adapter_test.c
    )
    target_link_libraries(four_g_adapter_test PRIVATE
        linux_app_core
    )

    add_executable(app_config_test
        test/app_config_test.c
    )
    target_link_libraries(app_config_test PRIVATE
        linux_app_core
    )

    add_executable(status_collector_test
        test/status_collector_test.c
    )
    target_link_libraries(status_collector_test PRIVATE
        linux_app_core
    )

    add_executable(egress_worker_test
        test/egress_worker_test.c
    )
    target_link_libraries(egress_worker_test PRIVATE
        linux_app_core
    )

    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(linux_shm_ipc_test PRIVATE -Wall -Wextra)
        target_compile_options(ethernet_adapter_test PRIVATE -Wall -Wextra)
        target_compile_options(can_adapter_test PRIVATE -Wall -Wextra)
        target_compile_options(wifi_adapter_test PRIVATE -Wall -Wextra)
        target_compile_options(four_g_adapter_test PRIVATE -Wall -Wextra)
        target_compile_options(app_config_test PRIVATE -Wall -Wextra)
        target_compile_options(status_collector_test PRIVATE -Wall -Wextra)
        target_compile_options(egress_worker_test PRIVATE -Wall -Wextra)
    endif()

    add_test(NAME linux_shm_ipc_test COMMAND linux_shm_ipc_test)
    add_test(NAME ethernet_adapter_test COMMAND ethernet_adapter_test)
    add_test(NAME can_adapter_test COMMAND can_adapter_test)
    add_test(NAME wifi_adapter_test COMMAND wifi_adapter_test)
    add_test(NAME four_g_adapter_test COMMAND four_g_adapter_test)
    add_test(NAME app_config_test COMMAND app_config_test)
    add_test(NAME status_collector_test COMMAND status_collector_test)
    add_test(NAME egress_worker_test COMMAND egress_worker_test)
endif()
