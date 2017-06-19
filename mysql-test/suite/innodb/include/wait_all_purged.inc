# Wait for everything to be purged.
# The user should have set innodb_purge_rseg_truncate_frequency=1.

let $wait_counter= 300;
while ($wait_counter)
{
    --replace_regex /.*History list length ([0-9]+).*/\1/
    let $remaining= `SHOW ENGINE INNODB STATUS`;
    if ($remaining == 'InnoDB		0')
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
