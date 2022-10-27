if (`SELECT count(*) = 0 FROM information_schema.GLOBAL_VARIABLES WHERE
      VARIABLE_NAME = 'have_ssl' AND VARIABLE_VALUE = 'YES'`){
  skip Needs SSL;
}
