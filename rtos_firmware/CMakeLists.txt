cmake_minimum_required(VERSION 3.16)

project(rtos_firmware C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RTOS_FIRMWARE_BUILD_TESTS "Build rtos_firmware host tests" ON)
option(RTOS_FIRMWARE_BUILD_EXECUTABLE "Build rtos_firmware smoke executable" ON)

set(RTOS_FIRMWARE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(rtos_firmware_ipc STATIC
    "${PROJECT_ROOT}/common/src/crc16.c"
    ipc/rtos_shm_ipc.c
    ipc/rtos_shm_platform.c
)

target_include_directories(rtos_firmware_ipc PUBLIC
    include
    ipc
    "${PROJECT_ROOT}/common/include"
)

add_library(rtos_firmware_core STATIC
    src/rtos_entry.c
    bsp/board.c
    bsp/clock.c
    bsp/pinmux.c
    bsp/interrupt.c
    can/rtos_can.c
    mailbox/rtos_mailbox_event.c
    mailbox/rtos_mailbox_isr.c
    mailbox/rtos_mailbox_port.c
    monitor/rtos_endpoint_heartbeat.c
    monitor/rtos_monitor.c
    queue/rtos_priority_queue.c
    router/rtos_router_adapter.c
    router/rtos_router_core.c
    router/rtos_router_policy.c
    router/rtos_router_table.c
    tasks/rtos_task_placeholders.c
    watchdog/rtos_watchdog.c
)

target_include_directories(rtos_firmware_core PUBLIC
    include
    bsp
    can
    mailbox
    monitor
    queue
    router
    tasks
    watchdog
    "${PROJECT_ROOT}/common/include"
)

target_link_libraries(rtos_firmware_core PUBLIC rtos_firmware_ipc)

if(RTOS_FIRMWARE_BUILD_EXECUTABLE)
    add_executable(rtos_firmware main.c)
    target_link_libraries(rtos_firmware PRIVATE rtos_firmware_core)
endif()

if(RTOS_FIRMWARE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()

    add_executable(rtos_shm_ipc_test
        test/rtos_shm_ipc_test.c
    )

    target_link_libraries(rtos_shm_ipc_test PRIVATE rtos_firmware_ipc)
    add_test(NAME rtos_shm_ipc COMMAND rtos_shm_ipc_test)

    add_executable(rtos_priority_queue_test
        test/rtos_priority_queue_test.c
    )

    target_link_libraries(rtos_priority_queue_test PRIVATE rtos_firmware_core)
    add_test(NAME rtos_priority_queue COMMAND rtos_priority_queue_test)

    add_executable(rtos_router_core_test
        test/rtos_router_core_test.c
    )

    target_link_libraries(rtos_router_core_test PRIVATE rtos_firmware_core)
    add_test(NAME rtos_router_core COMMAND rtos_router_core_test)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtos_firmware_ipc PRIVATE -Wall -Wextra)
    target_compile_options(rtos_firmware_core PRIVATE -Wall -Wextra)

    if(TARGET rtos_firmware)
        target_compile_options(rtos_firmware PRIVATE -Wall -Wextra)
    endif()

    if(TARGET rtos_shm_ipc_test)
        target_compile_options(rtos_shm_ipc_test PRIVATE -Wall -Wextra)
    endif()

    if(TARGET rtos_priority_queue_test)
        target_compile_options(rtos_priority_queue_test PRIVATE -Wall -Wextra)
    endif()

    if(TARGET rtos_router_core_test)
        target_compile_options(rtos_router_core_test PRIVATE -Wall -Wextra)
    endif()
endif()
