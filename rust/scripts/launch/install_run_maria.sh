#!/bin/sh
# https://mariadb.com/kb/en/generic-build-instructions/

set -eaux

cmake --install "$BUILD_DIR"

useradd mysql

touch /error.log /general.log
# Setup for file plugin examples
# install plugin file_key_management soname 'file_key_management.so';
# https://mariadb.com/kb/en/file-key-management-encryption-plugin/
echo "1;a7addd9adea9978fda19f21e6be987880e68ac92632ca052e5bb42b1a506939a" > /file-keys.txt

chown -R mysql /usr/local/mysql/ /error.log /general.log
cd /usr/local/mysql/
scripts/mariadb-install-db --user=mysql
/usr/local/mysql/bin/mariadbd-safe --user=mysql \
    --plugin-maturity=experimental \
    --log-error=/error.log \
    --general-log=on \
    --general-log-file=/general.log \
    --log-bin=on \
    --encrypt-binlog=on \
    --innodb-encrypt-log=on \
    --plugin-load-add='file_key_management_chacha=encryption_chacha' \
    --loose-file-key-management-filename=/file-keys.txt \
    --loose-file-key-management-chacha-filename=/file-keys.txt &
tail -f /error.log

# --plugin-load-add='encryption_example_aes=example_encryption_aes_rs'
