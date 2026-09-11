/* Copyright (c) 2000, 2018, Oracle and/or its affiliates.
   Copyright (c) 2010, 2020, MariaDB Corporation.

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

#include "heapdef.h"
#include <my_bit.h>

static int keys_compare(void *heap_rb, const void *key1, const void *key2);
static void init_block(HP_BLOCK *block, size_t reclength, ulong min_records,
		       const ulong max_records);


/*
  Map an offset in the SQL record buffer to its position in a stored
  record.

  Defined for every offset outside a promoted column's reserved payload,
  which is every offset the engine ever addresses: the null bytes, the
  fixed-width columns, and each descriptor's length prefix.  An offset
  inside a payload has no stored counterpart, because that is the space
  promotion removes.
*/

static uint hp_stored_offset(const HP_SHARE *share, uint offset)
{
  const HP_COPY_SPAN *span, *span_end;

  for (span= share->copy_spans, span_end= span + share->copy_span_count;
       span < span_end; span++)
  {
    if (offset >= span->offset && offset < span->offset + span->length)
      return span->store_offset + (offset - span->offset);
  }
  DBUG_ASSERT(0);                     /* Offset inside a promoted payload */
  return offset;
}


/*
  Build the map between the SQL record buffer and the stored record.

  The spans are the ranges between promoted columns' reserved payloads.
  A promoted column's length prefix rides along at the end of the span
  before it, so only the payload is a gap, and the stored record spends
  a chain pointer there instead.

  Once the spans exist, every descriptor's stored position follows from
  them -- including the native blobs', whose shape does not change but
  whose position does, because compaction moves everything after the
  first promoted column.

  ha_heap.cc computes stored_reclength independently while deciding what
  to promote.  Re-deriving it here and asserting the two agree is what
  catches a promotion decision that does not match the layout it implies.
*/

static void hp_setup_record_layout(HP_SHARE *share, uint reclength,
                                   uint stored_reclength)
{
  HP_COPY_SPAN *span= share->copy_spans;
  uint i, sql_pos= 0, store_pos= 0;

  for (i= 0; i < share->blob_count; i++)
  {
    HP_BLOB_DESC *desc= share->blob_descs + i;
    uint gap;

    if (!desc->promoted)
      continue;
    gap= desc->offset + desc->packlength;
    DBUG_ASSERT(gap >= sql_pos);        /* Descriptors ascend by offset */
    span->offset= sql_pos;
    span->store_offset= store_pos;
    span->length= gap - sql_pos;
    store_pos+= span->length + (uint) sizeof(uchar*);
    sql_pos= gap + desc->length;
    span++;
  }
  span->offset= sql_pos;
  span->store_offset= store_pos;
  span->length= reclength - sql_pos;

  DBUG_ASSERT((uint) (span - share->copy_spans) + 1 == share->copy_span_count);
  DBUG_ASSERT(store_pos + span->length == stored_reclength);

  for (i= 0; i < share->blob_count; i++)
  {
    HP_BLOB_DESC *desc= share->blob_descs + i;
    desc->store_offset= hp_stored_offset(share, desc->offset);
  }
  share->stored_reclength= stored_reclength;
}


/*
  Derive a key's stored segments from its SQL segments.

  Mostly this is re-addressing: compaction has moved every segment that
  follows the first promoted column.  A segment over a promoted column is
  additionally marked HA_BLOB_PART, which in a stored segment means the
  data is not inline but in a continuation chain, exactly as it is for a
  native blob.

  The segment keeps its VARCHAR type, so hashing and comparison keep
  applying VARCHAR rules -- prefix truncation, PAD/NOPAD, the multi-byte
  character position walk.  Only where the bytes are read from changes.
  Promotion must not change what a key means.
*/

