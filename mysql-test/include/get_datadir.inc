# Get the catalog or base directory
# The variables set by this script is automatically set by mtr
# for the running mariadbd server.
# This script is only needed if we are connecting to another
# mariadbd server!

# Sets the following variables
# $MARIADB_TOPDIR   The data directory (select @@datadir)
# $MARIADB_DATADIR  The directory of the current catalog
# $USING_CATALOGS   1 if catalogs, 0 if no catalogs
# When mtr --catalogs are used TOPDIR and DATADIR are different

--disable_query_log
let $MARIADB_TOPDIR=`select @@datadir`;
let $USING_CATALOGS=`select @@catalogs`;
if ($USING_CATALOGS == 1)
{
  let $MARIADB_DATADIR=$MARIADB_TOPDIR$MARIADB_CATALOG;
}
if ($USING_CATALOGS == 0)
{
  let $MARIADB_DATADIR=$MARIADB_TOPDIR;
}
--enable_query_log
