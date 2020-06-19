#!/usr/bin/env perl

# Copyright (C) 2000 MySQL AB
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

# This is a test with uses two processes to a database.
# The other inserts records in two tables, the other does a lot of joins
# on these.
# Every time the read thread outputs info, it does a ALTER TABLE command
# which should stop the insert thread until the ALTER TABLE command is ready.
#
# Warning, the output from this test will differ in 'found' from time to time,
# but there should never be any errors
#

$host = shift || "";
$test_db="test";
$test_count=10000;
srand 0;			# Repeatable test

use Mysql;
$|= 1;				# Autoflush

$dbh = Mysql->Connect($host) || die "Can't connect: $Mysql::db_errstr\n";
$dbh->SelectDB($test_db) || die "Can't use database $test_db: $Mysql::db_errstr\n";

$firsttable  = "test_lock_1";
$secondtable = "test_lock_2";
$dbh->Query("drop table $firsttable");
$dbh->Query("drop table $secondtable");

print "Creating tables $firsttable and $secondtable in database $test_db\n";
$dbh->Query("create table $firsttable (id int(6) not null, info char(32), auto int(11) not null auto_increment, primary key(id),key(auto))") or die $Mysql::db_errstr;

$dbh->Query("create table $secondtable (id int(6) not null, info varchar(32), key(id))") or die $Mysql::db_errstr;

$dbh=0;				# Close handler

if (fork() == 0)
{				# Insert process
  $dbh = Mysql->Connect($host) || die "Can't connect: $Mysql::db_errstr\n";
  $dbh->SelectDB($test_db) || die "Can't use database $test_db: $Mysql::db_errstr\n";
  $first_id=1; $second_id=1;
  $first_count=$second_count=0;
  print "Writing started\n";
  for ($i=1 ; $i <= $test_count ; $i++)
  {
    if (rand(3) <= 1)
    {
      $sth=$dbh->Query("insert into $firsttable values ($first_id,'This is entry $i',NULL)") || die "Got error on insert: $Mysql::db_errstr\n";
      die "Row not inserted, aborting\n" if ($sth->affected_rows != 1);
      $first_id++;
      $first_count++;
    }
    else
    {
      $sth=$dbh->Query("insert into $secondtable values ($second_id,'This is entry $i')") || die "Got error on insert: $Mysql::db_errstr\n";
      die "Row not inserted, aborting\n" if ($sth->affected_rows != 1);
      $second_id++ if (rand(10) <= 1); # Don't always count it up
      $second_count++;
    }
    print "Write: $i\n" if ($i % 1000 == 0);
  }
  print "Writing done ($first_count $second_count)\n";
}
else
{
  $dbh = Mysql->Connect($host) || die "Can't connect: $Mysql::db_errstr\n";
  $dbh->SelectDB($test_db) || die "Can't use database $test_db: $Mysql::db_errstr\n";
  $locked=$found=0;
  print "Reading started\n";
  for ($i=1 ; $i <= $test_count ; $i++)
  {
    $id=int(rand($test_count)/3)+1;
    $sth=$dbh->Query("select count(*) from $firsttable,$secondtable where $firsttable.id = $secondtable.id and $firsttable.id=$id") || die "Got error on select: $Mysql::db_errstr\n";
    $found++ if ($sth->numrows);
    if ($i % 1000 == 0)
    {
      print "Read:  $i  Found: $found\n";
      if ($found)
      {
	$locked=1-$locked;
	if ($locked)
	{
	  $sth=$dbh->Query("lock tables $firsttable write,$secondtable write");
	}
	$sth=$dbh->Query("alter table $firsttable CHANGE id id int(6) not null") || die "Got error on ALTER TABLE: $Mysql::db_errstr\n";
	$sth=$dbh->Query("alter table $secondtable CHANGE info info char(32) not null") || die "Got error on ALTER TABLE: $Mysql::db_errstr\n";
	if ($locked)
	{
	  $sth=$dbh->Query("unlock tables");
	}
      }
    }
  }
  print "Reading done  Found: $found\n";
}
