if (!`SELECT count(*) FROM information_schema.engines WHERE
      (support = 'YES' OR support = 'DEFAULT') AND
      engine = 'columnstore_cache'`){
  skip Need ColumnStore_Cache engine;
}
