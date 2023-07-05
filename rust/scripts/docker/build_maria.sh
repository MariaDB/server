#!/bin/sh
set -eaux

echo starting internal build

export CMAKE_ARCHIVE_OUTPUT_DIRECTORY=/obj/lib
export CMAKE_LIBRARY_OUTPUT_DIRECTORY=/obj/lib
export CMAKE_RUNTIME_OUTPUT_DIRECTORY=/obj/bin

mkdir -p build-mariadb
cd build-mariadb

git config --global --add safe.directory '*'

cmake \
    -S/checkout\
    -B/obj/build-mariadb \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=/obj/lib \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=/obj/lib \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/obj/bin \
    -DUPDATE_SUBMODULES=OFF
cmake --build .
