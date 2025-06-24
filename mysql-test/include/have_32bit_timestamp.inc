disable_query_log;
disable_warnings;
if (`SELECT from_unixtime((1<<31)+24*3600) is not null`) {
--skip Need a 32 bit timestamps
}
enable_warnings;
enable_query_log;
