--source include/have_partition.inc
--source include/gap_lock_error_init.inc

let $select_lock=for update;
let $autocommit = 0;
--source include/gap_lock_error_select.inc
let $autocommit = 1;
--source include/gap_lock_error_select.inc

let $select_lock=lock in share mode;
let $autocommit = 0;
--source include/gap_lock_error_select.inc
let $autocommit = 1;
--source include/gap_lock_error_select.inc

let $select_lock=;
let $autocommit = 0;
--source include/gap_lock_error_select.inc
let $autocommit = 1;
--source include/gap_lock_error_select.inc

let $autocommit = 0;
--source include/gap_lock_error_update.inc
let $autocommit = 1;
--source include/gap_lock_error_update.inc

--source include/gap_lock_error_cleanup.inc
