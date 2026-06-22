#################################################
# Checks if mariadbd-safe (mysqld_safe) is present
# in the build scripts directory.
#################################################

--disable_query_log
--let $temp_inc=$MYSQL_TMP_DIR/tmp.inc
let inc_tmp=$temp_inc;

--perl
use strict;
use File::Basename;

my $mysqld = $ENV{'MYSQLD'} or die "MYSQLD not set";
my $bindir = dirname($mysqld);
# mariadbd-safe lives in scripts/ next to sql/ where mariadbd is
my $scriptsdir = "$bindir/../scripts";
my $found = 0;
$found = 1 if -e "$scriptsdir/mariadbd-safe" || -e "$scriptsdir/mysqld_safe";

my $temp_inc = $ENV{'inc_tmp'} or die "temp_inc not set";
open(FILE_INC, ">", "$temp_inc") or die("can't open file \"$temp_inc\": $!");
print FILE_INC '--let $is_found= ' . $found . "\n";
if ($found) {
  my $safe = -e "$scriptsdir/mariadbd-safe" ? "$scriptsdir/mariadbd-safe" : "$scriptsdir/mysqld_safe";
  print FILE_INC '--let $MYSQLD_SAFE= ' . $safe . "\n";
}
close(FILE_INC);
EOF

--source $temp_inc

if (!$is_found)
{
  --die Test requires mariadbd-safe (not found)
}

--remove_file $temp_inc
--enable_query_log
