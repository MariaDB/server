# The time on ANALYSE FORMAT=JSON is rather variable

--replace_regex /("(r_[a-z_]*_time(_in_progress)?_ms|r_buffer_size)": )[^, \n]*/\1"REPLACED"/
