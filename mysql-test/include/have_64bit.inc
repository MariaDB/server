disable_query_log;
disable_warnings;
let $VERSION_COMPILE_64BIT=
  `SELECT IF(@@version_compile_machine like '%64%', 1, 0)`;
enable_warnings;
enable_query_log;
if (!$VERSION_COMPILE_64BIT) {
  skip Need a 64 bit machine/binary;
}