static void hp_make_stored_keysegs(HP_SHARE *share, HA_KEYSEG *sql_seg,
                                   HA_KEYSEG *store_seg, uint keysegs)
{
  uint i, j;

  memcpy(store_seg, sql_seg, sizeof(*store_seg) * keysegs);
  for (i= 0; i < keysegs; i++)
  {
    store_seg[i].start= hp_stored_offset(share, sql_seg[i].start);
    store_seg[i].bit_pos= hp_stored_offset(share, sql_seg[i].bit_pos);
    /*
      null_pos needs no translation, and gets none: the null bitmap sits
      at the front of the record, ahead of every field, so no promoted
      column can precede it and compaction cannot move it.  hp_hash.c
      reads a stored record at this offset, so assert what that relies on
      rather than leaving the one untranslated offset unexplained.
    */
    DBUG_ASSERT(!sql_seg[i].null_bit ||
                hp_stored_offset(share, sql_seg[i].null_pos) ==
                sql_seg[i].null_pos);
    for (j= 0; j < share->blob_count; j++)
    {
      const HP_BLOB_DESC *desc= share->blob_descs + j;
      if (desc->promoted && desc->offset == sql_seg[i].start)
      {
        store_seg[i].flag|= HA_BLOB_PART;
        break;
      }
    }
  }
}


/*
  Whether a key segment starting at this record offset reads its value
  through the SQL record rather than from it.

  A segment says so with HA_BLOB_PART, and hp_varchar_seg_data() then
  reads a pointer where the value would be, so a segment carrying the flag
  with no descriptor behind it dereferences whatever the record holds.
  A VARCHAR the SQL layer moved out of the record sets the flag
  legitimately, so what drives the strip is whether a descriptor stands
  behind the segment rather than how wide its prefix is.

  A descriptor the engine promoted does not count: there the SQL record
  still holds the value inline and only the stored record is indirect.
*/

static my_bool hp_seg_reads_out_of_line(const HP_CREATE_INFO *create_info,
                                        uint start)
{
  uint i;
  for (i= 0; i < create_info->blob_count; i++)
    if (create_info->blob_descs[i].offset == start &&
        !create_info->blob_descs[i].promoted)
      return TRUE;
  return FALSE;
}


/*
  In how many parts are we going to do allocations of memory and indexes
  If we assign 1M to the heap table memory, we will allocate roughly
  (1M/16) bytes per allocation
*/
static const int heap_allocation_parts= 16;

/*
  Upper bound for a block allocation derived from max_records.
  max_records is usually computed from the table memory ceiling
  (max_heap_table_size or tmp_memory_table_size), not from an estimate of
  the expected number of rows, so with a large ceiling max_records /
  heap_allocation_parts can yield allocations of hundreds of MB even for
  tables that will only ever hold a few rows.  Blocks above a few MB are
  served by mmap() and unmapped on free by common malloc implementations
  (never recycled), so short-lived tables (per-statement internal
  temporary tables) would pay mmap/munmap, page fault-in/zeroing and
  process-wide mmap_lock serialization on every statement.  4MB is
  recycled from the allocator's free lists by all mainstream mallocs
  (glibc, jemalloc, tcmalloc, mimalloc) and is large enough to keep
  typical blob values in a single continuation run (zero-copy reads).
*/
static const ulong heap_max_allocation_block= 4*1024*1024;

/* min block allocation */
static const ulong heap_min_allocation_block= 16384;

/* Create a heap table */

