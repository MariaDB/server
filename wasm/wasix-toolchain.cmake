# WASIX (wasm32) toolchain via wasixcc — https://wasix.org
# Install: https://github.com/wasix-org/wasixcc, then source ~/.wasixcc/env

SET(CMAKE_SYSTEM_NAME WASIX)
SET(CMAKE_SYSTEM_PROCESSOR wasm32)

SET(WASIXCC_ROOT "$ENV{HOME}/.wasixcc" CACHE PATH "wasixcc installation root")

SET(CMAKE_C_COMPILER   ${WASIXCC_ROOT}/bin/wasixcc)
SET(CMAKE_CXX_COMPILER ${WASIXCC_ROOT}/bin/wasixcc++)
SET(CMAKE_AR           ${WASIXCC_ROOT}/bin/wasixar)
SET(CMAKE_RANLIB       ${WASIXCC_ROOT}/bin/wasixranlib)
SET(CMAKE_NM           ${WASIXCC_ROOT}/bin/wasixnm)

# wasixcc links wasm executables fine without an emulator; linking probe
# programs is REQUIRED so CHECK_FUNCTION_EXISTS gives real results (with
# STATIC_LIBRARY every function probe would spuriously pass).
# CHECK_*_RUNS probes are skipped automatically (cross-compiling).

# Provides Platform/WASIX.cmake (CMake has no built-in WASIX platform).
LIST(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/platforms")

# wasix-libc is POSIX; make CMake's UNIX-conditional code paths apply.
SET(UNIX 1)

SET(CMAKE_EXECUTABLE_SUFFIX ".wasm")

SET(CMAKE_FIND_ROOT_PATH ${WASIXCC_ROOT}/sysroot/sysroot-eh)
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

SET(WASIX 1)
