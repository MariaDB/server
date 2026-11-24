#!/bin/sh -eu

# This is a simple example of wsrep notification script (wsrep_notify_cmd).
# It will create 'wsrep' schema and two tables in it: 'membeship' and 'status'
# and fill them on every membership or node status change.
#
# Edit parameters below to specify the address and login to server.
# Each can also be set via a WSREP_NOTIFY_* environment variable (used by
# the test suite so it can drive this same script instead of a copy of it).
#
USER="${WSREP_NOTIFY_USER-root}"
PSWD="${WSREP_NOTIFY_PSWD-rootpass}"
#
# If these parameters are not set, then the values
# passed by the server are taken:
#
HOST="${WSREP_NOTIFY_HOST-127.0.0.1}"
PORT="${WSREP_NOTIFY_PORT-3306}"
#
# Edit parameters below to specify SSL parameters:
#
ssl_cert="${WSREP_NOTIFY_SSL_CERT-}"
ssl_key="${WSREP_NOTIFY_SSL_KEY-}"
ssl_ca="${WSREP_NOTIFY_SSL_CA-}"
ssl_capath="${WSREP_NOTIFY_SSL_CAPATH-}"
ssl_cipher="${WSREP_NOTIFY_SSL_CIPHER-}"
ssl_crl="${WSREP_NOTIFY_SSL_CRL-}"
ssl_crlpath="${WSREP_NOTIFY_SSL_CRLPATH-}"
ssl_verify_server_cert="${WSREP_NOTIFY_SSL_VERIFY_SERVER_CERT-0}"
#
# Client executable path:
#
CLIENT="${WSREP_NOTIFY_CLIENT-mysql}"
#
# Name of schema and tables:
#
SCHEMA="${WSREP_NOTIFY_SCHEMA-wsrep}"
MEMB_TABLE="$SCHEMA.membership"
STATUS_TABLE="$SCHEMA.status"

WSREP_ON='SET wsrep_on=ON'
WSREP_OFF='SET wsrep_on=OFF'
# quote_sql() output relies on backslash escapes being honoured
SQL_MODE="SET SESSION sql_mode=REPLACE(@@sql_mode,'NO_BACKSLASH_ESCAPES','')"

BEGIN="CREATE SCHEMA IF NOT EXISTS $SCHEMA;
CREATE TABLE IF NOT EXISTS $MEMB_TABLE (
    idx  INT UNIQUE PRIMARY KEY,
    uuid CHAR(40) UNIQUE, /* node UUID */
    name VARCHAR(32),     /* node name */
    addr VARCHAR(256)     /* node address */
) ENGINE=MEMORY;
CREATE TABLE IF NOT EXISTS $STATUS_TABLE (
    size   INT,      /* component size   */
    idx    INT,      /* this node index  */
    status CHAR(16), /* this node status */
    uuid   CHAR(40), /* cluster UUID */
    prim   BOOLEAN   /* if component is primary */
) ENGINE=MEMORY;
BEGIN"
END="COMMIT; $WSREP_ON"

configuration_change()
{
    printf '%s\n' "$SQL_MODE; $WSREP_OFF; DROP SCHEMA IF EXISTS $SCHEMA; $BEGIN;"

    local idx=0 id rest name addr IFS=','

    # MEMBERS is "id/name/addr,..."; addr may itself contain '/' or '[]'
    set -f
    for NODE in $MEMBERS
    do
        id=${NODE%%/*}
        rest=${NODE#*/}
        name=${rest%%/*}
        addr=${rest#*/}
        printf '%s\n' "INSERT INTO $MEMB_TABLE VALUES ($idx, '$(quote_sql "$id")', '$(quote_sql "$name")', '$(quote_sql "$addr")');"
        idx=$(( $idx+1 ))
    done
    set +f

    printf '%s\n' "INSERT INTO $STATUS_TABLE VALUES($idx, $INDEX, '$(quote_sql "$STATUS")', '$(quote_sql "$CLUSTER_UUID")', $PRIMARY);"

    printf '%s\n' "$END;"
}

status_update()
{
    printf '%s\n' "$SQL_MODE; $WSREP_OFF; $BEGIN; UPDATE $STATUS_TABLE SET status='$(quote_sql "$STATUS")'; $END;"
}

