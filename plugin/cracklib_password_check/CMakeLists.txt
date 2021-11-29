INCLUDE (CheckIncludeFiles)
INCLUDE (CheckLibraryExists)

SET(CPACK_RPM_cracklib-password-check_PACKAGE_SUMMARY "CrackLib Password Validation Plugin for MariaDB" PARENT_SCOPE)
SET(CPACK_RPM_cracklib-password-check_PACKAGE_DESCRIPTION "This password validation plugin uses cracklib to allow only
sufficiently secure (as defined by cracklib) user passwords in MariaDB." PARENT_SCOPE)

CHECK_LIBRARY_EXISTS(crack FascistCheckUser "" HAVE_LIBCRACK)

SET(CMAKE_REQUIRED_DEFINITIONS -Dsize_t=int) # debian hack, debian bug.
CHECK_INCLUDE_FILES (crack.h HAVE_CRACK_H)

IF (HAVE_ALLOCA_H AND HAVE_CRACK_H AND HAVE_LIBCRACK AND HAVE_MEMCPY)
  MYSQL_ADD_PLUGIN(cracklib_password_check cracklib_password_check.c
                   LINK_LIBRARIES crack MODULE_ONLY
                   COMPONENT cracklib-password-check)
ENDIF()
