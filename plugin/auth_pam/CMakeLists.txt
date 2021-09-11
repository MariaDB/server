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
  ADD_DEFINITIONS(-D_GNU_SOURCE)
  MYSQL_ADD_PLUGIN(auth_pam_v1 auth_pam_v1.c LINK_LIBRARIES pam MODULE_ONLY)
  MYSQL_ADD_PLUGIN(auth_pam auth_pam.c LINK_LIBRARIES pam ${CMAKE_DL_LIBS} MODULE_ONLY)
  IF (TARGET auth_pam)
    MYSQL_ADD_EXECUTABLE(auth_pam_tool auth_pam_tool.c DESTINATION ${INSTALL_PLUGINDIR}/auth_pam_tool_dir COMPONENT Server)
    TARGET_LINK_LIBRARIES(auth_pam_tool pam)
    SET(CPACK_RPM_server_USER_FILELIST ${CPACK_RPM_server_USER_FILELIST}
        "%attr(700,-,-) ${INSTALL_PLUGINDIRABS}/auth_pam_tool_dir"
        "%attr(4755,-,-) ${INSTALL_PLUGINDIRABS}/auth_pam_tool_dir/auth_pam_tool")
    SET(CPACK_RPM_server_USER_FILELIST ${CPACK_RPM_server_USER_FILELIST} PARENT_SCOPE)
  ENDIF()
  IF(TARGET auth_pam OR TARGET auth_pam_v1)
    ADD_SUBDIRECTORY(testing)
    ADD_LIBRARY(pam_user_map MODULE mapper/pam_user_map.c)
    TARGET_LINK_LIBRARIES(pam_user_map pam)
    SET_TARGET_PROPERTIES (pam_user_map PROPERTIES PREFIX "")
    IF(INSTALL_PAMDIR)
      INSTALL(TARGETS pam_user_map DESTINATION ${INSTALL_PAMDIR} COMPONENT Server)
      INSTALL(FILES mapper/user_map.conf DESTINATION ${INSTALL_PAMDATADIR} COMPONENT Server)
      SET(CPACK_RPM_server_USER_FILELIST ${CPACK_RPM_server_USER_FILELIST} "%config(noreplace) ${INSTALL_PAMDATADIRABS}/*" PARENT_SCOPE)
    ENDIF()
  ENDIF()
ENDIF()

CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake
               ${CMAKE_CURRENT_BINARY_DIR}/config_auth_pam.h)
