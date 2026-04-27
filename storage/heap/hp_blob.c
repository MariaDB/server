/* Copyright (c) 2025, MariaDB Corporation.

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

/*
  LOB (BLOB/TEXT) support for HEAP tables using variable-length
  continuation runs.

  Each blob column's data is stored as a chain of continuation "runs".
  A run is a contiguous sequence of recbuffer-sized records in the same
  HP_BLOCK.  The first record of each run stores a header (next_cont
  pointer + run_rec_count); subsequent records carry pure blob payload.
  Runs are linked together via the next_cont pointer.

  This design amortizes the per-run header overhead across many records,
  giving near-100% space efficiency for typical blob sizes (150 KB and
  above), even when recbuffer is very small (e.g. 16 bytes).
*/

#include "heapdef.h"
#include <my_stacktrace.h>
#include <string.h>


/*
  Read blob data length from the record buffer.
*/

uint32 hp_blob_length(const HP_BLOB_DESC *desc, const uchar *record)
{
  switch (desc->packlength)
  {
  case 1:
    return (uint32) record[desc->offset];
  case 2:
    return uint2korr(record + desc->offset);
  case 3:
    return uint3korr(record + desc->offset);
  case 4:
    return uint4korr(record + desc->offset);
  default:
    DBUG_ASSERT(0);
    return 0;
  }
}


/*
  Allocate one record from the HP_BLOCK tail, bypassing the free list.
  Same accounting as next_free_record_pos() but never uses del_link.

  Maintains the scan-boundary invariant:
      total_records + deleted == block.last_allocated
  by incrementing both last_allocated and total_records together.
  heap_scan() relies on this invariant to know when to stop scanning.
*/

static uchar *hp_alloc_from_tail(HP_SHARE *share)
{
  int block_pos;
  size_t length;

  if (!(block_pos= (share->block.last_allocated %
                     share->block.records_in_block)))
  {
    if ((share->block.last_allocated > share->max_records &&
         share->max_records) ||
        (share->data_length + share->index_length >= share->max_table_size))
    {
      my_errno= HA_ERR_RECORD_FILE_FULL;
      return NULL;
    }
    if (hp_get_new_block(share, &share->block, &length))
      return NULL;
    share->data_length+= length;
  }
  share->block.last_allocated++;
  share->total_records++;
  return (uchar*) share->block.level_info[0].last_blocks +
         block_pos * share->block.recbuffer;
}


/*
  Free one continuation chain of variable-length runs.

  Walks from the first run, reads run_rec_count from each, frees all
  records individually to the free list, then follows next_cont to the
  next run.

  Maintains the scan-boundary invariant:
      total_records + deleted == block.last_allocated
  Each freed slot does total_records-- and deleted++, keeping the sum
  constant.  heap_scan() relies on this sum to know when to stop.

  @param share  Table share
  @param chain  Pointer to first record of first run (or NULL)
*/

void hp_free_run_chain(HP_SHARE *share, uchar *chain)
{
  uint recbuffer= share->block.recbuffer;
  uint visible= share->visible;

  while (chain)
  {
    uchar *next_run;
    uint16 run_rec_count;
    uint16 j;

    if (hp_blob_run_format(chain, visible) == HP_BLOB_CASE_A_SINGLE_REC)
    {
      /* Case A: single record, no header */
      next_run= NULL;
      run_rec_count= 1;
    }
    else
    {
      /* Case B/C: header present with next_cont and run_rec_count */
      memcpy(&next_run, chain, HP_CONT_NEXT_PTR_SIZE);
      run_rec_count= uint2korr(chain + HP_CONT_NEXT_PTR_SIZE);
    }

    for (j= 0; j < run_rec_count; j++)
    {
      uchar *pos= chain + j * recbuffer;
      *((uchar**) pos)= share->del_link;
      share->del_link= pos;
      pos[visible]= 0;
      share->deleted++;
      share->total_records--;
    }

    chain= next_run;
  }
}


/*
  Write blob data into a contiguous run of records.

  Writes the run header (next_cont=NULL, run_rec_count) in the first
  record, then copies blob data across all records in the run,
  advancing *offset.

  @param share         Table share
  @param data          Source blob data
  @param data_len      Total blob data length
  @param run_start     Pointer to first record of the run
  @param run_rec_count Number of consecutive records in this run
  @param format        Storage format (Case A / Case B / Case C)
  @param offset        [in/out] Current offset into blob data

  @note Caller must link runs by overwriting next_cont in the previous run.
*/

