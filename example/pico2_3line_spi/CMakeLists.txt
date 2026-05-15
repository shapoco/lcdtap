cmake_minimum_required(VERSION 3.20)

# Raspberry Pi Pico SDK — set PICO_SDK_PATH before calling this
# e.g. cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(lcdtap_pico2_3line_spi VERSION 0.1.0 LANGUAGES C CXX ASM)

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

add_executable(lcdtap_pico2_3line_spi
    src/main.cpp
)

# Generate PIO header
pico_generate_pio_header(lcdtap_pico2_3line_spi
    ${CMAKE_CURRENT_LIST_DIR}/src/3line_spi_slave.pio
)

target_include_directories(lcdtap_pico2_3line_spi PRIVATE
    include
    # PicoDVI pin-config header lives here
    ../../submodule/PicoDVI/software/include
)

# DVI_DEFAULT_PIO_INST must be pio0 so libdvi and pico_sock_cfg agree.
target_compile_definitions(lcdtap_pico2_3line_spi PRIVATE
    DVI_DEFAULT_PIO_INST=pio0
)

# Forward framebuffer-size overrides only when explicitly passed via -D.
# Defaults are defined in config.h via #ifndef/#define.
foreach(VAR
        LCDTAP_LCD_SIZE_W LCDTAP_LCD_SIZE_H
        LCDTAP_LCD_SIZE_ALT_W LCDTAP_LCD_SIZE_ALT_H)
    if(DEFINED ${VAR})
        target_compile_definitions(lcdtap_pico2_3line_spi PRIVATE "${VAR}=${${VAR}}")
    endif()
endforeach()

target_link_libraries(lcdtap_pico2_3line_spi
    lcdtap
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
pico_add_extra_outputs(lcdtap_pico2_3line_spi)
