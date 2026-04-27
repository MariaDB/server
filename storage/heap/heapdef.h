/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* This file is included in all heap-files */

#include <my_global.h>
#include <my_base.h>
C_MODE_START
#include <my_pthread.h>
#include "heap.h"			/* Structs & some defines */
#include "my_tree.h"

/*
  When allocating keys /rows in the internal block structure, do it
  within the following boundaries.

  The challenge is to find the balance between allocate as few blocks
  as possible and keep memory consumption down.
*/

#define HP_MIN_RECORDS_IN_BLOCK 16
#define HP_MAX_RECORDS_IN_BLOCK 8192

#define HP_ROW_ACTIVE    1   /* Bit 0: record is active (not deleted) */
#define HP_ROW_HAS_CONT  2   /* Bit 1: primary record has continuation chain(s) */
#define HP_ROW_IS_CONT   4   /* Bit 2: this record IS a continuation record */
#define HP_ROW_CONT_ZEROCOPY 8 /* Bit 3: zero-copy layout (data in rec 1..N-1) */
#define HP_ROW_SINGLE_REC   16 /* Bit 4: single-record run, no header — data at offset 0 */

/*
  Continuation run header: next_cont pointer + run_rec_count.
  Stored at the beginning of the first record in each run.
*/
#define HP_CONT_NEXT_PTR_SIZE  sizeof(uchar*)
#define HP_CONT_REC_COUNT_SIZE sizeof(uint16)
#define HP_CONT_HEADER_SIZE    (HP_CONT_NEXT_PTR_SIZE + HP_CONT_REC_COUNT_SIZE)

/*
  Minimum contiguous run size parameters.
  Runs smaller than this are not worth scavenging from the free list because
  the per-run header overhead (10 bytes) becomes a significant fraction of
  payload.  Skip them and allocate from the tail instead.

  HP_CONT_MIN_RUN_BYTES: absolute floor for minimum run payload.
  HP_CONT_RUN_FRACTION_NUM/DEN: minimum run size as a fraction of blob size.
    min_run_bytes = MAX(blob_length * NUM / DEN, HP_CONT_MIN_RUN_BYTES)
*/
/*
  Row flags byte predicates.
  The flags byte is at offset 'visible' in each primary or run-header record.
*/

/* Record is active (not deleted) */
static inline my_bool hp_is_active(const uchar *rec, uint visible)
{
  return (rec[visible] & HP_ROW_ACTIVE) != 0;
}

/* Primary record that owns blob continuation chain(s) */
static inline my_bool hp_has_cont(const uchar *rec, uint visible)
{
  return (rec[visible] & HP_ROW_HAS_CONT) != 0;
}

/* This record IS a continuation run header (rec 0 of a run) */
static inline my_bool hp_is_cont(const uchar *rec, uint visible)
{
  return (rec[visible] & HP_ROW_IS_CONT) != 0;
}

/*
  Continuation run header accessors.
  Read next_cont pointer and run_rec_count from the first record of a run.
*/
static inline const uchar *hp_cont_next(const uchar *chain)
{
  const uchar *next;
  memcpy(&next, chain, HP_CONT_NEXT_PTR_SIZE);
  return next;
}

static inline uint16 hp_cont_rec_count(const uchar *chain)
{
  return uint2korr(chain + HP_CONT_NEXT_PTR_SIZE);
}

/*
  Blob continuation run storage format.

  Case A (HP_BLOB_CASE_A_SINGLE_REC):  Single-record run, no header.
      Data starts at offset 0, full `visible` bytes available for
      payload.  Detected by HP_ROW_SINGLE_REC flag.
      Zero-copy: blob pointer → chain.

  Case B (HP_BLOB_CASE_B_ZEROCOPY):    Single run, multiple records.
      Header in rec 0, data contiguous in rec 1..N-1.  Detected by
      HP_ROW_CONT_ZEROCOPY flag.
      Zero-copy: blob pointer → chain + recbuffer.

  Case C (HP_BLOB_CASE_C_MULTI_RUN):   One or more runs linked via
      next_cont.  Header in each run's rec 0, data in rec 0 (after
      header) + rec 1..N-1.  Requires reassembly into blob_buff.
*/
enum hp_blob_format {
  HP_BLOB_CASE_A_SINGLE_REC,
  HP_BLOB_CASE_B_ZEROCOPY,
  HP_BLOB_CASE_C_MULTI_RUN
};

static inline enum hp_blob_format hp_blob_run_format(const uchar *chain,
                                                     uint visible)
{
  uchar flags= chain[visible];
  if (flags & HP_ROW_SINGLE_REC)
    return HP_BLOB_CASE_A_SINGLE_REC;
  if (flags & HP_ROW_CONT_ZEROCOPY)
    return HP_BLOB_CASE_B_ZEROCOPY;
  return HP_BLOB_CASE_C_MULTI_RUN;
}

/* Minimum acceptable contiguous run size in bytes for free list reuse */
#define HP_CONT_MIN_RUN_BYTES  128
/* Minimum run size as a fraction of blob size: NUM/DEN = 1/10 */
#define HP_CONT_RUN_FRACTION_NUM  1
#define HP_CONT_RUN_FRACTION_DEN  10

	/* Some extern variables */

extern LIST *heap_open_list,*heap_share_list;

#define test_active(info) \
if (!(info->update & HA_STATE_AKTIV))\
{ my_errno=HA_ERR_NO_ACTIVE_RECORD; DBUG_RETURN(-1); }
#define hp_find_hash(A,B) ((HASH_INFO*) hp_find_block((A),(B)))

	/* Find pos for record and update it in info->current_ptr */
