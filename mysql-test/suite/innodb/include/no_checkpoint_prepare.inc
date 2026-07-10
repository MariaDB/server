--disable_query_log
call mtr.add_suppression("InnoDB: innodb_read_only prevents crash recovery");
call mtr.add_suppression("InnoDB: Plugin initialization aborted");
call mtr.add_suppression("Plugin 'InnoDB' registration as a STORAGE ENGINE failed");
call mtr.add_suppression("unknown option '--innodb-invalid-option'");
call mtr.add_suppression("\\[ERROR\\] Aborting");
let $INNODB_PAGE_SIZE=`SELECT @@GLOBAL.innodb_page_size`;
--enable_query_log
