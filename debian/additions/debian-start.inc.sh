#!/bin/bash
#
# This file is included by /etc/mysql/debian-start
#

## Check for tables needing an upgrade.
# - Requires the server to be up.
# - Is supposed to run silently in background.
function upgrade_system_tables_if_necessary() {
  set -e
  set -u

  logger -p daemon.info -i -t"$0" "Upgrading MariaDB tables if necessary."

  # Filter all "duplicate column", "duplicate key" and "unknown column"
  # errors as the script is designed to be idempotent.
  LC_ALL=C "${MYUPGRADE[@]}" \
    2>&1 \
    | grep -E -v '^(1|@had|ERROR (1051|1054|1060|1061|1146|1347|1348))' \
    | logger -p daemon.warn -i -t"$0"
}

## Check for the presence of both, root accounts with and without password.
# This might have been caused by a bug related to mysql_install_db (#418672).
function check_root_accounts() {
  set -e
  set -u

  logger -p daemon.info -i -t"$0" "Checking for insecure root accounts."

  ret=$(echo "
     SELECT count(*) FROM mysql.global_priv
     WHERE user='root' AND
           JSON_VALUE(priv, '$.plugin') in ('mysql_native_password', 'mysql_old_password', 'parsec') AND
           JSON_VALUE(priv, '$.authentication_string') = '' AND
           JSON_VALUE(priv, '$.password_last_changed') != 0
     " | "${MARIADB[@]}" --skip-column-names)
  if [ "$ret" -ne "0" ]
  then
    logger -p daemon.warn -i -t"$0" "WARNING: mysql.global_priv contains $ret root accounts without password!"
  fi
}
