#!/usr/bin/env perl

# Copyright (C) 2022 MariaDB Foundation
# Use is subject to license terms
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

# This is a test that runs queries to meassure if the MariaDB cost calculations
# are reasonable.
#
# The following test are run:
# - Full table scan of a table
# - Range scan of the table
# - Index scan of the table
#
# The output can be used to finetune the optimizer cost variables.
#
# The table in question is a similar to the 'lineitem' table used by DBT3
# it has 16 field and could be regarded as a 'average kind of table'.
# Number of fields and record length places a small role when comparing
# index scan and table scan

##################### Standard benchmark inits ##############################

use DBI;
use Getopt::Long;
use Benchmark ':hireswallclock';

package main;

$opt_rows=1000000;
$opt_test_runs= 2;              # Run each test 2 times and take the average
$opt_verbose="";
$opt_host="";
$opt_db="test";
$opt_user="test";
$opt_password="";
$opt_socket=undef;
$opt_skip_drop= undef;
$opt_skip_create= undef;
$opt_init_query= undef;
$opt_analyze= undef;
$opt_where_check= undef;
$opt_engine=undef;
$opt_comment=undef;
$opt_table_suffix=undef;
$opt_table_name= undef;
$opt_grof= undef;
$opt_all_tests=undef;
$opt_ratios= undef;
$opt_mysql= undef;
$has_force_index=1;

@arguments= @ARGV;

GetOptions("host=s","user=s","password=s", "rows=i","test-runs=i","socket=s",
           "db=s", "table-name=s", "skip-drop","skip-create",
           "init-query=s","engine=s","comment=s",
           "gprof", "one-test=s",
           "mysql", "all-tests", "ratios", "where-check",
           "analyze", "verbose") ||
    die "Aborted";

$Mysql::db_errstr=undef;  # Ignore warnings from these

my ($base_table, $table, $dbh, $where_cost, $real_where_cost, $perf_ratio);

if (!$opt_mysql)
{
    @engines= ("aria","innodb","myisam","heap");
}
else
{
    @engines= ("innodb","myisam","heap");
}

# Special handling for some engines

$no_force= 0;

if (defined($opt_engine))
{
    if (lc($engine) eq "archive")
    {
        $has_force_index= 0;     # Skip tests with force index
    }
}


if (defined($opt_gprof) || defined($opt_one_test))
{
    die "one_test must be defined when --gprof is used"
        if (!defined($opt_one_test));
    die "engine must be defined when --gprof or --one-test is used"
        if (!defined($opt_engine));
    die "function '$opt_one_test' does not exist\n"
        if (!defined(&{$opt_one_test}));
}

# We add engine_name to the table name later

$opt_table_name="check_costs" if (!defined($opt_table_name));
$base_table="$opt_db.$opt_table_name";

####
####  Start timeing and start test
####

$|= 1;				# Autoflush
if ($opt_verbose)
{
    $opt_analyze= 1;
}

####
#### Create the table
####

my %attrib;

$attrib{'PrintError'}=0;

if (defined($opt_socket))
{
    $attrib{'mariadb_socket'}=$opt_socket;
}

$dbh = DBI->connect("DBI:MariaDB:$opt_db:$opt_host",
		    $opt_user, $opt_password,\%attrib) || die $DBI::errstr;

print_mariadb_version();
print "Server options: $opt_comment\n" if (defined($opt_comment));
print "Running tests with $opt_rows rows\n";

