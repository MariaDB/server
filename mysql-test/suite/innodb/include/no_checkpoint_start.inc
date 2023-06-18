# Preparation for using no_checkpoint_end.inc

--replace_regex /.*Last checkpoint at[ 	]*([0-9]+).*/\1/
let CHECKPOINT_LSN=`SHOW ENGINE INNODB STATUS`;
