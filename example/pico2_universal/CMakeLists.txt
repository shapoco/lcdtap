cmake_minimum_required(VERSION 3.20)

# Raspberry Pi Pico SDK — set PICO_SDK_PATH before calling this
# e.g. cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(lcdtap_pico2_universal VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

pico_sdk_init()

# Shared pico2 peripheral drivers (SPI/I2C/DVI)
add_subdirectory(../../example/pico2_common pico2_common)

add_executable(lcdtap_pico2_universal
    src/main.cpp
    src/flash_config.cpp
    src/uart_if.cpp
)

# Generate PIO headers for universal-only interface modes
pico_generate_pio_header(lcdtap_pico2_universal
    ${CMAKE_CURRENT_LIST_DIR}/src/spi_3line_mode0.pio
)
pico_generate_pio_header(lcdtap_pico2_universal
    ${CMAKE_CURRENT_LIST_DIR}/src/parallel_8bit.pio
)

target_include_directories(lcdtap_pico2_universal PRIVATE
    include
)

# DVI_DEFAULT_PIO_INST must be pio0 so libdvi and pico_sock_cfg agree.
target_compile_definitions(lcdtap_pico2_universal PRIVATE
    DVI_DEFAULT_PIO_INST=pio0
)

# Forward framebuffer-size overrides only when explicitly passed.
foreach(VAR
        LCDTAP_LCD_SIZE_W LCDTAP_LCD_SIZE_H)
    if(DEFINED ${VAR})
        target_compile_definitions(lcdtap_pico2_universal PRIVATE "${VAR}=${${VAR}}")
    endif()
endforeach()

target_link_libraries(lcdtap_pico2_universal
    pico2_common      # pulls in lcdtap, libdvi, hardware_dma/gpio/i2c/irq/pio
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_flash
    hardware_sync
    hardware_vreg
    hardware_watchdog
)

# Enable USB CDC stdio; disable UART stdio
pico_enable_stdio_usb(lcdtap_pico2_universal 1)
pico_enable_stdio_uart(lcdtap_pico2_universal 0)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(lcdtap_pico2_universal)
