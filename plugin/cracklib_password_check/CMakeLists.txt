INCLUDE (CheckIncludeFiles)
INCLUDE (CheckLibraryExists)

CHECK_LIBRARY_EXISTS(crack FascistCheckUser "" HAVE_LIBCRACK)

SET(CMAKE_REQUIRED_DEFINITIONS -Dsize_t=int) # debian hack, debian bug.
CHECK_INCLUDE_FILES (crack.h HAVE_CRACK_H)

IF (HAVE_ALLOCA_H AND HAVE_CRACK_H AND HAVE_LIBCRACK AND HAVE_MEMCPY)
  MYSQL_ADD_PLUGIN(cracklib_password_check cracklib_password_check.c
                   LINK_LIBRARIES crack MODULE_ONLY
                   COMPONENT cracklib-password-check)
ENDIF()
