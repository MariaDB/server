#!/usr/bin/env perl

# vim:sw=2:ai

# test for issue #78

BEGIN {
	push @INC, "../common/";
};

use strict;
use warnings;
use hstest;

my $dbh = hstest::init_testdb();
my $table = 'hstesttbl';
my $tablesize = 100;
$dbh->do(
  "create table $table (" .
  "id bigint(20) not null auto_increment, " .
  "t1 timestamp not null default current_timestamp, " .
  "primary key (id)" .
  ") engine = innodb");
srand(999);

my %valmap = ();

my $hs = hstest::get_hs_connection(undef, 9999);
my $dbname = $hstest::conf{dbname};
$hs->open_index(0, $dbname, $table, 'PRIMARY', 'id,t1');
my $res = $hs->execute_single(0, '+', [ 321 ], 0, 0);
die $hs->get_error() if $res->[0] != 0;
print "HS\n";
print join(' ', @$res) . "\n";

