/* Copyright (c) 2008, 2017, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/**
  @file storage/perfschema/pfs_digest.h
  Statement Digest data structures (implementation).
*/

/*
  This code needs extra visibility in the lexer structures
*/

#define MYSQL_LEX 1

#include "my_global.h"
#include "my_sys.h"
#include "pfs_instr.h"
#include "pfs_digest.h"
#include "pfs_global.h"
#include "table_helper.h"
#include "sql_lex.h"
#include "sql_get_diagnostics.h"
#include "sql_string.h"
#include <string.h>

size_t digest_max= 0;
ulong digest_lost= 0;

/** EVENTS_STATEMENTS_HISTORY_LONG circular buffer. */
PFS_statements_digest_stat *statements_digest_stat_array= NULL;
static unsigned char *statements_digest_token_array= NULL;
/** Consumer flag for table EVENTS_STATEMENTS_SUMMARY_BY_DIGEST. */
bool flag_statements_digest= true;
/**
  Current index in Stat array where new record is to be inserted.
  index 0 is reserved for "all else" case when entire array is full.
*/
volatile uint32 PFS_ALIGNED digest_monotonic_index;
bool digest_full= false;

LF_HASH digest_hash;
static bool digest_hash_inited= false;

/**
  Initialize table EVENTS_STATEMENTS_SUMMARY_BY_DIGEST.
  @param param performance schema sizing
*/
int init_digest(const PFS_global_param *param)
{
  /*
    Allocate memory for statements_digest_stat_array based on
    performance_schema_digests_size values
  */
  digest_max= param->m_digest_sizing;
  digest_lost= 0;
  PFS_atomic::store_u32(& digest_monotonic_index, 1);
  digest_full= false;

  if (digest_max == 0)
    return 0;

  statements_digest_stat_array=
    PFS_MALLOC_ARRAY(digest_max,
                     sizeof(PFS_statements_digest_stat),
                     PFS_statements_digest_stat,
                     MYF(MY_ZEROFILL));

  if (unlikely(statements_digest_stat_array == NULL))
  {
    cleanup_digest();
    return 1;
  }

  if (pfs_max_digest_length > 0)
  {
    /* Size of each digest array. */
    size_t digest_memory_size= pfs_max_digest_length * sizeof(unsigned char);

    statements_digest_token_array=
      PFS_MALLOC_ARRAY(digest_max,
                       digest_memory_size,
                       unsigned char,
                       MYF(MY_ZEROFILL));

    if (unlikely(statements_digest_token_array == NULL))
    {
      cleanup_digest();
      return 1;
    }
  }

  for (size_t index= 0; index < digest_max; index++)
  {
    statements_digest_stat_array[index].reset_data(statements_digest_token_array
                                                   + index * pfs_max_digest_length, pfs_max_digest_length);
  }

  /* Set record[0] as allocated. */
  statements_digest_stat_array[0].m_lock.set_allocated();

  return 0;
}

/** Cleanup table EVENTS_STATEMENTS_SUMMARY_BY_DIGEST. */
void cleanup_digest(void)
{
  /*  Free memory allocated to statements_digest_stat_array. */
  pfs_free(statements_digest_stat_array);
  pfs_free(statements_digest_token_array);
  statements_digest_stat_array= NULL;
  statements_digest_token_array= NULL;
}

C_MODE_START
static uchar *digest_hash_get_key(const uchar *entry, size_t *length,
                                  my_bool)
{
  const PFS_statements_digest_stat * const *typed_entry;
  const PFS_statements_digest_stat *digest;
  const void *result;
  typed_entry= reinterpret_cast<const PFS_statements_digest_stat*const*>(entry);
  DBUG_ASSERT(typed_entry != NULL);
  digest= *typed_entry;
  DBUG_ASSERT(digest != NULL);
  *length= sizeof (PFS_digest_key);
  result= & digest->m_digest_key;
  return const_cast<uchar*> (reinterpret_cast<const uchar*> (result));
}
C_MODE_END


/**
  Initialize the digest hash.
  @return 0 on success
*/
int init_digest_hash(void)
{
  if ((! digest_hash_inited) && (digest_max > 0))
  {
    lf_hash_init(&digest_hash, sizeof(PFS_statements_digest_stat*),
                 LF_HASH_UNIQUE, 0, 0, digest_hash_get_key,
                 &my_charset_bin);
    /* digest_hash.size= digest_max; */
    digest_hash_inited= true;
  }
  return 0;
}

void cleanup_digest_hash(void)
{
  if (digest_hash_inited)
  {
    lf_hash_destroy(&digest_hash);
    digest_hash_inited= false;
  }
}

static LF_PINS* get_digest_hash_pins(PFS_thread *thread)
{
  if (unlikely(thread->m_digest_hash_pins == NULL))
  {
    if (!digest_hash_inited)
      return NULL;
    thread->m_digest_hash_pins= lf_hash_get_pins(&digest_hash);
  }
  return thread->m_digest_hash_pins;
}

