IF(WIN32)
  RETURN()
ENDIF()

INCLUDE (CheckIncludeFiles)
INCLUDE (CheckFunctionExists)

CHECK_INCLUDE_FILES (security/pam_ext.h  HAVE_PAM_EXT_H)
CHECK_INCLUDE_FILES (security/pam_appl.h HAVE_PAM_APPL_H)
CHECK_FUNCTION_EXISTS (strndup HAVE_STRNDUP)
CHECK_FUNCTION_EXISTS (getgrouplist HAVE_GETGROUPLIST)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

# Check whether getgrouplist uses gtid_t for second and third arguments.
SET(CMAKE_REQUIRED_FLAGS -Werror)
CHECK_C_SOURCE_COMPILES(
"
#include <grp.h>
#include <unistd.h>
int main() {
  char *arg_1= 0;
  gid_t arg_2=0, arg_3;
  int arg_4;
  (void)getgrouplist(arg_1,arg_2,&arg_3,&arg_4);
  return 0;
}
"
HAVE_POSIX_GETGROUPLIST
)
SET(CMAKE_REQUIRED_FLAGS)

SET(CMAKE_REQUIRED_LIBRARIES pam)
CHECK_FUNCTION_EXISTS(pam_syslog HAVE_PAM_SYSLOG)
SET(CMAKE_REQUIRED_LIBRARIES)

IF(HAVE_PAM_APPL_H AND HAVE_GETGROUPLIST)
  FIND_LIBRARY(PAM_LIBRARY pam) # for srpm build-depends detection
  MYSQL_ADD_PLUGIN(auth_pam auth_pam.c LINK_LIBRARIES pam MODULE_ONLY)

  IF(TARGET auth_pam)
    ADD_LIBRARY(pam_user_map MODULE mapper/pam_user_map.c)
    TARGET_LINK_LIBRARIES(pam_user_map pam)
    SET_TARGET_PROPERTIES (pam_user_map PROPERTIES PREFIX "")
    IF(INSTALL_PAMDIR)
      INSTALL(TARGETS pam_user_map DESTINATION ${INSTALL_PAMDIR} COMPONENT Server)
      INSTALL(FILES mapper/user_map.conf DESTINATION ${INSTALL_PAMDATADIR} COMPONENT Server)
    ENDIF()
  ENDIF()
ENDIF()

CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake
               ${CMAKE_CURRENT_BINARY_DIR}/config_auth_pam.h)
