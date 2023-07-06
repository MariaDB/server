#!/bin/sh
set -eaux

echo starting internal build

# export CMAKE_ARCHIVE_OUTPUT_DIRECTORY=/obj/lib
# export CMAKE_LIBRARY_OUTPUT_DIRECTORY=/obj/lib
# export CMAKE_RUNTIME_OUTPUT_DIRECTORY=/obj/bin

mkdir -p build-mariadb
cd build-mariadb

git config --global --add safe.directory '*'

# We disable submodule updates and mroonga because they are two targets that
# touch the source directory.
cmake \
    -S/checkout\
    -B/obj/build-mariadb \
    -DCMAKE_BUILD_TYPE=Debug \
    -DUPDATE_SUBMODULES=OFF \
    -DPLUGIN_MROONGA=NO \
    -DPLUGIN_ROCKSDB=NO \
    -DPLUGIN_SPIDER=NO \
    -DPLUGIN_TOKUDB=NO \
    -G Ninja

# -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=/obj/lib \
# -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=/obj/lib \
# -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=/obj/bin \

# ninja automatically uses all the cores
cmake --build .