static void hp_write_run_data(HP_SHARE *share, const uchar *data,
                              uint32 data_len, uchar *run_start,
                              uint16 run_rec_count,
                              enum hp_blob_format format,
                              uint32 *offset)
{
  uint visible= share->visible;
  uint recbuffer= share->block.recbuffer;
  uint32 off= *offset;
  uint32 remaining= data_len - off;
  uint32 chunk;
  uint16 rec;

  if (format == HP_BLOB_CASE_A_SINGLE_REC)
  {
    /*
      Case A: single-record run, no header.  Data starts at offset 0,
      full `visible` bytes available.  HP_ROW_SINGLE_REC signals the
      reader that there is no run header to parse.
    */
    DBUG_ASSERT(run_rec_count == 1);
    DBUG_ASSERT(remaining <= visible);
    run_start[visible]= HP_ROW_ACTIVE | HP_ROW_IS_CONT | HP_ROW_SINGLE_REC;
    memcpy(run_start, data + off, remaining);
    *offset= off + remaining;
    return;
  }

  {
    uchar *null_ptr= NULL;
    /* First record: run header + flags byte */
    memcpy(run_start, &null_ptr, HP_CONT_NEXT_PTR_SIZE);
    int2store(run_start + HP_CONT_NEXT_PTR_SIZE, run_rec_count);
    run_start[visible]= HP_ROW_ACTIVE | HP_ROW_IS_CONT |
                         (format == HP_BLOB_CASE_B_ZEROCOPY
                          ? HP_ROW_CONT_ZEROCOPY : 0);
  }

  /*
    Case B: skip data copy in rec 0.
    All data goes into rec 1..N-1 contiguously for zero-copy reads.
    Case C: data starts in rec 0 after header.
  */
  if (format == HP_BLOB_CASE_C_MULTI_RUN)
  {
    chunk= visible - HP_CONT_HEADER_SIZE;
    if (chunk > remaining)
      chunk= remaining;
    memcpy(run_start + HP_CONT_HEADER_SIZE, data + off, chunk);
    off+= chunk;
    remaining-= chunk;
  }

  /*
    Inner records (rec 1..N-1): full recbuffer payload, no flags byte.
    This makes data in inner records contiguous, enabling zero-copy reads
    for single-run blobs (Case B).
  */
  for (rec= 1; rec < run_rec_count && remaining > 0; rec++)
  {
    uchar *rec_ptr= run_start + rec * recbuffer;
    chunk= recbuffer;
    if (chunk > remaining)
      chunk= remaining;
    memcpy(rec_ptr, data + off, chunk);
    off+= chunk;
    remaining-= chunk;
  }

  *offset= off;
}


/*
  Unlink a contiguous group from the free list and write blob data into it.

  @param share          Table share
  @param data_ptr       Blob data
  @param data_len       Total blob data length
  @param run_start      Lowest address of the contiguous group
  @param run_count      Number of contiguous records in the group
  @param visible        share->visible
  @param recbuffer      share->block.recbuffer
  @param data_offset    [in/out] Current offset into blob data
  @param first_run      [in/out] Pointer to first run (NULL initially)
  @param prev_run_start [in/out] Pointer to previous run's start
*/

static void hp_unlink_and_write_run(HP_SHARE *share, const uchar *data_ptr,
                                    uint32 data_len, uchar *run_start,
                                    uint16 run_count, uint visible,
                                    uint recbuffer, uint32 *data_offset,
                                    uchar **first_run, uchar **prev_run_start)
{
  uint32 remaining= data_len - *data_offset;
  uint32 records_needed;
  uint16 records_to_use;
  uint32 unlinked= 0;
  uchar **prev_link= &share->del_link;
  uchar *cur;
  uint32 first_payload= visible - HP_CONT_HEADER_SIZE;

  if (remaining <= first_payload)
    records_needed= 1;
  else
    records_needed= 1 + (remaining - first_payload + recbuffer - 1) / recbuffer;
  records_to_use= (records_needed > run_count) ? run_count :
                  (uint16) records_needed;

  cur= share->del_link;
  while (cur && unlinked < records_to_use)
  {
    uchar *next= *((uchar**) cur);
    if (cur >= run_start &&
        cur < run_start + records_to_use * recbuffer)
    {
      *prev_link= next;
      share->deleted--;
      share->total_records++;
      unlinked++;
    }
    else
      prev_link= (uchar**) cur;
    cur= next;
  }

  hp_write_run_data(share, data_ptr, data_len, run_start,
                    records_to_use, HP_BLOB_CASE_C_MULTI_RUN, data_offset);

  if (*prev_run_start)
    memcpy(*prev_run_start, &run_start, sizeof(run_start));
  else
    *first_run= run_start;
  *prev_run_start= run_start;
}


