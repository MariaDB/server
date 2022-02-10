/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates.
   Copyright (c) 2009, 2017, MariaDB Corporation

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

#ifdef USE_PRAGMA_INTERFACE
#pragma implementation /* gcc class implementation */
#endif

/**
  @file

  @brief
  Functions for easy reading of records, possible through a cache
*/

#include "mariadb.h"
#include "records.h"
#include "sql_priv.h"
#include "records.h"
#include "opt_range.h"                          // SQL_SELECT
#include "sql_class.h"                          // THD
#include "sql_base.h"
#include "sql_sort.h"                           // SORT_ADDON_FIELD

static int rr_quick(READ_RECORD *info);
int rr_sequential(READ_RECORD *info);
static int rr_from_tempfile(READ_RECORD *info);
template<bool> static int rr_unpack_from_tempfile(READ_RECORD *info);
template<bool,bool> static int rr_unpack_from_buffer(READ_RECORD *info);
int rr_from_pointers(READ_RECORD *info);
static int rr_from_cache(READ_RECORD *info);
static int init_rr_cache(THD *thd, READ_RECORD *info);
static int rr_cmp(uchar *a,uchar *b);
static int rr_index_first(READ_RECORD *info);
static int rr_index_last(READ_RECORD *info);
static int rr_index(READ_RECORD *info);
static int rr_index_desc(READ_RECORD *info);


/**
  Initialize READ_RECORD structure to perform full index scan in desired 
  direction using read_record.read_record() interface

    This function has been added at late stage and is used only by
    UPDATE/DELETE. Other statements perform index scans using
    join_read_first/next functions.

  @param info         READ_RECORD structure to initialize.
  @param thd          Thread handle
  @param table        Table to be accessed
  @param print_error  If true, call table->file->print_error() if an error
                      occurs (except for end-of-records error)
  @param idx          index to scan
  @param reverse      Scan in the reverse direction
*/

bool init_read_record_idx(READ_RECORD *info, THD *thd, TABLE *table,
                          bool print_error, uint idx, bool reverse)
{
  int error= 0;
  DBUG_ENTER("init_read_record_idx");

  empty_record(table);
  bzero((char*) info,sizeof(*info));
  info->thd= thd;
  info->table= table;
  info->print_error= print_error;
  info->unlock_row= rr_unlock_row;

  table->status=0;			/* And it's always found */
  if (!table->file->inited &&
      unlikely(error= table->file->ha_index_init(idx, 1)))
  {
    if (print_error)
      table->file->print_error(error, MYF(0));
  }

  /* read_record_func will be changed to rr_index in rr_index_first */
  info->read_record_func= reverse ? rr_index_last : rr_index_first;
  DBUG_RETURN(error != 0);
}


/*
  init_read_record is used to scan by using a number of different methods.
  Which method to use is set-up in this call so that later calls to
  the info->read_record will call the appropriate method using a function
  pointer.

  There are five methods that relate completely to the sort function
  filesort. The result of a filesort is retrieved using read_record
  calls. The other two methods are used for normal table access.

  The filesort will produce references to the records sorted, these
  references can be stored in memory or in a temporary file.

  The temporary file is normally used when the references doesn't fit into
  a properly sized memory buffer. For most small queries the references
  are stored in the memory buffer.
  SYNOPSIS
    init_read_record()
      info              OUT read structure
      thd               Thread handle
      table             Table the data [originally] comes from.
      select            SQL_SELECT structure. We may select->quick or 
                        select->file as data source
      use_record_cache  Call file->extra_opt(HA_EXTRA_CACHE,...)
                        if we're going to do sequential read and some
                        additional conditions are satisfied.
      print_error       Copy this to info->print_error
      disable_rr_cache  Don't use rr_from_cache (used by sort-union
                        index-merge which produces rowid sequences that 
                        are already ordered)

  DESCRIPTION
    This function sets up reading data via one of the methods:

  The temporary file is also used when performing an update where a key is
  modified.

  Methods used when ref's are in memory (using rr_from_pointers):
    rr_unpack_from_buffer:
    ----------------------
      This method is used when table->sort.addon_field is allocated.
      This is allocated for most SELECT queries not involving any BLOB's.
      In this case the records are fetched from a memory buffer.
    rr_from_pointers:
    -----------------
      Used when the above is not true, UPDATE, DELETE and so forth and
      SELECT's involving BLOB's. It is also used when the addon_field
      buffer is not allocated due to that its size was bigger than the
      session variable max_length_for_sort_data.
      In this case the record data is fetched from the handler using the
      saved reference using the rnd_pos handler call.

  Methods used when ref's are in a temporary file (using rr_from_tempfile)
    rr_unpack_from_tempfile:
    ------------------------
      Same as rr_unpack_from_buffer except that references are fetched from
      temporary file. Should obviously not really happen other than in
      strange configurations.

    rr_from_tempfile:
    -----------------
      Same as rr_from_pointers except that references are fetched from
      temporary file instead of from 
    rr_from_cache:
    --------------
      This is a special variant of rr_from_tempfile that can be used for
      handlers that is not using the HA_FAST_KEY_READ table flag. Instead
      of reading the references one by one from the temporary file it reads
      a set of them, sorts them and reads all of them into a buffer which
      is then used for a number of subsequent calls to rr_from_cache.
      It is only used for SELECT queries and a number of other conditions
      on table size.

  All other accesses use either index access methods (rr_quick) or a full
  table scan (rr_sequential).
  rr_quick:
  ---------
    rr_quick uses one of the QUICK_SELECT classes in opt_range.cc to
    perform an index scan. There are loads of functionality hidden
    in these quick classes. It handles all index scans of various kinds.
  rr_sequential:
  --------------
    This is the most basic access method of a table using rnd_init,
    rnd_next and rnd_end. No indexes are used.
*/

