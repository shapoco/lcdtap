add_library(pico2_common STATIC
    src/spi_slave.cpp
    src/i2c_slave.cpp
    src/dvi_timing.cpp
    src/hstx_out.cpp
    src/composite_timing.cpp
    src/composite_encode.cpp
    src/composite_out.cpp
    src/composite_sink_r2r.cpp
    src/composite_sink_pwm.cpp
    src/json_lex.cpp
    src/json_b64.cpp
    src/json_rle.cpp
    src/json_intf.cpp
    src/sys_clock.cpp
    src/bus_input.cpp
    src/flash_store.cpp
    src/splash.cpp
)

# Core library (MCU-agnostic)
add_subdirectory(../../lib lib)

pico_generate_pio_header(pico2_common
    ${CMAKE_CURRENT_LIST_DIR}/src/spi_4line_mode0.pio
)
pico_generate_pio_header(pico2_common
    ${CMAKE_CURRENT_LIST_DIR}/src/composite_dac.pio
)
pico_generate_pio_header(pico2_common
    ${CMAKE_CURRENT_LIST_DIR}/src/spi_3line_mode0.pio
)
pico_generate_pio_header(pico2_common
    ${CMAKE_CURRENT_LIST_DIR}/src/parallel_8bit.pio
)
pico_generate_pio_header(pico2_common
    ${CMAKE_CURRENT_LIST_DIR}/src/parallel_2cs.pio
)

target_include_directories(pico2_common
    PUBLIC  include
    # pico_generate_pio_header adds ${CMAKE_CURRENT_BINARY_DIR} as PUBLIC,
    # so spi_4line_mode0.pio.h is visible to all targets that link pico2_common.
)

# Selects the SRAM placement of the composite hot path (LCDTAP_CVBS_HOT). The
# host test compiles composite_encode.cpp without this, so it stays a no-op
# there and the file remains MCU-independent.
target_compile_definitions(pico2_common PUBLIC LCDTAP_TARGET_PICO=1)

target_link_libraries(pico2_common PUBLIC
    lcdtap
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_flash
    hardware_gpio
    hardware_i2c
    hardware_irq
    hardware_pio
    hardware_pll
    hardware_pwm
    hardware_vreg
)

# USB CDC transport for the JSON interface. INTERFACE library: the source
# compiles as part of each executable so it sees that target's TinyUSB/stdio
# configuration (pico_enable_stdio_usb).
add_library(pico2_json_cdc INTERFACE)
target_sources(pico2_json_cdc INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/cdc_trx.cpp
)
target_link_libraries(pico2_json_cdc INTERFACE pico2_common)
