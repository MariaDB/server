--sorted_result

--echo ## Looking for *.tokudb files in data_dir
--source include/table_files_replace_pattern.inc
--list_files $DATADIR *.tokudb

--echo ## Looking for *.tokudb files in data_dir/$DB
--source include/table_files_replace_pattern.inc
--list_files $DATADIR/$DB/ *.tokudb
