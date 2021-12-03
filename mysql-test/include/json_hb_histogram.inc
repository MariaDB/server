# The time on ANALYSE FORMAT=JSON is rather variable

--replace_regex /("(collected_at|collected_by)": )"[^"]*"/\1"REPLACED"/
