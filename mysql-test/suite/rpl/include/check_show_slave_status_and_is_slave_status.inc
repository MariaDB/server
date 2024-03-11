# Check all fields for show slave status and IS.slave_status
# Parameters
#   $is_slave_status_fields - comma separated fields
#   $is_table               - IS table (slave_status or replica_status)

let $num_rows= 1;
let $num= 1;
let $handle_null= 0;
let $show_query= SHOW ALL SLAVES STATUS;
if ($is_table=='')
{
  let $is_table= slave_status;
}

while ($is_slave_status_fields)
{
  let $sss_field= `SELECT SUBSTRING_INDEX('$is_slave_status_fields', ',', 1)`;
  let $is_slave_status_fields= `SELECT LTRIM(SUBSTRING('$is_slave_status_fields', LENGTH('$sss_field') + 2))`;

  --echo # Check $sss_field
  let $sss_field_value= query_get_value($show_query, $sss_field, $num_rows);
  let $is_field_value= `select $sss_field from information_schema.$is_table`;
  if (`SELECT strcmp("$sss_field_value", "$is_field_value") != 0`)
  {
    die SHOW SLAVE STATUS $sss_field value ($sss_field_value) does not match value of IS.$is_table ($is_field_value);
  }
}
