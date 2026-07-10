#!/bin/bash

set -e

# Uninstall Mroonga
mariadb --defaults-file=/etc/mysql/debian.cnf < /usr/share/mariadb/mroonga/uninstall.sql || true
# Always exit with success instead of leaving dpkg in a broken state


#DEBHELPER#
