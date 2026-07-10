--echo Performing --wsrep-recover ...
if ($wsrep_recover_additional)
{
--exec $MYSQLD --defaults-group-suffix=.$galera_wsrep_recover_server_id --defaults-file=$MYSQLTEST_VARDIR/my.cnf --log-error=$MYSQL_TMP_DIR/galera_wsrep_recover.log --innodb --wsrep-recover $wsrep_recover_additional > $MYSQL_TMP_DIR/galera_wsrep_recover.log 2>&1
}
if (!$wsrep_recover_additional)
{
--exec $MYSQLD --defaults-group-suffix=.$galera_wsrep_recover_server_id --defaults-file=$MYSQLTEST_VARDIR/my.cnf --log-error=$MYSQL_TMP_DIR/galera_wsrep_recover.log --innodb --wsrep-recover > $MYSQL_TMP_DIR/galera_wsrep_recover.log 2>&1
}

--perl
        use strict;
	my $file = "$ENV{MYSQL_TMP_DIR}/galera_wsrep_recover.log";
	if (! -e $file) {
          die "$file does not exists.";
        }
        my $wsrep_start_position = `grep -a 'WSREP: Recovered position:' $file | sed 's/.*WSREP\:\ Recovered\ position://' | sed 's/^[ \t]*//'`;
        chomp($wsrep_start_position);
	if ($wsrep_start_position eq '') {
          die "No recovered position found $wsrep_start_position";
        }
        my $path = "$ENV{MYSQL_TMP_DIR}/galera_wsrep_start_position.inc";
        open(FILE, ">", $path) or die "Can't open file: '$path' $!";
        print FILE "--let \$galera_wsrep_start_position = $wsrep_start_position\n" or die "Write to '$path' failed $!";
        close FILE or die "Closing $path failed $!";
EOF

--source $MYSQL_TMP_DIR/galera_wsrep_start_position.inc

if ($galera_wsrep_start_position == '') {
    --die "Could not obtain wsrep_start_position."
}

--remove_file $MYSQL_TMP_DIR/galera_wsrep_start_position.inc
