#!/bin/sh

set -eaux

echo starting internal build

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

git config --global --add safe.directory '*'

# Cmake won't automatically regenerate .so files for some reason, we force it to
# by removing
rm -f "${BUILD_DIR}/rust_target/debug/"*.so
rm -f "${BUILD_DIR}/rust_target/release/"*.so

# We disable submodule updates and mroonga because they are two targets that
# touch the source directory.
cmake \
    -S/checkout\
    -B${BUILD_DIR} \
    -DCMAKE_C_COMPILER_LAUNCHER=sccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=sccache \
    -DCMAKE_BUILD_TYPE=Debug \
    -DRUN_ABI_CHECK=NO \
    -DUPDATE_SUBMODULES=OFF \
    -DPLUGIN_MROONGA=NO \
    -DPLUGIN_ROCKSDB=NO \
    -DPLUGIN_SPIDER=NO \
    -DPLUGIN_TOKUDB=NO \
    -G Ninja

export CMD_CCMAKE="ccmake -S/checkout -B${BUILD_DIR}"

# ninja automatically uses all the cores
cmake --build "$BUILD_DIR"
