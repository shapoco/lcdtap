#!/bin/bash

set -eux

mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DPICO_COPY_TO_RAM=1 -DPICO_BOARD=pico2
make -j