/*
  Write one blob column's data into a chain of continuation runs.

  Allocates contiguous runs from the free list and/or block tail,
  copies blob data into them, and returns the first run pointer.
  On failure, frees any partially allocated chain.

  @param share         Table share
  @param data_ptr      Blob data to write
  @param data_len      Blob data length (must be > 0)
  @param first_run_out [out] Pointer to first run's first record

  @return 0 on success, my_errno on failure
*/

int hp_write_one_blob(HP_SHARE *share, const uchar *data_ptr,
                      uint32 data_len, uchar **first_run_out)
{
  uint visible= share->visible;
  uint recbuffer= share->block.recbuffer;
  uint32 min_run_bytes;
  uint32 min_run_records;
  uchar *first_run= NULL;
  uchar *prev_run_start= NULL;
  uint32 data_offset= 0;

  /*
    Calculate minimum acceptable contiguous run size for free-list reuse.

    The free-list walk (Step 1 below) rejects contiguous groups smaller
    than min_run_records, bailing to tail allocation instead.  This
    prevents excessive chain fragmentation for large blobs: accepting
    tiny fragments would produce long chains of many short runs, each
    with its own 10-byte header overhead and pointer dereference on read.

    The threshold is the larger of:
      - 1/10 of the blob size (caps chain length at ~10 runs)
      - 128 bytes absolute floor (HP_CONT_MIN_RUN_BYTES)
      - 2 records minimum (a single-slot run is pure overhead)

    For small blobs whose total bytes or records needed is below this
    threshold, the fragmentation concern doesn't apply — the entire blob
    fits in one short run.  Cap both min_run_bytes and min_run_records
    so the free list can satisfy the allocation without falling through
    to the tail unnecessarily.
  */
  min_run_bytes= data_len / HP_CONT_RUN_FRACTION_DEN *
                 HP_CONT_RUN_FRACTION_NUM;
  if (min_run_bytes < HP_CONT_MIN_RUN_BYTES)
    min_run_bytes= HP_CONT_MIN_RUN_BYTES;
  if (min_run_bytes > data_len)
    min_run_bytes= data_len;
  min_run_records= (min_run_bytes + recbuffer - 1) / recbuffer;
  if (min_run_records < 2)
    min_run_records= 2;
  {
    uint32 first_payload= visible - HP_CONT_HEADER_SIZE;
    uint32 total_records_needed= data_len <= first_payload ? 1 :
        1 + (data_len - first_payload + recbuffer - 1) / recbuffer;
    if (total_records_needed < min_run_records)
      min_run_records= total_records_needed;
  }

  /*
    Step 1: Try to allocate contiguous runs from the free list.

    Peek at free list records by walking next pointers without unlinking.
    Track contiguous groups (descending addresses — LIFO order from
    hp_free_run_chain).  On discontinuity: if the group qualifies
    (>= min_run_records), unlink and use it; if it doesn't, the free
    list is too fragmented — stop and fall through to tail allocation.
  */
  {
    uchar *run_start= NULL;
    uint16 run_count= 0;
    uchar *prev_pos= NULL;
    uchar *pos;

    for (pos= share->del_link;
         pos && data_offset < data_len;
         pos= *((uchar**) pos))
    {
      /*
        Only check descending direction: hp_free_run_chain() frees records
        in ascending address order (j=0..N), so LIFO pushes them onto the
        free list in reverse — consecutive free list entries have descending
        addresses.  Ascending adjacency from unrelated deletes is ignored
        intentionally; we only recover runs that were freed together.
      */
      if (prev_pos && pos == prev_pos - recbuffer && run_count < UINT_MAX16)
      {
        run_start= pos;
        run_count++;
        prev_pos= pos;
        continue;
      }

      /*
        Discontinuity.  If the accumulated group qualifies, use it.
        If not, the free list is fragmented — give up entirely.
      */
      if (run_count > 0)
      {
        if (run_count < min_run_records)
          break;
        hp_unlink_and_write_run(share, data_ptr, data_len, run_start,
                                run_count, visible, recbuffer,
                                &data_offset, &first_run, &prev_run_start);
      }

      run_start= pos;
      run_count= 1;
      prev_pos= pos;
    }

    /* Handle the last group after the loop ends */
    if (run_count >= min_run_records && data_offset < data_len)
      hp_unlink_and_write_run(share, data_ptr, data_len, run_start,
                              run_count, visible, recbuffer,
                              &data_offset, &first_run, &prev_run_start);
  }

  /*
    Step 2: Allocate remaining data from the block tail.

    Tail allocation is always contiguous within a leaf block.
    When we hit a block boundary, we start a new run.
  */
  while (data_offset < data_len)
  {
    uchar *run_start;
    uint16 run_rec_count;
    uint32 remaining= data_len - data_offset;
    uint32 run_payload;
    my_bool is_only_run;

    run_start= hp_alloc_from_tail(share);
    if (!run_start)
      goto err;
    run_rec_count= 1;

    /* Extend the run with consecutive tail records */
    for (;;)
    {
      uint block_pos;

      if (run_rec_count == 1)
        run_payload= visible;             /* HP_ROW_SINGLE_REC: no header */
      else
        run_payload= (visible - HP_CONT_HEADER_SIZE) +
                      (uint32)(run_rec_count - 1) * recbuffer;
      if (run_payload >= remaining)
        break;

      /*
        Check if the next record would be in the same leaf block.
        block_pos == 0 means last_allocated is at a block boundary
        and the next allocation would start a new block.
      */
      block_pos= share->block.last_allocated %
                 share->block.records_in_block;
      if (block_pos == 0)
        break;

      {
        uchar *next_rec= hp_alloc_from_tail(share);
        if (!next_rec)
          break;
        /*
          Contiguity guard (active in all builds, not just debug).

          Blob continuation runs use pointer arithmetic (run_start +
          i * recbuffer) to access inner records in the write, read,
          zero-copy, scan-skip, and free paths.  Today, contiguity
          within a leaf block is guaranteed by hp_get_new_block()
          allocating a single flat array of records_in_block * recbuffer
          bytes, and hp_alloc_from_tail() handing them out sequentially.
          But this is an implementation detail of HP_BLOCK, not a
          documented contract.  A future change (e.g. sub-block
          allocation, memory pooling, or alignment padding between
          records) could silently break this assumption, turning every
          blob path into a source of data corruption.  Abort here so
          such a change is caught immediately by any test that exercises
          blob writes.
        */
        if (unlikely(next_rec !=
                     run_start + (uint32) run_rec_count * recbuffer))
        {
          my_safe_printf_stderr(
            "HEAP blob: tail allocation not contiguous: "
            "expected %p, got %p (run_start=%p, count=%u, recbuffer=%u)\n",
            run_start + (uint32) run_rec_count * recbuffer,
            next_rec, run_start, (uint) run_rec_count, recbuffer);
          abort();
        }
        run_rec_count++;
      }
    }

    is_only_run= (first_run == NULL && prev_run_start == NULL);

    if (is_only_run && run_payload >= remaining)
    {
      /*
        Single-run blob — use zero-copy layout if possible.
        Case A: data fits in rec 0 payload (run_rec_count == 1).
        Case B: data in rec 1..N-1 only, contiguous for zero-copy reads.
      */
      if (run_rec_count == 1)
      {
        /* Case A: data fits in rec 0 */
        hp_write_run_data(share, data_ptr, data_len, run_start,
                          run_rec_count, HP_BLOB_CASE_A_SINGLE_REC,
                          &data_offset);
      }
      else
      {
        uint32 case_b_payload= (uint32)(run_rec_count - 1) * recbuffer;
        if (case_b_payload >= remaining)
        {
          /* Case B: rec 1..N-1 alone hold all data */
          hp_write_run_data(share, data_ptr, data_len, run_start,
                            run_rec_count, HP_BLOB_CASE_B_ZEROCOPY,
                            &data_offset);
        }
        else
        {
          /*
            Case B needs one more record than Case C.  Try to extend
            if we're not at a block boundary.
          */
          uint block_pos= share->block.last_allocated %
                           share->block.records_in_block;
          if (block_pos != 0)
          {
            uchar *extra= hp_alloc_from_tail(share);
            if (extra)
            {
              /*
                Contiguity guard for the Case B extra record, same
                rationale as the main extension loop ~60 lines above:
                hp_get_new_block() today allocates flat arrays but this
                is an HP_BLOCK implementation detail, not a contract.
                A future change could break contiguity and silently
                corrupt every blob read/write/free path that relies on
                run_start + i * recbuffer arithmetic.
              */
              if (unlikely(extra !=
                           run_start + (uint32) run_rec_count * recbuffer))
              {
                my_safe_printf_stderr(
                  "HEAP blob: Case B extra allocation not contiguous: "
                  "expected %p, got %p "
                  "(run_start=%p, count=%u, recbuffer=%u)\n",
                  run_start + (uint32) run_rec_count * recbuffer,
                  extra, run_start, (uint) run_rec_count, recbuffer);
                abort();
              }
              run_rec_count++;
              hp_write_run_data(share, data_ptr, data_len, run_start,
                                run_rec_count, HP_BLOB_CASE_B_ZEROCOPY,
                                &data_offset);
            }
            else
              /* Case B extension failed — fall back to Case C */
              hp_write_run_data(share, data_ptr, data_len, run_start,
                                run_rec_count, HP_BLOB_CASE_C_MULTI_RUN,
                                &data_offset);
          }
          else
            /* At block boundary — Case C */
            hp_write_run_data(share, data_ptr, data_len, run_start,
                              run_rec_count, HP_BLOB_CASE_C_MULTI_RUN,
                              &data_offset);
        }
      }
    }
    else
    {
      /* Case C: multi-run or not the only run */
      hp_write_run_data(share, data_ptr, data_len, run_start,
                        run_rec_count, HP_BLOB_CASE_C_MULTI_RUN,
                        &data_offset);
    }

    if (prev_run_start)
      memcpy(prev_run_start, &run_start, sizeof(run_start));
    else
      first_run= run_start;
    prev_run_start= run_start;
  }

  *first_run_out= first_run;
  return 0;

err:
  if (first_run)
    hp_free_run_chain(share, first_run);
  *first_run_out= NULL;
  return my_errno;
}


