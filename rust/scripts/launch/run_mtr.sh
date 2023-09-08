set -eaux

echo running tests

/obj/build-mariadb/mysql-test/mtr \
    --mem \
    --force \
    --max-test-fail=40 \
    "--parallel=$(nproc)"

# --suite=unit
