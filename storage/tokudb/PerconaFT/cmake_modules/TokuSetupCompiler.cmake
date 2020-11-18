function(add_c_defines)
  set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS ${ARGN})
endfunction(add_c_defines)

if (APPLE)
  add_c_defines(DARWIN=1 _DARWIN_C_SOURCE)
endif ()

## preprocessor definitions we want everywhere
add_c_defines(
  _FILE_OFFSET_BITS=64
  _LARGEFILE64_SOURCE
  __STDC_FORMAT_MACROS
  __STDC_LIMIT_MACROS
  __LONG_LONG_SUPPORTED
  )
if (NOT CMAKE_SYSTEM_NAME STREQUAL FreeBSD)
  ## on FreeBSD these types of macros actually remove functionality
  add_c_defines(
    _DEFAULT_SOURCE
    _XOPEN_SOURCE=600
    )
endif ()

## add TOKU_PTHREAD_DEBUG for debug builds
if (CMAKE_VERSION VERSION_LESS 3.0)
  set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS_DEBUG TOKU_PTHREAD_DEBUG=1 TOKU_DEBUG_TXN_SYNC=1)
  set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS_DRD TOKU_PTHREAD_DEBUG=1 TOKU_DEBUG_TXN_SYNC=1)
  set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS_DRD _FORTIFY_SOURCE=2)
else ()
  set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
    $<$<OR:$<CONFIG:DEBUG>,$<CONFIG:DRD>>:TOKU_PTHREAD_DEBUG=1 TOKU_DEBUG_TXN_SYNC=1>
    $<$<CONFIG:DRD>:_FORTIFY_SOURCE=2>
    )
endif ()

## coverage
option(USE_GCOV "Use gcov for test coverage." OFF)
if (USE_GCOV)
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES GNU)
    message(FATAL_ERROR "Must use the GNU compiler to compile for test coverage.")
  endif ()
  find_program(COVERAGE_COMMAND NAMES gcov47 gcov)
endif (USE_GCOV)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

## adds a compiler flag if the compiler supports it
macro(prepend_cflags_if_supported)
  foreach(flag ${ARGN})
    MY_CHECK_AND_SET_COMPILER_FLAG(${flag})
  endforeach(flag)
endmacro(prepend_cflags_if_supported)

if (NOT DEFINED MYSQL_PROJECT_NAME_DOCSTRING)
  set (OPTIONAL_CFLAGS "${OPTIONAL_CFLAGS} -Wmissing-format-attribute")
endif()

## disable some warnings
prepend_cflags_if_supported(
  -Wno-missing-field-initializers
  -Wstrict-null-sentinel
  -Winit-self
  -Wswitch
  -Wtrampolines
  -Wlogical-op
  ${OPTIONAL_CFLAGS}
  -Wno-error=missing-format-attribute
  -Wno-error=address-of-array-temporary
  -Wno-error=tautological-constant-out-of-range-compare
  -Wno-error=maybe-uninitialized
  -Wno-error=extern-c-compat
  -fno-rtti
  -fno-exceptions
  -Wno-error=nonnull-compare
  )

if (CMAKE_CXX_FLAGS MATCHES -fno-implicit-templates)
  # must append this because mysql sets -fno-implicit-templates and we need to override it
  check_cxx_compiler_flag(-fimplicit-templates HAVE_CXX_IMPLICIT_TEMPLATES)
  if (HAVE_CXX_IMPLICIT_TEMPLATES)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fimplicit-templates")
  endif ()
endif()

## Clang has stricter POD checks.  So, only enable this warning on our other builds (Linux + GCC)
if (NOT CMAKE_CXX_COMPILER_ID MATCHES Clang)
  prepend_cflags_if_supported(
    -Wpacked
    )
endif ()

option (PROFILING "Allow profiling and debug" ON)
if (PROFILING)
  prepend_cflags_if_supported(
    -fno-omit-frame-pointer
  )
endif ()

# new flag sets in MySQL 8.0 seem to explicitly disable this
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions")

## set extra debugging flags and preprocessor definitions
set(CMAKE_C_FLAGS_DEBUG "-g3 -O0 ${CMAKE_C_FLAGS_DEBUG}")
set(CMAKE_CXX_FLAGS_DEBUG "-g3 -O0 ${CMAKE_CXX_FLAGS_DEBUG}")

## flags to use when we want to run DRD on the resulting binaries
## DRD needs debugging symbols.
## -O0 makes it too slow, and -O2 inlines too much for our suppressions to work.  -O1 is just right.
set(CMAKE_C_FLAGS_DRD "-g3 -O1 ${CMAKE_C_FLAGS_DRD}")
set(CMAKE_CXX_FLAGS_DRD "-g3 -O1 ${CMAKE_CXX_FLAGS_DRD}")

