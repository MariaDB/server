if (`select @@have_ssl = 'YES'`)
{
  skip only without SSL;
}
