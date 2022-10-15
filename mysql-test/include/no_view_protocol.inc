# The file with expected results fits only to a run without
# view-protocol.
if (`SELECT $VIEW_PROTOCOL > 0`)
{
   --skip Test requires: view-protocol disabled
}