/*
  Write blob data from the record buffer into continuation runs.

  For each blob column, reads the (length, pointer) descriptor from
  the caller's record buffer, allocates variable-length continuation
  runs, copies blob data into them, and overwrites the pointer in
  the stored row (pos) to point to the first continuation run.

  @param info   Table handle
  @param record Source record buffer (caller's data)
  @param pos    Destination row in HP_BLOCK (already has memcpy'd record)

  @return 0 on success, my_errno on failure
*/

int hp_write_blobs(HP_INFO *info, const uchar *record, uchar *pos)
{
  HP_SHARE *share= info->s;
  uint i;
  my_bool has_blob_data= FALSE;
  DBUG_ENTER("hp_write_blobs");

  for (i= 0; i < share->blob_count; i++)
  {
    HP_BLOB_DESC *desc= &share->blob_descs[i];
    uint32 data_len;
    const uchar *data_ptr;
    uchar *first_run;

    data_len= hp_blob_length(desc, record);

    if (data_len == 0)
    {
      uchar *null_ptr= NULL;
      memcpy(pos + desc->offset + desc->packlength, &null_ptr, sizeof(null_ptr));
      continue;
    }

    has_blob_data= TRUE;
    memcpy(&data_ptr, record + desc->offset + desc->packlength, sizeof(data_ptr));

    if (hp_write_one_blob(share, data_ptr, data_len, &first_run))
    {
      /* Rollback: free all previously completed blob columns */
      uint j;
      for (j= 0; j < i; j++)
      {
        HP_BLOB_DESC *rd= &share->blob_descs[j];
        uchar *chain;
        memcpy(&chain, pos + rd->offset + rd->packlength, sizeof(chain));
        if (chain)
          hp_free_run_chain(share, chain);
        {
          uchar *null_ptr= NULL;
          memcpy(pos + rd->offset + rd->packlength, &null_ptr, sizeof(null_ptr));
        }
      }
      {
        uchar *null_ptr= NULL;
        memcpy(pos + desc->offset + desc->packlength, &null_ptr,
               sizeof(null_ptr));
      }
      DBUG_RETURN(my_errno);
    }

    memcpy(pos + desc->offset + desc->packlength, &first_run, sizeof(first_run));
  }

  pos[share->visible]= has_blob_data ?
    (HP_ROW_ACTIVE | HP_ROW_HAS_CONT) : HP_ROW_ACTIVE;
  DBUG_RETURN(0);
}


