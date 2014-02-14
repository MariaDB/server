# To be used in a test which should be skipped if server is compiled with wsrep
# support (-DWITH_WSREP=ON) and wsrep plugin is ACTIVE.

-- require r/not_wsrep.require
disable_query_log;
SELECT VERSION() LIKE '%wsrep%' AS 'HAVE_WSREP';
enable_query_log;
