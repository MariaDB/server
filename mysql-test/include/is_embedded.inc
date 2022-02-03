#
# Check if the current used server is an embedded server#
#
if (`SELECT VERSION() NOT LIKE '%embedded%'`)
{
   --skip Test requires: embedded server
}
