##############################################################################
#
# binlog_start_pos is the postion of the the first event in the binary log
# which follows the Format description event. Intended to reduce test suite
# dependance on the Format description event length changes (e.g. in case
# of adding new events). Evaluated as:
#  
#   binlog_start_pos = 4  /* binlog header */ +
#                      (Format_description_log_event length)
#
#   Format_description_log_event length = 
#                      19 /* event common header */ +
#                      57 /* misc stuff in the Format description header */ +
#                      number of events +
#                      1 /* Checksum algorithm */ +
#                      4 /* CRC32 length */
# 
# With current number of events = 161,
#
#   binlog_start_pos = 4 + 19 + 57 + 161 + 1 + 4 = 246.
#
##############################################################################

let $binlog_start_pos=246;
--disable_query_log
SET @binlog_start_pos=246;
--enable_query_log

