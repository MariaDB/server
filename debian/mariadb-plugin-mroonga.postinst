#!/bin/bash

set -e

# Install Mroonga
mariadb --defaults-file=/etc/mysql/debian.cnf < /usr/share/mysql/mroonga/install.sql || true
# Always exit with success instead of leaving dpkg in a broken state


#DEBHELPER#
