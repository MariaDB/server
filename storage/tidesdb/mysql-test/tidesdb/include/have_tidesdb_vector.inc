# Skip the test unless the server supports the VECTOR data type (MariaDB 11.7+).
# TidesDB itself implements vector indexes, but the SQL-layer VECTOR type does
# not exist on older servers, so CREATE TABLE ... VECTOR fails there.
--disable_query_log
--disable_warnings
--error 0,ER_UNKNOWN_DATA_TYPE,ER_PARSE_ERROR,ER_NOT_SUPPORTED_YET
CREATE TEMPORARY TABLE tdb_vector_probe (v VECTOR(4) NOT NULL);
let $tdb_no_vector= $mysql_errno;
DROP TEMPORARY TABLE IF EXISTS tdb_vector_probe;
--enable_warnings
--enable_query_log
if ($tdb_no_vector)
{
  --skip TidesDB: VECTOR data type not supported (requires MariaDB 11.7+)
}
