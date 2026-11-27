# MDEV-38999 fixture: pause node_2's applier before it applies the next
# write-set it receives. Source this, then commit the write that should get
# stuck (typically on node_1), then source
# galera_mdev38999_wait_applier_paused.inc to block until the applier has
# actually reached the pause point.

--connection node_2
SET GLOBAL debug_dbug = "d,sync.wsrep_apply_cb";
