#!/bin/sh -eu

# This is a simple example of wsrep notification script (wsrep_notify_cmd).
# It will create 'wsrep' schema and two tables in it: 'membership' and 'status'
# and fill them on every membership or node status change.
#
# Edit parameters below to specify the address and login to server:
#
USER='root'
PSWD=''
#
# If these parameters are not set, then the values
# passed by the server are taken:
#
HOST="127.0.0.1"
PORT=$NODE_MYPORT_1
#
# Edit parameters below to specify SSL parameters:
#
ssl_cert=""
ssl_key=""
ssl_ca=""
ssl_capath=""
ssl_cipher=""
ssl_crl=""
ssl_crlpath=""
ssl_verify_server_cert=0
#
# Client executable path:
#
CLIENT="$EXE_MYSQL"
#
# Name of schema and tables:
#
SCHEMA="mtr_wsrep_notify"
MEMB_TABLE="$SCHEMA.membership"
STATUS_TABLE="$SCHEMA.status"

WSREP_ON='SET wsrep_on=ON'
WSREP_OFF='SET wsrep_on=OFF'

BEGIN="$WSREP_OFF;
CREATE SCHEMA IF NOT EXISTS $SCHEMA;
CREATE TABLE IF NOT EXISTS $MEMB_TABLE (
    idx  INT,
    uuid CHAR(40),        /* node UUID */
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
    echo "$BEGIN;"

    local idx=0

    for NODE in $(echo "$MEMBERS" | sed s/,/\ /g)
    do
        echo "INSERT INTO $MEMB_TABLE VALUES ( $idx, "
        # Don't forget to properly quote string values
        echo "'$NODE'" | sed s/\\//\',\'/g
        echo ");"
        idx=$(( $idx+1 ))
    done

    echo "INSERT INTO $STATUS_TABLE VALUES($idx, $INDEX, '$STATUS', '$CLUSTER_UUID', $PRIMARY);"

    echo "$END;"
}

status_update()
{
    echo "$BEGIN; UPDATE $STATUS_TABLE SET status='$STATUS'; $END;"
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
        echo "$1" | sed -E "s/^$pattern+|$pattern+\$//g"
    fi
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

SSL_PARAM=""

if [ -n "$ssl_key$ssl_cert$ssl_ca$ssl_capath$ssl_cipher$ssl_crl$ssl_crlpath" ]
then
    SSL_PARAM=' --ssl'
    [ -n "$ssl_key" ]     && SSL_PARAM="$SSL_PARAM --ssl-key='$ssl_key'"
    [ -n "$ssl_cert" ]    && SSL_PARAM="$SSL_PARAM --ssl-cert='$ssl_cert'"
    [ -n "$ssl_ca" ]      && SSL_PARAM="$SSL_PARAM --ssl-ca='$ssl_ca'"
    [ -n "$ssl_capath" ]  && SSL_PARAM="$SSL_PARAM --ssl-capath='$ssl_capath'"
    [ -n "$ssl_cipher" ]  && SSL_PARAM="$SSL_PARAM --ssl-cipher='$ssl_cipher'"
    [ -n "$ssl_crl" ]     && SSL_PARAM="$SSL_PARAM --ssl-crl='$ssl_crl'"
    [ -n "$ssl_crlpath" ] && SSL_PARAM="$SSL_PARAM --ssl-crlpath='$ssl_crlpath'"
    if [ -n "$ssl_verify_server_cert" ]; then
        if [ "$ssl_verify_server_cert" != "0" -o \
             "$ssl_verify_server_cert" = "on" ]
        then
            SSL_PARAM="$SSL_PARAM --ssl-verify-server-cert"
        fi
    fi
fi

case "$STATUS" in
    'joined' | 'donor' | 'synced')
        "$COM" | eval "$CLIENT" -B "-u'$USER'"${PSWD:+" -p'$PSWD'"}\
                      "-h'$HOST'" "-P$PORT"$SSL_PARAM
        ;;
    *)
        # The node might be shutting down or not initialized
        ;;
esac

exit 0
