if (!`SELECT count(*) FROM information_schema.plugins WHERE
      (PLUGIN_STATUS = 'ACTIVE') AND PLUGIN_NAME = 'METADATA_LOCK_INFO'`){
  skip Need archive METADATA_LOCK_INFO plugin;
}
