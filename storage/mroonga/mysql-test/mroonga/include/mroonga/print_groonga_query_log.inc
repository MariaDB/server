SHOW GLOBAL VARIABLES LIKE "mroonga_query_log_file";
perl;
open(F, '<', $_="$ENV{MYSQLD_DATADIR}/groonga-query-log.log") or die "open(<$_): $!";
while (<F>) {
  s/^[^|]+\|[^|]+\|[^|]+\| *//;
  print;
}
EOF
