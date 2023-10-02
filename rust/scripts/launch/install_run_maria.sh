#!/bin/sh
# https://mariadb.com/kb/en/generic-build-instructions/

set -eaux

cmake --install "$BUILD_DIR"

useradd mysql

touch /log.txt
# Setup for file plugin examples
# install plugin file_key_management soname 'file_key_management.so';
# https://mariadb.com/kb/en/file-key-management-encryption-plugin/
echo "1;a7addd9adea9978fda19f21e6be987880e68ac92632ca052e5bb42b1a506939a" > /file-keys.txt

chown -R mysql /usr/local/mysql/ /log.txt
cd /usr/local/mysql/
scripts/mariadb-install-db --user=mysql
/usr/local/mysql/bin/mariadbd-safe --user=mysql \
    --plugin-maturity=experimental \
    --log-error=/log.txt \
    --loose-file-key-management-filename=/file-keys.txt &
tail -f /log.txt

# --plugin-load-add='encryption_example_aes=example_encryption_aes_rs'
