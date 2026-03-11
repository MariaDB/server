#
#Each query is run twice. However, between the runs
#the table is updated by either adding rows or deleting rows etc.
#During the first run of the query, optimizer_context from the trace
#is saved to a variable "saved_opt_context", and the explain output is saved
#into "saved_explain_output".
#Once the table is modified, the saved context is used to rerun the query.
#The execution plan output along with the stats should be one and the same
#during each run of the query.
#
#Parameters:
#$table_name
#$explain_query
#
set optimizer_replay_context= "";

echo $explain_query;

set optimizer_record_context= ON;

let $explain_output=`$explain_query`;

select context into dumpfile
    "../../tmp/dump1.sql" from information_schema.optimizer_context;

evalp set @explain_output= '$explain_output';

set @explain_output= (select json_pretty(round_cost(@explain_output)));
select @explain_output;
set @saved_explain_output= @explain_output;

eval drop table $table_name;

--echo #source the dump1.sql file
-- disable_query_log
-- source "$MYSQLTEST_VARDIR/tmp/dump1.sql"
-- enable_query_log

let $explain_output=`$explain_query`;
evalp set @explain_output= '$explain_output';
set @explain_output= (select json_pretty(round_cost(@explain_output)));
select JSON_EQUALS(@saved_explain_output, @explain_output);

--remove_file "$MYSQLTEST_VARDIR/tmp/dump1.sql"

set optimizer_replay_context= "";