int heap_create(const char *name, HP_CREATE_INFO *create_info,
                HP_SHARE **res, my_bool *created_new_share)
{
  uint i, key_segs, max_length, length;
  HP_SHARE *share= 0;
  HA_KEYSEG *keyseg, *stored_keyseg;
  HP_KEYDEF *keydef= create_info->keydef;
  uint reclength= create_info->reclength;
  /*
    Promoted columns make the stored record shorter than record[0].
    Everything about the block geometry -- the record stride, the
    visibility byte offset, the rows that fit in the table ceiling --
    follows the stored length, which is the whole point of promoting.
  */
  uint stored_reclength= (create_info->stored_reclength ?
                          create_info->stored_reclength : reclength);
  uint copy_spans= 1, promoted= 0;
  uint keys= create_info->keys;
  ulong min_records= create_info->min_records;
  ulong max_records= create_info->max_records;
  uint visible_offset;
  /*
    max_records is an expected record count used to size blocks, not a
    limit; 0 means the caller has no expectation.  Block sizing needs a
    concrete number, so derive one here.

    The row limit is create_info->max_rows, where 0 still means "no
    limit".  The share stores an explicit ceiling instead, writing "no
    limit" as NO_LIMIT_ROWS, so heap_write() tests one value with no
    special case.  That leaves 0 free to mean what it says on the share,
    a table that accepts no rows.
  */
  ulong block_max_records= (max_records ? max_records :
                            MY_MAX(min_records, 1000));
  DBUG_ENTER("heap_create");

  if (!create_info->internal_table)
  {
    mysql_mutex_lock(&THR_LOCK_heap);
    share= hp_find_named_heap(name);
    if (share && share->open_count == 0)
    {
      hp_free(share);
      share= 0;
    }
  }  
  else
  {
    DBUG_PRINT("info", ("Creating internal (no named) temporary table"));
  }
  *created_new_share= (share == NULL);

  if (!share)
  {
    HP_KEYDEF *keyinfo;
    /*
      A second key segment array, describing the stored record, is
      allocated only when something is promoted; without promotion the
      stored segments are the SQL ones and stored_keyseg aliases keyseg.
    */
    uint stored_key_segs;
    uchar *tail;
    DBUG_PRINT("info",("Initializing new table"));
    
    /*
      Deleted records store del_link (sizeof(uchar*) bytes), a block
      flags byte, and a uint16 block count.  visible_offset must be
      at least HP_DEL_METADATA_SIZE so that these fields never overlap
      the flags byte at offset 'visible'.  This also satisfies the
      blob continuation header requirement (HP_CONT_HEADER_SIZE + 1).
    */
    visible_offset= MY_MAX(stored_reclength, HP_DEL_METADATA_SIZE);

    /*
      One verbatim range per promoted column's payload gap, plus the
      trailing one.  A table with no promoted column keeps a single span
      covering the whole record.
    */
    for (i= 0; i < create_info->blob_count; i++)
    {
      /*
        The layout below is built by walking the descriptors in record
        order, and hp_stored_offset() maps an offset by finding the span
        it falls in, so this array has to ascend by offset.  ha_heap.cc
        sorts it before calling; assert the precondition here, where the
        contract is, so a caller that builds descriptors in field order
        fails at its own mistake rather than on a wrapped span length.
      */
      DBUG_ASSERT(!i || create_info->blob_descs[i - 1].offset <
                        create_info->blob_descs[i].offset);
      if (create_info->blob_descs[i].promoted)
      {
        promoted++;
        copy_spans++;
      }
    }

    for (i= key_segs= max_length= 0, keyinfo= keydef; i < keys; i++, keyinfo++)
    {
      HA_KEYSEG *keyseg, *keyseg_end;

      bzero((char*) &keyinfo->block,sizeof(keyinfo->block));
      bzero((char*) &keyinfo->rb_tree ,sizeof(keyinfo->rb_tree));
      for (keyseg= keyinfo->seg, keyseg_end= keyseg+ keyinfo->keysegs, length=0;
           keyseg < keyseg_end ;
           keyseg++)
      {
	length+= keyseg->length;
	if (keyseg->null_bit)
	{
	  length++;
	  if (!(keyinfo->flag & HA_NULL_ARE_EQUAL))
	    keyinfo->flag|= HA_NULL_PART_KEY;
	  if (keyinfo->algorithm == HA_KEY_ALG_BTREE)
	    keyinfo->rb_tree.size_of_element++;
	}
	switch (keyseg->type) {
	case HA_KEYTYPE_SHORT_INT:
	case HA_KEYTYPE_LONG_INT:
	case HA_KEYTYPE_FLOAT:
	case HA_KEYTYPE_DOUBLE:
	case HA_KEYTYPE_USHORT_INT:
	case HA_KEYTYPE_ULONG_INT:
	case HA_KEYTYPE_LONGLONG:
	case HA_KEYTYPE_ULONGLONG:
	case HA_KEYTYPE_INT24:
	case HA_KEYTYPE_UINT24:
	case HA_KEYTYPE_INT8:
	  keyseg->flag|= HA_SWAP_KEY;
          break;
        case HA_KEYTYPE_VARBINARY1:
          /* Case-insensitiveness is handled in hash_sort */
          keyseg->type= HA_KEYTYPE_VARTEXT1;
          /* fall through */
        case HA_KEYTYPE_VARTEXT1:
          keyinfo->flag|= HA_VAR_LENGTH_KEY;
          /*
            A real blob always enters as VARTEXT4 or VARBINARY4, so on a
            one-byte-prefix VARCHAR segment HA_BLOB_PART is a stray bit --
            unless the SQL layer moved the column out of the record, which
            sets the flag deliberately and keeps the one-byte prefix, a
            VARCHAR(200) in a single-byte charset being such a segment.
            The flag is therefore stripped from the segments that have no
            descriptor behind them rather than from all of them.

            The strip stays a release-build action, not an assertion.
            key_part_flag reaches here from a .frm byte that nothing masks
            (sql/table.cc), and hp_varchar_seg_data() reads the record as a
            pointer wherever the flag is set, so a stray bit that survives
            is a dereference of record contents.
          */
          if (!hp_seg_reads_out_of_line(create_info, keyseg->start))
          {
            DBUG_ASSERT(!(keyseg->flag & HA_BLOB_PART));
            keyseg->flag&= ~HA_BLOB_PART;
          }
          /*
            For BTREE algorithm, key length, greater than or equal
            to 255, is packed on 3 bytes.
          */
          if (keyinfo->algorithm == HA_KEY_ALG_BTREE)
            length+= size_to_store_key_length(keyseg->length);
          else
            length+= 2;
          keyseg->bit_start= 1;         /* Packlength for records */
          keyseg->bit_length= 2;        /* Packlength for key */
          break;
        case HA_KEYTYPE_VARBINARY4:
          /* fall through */
        case HA_KEYTYPE_VARTEXT4:
          /* Key is stored as 4 byte length + pointer to data */
          DBUG_ASSERT(keyseg->flag & HA_BLOB_PART);
          DBUG_ASSERT(keyinfo->algorithm != HA_KEY_ALG_BTREE);
          DBUG_ASSERT(keyseg->length == 4+portable_sizeof_char_ptr);
          DBUG_ASSERT(keyseg->bit_start >= 1 && keyseg->bit_start <= 4);
          DBUG_ASSERT(keyseg->bit_length == 0);

          /*
            bit_start holds the actual blob packlength (1-4), set by
            heap_prepare_hp_create_info().
          */
          keyinfo->flag|= HA_VAR_LENGTH_KEY;
          keyseg->type= HA_KEYTYPE_VARTEXT4;
          break;

        case HA_KEYTYPE_VARBINARY2:
          /* Case-insensitiveness is handled in hash_sort */
          /* fall through */
        case HA_KEYTYPE_VARTEXT2:
          keyinfo->flag|= HA_VAR_LENGTH_KEY;
          /* key is stored as [length] + data */
          keyseg->bit_start= 2;
          keyseg->bit_length= 2;
          /*
            Make future comparison simpler by only having to check for
            one type
          */
          keyseg->type= HA_KEYTYPE_VARTEXT1;

          /*
            For BTREE algorithm, key length, greater than or equal
            to 255, is packed on 3 bytes.
          */
          if (keyinfo->algorithm == HA_KEY_ALG_BTREE)
            length+= size_to_store_key_length(keyseg->length);
          else
            length+= keyseg->bit_start;
          break;
        case HA_KEYTYPE_BIT:
          /*
            The odd bits which stored separately (if they are present
            (bit_pos, bit_length)) are already present in seg[j].length as
            additional byte.
            See field.h, function key_length()
          */
          break;
	default:
	  break;
	}
      }
      keyinfo->length= length;
      length+= keyinfo->rb_tree.size_of_element + 
	       ((keyinfo->algorithm == HA_KEY_ALG_BTREE) ? sizeof(uchar*) : 0);
      if (length > max_length)
	max_length= length;
      key_segs+= keyinfo->keysegs;
      if (keyinfo->algorithm == HA_KEY_ALG_BTREE)
      {
        key_segs++; /* additional HA_KEYTYPE_END segment */
        if (keyinfo->flag & HA_VAR_LENGTH_KEY)
          keyinfo->get_key_length= hp_rb_var_key_length;
        else if (keyinfo->flag & HA_NULL_PART_KEY)
          keyinfo->get_key_length= hp_rb_null_key_length;
        else
          keyinfo->get_key_length= hp_rb_key_length;
      }
    }
    stored_key_segs= promoted ? key_segs : 0;
    if (!(share= (HP_SHARE*) my_malloc(hp_key_memory_HP_SHARE,
                                       sizeof(HP_SHARE)+
				       keys*sizeof(HP_KEYDEF)+
				       key_segs*sizeof(HA_KEYSEG)+
                                       stored_key_segs*sizeof(HA_KEYSEG)+
				       create_info->blob_count*
                                       sizeof(HP_BLOB_DESC)+
                                       copy_spans*sizeof(HP_COPY_SPAN),
				       MYF(MY_ZEROFILL |
                                           (create_info->internal_table ?
                                            MY_THREAD_SPECIFIC : 0)))))
      goto err;
    share->keydef= (HP_KEYDEF*) (share + 1);
    share->key_stat_version= 1;
    keyseg= (HA_KEYSEG*) (share->keydef + keys);
    stored_keyseg= keyseg + key_segs;
    tail= (uchar*) (keyseg + key_segs + stored_key_segs);
    if (create_info->blob_count)
    {
      share->blob_descs= (HP_BLOB_DESC*) tail;
      memcpy(share->blob_descs, create_info->blob_descs,
             create_info->blob_count * sizeof(HP_BLOB_DESC));
      share->blob_count= create_info->blob_count;
      tail= (uchar*) (share->blob_descs + create_info->blob_count);
    }
    share->copy_spans= (HP_COPY_SPAN*) tail;
    share->copy_span_count= copy_spans;
    share->promoted_count= promoted;
    share->declared_reclength= create_info->declared_reclength;
    hp_setup_record_layout(share, reclength, stored_reclength);
    init_block(&share->block, hp_memory_needed_per_row(stored_reclength),
               min_records, block_max_records);
	/* Fix keys */
    memcpy(share->keydef, keydef, (size_t) (sizeof(keydef[0]) * keys));
    for (i= 0, keyinfo= share->keydef; i < keys; i++, keyinfo++)
    {
      keyinfo->seg= keyseg;
      memcpy(keyseg, keydef[i].seg,
	     (size_t) (sizeof(keyseg[0]) * keydef[i].keysegs));
      if (promoted)
      {
        keyinfo->seg_stored= stored_keyseg;
        hp_make_stored_keysegs(share, keyseg, stored_keyseg,
                               keydef[i].keysegs);
      }
      else
        keyinfo->seg_stored= keyseg;
      keyseg+= keydef[i].keysegs;
      stored_keyseg+= keydef[i].keysegs;

      if (keydef[i].algorithm == HA_KEY_ALG_BTREE)
      {
	/* additional HA_KEYTYPE_END keyseg */
	keyseg->type=     HA_KEYTYPE_END;
	keyseg->length=   sizeof(uchar*);
	keyseg->flag=     0;
	keyseg->null_bit= 0;
	keyseg++;
        stored_keyseg++;             /* Keep the two arrays in lockstep */

	init_tree(&keyinfo->rb_tree, 0, 0, sizeof(uchar*),
		  keys_compare, NULL, NULL,
                  MYF((create_info->internal_table ? MY_THREAD_SPECIFIC : 0) |
                      MY_TREE_WITH_DELETE));
	keyinfo->delete_key= hp_rb_delete_key;
	keyinfo->write_key= hp_rb_write_key;
      }
      else
      {
	init_block(&keyinfo->block, sizeof(HASH_INFO), min_records,
		   block_max_records);
	keyinfo->delete_key= hp_delete_key;
	keyinfo->write_key= hp_write_key;
        keyinfo->hash_buckets= 0;
      }
      if ((keyinfo->flag & HA_AUTO_KEY) && create_info->with_auto_increment)
        share->auto_key= i + 1;
    }
    share->min_records= min_records;
    share->max_rows= (create_info->max_rows ? create_info->max_rows :
                      NO_LIMIT_ROWS);
    share->max_table_size= create_info->max_table_size;
    share->data_length= share->index_length= 0;
    share->deleted_entries= 0;
    share->reclength= reclength;
    share->visible= visible_offset;
    share->blength= 1;
    share->keys= keys;
    share->max_key_length= max_length;
    share->changed= 0;
    share->auto_key= create_info->auto_key;
    share->auto_key_type= create_info->auto_key_type;
    share->auto_increment= create_info->auto_increment;
    share->create_time= (long) time((time_t*) 0);
    share->internal= create_info->internal_table;
    /* Must be allocated separately for rename to work */
    if (!(share->name= my_strdup(hp_key_memory_HP_SHARE, name, MYF(0))))
    {
      my_free(share);
      goto err;
    }

    if (!create_info->internal_table)
    {
      thr_lock_init(&share->lock);
      share->open_list.data= (void*) share;
      heap_share_list= list_add(heap_share_list,&share->open_list);
    }
    else
      share->delete_on_close= 1;
  }
  if (!create_info->internal_table)
  {
    if (create_info->pin_share)
      ++share->open_count;
    mysql_mutex_unlock(&THR_LOCK_heap);
  }

  *res= share;
  DBUG_RETURN(0);

err:
  if (!create_info->internal_table)
    mysql_mutex_unlock(&THR_LOCK_heap);
  DBUG_RETURN(1);
} /* heap_create */


