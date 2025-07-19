#!/bin/bash
#
# This file is included by /etc/mysql/debian-start
#

## Is there MyISAM or Aria unclosed tables.
# - Requires the server to be up.
# - Is supposed to run silently in background.
function check_for_crashed_tables() {
  set -e
  set -u

  # But do it in the background to not stall the boot process.
  logger -p daemon.info -i -t"$0" "Triggering myisam-recover for all MyISAM tables and aria-recover for all Aria tables"

  # Checking for $? is unreliable so the size of the output is checked.
  # Some table handlers like HEAP do not support CHECK TABLE.
  tempfile=$(mktemp)

  # We have to use xargs in this case, because a for loop barfs on the
  # spaces in the thing to be looped over.

  # If a crashed table is encountered, the "mariadb" command will return with a status different from 0
  #
  # The first query will generate lines like.
  #   select count(*) into @discard from 'mysql'.'db'
  # The second line will load all tables without printing any actual results,
  # but may show warnings and definitely is expected to have some error and
  # exit code if crashed tables are encountered.
  #
  # Note that inside single quotes must be quoted with '\'' (to be outside of single quotes).
  set +e
  # The $MARIADB is intentionally used to expand into a command and arguments
  # shellcheck disable=SC2086
  echo '
    SELECT CONCAT("select count(*) into @discard from '\''", TABLE_SCHEMA, "'\''.'\''", TABLE_NAME, "'\''")
    FROM information_schema.TABLES WHERE TABLE_SCHEMA<>"INFORMATION_SCHEMA" AND TABLE_SCHEMA<>"PERFORMANCE_SCHEMA"
    AND (ENGINE="MyISAM" OR ENGINE="Aria")
    ' | \
    LC_ALL=C $MARIADB --skip-column-names --batch | \
    xargs --no-run-if-empty -i $MARIADB --skip-column-names --silent --batch --force -e "{}" &> "${tempfile}"
  set -e

  if [ -s "$tempfile" ]
  then
    (
      /bin/echo -e "\n" \
        "Improperly closed tables are also reported if clients are accessing\n" \
        "the tables *now*. A list of current connections is below.\n";
        $MYADMIN processlist status
    ) >> "${tempfile}"
    # Check for presence as a dependency on mailx would require an MTA.
    if [ -x /usr/bin/mailx ]
    then
      mailx -e -s"$MYCHECK_SUBJECT" "$MYCHECK_RCPT" < "$tempfile"
    fi
    (echo "$MYCHECK_SUBJECT"; cat "${tempfile}") | logger -p daemon.warn -i -t"$0"
  fi
  rm "${tempfile}"
}

## Check for tables needing an upgrade.
# - Requires the server to be up.
# - Is supposed to run silently in background.
function upgrade_system_tables_if_necessary() {
  set -e
  set -u

  logger -p daemon.info -i -t"$0" "Upgrading MariaDB tables if necessary."

  # Filter all "duplicate column", "duplicate key" and "unknown column"
  # errors as the script is designed to be idempotent.
  LC_ALL=C $MYUPGRADE \
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
     " | $MARIADB --skip-column-names)
  if [ "$ret" -ne "0" ]
  then
    logger -p daemon.warn -i -t"$0" "WARNING: mysql.user contains $ret root accounts without password!"
  fi
}