/*
  Read blob data from continuation runs into the reassembly buffer.

  After memcpy(record, pos, reclength), blob descriptor pointers in
  record[] point into HP_BLOCK continuation run chains.  This function
  walks each chain, reassembles blob data into info->blob_buff, and
  rewrites the pointers in record[] to point into blob_buff.

  @param info   Table handle
  @param record Record buffer (already has memcpy'd row data)
  @param pos    Row pointer in HP_BLOCK

  @return 0 on success, my_errno on failure
*/

int hp_read_blobs(HP_INFO *info, uchar *record, const uchar *pos)
{
  HP_SHARE *share= info->s;
  uint i;
  uint visible= share->visible;
  uint recbuffer= share->block.recbuffer;
  uint32 total_copy_size= 0;
  uchar *buff_ptr;
  DBUG_ENTER("hp_read_blobs");

  info->has_zerocopy_blobs= FALSE;

  if (!hp_has_cont(pos, share->visible))
    DBUG_RETURN(0);

  /*
    Pass 1: sum data_len for blobs that need reassembly (not zero-copy).
    Cases A and B (HP_ROW_CONT_ZEROCOPY set, or single-record run) use
    zero-copy pointers into HP_BLOCK, no blob_buff needed.
  */
  for (i= 0; i < share->blob_count; i++)
  {
    HP_BLOB_DESC *desc= &share->blob_descs[i];
    uint32 data_len;
    const uchar *chain;

    data_len= hp_blob_length(desc, record);
    if (data_len == 0)
      continue;

    memcpy(&chain, record + desc->offset + desc->packlength, sizeof(chain));

    /* Case A and Case B are zero-copy — need no reassembly buffer space */
    if (hp_blob_run_format(chain, visible) != HP_BLOB_CASE_C_MULTI_RUN)
    {
      info->has_zerocopy_blobs= TRUE;
      continue;
    }
    total_copy_size+= data_len;
  }

  /* Grow reassembly buffer for Case C blobs */
  if (total_copy_size > 0)
  {
    if (total_copy_size > info->blob_buff_len)
    {
      uchar *new_buff= (uchar*) my_realloc(hp_key_memory_HP_BLOB,
                                            info->blob_buff,
                                            total_copy_size,
                                            MYF(MY_ALLOW_ZERO_PTR));
      if (!new_buff)
        DBUG_RETURN(my_errno= HA_ERR_OUT_OF_MEM);
      info->blob_buff= new_buff;
      info->blob_buff_len= total_copy_size;
    }
  }

  /* Pass 2: process each blob column */
  buff_ptr= info->blob_buff;
  for (i= 0; i < share->blob_count; i++)
  {
    HP_BLOB_DESC *desc= &share->blob_descs[i];
    uint32 data_len;
    const uchar *chain;

    data_len= hp_blob_length(desc, record);
    if (data_len == 0)
      continue;

    memcpy(&chain, record + desc->offset + desc->packlength, sizeof(chain));

    switch (hp_blob_run_format(chain, visible))
    {
    case HP_BLOB_CASE_A_SINGLE_REC:
    {
      /* Case A: single-record single-run, no header — zero-copy */
      const uchar *blob_data= chain;
      memcpy(record + desc->offset + desc->packlength, &blob_data,
             sizeof(blob_data));
      continue;
    }
    case HP_BLOB_CASE_B_ZEROCOPY:
    {
      /* Case B: data in rec 1..N-1, contiguous — zero-copy */
      const uchar *blob_data= chain + recbuffer;
      memcpy(record + desc->offset + desc->packlength, &blob_data,
             sizeof(blob_data));
      continue;
    }
    case HP_BLOB_CASE_C_MULTI_RUN:
    {
      /* Case C: reassemble into blob_buff */
      uint32 remaining= data_len;
      const uchar *next_cont;
      while (chain && remaining > 0)
      {
        uint16 rec;
        uint16 run_rec_count;
        uint32 chunk;

        next_cont= hp_cont_next(chain);
        run_rec_count= hp_cont_rec_count(chain);

        /* First record payload (after header) */
        chunk= visible - HP_CONT_HEADER_SIZE;
        if (chunk > remaining)
          chunk= remaining;
        memcpy(buff_ptr, chain + HP_CONT_HEADER_SIZE, chunk);
        buff_ptr+= chunk;
        remaining-= chunk;

        /* Inner records: recbuffer stride, no flags byte */
        for (rec= 1; rec < run_rec_count && remaining > 0; rec++)
        {
          const uchar *rec_ptr= chain + rec * recbuffer;
          chunk= recbuffer;
          if (chunk > remaining)
            chunk= remaining;
          memcpy(buff_ptr, rec_ptr, chunk);
          buff_ptr+= chunk;
          remaining-= chunk;
        }

        chain= next_cont;
      }

      /* Update blob pointer to reassembly buffer */
      {
        uchar *blob_data= buff_ptr - data_len;
        memcpy(record + desc->offset + desc->packlength, &blob_data,
               sizeof(blob_data));
      }
      break;
    }
    } /* switch */
  }

  DBUG_RETURN(0);
}


