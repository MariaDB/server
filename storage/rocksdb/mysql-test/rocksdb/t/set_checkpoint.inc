# Usage:
# let $checkpoint = <value to set the checkpoint>;
# let $succeeds = <1 if checkpoint creation should succeed, 0 otherwise>;
# --source set_checkpoint.inc


if ($succeeds)
{
  # Create checkpoint
  --replace_result '$checkpoint' [CHECKPOINT]
  eval SET GLOBAL ROCKSDB_CREATE_CHECKPOINT = '$checkpoint';

  # Check checkpoint
  --exec ls $checkpoint/CURRENT | sed s/.*CURRENT/CURRENT/g

  # Cleanup
  --exec rm -rf $checkpoint
}
if (!$succeeds)
{
  --disable_result_log
  --disable_query_log
  --error ER_UNKNOWN_ERROR
  eval SET GLOBAL ROCKSDB_CREATE_CHECKPOINT = '$checkpoint';
  --enable_query_log
  --enable_result_log
}
