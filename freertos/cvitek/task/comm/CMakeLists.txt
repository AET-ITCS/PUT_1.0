cmake_minimum_required(VERSION 3.16)

project(rtos_comm_mock C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(RTOS_COMM_BUILD_TESTS "Build FreeRTOS comm mock tests" ON)

add_library(rtos_comm_mock STATIC
    ../../../../common/src/crc16.c
    src/riscv64/comm_main.c
    src/riscv64/rtos_can_driver.c
    src/riscv64/rtos_can_forward.c
    src/riscv64/rtos_can_task.c
    src/riscv64/rtos_ipc.c
    src/riscv64/rtos_protocol_adapter.c
    src/riscv64/rtos_recovery.c
    src/riscv64/rtos_status.c
)

target_include_directories(rtos_comm_mock PUBLIC
    include
    ../../../../common/include
)

if(RTOS_COMM_BUILD_TESTS)
    enable_testing()

    add_executable(rtos_comm_mock_test
        test/rtos_comm_mock_test.c
    )
    target_link_libraries(rtos_comm_mock_test PRIVATE rtos_comm_mock)
    add_test(NAME rtos_comm_mock COMMAND rtos_comm_mock_test)

    add_executable(rtos_comm_stage4_test
        test/rtos_comm_stage4_test.c
    )
    target_link_libraries(rtos_comm_stage4_test PRIVATE rtos_comm_mock)
    add_test(NAME rtos_comm_stage4 COMMAND rtos_comm_stage4_test)

    add_executable(rtos_comm_stage5_test
        test/rtos_comm_stage5_test.c
    )
    target_link_libraries(rtos_comm_stage5_test PRIVATE rtos_comm_mock)
    add_test(NAME rtos_comm_stage5 COMMAND rtos_comm_stage5_test)

    add_executable(rtos_comm_stage6_test
        ../../../../linux_app/core/frame_packer.c
        test/rtos_comm_stage6_test.c
    )
    target_include_directories(rtos_comm_stage6_test PRIVATE
        include
        ../../../../linux_app/core
        ../../../../common/include
    )
    target_link_libraries(rtos_comm_stage6_test PRIVATE rtos_comm_mock)
    add_test(NAME rtos_comm_stage6 COMMAND rtos_comm_stage6_test)

    add_executable(rtos_xl2515_fake_test
        src/riscv64/rtos_can_driver.c
        test/rtos_xl2515_fake_platform.c
        test/rtos_xl2515_fake_test.c
    )
    target_include_directories(rtos_xl2515_fake_test PRIVATE
        include
        test
        ../../../../common/include
    )
    target_compile_definitions(rtos_xl2515_fake_test PRIVATE
        RTOS_CAN_DRIVER_XL2515_ENABLE=1
        RTOS_CAN_DRIVER_XL2515_FAKE_PLATFORM=1
        RTOS_CAN_LOOPBACK_ENABLE=1
    )
    add_test(NAME rtos_xl2515_fake COMMAND rtos_xl2515_fake_test)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtos_comm_mock PRIVATE -Wall -Wextra)
    if(RTOS_COMM_BUILD_TESTS)
        target_compile_options(rtos_comm_mock_test PRIVATE -Wall -Wextra)
        target_compile_options(rtos_comm_stage4_test PRIVATE -Wall -Wextra)
        target_compile_options(rtos_comm_stage5_test PRIVATE -Wall -Wextra)
        target_compile_options(rtos_comm_stage6_test PRIVATE -Wall -Wextra)
        target_compile_options(rtos_xl2515_fake_test PRIVATE -Wall -Wextra)
    endif()
endif()
