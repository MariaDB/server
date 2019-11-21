if (`select count(*) = 0 from information_schema.session_variables where variable_name = 'rocksdb_write_policy' and variable_value = 'write_prepared';`) {
  --skip Test requires write_prepared policy
}
