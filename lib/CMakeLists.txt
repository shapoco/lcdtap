cmake_minimum_required(VERSION 3.20)
project(lcdtap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(lcdtap STATIC
    src/lcdtap.cpp
    src/st7789_controller.cpp
    src/ssd1309_controller.cpp
)

target_include_directories(lcdtap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
