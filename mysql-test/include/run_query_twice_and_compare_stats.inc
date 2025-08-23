#
# Each query is run twice. However, between the runs
# the table is updated by either adding rows or deleting rows etc. 
# During the first run of the query, optimizer_context from the trace 
# is saved to a variable "saved_opt_context", and the explain output is saved
# into "saved_explain_output".
# Once the table is modified, the saved context is used to rerun the query.
# The execution plan output along with the stats should be one and the same
# during each run of the query.
#
# Parameters:
#    $table_name
#    $explain_query
#    $table_update_query
#
set optimizer_replay_context="";

echo $explain_query;
let $explain_output=`$explain_query`;

set @trace= (select trace from information_schema.optimizer_trace);
set @saved_opt_context=
  (select json_pretty(json_extract(
                        json_extract(@trace, "$**.optimizer_context"),
                        '$[0]'
                      )
  ));

#TO-DO: enable it after MDEV-38033 is resolved
#select JSON_SCHEMA_VALID(@opt_context_schema, @saved_opt_context);

evalp set @explain_output='$explain_output';
select @explain_output;
set @saved_explain_output=@explain_output;

set optimizer_replay_context="";
eval $table_update_query;
let $analyze_stmt=analyze table $table_name;
eval $analyze_stmt;

set optimizer_replay_context=@saved_opt_context;

let $explain_output=`$explain_query`;
evalp set @explain_output='$explain_output';

select JSON_EQUALS(@saved_explain_output, @explain_output);

set optimizer_replay_context="";
