#  gcc pam_mariadb_mtr.c -shared -lpam -fPIC -o pam_mariadb_mtr.so

ADD_LIBRARY(pam_mariadb_mtr MODULE pam_mariadb_mtr.c)
SET_TARGET_PROPERTIES (pam_mariadb_mtr PROPERTIES PREFIX "")
TARGET_LINK_LIBRARIES(pam_mariadb_mtr pam)

IF(CMAKE_C_COMPILER_ID MATCHES "Clang")
  SET_SOURCE_FILES_PROPERTIES(
    pam_mariadb_mtr.c
    PROPERTY COMPILE_FLAGS "-Wno-incompatible-pointer-types-discards-qualifiers")
ENDIF()

IF (NOT DEB) # avoid arch-dependent-file-in-usr-share error
  SET(dest DESTINATION "${INSTALL_MYSQLTESTDIR}/suite/plugins/pam" COMPONENT Test)
  INSTALL(TARGETS pam_mariadb_mtr ${dest})
  INSTALL(FILES mariadb_mtr.conf RENAME mariadb_mtr ${dest})
ENDIF()