bool init_read_record(READ_RECORD *info,THD *thd, TABLE *table,
		      SQL_SELECT *select,
                      SORT_INFO *filesort,
		      int use_record_cache, bool print_error, 
                      bool disable_rr_cache)
{
  IO_CACHE *tempfile;
  DBUG_ENTER("init_read_record");

  const bool using_addon_fields= filesort && filesort->using_addon_fields();
  bool using_packed_sortkeys= filesort && filesort->using_packed_sortkeys();

  bzero((char*) info,sizeof(*info));
  info->thd=thd;
  info->table=table;
  info->sort_info= filesort;
  
  if ((table->s->tmp_table == INTERNAL_TMP_TABLE) &&
      !using_addon_fields)
    (void) table->file->extra(HA_EXTRA_MMAP);
  
  if (using_addon_fields)
  {
    info->rec_buf=    filesort->addon_fields->get_addon_buf();
    info->ref_length= filesort->addon_fields->get_addon_buf_length();
  }
  else
  {
    empty_record(table);
    info->ref_length= (uint)table->file->ref_length;
  }
  info->select=select;
  info->print_error=print_error;
  info->unlock_row= rr_unlock_row;
  table->status= 0;			/* Rows are always found */

  tempfile= 0;
  if (select && my_b_inited(&select->file))
    tempfile= &select->file;
  else if (filesort && my_b_inited(&filesort->io_cache))
    tempfile= &filesort->io_cache;

  if (tempfile && !(select && select->quick))
  {
    if (using_addon_fields)
    {
      DBUG_PRINT("info",("using rr_from_tempfile"));
      if (filesort->addon_fields->using_packed_addons())
        info->read_record_func= rr_unpack_from_tempfile<true>;
      else
        info->read_record_func= rr_unpack_from_tempfile<false>;
    }
    else
    {
      DBUG_PRINT("info",("using rr_from_tempfile"));
      info->read_record_func= rr_from_tempfile;
    }

    info->io_cache= tempfile;
    reinit_io_cache(info->io_cache,READ_CACHE,0L,0,0);
    info->ref_pos=table->file->ref;
    if (!table->file->inited)
      if (unlikely(table->file->ha_rnd_init_with_error(0)))
        DBUG_RETURN(1);

    /*
      addon_field is checked because if we use addon fields,
      it doesn't make sense to use cache - we don't read from the table
      and filesort->io_cache is read sequentially
    */
    if (!disable_rr_cache &&
        !using_addon_fields &&
	thd->variables.read_rnd_buff_size &&
	!(table->file->ha_table_flags() & HA_FAST_KEY_READ) &&
	(table->db_stat & HA_READ_ONLY ||
	 table->reginfo.lock_type < TL_FIRST_WRITE) &&
	(ulonglong) table->s->reclength* (table->file->stats.records+
                                          table->file->stats.deleted) >
	(ulonglong) MIN_FILE_LENGTH_TO_USE_ROW_CACHE &&
	info->io_cache->end_of_file/info->ref_length * table->s->reclength >
	(my_off_t) MIN_ROWS_TO_USE_TABLE_CACHE &&
	!table->s->blob_fields &&
        info->ref_length <= MAX_REFLENGTH)
    {
      if (! init_rr_cache(thd, info))
      {
	DBUG_PRINT("info",("using rr_from_cache"));
        info->read_record_func= rr_from_cache;
      }
    }
  }
  else if (select && select->quick)
  {
    DBUG_PRINT("info",("using rr_quick"));
    info->read_record_func= rr_quick;
  }
  else if (filesort && filesort->has_filesort_result_in_memory())
  {
    DBUG_PRINT("info",("using record_pointers"));
    if (unlikely(table->file->ha_rnd_init_with_error(0)))
      DBUG_RETURN(1);

    info->cache_pos= filesort->record_pointers;
    if (using_addon_fields)
    {
      DBUG_PRINT("info",("using rr_unpack_from_buffer"));
      DBUG_ASSERT(filesort->sorted_result_in_fsbuf);
      info->unpack_counter= 0;

      if (filesort->using_packed_addons())
      {
        info->read_record_func= using_packed_sortkeys ?
                                rr_unpack_from_buffer<true, true> :
                                rr_unpack_from_buffer<true, false>;
      }
      else
      {
        info->read_record_func= using_packed_sortkeys ?
                                rr_unpack_from_buffer<false, true> :
                                rr_unpack_from_buffer<false, false>;
      }
    }
    else
    {
      info->cache_end= (info->cache_pos+
                        filesort->return_rows * info->ref_length);
      info->read_record_func= rr_from_pointers;
    }
  }
  else if (table->file->keyread_enabled())
  {
    int error;
    info->read_record_func= rr_index_first;
    if (!table->file->inited &&
        unlikely((error= table->file->ha_index_init(table->file->keyread, 1))))
    {
      if (print_error)
        table->file->print_error(error, MYF(0));
      DBUG_RETURN(1);
    }
  }
  else
  {
    DBUG_PRINT("info",("using rr_sequential"));
    info->read_record_func= rr_sequential;
    if (unlikely(table->file->ha_rnd_init_with_error(1)))
      DBUG_RETURN(1);
    /* We can use record cache if we don't update dynamic length tables */
    if (!table->no_cache &&
	(use_record_cache > 0 ||
	 (int) table->reginfo.lock_type <= (int) TL_READ_HIGH_PRIORITY ||
	 !(table->s->db_options_in_use & HA_OPTION_PACK_RECORD) ||
	 (use_record_cache < 0 &&
	  !(table->file->ha_table_flags() & HA_NOT_DELETE_WITH_CACHE))))
      (void) table->file->extra_opt(HA_EXTRA_CACHE,
                                    thd->variables.read_buff_size);
  }
  /* Condition pushdown to storage engine */
  if ((table->file->ha_table_flags() & HA_CAN_TABLE_CONDITION_PUSHDOWN) &&
      select && select->cond && 
      (select->cond->used_tables() & table->map) &&
      !table->file->pushed_cond)
    table->file->cond_push(select->cond);

  DBUG_RETURN(0);
} /* init_read_record */



