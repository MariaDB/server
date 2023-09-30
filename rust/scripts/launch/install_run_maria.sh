#!/bin/sh
# https://mariadb.com/kb/en/generic-build-instructions/

set -eaux

cmake --install "$BUILD_DIR"

useradd mysql

chown -R mysql /usr/local/mysql/
cd /usr/local/mysql/
scripts/mariadb-install-db --user=mysql
/usr/local/mysql/bin/mariadbd-safe --user=mysql --plugin-maturity=experimental
