disable_query_log;
# Restore the shared `test` database to the server's default character set.
# The tests run under utf8mb4 (set in have_tidesdb.inc); without restoring it
# here every test would leave `test` as utf8mb4 versus the server default,
# which mtr's check-testcase flags (it compares the test database charset).
let $tdb_srv_cs= `SELECT @@global.character_set_server`;
let $tdb_srv_co= `SELECT @@global.collation_server`;
eval ALTER DATABASE test DEFAULT CHARACTER SET $tdb_srv_cs COLLATE $tdb_srv_co;
enable_query_log;
