# - Try to find Judy
# Once done this will define
#
#  Judy_FOUND - system has Judy
#  Judy_INCLUDE_DIR - the Judy include directory
#  Judy_LIBRARIES - Link these to use Judy
#  Judy_DEFINITIONS - Compiler switches required for using Judy

IF (Judy_INCLUDE_DIR AND Judy_LIBRARIES)
    SET(Judy_FIND_QUIETLY TRUE)
ENDIF (Judy_INCLUDE_DIR AND Judy_LIBRARIES)

FIND_PATH(Judy_INCLUDE_DIR Judy.h)
FIND_LIBRARY(Judy_LIBRARIES NAMES Judy)

IF (Judy_INCLUDE_DIR AND Judy_LIBRARIES)
   SET(Judy_FOUND TRUE)
ELSE (Judy_INCLUDE_DIR AND Judy_LIBRARIES)
   SET(Judy_FOUND FALSE)
ENDIF (Judy_INCLUDE_DIR AND Judy_LIBRARIES)

IF (Judy_FOUND)
  IF (NOT Judy_FIND_QUIETLY)
    MESSAGE(STATUS "Found libjudy: ${Judy_LIBRARIES}")
  ENDIF (NOT Judy_FIND_QUIETLY)
ELSE (Judy_FOUND)
  IF (Judy_FIND_REQUIRED)
    MESSAGE(FATAL_ERROR "Could NOT find libjudy")
  ENDIF (Judy_FIND_REQUIRED)
ENDIF (Judy_FOUND)

MARK_AS_ADVANCED(Judy_INCLUDE_DIR Judy_LIBRARIES)

