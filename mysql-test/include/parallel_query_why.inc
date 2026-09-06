# The reason the gate gave for the statement just run. One row, one reason:
# more than one would mean two checks objected, which is worth seeing.
--enable_result_log
SELECT JSON_EXTRACT(trace, '$**.parallel_scan_declined_because') AS why
  FROM information_schema.optimizer_trace;
--disable_result_log
