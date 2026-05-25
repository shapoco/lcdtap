cmake_minimum_required(VERSION 3.20)

# Raspberry Pi Pico SDK — set PICO_SDK_PATH before calling this
# e.g. cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(lcdtap_pico2_ssd1306 VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# Shared pico2 peripheral drivers (SPI/I2C/DVI)
add_subdirectory(../../example/pico2_common pico2_common)

add_executable(lcdtap_pico2_ssd1306
    src/main.cpp
)

target_include_directories(lcdtap_pico2_ssd1306 PRIVATE
    include
)

# DVI_DEFAULT_PIO_INST must be pio0 so libdvi and pico_sock_cfg agree.
target_compile_definitions(lcdtap_pico2_ssd1306 PRIVATE
    DVI_DEFAULT_PIO_INST=pio0
)

# Forward framebuffer-size overrides only when explicitly passed via -D.
# Defaults are defined in config.h via #ifndef/#define.
foreach(VAR LCDTAP_LCD_SIZE1_W LCDTAP_LCD_SIZE1_H LCDTAP_LCD_SIZE2_W LCDTAP_LCD_SIZE2_H)
    if(DEFINED ${VAR})
        target_compile_definitions(lcdtap_pico2_ssd1306 PRIVATE "${VAR}=${${VAR}}")
    endif()
endforeach()

target_link_libraries(lcdtap_pico2_ssd1306
    pico2_common      # pulls in lcdtap, libdvi, hardware_dma/gpio/i2c/irq/pio
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_vreg
)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(lcdtap_pico2_ssd1306)
