#include "strings_def.h"
#include <m_ctype.h>

#ifndef XXH_STATIC_LINKING_ONLY
#define XXH_STATIC_LINKING_ONLY 1
#endif // !defined(XXH_STATIC_LINKING_ONLY)
#ifndef XXH_IMPLEMENTATION
#define XXH_IMPLEMENTATION 1
#endif // !defined(XXH_IMPLEMENTATION)
#include "../mysys/xxhash.h"

static void my_hasher_xxh32_hash_str(my_hasher_st *hasher, const uchar *key, size_t len)
{
  if (hasher->m_streaming)
  {
    hasher->m_nr= XXH32_digest((XXH32_state_t *) hasher->m_specific);
    hasher->m_streaming= FALSE;
  }
  hasher->m_nr= XXH32(key, len, hasher->m_nr);
}

static void my_hasher_xxh32_hash_byte(my_hasher_st *hasher, uchar value)
{
  if (!hasher->m_streaming)
  {
    XXH32_reset((XXH32_state_t *) hasher->m_specific, hasher->m_nr);
    hasher->m_streaming= TRUE;
  }
  XXH32_update((XXH32_state_t *) hasher->m_specific, &value, 1);
}

static uint32 my_hasher_xxh32_finalize(my_hasher_st *hasher)
{
  if (hasher->m_streaming)
    hasher->m_nr= XXH32_digest((XXH32_state_t *) hasher->m_specific);
  XXH32_freeState((XXH32_state_t *) hasher->m_specific);
  return hasher->m_nr;
}

my_hasher_st my_hasher_xxh32(void)
{
  /*
     TODOs:
     1. check OOM (XXH32_createState() returns NULL)
     2. memory management in error handling
  */
  my_hasher_st tmp=
    { 1, 4, 0, FALSE, my_hasher_xxh32_hash_str, my_hasher_xxh32_hash_byte,
      my_hasher_xxh32_finalize, (void *) XXH32_createState() };
  return tmp;
}

static void my_hasher_xxh3_hash_str(my_hasher_st *hasher, const uchar *key, size_t len)
{
  if (hasher->m_streaming)
  {
    hasher->m_nr=
      (uint32) XXH3_64bits_digest((XXH3_state_t *) hasher->m_specific);
    hasher->m_streaming= FALSE;
  }
  hasher->m_nr= (uint32) XXH3_64bits_withSeed(key, len, hasher->m_nr);
}

static void my_hasher_xxh3_hash_byte(my_hasher_st *hasher, uchar value)
{
  if (!hasher->m_streaming)
  {
    XXH3_64bits_reset_withSeed((XXH3_state_t *) hasher->m_specific,
                               hasher->m_nr);
    hasher->m_streaming= TRUE;
  }
  XXH3_64bits_update((XXH3_state_t *) hasher->m_specific, &value, 1);
}

static uint32 my_hasher_xxh3_finalize(my_hasher_st *hasher)
{
  if (hasher->m_streaming)
    hasher->m_nr=
      (uint32) XXH3_64bits_digest((XXH3_state_t *) hasher->m_specific);
  XXH3_freeState((XXH3_state_t *) hasher->m_specific);
  return hasher->m_nr;
}

my_hasher_st my_hasher_xxh3(void)
{
  /*
     TODOs:
     1. check OOM (XXH32_createState() returns NULL)
     2. memory management in error handling
  */
  my_hasher_st tmp=
    { 1, 4, 0, FALSE, my_hasher_xxh3_hash_str, my_hasher_xxh3_hash_byte,
      my_hasher_xxh3_finalize, (void *) XXH3_createState() };
  return tmp;
}
