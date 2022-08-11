#!/usr/bin/env perl

# Copyright (C) 2000, 2001 MySQL AB
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
use JSON;
use Data::Dumper;

package main;

$opt_rows=1000000;
$opt_verbose="";
$opt_host="";
$opt_db="test";
$opt_user="test";
$opt_password="";
$opt_socket=undef;
$opt_skip_drop= undef;
$opt_skip_create= undef;
$opt_init_query= undef;
$opt_print_analyze= undef;
$opt_skip_where_check= undef;
$opt_engine=undef;
$opt_comment=undef;

@arguments= @ARGV;

GetOptions("host=s","user=s","password=s", "db=s","rows=i","socket=s",
           "skip-drop",
           "init-query=s","engine=s","comment=s","skip-create",
           "skip-where-check", "print-analyze", "verbose") ||
    die "Aborted";

$Mysql::db_errstr=undef;  # Ignore warnings from these

my ($table, $dbh, $where_cost, $real_where_cost, $perf_ratio);

@engines= ("Aria","InnoDB","MyISAM","heap");

####
####  Start timeing and start test
####

$|= 1;				# Autoflush
if ($opt_verbose)
{
    $opt_print_analyze= 1;
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

$table= "$opt_db.check_costs";

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
     "range scan", "index scan", "eq_join");
$where_tests=3; # Number of where test to be compared with test[0]

