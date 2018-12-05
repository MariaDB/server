# Add MariaDB symlinks 
MACRO(INSTALL_MARIADB_SYMLINK filepath symname)
    # EXECUTE_PROCESS(COMMAND ln -s ${filepath} ${symname})
    # INSTALL(CODE "EXECUTE_PROCESS(COMMAND ln -s ${filepath} ${CMAKE_INSTALL_PREFIX}/bin/${symname})")
    install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${filepath} ${symname})")
    install(CODE "message(\"-- Created symlink: ${CMAKE_INSTALL_PREFIX}/bin/${symname} -> ${filepath}\")")
    # INSTALL(TARGETS ${CMAKE_BINARY_DIR}/${symname} RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
    # INSTALL(FILES ${CMAKE_BINARY_DIR}/${symname} DESTINATION ${CMAKE_INSTALL_PREFIX}/bin COMPONENT MariaDBSymlink)
    # CMAKE_BINARY_DIR:
    # /home/rasmus/development/mariadb/10.4-development/rasmushoj-mariadb-server/build
    # CMAKE_INSTALL_PREFIX: /usr
    # MESSAGE(FATAL_ERROR "CMAKE_BINARY_DIR: " ${CMAKE_BINARY_DIR}/${symname} " CMAKE_INSTALL_PREFIX: " ${CMAKE_INSTALL_PREFIX}/bin)
ENDMACRO(INSTALL_MARIADB_SYMLINK)

MACRO(CREATE_MARIADB_SYMLINK filepath sympath)
    install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${filepath} ${sympath})" COMPONENT Client)
    install(CODE "message(\"-- Created symlink: ${sympath} -> ${filepath}\")" COMPONENT Client)

    install(FILES ${sympath} DESTINATION ${CMAKE_INSTALL_PREFIX}/bin COMPONENT Client)
ENDMACRO(CREATE_MARIADB_SYMLINK)
