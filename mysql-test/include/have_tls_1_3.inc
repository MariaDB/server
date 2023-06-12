connect (conn1,localhost,root,,,,,SSL);
if (`SELECT count(*) = 0 FROM information_schema.GLOBAL_STATUS WHERE
      VARIABLE_NAME = 'ssl_version' AND VARIABLE_VALUE = 'TLSv1.3'`){
  skip Needs TLSv1.3;
}