--source include/have_rocksdb.inc
--source include/have_debug_sync.inc

# This validates the fix for Issue #144.  The problem was that with more
# than one client accessing/deleting the same row there was a possibility
# of client A finding a row (through Next() or Prev()) but the row being
# deleted before the GetForUpdate() call could occur.  When this happened
# a nearly useless error was being returned.

let $order=ASC;
let $comment="";
--source rocksdb_concurrent_delete.inc
--source rocksdb_concurrent_delete_sk.inc

let $order=DESC;
let $comment="";
--source rocksdb_concurrent_delete.inc

let $order=ASC;
let $comment="rev:cf2";
--source rocksdb_concurrent_delete.inc

let $order=DESC;
let $comment="rev:cf2";
--source rocksdb_concurrent_delete.inc

let $index=PRIMARY;
--source rocksdb_concurrent_delete_range.inc
let $index=sk;
--source rocksdb_concurrent_delete_range.inc
