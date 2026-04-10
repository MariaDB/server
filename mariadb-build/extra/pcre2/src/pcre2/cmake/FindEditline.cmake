# Modified from FindReadline.cmake (PH Feb 2012)

find_path(EDITLINE_INCLUDE_DIR readline.h PATH_SUFFIXES editline edit/readline)
mark_as_advanced(EDITLINE_INCLUDE_DIR)

find_library(EDITLINE_LIBRARY NAMES edit)
mark_as_advanced(EDITLINE_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Editline DEFAULT_MSG EDITLINE_LIBRARY EDITLINE_INCLUDE_DIR)
