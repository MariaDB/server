#include "strings_def.h"
#include <m_ctype.h>

static uint32 my_hasher_mysql5x_finalize(my_hasher_st *hasher)
{
  return (uint32) hasher->m_nr1;
}

/* The default MYSQL51/MYSQL55 hash algorithms. */
my_hasher_st my_hasher_mysql5x()
{
  my_hasher_st tmp=
    { 1, 4, 0, FALSE, NULL, NULL, my_hasher_mysql5x_finalize, NULL };
  return tmp;
}
