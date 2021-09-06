if(PMEM_LIBRARIES)
  set(PMEM_FOUND TRUE)
  return()
endif()
if(DEFINED PMEM_LIBRARIES)
  set(PMEM_FOUND FALSE)
  return()
endif()

find_path(PMEM_INCLUDE_DIR NAMES libpmem.h)
find_library(PMEM_LIBRARIES NAMES pmem)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    PMEM DEFAULT_MSG
    PMEM_LIBRARIES PMEM_INCLUDE_DIR)

mark_as_advanced(PMEM_INCLUDE_DIR PMEM_LIBRARIES)
