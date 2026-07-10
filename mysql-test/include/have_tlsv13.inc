--disable_query_log
connect (ssl_connection,localhost,root,,,,,SSL);

if (`SELECT VARIABLE_VALUE NOT LIKE 'TLSv1.3' FROM information_schema.SESSION_STATUS WHERE VARIABLE_NAME = 'ssl_version'`) {
  skip Needs TLSv1.3;
}

disconnect ssl_connection;
connection default;
--enable_query_log
