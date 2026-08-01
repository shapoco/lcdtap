cmake_minimum_required(VERSION 3.20)

# Raspberry Pi Pico SDK — set PICO_SDK_PATH before calling this
# e.g. cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(lcdtap_pico2w_remote VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Requires the wireless board (CYW43 WiFi + LED).
if(NOT PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

pico_sdk_init()

# Shared pico2 peripheral drivers (SPI/I2C input, JSON engine, flash store)
add_subdirectory(../../example/pico2_common pico2_common)

add_executable(lcdtap_pico2w_remote
    src/main.cpp
    src/net_config.cpp
    src/net_cmds.cpp
    src/stats.cpp
    src/wifi_mgr.cpp
    src/led_status.cpp
    src/http_server.cpp
)

target_include_directories(lcdtap_pico2w_remote PRIVATE
    include
)

# clk_sys runs at 312 MHz; the CYW43 PIO SPI is clk_sys/(2*div), and the
# chip tops out around 50 MHz, so raise the divider from its 150 MHz-tuned
# default of 2 to 4 (=> 39 MHz).
target_compile_definitions(lcdtap_pico2w_remote PRIVATE
    CYW43_PIO_CLOCK_DIV_INT=4
)

# Forward framebuffer-size overrides only when explicitly passed.
foreach(VAR
        LCDTAP_LCD_SIZE_W LCDTAP_LCD_SIZE_H)
    if(DEFINED ${VAR})
        target_compile_definitions(lcdtap_pico2w_remote PRIVATE "${VAR}=${${VAR}}")
    endif()
endforeach()

target_link_libraries(lcdtap_pico2w_remote
    pico2_common               # lcdtap, bus input, JSON engine, flash store
    pico2_json_cdc             # USB CDC transport for the JSON interface
    pico_cyw43_arch_lwip_poll  # WiFi + lwIP, NO_SYS polling mode
    pico_lwip_mdns             # mDNS responder (hostname.local)
    pico_stdlib
    pico_multicore
    hardware_flash
    hardware_sync
    hardware_watchdog
)

# Enable USB CDC stdio; disable UART stdio
pico_enable_stdio_usb(lcdtap_pico2w_remote 1)
pico_enable_stdio_uart(lcdtap_pico2w_remote 0)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(lcdtap_pico2w_remote)
