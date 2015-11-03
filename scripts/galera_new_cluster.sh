#!/bin/sh

# This file is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or
# (at your option) any later version.

VERSION="@VERSION@@MYSQL_SERVER_SUFFIX@"
COMPILATION_COMMENT="@COMPILATION_COMMENT@"

systemctl set-environment _WSREP_NEW_CLUSTER='--wsrep-new-cluster' && \
    systemctl start ${1:-mariadb}

systemctl set-environment _WSREP_NEW_CLUSTER=''
