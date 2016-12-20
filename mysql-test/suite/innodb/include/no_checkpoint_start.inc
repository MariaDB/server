# Preparation for using no_checkpoint_end.inc

let MYSQLD_DATADIR= `select @@datadir`;
--replace_regex /.*Last checkpoint at[ 	]*([0-9]+).*/\1/
let CHECKPOINT_LSN=`SHOW ENGINE INNODB STATUS`;
