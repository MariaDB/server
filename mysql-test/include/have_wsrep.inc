-- require r/have_wsrep.require
disable_query_log;
show variables like 'wsrep_on';
enable_query_log;
