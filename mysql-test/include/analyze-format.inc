# Remove non-deterministic parts of ANALYZE FORMAT=JSON output:
# - any timings
# - Buffer sizes (depend on pointer size)
# - r_engine_stats depends on buffer pool state and whether old record versions
#   were purged.
# - r_rows_per_worker: the parallel workers take chunks from a shared queue, so
#   which worker reads which chunk is down to scheduling and the split differs
#   from run to run. Only the total, r_rows, is deterministic.

--replace_regex /("(r_[a-z_]*_time(_in_progress)?_ms|r_buffer_size|cost|r_partial_match_buffer_size)": )[^, \n]*/\1"REPLACED"/ /("r_engine_stats":) \{(?:[^{}]|\{[^}]*\})*\}/\1 REPLACED/ /("r_rows_per_worker": )\[[^\]]*\]/\1REPLACED/
