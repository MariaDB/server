--error 0,1,127
--exec openssl version > /dev/null 2> /dev/null
if ($sys_errno)
{
  --skip Requires openssl executable
}
