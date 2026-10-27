# Report whether the aggregate that ran just before this file was sourced
# converted its temporary table to the on-disk engine.
#
# Reading INFORMATION_SCHEMA materializes a temporary table of its own, and
# its VARIABLE_VALUE column is wide enough to be stored as a blob, so under
# the shrunken in-memory limit that table can overflow and be converted as
# well, and would then report its own conversion rather than the aggregate's.
# The limit is restored for the duration of the read and shrunk again
# afterwards.
#
# Expects @save_tmp_memory_table_size, @save_max_heap_table_size and
# @small_heap to be set by the test.

--disable_query_log
SET @@tmp_memory_table_size= @save_tmp_memory_table_size;
SET @@max_heap_table_size= @save_max_heap_table_size;
--enable_query_log

SELECT IF(VARIABLE_VALUE > 0, 'ON', 'OFF') AS CONVERTED
FROM INFORMATION_SCHEMA.SESSION_STATUS
WHERE VARIABLE_NAME = 'CREATED_TMP_DISK_TABLES';

--disable_query_log
SET @@tmp_memory_table_size= @small_heap;
SET @@max_heap_table_size= @small_heap;
--enable_query_log
