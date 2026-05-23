#!/bin/bash

set -eux

mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 ${@}
make -j