static int keys_compare(void *heap_rb_, const void *key1_,
                        const void *key2_)
{
  heap_rb_param *heap_rb= heap_rb_;
  const uchar *key1= key1_;
  const uchar *key2= key2_;
  uint not_used[2];
  return ha_key_cmp(heap_rb->keyseg, key1, key2, heap_rb->key_length,
                    heap_rb->search_flag, not_used);
}


/*
  Calculate length needed for storing one row
*/

size_t hp_memory_needed_per_row(size_t reclength)
{
  /*
    Must accommodate del_link + del_flag + block count.
    The + 1 below is for the required visibility byte at the end of each record.
  */
  reclength= MY_MAX(reclength + 1, HP_DEL_METADATA_SIZE);
  /*
    Record has to be aligned for faster memcpy and also for allowing
    direct access to delete link/chain at start of record.
  */
  reclength= MY_ALIGN(reclength, sizeof(char*));
  return reclength;
}

/*
  Calculate the number of rows that fits into a given memory size
*/

ha_rows hp_rows_in_memory(size_t reclength, size_t index_size,
                          size_t memory_limit)
{
  reclength= hp_memory_needed_per_row(reclength);
  if ((memory_limit < index_size + reclength + sizeof(HP_PTRS)))
    return 0;                                   /* Wrong arguments */
  return (ha_rows) ((memory_limit - sizeof(HP_PTRS)) /
                    (index_size + reclength));
}


