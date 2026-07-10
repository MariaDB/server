# Skip unless the mariabackup binary was built (WITH_MARIABACKUP).
# This differs from have_mariabackup.inc, which also requires the Galera
# transfer tool (socat or nc).  Tests that source this file copy the backup
# to a local directory instead of streaming it, so that tool is not needed.

if (!$XTRABACKUP) {
  skip Needs mariabackup;
}
