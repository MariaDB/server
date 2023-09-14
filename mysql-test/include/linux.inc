if (`select @@version_compile_os not LIKE 'Linux%'`)
{
  skip Need Linux;
}

