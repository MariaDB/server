#!/bin/sh

# Fake mysql client for the galera_wsrep_notify_pwd_injection test.
# Records argv and MYSQL_PWD instead of connecting anywhere, and drops
# the SQL the wsrep_notify script pipes into stdin.

: > "$MYSQLTEST_VARDIR/tmp/wsrep_notify_pwd_injection.argv"
for arg in "$@"; do
    printf '%s\n' "$arg" >> "$MYSQLTEST_VARDIR/tmp/wsrep_notify_pwd_injection.argv"
done
printf 'MYSQL_PWD=%s\n' "${MYSQL_PWD-<unset>}" > "$MYSQLTEST_VARDIR/tmp/wsrep_notify_pwd_injection.env"
cat >/dev/null
exit 0
