cmake_minimum_required(VERSION 3.20)

# Under an ESP-IDF build, register as a component instead of a standalone
# CMake project -- idf_component_register() must run in project() context
# set up by the top-level ESP-IDF project.cmake, and target_link_libraries
# on plain STATIC targets doesn't interoperate with IDF's component
# dependency graph the way idf_component_register() does.
if(ESP_PLATFORM)
    idf_component_register(
        SRCS
            src/lcdtap.cpp
            src/config.cpp
            src/osd.cpp
            src/spi_display_base.cpp
            src/st7789_controller.cpp
            src/ili9341_controller.cpp
            src/ssd1306_controller.cpp
            src/ssd1331_controller.cpp
        INCLUDE_DIRS include
    )
    target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
    # ESP-IDF's default per-component flags turn on -Werror=all (unlike the
    # PlatformIO/host builds, which only warn); two pre-existing warnings
    # here are false positives under that stricter setting:
    #   -Wswitch:            ControllerBase::processRamwrData()'s switch
    #                         omits InterfaceFormat::GRAY1_VPACK8_H2L --
    #                         harmless because Ssd1306Controller overrides
    #                         processRamwrData() entirely (virtual dispatch
    #                         never reaches the base switch for that
    #                         format). The other switches only omit each
    #                         enum's NUM_* sentinel value.
    #   -Wformat-truncation:  a bounded-width snprintf in osd.cpp that GCC
    #                         can't prove fits from the declared uint16_t
    #                         range alone.
    target_compile_options(${COMPONENT_LIB} PRIVATE
        -Wno-error=switch
        -Wno-error=format-truncation
    )
    return()
endif()

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
