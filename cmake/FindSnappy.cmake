find_path(Snappy_INCLUDE_DIRS NAMES snappy.h)
find_library(Snappy_LIBRARIES NAMES snappy)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    Snappy DEFAULT_MSG
    Snappy_LIBRARIES Snappy_INCLUDE_DIRS)

mark_as_advanced(Snappy_INCLUDE_DIRS Snappy_LIBRARIES)
