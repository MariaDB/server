# This file provides logic to ensure that all tables in a database are the
# same between two connections.
#
# param $check_db : The name of the database to validate all tables are the
#                   same within (test by default)
# param $con1     : The connection name of the primary server, defaults to
#                   master
# param $con2     : The connection name of the replica server, defaults to
#                   slave

--let $include_filename= rpl_check_table_consistency.inc
--source include/begin_include_file.inc

if (!$con1)
{
    --let $con1= master
}
if (!$con2)
{
    --let $con2= slave
}
if (!$check_db)
{
    --let $check_db= test
}

--connection $con2
--let $n_tables= `select count(*) from information_schema.tables WHERE table_schema = '$check_db'`

--echo # Checking consistency of '$check_db' database tables between $con1 and $con2

--connection $con1
--let $c1_n_tables= `select count(*) from information_schema.tables WHERE table_schema = '$check_db'`
if (`SELECT $c1_n_tables != $n_tables`)
{
    die "$con1 had $c1_n_tables tables but $con2 had $n_tables after binlog replay";
}
--echo # Both servers have $n_tables tables

--let $ctr= 1
--echo # Verifying integrity of tables..
while($ctr <= $n_tables)
{
    --let $cksum_tbl= query_get_value(SELECT table_name FROM information_schema.tables WHERE table_schema = 'test' ORDER BY table_name ASC, table_name, $ctr)
    --connection $con1
    --let $c1_cksum= `CHECKSUM TABLE $cksum_tbl`
    --connection $con2
    --let $c2_cksum= `CHECKSUM TABLE $cksum_tbl`

    if ($c1_cksum != $c2_cksum)
    {
        die "Table $cksum_tbl differs between connections $con1 and $con2";
    }
    if ($c1_cksum == $c2_cksum)
    {
        --echo # $cksum_tbl is equivalent on $con1 and $con2
    }
    --let $ctr= `SELECT $ctr+1`
}
--echo # All tables are consistent

--let $include_filename= rpl_check_table_consistency.inc
--source include/end_include_file.inc
