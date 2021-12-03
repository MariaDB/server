# Create lists
if(COMMAND REGISTER_SYMLINK)
  return()
endif()

macro(REGISTER_SYMLINK from to)
  list(APPEND MARIADB_SYMLINK_FROMS ${from})
  list(APPEND MARIADB_SYMLINK_TOS ${to})
endmacro()

# MariaDB names for executables
REGISTER_SYMLINK("mysql" "mariadb")
REGISTER_SYMLINK("mysqlaccess" "mariadb-access")
REGISTER_SYMLINK("mysqladmin" "mariadb-admin")
REGISTER_SYMLINK("mariabackup" "mariadb-backup")
REGISTER_SYMLINK("mysqlbinlog" "mariadb-binlog")
REGISTER_SYMLINK("mysqlcheck" "mariadb-check")
REGISTER_SYMLINK("mysql_client_test_embedded" "mariadb-client-test-embedded")
REGISTER_SYMLINK("mysql_client_test" "mariadb-client-test")
REGISTER_SYMLINK("mariadb_config" "mariadb-config")
REGISTER_SYMLINK("mysql_convert_table_format" "mariadb-convert-table-format")
REGISTER_SYMLINK("mysqldump" "mariadb-dump")
REGISTER_SYMLINK("mysqldumpslow" "mariadb-dumpslow")
REGISTER_SYMLINK("mysql_embedded" "mariadb-embedded")
REGISTER_SYMLINK("mysql_find_rows" "mariadb-find-rows")
REGISTER_SYMLINK("mysql_fix_extensions" "mariadb-fix-extensions")
REGISTER_SYMLINK("mysqlhotcopy" "mariadb-hotcopy")
REGISTER_SYMLINK("mysqlimport" "mariadb-import")
REGISTER_SYMLINK("mysql_install_db" "mariadb-install-db")
REGISTER_SYMLINK("mysql_ldb" "mariadb-ldb")
REGISTER_SYMLINK("mysql_plugin" "mariadb-plugin")
REGISTER_SYMLINK("mysql_secure_installation" "mariadb-secure-installation")
REGISTER_SYMLINK("mysql_setpermission" "mariadb-setpermission")
REGISTER_SYMLINK("mysqlshow" "mariadb-show")
REGISTER_SYMLINK("mysqlslap" "mariadb-slap")
REGISTER_SYMLINK("mysqltest" "mariadb-test")
REGISTER_SYMLINK("mysqltest_embedded" "mariadb-test-embedded")
REGISTER_SYMLINK("mysql_tzinfo_to_sql" "mariadb-tzinfo-to-sql")
REGISTER_SYMLINK("mysql_upgrade" "mariadb-upgrade")
REGISTER_SYMLINK("mysql_upgrade_service" "mariadb-upgrade-service")
REGISTER_SYMLINK("mysql_upgrade_wizard" "mariadb-upgrade-wizard")
REGISTER_SYMLINK("mysql_waitpid" "mariadb-waitpid")
REGISTER_SYMLINK("mysqld" "mariadbd")
REGISTER_SYMLINK("mysqld_multi" "mariadbd-multi")
REGISTER_SYMLINK("mysqld_safe" "mariadbd-safe")
REGISTER_SYMLINK("mysqld_safe_helper" "mariadbd-safe-helper")

# Add MariaDB symlinks
macro(CREATE_MARIADB_SYMLINK src dir comp)
  # Find the MariaDB name for executable
  list(FIND MARIADB_SYMLINK_FROMS ${src} _index)

  if (${_index} GREATER -1)
    list(GET MARIADB_SYMLINK_TOS ${_index} mariadbname)
  endif()

  if (mariadbname)
    CREATE_MARIADB_SYMLINK_IN_DIR(${src} ${mariadbname} ${dir} ${comp})
  endif()
endmacro(CREATE_MARIADB_SYMLINK)

# Add MariaDB symlinks in directory
macro(CREATE_MARIADB_SYMLINK_IN_DIR src dest dir comp)
  if(UNIX)
    add_custom_target(
      SYM_${dest} ALL
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${dest}
    )

    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${dest} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E create_symlink ${src} ${dest}
      COMMENT "mklink ${src} -> ${dest}")

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${dest} DESTINATION ${dir} COMPONENT ${comp})
  endif()
endmacro(CREATE_MARIADB_SYMLINK_IN_DIR)
