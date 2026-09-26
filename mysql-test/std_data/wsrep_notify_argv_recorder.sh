#!/bin/sh

# Fake mysql client for the galera_wsrep_notify_pwd_injection test.
# Records argv instead of connecting anywhere, and drops the SQL piped
# into stdin by wsrep_notify_pwd_injection.sh.

: > "$MYSQLTEST_VARDIR/tmp/wsrep_notify_pwd_injection.argv"
for arg in "$@"; do
    printf '%s\n' "$arg" >> "$MYSQLTEST_VARDIR/tmp/wsrep_notify_pwd_injection.argv"
done
cat >/dev/null
exit 0
