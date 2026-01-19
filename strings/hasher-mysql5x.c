#include "strings_def.h"
#include <m_ctype.h>

static uint64 my_hasher_mysql5x_finalize(my_hasher_st *hasher)
{
  /* Cast to uint32 for backward compatibility */
  return (uint32) hasher->m_nr1;
}

static void my_hasher_mysql5x_hash_num(struct my_hasher_st *hasher,
                                       const uchar* num,
                                       size_t binary_size)
{
  my_hash_sort_simple(hasher, &my_charset_latin1, num, binary_size);
}

/* The default MYSQL51/MYSQL55 hash algorithms. */
my_hasher_st my_hasher_mysql5x(void)
{
  my_hasher_st tmp=
    { {{.m_nr1 = 1, .m_nr2 = 4}}, FALSE, NULL, NULL,
      my_hasher_mysql5x_hash_num, my_hasher_mysql5x_finalize, NULL };
  return tmp;
}

/*
  Used in myisam/aria hash of row with unique constraints. Likely
  introduced by mistake - don't use in new code
*/
my_hasher_st my_hasher_mysql5x_for_unique(void)
{
  my_hasher_st tmp=
    { {{.m_nr1 = 0, .m_nr2 = 4}}, FALSE, NULL, NULL,
      my_hasher_mysql5x_hash_num, my_hasher_mysql5x_finalize, NULL };
  return tmp;
}
