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

/* Flags stored in the 'visible' byte at end of each record */
#define HP_ROW_ACTIVE    1   /* Bit 0: record is active (not deleted) */
#define HP_ROW_HAS_CONT  2   /* Bit 1: primary record has continuation chain(s) */
#define HP_ROW_IS_CONT   4   /* Bit 2: this record IS a continuation record */
#define HP_ROW_CONT_ZEROCOPY 8 /* Bit 3: zero-copy layout (data in rec 1..N-1) */
#define HP_ROW_SINGLE_REC   16 /* Bit 4: single-record run, no header -- data at offset 0 */
#define HP_ROW_MULTIPLE_REC  32 /* Bit 5: multi-run chain, data needs reassembly */

/*
  Continuation run header: next_cont pointer + run_rec_count.
  Stored at the beginning of the first blob segment in each run.
*/
#define HP_CONT_REC_COUNT_SIZE 2
#define HP_CONT_HEADER_SIZE    (sizeof(uchar*) + HP_CONT_REC_COUNT_SIZE)

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

/* Case A: single-record run, no header -- data at offset 0 */
static inline my_bool hp_is_single_rec(const uchar *rec, uint visible)
{
  return (rec[visible] & HP_ROW_SINGLE_REC) != 0;
}

/* Case B: single run, data in rec 1..N-1 -- zero-copy read */
static inline my_bool hp_is_zerocopy(const uchar *rec, uint visible)
{
  return (rec[visible] & HP_ROW_CONT_ZEROCOPY) != 0;
}

/* Case C: multi-run chain -- data needs reassembly into blob_buff */
static inline my_bool hp_is_multi_run(const uchar *rec, uint visible)
{
  return (rec[visible] & HP_ROW_MULTIPLE_REC) != 0;
}

/*
  Continuation run header accessors.
  Read next_cont pointer and run_rec_count from the first record of a run.
  In heap, all delete/chain link pointers in records are 8 byte aligned
  and thus safe to access directly.
*/

static inline const uchar *hp_cont_next(const uchar *chain)
{
  return *((const uchar**) chain);
}

static inline uint16 hp_cont_rec_count(const uchar *chain)
{
  return uint2korr(chain + sizeof(uchar*));
}

/*
  Blob continuation run storage format.

  Case A (HP_ROW_SINGLE_REC):     Single-record run, no header.
      Data starts at offset 0, full `visible` bytes available for
      payload.  Detected by hp_is_single_rec().
      Zero-copy: blob pointer -> chain.

  Case B (HP_ROW_CONT_ZEROCOPY):  Single run, multiple records.
      Header in rec 0, data contiguous in rec 1..N-1.  Detected by
      hp_is_zerocopy().
      Zero-copy: blob pointer -> chain + recbuffer.

  Case C (HP_ROW_MULTIPLE_REC):   One or more runs linked via
      next_cont.  Header in each run's rec 0, data in rec 0 (after
      header) + rec 1..N-1.  Detected by hp_is_multi_run().
      Requires reassembly into blob_buff.
*/

/*
  Minimum contiguous run size parameters.
  Runs smaller than this are not worth scavenging from the
  delete list because the per-run header overhead (10 bytes)
  becomes a significant fraction of payload.  Skip them and
  allocate from the tail instead.

  HP_CONT_MIN_RUN_BYTES: absolute floor for minimum run payload.
  HP_CONT_RUN_FRACTION_NUM/DEN: minimum run size as a fraction
    of blob size.
    min_run_bytes = MAX(blob_length * NUM / DEN,
                        HP_CONT_MIN_RUN_BYTES)
*/
#define HP_CONT_MIN_RUN_BYTES  128
#define HP_CONT_RUN_FRACTION_NUM  1
#define HP_CONT_RUN_FRACTION_DEN  10

static inline uint32 hp_blob_min_run_bytes(uint32 blob_length)
{
  uint32 length= (blob_length / HP_CONT_RUN_FRACTION_DEN *
                  HP_CONT_RUN_FRACTION_NUM);
  set_if_bigger(length, HP_CONT_MIN_RUN_BYTES);
  set_if_smaller(length, blob_length);
  return length;
}

/*
  Delete-list block metadata.

  Deleted records store a del_link pointer in bytes 0..sizeof(uchar*)-1
  and a flags byte at offset 'visible'.  Block coalescing reuses bytes
  after del_link for block metadata:
    byte HP_DEL_FLAG_OFFSET   (8):  del_flag (block-start / block-end bits)
    bytes HP_DEL_COUNT_OFFSET (9-10): block record count (uint16, LE)

  HP_DEL_METADATA_SIZE (11) is the minimum 'visible' offset needed to
  guarantee these bytes never overlap with the flags byte.
*/
#define HP_DEL_FLAG_OFFSET    sizeof(uchar*)
#define HP_DEL_BLOCK_END      1              /* bit 0: last record of block */
#define HP_DEL_BLOCK_START    2              /* bit 1: first record of block */
#define HP_DEL_COUNT_OFFSET   (sizeof(uchar*) + 1)
#define HP_DEL_METADATA_SIZE  (sizeof(uchar*) + 1 + HP_CONT_REC_COUNT_SIZE)

