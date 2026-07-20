add_library(pico2_common STATIC
    src/spi_slave.cpp
    src/i2c_slave.cpp
    src/dvi_timing.cpp
    src/hstx_out.cpp
    src/composite_timing.cpp
    src/composite_encode.cpp
    src/composite_out.cpp
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
    hardware_vreg
)
