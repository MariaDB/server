#include <stdio.h>

#include "my_global.h"
#include "sql_type.h"


const Type_handler *type_handler_for_datetime()
{
  return &type_handler_datetime;
}

int main(int argc, char **argv)
{

  printf("hello world");

  return 0;
}