void end_read_record(READ_RECORD *info)
{
  /* free cache if used */
  free_cache(info);
  if (info->table)
  {
    if (info->table->db_stat) // if opened
      (void) info->table->file->extra(HA_EXTRA_NO_CACHE);
    if (info->read_record_func != rr_quick) // otherwise quick_range does it
      (void) info->table->file->ha_index_or_rnd_end();
    info->table=0;
  }
}


void free_cache(READ_RECORD *info)
{
  if (info->cache)
  {
    my_free_lock(info->cache);
    info->cache=0;
  }
}


static int rr_handle_error(READ_RECORD *info, int error)
{
  if (info->thd->killed)
  {
    info->thd->send_kill_message();
    return 1;
  }

  if (error == HA_ERR_END_OF_FILE)
    error= -1;
  else
  {
    if (info->print_error)
      info->table->file->print_error(error, MYF(0));
    if (error < 0)                            // Fix negative BDB errno
      error= 1;
  }
  return error;
}


/** Read a record from head-database. */

static int rr_quick(READ_RECORD *info)
{
  int tmp;
  while ((tmp= info->select->quick->get_next()))
  {
    tmp= rr_handle_error(info, tmp);
    break;
  }
  return tmp;
}


/**
  Reads first row in an index scan.

  @param info  	Scan info

  @retval
    0   Ok
  @retval
    -1   End of records
  @retval
    1   Error
*/

