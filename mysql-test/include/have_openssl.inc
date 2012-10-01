-- source include/have_ssl_communication.inc
let $crllen=`select length(trim(coalesce(@@ssl_crl, ''))) + length(trim(coalesce(@@ssl_crlpath, '')))`;
if (!$crllen)
{
  skip Needs OpenSSL;
}

