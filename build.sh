#!/bin/bash

cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
ctest --test-dir build

# Static build (for initramfs / QEMU)
cmake -B build-static -DTINIT_STATIC=ON -DTINIT_BUILD_TESTS=OFF -DCMAKE_CXX_FLAGS="-Os"
cmake --build build-static -j$(nproc) --target trivialInit
