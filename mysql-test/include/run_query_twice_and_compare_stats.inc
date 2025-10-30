set optimizer_replay_context="";

evalp set @explain_output='$explain_query';

set @saved_opt_context=
  (select json_pretty(json_extract(
                        json_extract(trace, "$**.optimizer_context"),
                        '$[0]'
                      )
  )
  from information_schema.optimizer_trace);

set @saved_explain_output=@explain_output;

set optimizer_replay_context="";
eval $table_update_query;

set optimizer_replay_context=@saved_opt_context;

evalp set @explain_output='$explain_query';

select JSON_EQUALS(@saved_explain_output, @explain_output);

set optimizer_replay_context="";
