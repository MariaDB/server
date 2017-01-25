# The time on ANALYSE FORMAT=JSON is rather variable

--replace_regex /("(r_total_time_ms|r_buffer_size)": )[^, \n]*/\1"REPLACED"/
