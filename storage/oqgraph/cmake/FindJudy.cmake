# - Try to find Judy.
#
# Additionally, on Windows, this module reads hints about search locations from variables:
#  JUDY_ROOT             - Preferred installation prefix
#
# To build Judy on Windows: (Tested with judy-1.0.5)
#
# * Download the sources tarball from http://sourceforge.net/projects/judy/
# * Extract the source
# * Win32: open the Visual Studio C++ Express 2010 command prompt and navigate to the src/ directory.
#   Then execute:  build.bat
#	* Win64: open the Windows SDK 7.1 Command Prompt and navigate to the src/ directory
#   Then execute:  build.bat
# * Run the mariadb build with JUDY_ROOT=path\to\judy
#
# Once done this will define
#
#  Judy_FOUND - system has Judy
#  Judy_INCLUDE_DIRS - the Judy include directory
#  Judy_LIBRARIES - Link these to use Judy
#  Judy_DEFINITIONS - Compiler switches required for using Judy

IF(MSVC)
  # For now, assume Judy built according to the above instructions
  if (NOT "$ENV{JUDY_ROOT}" STREQUAL "")
    # Avoid passing backslashes to _Boost_FIND_LIBRARY due to macro re-parsing.
    string(REPLACE "\\" "/" Judy_INCLUDE_DIRS_search $ENV{JUDY_ROOT}/src)
    string(REPLACE "\\" "/" Judy_LIBRARIES_search $ENV{JUDY_ROOT}/src)
  endif()
ELSE(MSVC)
  IF (Judy_INCLUDE_DIRS AND Judy_LIBRARIES)
      SET(Judy_FIND_QUIETLY TRUE)
  ENDIF (Judy_INCLUDE_DIRS AND Judy_LIBRARIES)
ENDIF(MSVC)

FIND_PATH(Judy_INCLUDE_DIRS Judy.h PATHS ${Judy_INCLUDE_DIRS_search})
FIND_LIBRARY(Judy_LIBRARIES Judy PATHS ${Judy_LIBRARIES_search})

IF (Judy_INCLUDE_DIRS AND Judy_LIBRARIES)
  SET(Judy_FOUND TRUE)
ELSE (Judy_INCLUDE_DIRS AND Judy_LIBRARIES)
  SET(Judy_FOUND FALSE)
  if (MSVC)
    MESSAGE(STATUS "How to build Judy on Windows:")
    MESSAGE(STATUS "1. Download the source tarball from http://sourceforge.net/projects/judy/")
    IF (CMAKE_SIZEOF_VOID_P EQUAL 8)
      MESSAGE(STATUS "2. Extract the source, open the Visual Studio command prompt and navigate to the src/ directory.")
    ELSE (CMAKE_SIZEOF_VOID_P EQUAL 8)
      MESSAGE(STATUS "2. Extract the source, open the Windows SDK 7.1 Command Prompt and navigate to the src/ directory.")
    ENDIF (CMAKE_SIZEOF_VOID_P EQUAL 8)
    MESSAGE(STATUS "3. Execute the command: 'build'")
    MESSAGE(STATUS "4. Rerun this cmake with the environment variable: 'set JUDY_ROOT=x:\\path\\to\\judy'")
  endif(MSVC)
ENDIF (Judy_INCLUDE_DIRS AND Judy_LIBRARIES)

IF (Judy_FOUND)
  IF (NOT Judy_FIND_QUIETLY)
    MESSAGE(STATUS "Found libjudy: ${Judy_LIBRARIES}")
  ENDIF (NOT Judy_FIND_QUIETLY)
ELSE (Judy_FOUND)
  IF (Judy_FIND_REQUIRED)
    MESSAGE(FATAL_ERROR "Could NOT find libjudy")
  ENDIF (Judy_FIND_REQUIRED)
ENDIF (Judy_FOUND)

MARK_AS_ADVANCED(Judy_INCLUDE_DIRS Judy_LIBRARIES)

