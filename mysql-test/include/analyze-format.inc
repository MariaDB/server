# The time on ANALYSE FORMAT=JSON is rather variable

--replace_regex /("(r_total_time_ms|r_table_time_ms|r_other_time_ms|r_buffer_size|r_filling_time_ms|r_query_time_in_progress_ms)": )[^, \n]*/\1"REPLACED"/
