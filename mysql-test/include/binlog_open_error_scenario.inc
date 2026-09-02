#
# === Name
#
# binlog_open_error_scenario.inc
#
# === Description
#
# Drive a single error-injection scenario for MYSQL_BIN_LOG::open / new_file
# and report observed behavior in a self-describing block.  Used by the
# binlog_open_errors_*.test files to exercise the failure points
# catalogued by the MDEV-36606 static analysis.
#
# Each invocation produces a banner + a fixed set of observation fields so
# that two scenarios reading the same source row of MDEV-36606 are directly
# comparable in the .result file.
#
# The post-trigger state is asserted exactly in both directions: binlogging
# still on means SHOW BINARY LOGS lists files, off means it fails with
# ER_NO_BINARY_LOGGING. Neither outcome is tolerated in place of the other.
#
# === Inputs (set with --let before --source)
#
#   $scn_id                Short id, e.g. open.write_binlog_magic
#   $scn_kind              error_inject | crash | startup
#   $scn_dbug              Single DBUG keyword, e.g. fault_injection_registering_index
#   $scn_trigger           flush | rotate_size | reset_master |
#                          checksum_update | startup
#   $scn_expected_client_error
#                          Expected client-visible error symbol (or empty if
#                          none expected). Used as --error <symbol>.
#   $scn_expected_logging_off
#                          0 or 1 (default 0). 1 asserts the trigger turned
#                          binlogging off, i.e. SHOW BINARY LOGS must fail
#                          with ER_NO_BINARY_LOGGING; 0 asserts it is still
#                          on and records the binlog list.
#   $scn_mdev36606_row     Reference label from MDEV-36606 spreadsheet, used
#                          in the banner.
#   $scn_skip_reason       If non-empty, scenario is reported as skipped with
#                          this reason and nothing else runs. Use for
#                          unreachable trigger/knob combinations.
#   $scn_poststate_crashes 0 or 1 (default 0). Some knobs leave the server in
#                          a state where the *trigger* statement reports no
#                          error but a later statement crashes it (observed
#                          for RESET MASTER + fault_injection_openning_index /
#                          fault_injection_recovering_index: reset_logs()
#                          never sets `error` when open_index_file() fails,
#                          so RESET MASTER "succeeds" and the corruption only
#                          surfaces on the next statement). If 1, the post-
#                          state SHOW BINARY LOGS is run through the same
#                          crash+restart dance as scn_kind=crash.
#
# All inputs are unset on exit, on every path, so subsequent invocations
# start clean.

if (!$scn_id)
{
  --die scn_id must be set before sourcing binlog_open_error_scenario.inc
}
if (!$scn_kind)
{
  --die scn_kind must be set before sourcing binlog_open_error_scenario.inc
}

--echo
--echo ### scenario id=$scn_id kind=$scn_kind trigger=$scn_trigger dbug=$scn_dbug mdev36606=$scn_mdev36606_row

# Latch the skip decision: $scn_skip_reason is cleared in the common reset
# block below, so it cannot be re-tested after that point.
--let $_scn_skipped= 0
if ($scn_skip_reason)
{
  --echo # SKIPPED: $scn_skip_reason
  --let $_scn_skipped= 1
}

if (!$_scn_skipped)
{
  --disable_query_log
  set @scn_saved_dbug = @@global.debug_dbug;
  --enable_query_log

  if ($scn_kind == error_inject)
  {
    --source include/binlog_open_error_scenario_inject.inc
  }
  if ($scn_kind == crash)
  {
    --source include/binlog_open_error_scenario_crash.inc
  }
  if ($scn_kind == startup)
  {
    --source include/binlog_open_error_scenario_startup.inc
  }

  if ($scn_poststate_crashes)
  {
    --echo # post-state query is expected to crash the server
    --let $_scn_expect_file= $MYSQLTEST_VARDIR/tmp/mysqld.1.expect
    --exec echo "wait" > $_scn_expect_file
    # The crash itself is asserted by wait_until_disconnected.inc below,
    # which dies on timeout; the error list only tolerates the race between
    # a clean error reply and the connection simply dropping.
    --error 0,2006,2013,ER_CONNECTION_KILLED
    SHOW BINARY LOGS;
    --source include/wait_until_disconnected.inc
    --exec echo "restart" > $_scn_expect_file
    --enable_reconnect
    --source include/wait_until_connected_again.inc
    --disable_reconnect
    --let $_scn_expect_file=
  }
  if (!$scn_poststate_crashes)
  {
    if (!$scn_expected_logging_off)
    {
      --source include/binlog_open_error_show_binary_logs.inc
    }
    if ($scn_expected_logging_off)
    {
      # Strict: binlogging must be off, so this must fail. Accepting
      # success here too would make the expectation unfalsifiable.
      --error ER_NO_BINARY_LOGGING
      SHOW BINARY LOGS;
    }
  }
}

--let $scn_id=
--let $scn_kind=
--let $scn_dbug=
--let $scn_trigger=
--let $scn_expected_client_error=
--let $scn_expected_logging_off=
--let $scn_mdev36606_row=
--let $scn_skip_reason=
--let $scn_poststate_crashes=
--let $_scn_skipped=
