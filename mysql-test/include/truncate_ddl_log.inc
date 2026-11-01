# Truncate the ddl log by cycling BACKUP STAGE END/START on con1, so a
# subsequent print_ddl_log.inc only shows entries logged after this point.
connection con1;
BACKUP STAGE END;
BACKUP STAGE START;
connection default;
