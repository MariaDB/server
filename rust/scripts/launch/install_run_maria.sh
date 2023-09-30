#!/bin/sh
# https://mariadb.com/kb/en/generic-build-instructions/

set -eaux

cmake --install "$BUILD_DIR"

useradd mysql

touch /log.txt
chown -R mysql /usr/local/mysql/ /log.txt
cd /usr/local/mysql/
scripts/mariadb-install-db --user=mysql
/usr/local/mysql/bin/mariadbd-safe --user=mysql \
    --plugin-maturity=experimental \
    --log-error=/log.txt &
tail -f /log.txt
