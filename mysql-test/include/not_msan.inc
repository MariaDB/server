# This file should only be used with tests that are too big or slow for MSAN (even with --big-test).
# Use no_msan_without_big instead unless this really won't complete in a test timeout period.

if (`select count(*) from information_schema.system_variables where variable_name='have_sanitizer' and global_value like "MSAN%"`)
{
--skip Can't be run WITH_MSAN
}
