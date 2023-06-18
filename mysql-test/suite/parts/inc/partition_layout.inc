# inc/partition_layout.inc
#
# Print partitioning related information about the table t1
#

eval SHOW CREATE TABLE t1;

# Optional (most probably issues with separators and case sensitivity)
# listing of files belonging to the table t1
if ($ls)
{
   --replace_result $MARIADB_DATADIR MARIADB_DATADIR #p# #P# #sp# #SP#
   --list_files $MARIADB_DATADIR/test t1*
}
