if (`select count(*) = 0 from information_schema.session_variables where variable_name = 'rocksdb_write_policy' and variable_value = 'write_committed';`) {
  --skip Test requires write_committed policy
}