static int rr_index_first(READ_RECORD *info)
{
  int tmp;
  // tell handler that we are doing an index scan
  if ((tmp = info->table->file->prepare_index_scan())) 
  {
    tmp= rr_handle_error(info, tmp);
    return tmp;
  }

  tmp= info->table->file->ha_index_first(info->record());
  info->read_record_func= rr_index;
  if (tmp)
    tmp= rr_handle_error(info, tmp);
  return tmp;
}


/**
  Reads last row in an index scan.

  @param info  	Scan info

  @retval
    0   Ok
  @retval
    -1   End of records
  @retval
    1   Error
*/

static int rr_index_last(READ_RECORD *info)
{
  int tmp= info->table->file->ha_index_last(info->record());
  info->read_record_func= rr_index_desc;
  if (tmp)
    tmp= rr_handle_error(info, tmp);
  return tmp;
}


/**
  Reads index sequentially after first row.

  Read the next index record (in forward direction) and translate return
  value.

  @param info  Scan info

  @retval
    0   Ok
  @retval
    -1   End of records
  @retval
    1   Error
*/

static int rr_index(READ_RECORD *info)
{
  int tmp= info->table->file->ha_index_next(info->record());
  if (tmp)
    tmp= rr_handle_error(info, tmp);
  return tmp;
}


/**
  Reads index sequentially from the last row to the first.

  Read the prev index record (in backward direction) and translate return
  value.

  @param info  Scan info

  @retval
    0   Ok
  @retval
    -1   End of records
  @retval
    1   Error
*/

static int rr_index_desc(READ_RECORD *info)
{
  int tmp= info->table->file->ha_index_prev(info->record());
  if (tmp)
    tmp= rr_handle_error(info, tmp);
  return tmp;
}


int rr_sequential(READ_RECORD *info)
{
  int tmp;
  while ((tmp= info->table->file->ha_rnd_next(info->record())))
  {
    tmp= rr_handle_error(info, tmp);
    break;
  }
  return tmp;
}


static int rr_from_tempfile(READ_RECORD *info)
{
  int tmp;
  for (;;)
  {
    if (my_b_read(info->io_cache,info->ref_pos,info->ref_length))
      return -1;					/* End of file */
    if (!(tmp= info->table->file->ha_rnd_pos(info->record(), info->ref_pos)))
      break;
    /* The following is extremely unlikely to happen */
    if (tmp == HA_ERR_KEY_NOT_FOUND)
      continue;
    tmp= rr_handle_error(info, tmp);
    break;
  }
  return tmp;
} /* rr_from_tempfile */


/**
  Read a result set record from a temporary file after sorting.

  The function first reads the next sorted record from the temporary file.
  into a buffer. If a success it calls a callback function that unpacks 
  the fields values use in the result set from this buffer into their
  positions in the regular record buffer.

  @param info                 Reference to the context including record
                              descriptors
  @param Packed_addon_fields  Are the addon fields packed?
                              This is a compile-time constant, to
                              avoid if (....) tests during execution.

  @retval
    0   Record successfully read.
  @retval
    -1   There is no record to be read anymore.
*/

template<bool Packed_addon_fields>
static int rr_unpack_from_tempfile(READ_RECORD *info)
{
  uchar *destination= info->rec_buf;
#ifndef DBUG_OFF
  my_off_t where= my_b_tell(info->io_cache);
#endif
  if (Packed_addon_fields)
  {
    const uint len_sz= Addon_fields::size_of_length_field;

    // First read length of the record.
    if (my_b_read(info->io_cache, destination, len_sz))
      return -1;
    uint res_length= Addon_fields::read_addon_length(destination);
    DBUG_PRINT("info", ("rr_unpack from %llu to %p sz %u",
                        static_cast<ulonglong>(where),
                        destination, res_length));
    DBUG_ASSERT(res_length > len_sz);
    DBUG_ASSERT(info->sort_info->using_addon_fields());

    // Then read the rest of the record.
    if (my_b_read(info->io_cache, destination + len_sz, res_length - len_sz))
      return -1;                                /* purecov: inspected */
  }
  else
  {
    if (my_b_read(info->io_cache, destination, info->ref_length))
      return -1;
  }

  info->sort_info->unpack_addon_fields<Packed_addon_fields>(destination);

  return 0;
}

