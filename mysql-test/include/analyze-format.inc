# Remove non-deterministic parts of ANALYZE FORMAT=JSON output:
# - any timings
# - Buffer sizes (depend on pointer size)
# - r_engine_stats depends on buffer pool state and whether old record versions
#   were purged.

--replace_regex /("(r_[a-z_]*_time(_in_progress)?_ms|r_buffer_size|r_partial_match_buffer_size)": )[^, \n]*/\1"REPLACED"/ /("r_engine_stats":) {[^}]*}/\1 REPLACED/