print "Program arguments:\n";
for ($i= 0 ; $i <= $#arguments; $i++)
{
    my $arg=$arguments[$i];
    if ($arg =~ / /)
    {
        if ($arg =~ /([^ =]*)=(.*)/)
        {
            print "$1=\"$2\" ";
        }
        else
        {
            print "\"$arg\"" . " ";
        }
    }
    else
    {
        print $arguments[$i] . " ";
    }
}
print "\n\n";

@test_names=
    ("table scan no where", "table scan simple where",
     "table scan where no match", "table scan complex where", "table scan",
     "index scan", "index scan 4 parts", "range scan", "eq_ref_index_join",
     "eq_ref_cluster_join", "eq_ref_join", "eq_ref_btree");
$where_tests=3; # Number of where test to be compared with test[0]

if ($opt_mysql)
{
    create_seq_table();
}


if ($opt_engine || defined($opt_one_test))
{
    test_engine(0, $opt_engine);
}
else
{
    my $i;
    undef($opt_skip_create);
    for ($i= 0 ; $i <= $#engines; $i++)
    {
        test_engine($i, $engines[$i]);

        if ($i > 0 && $opt_ratios)
        {
            print "\n";
            my $j;

            print "Ratios $engines[$i] / $engines[0]\n";
            for ($j= $where_tests+1 ; $j <= $#test_names ; $j++)
            {
                if ($res[$i][$j])
                {
                    my $cmp_cost= $res[0][$j]->{'cost'} - $res[0][$j]->{'where_cost'};
                    my $cmp_time= $res[0][$j]->{'time'};
                    my $cur_cost= $res[$i][$j]->{'cost'} - $res[$i][$j]->{'where_cost'};
                    my $cur_time= $res[$i][$j]->{'time'};

                    printf "%14.14s  cost: %6.4f  time: %6.4f  cost_multiplier: %6.4f\n",
                        $test_names[$j],
                        $cur_cost / $cmp_cost,
                        $cur_time / $cmp_time,
                        ($cmp_cost * ($cur_time / $cmp_time))/$cur_cost;
                }
000000            }
        }
#       if ($i + 1 <= $#engines)
        {
            print "-------------------------\n\n";
        }
    }
    print_totals();
}

$dbh->do("drop table if exists $table") if (!defined($opt_skip_drop));
$dbh->disconnect; $dbh=0;	# Close handler
exit(0);


sub test_engine()
{
    my ($i, $engine)= @_;
    my ($cur_rows);

    setup_engine($engine);
    setup($opt_init_query);
    $table= $base_table . "_$engine";
    if (!defined($opt_skip_create) || !check_if_table_exist($table))
    {
        my $index_type="";

        # We should use btree index with heap to ge range scans
        $index_type= "using btree" if (lc($engine) eq "heap");

        print "Creating table $table of type $engine\n";
        $dbh->do("drop table if exists $table");
        $dbh->do("create table $table (
                 `l_orderkey` int(11) NOT NULL,
                 `l_partkey` int(11) DEFAULT NULL,
                 `l_suppkey` int(11) DEFAULT NULL,
                 `l_linenumber` int(11) NOT NULL,
                 `l_extra` int(11) NOT NULL,
                 `l_quantity` double DEFAULT NULL,
                 `l_extendedprice` double DEFAULT NULL,
                 `l_discount` double DEFAULT NULL,
                 `l_tax` double DEFAULT NULL,
                 `l_returnflag` char(1) DEFAULT NULL,
                 `l_linestatus` char(1) DEFAULT NULL,
                 `l_shipDATE` date DEFAULT NULL,
                 `l_commitDATE` date DEFAULT NULL,
                 `l_receiptDATE` date DEFAULT NULL,
                 `l_shipinstruct` char(25) DEFAULT NULL,
                 `l_shipmode` char(10) DEFAULT NULL,
                 `l_comment` varchar(44) DEFAULT NULL,
                 PRIMARY KEY (`l_orderkey`),
                 UNIQUE (`l_linenumber`),
                 UNIQUE (`l_extra`) $index_type,
                 KEY `l_suppkey` $index_type (l_suppkey, l_partkey),
                 KEY `long_suppkey` $index_type
                     (l_partkey, l_suppkey, l_linenumber, l_extra) )
                 ENGINE= $engine")
            or die "Got error on CREATE TABLE: $DBI::errstr";
    }
    $cur_rows= get_row_count($table);
    if ($cur_rows == 0 || !defined($opt_skip_create))
    {
        $dbh->do("insert into $table select
                 seq, seq/10, seq, seq, seq, seq, seq, mod(seq,10)*10,
                 0, 'a','b',
                 date_add('2000-01-01', interval seq/500 day),
                 date_add('2000-01-10', interval seq/500 day),
                 date_add('2000-01-20', interval seq/500 day),
                 left(md5(seq),25),
                 if(seq & 1,'mail','ship'),
                 repeat('a',mod(seq,40))
                 from seq_1_to_$opt_rows")
            or die "Got error on INSERT: $DBI::errstr";

        $sth= $dbh->do("analyze table $table")
            or die "Got error on 'analyze table: " . $dbh->errstr . "\n";
    }
    else
    {
        $opt_rows= $cur_rows;
        die "Table $table is empty. Please run without --skip-create"
            if ($opt_rows == 0);
        print "Reusing old table $table, which has $opt_rows rows\n";
    }

    if (!$opt_mysql)
    {
        $where_cost=get_variable("optimizer_where_cost");
        if (defined($where_cost))
        {
            # Calculate cost of where once. Must be done after table is created
            $real_where_cost= get_where_cost();
            $perf_ratio= $real_where_cost/$where_cost;
            printf "Performance ratio compared to base computer: %6.4f\n",
                $perf_ratio;
        }
        print "\n";
    }
    else
    {
        $where_cost=0.1;        # mysql 'm_row_evaluate_cost'
    }


    if (defined($opt_one_test))
    {
        if (defined($opt_gprof))
        {
            # Argument is the name of the test function
            test_with_gprof($opt_one_test, 10);
            return;
        }
        $opt_one_test->();
        return;
    }

    if ($opt_where_check)
    {
        $res[$i][0]= table_scan_without_where(0);
        $res[$i][1]= table_scan_with_where(1);
        $res[$i][2]= table_scan_with_where_no_match(2);
        $res[$i][3]= table_scan_with_complex_where(3);
    }
    $res[$i][4]=  table_scan_without_where_analyze(4);
    $res[$i][5]=  index_scan(5);
    $res[$i][6]=  index_scan_4_parts(6)  if ($opt_all_tests);
    $res[$i][7]=  range_scan(7);
    $res[$i][8]=  eq_ref_index_join(8);
    $res[$i][9]=  eq_ref_clustered_join(9);
    $res[$i][10]= eq_ref_join(10);
    $res[$i][11]= eq_ref_join_btree(11);

    if ($opt_where_check)
    {
        printf "Variable optimizer_where_cost:  cur: %6.4f  real: %6.4f  prop: %6.4f\n",
            $where_cost, $real_where_cost, $perf_ratio;
        print "Ratio of WHERE costs compared to scan without a WHERE\n";
        for ($j= 1 ; $j <= $where_tests ; $j++)
        {
            print_where_costs($i,$j,0);
        }
        print "\n";
    }

    print "Cost/time ratio for different scans types\n";
    for ($j= $where_tests+1 ; $j <= $#test_names ; $j++)
    {
        if ($res[$i][$j])
        {
            print_costs($test_names[$j], $res[$i][$j]);
        }
    }
}


sub print_costs($;$)
{
    my ($name, $cur_res)= @_;

    # Cost without where clause
    my $cur_cost= $cur_res->{'cost'} - $cur_res->{'where_cost'};
    my $cur_time= $cur_res->{'time'};

    printf "%-20.20s  cost: %9.4f  time: %9.4f  cost/time:  %8.4f\n",
        $name,
        $cur_cost, $cur_time, $cur_cost/$cur_time;
}

sub print_where_costs()
{
    my ($index, $cmp, $base)= @_;

    my $cmp_time= $res[$index][$cmp]->{'time'};
    my $base_time= $res[$index][$base]->{'time'};

    printf "%-30.30s time: %6.4f\n", $test_names[$cmp], $cmp_time / $base_time;
}


# Used to setup things like optimizer_switch or optimizer_cache_hit_ratio

sub setup()
{
    my ($query)= @_;
    my ($sth,$query);

    $sth= $dbh->do("flush tables") ||
        die "Got error on 'flush tables': " . $dbh->errstr . "\n";
    if (defined($query))
    {
      $sth= $dbh->do("$query") ||
          die "Got error on '$query': " . $dbh->errstr . "\n";
    }

    # Set variables that may interfer with timings
    $query= "set \@\@optimizer_switch='index_condition_pushdown=off'";
    $sth= $dbh->do($query) ||
        die "Got error on '$query': " . $dbh->errstr . "\n";
}


sub setup_engine()
{
    my ($engine)= @_;
    my ($sth,$query);

    if (!$opt_mysql)
    {
        # Set variables that may interfere with timings
        $query= "set global $engine.optimizer_disk_read_ratio=0";
        $sth= $dbh->do($query) ||
            die "Got error on '$query': " . $dbh->errstr . "\n";
    }
}

sub create_seq_table
{
    my $name= "seq_1_to_$opt_rows";
    my $i;
    print "Creating $name\n";
    $dbh->do("drop table if exists $name") ||
        die "Error on drop: " . $dbh->errstr ."\n";
    $dbh->do("create table $name (seq int(11) not null) engine=heap")
        || die "Error on create: " . $dbh->errstr ."\n";
    for ($i= 1 ; $i < $opt_rows ; $i+=10)
    {
        $dbh->do("insert into $name values
                 ($i),($i+1),($i+2),($i+3),($i+4),($i+5),($i+6),($i+7),($i+8),($i+9)") || die "Error on insert";
    }
}



##############################################################################
# Query functions
##############################################################################

# Calculate the cost of the WHERE clause

sub table_scan_without_where()
{
    my ($query_id)= @_;
    return run_query($test_names[$query_id],
                     "table_scan", "ALL", $opt_rows,
"select sum(l_quantity) from $table");
}

sub table_scan_with_where()
{
    my ($query_id)= @_;
    return run_query($test_names[$query_id],
                     "table_scan", "ALL", $opt_rows,
"select sum(l_quantity) from $table where l_commitDate >= '2000-01-01' and l_tax >= 0.0");
}

sub table_scan_with_where_no_match()
{
    my ($query_id)= @_;
    return run_query($test_names[$query_id],
                     "table_scan", "ALL", $opt_rows,
"select sum(l_quantity) from $table where l_commitDate >= '2000-01-01' and l_tax > 0.0 /* NO MATCH */");
}


sub table_scan_with_complex_where()
{
    my ($query_id)= @_;
    return run_query($test_names[$query_id],
                     "table_scan", "ALL", $opt_rows,
"select sum(l_quantity) from $table where l_commitDate >= '2000-01-01' and l_quantity*l_extendedprice-l_discount+l_tax > 0.0");
}

# Calculate the time spent for table accesses (done with analyze statement)

# Table scan

sub table_scan_without_where_analyze()
{
    my ($query_id)= @_;
    return run_query_with_analyze($test_names[$query_id],
                                  "table_scan", "ALL", $opt_rows,
"select sum(l_quantity) from $table");
}

# Index scan with 2 key parts

sub index_scan()
{
    my ($query_id)= @_;
    return 0 if (!$has_force_index);
    my ($query_id)= @_;
    return run_query_with_analyze($test_names[$query_id],
                                  "index_scan", "index", $opt_rows,
"select count(*) from $table force index (l_suppkey) where l_suppkey >= 0 and l_partkey >=0");
}

# Index scan with 2 key parts
# This is to check how the number of key parts affects the timeings

sub index_scan_4_parts()
{
    my ($query_id)= @_;
    return 0 if (!$has_force_index);
    return run_query_with_analyze($test_names[$query_id],
                                  "index_scan_4_parts", "index", $opt_rows,
"select count(*) from $table force index (long_suppkey) where l_linenumber >= 0 and l_extra >0");
}

sub range_scan()
{
    my ($query_id)= @_;
    return 0 if (!$has_force_index);
    return run_query_with_analyze($test_names[$query_id],
                                  "range_scan", "range", $opt_rows,
"select sum(l_orderkey) from $table force index(l_suppkey) where l_suppkey >= 0 and l_partkey >=0 and l_discount>=0.0");
}

sub eq_ref_index_join()
{
    my ($query_id)= @_;
    return run_query_with_analyze($test_names[$query_id],
                                  "eq_ref_index_join", "eq_ref", 1,
"select straight_join count(*) from seq_1_to_$opt_rows,$table where seq=l_linenumber");
}

sub eq_ref_clustered_join()
{
    my ($query_id)= @_;
    return run_query_with_analyze($test_names[$query_id],
                                  "eq_ref_cluster_join", "eq_ref", 1,
"select straight_join count(*) from seq_1_to_$opt_rows,$table where seq=l_orderkey");
}

sub eq_ref_join()
{
    my ($query_id)= @_;
    return run_query_with_analyze($test_names[$query_id],
                                  "eq_ref_join", "eq_ref", 1,
"select straight_join count(*) from seq_1_to_$opt_rows,$table where seq=l_linenumber and l_partkey >= 0");
}

sub eq_ref_join_btree()
{
    my ($query_id)= @_;
    return run_query_with_analyze($test_names[$query_id],
                                  "eq_ref_btree", "eq_ref", 1,
"select straight_join count(*) from seq_1_to_$opt_rows,$table where seq=l_extra and l_partkey >= 0");
}


# Calculate the cost of a basic where clause
# This can be used to find out the speed of the current computer compared
# to the reference computer on which the costs where calibrated.

sub get_where_cost()
{
    my ($loop);
    $loop=10000000;
    # Return time in microseconds for one where (= optimizer_where_cost)
    return query_time("select benchmark($loop, l_commitDate >= '2000-01-01' and l_tax >= 0.0) from $table limit 1")/$loop;
}


# Run a query to be able to calculate the costs of filter

sub cost_of_filtering()
{
    my ($query, $cost1, $cost2);
    do_query("set \@\@max_rowid_filter_size=10000000," .
              "optimizer_switch='rowid_filter=on',".
              "\@\@optimizer_scan_setup_cost=1000000");
    do_query("set \@old_cost=\@\@aria.OPTIMIZER_ROW_LOOKUP_COST");
    do_query("set global aria.OPTIMIZER_ROW_LOOKUP_COST=1");
    do_query("flush tables");
    $cost1= run_query_with_analyze("range", "range", "range", 500000,
                           "select count(l_discount) from check_costs_aria as t1 where t1.l_orderkey between 1 and 500000");
    $cost2= run_query_with_analyze("range-all", "range-all", "range|filter", 500000,
                           "select count(l_discount) from check_costs_aria as t1 where t1.l_orderkey between 1 and 500000 and l_linenumber between 1 and 500000");
    $cost3= run_query_with_analyze("range-none","range-none", "range|filter", 500000,
                           "select count(l_discount) from check_costs_aria as t1 where t1.l_orderkey between 1 and 500000 and l_linenumber between 500000 and 1000000");
    do_query("set global aria.OPTIMIZER_ROW_LOOKUP_COST=\@old_cost");
    do_query("flush tables");
    print_costs("range", $cost1);
    print_costs("filter-all",  $cost2);
    print_costs("filter-none", $cost3);
}

sub gprof_cost_of_filtering()
{
    $cost2= run_query_with_analyze("gprof","range-all", "range|filter", 500000,
                           "select count(l_discount) from check_costs_aria as t1 where t1.l_orderkey between 1 and 500000 and l_linenumber between 1 and 500000");
}


###############################################################################
# Help functions for running the queries
###############################################################################


# Run query and return time for query in microseconds

sub query_time()
{
    my ($query)= @_;
    my ($start_time,$end_time,$time,$ms,$sth,$row);

    $start_time= new Benchmark;
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
    $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";
    $end_time=new Benchmark;
    $row= $sth->fetchrow_arrayref();
    $sth=0;

    $time= timestr(timediff($end_time, $start_time),"nop");
    $time =~ /([\d.]*)/;
    return $1*1000000.0;
}

#
# Run a query and compare the clock time
#

sub run_query()
{
    my ($full_name, $name, $type, $expected_rows, $query)= @_;
    my ($start_time,$end_time,$sth,@row,%res,$i,$optimizer_rows);
    my ($extra, $last_type, $adjust_cost, $ms);
    $adjust_cost=1.0;

    print "Timing full query: $full_name\n$query\n";

    $sth= $dbh->prepare("explain $query") || die "Got error on 'explain $query': " . $dbh->errstr . "\n";
    $sth->execute || die "Got error on 'explain $query': " . $dbh->errstr . "\n";

    print "explain:\n";
    while ($row= $sth->fetchrow_arrayref())
    {
        print $row->[0];
        for ($i= 1 ; $i < @$row; $i++)
        {
            print "  " .  $row->[$i] if (defined($row->[$i]));
        }
        print "\n";

        $extra= $row->[@$row-1];
        $last_type= $row->[3];
        $optimizer_rows= $row->[8];
    }
    if ($last_type ne $type &&
        ($type ne "index" || !($extra =~ /Using index/)))
    {
        print "Warning: Wrong scan type: '$last_type', expected '$type'\n";
    }

    if ($expected_rows >= 0 &&
        (abs($optimizer_rows - $expected_rows)/$expected_rows) > 0.1)
    {
        printf "Warning: Expected $expected_rows instead of $optimizer_rows from EXPLAIN. Adjusting costs\n";
        $adjust_cost= $expected_rows / $optimizer_rows;
    }

    # Do one query to fill the cache
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
    $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";
    $end_time=new Benchmark;
    $row= $sth->fetchrow_arrayref();
    $sth=0;

    # Run query for real
    $start_time= new Benchmark;
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
    $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";
    $end_time=new Benchmark;
    $row= $sth->fetchrow_arrayref();
    $sth=0;

    $time= timestr(timediff($end_time, $start_time),"nop");
    $time =~ /([\d.]*)/;
    $ms= $1*1000.0;

    $query= "show status like 'last_query_cost'";
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
    $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";;
    $row= $sth->fetchrow_arrayref();
    $sth=0;
    $cost= $row->[1] * $adjust_cost;
    printf "%10s  time: %10.10s ms  cost: %6.4f", $name, $ms, $cost;
    if ($adjust_cost != 1.0)
    {
        printf " (was %6.4f)", $row->[1];
    }
    print "\n\n";

    $res{'cost'}= $cost;
    $res{'time'}= $ms;
    return \%res;
}

#
# Run a query and compare the table access time from analyze statement
# The cost works for queries with one or two tables!
#

sub run_query_with_analyze()
{
    my ($full_name,$name, $type, $expected_rows, $query)= @_;
    my ($start_time,$end_time,$sth,@row,%res,$i,$j);
    my ($optimizer_rows, $optimizer_rows_first);
    my ($adjust_cost, $ms, $second_ms, $analyze, $local_where_cost);
    my ($extra, $last_type, $tot_ms, $found_two_tables);

    $found_two_tables= 0;
    $adjust_cost=1.0;
    if (!$opt_mysql)
    {
        $local_where_cost= $where_cost/1000 * $opt_rows;
    }
    else
    {
        $local_where_cost= $where_cost * $opt_rows;
    }
    $optimizer_rows_first= undef;

    print "Timing table access for query: $full_name\n$query\n";

    $sth= $dbh->prepare("explain $query") || die "Got error on 'explain $query': " . $dbh->errstr . "\n";
    $sth->execute || die "Got error on 'explain $query': " . $dbh->errstr . "\n";

    print "explain:\n";
    if (!$opt_mysql)
    {
        $type_pos= 3;
        $row_pos= 8;
    }
    else
    {
        $type_pos= 4;
        $row_pos= 9;
    }

    $j= 0;
    while ($row= $sth->fetchrow_arrayref())
    {
        $j++;
        print $row->[0];
        for ($i= 1 ; $i < @$row; $i++)
        {
            print "  " .  $row->[$i] if (defined($row->[$i]));
            # print "  X" if (!defined($row->[$i]));
        }
        print "\n";

        $extra= $row->[@$row-1];
        $last_type= $row->[$type_pos];
        if (!defined($optimizer_rows_first))
        {
            $optimizer_rows_first= $row->[$row_pos];
        }
        $optimizer_rows= $row->[$row_pos];
    }
    $found_two_tables= 1 if ($j > 1);

    if ($last_type ne $type &&
        ($type ne "index" || !($extra =~ /Using index/)))
    {
        print "Warning: Wrong scan type: '$last_type', expected '$type'\n";
    }
    if ($expected_rows >= 0 &&
        (abs($optimizer_rows - $expected_rows)/$expected_rows) > 0.1)
    {
        printf "Warning: Expected $expected_rows instead of $optimizer_rows from EXPLAIN. Adjusting costs\n";
        $adjust_cost= $expected_rows / $optimizer_rows;
    }

    # Do one query to fill the cache
    if (!defined($opt_grof))
    {
        $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
        $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";
        $row= $sth->fetchrow_arrayref();
        $sth=0;
    }

    # Run the query through analyze statement
    $tot_ms=0;
    if (!$opt_mysql)
    {
    for ($i=0 ; $i < $opt_test_runs ; $i++)
    {
        my ($j);
        $sth= $dbh->prepare("analyze format=json $query" ) || die "Got error on 'analzye $query': " . $dbh->errstr . "\n";
        $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";
        $row= $sth->fetchrow_arrayref();
        $analyze= $row->[0];
        $sth=0;

        # Fetch the timings
        $j=0;
        while ($analyze =~ /r_table_time_ms": ([0-9.]*)/g)
        {
            $tot_ms= $tot_ms+ $1;
            $j++;
        }
        if ($j > 2)
        {
            die "Found too many tables, program needs to be extended!"
        }
        # Add cost of filtering
        while ($analyze =~ /r_filling_time_ms": ([0-9.]*)/g)
        {
            $tot_ms= $tot_ms+ $1;
        }
    }
    }
    else
    {
        my $local_table= substr($table,index($table,".")+1);
        for ($i=0 ; $i < $opt_test_runs ; $i++)
        {
            my ($j);
            $sth= $dbh->prepare("explain analyze $query" ) || die "Got error on 'analzye $query': " . $dbh->errstr . "\n";
            $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";
            $row= $sth->fetchrow_arrayref();
            $analyze= $row->[0];
            $sth=0;
        }
        # Fetch the timings
        $j=0;

        if ($analyze =~ / $local_table .*actual time=([0-9.]*) .*loops=([0-9]*)/g)
        {
            my $times= $1;
            my $loops= $2;
            $times =~ /\.\.([0-9.]*)/;
            $times= $1;
            $times="0.005" if ($times == 0);
            #print "time: $times  \$1: $1  loops: $loops\n";
            $tot_ms= $tot_ms+ $times*$loops;
            $j++;
        }
        if ($j > 1)
        {
            die "Found too many tables, program needs to be extended!"
        }
    }


    if ($found_two_tables)
    {
        # Add the cost of the where for the two tables. The last table
        # is assumed to have $expected_rows while the first (driving table)
        # may have less rows. Take that into account when calculating the
        # total where cost.
        $local_where_cost= ($local_where_cost +
                            $local_where_cost *
                            ($optimizer_rows_first/$opt_rows));
    }
    $ms= $tot_ms/$opt_test_runs;

    if ($opt_analyze)
    {
        print "\nanalyze:\n" . $analyze . "\n\n";
    }

    if (!defined($opt_grof))
    {
        # Get last query cost
        $query= "show status like 'last_query_cost'";
        $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
        $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";;
        $row= $sth->fetchrow_arrayref();
        $sth=0;
        $cost= $row->[1] * $adjust_cost;

        printf "%10s  time: %10.10s ms  cost-where: %6.4f   cost: %6.4f",
            $name, $ms, $cost - $local_where_cost, $cost;
        if ($adjust_cost != 1.0)
        {
            printf " (cost was %6.4f)", $row->[1];
        }
    }
    else
    {
        printf "%10s  time: %10.10s ms", $name, $ms;
        $cost= 0; $local_where_cost= 0;
    }
    print "\n\n";

    $res{'cost'}= $cost;
    $res{'where_cost'}= $local_where_cost;
    $res{'time'}= $ms;
    return \%res;
}


sub do_query()
{
    my ($query)= @_;
    $dbh->do($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
}


sub print_totals()
{
    my ($i, $j);
    print "Totals per test\n";
    for ($j= $where_tests+1 ; $j <= $#test_names; $j++)
    {
        print "$test_names[$j]:\n";
        for ($i= $0 ; $i <= $#engines ; $i++)
        {
            if ($res[$i][$j])
            {
                my $cost= $res[$i][$j]->{'cost'} - $res[$i][$j]->{'where_cost'};
                my $ms= $res[$i][$j]->{'time'};
                printf "%-8s  %10.4f ms  cost: %10.4f  cost/time: %8.4f\n",
                    $engines[$i], $ms, $cost, $cost/$ms;
            }
        }
    }
}


# This function can be used to test things with gprof

sub test_with_gprof()
{
    my ($function_ref, $loops)= @_;
    my ($sum, $i, $cost);

    printf "Running test $function_ref $loops time\n";
    $sum= 0; $loops=10;
    for ($i=0 ; $i < $loops ; $i++)
    {
        $cost= $function_ref->();
        $sum+= $cost->{'time'};
    }
    print "Average: " . ($sum/$loops) . "\n";
    print "Shuting down server\n";
    $dbh->do("shutdown") || die "Got error ..";
}

##############################################################################
# Get various simple data from MariaDB
##############################################################################

sub print_mariadb_version()
{
    my ($query, $sth, $row);
    $query= "select VERSION()";
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
$sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";;
    $row= $sth->fetchrow_arrayref();
    print "Server: $row->[0]";

    $query= "show variables like 'VERSION_SOURCE_REVISION'";
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
$sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";;
    $row= $sth->fetchrow_arrayref();
    print "  Commit: $row->[1]\n";
}


sub get_row_count()
{
    my ($table)= @_;
    my ($query, $sth, $row);
    $query= "select count(*) from $table";
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
    if (!$sth->execute)
    {
        if (!($dbh->errstr =~ /doesn.*exist/))
        {
            die "Got error on '$query': " . $dbh->errstr . "\n";
        }
        return 0;
    }
    $row= $sth->fetchrow_arrayref();
    return $row->[0];
}


sub get_variable()
{
    my ($name)= @_;
    my ($query, $sth, $row);
    $query= "select @@" . $name;
    if (!($sth= $dbh->prepare($query)))
    {
        die "Got error on '$query': " . $dbh->errstr . "\n";
    }
    $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";;
    $row= $sth->fetchrow_arrayref();
    return $row->[0];
}


sub check_if_table_exist()
{
    my ($name)= @_;
    my ($query,$sth);
    $query= "select 1 from $name limit 1";
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
    print $sth->fetchrow_arrayref();
    if (!$sth->execute || !defined($sth->fetchrow_arrayref()))
    {
        return 0;               # Table does not exists
    }
    return 1;
}
