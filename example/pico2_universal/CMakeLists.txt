cmake_minimum_required(VERSION 3.20)

# Raspberry Pi Pico SDK — set PICO_SDK_PATH before calling this
# e.g. cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(lcdtap_pico2_universal VERSION 0.1.0 LANGUAGES C CXX ASM)

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

add_executable(lcdtap_pico2_universal
    src/main.cpp
    src/flash_config.cpp
)

# Generate PIO headers
pico_generate_pio_header(lcdtap_pico2_universal
    ${CMAKE_CURRENT_LIST_DIR}/src/spi_4line_mode0.pio
)
pico_generate_pio_header(lcdtap_pico2_universal
    ${CMAKE_CURRENT_LIST_DIR}/src/spi_3line_mode0.pio
)
pico_generate_pio_header(lcdtap_pico2_universal
    ${CMAKE_CURRENT_LIST_DIR}/src/parallel_8bit.pio
)

target_include_directories(lcdtap_pico2_universal PRIVATE
    include
    # PicoDVI pin-config header lives here
    ../../submodule/PicoDVI/software/include
)

# DVI_DEFAULT_PIO_INST must be pio0 so libdvi and pico_sock_cfg agree.
target_compile_definitions(lcdtap_pico2_universal PRIVATE
    DVI_DEFAULT_PIO_INST=pio0
)

# Forward framebuffer-size overrides only when explicitly passed via -D.
# Defaults are defined in config.h via #ifndef/#define.
foreach(VAR
        LCDTAP_LCD_SIZE_W LCDTAP_LCD_SIZE_H)
    if(DEFINED ${VAR})
        target_compile_definitions(lcdtap_pico2_universal PRIVATE "${VAR}=${${VAR}}")
    endif()
endforeach()

target_link_libraries(lcdtap_pico2_universal
    lcdtap
    libdvi
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_flash
    hardware_gpio
    hardware_i2c
    hardware_irq
    hardware_pio
    hardware_sync
    hardware_vreg
)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(lcdtap_pico2_universal)
