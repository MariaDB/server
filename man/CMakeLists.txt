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

IF(NOT WITHOUT_SERVER)
  INSTALL_MANPAGES(Server
    innochecksum.1 myisam_ftdump.1 myisamchk.1 aria_chk.1 aria_dump_log.1
    aria_ftdump.1 aria_pack.1 aria_read_log.1 myisamlog.1
    myisampack.1 mysql.server.1 mariadb-conv.1 mariadb-fix-extensions.1
    mariadb-install-db.1 mariadb-upgrade.1 mariadbd-multi.1 mariadbd-safe.1
    resolveip.1 mariadb-service-convert.1 mariadbd-safe-helper.1 mariadbd.8)
  INSTALL_MANPAGES(Backup mbstream.1 mariadb-backup.1)
  IF(WITH_WSREP)
    INSTALL_MANPAGES(Server
      wsrep_sst_rsync.1 wsrep_sst_common.1 wsrep_sst_mariabackup.1
      wsrep_sst_mysqldump.1 wsrep_sst_rsync_wan.1 galera_recovery.1
      galera_new_cluster.1 wsrep_sst_backup.1)
  ENDIF()
ENDIF()
INSTALL_MANPAGES(Client
  msql2mysql.1 mariadb.1 mariadb-find-rows.1 mariadb-waitpid.1
  mariadb-dumpslow.1 mariadb-access.1 mariadb-admin.1 mariadb-binlog.1
  mariadb-check.1 mariadb-dump.1 mariadb-import.1 mariadb-show.1
  mariadb-slap.1 mytop.1 mariadb-plugin.1 mariadb-embedded.1
  my_print_defaults.1 mariadb-hotcopy.1 mariadb-setpermission.1
  mariadb-tzinfo-to-sql.1 mariadb-convert-table-format.1 replace.1
  mariadb-secure-installation.1 perror.1 resolve_stack_dump.1)
INSTALL_MANPAGES(Development mariadb_config.1)
INSTALL_MANPAGES(Test
  mysql-stress-test.pl.1 mysql-test-run.pl.1 mariadb-client-test.1
  mariadb-test.1 mariadb-test-embedded.1 mariadb-client-test-embedded.1
  my_safe_process.1)
