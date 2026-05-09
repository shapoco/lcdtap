cmake_minimum_required(VERSION 3.20)

# Raspberry Pi Pico SDK — set PICO_SDK_PATH before calling this
# e.g. cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(spilcd2dvi_pico2 VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Core library
add_subdirectory(../../lib lib)

add_executable(spilcd2dvi_pico2
    src/main.cpp
)

target_link_libraries(spilcd2dvi_pico2
    spilcd2dvi
    pico_stdlib
    # TODO: add more Pico SDK libraries as needed
)

target_include_directories(spilcd2dvi_pico2 PRIVATE
    include
)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(spilcd2dvi_pico2)
