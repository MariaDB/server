# Copyright (c) 2012, 2020, MariaDB
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA
SET(MAN1_WSREP wsrep_sst_rsync.1 wsrep_sst_common.1 wsrep_sst_mariabackup.1
    wsrep_sst_rsync_wan.1)
SET(MAN1_SERVER innochecksum.1 myisam_ftdump.1 myisamchk.1
                aria_chk.1 aria_dump_log.1 aria_ftdump.1 aria_pack.1 aria_read_log.1
                aria_s3_copy.1
		myisamlog.1 myisampack.1 mysql.server.1 mariadb-conv.1
		mysql_fix_extensions.1
		mysql_install_db.1
		mysql_upgrade.1
		mysqld_multi.1 mysqld_safe.1
		resolveip.1 mariadb-service-convert.1
		mysqld_safe_helper.1
		wsrep_sst_mysqldump.1
		galera_recovery.1 galera_new_cluster.1
		mysql_ldb.1 myrocks_hotbackup.1
		mbstream.1 mariabackup.1 ${MAN1_WSREP})
SET(MAN8_SERVER mysqld.8)
SET(MAN1_CLIENT msql2mysql.1 mysql.1 mysql_find_rows.1 mysql_waitpid.1
		mysqldumpslow.1
		mysqlaccess.1 mysqladmin.1 mysqlbinlog.1 mysqlcheck.1
		mysqldump.1 mysqlimport.1 mysqlshow.1 mysqlslap.1 mytop.1
                mysql_plugin.1 mysql_embedded.1 my_print_defaults.1
                mysqlhotcopy.1 mysql_setpermission.1 mysql_tzinfo_to_sql.1
                mysql_convert_table_format.1 replace.1 mysql_secure_installation.1
		perror.1 resolve_stack_dump.1)
SET(MAN1_DEVEL mysql_config.1)
SET(MAN1_TEST mysql-stress-test.pl.1 mysql-test-run.pl.1 mysql_client_test.1
              mysqltest.1 mysqltest_embedded.1 mysql_client_test_embedded.1
              my_safe_process.1)
IF(NOT WITHOUT_SERVER)
  INSTALL(FILES ${MAN1_SERVER} DESTINATION ${INSTALL_MANDIR}/man1 COMPONENT ManPagesServer)
  INSTALL(FILES ${MAN8_SERVER} DESTINATION ${INSTALL_MANDIR}/man8 COMPONENT ManPagesServer)
ENDIF()

INSTALL(FILES ${MAN1_CLIENT} DESTINATION ${INSTALL_MANDIR}/man1 COMPONENT ManPagesClient)
INSTALL(FILES ${MAN1_DEVEL} DESTINATION ${INSTALL_MANDIR}/man1 COMPONENT ManPagesDevelopment)
INSTALL(FILES ${MAN1_TEST} DESTINATION ${INSTALL_MANDIR}/man1 COMPONENT ManPagesTest)

macro(MARIADB_SYMLINK_MANPAGE)
  list(LENGTH MARIADB_SYMLINK_TOS _len)
  math(EXPR _listlen "${_len}-1")

  foreach(_index RANGE ${_listlen})
    list(GET MARIADB_SYMLINK_TOS ${_index} _name)

    set(_manname "${_name}")
    list(FIND MAN1_SERVER ${_manname}.1 _iman1server)
    list(FIND MAN8_SERVER ${_manname}.8 _iman8server)
    list(FIND MAN1_CLIENT ${_manname}.1 _iman1client)
    list(FIND MAN1_DEVEL ${_manname}.1 _iman1devel)
    list(FIND MAN1_TEST ${_manname}.1 _iman1test)

    if (${_iman1client} GREATER -1)
      MARIADB_SYMLINK_MANPAGE_CREATE(${_manname} ${_index} 1 "man1" ManPagesClient)
    endif()
    if (${_iman1devel} GREATER -1)
      MARIADB_SYMLINK_MANPAGE_CREATE(${_manname} ${_index} 1 "man1" ManPagesDevelopment)
    endif()
    if (${_iman1test} GREATER -1)
      MARIADB_SYMLINK_MANPAGE_CREATE(${_manname} ${_index} 1 "man1" ManPagesTest)
    endif()
    if (WITHOUT_SERVER)
	    continue()
    endif()
    if (${_iman1server} GREATER -1)
      MARIADB_SYMLINK_MANPAGE_CREATE(${_manname} ${_index} 1 "man1" ManPagesServer)
    endif()
    if (${_iman8server} GREATER -1)
      MARIADB_SYMLINK_MANPAGE_CREATE(${_manname} ${_index} 8 "man8" ManPagesServer)
    endif()
  endforeach(_index)
endmacro(MARIADB_SYMLINK_MANPAGE)

macro(MARIADB_SYMLINK_MANPAGE_CREATE mysqlname index mannr dir comp)
  LIST(GET MARIADB_SYMLINK_FROMS ${index} _mariadbname)
  SET(dest "${CMAKE_CURRENT_BINARY_DIR}/${_mariadbname}.${mannr}")
  FILE(WRITE ${dest} ".so ${dir}/${mysqlname}.${mannr}")
  INSTALL(FILES ${dest} DESTINATION ${INSTALL_MANDIR}/${dir} COMPONENT ${comp})
endmacro(MARIADB_SYMLINK_MANPAGE_CREATE)

MARIADB_SYMLINK_MANPAGE()