trim_string()
{
    if [ -n "${BASH_VERSION:-}" ]; then
        local pattern="[![:space:]${2:-}]"
        local x="${1#*$pattern}"
        local z=${#1}
        x=${#x}
        if [ $x -ne $z ]; then
            local y="${1%$pattern*}"
            y=${#y}
            x=$(( z-x-1 ))
            y=$(( y-x+1 ))
            printf '%s' "${1:$x:$y}"
        else
            printf ''
        fi
    else
        local pattern="[[:space:]${2:-}]"
        printf '%s\n' "$1" | sed -E "s/^$pattern+|$pattern+\$//g"
    fi
}

# Escape a value for embedding in a single-quoted SQL string literal.
# Backslash must be escaped first, or a trailing backslash would swallow
# the closing quote. Quotes are doubled so even with NO_BACKSLASH_ESCAPES
# they can't end the literal early.
quote_sql()
{
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e "s/'/''/g"
}

COM='status_update' # not a configuration change by default

STATUS=""
CLUSTER_UUID=""
PRIMARY=0
INDEX=""
MEMBERS=""

while [ $# -gt 0 ]; do
    case $1 in
    '--status')
        STATUS=$(trim_string "$2")
        shift
        ;;
    '--uuid')
        CLUSTER_UUID=$(trim_string "$2")
        shift
        ;;
    '--primary')
        arg=$(trim_string "$2")
        [ "$arg" = 'yes' ] && PRIMARY=1 || PRIMARY=0
        COM='configuration_change'
        shift
        ;;
    '--index')
        INDEX=$(trim_string "$2")
        shift
        ;;
    '--members')
        MEMBERS=$(trim_string "$2")
        shift
        ;;
    esac
    shift
done

USER=$(trim_string "$USER")
PSWD=$(trim_string "$PSWD")

HOST=$(trim_string "$HOST")
PORT=$(trim_string "$PORT")

case "$HOST" in
\[*)
    HOST="${HOST##\[}"
    HOST=$(trim_string "${HOST%%\]}")
    ;;
esac

if [ -z "$HOST" ]; then
    HOST="${NOTIFY_HOST:-}"
fi
if [ -z "$PORT" ]; then
    PORT="${NOTIFY_PORT:-}"
fi

ssl_key=$(trim_string "$ssl_key");
ssl_cert=$(trim_string "$ssl_cert");
ssl_ca=$(trim_string "$ssl_ca");
ssl_capath=$(trim_string "$ssl_capath");
ssl_cipher=$(trim_string "$ssl_cipher");
ssl_crl=$(trim_string "$ssl_crl");
ssl_crlpath=$(trim_string "$ssl_crlpath");
ssl_verify_server_cert=$(trim_string "$ssl_verify_server_cert");

case "$STATUS" in
    'joined' | 'donor' | 'synced')
        set -- -B -u"$USER" -h"$HOST" -P"$PORT"

        if [ -n "$ssl_key$ssl_cert$ssl_ca$ssl_capath$ssl_cipher$ssl_crl$ssl_crlpath" ]
        then
            set -- "$@" --ssl
            [ -n "$ssl_key" ]     && set -- "$@" --ssl-key="$ssl_key"
            [ -n "$ssl_cert" ]    && set -- "$@" --ssl-cert="$ssl_cert"
            [ -n "$ssl_ca" ]      && set -- "$@" --ssl-ca="$ssl_ca"
            [ -n "$ssl_capath" ]  && set -- "$@" --ssl-capath="$ssl_capath"
            [ -n "$ssl_cipher" ]  && set -- "$@" --ssl-cipher="$ssl_cipher"
            [ -n "$ssl_crl" ]     && set -- "$@" --ssl-crl="$ssl_crl"
            [ -n "$ssl_crlpath" ] && set -- "$@" --ssl-crlpath="$ssl_crlpath"
            if [ -n "$ssl_verify_server_cert" ]; then
                if [ "$ssl_verify_server_cert" != "0" -o \
                     "$ssl_verify_server_cert" = "on" ]
                then
                    set -- "$@" --ssl-verify-server-cert
                fi
            fi
        fi

        "$COM" | MYSQL_PWD="$PSWD" exec "$CLIENT" "$@"
        ;;
    *)
        # The node might be shutting down or not initialized
        ;;
esac

exit 0
