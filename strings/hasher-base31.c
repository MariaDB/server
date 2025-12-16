#include "strings_def.h"
#include <m_ctype.h>

static void my_hasher_base31_hash_str(my_hasher_st *hasher, const uchar *key,
                                      size_t len)
{
  for (const uchar *u= key; u < key + len; u++)
    hasher->m_nr= ((uint) *u) * 31 + hasher->m_nr;
}

static void my_hasher_base31_hash_byte(my_hasher_st *hasher, uchar byte)
{
  hasher->m_nr= ((uint) byte) * 31 + hasher->m_nr;
}

static uint32 my_hasher_base31_finalize(my_hasher_st *hasher)
{
  return hasher->m_nr;
}

/* A baseline base-31 modular hash function */
my_hasher_st my_hasher_base31()
{
  my_hasher_st tmp=
    { 1, 4, 0, FALSE, my_hasher_base31_hash_str, my_hasher_base31_hash_byte,
      my_hasher_base31_finalize, NULL };
  return tmp;
}
