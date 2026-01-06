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
# With current number of events = 171,
#
#   binlog_start_pos = 4 + 19 + 57 + 171 + 1 + 4 = 256.
#
##############################################################################

--disable_query_log
set @binlog_start_pos=256 + @@encrypt_binlog * (36 + (@@binlog_checksum != 'NONE') * 4);
--enable_query_log
let $binlog_start_pos=`select @binlog_start_pos`;

