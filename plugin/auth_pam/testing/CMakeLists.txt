#  gcc pam_mariadb_mtr.c -shared -lpam -fPIC -o pam_mariadb_mtr.so

ADD_LIBRARY(pam_mariadb_mtr MODULE pam_mariadb_mtr.c)
SET_TARGET_PROPERTIES (pam_mariadb_mtr PROPERTIES PREFIX "")
TARGET_LINK_LIBRARIES(pam_mariadb_mtr pam)

IF(CMAKE_C_COMPILER_ID MATCHES "Clang")
  SET_SOURCE_FILES_PROPERTIES(
    pam_mariadb_mtr.c
    PROPERTY COMPILE_FLAGS "-Wno-incompatible-pointer-types-discards-qualifiers")
ENDIF()

SET(PAM_MARIADB_MTR_LOCATON ${INSTALL_PLUGINDIR}/test_pam_modules)
INSTALL(TARGETS pam_mariadb_mtr DESTINATION ${PAM_MARIADB_MTR_LOCATON} COMPONENT TEST)
CONFIGURE_FILE(mariadb_mtr.conf mariadb_mtr)
INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mariadb_mtr DESTINATION "${INSTALL_MYSQLTESTDIR}/suite/plugins/pam" COMPONENT TEST)
