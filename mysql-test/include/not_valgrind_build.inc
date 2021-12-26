if (`select version() like '%valgrind%'`)
{
  skip Does not run with binaries built with valgrind;
}
