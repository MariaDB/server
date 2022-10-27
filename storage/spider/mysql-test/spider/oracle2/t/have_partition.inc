let $HAVE_PARTITION= 0;
if (`SELECT count(*) FROM information_schema.plugins WHERE
      plugin_status = 'ACTIVE' AND
      plugin_name = 'partition'`)
{
  let $HAVE_PARTITION= 1;
}