PFS_statement_stat*
find_or_create_digest(PFS_thread *thread,
                      const sql_digest_storage *digest_storage,
                      const char *schema_name,
                      uint schema_name_length)
{
  DBUG_ASSERT(digest_storage != NULL);

  if (statements_digest_stat_array == NULL)
    return NULL;

  if (digest_storage->m_byte_count <= 0)
    return NULL;

  LF_PINS *pins= get_digest_hash_pins(thread);
  if (unlikely(pins == NULL))
    return NULL;

  /*
    Note: the LF_HASH key is a block of memory,
    make sure to clean unused bytes,
    so that memcmp() can compare keys.
  */
  PFS_digest_key hash_key;
  memset(& hash_key, 0, sizeof(hash_key));
  /* Compute MD5 Hash of the tokens received. */
  compute_digest_md5(digest_storage, hash_key.m_md5);
  memcpy((void*)& digest_storage->m_md5, &hash_key.m_md5, MD5_HASH_SIZE);
  /* Add the current schema to the key */
  hash_key.m_schema_name_length= schema_name_length;
  if (schema_name_length > 0)
    memcpy(hash_key.m_schema_name, schema_name, schema_name_length);

  int res;
  uint retry_count= 0;
  const uint retry_max= 3;
  size_t safe_index;
  size_t attempts= 0;
  PFS_statements_digest_stat **entry;
  PFS_statements_digest_stat *pfs= NULL;

  ulonglong now= my_hrtime().val;

search:

  /* Lookup LF_HASH using this new key. */
  entry= reinterpret_cast<PFS_statements_digest_stat**>
    (lf_hash_search(&digest_hash, pins,
                    &hash_key, sizeof(PFS_digest_key)));

  if (entry && (entry != MY_ERRPTR))
  {
    /* If digest already exists, update stats and return. */
    pfs= *entry;
    pfs->m_last_seen= now;
    lf_hash_search_unpin(pins);
    return & pfs->m_stat;
  }

  lf_hash_search_unpin(pins);

  if (digest_full)
  {
    /*  digest_stat array is full. Add stat at index 0 and return. */
    pfs= &statements_digest_stat_array[0];
    digest_lost++;

    if (pfs->m_first_seen == 0)
      pfs->m_first_seen= now;
    pfs->m_last_seen= now;
    return & pfs->m_stat;
  }

  while (++attempts <= digest_max)
  {
    safe_index= PFS_atomic::add_u32(& digest_monotonic_index, 1) % digest_max;
    if (safe_index == 0)
    {
      /* Record [0] is reserved. */
      continue;
    }

    /* Add a new record in digest stat array. */
    DBUG_ASSERT(safe_index < digest_max);
    pfs= &statements_digest_stat_array[safe_index];

    if (pfs->m_lock.is_free())
    {
      if (pfs->m_lock.free_to_dirty())
      {
        /* Copy digest hash/LF Hash search key. */
        memcpy(& pfs->m_digest_key, &hash_key, sizeof(PFS_digest_key));

        /*
          Copy digest storage to statement_digest_stat_array so that it could be
          used later to generate digest text.
        */
        pfs->m_digest_storage.copy(digest_storage);

        pfs->m_first_seen= now;
        pfs->m_last_seen= now;

        res= lf_hash_insert(&digest_hash, pins, &pfs);
        if (likely(res == 0))
        {
          pfs->m_lock.dirty_to_allocated();
          return & pfs->m_stat;
        }

        pfs->m_lock.dirty_to_free();

        if (res > 0)
        {
          /* Duplicate insert by another thread */
          if (++retry_count > retry_max)
          {
            /* Avoid infinite loops */
            digest_lost++;
            return NULL;
          }
          goto search;
        }

        /* OOM in lf_hash_insert */
        digest_lost++;
        return NULL;
      }
    }
  }

  /* The digest array is now full. */
  digest_full= true;
  pfs= &statements_digest_stat_array[0];

  if (pfs->m_first_seen == 0)
    pfs->m_first_seen= now;
  pfs->m_last_seen= now;
  return & pfs->m_stat;
}

void purge_digest(PFS_thread* thread, PFS_digest_key *hash_key)
{
  LF_PINS *pins= get_digest_hash_pins(thread);
  if (unlikely(pins == NULL))
    return;

  PFS_statements_digest_stat **entry;

  /* Lookup LF_HASH using this new key. */
  entry= reinterpret_cast<PFS_statements_digest_stat**>
    (lf_hash_search(&digest_hash, pins,
                    hash_key, sizeof(PFS_digest_key)));

  if (entry && (entry != MY_ERRPTR))
  {
    lf_hash_delete(&digest_hash, pins,
                   hash_key, sizeof(PFS_digest_key));
  }
  lf_hash_search_unpin(pins);
  return;
}

void PFS_statements_digest_stat::reset_data(unsigned char *token_array, size_t length)
{
  m_lock.set_dirty();
  m_digest_storage.reset(token_array, length);
  m_stat.reset();
  m_first_seen= 0;
  m_last_seen= 0;
  m_lock.dirty_to_free();
}

void PFS_statements_digest_stat::reset_index(PFS_thread *thread)
{
  /* Only remove entries that exists in the HASH index. */
  if (m_digest_storage.m_byte_count > 0)
  {
    purge_digest(thread, & m_digest_key);
  }
}

void reset_esms_by_digest()
{
  if (statements_digest_stat_array == NULL)
    return;

  PFS_thread *thread= PFS_thread::get_current_thread();
  if (unlikely(thread == NULL))
    return;

  /* Reset statements_digest_stat_array. */
  for (size_t index= 0; index < digest_max; index++)
  {
    statements_digest_stat_array[index].reset_index(thread);
    statements_digest_stat_array[index].reset_data(statements_digest_token_array + index * pfs_max_digest_length, pfs_max_digest_length);
  }

  /* Mark record[0] as allocated again. */
  statements_digest_stat_array[0].m_lock.set_allocated();

  /*
    Reset index which indicates where the next calculated digest information
    to be inserted in statements_digest_stat_array.
  */
  PFS_atomic::store_u32(& digest_monotonic_index, 1);
  digest_full= false;
}

