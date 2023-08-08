#!/bin/sh

# This file is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or
# (at your option) any later version.

if [ "${1}" = "-h" ] || [ "${1}" = "--help" ]; then
    cat <<EOF

Usage: ${0}

    The script galera_new_cluster is used to bootstrap new Galera Cluster,
    when all the nodes are down. Run galera_new_cluster on the first node only.
    On the remaining nodes simply run 'service @DAEMON_NAME@ start'.

    For more information on Galera Cluster configuration and usage see:
    https://mariadb.com/kb/en/mariadb/getting-started-with-mariadb-galera-cluster/

EOF
    exit 0
fi

systemctl set-environment _WSREP_NEW_CLUSTER='--wsrep-new-cluster' && \
    systemctl restart ${1:-mariadb}

extcode=$?

systemctl set-environment _WSREP_NEW_CLUSTER=''

exit $extcode
