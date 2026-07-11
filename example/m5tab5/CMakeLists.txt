cmake_minimum_required(VERSION 3.16)

# Core lcdtap library lives outside the example tree; register it as an
# extra component (see lib/CMakeLists.txt's ESP_PLATFORM branch).
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../lib)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lcdtap_m5tab5)
