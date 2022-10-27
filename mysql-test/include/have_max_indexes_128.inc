--source include/max_indexes.inc

if (`SELECT "$max_indexes" NOT REGEXP "^128U?\$"`) {
  --skip Test needs mysqld built with --with-max-indexes=128
}