int rr_from_pointers(READ_RECORD *info)
{
  int tmp;
  uchar *cache_pos;

  for (;;)
  {
    if (info->cache_pos == info->cache_end)
      return -1;					/* End of file */
    cache_pos= info->cache_pos;
    info->cache_pos+= info->ref_length;

    if (!(tmp= info->table->file->ha_rnd_pos(info->record(), cache_pos)))
      break;

    /* The following is extremely unlikely to happen */
    if (tmp == HA_ERR_KEY_NOT_FOUND)
      continue;
    tmp= rr_handle_error(info, tmp);
    break;
  }
  return tmp;
}

/**
  Read a result set record from a buffer after sorting.

  The function first reads the next sorted record from the sort buffer.
  If a success it calls a callback function that unpacks 
  the fields values use in the result set from this buffer into their
  positions in the regular record buffer.

  @param info                 Reference to the context including record
                              descriptors
  @param Packed_addon_fields  Are the addon fields packed?
                              This is a compile-time constant, to
                              avoid if (....) tests during execution.

  @retval
    0   Record successfully read.
  @retval
    -1   There is no record to be read anymore.
*/

template<bool Packed_addon_fields, bool Packed_sort_keys>
static int rr_unpack_from_buffer(READ_RECORD *info)
{
  if (info->unpack_counter == info->sort_info->return_rows)
    return -1;                      /* End of buffer */

  uchar *record= info->sort_info->get_sorted_record(
    static_cast<uint>(info->unpack_counter));

  uint sort_length= Packed_sort_keys ?
                    Sort_keys::read_sortkey_length(record):
                    info->sort_info->get_sort_length();

  uchar *plen= record + sort_length;
  info->sort_info->unpack_addon_fields<Packed_addon_fields>(plen);
  info->unpack_counter++;
  return 0;
}
	/* cacheing of records from a database */

static const uint STRUCT_LENGTH= 3 + MAX_REFLENGTH;

static int init_rr_cache(THD *thd, READ_RECORD *info)
{
  uint rec_cache_size, cache_records;
  DBUG_ENTER("init_rr_cache");

  info->reclength= ALIGN_SIZE(info->table->s->reclength+1);
  if (info->reclength < STRUCT_LENGTH)
    info->reclength= ALIGN_SIZE(STRUCT_LENGTH);

  info->error_offset= info->table->s->reclength;
  cache_records= thd->variables.read_rnd_buff_size /
                 (info->reclength + STRUCT_LENGTH);
  rec_cache_size= cache_records * info->reclength;
  info->rec_cache_size= cache_records * info->ref_length;

  // We have to allocate one more byte to use uint3korr (see comments for it)
  if (cache_records <= 2 ||
      !(info->cache= (uchar*) my_malloc_lock(rec_cache_size + cache_records *
                                             STRUCT_LENGTH + 1,
                                             MYF(MY_THREAD_SPECIFIC))))
    DBUG_RETURN(1);
#ifdef HAVE_valgrind
  // Avoid warnings in qsort
  bzero(info->cache, rec_cache_size + cache_records * STRUCT_LENGTH + 1);
#endif
  DBUG_PRINT("info", ("Allocated buffer for %d records", cache_records));
  info->read_positions=info->cache+rec_cache_size;
  info->cache_pos=info->cache_end=info->cache;
  DBUG_RETURN(0);
} /* init_rr_cache */


