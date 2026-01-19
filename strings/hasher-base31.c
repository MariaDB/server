#include "strings_def.h"
#include <m_ctype.h>

static void my_hasher_base31_hash_str(my_hasher_st *hasher, const uchar *key,
                                      size_t len)
{
  for (const uchar *u= key; u < key + len; u++)
    hasher->m_nr= hasher->m_nr * 31 + ((uint) *u);
}

static void my_hasher_base31_hash_byte(my_hasher_st *hasher, uchar byte)
{
  hasher->m_nr= hasher->m_nr * 31 + ((uint) byte);
}

static uint64 my_hasher_base31_finalize(my_hasher_st *hasher)
{
  return hasher->m_nr;
}

/* A baseline base-31 modular hash function */
my_hasher_st my_hasher_base31(void)
{
  my_hasher_st tmp=
    { {.m_nr = 0}, FALSE, my_hasher_base31_hash_str, my_hasher_base31_hash_byte,
      my_hasher_hash_num, my_hasher_base31_finalize, NULL };
  return tmp;
}
