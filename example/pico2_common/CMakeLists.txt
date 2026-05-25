add_library(pico2_common STATIC
    src/spi_slave.cpp
    src/i2c_slave.cpp
    src/dvi_out.cpp
)

# PicoDVI libdvi (INTERFACE library)
add_subdirectory(
    ../../submodule/PicoDVI/software/libdvi
    libdvi
)

# Core library (MCU-agnostic)
add_subdirectory(../../lib lib)

target_compile_definitions(pico2_common PUBLIC
    DVI_SYMBOLS_PER_WORD=2
    DVI_VERTICAL_REPEAT=2
	DVI_N_TMDS_BUFFERS=3
)

pico_generate_pio_header(pico2_common
    ${CMAKE_CURRENT_LIST_DIR}/src/spi_4line_mode0.pio
)

target_include_directories(pico2_common
    PUBLIC  include
    # PicoDVI pin-config header lives here
    ../../submodule/PicoDVI/software/include
    # pico_generate_pio_header adds ${CMAKE_CURRENT_BINARY_DIR} as PUBLIC,
    # so spi_4line_mode0.pio.h is visible to all targets that link pico2_common.
)

target_link_libraries(pico2_common PUBLIC
    lcdtap
    libdvi
    pico_multicore
    hardware_dma
    hardware_gpio
    hardware_i2c
    hardware_irq
    hardware_pio
)
