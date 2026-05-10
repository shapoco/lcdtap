cmake_minimum_required(VERSION 3.20)

# Raspberry Pi Pico SDK — set PICO_SDK_PATH before calling this
# e.g. cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(spilcd2dvi_pico2 VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# Core library (MCU-agnostic)
add_subdirectory(../../lib lib)

# PicoDVI libdvi (INTERFACE library)
add_subdirectory(
    ../../submodule/PicoDVI/software/libdvi
    libdvi
)

add_executable(spilcd2dvi_pico2
    src/main.cpp
)

# Generate the PIO header from par_slave.pio
pico_generate_pio_header(spilcd2dvi_pico2
    ${CMAKE_CURRENT_LIST_DIR}/src/par_slave.pio
)

target_include_directories(spilcd2dvi_pico2 PRIVATE
    include
    # PicoDVI pin-config header lives here
    ../../submodule/PicoDVI/software/include
)

# DVI_DEFAULT_PIO_INST must be pio0 so libdvi and pico_sock_cfg agree.
target_compile_definitions(spilcd2dvi_pico2 PRIVATE
    DVI_DEFAULT_PIO_INST=pio0
)

target_link_libraries(spilcd2dvi_pico2
    spilcd2dvi
    libdvi
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_irq
    hardware_pio
    hardware_vreg
)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(spilcd2dvi_pico2)
