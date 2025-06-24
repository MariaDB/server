disable_query_log;
disable_warnings;
if (`SELECT from_unixtime((1<<31)+24*3600) is null`) {
--skip Need a 64 bit timestamps
}
enable_warnings;
enable_query_log;
