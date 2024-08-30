if (`select version() like '%valgrind%' || version() like '%asan%'`)
{
  skip Does not run with binaries built with valgrind or asan;
}
