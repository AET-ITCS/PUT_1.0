cmake_minimum_required(VERSION 3.16)

project(rtos_comm_mock C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(RTOS_COMM_BUILD_TESTS "Build FreeRTOS comm mock tests" ON)

add_library(rtos_comm_mock STATIC
    src/riscv64/comm_main.c
    src/riscv64/rtos_can_driver.c
    src/riscv64/rtos_can_forward.c
    src/riscv64/rtos_can_task.c
    src/riscv64/rtos_ipc.c
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
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rtos_comm_mock PRIVATE -Wall -Wextra)
    if(RTOS_COMM_BUILD_TESTS)
        target_compile_options(rtos_comm_mock_test PRIVATE -Wall -Wextra)
    endif()
endif()
