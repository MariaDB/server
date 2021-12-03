if (!$XTRABACKUP) {
  skip Needs mariabackup;
}

if (!$MTR_GALERA_TFMT) {
  skip Needs socat or nc;
}
