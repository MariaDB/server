#
# Macro to run wsrep recovery step. This is adapted from
# suite/galera/include/galera_wsrep_recover.inc, with additional
# option to pass binlog argument to recovery command. The macro
# returns recovered position split in uuid and seqno parts.
#
# Arguments:
#
# wsrep_recover_binlog_opt - Binlog options to recovery command
#
# Return:
#
# wsrep_recover_start_position_uuid - UUID corresponding to recovered position
# wsrep_recover_start_position_seqno - seqno corresponding to recovered position
#

--exec $MYSQLD --defaults-group-suffix=.1 --defaults-file=$MYSQLTEST_VARDIR/my.cnf --log-error=$MYSQL_TMP_DIR/galera_wsrep_recover.log --innodb --wsrep-recover $wsrep_recover_binlog_opt --core-file > $MYSQL_TMP_DIR/galera_wsrep_recover.log 2>&1

--perl
	use strict;
	my $wsrep_start_position = `grep 'WSREP: Recovered position:' $ENV{MYSQL_TMP_DIR}/galera_wsrep_recover.log | sed 's/.*WSREP\:\ Recovered\ position://' | sed 's/^[ \t]*//'`;
	chomp($wsrep_start_position);
        die if $wsrep_start_position eq '';
	open(FILE, ">", "$ENV{MYSQL_TMP_DIR}/galera_wsrep_start_position.inc") or die;
	my ($uuid, $seqno) = split /:/, $wsrep_start_position;
	print FILE "--let \$wsrep_recover_start_position_uuid = $uuid\n";
	print FILE "--let \$wsrep_recover_start_position_seqno = $seqno\n";
	close FILE;
EOF

--source $MYSQL_TMP_DIR/galera_wsrep_start_position.inc

if ($wsrep_recover_start_position_uuid == '') {
   --die "Could not obtain start_position_uuid."
}

if ($wsrep_recover_start_position_seqno == '') {
   --die "Could not obtain start_position_seqno."
}

--remove_file $MYSQL_TMP_DIR/galera_wsrep_start_position.inc
