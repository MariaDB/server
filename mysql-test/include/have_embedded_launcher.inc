# MDEV-11111: the test needs the embedded server started by mysqltest, i.e.
# mtr --embedded-launcher
if (`SELECT '$MTR_EMBEDDED_LAUNCHER' != '1'`)
{
  --skip Needs mtr --embedded-launcher
}
