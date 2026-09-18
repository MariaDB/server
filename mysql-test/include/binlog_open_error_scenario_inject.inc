#
# Sub-helper for binlog_open_error_scenario.inc — error-injection kind.
#
# Sets the DBUG knob, fires the trigger described by $scn_trigger, and
# reports the observed client error.  Server stays alive throughout.
#
# The same --replace_regex is applied to both the trigger and the following
# SHOW WARNINGS: the identical message is reported through both, and would
# otherwise be recorded unmasked (errno/path-sensitive) by the latter.
#

--disable_query_log
--eval SET @@global.debug_dbug = "d,$scn_dbug"
--enable_query_log

if ($scn_trigger == flush)
{
  if ($scn_expected_client_error)
  {
    --replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
    --error $scn_expected_client_error
    FLUSH BINARY LOGS;
  }
  if (!$scn_expected_client_error)
  {
    --replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
    FLUSH BINARY LOGS;
  }
}
if ($scn_trigger == rotate_size)
{
  # Trigger an implicit rotation by writing past max_binlog_size.  The test
  # is responsible for setting max_binlog_size small enough and creating
  # any tables it needs before sourcing this helper.
  if ($scn_expected_client_error)
  {
    --replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
    --error $scn_expected_client_error
    INSERT INTO scn_filler VALUES (REPEAT('x', 6000));
  }
  if (!$scn_expected_client_error)
  {
    --replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
    INSERT INTO scn_filler VALUES (REPEAT('x', 6000));
  }
}
if ($scn_trigger == reset_master)
{
  if ($scn_expected_client_error)
  {
    --replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
    --error $scn_expected_client_error
    RESET MASTER;
  }
  if (!$scn_expected_client_error)
  {
    --replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
    RESET MASTER;
  }
}
if ($scn_trigger == checksum_update)
{
  # Always toggles to a different value, so it always forces
  # binlog_checksum_update() -> MYSQL_BIN_LOG::rotate(), regardless of the
  # value going in.
  if ($scn_expected_client_error)
  {
    --replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
    --error $scn_expected_client_error
    --eval SET GLOBAL binlog_checksum= IF(@@global.binlog_checksum='NONE', 'CRC32', 'NONE')
  }
  if (!$scn_expected_client_error)
  {
    --replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
    --eval SET GLOBAL binlog_checksum= IF(@@global.binlog_checksum='NONE', 'CRC32', 'NONE')
  }
}

--replace_regex /\.[\\\/]master/master/ /errno: [0-9]+/errno: NN/
SHOW WARNINGS;

--disable_query_log
SET @@global.debug_dbug = @scn_saved_dbug;
--enable_query_log
