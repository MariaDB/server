# Run $query, print its result, then check that the scan of the 1000-row
# table stopped early (Handler_read_rnd_next < 500) iff $expect_stop is 1.
# Prints "UNEXPECTED ..." on a mismatch, and nothing otherwise.
# If $show_found_rows is 1, SELECT FOUND_ROWS() right after the query.
--disable_query_log
FLUSH STATUS;
--enable_query_log
--eval $query
if ($show_found_rows)
{
  SELECT FOUND_ROWS();
}
--let $rnd_next= query_get_value(SELECT variable_value FROM information_schema.session_status WHERE variable_name='Handler_read_rnd_next', variable_value, 1)
--disable_query_log
--let $stopped= `SELECT $rnd_next < 500`
if ($stopped != $expect_stop)
{
  --echo # UNEXPECTED scan length: Handler_read_rnd_next=$rnd_next (expect_stop=$expect_stop)
}
--enable_query_log
