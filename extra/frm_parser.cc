#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation // gcc: Class implementation
#endif

#define MYSQL_SERVER
#include "my_global.h"
#include "mysql_version.h"
#include "sql_class.h"



int main(int argc, char **argv)
{
  THD *thd;
  thd= new THD(0);

  delete thd;
  return 0;
}