/*
  Free-list block predicates and accessors.
  A contiguous group of N >= 2 deleted records is represented as a block:
    rec[0]     (first): del_flag = HP_DEL_BLOCK_START, del_link = next entry,
                        count at bytes 9-10
    rec[1..N-2] (dark): bytes 0-10 zeroed, pos[visible] = 0
    rec[N-1]   (last):  del_flag = HP_DEL_BLOCK_END, del_link = &rec[0]
    share->del_link = &rec[N-1]
  Single deleted records have del_flag = 0 (unchanged behavior).
*/

static inline my_bool hp_is_free_block_end(const uchar *pos)
{
  return (pos[HP_DEL_FLAG_OFFSET] & HP_DEL_BLOCK_END) != 0;
}

static inline uchar *hp_free_block_first(uchar *pos)
{
  return *((uchar**) pos);
}

static inline my_bool hp_is_free_block_start(const uchar *pos)
{
  return (pos[HP_DEL_FLAG_OFFSET] & HP_DEL_BLOCK_START) != 0;
}

static inline uint16 hp_free_block_start_count(const uchar *pos)
{
  return uint2korr(pos + HP_DEL_COUNT_OFFSET);
}

/*
  Clear the metadata bytes of dark records (records between block-start
  and block-end that are not individually on the free list).
  Uses a strided loop so only the essential bytes per record are touched.
  For short record lengths a contiguous bzero over the full range would
  be faster, but the crossover point has not been measured.
*/

static inline void hp_clear_dark_records(uchar *from, uchar *to,
                                         uint recbuffer, uint visible)
{
  uchar *pos;
  for (pos= from; pos < to; pos+= recbuffer)
  {
    *((uchar**) pos)= NULL;
    pos[HP_DEL_FLAG_OFFSET]= 0;
    pos[visible]= 0;
  }
}

/*
  Push a single deleted record onto the free list.
  Replaces the inlined 3-line pattern in heap_delete(), heap_write()
  error path, and hp_free_run_chain() for single-record runs.
*/

static inline void hp_push_free_record(HP_SHARE *share, uchar *pos)
{
  *((uchar**) pos)= share->del_link;
  pos[HP_DEL_FLAG_OFFSET]= 0;
  share->del_link= pos;
  pos[share->visible]= 0;
  share->deleted++;
  share->total_records--;
}

/*
  Push a contiguous block of deleted records onto the free list.
  Defined in hp_delete.c.
*/
extern void hp_push_free_block(HP_SHARE *share, uchar *first, uint16 count);

/*
  Push a contiguous block (count >= 2) or single record (count == 1)
  onto the free list, coalescing with the head entry if adjacent.
  Defined in hp_delete.c.
*/
extern void hp_push_free_block_coalesce(HP_SHARE *share, uchar *first,
                                        uint16 count);

/*
  Push a deleted record onto the free list, coalescing with the head
  entry if adjacent.  Delegates to hp_push_free_block_coalesce with
  count=1; both merge branches produce correct results for a single
  record (dark-record clearing ranges become empty no-ops where no
  dark records exist).
*/

static inline void hp_push_free_record_coalesce(HP_SHARE *share, uchar *pos)
{
  hp_push_free_block_coalesce(share, pos, 1);
}

/*
  Take 'count' contiguous records from the head block.
  Defined in hp_write.c.
*/
extern uchar *hp_take_free_block(HP_SHARE *share, uint16 count);

/*
  Pop one record from the head of the free list.
  If the head is a block-end, takes the last record and shrinks
  or collapses the block.
*/

static inline uchar *hp_pop_free_record(HP_SHARE *share)
{
  uchar *pos= share->del_link;

  DBUG_ASSERT(pos);

  if (!hp_is_free_block_end(pos))
  {
    /* Single record */
    share->del_link= *((uchar**) pos);
    share->deleted--;
    share->total_records++;
    return pos;
  }

  return hp_take_free_block(share, 1);
}

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
extern ulong hp_rec_hashnr(HP_INFO *info, HP_KEYDEF *keyinfo,const uchar *rec);
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

extern uchar *hp_alloc_from_tail(HP_SHARE *info, uint *blocks);
extern uchar *next_free_record_pos(HP_SHARE *info);
static inline uint32 hp_blob_length(const HP_BLOB_DESC *desc,
                                    const uchar *record)
{
  return (uint32) read_lowendian(record + desc->offset, desc->packlength);
}
extern int hp_write_one_blob(HP_SHARE *share, const uchar *data_ptr,
                             uint32 data_len, uchar **first_run_out);
extern int hp_write_blobs(HP_INFO *info, const uchar *record, uchar *pos);
extern int hp_read_blobs(HP_INFO *info, uchar *record, const uchar *pos);
extern void hp_free_blobs(HP_SHARE *share, uchar *pos);
extern void hp_free_run_chain(HP_SHARE *share, uchar *chain);
extern void hp_shrink_tail(HP_SHARE *share);
extern void hp_flush_pending_blob_free_impl(HP_INFO *info);

static inline void hp_flush_pending_blob_free(HP_INFO *info)
{
  if (info->has_pending_blob_free)
    hp_flush_pending_blob_free_impl(info);
}

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
