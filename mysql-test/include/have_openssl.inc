if (`SELECT count(*) = 0 FROM information_schema.GLOBAL_VARIABLES WHERE
      VARIABLE_NAME = 'have_openssl' AND VARIABLE_VALUE = 'YES'`){
  skip Needs OpenSSL;
}
