#!/bin/sh

set -eaux

echo running tests

/obj/build-mariadb/mysql-test/mtr \
    --force \
    --max-test-fail=40 \
    "--parallel=$(nproc)"

# --mem \
# export MTR_BINDIR=/obj/build-mariadb
# mkdir /test
# cd /test

# --suite=unit
