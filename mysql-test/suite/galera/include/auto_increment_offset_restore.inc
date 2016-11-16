# See auto_increment_offset_restore.inc for details.

if (!$node_1)
{
  --die ERROR IN TEST: $node_1 must be set before sourcing auto_increment_offset_save.inc
}

if (!$node_2)
{
  --die ERROR IN TEST: $node_2 must be set before sourcing auto_increment_offset_save.inc
}

if (!$auto_increment_offset_node_1)
{
  --die ERROR IN TEST: $auto_increment_offset_node_1 must be set before sourcing auto_increment_offset_save.inc
}

if (!$auto_increment_offset_node_2)
{
  --die ERROR IN TEST: $auto_increment_offset_node_2 must be set before sourcing auto_increment_offset_save.inc
}

# Restore original auto_increment_offset values.
--disable_query_log
--connection $node_1
--eval SET @@global.auto_increment_offset = $auto_increment_offset_node_1;
--connection $node_2
--eval SET @@global.auto_increment_offset = $auto_increment_offset_node_2;

if ($node_3)
{
--connection $node_3
--eval SET @@global.auto_increment_offset = $auto_increment_offset_node_3;
}

if ($node_4)
{
--connection $node_4
--eval SET @@global.auto_increment_offset = $auto_increment_offset_node_4;
}
--enable_query_log
