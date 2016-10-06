#!/bin/awk

/Query_time:/ {
  results["Rows_examined:"] = "uninit";
  results["RocksDB_key_skipped:"] = "uninit";
  results["RocksDB_del_skipped:"] = "uninit";

  for (i = 2; i <= NF; i = i+2) {
    results[$i] = $(i+1);
  }

  # If the output format has changed and we don't find these keys,
  # error out.
  if (results["Rows_examined:"] == "uninit" ||
      results["RocksDB_key_skipped:"] == "uninit" ||
      results["RocksDB_del_skipped:"] == "uninit") {
    exit(-2);
  }

  if (results["Rows_examined:"] == 0) {
    next
  }
  if (results["RocksDB_key_skipped:"] == 0 ||
      results["RocksDB_del_skipped:"] == 0) {
    exit(-1);
  }
}