static int rr_from_cache(READ_RECORD *info)
{
  uint i;
  ulong length;
  my_off_t rest_of_file;
  int16 error;
  uchar *position,*ref_position,*record_pos;
  ulong record;

  for (;;)
  {
    if (info->cache_pos != info->cache_end)
    {
      if (unlikely(info->cache_pos[info->error_offset]))
      {
	shortget(error,info->cache_pos);
	if (info->print_error)
	  info->table->file->print_error(error,MYF(0));
      }
      else
      {
	error=0;
        memcpy(info->record(), info->cache_pos,
               (size_t) info->table->s->reclength);
      }
      info->cache_pos+=info->reclength;
      return ((int) error);
    }
    length=info->rec_cache_size;
    rest_of_file=info->io_cache->end_of_file - my_b_tell(info->io_cache);
    if ((my_off_t) length > rest_of_file)
      length= (ulong) rest_of_file;
    if (!length || my_b_read(info->io_cache,info->cache,length))
    {
      DBUG_PRINT("info",("Found end of file"));
      return -1;			/* End of file */
    }

    length/=info->ref_length;
    position=info->cache;
    ref_position=info->read_positions;
    for (i=0 ; i < length ; i++,position+=info->ref_length)
    {
      memcpy(ref_position,position,(size_t) info->ref_length);
      ref_position+=MAX_REFLENGTH;
      int3store(ref_position,(long) i);
      ref_position+=3;
    }
    my_qsort(info->read_positions, length, STRUCT_LENGTH, (qsort_cmp) rr_cmp);

    position=info->read_positions;
    for (i=0 ; i < length ; i++)
    {
      memcpy(info->ref_pos,position,(size_t) info->ref_length);
      position+=MAX_REFLENGTH;
      record=uint3korr(position);
      position+=3;
      record_pos=info->cache+record*info->reclength;
      if (unlikely((error= (int16) info->table->file->
                    ha_rnd_pos(record_pos,info->ref_pos))))
      {
	record_pos[info->error_offset]=1;
	shortstore(record_pos,error);
	DBUG_PRINT("error",("Got error: %d:%d when reading row",
			    my_errno, error));
      }
      else
	record_pos[info->error_offset]=0;
    }
    info->cache_end=(info->cache_pos=info->cache)+length*info->reclength;
  }
} /* rr_from_cache */


static int rr_cmp(uchar *a,uchar *b)
{
  if (a[0] != b[0])
    return (int) a[0] - (int) b[0];
  if (a[1] != b[1])
    return (int) a[1] - (int) b[1];
  if (a[2] != b[2])
    return (int) a[2] - (int) b[2];
#if MAX_REFLENGTH == 4
  return (int) a[3] - (int) b[3];
#else
  if (a[3] != b[3])
    return (int) a[3] - (int) b[3];
  if (a[4] != b[4])
    return (int) a[4] - (int) b[4];
  if (a[5] != b[5])
    return (int) a[5] - (int) b[5];
  if (a[6] != b[6])
    return (int) a[6] - (int) b[6];
  return (int) a[7] - (int) b[7];
#endif
}


/**
  Copy (unpack) values appended to sorted fields from a buffer back to
  their regular positions specified by the Field::ptr pointers.

  @param addon_field     Array of descriptors for appended fields
  @param buff            Buffer which to unpack the value from

  @note
    The function is supposed to be used only as a callback function
    when getting field values for the sorted result set.

*/
template<bool Packed_addon_fields>
inline void SORT_INFO::unpack_addon_fields(uchar *buff)
{
  SORT_ADDON_FIELD *addonf= addon_fields->begin();
  uchar *buff_end= buff + sort_buffer_size();
  const uchar *start_of_record= buff + addonf->offset;

  for ( ; addonf != addon_fields->end() ; addonf++)
  {
    Field *field= addonf->field;
    if (addonf->null_bit && (addonf->null_bit & buff[addonf->null_offset]))
    {
      field->set_null();
      continue;
    }
    field->set_notnull();
    if (Packed_addon_fields)
      start_of_record= field->unpack(field->ptr, start_of_record, buff_end, 0);
    else
      field->unpack(field->ptr, buff + addonf->offset, buff_end, 0);
  }
}


/*
  @brief
    Read and unpack next record from a table

  @details
    The function first reads the next record from the table.
    If a success then it unpacks the values to the base table fields.
    This is used by SJM scan table to unpack the values of the materialized
    table to the base table fields

  @retval
    0   Record successfully read.
  @retval
    -1   There is no record to be read anymore.
    >0   Error
*/
int read_record_func_for_rr_and_unpack(READ_RECORD *info)
{
  int error;
  if ((error= info->read_record_func_and_unpack_calls(info)))
    return error;

  for (Copy_field *cp= info->copy_field; cp != info->copy_field_end; cp++)
    (*cp->do_copy)(cp);

  return error;
}
