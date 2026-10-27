#
# SUMMARY
#   Print the range(s) built for the query just run, either from the
#   optimizer trace or from the recorded optimizer context, so that the
#   two range-printing paths can share the same probe queries.
#
# PARAMETERS
#   $opt_gis_range_print_use_trace  If set, dump range_scan_alternatives
#                                   from the optimizer trace. Otherwise,
#                                   dump the recorded optimizer context.
#
# EXAMPLE
#   opt_trace.test, opt_context_store_stats.test
#
if ($opt_gis_range_print_use_trace)
{
  select json_detailed(json_extract(trace, '$**.range_scan_alternatives')) as rsa from information_schema.optimizer_trace;
}
if (!$opt_gis_range_print_use_trace)
{
  --source include/opt_context_list_tables_and_ranges.inc
}
