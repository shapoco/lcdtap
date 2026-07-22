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
)

# Core library (MCU-agnostic)
add_subdirectory(../../lib lib)

pico_generate_pio_header(pico2_common
    ${CMAKE_CURRENT_LIST_DIR}/src/spi_4line_mode0.pio
)
pico_generate_pio_header(pico2_common
    ${CMAKE_CURRENT_LIST_DIR}/src/composite_dac.pio
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
    hardware_gpio
    hardware_i2c
    hardware_irq
    hardware_pio
    hardware_pll
    hardware_pwm
    hardware_vreg
)
