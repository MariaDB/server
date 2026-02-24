# Remove non-deterministic parts of JSON_HB histogram

--replace_regex /("(collected_at|collected_by)": )"[^"]*"/\1"REPLACED"/
