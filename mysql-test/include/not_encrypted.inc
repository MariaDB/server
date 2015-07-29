if (`select @@innodb_encrypt_tables = 1`)
{
  skip only without encryption;
}
