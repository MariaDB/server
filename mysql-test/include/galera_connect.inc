# galera_connect.inc
# ==================
#
# Description
# -----------
# Open a connection to the specified server number ($galera_server_number).
# The connection itself would be identified by $galera_connection_name.
#
# Parameters
# ----------
# $galera_connection_name
#   Name of the resulting connection.
#
# $galera_connection_address
#   IP to connect to for galera (defaults to 127.0.0.1)
#
# $galera_server_number
#   Sequence number of the node in the galera cluster.
#
# $galera_debug
#   Print debug information.
#
# $galera_no_wait_ready
#   Skip the post-connect wait for the node to report ready. Set this
#   when connecting to a node that is expected to join a non-Primary
#   or otherwise not-yet-ready component (the wait would just run out
#   its timeout). One-shot: cleared at the end of this include.
#

if (!$galera_connection_name)
{
  --die ERROR IN TEST: $galera_connection_name must be set before sourcing include/galera_connect.inc
}

if (!$galera_server_number)
{
  --die ERROR IN TEST: $galera_server_number must be set before sourcing include/galera_connect.inc
}

if (!$galera_connection_address)
{
  --let $galera_connection_address= 127.0.0.1
}

--let $_galera_port= \$NODE_MYPORT_$galera_server_number
if (!$_galera_port)
{
  --echo Bug in test case: '\$NODE_MYPORT_$galera_server_number' not initialized. Check the test's .cfg file.
  --die Not all NODE_MYPORT_* environment variables are setup correctly.
}

if ($galera_debug)
{
--disable_query_log
  --echo connect($galera_connection_name,$galera_connection_address,root,,test,$_galera_port,)
--enable_query_log
}

# Open a connection
--disable_query_log
--connect($galera_connection_name,$galera_connection_address,root,,test,$_galera_port,)
--enable_query_log

# MDEV-7178: connecting to a node that is concurrently still joining the
# cluster (receiving IST/SST) can race the provider. Wait for the node to
# report ready instead of guessing a fixed delay (MDEV-40211).
if (!$galera_no_wait_ready)
{
  # The ready check must not itself wait on causality: a test can
  # legitimately connect a second session to a node whose applier is
  # stuck behind a local lock (e.g. LOCK TABLES on another connection),
  # and wsrep_sync_wait (every one of its bits covers SELECT, SHOW,
  # INSERT/REPLACE and UPDATE/DELETE - setting it non-zero commonly
  # means all of them, as galera_2nodes.cnf does) would otherwise block
  # this new connection's first query on that same stuck applier,
  # hanging the very check meant to confirm the node is up. Read
  # nothing back first: even a bare SELECT of the session variable hits
  # the same wait; SET does not, so go straight to zero and restore
  # from the global value, entirely without a SELECT.
  --disable_query_log
  SET SESSION wsrep_sync_wait= 0;
  --enable_query_log
  --source include/galera_wait_ready.inc
  --disable_query_log
  SET SESSION wsrep_sync_wait= @@GLOBAL.wsrep_sync_wait;
  --enable_query_log
}
--let $galera_no_wait_ready= 0
