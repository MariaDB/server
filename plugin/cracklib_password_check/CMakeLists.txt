INCLUDE (CheckIncludeFiles)
INCLUDE (CheckLibraryExists)

CHECK_INCLUDE_FILES (crack.h HAVE_CRACK_H)
CHECK_LIBRARY_EXISTS(crack FascistCheckUser "" HAVE_LIBCRACK)

IF (HAVE_ALLOCA_H AND HAVE_CRACK_H AND HAVE_LIBCRACK AND HAVE_MEMCPY)
  MYSQL_ADD_PLUGIN(cracklib_password_check cracklib_password_check.c
                   LINK_LIBRARIES crack MODULE_ONLY)
ENDIF()
