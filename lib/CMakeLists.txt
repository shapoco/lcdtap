cmake_minimum_required(VERSION 3.20)
project(spilcd2dvi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(spilcd2dvi STATIC
    src/spilcd2dvi.cpp
    src/st7789_controller.cpp
)

target_include_directories(spilcd2dvi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