#define hp_find_record(info,pos) (info)->current_ptr= hp_find_block(&(info)->s->block,pos)

typedef struct st_hp_hash_info
{
  struct st_hp_hash_info *next_key;
  uchar *ptr_to_rec;
  ulong hash_of_key;
} HASH_INFO;

typedef struct {
  HA_KEYSEG *keyseg;
  uint key_length;
  uint search_flag;
} heap_rb_param;
      
	/* Prototypes for intern functions */

extern HP_SHARE *hp_find_named_heap(const char *name);
extern int hp_rectest(HP_INFO *info,const uchar *old);
extern uchar *hp_find_block(HP_BLOCK *info,ulong pos);
extern int hp_get_new_block(HP_SHARE *info, HP_BLOCK *block,
                            size_t* alloc_length);
extern void hp_free(HP_SHARE *info);
extern uchar *hp_free_level(HP_BLOCK *block,uint level,HP_PTRS *pos,
			   uchar *last_pos);
extern int hp_write_key(HP_INFO *info, HP_KEYDEF *keyinfo,
			const uchar *record, uchar *recpos);
extern int hp_rb_write_key(HP_INFO *info, HP_KEYDEF *keyinfo, 
			   const uchar *record, uchar *recpos);
extern int hp_rb_delete_key(HP_INFO *info,HP_KEYDEF *keyinfo,
			    const uchar *record,uchar *recpos,int flag);
extern int hp_delete_key(HP_INFO *info,HP_KEYDEF *keyinfo,
			 const uchar *record,uchar *recpos,int flag);
extern HASH_INFO *_heap_find_hash(HP_BLOCK *block,ulong pos);
extern uchar *hp_search(HP_INFO *info,HP_KEYDEF *keyinfo,const uchar *key,
		       uint nextflag);
extern uchar *hp_search_next(HP_INFO *info, HP_KEYDEF *keyinfo,
			    const uchar *key, HASH_INFO *pos);
extern ulong hp_rec_hashnr(HP_KEYDEF *keyinfo,const uchar *rec);
extern void hp_movelink(HASH_INFO *pos,HASH_INFO *next_link,
			 HASH_INFO *newlink);
extern int hp_rec_key_cmp(HP_KEYDEF *keydef,const uchar *rec1,
			  const uchar *rec2, HP_INFO *info);
extern int hp_key_cmp(HP_KEYDEF *keydef,const uchar *rec,
		      const uchar *key, HP_INFO *info);
extern const uchar *hp_materialize_one_blob(HP_INFO *info,
                                            const uchar *chain,
                                            uint32 data_len);
extern void hp_make_key(HP_KEYDEF *keydef,uchar *key,const uchar *rec);
extern uint hp_rb_make_key(HP_KEYDEF *keydef, uchar *key,
			   const uchar *rec, uchar *recpos);
extern uint hp_rb_key_length(HP_KEYDEF *keydef, const uchar *key);
extern uint hp_rb_null_key_length(HP_KEYDEF *keydef, const uchar *key);
extern uint hp_rb_var_key_length(HP_KEYDEF *keydef, const uchar *key);
extern my_bool hp_if_null_in_key(HP_KEYDEF *keyinfo, const uchar *record);
extern int hp_close(HP_INFO *info);
extern void hp_clear(HP_SHARE *info);
extern void hp_clear_keys(HP_SHARE *info);
extern uint hp_rb_pack_key(HP_KEYDEF *keydef, uchar *key, const uchar *old,
                           key_part_map keypart_map);
extern ha_rows hp_rows_in_memory(size_t reclength, size_t index_size,
                          size_t memory_limit);
extern size_t hp_memory_needed_per_row(size_t reclength);

extern uchar *next_free_record_pos(HP_SHARE *info);
extern uint32 hp_blob_length(const HP_BLOB_DESC *desc, const uchar *record);
extern int hp_write_one_blob(HP_SHARE *share, const uchar *data_ptr,
                             uint32 data_len, uchar **first_run_out);
extern int hp_write_blobs(HP_INFO *info, const uchar *record, uchar *pos);
extern int hp_read_blobs(HP_INFO *info, uchar *record, const uchar *pos);
extern void hp_free_blobs(HP_SHARE *share, uchar *pos);
extern void hp_free_run_chain(HP_SHARE *share, uchar *chain);

extern mysql_mutex_t THR_LOCK_heap;

extern PSI_memory_key hp_key_memory_HP_SHARE;
extern PSI_memory_key hp_key_memory_HP_INFO;
extern PSI_memory_key hp_key_memory_HP_PTRS;
extern PSI_memory_key hp_key_memory_HP_KEYDEF;
extern PSI_memory_key hp_key_memory_HP_BLOB;

#ifdef HAVE_PSI_INTERFACE
void init_heap_psi_keys();
#else
#define init_heap_psi_keys() do { } while(0)
#endif /* HAVE_PSI_INTERFACE */

C_MODE_END

/*
  Calculate position number for hash value.
  SYNOPSIS
    hp_mask()
      hashnr     Hash value
      buffmax    Value such that
                 2^(n-1) < maxlength <= 2^n = buffmax
      maxlength

  RETURN
    Array index, in [0..maxlength)
*/

static inline ulong hp_mask(ulong hashnr, ulong buffmax, ulong maxlength)
{
  if ((hashnr & (buffmax-1)) < maxlength) return (hashnr & (buffmax-1));
  return (hashnr & ((buffmax >> 1) -1));
}
