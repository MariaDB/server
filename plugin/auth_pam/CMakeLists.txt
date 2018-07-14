INCLUDE (CheckIncludeFiles)
INCLUDE (CheckFunctionExists)

CHECK_INCLUDE_FILES (security/pam_appl.h HAVE_PAM_APPL_H)
CHECK_FUNCTION_EXISTS (strndup HAVE_STRNDUP)

IF(HAVE_PAM_APPL_H)
  IF(HAVE_STRNDUP)
    ADD_DEFINITIONS(-DHAVE_STRNDUP)
  ENDIF(HAVE_STRNDUP)
  ADD_DEFINITIONS(-D_GNU_SOURCE)
  MYSQL_ADD_PLUGIN(auth_pam_v1 auth_pam_v1.c LINK_LIBRARIES pam MODULE_ONLY)
  MYSQL_ADD_PLUGIN(auth_pam auth_pam.c LINK_LIBRARIES pam dl MODULE_ONLY)
  MYSQL_ADD_EXECUTABLE(auth_pam_tool auth_pam_tool.c DESTINATION ${INSTALL_PLUGINDIR}/auth_pam_tool_dir COMPONENT Server)
  TARGET_LINK_LIBRARIES(auth_pam_tool pam)
  INSTALL(CODE "EXECUTE_PROCESS(
                   COMMAND chmod u=rwx,g=,o= auth_pam_tool_dir
                   COMMAND chmod u=rwxs,g=rx,o=rx auth_pam_tool_dir/auth_pam_tool
                   WORKING_DIRECTORY \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${INSTALL_PLUGINDIR}/)"
                 COMPONENT Server)
ENDIF(HAVE_PAM_APPL_H)

