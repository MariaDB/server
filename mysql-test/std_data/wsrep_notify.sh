#!/bin/sh -eu

# This is a simple example of wsrep notification script (wsrep_notify_cmd).
# It will create 'wsrep' schema and two tables in it: 'membeship' and 'status'
# and fill them on every membership or node status change.
#
# Edit parameters below to specify the address and login to server.

USER=root
HOST=127.0.0.1
PORT=$NODE_MYPORT_1

SCHEMA="mtr_wsrep_notify"
MEMB_TABLE="$SCHEMA.membership"
STATUS_TABLE="$SCHEMA.status"

BEGIN="
SET wsrep_on=0;
CREATE SCHEMA IF NOT EXISTS $SCHEMA;
CREATE TABLE IF NOT EXISTS $MEMB_TABLE (
    idx  INT,
    uuid CHAR(40), /* node UUID */
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
BEGIN;
"
END="COMMIT;"

configuration_change()
{
    echo "$BEGIN;"

    local idx=0

    for NODE in $(echo $MEMBERS | sed s/,/\ /g)
    do
        echo "INSERT INTO $MEMB_TABLE VALUES ( $idx, "
        # Don't forget to properly quote string values
        echo "'$NODE'" | sed  s/\\//\',\'/g
        echo ");"
        idx=$(( $idx + 1 ))
    done

    echo "INSERT INTO $STATUS_TABLE VALUES($idx, $INDEX, '$STATUS', '$CLUSTER_UUID', $PRIMARY);"

    echo "$END"
}

status_update()
{
    echo "$BEGIN; UPDATE $STATUS_TABLE SET status='$STATUS'; $END;"
}

COM=status_update # not a configuration change by default

while [ $# -gt 0 ]
do
    case $1 in
    --status)
        STATUS=$2
        shift
        ;;
    --uuid)
        CLUSTER_UUID=$2
        shift
        ;;
    --primary)
        [ "$2" = "yes" ] && PRIMARY="1" || PRIMARY="0"
        COM=configuration_change
        shift
        ;;
    --index)
        INDEX=$2
        shift
        ;;
    --members)
        MEMBERS=$2
        shift
        ;;
    esac
    shift
done

case $STATUS in
    "joined" | "donor" | "synced")
        $COM | mysql -B -u$USER -h$HOST -P$PORT
        ;;
    *) 
        exit 0
        ;;
esac
