if (`select convert(@@version_compile_os using latin1) LIKE 'Linux' = 0`)
{
  skip Need Linux;
}

