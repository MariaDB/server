if (`select variable_value <> '' from information_schema.session_status where variable_name='ssl_cipher'`)
{
  skip only without SSL;
}
