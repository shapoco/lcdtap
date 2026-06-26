cmake_minimum_required(VERSION 3.20)
project(lcdtap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(lcdtap STATIC
    src/lcdtap.cpp
    src/config.cpp
    src/osd.cpp
    src/spi_display_base.cpp
    src/st7789_controller.cpp
    src/ili9341_controller.cpp
    src/ssd1306_controller.cpp
    src/ssd1331_controller.cpp
)

target_include_directories(lcdtap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# hardware_interp_headers is an INTERFACE-only target (no .c files) that
# provides the include path for hardware/interp.h, used under #ifdef PICO_RP2350.
if(TARGET hardware_interp_headers)
    target_link_libraries(lcdtap PRIVATE
        hardware_interp_headers
    )
endif()
