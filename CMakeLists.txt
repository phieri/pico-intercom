cmake_minimum_required(VERSION 3.21)

option(PICO_INTERCOM_FORCE_HOST_BUILD "Build a host-side demo instead of the Pico firmware" OFF)
option(PICO_INTERCOM_BUILD_HOST_TESTS "Build host-side routing tests" ON)

project(pico_intercom C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)

add_library(intercom_core STATIC
    src/intercom.c
    src/bluetooth.c
)

target_include_directories(intercom_core PUBLIC include)

if(PICO_INTERCOM_FORCE_HOST_BUILD)
    add_executable(pico_intercom_app src/main.c)
    target_link_libraries(pico_intercom_app PRIVATE intercom_core)

    if(PICO_INTERCOM_BUILD_HOST_TESTS)
        enable_testing()
        add_executable(intercom_tests tests/test_intercom.c)
        target_link_libraries(intercom_tests PRIVATE intercom_core)
        add_test(NAME intercom_tests COMMAND intercom_tests)
    endif()
else()
    if(NOT DEFINED PICO_BOARD OR PICO_BOARD STREQUAL "")
        set(PICO_BOARD pico_w CACHE STRING "Board type" FORCE)
    endif()

    include(pico_sdk_import.cmake)
    pico_sdk_init()

    add_executable(pico_intercom src/main.c)
    target_link_libraries(pico_intercom PRIVATE intercom_core pico_stdlib)
    target_compile_definitions(pico_intercom PRIVATE PICO_INTERCOM_TARGET=1)

    pico_enable_stdio_usb(pico_intercom 1)
    pico_enable_stdio_uart(pico_intercom 0)
    pico_add_extra_outputs(pico_intercom)
endif()
