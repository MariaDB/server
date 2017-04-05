find_path(
    ZSTD_INCLUDE_DIR
    NAMES "zstd.h"
)

find_library(
    ZSTD_LIBRARY
    NAMES zstd
)

set(ZSTD_LIBRARIES ${ZSTD_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    ZSTD DEFAULT_MSG ZSTD_INCLUDE_DIR ZSTD_LIBRARIES)

mark_as_advanced(ZSTD_INCLUDE_DIR ZSTD_LIBRARIES ZSTD_FOUND)