## set extra release flags
## need to set flags for RelWithDebInfo as well because we want the MySQL/MariaDB builds to use them
if (CMAKE_CXX_COMPILER_ID STREQUAL Clang)
  # have tried -flto and -O4, both make our statically linked executables break apple's linker
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -g -O3 -UNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -g -O3 -UNDEBUG")
  set(CMAKE_C_FLAGS_RELEASE "-g -O3 ${CMAKE_C_FLAGS_RELEASE} -UNDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "-g -O3 ${CMAKE_CXX_FLAGS_RELEASE} -UNDEBUG")
else ()
  if (APPLE)
    set(FLTO_OPTS "-fwhole-program")
  else ()
    set(FLTO_OPTS "-fuse-linker-plugin")
  endif()
  # we overwrite this because the default passes -DNDEBUG and we don't want that
  set(CMAKE_C_FLAGS_RELWITHDEBINFO "-flto ${FLTO_OPTS} ${CMAKE_C_FLAGS_RELWITHDEBINFO} -g -O3 -UNDEBUG")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-flto ${FLTO_OPTS} ${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -g -O3 -UNDEBUG")
  set(CMAKE_C_FLAGS_RELEASE "-g -O3 -flto ${FLTO_OPTS} ${CMAKE_C_FLAGS_RELEASE} -UNDEBUG")
  set(CMAKE_CXX_FLAGS_RELEASE "-g -O3 -flto ${FLTO_OPTS} ${CMAKE_CXX_FLAGS_RELEASE} -UNDEBUG")
  set(CMAKE_EXE_LINKER_FLAGS "-g ${FLTO_OPTS} ${CMAKE_EXE_LINKER_FLAGS}")
  set(CMAKE_SHARED_LINKER_FLAGS "-g ${FLTO_OPTS} ${CMAKE_SHARED_LINKER_FLAGS}")
endif ()

## set warnings
prepend_cflags_if_supported(
  -Wextra
  -Wbad-function-cast
  -Wno-missing-noreturn
  -Wstrict-prototypes
  -Wmissing-prototypes
  -Wmissing-declarations
  -Wpointer-arith
  #-Wshadow will fail with GCC-8
  ${OPTIONAL_CFLAGS}
  ## other flags to try:
  #-Wunsafe-loop-optimizations
  #-Wpointer-arith
  #-Wc++-compat
  #-Wc++11-compat
  #-Wwrite-strings
  #-Wzero-as-null-pointer-constant
  #-Wlogical-op
  #-Wvector-optimization-performance
  )

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL Clang)
  # Disabling -Wcast-align with clang.  TODO: fix casting and re-enable it, someday.
  prepend_cflags_if_supported(-Wcast-align)
endif ()

## never want these
set(CMAKE_C_FLAGS "-Wno-error ${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "-Wno-error ${CMAKE_CXX_FLAGS}")

# pick language dialect
set(CMAKE_C_FLAGS "-std=c99 ${CMAKE_C_FLAGS}")
check_cxx_compiler_flag(-std=c++11 HAVE_STDCXX11)
check_cxx_compiler_flag(-std=c++0x HAVE_STDCXX0X)
if (HAVE_STDCXX11)
  set(CMAKE_CXX_FLAGS "-std=c++11 ${CMAKE_CXX_FLAGS}")
elseif (HAVE_STDCXX0X)
  set(CMAKE_CXX_FLAGS "-std=c++0x ${CMAKE_CXX_FLAGS}")
else ()
  message(FATAL_ERROR "${CMAKE_CXX_COMPILER} doesn't support -std=c++11 or -std=c++0x, you need one that does.")
endif ()

function(add_space_separated_property type obj propname val)
  get_property(oldval ${type} ${obj} PROPERTY ${propname})
  if (oldval MATCHES NOTFOUND)
    set_property(${type} ${obj} PROPERTY ${propname} "${val}")
  else ()
    set_property(${type} ${obj} PROPERTY ${propname} "${val} ${oldval}")
  endif ()
endfunction(add_space_separated_property)

## this function makes sure that the libraries passed to it get compiled
## with gcov-needed flags, we only add those flags to our libraries
## because we don't really care whether our tests get covered
function(maybe_add_gcov_to_libraries)
  if (USE_GCOV)
    foreach(lib ${ARGN})
      add_space_separated_property(TARGET ${lib} COMPILE_FLAGS --coverage)
      add_space_separated_property(TARGET ${lib} LINK_FLAGS --coverage)
      target_link_libraries(${lib} LINK_PRIVATE gcov)
    endforeach(lib)
  endif (USE_GCOV)
endfunction(maybe_add_gcov_to_libraries)
