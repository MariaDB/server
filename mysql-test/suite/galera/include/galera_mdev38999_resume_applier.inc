# MDEV-38999 fixture: release node_2's applier paused by
# galera_mdev38999_pause_applier.inc and let it catch up.

--connection node_2a
SET GLOBAL debug_dbug = "";
SET DEBUG_SYNC = "now SIGNAL signal.wsrep_apply_cb";
SET DEBUG_SYNC = "RESET";
