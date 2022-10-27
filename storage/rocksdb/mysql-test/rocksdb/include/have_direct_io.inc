# Common test pattern for options that control direct i/o
#
# Required input:
#   $io_option - name and assignment to enable on server command line

--perl
use Cwd 'abs_path';

open(FILE, ">", "$ENV{MYSQL_TMP_DIR}/data_in_shm.inc") or die;
my $real_path= abs_path($ENV{'MYSQLTEST_VARDIR'});
my $in_shm= (index($real_path, "/dev/shm") != -1) ||
            (index($real_path, "/run/shm") != -1);
print FILE "let \$DATA_IN_SHM= $in_shm;\n";
close FILE;
EOF

--source $MYSQL_TMP_DIR/data_in_shm.inc
--remove_file $MYSQL_TMP_DIR/data_in_shm.inc

if ($DATA_IN_SHM)
{
  --skip DATADIR is in /{dev|run}/shm, possibly due to --mem
}