if ($opt_engine)
{
    test_engine(0, $opt_engine);
}
else
{
    undef($opt_skip_create);
    for ($i= 0 ; $i <= $#engines; $i++)
    {
        test_engine($i, $engines[$i]);

        if ($i > 0)
        {
            print "\n";
            my $j;
            my $cmp_cost= $res[0][$j]->{'cost'};
            my $cmp_time= $res[0][$j]->{'time'};

            print "Ratios $engines[$i] / $engines[0].  Multiplier should be close to 1.0\n";
            for ($j= 0 ; $j <= $#test_names ; $j++)
            {
                my $cur_cost= $res[$i][$j]->{'cost'};
                my $cur_time= $res[$i][$j]->{'time'};

                printf "%10s  cost: %6.4f  time: %6.4f  cost_correction_multiplier: %6.4f\n",
                    $test_names[$j],
                    $cur_cost / $cmp_cost,
                    $cur_time / $cmp_time,
                    ($cmp_cost * ($cur_time / $cmp_time))/$cur_cost;
            }
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

    setup($opt_init_query);

    if (!defined($opt_skip_create))
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
                 KEY `commitdate` $index_type (`l_commitDATE`,l_discount) )
                 ENGINE= $engine")
            or die $DBI::errstr;

        $dbh->do("insert into $table select
                 seq, seq/10, seq/100, seq, seq, seq, mod(seq,10)*10,
                 0, 'a','b',
                 date_add('2000-01-01', interval seq/500 day),
                 date_add('2000-01-10', interval seq/500 day),
                 date_add('2000-01-20', interval seq/500 day),
                 left(md5(seq),25),
                 if(seq & 1,'mail','ship'),
                 repeat('a',mod(seq,40))
                 from seq_1_to_$opt_rows")
            or die $DBI::errstr;
    }
    else
    {
        $opt_rows= get_row_count();
        die "Table $table is empty. Please run without --skip-create"
            if ($opt_rows == 0);
        print "Reusing old table $table, which has $opt_rows rows\n";
    }

    $where_cost=get_variable("optimizer_where_cost");
    if (defined($where_cost) && !$opt_skip_where_check)
    {
        # Calculate cost of where once. Must be done after table is created
        $real_where_cost= get_where_cost();
        $perf_ratio= $real_where_cost/$where_cost;
        printf "Performance ratio compared to base computer: %6.4f\n", $perf_ratio;
    }
    print "\n";
    if (!$opt_skip_where_check)
    {
        $res[$i][0]= table_scan_without_where();
        $res[$i][1]= table_scan_with_where();
        $res[$i][2]= table_scan_with_where_no_match();
        $res[$i][3]= table_scan_with_complex_where();
    }
    $res[$i][4]= table_scan_without_where_analyze();
    $res[$i][5]= range_scan();
    $res[$i][6]= index_scan();
    $res[$i][7]= eq_ref_join();

    if (!$opt_skip_where_check)
    {
        printf "Variable optimizer_where_cost:  cur: %6.4f  real: %6.4f  prop: %6.4f\n",
            $where_cost, $real_where_cost, $perf_ratio;
      print "Ratio of WHERE costs compared to scan without a WHERE\n";
      for ($j= 1 ; $j <= $where_tests ; $j++)
      {
          print_where_costs($i,$j,0);
      }
    }

    print "\nCost/time ratio for different scans types\n";
    for ($j= $where_tests+1 ; $j <= $#test_names ; $j++)
    {
        print_costs($test_names[$j], $res[$i][$j]);
    }
}


sub print_costs()
{
    my ($name, $cur_res)= @_;

    # Cost without where clause
    my $cur_cost= $cur_res->{'cost'} - $cur_res->{'where_cost'};
    my $cur_time= $cur_res->{'time'};

    printf "%-20.20s  cost: %8.4f  time: %8.4f  cost/time:  %8.4f\n",
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
    my ($sth);

    $sth= $dbh->do("analyze table $table") || die "Got error on 'analyze table: " . $dbh->errstr . "\n";
    $sth= $dbh->do("flush tables") || die "Got error on 'flush tables': " . $dbh->errstr . "\n";
    if (defined($query))
    {
      $sth= $dbh->do("$query") || die "Got error on '$query': " . $dbh->errstr . "\n";
    }

    # Set variables that may interfer with timings
    $query= "set \@\@optimizer_switch='index_condition_pushdown=off'";
    $sth= $dbh->do($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
}

##############################################################################
# Query functions
##############################################################################

# Calculate the cost of the WHERE clause

sub table_scan_without_where()
{
    return run_query("table_scan", "ALL", $opt_rows, "select sum(l_partkey) from $table");
}

sub table_scan_with_where()
{
    return run_query("table_scan", "ALL", $opt_rows, "select sum(l_partkey) from $table where l_commitDate >= '2000-01-01' and l_tax >= 0.0");
}

sub table_scan_with_where_no_match()
{
    return run_query("table_scan", "ALL", $opt_rows, "select sum(l_partkey) from $table where l_commitDate >= '2000-01-01' and l_tax > 0.0 /* NO MATCH */");
}


sub table_scan_with_complex_where()
{
    return run_query("table_scan", "ALL", $opt_rows, "select sum(l_partkey) from $table where l_commitDate >= '2000-01-01' and l_quantity*l_extendedprice-l_discount+l_tax > 0.0");
}


# Calculate the table access times

sub table_scan_without_where_analyze()
{
    return run_query_with_analyze("table_scan", "ALL", $opt_rows, "select sum(l_partkey) from $table");
}

sub range_scan()
{
    return run_query_with_analyze("range_scan", "range", $opt_rows, "select sum(l_orderkey) from $table force index(commitdate) where l_commitDate >= '2000-01-01' and l_tax >= 0.0");
}

sub index_scan()
{
    return run_query_with_analyze("index_scan", "index", $opt_rows, "select sum(l_discount) from $table force index(commitdate) where l_commitDate > '2000-01-01' and l_discount >= 0.0");
}

sub eq_ref_join()
{
    return run_query_with_analyze("eq_ref_join", "eq_ref", 1, "select straight_join count(*) from seq_1_to_1000000,check_costs where seq=l_orderkey");
}

sub get_where_cost()
{
    my ($loop);
    $loop=10000000;
    # Return time in microseconds for one where (= optimizer_where_cost)
    return query_time("select benchmark($loop, l_commitDate >= '2000-01-01' and l_tax >= 0.0) from $table limit 1")/$loop;
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
    my ($name, $type, $expected_rows, $query)= @_;
    my ($start_time,$end_time,$sth,@row,%res,$i, $optimizer_rows, $adjust_cost);
    my ($ms);
    $adjust_cost=1.0;

    print "Timing full query: $query\n";

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

        if ($row->[3] ne $type)
        {
            print "Warning: Wrong scan type: '$row->[3]', expected '$type'\n";
        }
        $optimizer_rows= $row->[8];
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
    printf "%10s  ms: %-10s  cost: %6.4f", $name, $ms, $cost;
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
    my ($name, $type, $expected_rows, $query)= @_;
    my ($start_time,$end_time,$sth,@row,%res,$i, $optimizer_rows);
    my ($adjust_cost, $ms, $first_ms, $analyze, $json, $local_where_cost);

    $adjust_cost=1.0;
    $local_where_cost= $where_cost/1000 * $opt_rows;

    print "Timing table access for query: $query\n";

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

        if ($row->[3] ne $type)
        {
            print "Warning: Wrong scan type: '$row->[3]', expected '$type'\n";
        }
        $optimizer_rows= $row->[8];
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
    $row= $sth->fetchrow_arrayref();
    $sth=0;

    # Run the query through analyze statement

    $sth= $dbh->prepare("analyze format=json $query" ) || die "Got error on 'analzye $query': " . $dbh->errstr . "\n";
    $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";
    $row= $sth->fetchrow_arrayref();
    $analyze= $row->[0];
    $sth=0;

    # Fetch the timings
    $analyze=~ /r_table_time_ms": ([0-9.]*)/;
    $first_ms= $1;

    # Fetch the timing for the last table in the query

    $json= from_json($analyze);
    $ms= $json->{'query_block'}->{'table'}->{'r_table_time_ms'};
    die "Cannot find r_table_time_ms in JSON object from analyze statement\n" if (!defined($ms));

    # If two tables, add the costs for the other table!
    if ($ms != $first_ms)
    {
        $ms= $ms + $first_ms;
        $local_where_cost= $local_where_cost*2;
    }

    if ($opt_print_analyze)
    {
        print "\nanalyze:\n" . $analyze . "\n\n";
        # print "QQ: $analyze\n";
        # print Dumper($json) . "\n";
    }

    # Get last query cost
    $query= "show status like 'last_query_cost'";
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
    $sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";;
    $row= $sth->fetchrow_arrayref();
    $sth=0;
    $cost= $row->[1] * $adjust_cost;

    printf "%10s  ms: %-10s  cost-where: %6.4f   cost: %6.4f",
        $name, $ms, $cost - $local_where_cost, $cost;
    if ($adjust_cost != 1.0)
    {
        printf " (cost was %6.4f)", $row->[1];
    }
    print "\n\n";

    $res{'cost'}= $cost;
    $res{'where_cost'}= $local_where_cost;
    $res{'time'}= $ms;
    return \%res;
}


sub print_totals()
{
    my ($i, $j);
    print "Totals per test\n";
    for ($j= 0 ; $j <= $#test_names; $j++)
    {
        print "$test_names[$j]:\n";
        for ($i= 0 ; $i <= $#engines ; $i++)
        {
            my $cost= $res[$i][$j]->{'cost'};
            my $ms= $res[$i][$j]->{'time'};
            printf "%-8s  ms: %-10.10f  cost: %10.6f\n", $engines[$i], $ms, $cost;
        }
    }
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
    $query= "select count(*) from $table";
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
$sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";;
    $row= $sth->fetchrow_arrayref();
    return $row->[0];
}


sub get_variable()
{
    my ($name)= @_;
    $query= "select @@" . $name;
    $sth= $dbh->prepare($query) || die "Got error on '$query': " . $dbh->errstr . "\n";
$sth->execute || die "Got error on '$query': " . $dbh->errstr . "\n";;
    $row= $sth->fetchrow_arrayref();
    return $row->[0];
}
