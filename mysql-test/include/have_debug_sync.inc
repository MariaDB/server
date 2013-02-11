if (`SELECT COUNT(*) = 0 FROM information_schema.session_variables WHERE
      variable_name = 'debug_sync' AND variable_value LIKE 'ON %'`) {
  --skip Needs debug_sync enabled
}
