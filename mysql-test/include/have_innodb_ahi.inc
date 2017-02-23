if (`select count(*)!=1 from information_schema.GLOBAL_VARIABLES
       where variable_name='innodb_adaptive_hash_index'`){
  skip Test requires cmake -DWITH_INNODB_AHI=ON;
}