/*
  Materialize a single blob column's data from a continuation chain
  into info->blob_buff.

  Used by hash comparison functions when comparing a stored record
  (where the blob data pointer has been overwritten with a continuation
  chain pointer) against an input record.

  @param info      Table handle (provides blob_buff)
  @param chain     Pointer to first run of the continuation chain
  @param data_len  Total blob data length (from record's packlength bytes)

  @return Pointer into info->blob_buff with contiguous blob data,
          or NULL on allocation failure.
*/

const uchar *hp_materialize_one_blob(HP_INFO *info,
                                     const uchar *chain,
                                     uint32 data_len)
{
  HP_SHARE *share= info->s;
  uint visible= share->visible;
  uint recbuffer= share->block.recbuffer;
  uint32 remaining;
  uchar *buff_ptr;
  const uchar *next_cont;
  uint16 run_rec_count;

  if (data_len == 0 || !chain)
    return chain;

  switch (hp_blob_run_format(chain, visible))
  {
  case HP_BLOB_CASE_A_SINGLE_REC:
    return chain;                         /* Case A: no header, data at offset 0 */
  case HP_BLOB_CASE_B_ZEROCOPY:
    return chain + recbuffer;             /* Case B: data in rec 1..N-1 */
  case HP_BLOB_CASE_C_MULTI_RUN:
    break;                                /* Case C: fall through to reassembly */
  }

  /* Case C: multiple runs, reassemble into blob_buff */
  if (data_len > info->blob_buff_len)
  {
    uchar *new_buff= (uchar*) my_realloc(hp_key_memory_HP_BLOB,
                                          info->blob_buff,
                                          data_len,
                                          MYF(MY_ALLOW_ZERO_PTR));
    if (!new_buff)
      return NULL;
    info->blob_buff= new_buff;
    info->blob_buff_len= data_len;
  }

  buff_ptr= info->blob_buff;
  remaining= data_len;
  while (chain && remaining > 0)
  {
    uint16 rec;
    uint32 chunk;

    next_cont= hp_cont_next(chain);
    run_rec_count= hp_cont_rec_count(chain);

    /* First record payload (after header) */
    chunk= visible - HP_CONT_HEADER_SIZE;
    if (chunk > remaining)
      chunk= remaining;
    memcpy(buff_ptr, chain + HP_CONT_HEADER_SIZE, chunk);
    buff_ptr+= chunk;
    remaining-= chunk;

    /* Inner records: recbuffer stride, no flags byte */
    for (rec= 1; rec < run_rec_count && remaining > 0; rec++)
    {
      const uchar *rec_ptr= chain + rec * recbuffer;
      chunk= recbuffer;
      if (chunk > remaining)
        chunk= remaining;
      memcpy(buff_ptr, rec_ptr, chunk);
      buff_ptr+= chunk;
      remaining-= chunk;
    }

    chain= next_cont;
  }

  return info->blob_buff;
}


/*
  Free continuation run chains for all blob columns of a row.

  Walks each blob column's run chain and adds all records back to the
  free list.

  @param share  Table share
  @param pos    Primary record pointer in HP_BLOCK
*/

void hp_free_blobs(HP_SHARE *share, uchar *pos)
{
  uint i;
  DBUG_ENTER("hp_free_blobs");

  if (!hp_has_cont(pos, share->visible))
    DBUG_VOID_RETURN;

  for (i= 0; i < share->blob_count; i++)
  {
    HP_BLOB_DESC *desc= &share->blob_descs[i];
    uchar *chain;

    memcpy(&chain, pos + desc->offset + desc->packlength, sizeof(chain));
    hp_free_run_chain(share, chain);
  }

  DBUG_VOID_RETURN;
}
