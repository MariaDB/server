# Add MariaDB symlinks 
macro(MARIADB_INSTALL_SYMLINK filepath sympath)
    install(CODE "execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink ${filepath} ${sympath})")
    install(CODE "message(\"-- Created symlink: ${sympath} -> ${filepath}\")")
endmacro(MARIADB_INSTALL_SYMLINK)
