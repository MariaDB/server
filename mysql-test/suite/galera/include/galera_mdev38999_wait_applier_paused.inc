# MDEV-38999 fixture: block until node_2's applier has actually reached the
# pause point set up by galera_mdev38999_pause_applier.inc. Source this on
# node_2a after committing the write that should get stuck.

--connection node_2a
SET DEBUG_SYNC = "now WAIT_FOR sync.wsrep_apply_cb_reached";
