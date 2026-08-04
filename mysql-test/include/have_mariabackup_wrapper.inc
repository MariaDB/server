# Redirect `$XTRABACKUP` so existing test invocations like
#
#     --exec $XTRABACKUP --defaults-file=$MYSQLTEST_VARDIR/my.cnf \
#         --backup --target-dir=$targetdir
#
# run through extra/mariabackup/scripts/mariadb-backup-server.sh — the BACKUP
# SERVER compatibility wrapper — without any change to the test body.
#
#   --source include/have_mariabackup_wrapper.inc
#   # ... rest of the test, using $XTRABACKUP as usual ...
#
#   $XTRABACKUP : now points at mariadb-backup-server.sh

--source include/not_windows.inc

--let MARIABACKUP_WRAPPER=$MYSQL_TEST_DIR/../extra/mariabackup/scripts/mariadb-backup-server.sh
--let MBSTREAM_WRAPPER=$MYSQL_TEST_DIR/../extra/mariabackup/scripts/mbstream-server.sh

# The wrapper shells out to the bare `mariadb` client, which mtr does not put
# on PATH. Prepend the build's client directories so it resolves. A `let` with
# no leading $ is exported to the environment of later --exec commands.
--let PATH=$MYSQL_BINDIR/client:$MYSQL_BINDIR/client_release:$MYSQL_BINDIR/client_debug:$MYSQL_BINDIR/bin:$PATH

--error 0,1
perl;
my $w = $ENV{MARIABACKUP_WRAPPER};
my $m = $ENV{MBSTREAM_WRAPPER};
exit 1 unless $w && -x $w && $m && -x $m;
exit 0;
EOF

if ($errno)
{
  --skip mariadb-backup-server.sh wrapper unavailable (script or sh missing)
}

--let XTRABACKUP=$MARIABACKUP_WRAPPER
--let XBSTREAM=$MBSTREAM_WRAPPER