static void init_block(HP_BLOCK *block, size_t reclength, ulong min_records,
		       const ulong max_records)
{
  ulong i,records_in_block,cap_records;
  ulong recbuffer= (ulong) MY_ALIGN(reclength, sizeof(uchar*));
  ulong extra;
  ulong requested_min_records= min_records;
  ulonglong memory_needed;
  size_t alloc_size;

  /*
    If no min_records is given, optimize for 1000 rows.  max_records is
    the caller's sizing ceiling and is never changed here.
  */
  if (!min_records)
    min_records= MY_MIN(1000, max_records / heap_allocation_parts);
  min_records= MY_MIN(min_records, max_records);
  /*
    An explicit min_records may override the cap below, but only as far
    as the ceiling reaches: max_records is the most rows the table's
    memory ceiling (max_heap_table_size / tmp_memory_table_size) can
    ever hold, so a larger MIN_ROWS is unreachable and pre-sizing for it
    would reserve memory the table can never use.  MY_MIN keeps 0 at 0,
    so "no min_records requested" stays distinguishable from an explicit
    one.
  */
  requested_min_records= MY_MIN(requested_min_records, max_records);

 /*
    We don't want too few records_in_block as otherwise the overhead of
    of the HP_PTRS block will be too notable
  */
  records_in_block= MY_MAX(min_records, max_records / heap_allocation_parts);

  /*
    Align allocation sizes to power of 2 to get less memory fragmentation from
    system alloc().
    As long as we have less than 128 allocations, all but one of the
    allocations will have an extra HP_PTRS size structure at the start
    of the block.

    We ensure that the block is not smaller than heap_min_allocation_block
    as otherwise we get strange results when max_records <
    heap_allocation_parts)
  */
  extra= sizeof(HP_PTRS) + MALLOC_OVERHEAD;

  /*
    Cap block allocations derived from max_records at
    heap_max_allocation_block.  Only an explicit min_records from the
    caller (CREATE TABLE ... MIN_ROWS) is a real row count expectation
    and may pre-size beyond the cap; the 1000-row default min_records
    is a heuristic and must not override the cap.  That override is
    bounded by max_records (clamped above), so an unreachable MIN_ROWS
    degrades to plain ceiling-derived sizing instead of reserving a
    block the ceiling can never fill.
  */
  cap_records= (heap_max_allocation_block - extra) / recbuffer;
  if (records_in_block > cap_records)
    records_in_block= MY_MAX(requested_min_records, cap_records);

  /* We don't want too few blocks per row either */
  if (records_in_block < 10)
    records_in_block= MY_MIN(10, max_records);
  memory_needed= MY_MAX(((ulonglong) records_in_block * recbuffer + extra),
                        (ulonglong) heap_min_allocation_block);

  /* We have to limit memory to INT_MAX32 as my_round_up_to_next_power() is 32 bit */
  memory_needed= MY_MIN(memory_needed, (ulonglong) INT_MAX32);
  alloc_size= my_round_up_to_next_power((uint32)memory_needed);
  records_in_block= (ulong) ((alloc_size - extra)/ recbuffer);

  DBUG_PRINT("info", ("records_in_block: %lu" ,records_in_block));

  block->records_in_block= records_in_block;
  block->recbuffer= recbuffer;
  block->last_allocated= 0L;
  block->high_water_allocated= 0L;
  /* All allocations are done with this size, if possible */
  block->alloc_size= alloc_size - MALLOC_OVERHEAD;

  for (i= 0; i <= HP_MAX_LEVELS; i++)
    block->level_info[i].records_under_level=
      (!i ? 1 : i == 1 ? records_in_block :
       HP_PTRS_IN_NOD * block->level_info[i - 1].records_under_level);
}


