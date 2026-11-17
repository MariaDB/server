# KILL QUERY, KILL CONNECTION and max_statement_time stop the SQL a
# handler runs. Takes $sqlext_verb: SQL (the as-client connection),
# FORWARD (forwarded SQL) or SWALLOW (a handler that ignores the error of
# its SQL: the statement fails all the same).
# Runs on the default connection and leaves the caller there.
--echo # The timer of the extension statement applies: the SQL fails, as
--echo # in a stored procedure (a plain SELECT SLEEP() would return 1)
SET max_statement_time= 0.2;
--error ER_STATEMENT_TIMEOUT
eval TEST SQLEXT $sqlext_verb SELECT SLEEP(10) AS s;
SET max_statement_time= DEFAULT;
eval TEST SQLEXT $sqlext_verb SELECT 1 AS a;

--echo # KILL QUERY while the SQL runs
let $sqlext_kill= QUERY;
--source suite/sql_extension/include/kill_during_sql.inc
--echo # KILL CONNECTION while the SQL runs: the connection ends
let $sqlext_kill= CONNECTION;
--source suite/sql_extension/include/kill_during_sql.inc
