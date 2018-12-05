# Add MariaDB symlinks 
MACRO(CREATE_MARIADB_SYMLINK filepath sympath)
    install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${filepath} ${sympath})" COMPONENT Client)
    install(CODE "message(\"-- Created symlink: ${sympath} -> ${filepath}\")" COMPONENT Client)

    install(FILES ${sympath} DESTINATION ${INSTALL_BINDIR} COMPONENT Client)
ENDMACRO(CREATE_MARIADB_SYMLINK)
