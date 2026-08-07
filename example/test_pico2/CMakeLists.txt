cmake_minimum_required(VERSION 3.20)

# Raspberry Pi Pico SDK — set PICO_SDK_PATH before calling this
# e.g. cmake -DPICO_SDK_PATH=/path/to/pico-sdk ..
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(lcdtap_test_pico2 VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Requires the wireless board (CYW43 LED); override with -DPICO_BOARD.
if(NOT PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

pico_sdk_init()

# MCU-agnostic lcdtap core: used only for its headers (config enums, device
# command constants) — the rig links no controller objects.
add_subdirectory(../../lib lcdtap_lib)

# ---------------------------------------------------------------------------
# LovyanGFX (UI OLED). The submodule's own CMakeLists calls pico_sdk_init()
# itself, so it cannot be add_subdirectory'd after ours; replicate its
# pico-sdk source list as a local INTERFACE library instead.
# ---------------------------------------------------------------------------
set(LGFX_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../submodule/LovyanGFX)
file(GLOB LGFX_SRCS
    ${LGFX_ROOT}/src/lgfx/Fonts/efont/*.c
    ${LGFX_ROOT}/src/lgfx/Fonts/IPA/*.c
    ${LGFX_ROOT}/src/lgfx/Fonts/lvgl/*.c
    ${LGFX_ROOT}/src/lgfx/utility/*.c
    ${LGFX_ROOT}/src/lgfx/v1/*.cpp
    ${LGFX_ROOT}/src/lgfx/v1/lv_font/*.c
    ${LGFX_ROOT}/src/lgfx/v1/misc/*.cpp
    ${LGFX_ROOT}/src/lgfx/v1/panel/*.cpp
    ${LGFX_ROOT}/src/lgfx/v1/platforms/arduino_default/*.cpp
    ${LGFX_ROOT}/src/lgfx/v1/platforms/rp2040/*.cpp
    ${LGFX_ROOT}/src/lgfx/v1/touch/*.cpp
)
add_library(lovyangfx INTERFACE)
target_sources(lovyangfx INTERFACE ${LGFX_SRCS})
target_include_directories(lovyangfx INTERFACE ${LGFX_ROOT}/src)
target_compile_definitions(lovyangfx INTERFACE USE_PICO_SDK=1)
target_link_libraries(lovyangfx INTERFACE
    pico_stdlib
    hardware_dma
    hardware_gpio
    hardware_spi
    hardware_i2c
    hardware_pwm
)

# ---------------------------------------------------------------------------
# ArduinoJson (JSON client) — header only.
# ---------------------------------------------------------------------------
add_library(arduinojson INTERFACE)
target_include_directories(arduinojson INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/../../submodule/ArduinoJson/src
)

# ---------------------------------------------------------------------------
# Pico-PIO-USB (USB host to the target) + TinyUSB glue from the SDK tree.
# ---------------------------------------------------------------------------
set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/../../submodule/Pico-PIO-USB)
add_subdirectory(${PICO_PIO_USB_PATH} pico_pio_usb)
set(PICO_TINYUSB_PATH ${PICO_SDK_PATH}/lib/tinyusb)

# ---------------------------------------------------------------------------
# Test rig firmware
# ---------------------------------------------------------------------------
add_executable(lcdtap_test_pico2
    src/main.cpp
    src/oled.cpp
    src/input.cpp
    src/testgen.cpp
    src/verify.cpp
    src/vectors.cpp
    src/bus.cpp
    src/ctrl_init.cpp
    src/executor.cpp
    src/ui.cpp
    src/usb_descriptors.c
    src/usb_device.cpp
    src/usb_host.cpp
    src/link_cdc.cpp
    src/jsonclient.cpp
    # PIO-USB glue for TinyUSB (host uses hcd; dcd unused but harmless)
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/dcd_pio_usb.c
)

target_include_directories(lcdtap_test_pico2 PRIVATE
    include
)

# Tell Pico-PIO-USB to use the TinyUSB stack.
target_compile_definitions(lcdtap_test_pico2 PRIVATE PIO_USB_USE_TINYUSB)

# CYW43 PIO SPI divider: the sample-timing-tuned SPI program needs the PIO
# clock near the stock 75 MHz (both faster AND slower break it — 60 MHz
# fails, verified on hardware). main.cpp computes the divider from clk_sys
# at runtime via the dynamic-divider hook.
target_compile_definitions(lcdtap_test_pico2 PRIVATE
    CYW43_PIO_CLOCK_DIV_DYNAMIC=1
)

# CYW43 staged fault-isolation firmware: -DTESTRIG_CYW43_DIAG=1
if(DEFINED TESTRIG_CYW43_DIAG)
    target_compile_definitions(lcdtap_test_pico2 PRIVATE
        "TESTRIG_CYW43_DIAG=${TESTRIG_CYW43_DIAG}"
    )
endif()

# Optional clock override for fault isolation, e.g. -DTESTRIG_SYS_CLOCK_KHZ=120000
if(DEFINED TESTRIG_SYS_CLOCK_KHZ)
    target_compile_definitions(lcdtap_test_pico2 PRIVATE
        "TESTRIG_SYS_CLOCK_KHZ=${TESTRIG_SYS_CLOCK_KHZ}"
    )
endif()

# Bus master PIO programs (pio2; PIO-USB owns pio0/pio1).
pico_generate_pio_header(lcdtap_test_pico2 ${CMAKE_CURRENT_LIST_DIR}/src/spi_master.pio)
pico_generate_pio_header(lcdtap_test_pico2 ${CMAKE_CURRENT_LIST_DIR}/src/par8_master.pio)

target_link_libraries(lcdtap_test_pico2
    lcdtap
    lovyangfx
    arduinojson
    pico_stdlib
    pico_multicore
    pico_unique_id
    pico_cyw43_arch_none
    tinyusb_device
    tinyusb_host
    pico_pio_usb
    hardware_dma
    hardware_i2c
    hardware_pio
    hardware_timer
)

# No SDK stdio driver: pico_stdio_usb disables itself when tinyusb_host is
# linked, so printf goes through the custom CDC driver in usb_device.cpp.
pico_enable_stdio_usb(lcdtap_test_pico2 0)
pico_enable_stdio_uart(lcdtap_test_pico2 0)

# Generate UF2 for drag-and-drop flashing
pico_add_extra_outputs(lcdtap_test_pico2)
