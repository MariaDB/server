# Drive one shell-injection rejection check for a single SST directory parameter
# that is validated by the SST script (safe_path).
#
# Caller sets before sourcing (and the cluster must be healthy, rsync SST):
#   $inj_param  - mysqld option name, clean text (e.g. innodb-log-group-home-dir)
#   $inj_reject - log pattern in mysqld.2.err that proves the value was rejected
#
# Points $inj_param at a directory whose name contains a shell metacharacter and
# forces a full SST on node 2, so the restarted joiner runs the value through the
# SST script; asserts node 2 refuses it (logs $inj_reject). For
# innodb-data-home-dir the system tablespace (ibdata1) is moved into that dir so
# mysqld can still start. Restores the config and rejoins the cluster.

--connection node_2
--source include/shutdown_mysqld.inc

# pass the (clean) option name to perl via a sidecar file
--exec echo "$inj_param" > $MYSQL_TMP_DIR/inj_param

perl;
    use strict;
    use File::Path;
    my $tmp    = $ENV{MYSQL_TMP_DIR};
    my $vardir = $ENV{MYSQLTEST_VARDIR};
    # read the option name from the sidecar (perl can't see mysqltest vars)
    open(my $pf, '<', "$tmp/inj_param") or die $!;
    my $param = <$pf>; chomp $param; close $pf;
    my $ddir = "$tmp/pinj'x";                 # name carries a shell metachar (')
    my $cnf  = "$vardir/my.cnf";
    mkpath($ddir);                            # must exist for mysqld to start
    # innodb-data-home-dir holds the system tablespace: move ibdata1 in so the
    # joiner can still start and reach the SST
    rename("$vardir/mysqld.2/data/ibdata1", "$ddir/ibdata1")
        if $param eq 'innodb-data-home-dir';
    # remember my.cnf size so cleanup can truncate the appended block away
    open(my $sz, '>', "$tmp/inj_cnf_size") or die $!; print $sz -s $cnf; close $sz;
    # point the option at the malicious dir (perl writes it, so no shell parses the ')
    open(my $fh, '>>', $cnf) or die $!;
    print $fh "[mysqld.2]\n$param=\"$ddir\"\n";
    close $fh;
    unlink "$vardir/mysqld.2/data/grastate.dat";   # force a full SST
EOF

# start the joiner; it must refuse SST and exit
--connection node_2
--error 1,134
--exec $MYSQLD_LAST_CMD

# joiner must have rejected the value
--let SEARCH_FILE = $MYSQLTEST_VARDIR/log/mysqld.2.err
--let SEARCH_PATTERN = $inj_reject
--source include/search_pattern_in_file.inc

# cleanup: restore my.cnf, ibdata1 and the dir, rejoin. No stray helpers to kill
# - safe_path rejects in create_dirs, before the rsync daemon is ever started.
perl;
    use strict;
    use File::Path qw(rmtree);
    my $tmp    = $ENV{MYSQL_TMP_DIR};
    my $vardir = $ENV{MYSQLTEST_VARDIR};
    my $cnf    = "$vardir/my.cnf";
    my $ddir   = "$tmp/pinj'x";
    open(my $pf, '<', "$tmp/inj_param") or die $!;
    my $param = <$pf>; chomp $param; close $pf;
    open(my $sz, '<', "$tmp/inj_cnf_size") or die $!;
    my $orig = <$sz>; close $sz;
    truncate($cnf, $orig) or die "truncate $cnf: $!";
    rename("$ddir/ibdata1", "$vardir/mysqld.2/data/ibdata1")
        if $param eq 'innodb-data-home-dir';
    rmtree($ddir);
    unlink "$tmp/inj_param", "$tmp/inj_cnf_size";
EOF

--connection node_2
--source include/start_mysqld.inc

--let $wait_condition = SELECT VARIABLE_VALUE = 2 FROM INFORMATION_SCHEMA.GLOBAL_STATUS WHERE VARIABLE_NAME = 'wsrep_cluster_size'
--source include/wait_condition.inc
