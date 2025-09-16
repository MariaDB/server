if (`SELECT COUNT(*)=0 FROM INFORMATION_SCHEMA.PLUGINS WHERE PLUGIN_NAME = 'GENERATE_EMBEDDING_OPENAI' AND PLUGIN_STATUS='ACTIVE'`)
{
  --skip Test requires active GENERATE_EMBEDDING_OPENAI plugin
}

# Helper function
let $get_expensive_calls= (select variable_value 
   from information_schema.session_status 
   where variable_name like 'Generate_embedding_openai_successful_http_requests');
