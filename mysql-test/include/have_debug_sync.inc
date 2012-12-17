if (`select @@debug_sync not like 'ON %'`) {
  --skip Needs a debug_sync enabled
}
