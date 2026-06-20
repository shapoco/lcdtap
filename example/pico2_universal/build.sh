#!/bin/bash

set -eux

mkdir -p build
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPICO_BOARD=pico2 \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    ${@}
make -j
