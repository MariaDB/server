# Check that node_1 and node_2 have binlogged the same replicated
# transactions, in wsrep_gtid_domain_id (1): the last GTID, and the whole
# sequence of GTIDs read from the binlog of each node with mariadb-binlog.
# (Not for a node which uses a file binlog: it starts a fresh binlog after a
# mariabackup or rsync SST, without the history from before the SST.)
# Both nodes must be synced.
--source suite/galera/include/galera_binlog_in_engine_check_gtid.inc
--perl
  my @seq;
  for my $port ($ENV{NODE_MYPORT_1}, $ENV{NODE_MYPORT_2}) {
    # Read from the start of the binlog: a GTID position in an unused
    # domain, with non-strict mode.
    my $out= `$ENV{MYSQL_BINLOG} --read-from-remote-server --user=root --host=127.0.0.1 --port=$port --start-position=42-42-42 --gtid-strict-mode=0 master-bin.000001 2>&1`;
    die "mariadb-binlog failed for port $port: $out" if $?;
    push @seq, join(' ', $out =~ /GTID (1-\d+-\d+)/g);
  }
  die "No replicated transactions in the binlog\n" unless $seq[0];
  die "The binlogs differ:\nnode_1: $seq[0]\nnode_2: $seq[1]\n"
    if $seq[0] ne $seq[1];
  print "# The binlogs of both nodes have the same replicated GTIDs\n";
EOF
