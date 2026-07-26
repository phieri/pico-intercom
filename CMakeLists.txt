cmake_minimum_required(VERSION 3.21)

option(PICO_INTERCOM_FORCE_HOST_BUILD "Build a host-side demo instead of the Pico firmware" OFF)
option(PICO_INTERCOM_BUILD_HOST_TESTS "Build host-side routing tests" ON)

if(PICO_INTERCOM_FORCE_HOST_BUILD)
    project(pico_intercom C CXX ASM)
else()
    include(pico_sdk_import.cmake)
    pico_sdk_init()
    project(pico_intercom C CXX ASM)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The Pico SDK uses GNU-style extensions in its runtime sources, so enable them here.
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)

add_library(intercom_core STATIC
    src/intercom.c
    src/bluetooth.c
    src/bluetooth_transport.c
    src/bluetooth_classic.c
    src/pairings.c
)

target_include_directories(intercom_core PUBLIC include)

if(NOT PICO_INTERCOM_FORCE_HOST_BUILD)
    target_compile_definitions(intercom_core PRIVATE PICO_INTERCOM_TARGET=1)
    target_link_libraries(intercom_core PUBLIC pico_stdlib hardware_flash hardware_sync)
endif()

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
        set(PICO_BOARD pico2_w CACHE STRING "Board type" FORCE)
    endif()

    add_executable(pico_intercom src/main.c)
    target_link_libraries(pico_intercom PRIVATE intercom_core pico_stdlib)
    target_compile_definitions(pico_intercom PRIVATE PICO_INTERCOM_TARGET=1)

    pico_enable_stdio_usb(pico_intercom 1)
    pico_enable_stdio_uart(pico_intercom 0)
    pico_add_extra_outputs(pico_intercom)
endif()
