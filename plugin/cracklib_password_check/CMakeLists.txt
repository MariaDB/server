INCLUDE (CheckIncludeFiles)
INCLUDE (CheckLibraryExists)

SET(CPACK_RPM_cracklib-password-check_PACKAGE_SUMMARY "CrackLib Password Validation Plugin for MariaDB server" PARENT_SCOPE)
SET(CPACK_RPM_cracklib-password-check_PACKAGE_DESCRIPTION "This password validation plugin uses cracklib to allow only
sufficiently secure (as defined by cracklib) user passwords in MariaDB server." PARENT_SCOPE)

CHECK_LIBRARY_EXISTS(crack FascistCheckUser "" HAVE_LIBCRACK)

SET(CMAKE_REQUIRED_DEFINITIONS -Dsize_t=int) # debian hack, debian bug.
CHECK_INCLUDE_FILES (crack.h HAVE_CRACK_H)

IF (HAVE_ALLOCA_H AND HAVE_CRACK_H AND HAVE_LIBCRACK AND HAVE_MEMCPY)
  MYSQL_ADD_PLUGIN(cracklib_password_check cracklib_password_check.c
                   LINK_LIBRARIES crack MODULE_ONLY
                   COMPONENT cracklib-password-check)

  IF (RPM)
    SET(inst_location ${INSTALL_SUPPORTFILESDIR})
    INSTALL(DIRECTORY policy DESTINATION ${inst_location} COMPONENT cracklib-password-check)
    FIND_PROGRAM(CHECKMODULE checkmodule)
    FIND_PROGRAM(SEMODULE_PACKAGE semodule_package)
    MARK_AS_ADVANCED(CHECKMODULE SEMODULE_PACKAGE)
    
    # Build pp files in policy/selinux
    IF(CHECKMODULE AND SEMODULE_PACKAGE)
      FOREACH(pol mariadb-plugin-cracklib-password-check)
        SET(src ${CMAKE_CURRENT_SOURCE_DIR}/policy/selinux/${pol}.te)
        SET(tmp ${CMAKE_CURRENT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/${pol}-pp.dir/${pol}.mod)
        SET(out ${CMAKE_CURRENT_BINARY_DIR}/${pol}.pp)
        ADD_CUSTOM_COMMAND(OUTPUT ${out}
          COMMAND ${CHECKMODULE} -M -m ${src} -o ${tmp}
          COMMAND ${SEMODULE_PACKAGE} -m ${tmp} -o ${out}
        DEPENDS ${src})
        ADD_CUSTOM_TARGET(${pol}-pp ALL DEPENDS ${out})
        INSTALL(FILES ${out} DESTINATION ${inst_location}/policy/selinux COMPONENT cracklib-password-check)
      ENDFOREACH()
    ENDIF()

  ENDIF()

ENDIF()
