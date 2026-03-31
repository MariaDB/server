--error 0,1,2
--exec getent hosts 127.0.0.1 ::1 ::ffff:127.0.0.1 > /dev/null 2>&1
if ($errno)
{
    skip IPv4-mapped IPv6 address resolution not possible;
}