static inline void heap_try_free(HP_SHARE *share)
{
  DBUG_ENTER("heap_try_free");
  if (share->open_count == 0)
    hp_free(share);
  else
  {
    DBUG_PRINT("info", ("Table is still in use. Will be freed on close"));
    share->delete_on_close= 1;
  }
  DBUG_VOID_RETURN;
}


int heap_delete_table(const char *name)
{
  int result;
  reg1 HP_SHARE *share;
  DBUG_ENTER("heap_delete_table");

  mysql_mutex_lock(&THR_LOCK_heap);
  if ((share= hp_find_named_heap(name)))
  {
    heap_try_free(share);
    result= 0;
  }
  else
  {
    result= my_errno=ENOENT;
    DBUG_PRINT("error", ("Could not find table '%s'", name));
  }
  mysql_mutex_unlock(&THR_LOCK_heap);
  DBUG_RETURN(result);
}


void heap_drop_table(HP_INFO *info)
{
  DBUG_ENTER("heap_drop_table");
  mysql_mutex_lock(&THR_LOCK_heap);
  heap_try_free(info->s);
  mysql_mutex_unlock(&THR_LOCK_heap);
  DBUG_VOID_RETURN;
}


void hp_free(HP_SHARE *share)
{
  if (!share->internal)
  {
    heap_share_list= list_delete(heap_share_list, &share->open_list);
    thr_lock_delete(&share->lock);
  }
  hp_clear(share);			/* Remove blocks from memory */
  my_free(share->name);
  my_free(share);
  return;
}
