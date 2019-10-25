# Wait for everything to be purged.
# The user should have set innodb_purge_rseg_truncate_frequency=1.

if (!$wait_all_purged)
{
  let $wait_all_purged= 0;
}
let $remaining_expect= `select concat('InnoDB		',$wait_all_purged)`;

let $wait_counter= 600;
if ($VALGRIND_TEST)
{
  let $wait_counter= 2000;
}

while ($wait_counter)
{
    --replace_regex /.*History list length ([0-9]+).*/\1/
    let $remaining= `SHOW ENGINE INNODB STATUS`;
    if ($remaining == $remaining_expect)
    {
        let $wait_counter= 0;
    }
    if ($wait_counter)
    {
        real_sleep 0.1;
        dec $wait_counter;
    }
}
echo $remaining transactions not purged;
