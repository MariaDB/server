#include "strings_def.h"
#include <m_ctype.h>
#include <my_sys.h>

static void my_hasher_crc32c_hash_str(my_hasher_st *hasher, const uchar *key,
                                      size_t len)
{
  hasher->m_nr= my_crc32c(hasher->m_nr, key, len);
}

static void my_hasher_crc32c_hash_byte(my_hasher_st *hasher, const uchar byte)
{
  hasher->m_nr= my_crc32c(hasher->m_nr, &byte, 1);
}

static uint32 my_hasher_crc32c_finalize(my_hasher_st *hasher)
{
  return hasher->m_nr;
}

my_hasher_st my_hasher_crc32c(void)
{
  my_hasher_st tmp=
    { 1, 4, 0, FALSE, my_hasher_crc32c_hash_str, my_hasher_crc32c_hash_byte,
      my_hasher_crc32c_finalize, NULL };
  return tmp;
}
