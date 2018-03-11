--echo Performing --wsrep-recover ...
--exec $MYSQLD --defaults-group-suffix=.$galera_wsrep_recover_server_id --defaults-file=$MYSQLTEST_VARDIR/my.cnf --log-error=$MYSQL_TMP_DIR/galera_wsrep_recover.log --innodb --wsrep-recover > $MYSQL_TMP_DIR/galera_wsrep_recover.log 2>&1

--perl
	use strict;
        my $wsrep_start_position_str = "grep 'WSREP: Recovered position:' $ENV{MYSQL_TMP_DIR}/galera_wsrep_recover.log | sed 's/.*WSREP\:\ Recovered\ position://' | sed 's/^[ \t]*//'";
        my $wsrep_start_position = `grep 'WSREP: Recovered position:' $ENV{MYSQL_TMP_DIR}/galera_wsrep_recover.log | sed 's/.*WSREP\:\ Recovered\ position://' | sed 's/^[ \t]*//'`;
        chomp($wsrep_start_position);

        die if $wsrep_start_position eq '';

	open(FILE, ">", "$ENV{MYSQL_TMP_DIR}/galera_wsrep_start_position.inc") or die;
        print FILE "--let \$galera_wsrep_start_position = $wsrep_start_position\n";
        close FILE;
EOF

--source $MYSQL_TMP_DIR/galera_wsrep_start_position.inc

if ($galera_wsrep_start_position == '') {
    --die "Could not obtain wsrep_start_position."
}

--remove_file $MYSQL_TMP_DIR/galera_wsrep_start_position.inc
