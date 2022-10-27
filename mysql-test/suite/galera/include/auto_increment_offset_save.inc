# This file can be used to save the @@global.auto_increment_offset value at
# the beginning of any test that intends to restart any of the participating
# nodes. This is required as the node may get auto-assigned a different
# auto_increment_offset value on restart, which could cause MTR's internal
# post-check to fail. auto_increment_offset_restore.inc can be used at the
# end of the test to restore these saved values.

# Parameters
# ----------
# $node_1
#   Connection handle for 1st node
# $node_2
#   Connection handle for 2nd node
# $node_3 (optional)
#   Connection handle for 3rd node
# $node_4 (optional)
#   Connection handle for 4th node

if (!$node_1)
{
  --die ERROR IN TEST: $node_1 must be set before sourcing auto_increment_offset_save.inc
}

if (!$node_2)
{
  --die ERROR IN TEST: $node_2 must be set before sourcing auto_increment_offset_save.inc
}

--connection $node_1
let $auto_increment_offset_node_1 = `SELECT @@global.auto_increment_offset`;
--connection $node_2
let $auto_increment_offset_node_2 = `SELECT @@global.auto_increment_offset`;

if ($node_3)
{
  --connection $node_3
  let $auto_increment_offset_node_3 = `SELECT @@global.auto_increment_offset`;
}

if ($node_4)
{
  --connection $node_4
  let $auto_increment_offset_node_4 = `SELECT @@global.auto_increment_offset`;
}

