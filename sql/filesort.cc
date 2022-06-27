/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


/**
  @file

  @brief
  Sorts a database
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "filesort.h"
#include <m_ctype.h>
#include "sql_sort.h"
#include "probes_mysql.h"
#include "sql_base.h"
#include "sql_test.h"                           // TEST_filesort
#include "opt_range.h"                          // SQL_SELECT
#include "bounded_queue.h"
#include "filesort_utils.h"
#include "sql_select.h"
#include "debug_sync.h"

	/* functions defined in this file */

static uchar *read_buffpek_from_file(IO_CACHE *buffer_file, uint count,
                                     uchar *buf);
static ha_rows find_all_keys(THD *thd, Sort_param *param, SQL_SELECT *select,
                             SORT_INFO *fs_info,
                             IO_CACHE *buffer_file,
                             IO_CACHE *tempfile,
                             Bounded_queue<uchar, uchar> *pq,
                             ha_rows *found_rows);
static bool write_keys(Sort_param *param, SORT_INFO *fs_info,
                      uint count, IO_CACHE *buffer_file, IO_CACHE *tempfile);
static uint make_sortkey(Sort_param *param, uchar *to, uchar *ref_pos,
                         bool using_packed_sortkeys= false);
static uint make_sortkey(Sort_param *param, uchar *to);
static uint make_packed_sortkey(Sort_param *param, uchar *to);

static void register_used_fields(Sort_param *param);
static bool save_index(Sort_param *param, uint count,
                       SORT_INFO *table_sort);
static uint suffix_length(ulong string_length);
static uint sortlength(THD *thd, Sort_keys *sortorder,
                       bool *allow_packing_for_sortkeys);
static Addon_fields *get_addon_fields(TABLE *table, uint sortlength,
                                      uint *addon_length,
                                      uint *m_packable_length);

static bool check_if_pq_applicable(Sort_param *param, SORT_INFO *info,
                                   TABLE *table,
                                   ha_rows records, size_t memory_available);

static void store_key_part_length(uint32 num, uchar *to, uint bytes)
{
  switch(bytes) {
  case 1: *to= (uchar)num;    break;
  case 2: int2store(to, num); break;
  case 3: int3store(to, num); break;
  case 4: int4store(to, num); break;
  default: DBUG_ASSERT(0);
  }
}


static uint32 read_keypart_length(const uchar *from, uint bytes)
{
  switch(bytes) {
  case 1: return from[0];
  case 2: return uint2korr(from);
  case 3: return uint3korr(from);
  case 4: return uint4korr(from);
  default: DBUG_ASSERT(0); return 0;
  }
}


// @param sortlen  [Maximum] length of the sort key
void Sort_param::init_for_filesort(uint sortlen, TABLE *table,
                                   ha_rows maxrows, Filesort *filesort)
{
  DBUG_ASSERT(addon_fields == NULL);

  sort_length= sortlen;
  ref_length= table->file->ref_length;
  accepted_rows= filesort->accepted_rows;

  if (!(table->file->ha_table_flags() & HA_FAST_KEY_READ) &&
      !table->fulltext_searched && !filesort->sort_positions)
  {
    /* 
      Get the descriptors of all fields whose values are appended 
      to sorted fields and get its total length in addon_buf.length
    */
    addon_fields= get_addon_fields(table, sort_length, &addon_length,
                                   &m_packable_length);
  }
  if (using_addon_fields())
  {
    DBUG_ASSERT(addon_length < UINT_MAX32);
    res_length= addon_length;
  }
  else
  {
    res_length= ref_length;
    /* 
      The reference to the record is considered 
      as an additional sorted field
    */
    sort_length+= ref_length;
  }
  rec_length= sort_length + addon_length;
  max_rows= maxrows;
}


void Sort_param::try_to_pack_addons(ulong max_length_for_sort_data)
{
  if (!using_addon_fields() ||                  // no addons, or
      using_packed_addons())                    // already packed
    return;

  if (!Addon_fields::can_pack_addon_fields(res_length))
    return;

  const uint sz= Addon_fields::size_of_length_field;

  // Heuristic: skip packing if potential savings are less than 10 bytes.
  if (m_packable_length < (10 + sz))
    return;

  SORT_ADDON_FIELD *addonf= addon_fields->begin();
  for (;addonf != addon_fields->end(); ++addonf)
  {
    addonf->offset+= sz;
    addonf->null_offset+= sz;
  }

  addon_fields->set_using_packed_addons(true);
  m_using_packed_addons= true;
  m_packed_format= true;

  addon_length+= sz;
  res_length+= sz;
  rec_length+= sz;
}

/**
  Sort a table.
  Creates a set of pointers that can be used to read the rows
  in sorted order. This should be done with the functions
  in records.cc.

  Before calling filesort, one must have done
  table->file->info(HA_STATUS_VARIABLE)

  The result set is stored in
  filesort_info->io_cache or
  filesort_info->record_pointers.

  @param      thd            Current thread
  @param      table          Table to sort
  @param      filesort       How to sort the table
  @param[out] found_rows     Store the number of found rows here.
                             This is the number of found rows after
                             applying WHERE condition.
  @note
    If we sort by position (like if filesort->sort_positions==true) 
    filesort() will call table->prepare_for_position().

  @retval
    0			Error
    #			SORT_INFO
*/

SORT_INFO *filesort(THD *thd, TABLE *table, Filesort *filesort,
                    Filesort_tracker* tracker, JOIN *join,
                    table_map first_table_bit)
{
  int error;
  DBUG_ASSERT(thd->variables.sortbuff_size <= SIZE_T_MAX);
  size_t memory_available= (size_t)thd->variables.sortbuff_size;
  uint maxbuffer;
  Merge_chunk *buffpek;
  ha_rows num_rows= HA_POS_ERROR, not_used=0;
  IO_CACHE tempfile, buffpek_pointers, *outfile; 
  Sort_param param;
  bool allow_packing_for_sortkeys;
  Bounded_queue<uchar, uchar> pq;
  SQL_SELECT *const select= filesort->select;
  ha_rows max_rows= filesort->limit;
  uint s_length= 0, sort_len;
  Sort_keys *sort_keys;
  DBUG_ENTER("filesort");

  if (!(sort_keys= filesort->make_sortorder(thd, join, first_table_bit)))
    DBUG_RETURN(NULL);  /* purecov: inspected */

  s_length= static_cast<uint>(sort_keys->size());

  DBUG_EXECUTE("info",TEST_filesort(filesort->sortorder, s_length););
#ifdef SKIP_DBUG_IN_FILESORT
  DBUG_PUSH_EMPTY;		/* No DBUG here */
#endif
  SORT_INFO *sort;
  TABLE_LIST *tab= table->pos_in_table_list;
  Item_subselect *subselect= tab ? tab->containing_subselect() : 0;
  MYSQL_FILESORT_START(table->s->db.str, table->s->table_name.str);
  DEBUG_SYNC(thd, "filesort_start");

  if (!(sort= new SORT_INFO))
    return 0;

  if (subselect && subselect->filesort_buffer.is_allocated())
  {
    // Reuse cache from last call
    sort->filesort_buffer= subselect->filesort_buffer;
    sort->buffpek= subselect->sortbuffer;
    subselect->filesort_buffer.reset();
    subselect->sortbuffer.str=0;
  }

  DBUG_ASSERT(sort->sorted_result_in_fsbuf == FALSE ||
              sort->record_pointers == NULL);

  outfile= &sort->io_cache;

  my_b_clear(&tempfile);
  my_b_clear(&buffpek_pointers);
  buffpek=0;
  error= 1;
  sort->found_rows= HA_POS_ERROR;

  param.sort_keys= sort_keys;
  sort_len= sortlength(thd, sort_keys, &allow_packing_for_sortkeys);
  param.init_for_filesort(sort_len, table, max_rows, filesort);
  if (!param.accepted_rows)
    param.accepted_rows= &not_used;

  param.set_all_read_bits= filesort->set_all_read_bits;
  param.unpack= filesort->unpack;

  sort->addon_fields=  param.addon_fields;
  sort->sort_keys= param.sort_keys;

  if (select && select->quick)
    thd->inc_status_sort_range();
  else
    thd->inc_status_sort_scan();
  thd->query_plan_flags|= QPLAN_FILESORT;
  tracker->report_use(thd, max_rows);

  // If number of rows is not known, use as much of sort buffer as possible. 
  num_rows= table->file->estimate_rows_upper_bound();

  if (check_if_pq_applicable(&param, sort,
                             table, num_rows, memory_available))
  {
    DBUG_PRINT("info", ("filesort PQ is applicable"));
    thd->query_plan_flags|= QPLAN_FILESORT_PRIORITY_QUEUE;
    status_var_increment(thd->status_var.filesort_pq_sorts_);
    tracker->incr_pq_used();
    param.using_pq= true;
    const size_t compare_length= param.sort_length;
    DBUG_ASSERT(param.using_packed_sortkeys() == false);
    /*
      For PQ queries (with limit) we know exactly how many pointers/records
      we have in the buffer, so to simplify things, we initialize
      all pointers here. (We cannot pack fields anyways, so there is no
      point in doing lazy initialization).
    */
    sort->init_record_pointers();
    if (pq.init(param.max_rows,
                true,                           // max_at_top
                NULL,                           // compare_function
                compare_length,
                &make_sortkey, &param, sort->get_sort_keys()))
    {
      /*
       If we fail to init pq, we have to give up:
       out of memory means my_malloc() will call my_error().
      */
      DBUG_PRINT("info", ("failed to allocate PQ"));
      DBUG_ASSERT(thd->is_error());
      goto err;
    }
  }
  else
  {
    DBUG_PRINT("info", ("filesort PQ is not applicable"));

    if (allow_packing_for_sortkeys)
      param.try_to_pack_sortkeys();

    param.try_to_pack_addons(thd->variables.max_length_for_sort_data);
    tracker->report_sort_keys_format(param.using_packed_sortkeys());
    param.using_pq= false;

    size_t min_sort_memory= MY_MAX(MIN_SORT_MEMORY,
                                   param.sort_length*MERGEBUFF2);
    set_if_bigger(min_sort_memory, sizeof(Merge_chunk*)*MERGEBUFF2);
    while (memory_available >= min_sort_memory)
    {
      ulonglong keys= memory_available / (param.rec_length + sizeof(char*));
      param.max_keys_per_buffer= (uint) MY_MAX(MERGEBUFF2,
                                               MY_MIN(num_rows, keys));
      sort->alloc_sort_buffer(param.max_keys_per_buffer, param.rec_length);
      if (sort->sort_buffer_size() > 0)
        break;
      size_t old_memory_available= memory_available;
      memory_available= memory_available/4*3;
      if (memory_available < min_sort_memory &&
          old_memory_available > min_sort_memory)
        memory_available= min_sort_memory;
    }
    if (memory_available < min_sort_memory)
    {
      my_error(ER_OUT_OF_SORTMEMORY,MYF(ME_ERROR_LOG + ME_FATAL));
      goto err;
    }
    tracker->report_sort_buffer_size(sort->sort_buffer_size());
  }

  if (param.using_addon_fields())
  {
    // report information whether addon fields are packed or not
    tracker->report_addon_fields_format(param.using_packed_addons());
  }

  if (param.tmp_buffer.alloc(param.sort_length))
    goto err;

  if (open_cached_file(&buffpek_pointers,mysql_tmpdir,TEMP_PREFIX,
		       DISK_BUFFER_SIZE, MYF(MY_WME)))
    goto err;

  param.sort_form= table;
  param.local_sortorder=
    Bounds_checked_array<SORT_FIELD>(filesort->sortorder, s_length);

  num_rows= find_all_keys(thd, &param, select,
                          sort,
                          &buffpek_pointers,
                          &tempfile, 
                          pq.is_initialized() ? &pq : NULL,
                          &sort->found_rows);
  if (num_rows == HA_POS_ERROR)
    goto err;

  maxbuffer= (uint) (my_b_tell(&buffpek_pointers)/sizeof(*buffpek));
  tracker->report_merge_passes_at_start(thd->query_plan_fsort_passes);
  tracker->report_row_numbers(param.examined_rows, sort->found_rows, num_rows);

  if (maxbuffer == 0)			// The whole set is in memory
  {
    if (save_index(&param, (uint) num_rows, sort))
      goto err;
  }
  else
  {
    /* filesort cannot handle zero-length records during merge. */
    DBUG_ASSERT(param.sort_length != 0);

    if (sort->buffpek.str && sort->buffpek.length < maxbuffer)
    {
      my_free(sort->buffpek.str);
      sort->buffpek.str= 0;
    }

    if (param.using_addon_fields())
    {
      DBUG_ASSERT(sort->addon_fields);
      if (!sort->addon_fields->allocate_addon_buf(param.addon_length))
        goto err;
    }

    if (!(sort->buffpek.str=
          (char *) read_buffpek_from_file(&buffpek_pointers, maxbuffer,
                                          (uchar*) sort->buffpek.str)))
      goto err;
    sort->buffpek.length= maxbuffer;
    buffpek= (Merge_chunk *) sort->buffpek.str;
    close_cached_file(&buffpek_pointers);
	/* Open cached file if it isn't open */
    if (! my_b_inited(outfile) &&
	open_cached_file(outfile,mysql_tmpdir,TEMP_PREFIX,READ_RECORD_BUFFER,
			  MYF(MY_WME)))
      goto err;
    if (reinit_io_cache(outfile,WRITE_CACHE,0L,0,0))
      goto err;

    /*
      Use also the space previously used by string pointers in sort_buffer
      for temporary key storage.
    */

    param.max_keys_per_buffer= static_cast<uint>(sort->sort_buffer_size()) /
                               param.rec_length;
    set_if_bigger(param.max_keys_per_buffer, 1);
    maxbuffer--;				// Offset from 0

    if (merge_many_buff(&param, sort->get_raw_buf(),
                        buffpek,&maxbuffer,
	                      &tempfile))
      goto err;
    if (flush_io_cache(&tempfile) ||
	reinit_io_cache(&tempfile,READ_CACHE,0L,0,0))
      goto err;
    if (merge_index(&param,
                    sort->get_raw_buf(),
                    buffpek,
                    maxbuffer,
                    &tempfile,
                    outfile))
      goto err;
  }

  if (num_rows > param.max_rows)
  {
    // If find_all_keys() produced more results than the query LIMIT.
    num_rows= param.max_rows;
  }
  error= 0;

  err:
  if (!subselect || !subselect->is_uncacheable())
  {
    if (!param.using_addon_fields())
      sort->free_sort_buffer();
    my_free(sort->buffpek.str);
  }
  else
  {
    /* Remember sort buffers for next subquery call */
    subselect->filesort_buffer= sort->filesort_buffer;
    subselect->sortbuffer=      sort->buffpek;
    sort->filesort_buffer.reset();              // Don't free this*/
  }
  sort->buffpek.str= 0;

  close_cached_file(&tempfile);
  close_cached_file(&buffpek_pointers);
  if (my_b_inited(outfile))
  {
    if (flush_io_cache(outfile))
      error=1;
    {
      my_off_t save_pos=outfile->pos_in_file;
      /* For following reads */
      if (reinit_io_cache(outfile,READ_CACHE,0L,0,0))
        error=1;
      outfile->end_of_file=save_pos;
    }
  }
  tracker->report_merge_passes_at_end(thd, thd->query_plan_fsort_passes);
  if (unlikely(error))
  {
    int kill_errno= thd->killed_errno();
    DBUG_ASSERT(thd->is_error() || kill_errno || thd->killed == ABORT_QUERY);

    my_printf_error(ER_FILSORT_ABORT,
                    "%s: %s",
                    MYF(0),
                    ER_THD(thd, ER_FILSORT_ABORT),
                    kill_errno ? ER_THD(thd, kill_errno) :
                    thd->killed == ABORT_QUERY ? "" :
                    thd->get_stmt_da()->message());

    if ((thd->killed == ABORT_QUERY || kill_errno) &&
        global_system_variables.log_warnings > 1)
    { 
      sql_print_warning("%s, host: %s, user: %s, thread: %lu, query: %-.4096s",
                        ER_THD(thd, ER_FILSORT_ABORT),
                        thd->security_ctx->host_or_ip,
                        &thd->security_ctx->priv_user[0],
                        (ulong) thd->thread_id,
                        thd->query());
    }
  }
  else
    thd->inc_status_sort_rows(num_rows);

  sort->examined_rows= param.examined_rows;
  sort->return_rows= num_rows;
#ifdef SKIP_DBUG_IN_FILESORT
  DBUG_POP_EMPTY;		/* Ok to DBUG */
#endif

  DBUG_PRINT("exit",
             ("num_rows: %lld examined_rows: %lld found_rows: %lld",
              (longlong) sort->return_rows, (longlong) sort->examined_rows,
              (longlong) sort->found_rows));
  MYSQL_FILESORT_DONE(error, num_rows);

  if (unlikely(error))
  {
    delete sort;
    sort= 0;
  }
  DBUG_RETURN(sort);
} /* filesort */


void Filesort::cleanup()
{
  if (select && own_select)
  {
    select->cleanup();
    select= NULL;
  }
}


/*
  Create the Sort_keys array and fill the sort_keys[i]->{item|field}.

  This indicates which field/item values will be used as sort keys.
  Attributes like lengths are not filled yet.
*/

Sort_keys*
Filesort::make_sortorder(THD *thd, JOIN *join, table_map first_table_bit)
{
  uint count;
  SORT_FIELD *sort,*pos;
  ORDER *ord;
  DBUG_ENTER("make_sortorder");

  count=0;
  for (ord = order; ord; ord= ord->next)
    count++;

  if (sortorder)
    DBUG_RETURN(sort_keys);

  DBUG_ASSERT(sort_keys == NULL);

  sortorder= (SORT_FIELD*) thd->alloc(sizeof(SORT_FIELD) * count);
  pos= sort= sortorder;

  if (!pos)
    DBUG_RETURN(0);

  sort_keys= new Sort_keys(sortorder, count);

  if (!sort_keys)
    DBUG_RETURN(0);

  pos= sort_keys->begin();
  for (ord= order; ord; ord= ord->next, pos++)
  {
    Item *first= ord->item[0];
    /*
      It is possible that the query plan is to read table t1, while the
      sort criteria actually has "ORDER BY t2.col" and the WHERE clause has
      a multi-equality(t1.col, t2.col, ...).
      The optimizer detects such cases (grep for
      UseMultipleEqualitiesToRemoveTempTable to see where), but doesn't
      perform equality substitution in the order->item. We need to do the
      substitution here ourselves.
    */
    table_map item_map= first->used_tables();
    if (join && (item_map & ~join->const_table_map) &&
        !(item_map & first_table_bit) && join->cond_equal &&
         first->get_item_equal())
    {
      /*
        Ok, this is the case descibed just above. Get the first element of the
        multi-equality.
      */
      Item_equal *item_eq= first->get_item_equal();
      first= item_eq->get_first(NO_PARTICULAR_TAB, NULL);
    }

    Item *item= first->real_item();
    pos->field= 0; pos->item= 0;
    if (item->type() == Item::FIELD_ITEM)
      pos->field= ((Item_field*) item)->field;
    else if (item->type() == Item::SUM_FUNC_ITEM && !item->const_item())
    {
      // Aggregate, or Item_aggregate_ref
      DBUG_ASSERT(first->type() == Item::SUM_FUNC_ITEM ||
                  (first->type() == Item::REF_ITEM &&
                   static_cast<Item_ref*>(first)->ref_type() ==
                   Item_ref::AGGREGATE_REF));
      pos->field= first->get_tmp_table_field();
    }
    else if (item->type() == Item::COPY_STR_ITEM)
    {						// Blob patch
      pos->item= ((Item_copy*) item)->get_item();
    }
    else
      pos->item= *ord->item;
    pos->reverse= (ord->direction == ORDER::ORDER_DESC);
    DBUG_ASSERT(pos->field != NULL || pos->item != NULL);
  }
  DBUG_RETURN(sort_keys);
}


/** Read 'count' number of buffer pointers into memory. */

static uchar *read_buffpek_from_file(IO_CACHE *buffpek_pointers, uint count,
                                     uchar *buf)
{
  size_t length= sizeof(Merge_chunk)*count;
  uchar *tmp= buf;
  DBUG_ENTER("read_buffpek_from_file");
  if (count > UINT_MAX/sizeof(Merge_chunk))
    return 0; /* sizeof(BUFFPEK)*count will overflow */
  if (!tmp)
    tmp= (uchar *)my_malloc(key_memory_Filesort_info_merge, length,
                            MYF(MY_WME | MY_THREAD_SPECIFIC));
  if (tmp)
  {
    if (reinit_io_cache(buffpek_pointers,READ_CACHE,0L,0,0) ||
	my_b_read(buffpek_pointers, (uchar*) tmp, length))
    {
      my_free(tmp);
      tmp=0;
    }
  }
  DBUG_RETURN(tmp);
}

#ifndef DBUG_OFF

/* Buffer where record is returned */
char dbug_print_row_buff[512];

/* Temporary buffer for printing a column */
char dbug_print_row_buff_tmp[512];

/*
  Print table's current row into a buffer and return a pointer to it.

  This is intended to be used from gdb:
  
    (gdb) p dbug_print_table_row(table)
      $33 = "SUBQUERY2_t1(col_int_key,col_varchar_nokey)=(7,c)"
    (gdb)

  Only columns in table->read_set are printed
*/

const char* dbug_print_table_row(TABLE *table)
{
  Field **pfield;
  String tmp(dbug_print_row_buff_tmp,
             sizeof(dbug_print_row_buff_tmp),&my_charset_bin);

  String output(dbug_print_row_buff, sizeof(dbug_print_row_buff),
                &my_charset_bin);

  output.length(0);
  output.append(table->alias);
  output.append('(');
  bool first= true;

  for (pfield= table->field; *pfield ; pfield++)
  {
    const LEX_CSTRING *name;
    if (table->read_set && !bitmap_is_set(table->read_set, (*pfield)->field_index))
      continue;
    
    if (first)
      first= false;
    else
      output.append(',');

    name= (*pfield)->field_name.str ? &(*pfield)->field_name: &NULL_clex_str;
    output.append(name);
  }

  output.append(STRING_WITH_LEN(")=("));

  first= true;
  for (pfield= table->field; *pfield ; pfield++)
  {
    Field *field=  *pfield;

    if (table->read_set && !bitmap_is_set(table->read_set, (*pfield)->field_index))
      continue;

    if (first)
      first= false;
    else
      output.append(',');

    if (field->is_null())
      output.append(&NULL_clex_str);
    else
    {
      if (field->type() == MYSQL_TYPE_BIT)
        (void) field->val_int_as_str(&tmp, 1);
      else
        field->val_str(&tmp);
      output.append(tmp.ptr(), tmp.length());
    }
  }
  output.append(')');
  
  return output.c_ptr_safe();
}


const char* dbug_print_row(TABLE *table, uchar *rec)
{
  table->move_fields(table->field, rec, table->record[0]);
  const char* ret= dbug_print_table_row(table);
  table->move_fields(table->field, table->record[0], rec);
  return ret;
}


/*
  Print a text, SQL-like record representation into dbug trace.

  Note: this function is a work in progress: at the moment
   - column read bitmap is ignored (can print garbage for unused columns)
   - there is no quoting
*/
static void dbug_print_record(TABLE *table, bool print_rowid)
{
  char buff[1024];
  Field **pfield;
  String tmp(buff,sizeof(buff),&my_charset_bin);
  DBUG_LOCK_FILE;
  
  fprintf(DBUG_FILE, "record (");
  for (pfield= table->field; *pfield ; pfield++)
    fprintf(DBUG_FILE, "%s%s", (*pfield)->field_name.str,
            (pfield[1])? ", ":"");
  fprintf(DBUG_FILE, ") = ");

  fprintf(DBUG_FILE, "(");
  for (pfield= table->field; *pfield ; pfield++)
  {
    Field *field=  *pfield;

    if (field->is_null())
      fwrite("NULL", sizeof(char), 4, DBUG_FILE);
   
    if (field->type() == MYSQL_TYPE_BIT)
      (void) field->val_int_as_str(&tmp, 1);
    else
      field->val_str(&tmp);

    fwrite(tmp.ptr(),sizeof(char),tmp.length(),DBUG_FILE);
    if (pfield[1])
      fwrite(", ", sizeof(char), 2, DBUG_FILE);
  }
  fprintf(DBUG_FILE, ")");
  if (print_rowid)
  {
    fprintf(DBUG_FILE, " rowid ");
    for (uint i=0; i < table->file->ref_length; i++)
    {
      fprintf(DBUG_FILE, "%x", (uchar)table->file->ref[i]);
    }
  }
  fprintf(DBUG_FILE, "\n");
  DBUG_UNLOCK_FILE;
}

#endif 


/**
  Search after sort_keys, and write them into tempfile
  (if we run out of space in the sort_keys buffer).
  All produced sequences are guaranteed to be non-empty.

  @param param             Sorting parameter
  @param select            Use this to get source data
  @param sort_keys         Array of pointers to sort key + addon buffers.
  @param buffpek_pointers  File to write BUFFPEKs describing sorted segments
                           in tempfile.
  @param tempfile          File to write sorted sequences of sortkeys to.
  @param pq                If !NULL, use it for keeping top N elements
  @param [out] found_rows  The number of FOUND_ROWS().
                           For a query with LIMIT, this value will typically
                           be larger than the function return value.

  @note
    Basic idea:
    @verbatim
     while (get_next_sortkey())
     {
       if (using priority queue)
         push sort key into queue
       else
       {
         if (no free space in sort_keys buffers)
         {
           sort sort_keys buffer;
           dump sorted sequence to 'tempfile';
           dump BUFFPEK describing sequence location into 'buffpek_pointers';
         }
         put sort key into 'sort_keys';
       }
     }
     if (sort_keys has some elements && dumped at least once)
       sort-dump-dump as above;
     else
       don't sort, leave sort_keys array to be sorted by caller.
  @endverbatim

  @retval
    Number of records written on success.
  @retval
    HA_POS_ERROR on error.
*/

static ha_rows find_all_keys(THD *thd, Sort_param *param, SQL_SELECT *select,
                             SORT_INFO *fs_info,
                             IO_CACHE *buffpek_pointers,
                             IO_CACHE *tempfile,
                             Bounded_queue<uchar, uchar> *pq,
                             ha_rows *found_rows)
{
  int error, quick_select;
  uint idx, indexpos;
  uchar *ref_pos, *next_pos, ref_buff[MAX_REFLENGTH];
  TABLE *sort_form;
  handler *file;
  MY_BITMAP *save_read_set, *save_write_set;
  Item *sort_cond;
  ha_rows num_records= 0;
  const bool packed_format= param->is_packed_format();
  const bool using_packed_sortkeys= param->using_packed_sortkeys();

  DBUG_ENTER("find_all_keys");
  DBUG_PRINT("info",("using: %s",
                     (select ? select->quick ? "ranges" : "where":
                      "every row")));

  idx=indexpos=0;
  error=quick_select=0;
  sort_form=param->sort_form;
  file=sort_form->file;
  ref_pos= ref_buff;
  quick_select=select && select->quick;
  *found_rows= 0;
  ref_pos= &file->ref[0];
  next_pos=ref_pos;

  DBUG_EXECUTE_IF("show_explain_in_find_all_keys", 
                  dbug_serve_apcs(thd, 1);
                 );

  if (!quick_select)
  {
    next_pos=(uchar*) 0;			/* Find records in sequence */
    DBUG_EXECUTE_IF("bug14365043_1",
                    DBUG_SET("+d,ha_rnd_init_fail"););
    if (unlikely(file->ha_rnd_init_with_error(1)))
      DBUG_RETURN(HA_POS_ERROR);
    file->extra_opt(HA_EXTRA_CACHE, thd->variables.read_buff_size);
  }

  /* Remember original bitmaps */
  save_read_set=  sort_form->read_set;
  save_write_set= sort_form->write_set;

  /* Set up temporary column read map for columns used by sort */
  DBUG_ASSERT(save_read_set != &sort_form->tmp_set);
  bitmap_clear_all(&sort_form->tmp_set);
  sort_form->column_bitmaps_set(&sort_form->tmp_set, &sort_form->tmp_set);
  register_used_fields(param);
  if (quick_select)
    select->quick->add_used_key_part_to_set();

  sort_cond= (!select ? 0 :
              (!select->pre_idx_push_select_cond ?
               select->cond : select->pre_idx_push_select_cond));
  if (sort_cond)
    sort_cond->walk(&Item::register_field_in_read_map, 1, sort_form);
  sort_form->file->column_bitmaps_signal();

  if (quick_select)
  {
    if (select->quick->reset())
      goto err;
  }

  if (param->set_all_read_bits)
    sort_form->column_bitmaps_set(save_read_set, save_write_set);
  DEBUG_SYNC(thd, "after_index_merge_phase1");

  for (;;)
  {
    if (quick_select)
      error= select->quick->get_next();
    else					/* Not quick-select */
    {
      error= file->ha_rnd_next(sort_form->record[0]);
      if (param->unpack)
        param->unpack(sort_form);
    }
    if (unlikely(error))
      break;
    file->position(sort_form->record[0]);
    DBUG_EXECUTE_IF("debug_filesort", dbug_print_record(sort_form, TRUE););

    if (unlikely(thd->check_killed()))
    {
      DBUG_PRINT("info",("Sort killed by user"));
      if (!quick_select)
      {
        (void) file->extra(HA_EXTRA_NO_CACHE);
        file->ha_rnd_end();
      }
      goto err;                               /* purecov: inspected */
    }

    bool write_record= false;
    if (likely(error == 0))
    {
      param->examined_rows++;
      if (select && select->cond)
      {
        /*
          If the condition 'select->cond' contains a subquery, restore the
          original read/write sets of the table 'sort_form' because when
          SQL_SELECT::skip_record evaluates this condition. it may include a
          correlated subquery predicate, such that some field in the subquery
          refers to 'sort_form'.

          PSergey-todo: discuss the above with Timour.
        */
        MY_BITMAP *tmp_read_set= sort_form->read_set;
        MY_BITMAP *tmp_write_set= sort_form->write_set;

        if (select->cond->with_subquery())
          sort_form->column_bitmaps_set(save_read_set, save_write_set);
        write_record= (select->skip_record(thd) > 0);
        if (select->cond->with_subquery())
          sort_form->column_bitmaps_set(tmp_read_set, tmp_write_set);
      }
      else
        write_record= true;
    }

    if (write_record)
    {
      if (pq)
        pq->push(ref_pos);
      else
      {
        if (fs_info->isfull())
        {
          if (write_keys(param, fs_info, idx, buffpek_pointers, tempfile))
            goto err;
          idx= 0;
          indexpos++;
        }
        if (idx == 0)
          fs_info->init_next_record_pointer();
        uchar *start_of_rec= fs_info->get_next_record_pointer();

        const uint rec_sz= make_sortkey(param, start_of_rec,
                                        ref_pos, using_packed_sortkeys);
        if (packed_format && rec_sz != param->rec_length)
          fs_info->adjust_next_record_pointer(rec_sz);
        idx++;
      }
      num_records++;
      (*param->accepted_rows)++;
    }

    /* It does not make sense to read more keys in case of a fatal error */
    if (unlikely(thd->is_error()))
      break;

    /*
      We need to this after checking the error as the transaction may have
      rolled back in case of a deadlock
    */
    if (!write_record)
      file->unlock_row();
  }
  if (!quick_select)
  {
    (void) file->extra(HA_EXTRA_NO_CACHE);	/* End caching of records */
    if (!next_pos)
      file->ha_rnd_end();
  }

  /* Signal we should use original column read and write maps */
  sort_form->column_bitmaps_set(save_read_set, save_write_set);

  if (unlikely(thd->is_error()))
    DBUG_RETURN(HA_POS_ERROR);

  DBUG_PRINT("test",("error: %d  indexpos: %d",error,indexpos));
  if (unlikely(error != HA_ERR_END_OF_FILE))
  {
    file->print_error(error,MYF(ME_ERROR_LOG));
    DBUG_RETURN(HA_POS_ERROR);
  }
  if (indexpos && idx &&
      write_keys(param, fs_info, idx, buffpek_pointers, tempfile))
    DBUG_RETURN(HA_POS_ERROR);			/* purecov: inspected */

  (*found_rows)= num_records;
  if (pq)
    num_records= pq->num_elements();


  DBUG_PRINT("info", ("find_all_keys return %llu", (ulonglong) num_records));
  DBUG_RETURN(num_records);

err:
  sort_form->column_bitmaps_set(save_read_set, save_write_set);
  DBUG_RETURN(HA_POS_ERROR);
} /* find_all_keys */


/**
  @details
  Sort the buffer and write:
  -# the sorted sequence to tempfile
  -# a BUFFPEK describing the sorted sequence position to buffpek_pointers

    (was: Skriver en buffert med nycklar till filen)

  @param param             Sort parameters
  @param sort_keys         Array of pointers to keys to sort
  @param count             Number of elements in sort_keys array
  @param buffpek_pointers  One 'BUFFPEK' struct will be written into this file.
                           The BUFFPEK::{file_pos, count} will indicate where
                           the sorted data was stored.
  @param tempfile          The sorted sequence will be written into this file.

  @retval
    0 OK
  @retval
    1 Error
*/

static bool
write_keys(Sort_param *param,  SORT_INFO *fs_info, uint count,
           IO_CACHE *buffpek_pointers, IO_CACHE *tempfile)
{
  Merge_chunk buffpek;
  DBUG_ENTER("write_keys");

  fs_info->sort_buffer(param, count);

  if (!my_b_inited(tempfile) &&
      open_cached_file(tempfile, mysql_tmpdir, TEMP_PREFIX, DISK_BUFFER_SIZE,
                       MYF(MY_WME)))
    DBUG_RETURN(1);                                /* purecov: inspected */
  /* check we won't have more buffpeks than we can possibly keep in memory */
  if (my_b_tell(buffpek_pointers) + sizeof(Merge_chunk) > (ulonglong)UINT_MAX)
    DBUG_RETURN(1);

  buffpek.set_file_position(my_b_tell(tempfile));
  if ((ha_rows) count > param->max_rows)
    count=(uint) param->max_rows;               /* purecov: inspected */
  buffpek.set_rowcount(static_cast<ha_rows>(count));

  for (uint ix= 0; ix < count; ++ix)
  {
    uchar *record= fs_info->get_sorted_record(ix);


    if (my_b_write(tempfile, record, param->get_record_length(record)))
      DBUG_RETURN(1);                           /* purecov: inspected */
  }

  if (my_b_write(buffpek_pointers, (uchar*) &buffpek, sizeof(buffpek)))
    DBUG_RETURN(1);

  DBUG_RETURN(0);

} /* write_keys */


/**
  Store length in high-byte-first order.
*/
void store_length(uchar *to, uint length, uint pack_length)
{
  switch (pack_length) {
  case 1:
    *to= (uchar) length;
    break;
  case 2:
    mi_int2store(to, length);
    break;
  case 3:
    mi_int3store(to, length);
    break;
  default:
    mi_int4store(to, length);
    break;
  }
}


void
Type_handler_string_result::make_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  CHARSET_INFO *cs= item->collation.collation;
  bool maybe_null= item->maybe_null();

  if (maybe_null)
    *to++= 1;

  String *res= item->str_result(&param->tmp_buffer);
  if (!res)
  {
    if (maybe_null)
      memset(to - 1, 0, sort_field->length + 1);
    else
    {
      /* purecov: begin deadcode */
      /*
        This should only happen during extreme conditions if we run out
        of memory or have an item marked not null when it can be null.
        This code is here mainly to avoid a hard crash in this case.
      */
      DBUG_ASSERT(0);
      DBUG_PRINT("warning",
                 ("Got null on something that shouldn't be null"));
      memset(to, 0, sort_field->length);	// Avoid crash
      /* purecov: end */
    }
    return;
  }

  if (use_strnxfrm(cs))
  {
#ifdef DBUG_ASSERT_EXISTS
    size_t tmp_length=
#endif
    cs->strnxfrm(to, sort_field->length,
                 item->max_char_length() * cs->strxfrm_multiply,
                 (uchar*) res->ptr(), res->length(),
                 MY_STRXFRM_PAD_WITH_SPACE |
                 MY_STRXFRM_PAD_TO_MAXLEN);
    DBUG_ASSERT(tmp_length == sort_field->length);
  }
  else
  {
    uint diff;
    uint sort_field_length= sort_field->length - sort_field->suffix_length;
    uint length= res->length();
    if (sort_field_length < length)
    {
      diff= 0;
      length= sort_field_length;
    }
    else
      diff= sort_field_length - length;
    if (sort_field->suffix_length)
    {
      /* Store length last in result_string */
      store_length(to + sort_field_length, length, sort_field->suffix_length);
    }
    /* apply cs->sort_order for case-insensitive comparison if needed */
    cs->strnxfrm((uchar*)to, length, (const uchar*) res->ptr(), length);
    char fill_char= ((cs->state & MY_CS_BINSORT) ? (char) 0 : ' ');
    cs->fill((char *) to + length, diff, fill_char);
  }
}


void
Type_handler_int_result::make_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  longlong value= item->val_int_result();
  make_sort_key_longlong(to, item->maybe_null(), item->null_value,
                         item->unsigned_flag, value);
}


void
Type_handler_temporal_result::make_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  MYSQL_TIME buf;
  // This is a temporal type. No nanoseconds. Rounding mode is not important.
  DBUG_ASSERT(item->cmp_type() == TIME_RESULT);
  static const Temporal::Options opt(TIME_INVALID_DATES, TIME_FRAC_NONE);
  if (item->get_date_result(current_thd, &buf, opt))
  {
    DBUG_ASSERT(item->maybe_null());
    DBUG_ASSERT(item->null_value);
    make_sort_key_longlong(to, item->maybe_null(), true,
                           item->unsigned_flag, 0);
  }
  else
    make_sort_key_longlong(to, item->maybe_null(), false,
                           item->unsigned_flag, pack_time(&buf));
}


void
Type_handler_timestamp_common::make_sort_key_part(uchar *to, Item *item,
                                             const SORT_FIELD_ATTR *sort_field,
                                             Sort_param *param) const
{
  THD *thd= current_thd;
  uint binlen= my_timestamp_binary_length(item->decimals);
  Timestamp_or_zero_datetime_native_null native(thd, item);
  if (native.is_null() || native.is_zero_datetime())
  {
    // NULL or '0000-00-00 00:00:00'
    bzero(to, item->maybe_null() ? binlen + 1 : binlen);
  }
  else
  {
    if (item->maybe_null())
      *to++= 1;
    if (native.length() != binlen)
    {
      /*
        Some items can return native representation with a different
        number of fractional digits, e.g.: GREATEST(ts_3, ts_4) can
        return a value with 3 fractional digits, although its fractional
        precision is 4. Re-pack with a proper precision now.
      */
      Timestamp(native).to_native(&native, item->datetime_precision(thd));
    }
    DBUG_ASSERT(native.length() == binlen);
    memcpy((char *) to, native.ptr(), binlen);
  }
}


void
Type_handler::store_sort_key_longlong(uchar *to, bool unsigned_flag,
                                      longlong value) const
{
  to[7]= (uchar) value;
  to[6]= (uchar) (value >> 8);
  to[5]= (uchar) (value >> 16);
  to[4]= (uchar) (value >> 24);
  to[3]= (uchar) (value >> 32);
  to[2]= (uchar) (value >> 40);
  to[1]= (uchar) (value >> 48);
  if (unsigned_flag)                    /* Fix sign */
    to[0]= (uchar) (value >> 56);
  else
    to[0]= (uchar) (value >> 56) ^ 128; /* Reverse signbit */
}


void
Type_handler::make_sort_key_longlong(uchar *to,
                                     bool maybe_null,
                                     bool null_value,
                                     bool unsigned_flag,
                                     longlong value) const

{
  if (maybe_null)
  {
    if (null_value)
    {
      memset(to, 0, 9);
      return;
    }
    *to++= 1;
  }
  store_sort_key_longlong(to, unsigned_flag, value);
}


uint
Type_handler::make_packed_sort_key_longlong(uchar *to, bool maybe_null,
                                      bool null_value, bool unsigned_flag,
                                      longlong value,
                                      const SORT_FIELD_ATTR *sort_field) const
{
  if (maybe_null)
  {
    if (null_value)
    {
      *to++= 0;
      return 0;
    }
    *to++= 1;
  }
  store_sort_key_longlong(to, unsigned_flag, value);
  DBUG_ASSERT(sort_field->original_length == sort_field->length);
  return sort_field->original_length;
}


void
Type_handler_decimal_result::make_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  my_decimal dec_buf, *dec_val= item->val_decimal_result(&dec_buf);
  if (item->maybe_null())
  {
    if (item->null_value)
    {
      memset(to, 0, sort_field->length + 1);
      return;
    }
    *to++= 1;
  }
  dec_val->to_binary(to, item->max_length - (item->decimals ? 1 : 0),
                     item->decimals);
}


void
Type_handler_real_result::make_sort_key_part(uchar *to, Item *item,
                                             const SORT_FIELD_ATTR *sort_field,
                                             Sort_param *param) const
{
  double value= item->val_result();
  if (item->maybe_null())
  {
    if (item->null_value)
    {
      memset(to, 0, sort_field->length + 1);
      return;
    }
    *to++= 1;
  }
  change_double_for_sort(value, to);
}


/** Make a sort-key from record. */

static uint make_sortkey(Sort_param *param, uchar *to, uchar *ref_pos,
                         bool using_packed_sortkeys)
{
  uchar *orig_to= to;

  to+= using_packed_sortkeys ?
       make_packed_sortkey(param, to) :
       make_sortkey(param, to);

  if (param->using_addon_fields())
  {
    /* 
      Save field values appended to sorted fields.
      First null bit indicators are appended then field values follow.
      In this implementation we use fixed layout for field values -
      the same for all records.
    */
    SORT_ADDON_FIELD *addonf= param->addon_fields->begin();
    uchar *nulls= to;
    uchar *p_len= to;
    DBUG_ASSERT(addonf != 0);
    const bool packed_addon_fields= param->addon_fields->using_packed_addons();
    uint32 res_len= addonf->offset;
    memset(nulls, 0, addonf->offset);
    to+= addonf->offset;
    for ( ; addonf != param->addon_fields->end() ; addonf++)
    {
      Field *field= addonf->field;
      if (addonf->null_bit && field->is_null())
      {
        nulls[addonf->null_offset]|= addonf->null_bit;
        if (!packed_addon_fields)
          to+= addonf->length;
      }
      else
      {
        uchar *end= field->pack(to, field->ptr);
        DBUG_ASSERT(end >= to);
        uint sz= static_cast<uint>(end - to);
        res_len += sz;
        if (packed_addon_fields)
          to+= sz;
        else
        {
          if (addonf->length > sz)
            bzero(end, addonf->length - sz); // Make Valgrind/MSAN happy
          to+= addonf->length;
        }
      }
    }
    if (packed_addon_fields)
      Addon_fields::store_addon_length(p_len, res_len);
  }
  else
  {
    /* Save filepos last */
    memcpy((uchar*) to, ref_pos, (size_t) param->ref_length);
    to+= param->ref_length;
  }
  return static_cast<uint>(to - orig_to);
}


/*
  Register fields used by sorting in the sorted table's read set
*/

static void register_used_fields(Sort_param *param)
{
  SORT_FIELD *sort_field;
  TABLE *table=param->sort_form;

  for (sort_field= param->local_sortorder.begin() ;
       sort_field != param->local_sortorder.end() ;
       sort_field++)
  {
    Field *field;
    if ((field= sort_field->field))
    {
      if (field->table == table)
        field->register_field_in_read_map();
    }
    else
    {						// Item
      sort_field->item->walk(&Item::register_field_in_read_map, 1, table);
    }
  }

  if (param->using_addon_fields())
  {
    SORT_ADDON_FIELD *addonf= param->addon_fields->begin();
    for ( ; (addonf != param->addon_fields->end()) ; addonf++)
    {
      Field *field= addonf->field;
      field->register_field_in_read_map();
    }
  }
  else
  {
    /* Save filepos last */
    table->prepare_for_position();
  }
}


static bool save_index(Sort_param *param, uint count,
                       SORT_INFO *table_sort)
{
  uint offset,res_length, length;
  uchar *to;
  DBUG_ENTER("save_index");
  DBUG_ASSERT(table_sort->record_pointers == 0);

  table_sort->sort_buffer(param, count);

  if (param->using_addon_fields())
  {
    table_sort->sorted_result_in_fsbuf= TRUE;
    table_sort->set_sort_length(param->sort_length);
    DBUG_RETURN(0);
  }

  bool using_packed_sortkeys= param->using_packed_sortkeys();
  res_length= param->res_length;
  offset= param->rec_length-res_length;
  if (!(to= table_sort->record_pointers= 
        (uchar*) my_malloc(key_memory_Filesort_info_record_pointers,
                           res_length*count, MYF(MY_WME | MY_THREAD_SPECIFIC))))
    DBUG_RETURN(1);                 /* purecov: inspected */
  for (uint ix= 0; ix < count; ++ix)
  {
    uchar *record= table_sort->get_sorted_record(ix);

    length= using_packed_sortkeys ?
            Sort_keys::read_sortkey_length(record) : offset;

    memcpy(to, record + length, res_length);
    to+= res_length;
  }
  DBUG_RETURN(0);
}


/**
  Test whether priority queue is worth using to get top elements of an
  ordered result set. If it is, then allocates buffer for required amount of
  records

  @param param            Sort parameters.
  @param filesort_info    Filesort information.
  @param table            Table to sort.
  @param num_rows         Estimate of number of rows in source record set.
  @param memory_available Memory available for sorting.

  DESCRIPTION
    Given a query like this:
      SELECT ... FROM t ORDER BY a1,...,an LIMIT max_rows;
    This function tests whether a priority queue should be used to keep
    the result. Necessary conditions are:
    - estimate that it is actually cheaper than merge-sort
    - enough memory to store the <max_rows> records.

    If we don't have space for <max_rows> records, but we *do* have
    space for <max_rows> keys, we may rewrite 'table' to sort with
    references to records instead of additional data.
    (again, based on estimates that it will actually be cheaper).

   @retval
    true  - if it's ok to use PQ
    false - PQ will be slower than merge-sort, or there is not enough memory.
*/

static bool check_if_pq_applicable(Sort_param *param,
                            SORT_INFO *filesort_info,
                            TABLE *table, ha_rows num_rows,
                            size_t memory_available)
{
  DBUG_ENTER("check_if_pq_applicable");

  /*
    How much Priority Queue sort is slower than qsort.
    Measurements (see unit test) indicate that PQ is roughly 3 times slower.
  */
  const double PQ_slowness= 3.0;

  if (param->max_rows == HA_POS_ERROR)
  {
    DBUG_PRINT("info", ("No LIMIT"));
    DBUG_RETURN(false);
  }

  if (param->max_rows + 2 >= UINT_MAX)
  {
    DBUG_PRINT("info", ("Too large LIMIT"));
    DBUG_RETURN(false);
  }

  size_t num_available_keys=
    memory_available / (param->rec_length + sizeof(char*));
  // We need 1 extra record in the buffer, when using PQ.
  param->max_keys_per_buffer= (uint) param->max_rows + 1;

  if (num_rows < num_available_keys)
  {
    // The whole source set fits into memory.
    if (param->max_rows < num_rows/PQ_slowness )
    {
      filesort_info->alloc_sort_buffer(param->max_keys_per_buffer,
                                       param->rec_length);
      DBUG_RETURN(filesort_info->sort_buffer_size() != 0);
    }
    else
    {
      // PQ will be slower.
      DBUG_RETURN(false);
    }
  }

  // Do we have space for LIMIT rows in memory?
  if (param->max_keys_per_buffer < num_available_keys)
  {
    filesort_info->alloc_sort_buffer(param->max_keys_per_buffer,
                                     param->rec_length);
    DBUG_RETURN(filesort_info->sort_buffer_size() != 0);
  }

  // Try to strip off addon fields.
  if (param->addon_fields)
  {
    const size_t row_length=
      param->sort_length + param->ref_length + sizeof(char*);
    num_available_keys= memory_available / row_length;

    // Can we fit all the keys in memory?
    if (param->max_keys_per_buffer < num_available_keys)
    {
      const double sort_merge_cost=
        get_merge_many_buffs_cost_fast(num_rows,
                                       num_available_keys,
                                       (uint)row_length);
      /*
        PQ has cost:
        (insert + qsort) * log(queue size) / TIME_FOR_COMPARE_ROWID +
        cost of file lookup afterwards.
        The lookup cost is a bit pessimistic: we take scan_time and assume
        that on average we find the row after scanning half of the file.
        A better estimate would be lookup cost, but note that we are doing
        random lookups here, rather than sequential scan.
      */
      const double pq_cpu_cost= 
        (PQ_slowness * num_rows + param->max_keys_per_buffer) *
        log((double) param->max_keys_per_buffer) / TIME_FOR_COMPARE_ROWID;
      const double pq_io_cost=
        param->max_rows * table->file->scan_time() / 2.0;
      const double pq_cost= pq_cpu_cost + pq_io_cost;

      if (sort_merge_cost < pq_cost)
        DBUG_RETURN(false);

      filesort_info->alloc_sort_buffer(param->max_keys_per_buffer,
                                       param->sort_length + param->ref_length);

      if (filesort_info->sort_buffer_size() > 0)
      {
        /* Make attached data to be references instead of fields. */
        my_free(filesort_info->addon_fields);
        filesort_info->addon_fields= NULL;
        param->addon_fields= NULL;

        param->res_length= param->ref_length;
        param->sort_length+= param->ref_length;
        param->rec_length= param->sort_length;

        DBUG_RETURN(true);
      }
    }
  }
  DBUG_RETURN(false);
}


/** Merge buffers to make < MERGEBUFF2 buffers. */

int merge_many_buff(Sort_param *param, Sort_buffer sort_buffer,
                    Merge_chunk *buffpek, uint *maxbuffer, IO_CACHE *t_file)
{
  uint i;
  IO_CACHE t_file2,*from_file,*to_file,*temp;
  Merge_chunk *lastbuff;
  DBUG_ENTER("merge_many_buff");

  if (*maxbuffer < MERGEBUFF2)
    DBUG_RETURN(0);				/* purecov: inspected */
  if (flush_io_cache(t_file) ||
      open_cached_file(&t_file2,mysql_tmpdir,TEMP_PREFIX,DISK_BUFFER_SIZE,
			MYF(MY_WME)))
    DBUG_RETURN(1);				/* purecov: inspected */

  from_file= t_file ; to_file= &t_file2;
  while (*maxbuffer >= MERGEBUFF2)
  {
    if (reinit_io_cache(from_file,READ_CACHE,0L,0,0))
      goto cleanup;
    if (reinit_io_cache(to_file,WRITE_CACHE,0L,0,0))
      goto cleanup;
    lastbuff=buffpek;
    for (i=0 ; i <= *maxbuffer-MERGEBUFF*3/2 ; i+=MERGEBUFF)
    {
      if (merge_buffers(param,from_file,to_file,sort_buffer, lastbuff++,
			buffpek+i,buffpek+i+MERGEBUFF-1,0))
      goto cleanup;
    }
    if (merge_buffers(param,from_file,to_file,sort_buffer, lastbuff++,
		      buffpek+i,buffpek+ *maxbuffer,0))
      break;					/* purecov: inspected */
    if (flush_io_cache(to_file))
      break;					/* purecov: inspected */
    temp=from_file; from_file=to_file; to_file=temp;
    *maxbuffer= (uint) (lastbuff-buffpek)-1;
  }
cleanup:
  close_cached_file(to_file);			// This holds old result
  if (to_file == t_file)
  {
    *t_file=t_file2;				// Copy result file
  }

  DBUG_RETURN(*maxbuffer >= MERGEBUFF2);	/* Return 1 if interrupted */
} /* merge_many_buff */


/**
  Read data to buffer.

  @retval  Number of bytes read
           (ulong)-1 if something goes wrong
*/

ulong read_to_buffer(IO_CACHE *fromfile, Merge_chunk *buffpek,
                     Sort_param *param, bool packed_format)
{
  ha_rows count;
  uint rec_length= param->rec_length;

  if ((count= MY_MIN(buffpek->max_keys(),buffpek->rowcount())))
  {
    size_t bytes_to_read;
    if (packed_format)
    {
      count= buffpek->rowcount();
      bytes_to_read= MY_MIN(buffpek->buffer_size(),
                            static_cast<size_t>(fromfile->end_of_file -
                                                 buffpek->file_position()));
    }
    else
      bytes_to_read= rec_length * static_cast<size_t>(count);

    if (unlikely(my_b_pread(fromfile, buffpek->buffer_start(),
                            bytes_to_read, buffpek->file_position())))
      return ((ulong) -1);

    size_t num_bytes_read;

    if (packed_format)
    {
      /*
        The last record read is most likely not complete here.
        We need to loop through all the records, reading the length fields,
        and then "chop off" the final incomplete record.
      */
      uchar *record= buffpek->buffer_start();
      uint ix= 0;
      uint size_of_addon_length= param->using_packed_addons()  ?
                                 Addon_fields::size_of_length_field : 0;

      uint size_of_sort_length= param->using_packed_sortkeys() ?
                                Sort_keys::size_of_length_field : 0;

      for (; ix < count; ++ix)
      {
        if (record + size_of_sort_length > buffpek->buffer_end())
          break;
        uint sort_length=  param->using_packed_sortkeys() ?
                           Sort_keys::read_sortkey_length(record) :
                           param->sort_length;

        DBUG_ASSERT(sort_length <= param->sort_length);

        if (record + sort_length + size_of_addon_length >
            buffpek->buffer_end())
          break;                                // Incomplete record.

        uchar *plen= record + sort_length;
        uint res_length= param->get_result_length(plen);
        if (plen + res_length > buffpek->buffer_end())
          break;                                // Incomplete record.
        DBUG_ASSERT(res_length > 0);
        DBUG_ASSERT(sort_length + res_length <= param->rec_length);
        record+= sort_length;
        record+= res_length;
      }
      DBUG_ASSERT(ix > 0);
      count= ix;
      num_bytes_read= record - buffpek->buffer_start();
      DBUG_PRINT("info", ("read %llu bytes of complete records",
                          static_cast<ulonglong>(bytes_to_read)));
    }
    else
      num_bytes_read= bytes_to_read;

    buffpek->init_current_key();
    buffpek->advance_file_position(num_bytes_read);			/* New filepos */
    buffpek->decrement_rowcount(count);
    buffpek->set_mem_count(count);
    return (ulong) num_bytes_read;
  }
  return 0;
} /* read_to_buffer */


/**
  Put all room used by freed buffer to use in adjacent buffer.

  Note, that we can't simply distribute memory evenly between all buffers,
  because new areas must not overlap with old ones.

  @param[in] queue      list of non-empty buffers, without freed buffer
  @param[in] reuse      empty buffer
  @param[in] key_length key length
*/

void reuse_freed_buff(QUEUE *queue, Merge_chunk *reuse, uint key_length)
{
  for (uint i= queue_first_element(queue);
       i <= queue_last_element(queue);
       i++)
  {
    Merge_chunk *bp= (Merge_chunk *) queue_element(queue, i);
    if (reuse->merge_freed_buff(bp))
      return;
  }
  DBUG_ASSERT(0);
}


/**
  Merge buffers to one buffer.

  @param param        Sort parameter
  @param from_file    File with source data (BUFFPEKs point to this file)
  @param to_file      File to write the sorted result data.
  @param sort_buffer  Buffer for data to store up to MERGEBUFF2 sort keys.
  @param lastbuff     OUT Store here BUFFPEK describing data written to to_file
  @param Fb           First element in source BUFFPEKs array
  @param Tb           Last element in source BUFFPEKs array
  @param flag         0 <=> write {sort_key, addon_fields} pairs as further
                            sorting will be performed
                      1 <=> write just addon_fields as this is the final
                            merge pass

  @retval
    0      OK
  @retval
    1      ERROR
*/

bool merge_buffers(Sort_param *param, IO_CACHE *from_file,
                   IO_CACHE *to_file, Sort_buffer sort_buffer,
                   Merge_chunk *lastbuff, Merge_chunk *Fb, Merge_chunk *Tb,
                   int flag)
{
  bool error= 0;
  uint rec_length,res_length,offset;
  size_t sort_length;
  ulong maxcount, bytes_read;
  ha_rows max_rows,org_max_rows;
  my_off_t to_start_filepos;
  uchar *strpos;
  Merge_chunk *buffpek;
  QUEUE queue;
  qsort2_cmp cmp;
  void *first_cmp_arg;
  element_count dupl_count= 0;
  uchar *src;
  uchar *unique_buff= param->unique_buff;
  const bool killable= !param->not_killable;
  THD* const thd=current_thd;
  DBUG_ENTER("merge_buffers");

  thd->inc_status_sort_merge_passes();
  thd->query_plan_fsort_passes++;

  rec_length= param->rec_length;
  res_length= param->res_length;
  sort_length= param->sort_length;
  uint dupl_count_ofs= rec_length-sizeof(element_count);
  uint min_dupl_count= param->min_dupl_count;
  bool check_dupl_count= flag && min_dupl_count;
  offset= (rec_length-
           (flag && min_dupl_count ? sizeof(dupl_count) : 0)-res_length);
  uint wr_len= flag ? res_length : rec_length;
  uint wr_offset= flag ? offset : 0;

  const bool using_packed_sortkeys= param->using_packed_sortkeys();
  bool offset_for_packing= (flag == 1 && using_packed_sortkeys);
  const bool packed_format= param->is_packed_format();

  maxcount= (ulong) (param->max_keys_per_buffer/((uint) (Tb-Fb) +1));
  to_start_filepos= my_b_tell(to_file);
  strpos= sort_buffer.array();
  org_max_rows=max_rows= param->max_rows;
  
  set_if_bigger(maxcount, 1);
  
  if (unique_buff)
  {
    cmp= param->compare;
    first_cmp_arg= (void *) &param->cmp_context;
  }
  else
  {
    cmp= param->get_compare_function();
    first_cmp_arg= param->get_compare_argument(&sort_length);
  }
  if (unlikely(init_queue(&queue, (uint) (Tb-Fb)+1,
                         offsetof(Merge_chunk,m_current_key), 0,
                          (queue_compare) cmp, first_cmp_arg, 0, 0)))
    DBUG_RETURN(1);                                /* purecov: inspected */
  const size_t chunk_sz = (sort_buffer.size()/((uint) (Tb-Fb) +1));
  for (buffpek= Fb ; buffpek <= Tb ; buffpek++)
  {
    buffpek->set_buffer(strpos, strpos + chunk_sz);
    buffpek->set_max_keys(maxcount);
    bytes_read= read_to_buffer(from_file, buffpek, param, packed_format);
    if (unlikely(bytes_read == (ulong) -1))
      goto err;					/* purecov: inspected */
    strpos+= chunk_sz;
    // If less data in buffers than expected
    buffpek->set_max_keys(buffpek->mem_count());
    queue_insert(&queue, (uchar*) buffpek);
  }

  if (unique_buff)
  {
    /* 
       Called by Unique::get()
       Copy the first argument to unique_buff for unique removal.
       Store it also in 'to_file'.
    */
    buffpek= (Merge_chunk*) queue_top(&queue);
    memcpy(unique_buff, buffpek->current_key(), rec_length);
    if (min_dupl_count)
      memcpy(&dupl_count, unique_buff+dupl_count_ofs, 
             sizeof(dupl_count));
    buffpek->advance_current_key(rec_length);
    buffpek->decrement_mem_count();
    if (buffpek->mem_count() == 0)
    {
      if (unlikely(!(bytes_read= read_to_buffer(from_file, buffpek,
                                                param, packed_format))))
      {
        (void) queue_remove_top(&queue);
        reuse_freed_buff(&queue, buffpek, rec_length);
      }
      else if (unlikely(bytes_read == (ulong) -1))
        goto err;                        /* purecov: inspected */ 
    }
    queue_replace_top(&queue);            // Top element has been used
  }
  else
    cmp= 0;                                        // Not unique

  while (queue.elements > 1)
  {
    if (killable && unlikely(thd->check_killed()))
      goto err;                               /* purecov: inspected */

    for (;;)
    {
      buffpek= (Merge_chunk*) queue_top(&queue);
      src= buffpek->current_key();
      if (cmp)                                        // Remove duplicates
      {
        uchar *current_key= buffpek->current_key();
        if (!(*cmp)(first_cmp_arg, &unique_buff, &current_key))
        {
          if (min_dupl_count)
          {
            element_count cnt;
            memcpy(&cnt, buffpek->current_key() + dupl_count_ofs, sizeof(cnt));
            dupl_count+= cnt;
          }
          goto skip_duplicate;
        }
        if (min_dupl_count)
        {
          memcpy(unique_buff+dupl_count_ofs, &dupl_count,
                 sizeof(dupl_count));
        }
        src= unique_buff;
      }

      {
        param->get_rec_and_res_len(buffpek->current_key(),
                                   &rec_length, &res_length);
        const uint bytes_to_write= (flag == 0) ? rec_length : res_length;

        /*
          Do not write into the output file if this is the final merge called
          for a Unique object used for intersection and dupl_count is less
          than min_dupl_count.
          If the Unique object is used to intersect N sets of unique elements
          then for any element:
          dupl_count >= N <=> the element is occurred in each of these N sets.
        */
        if (!check_dupl_count || dupl_count >= min_dupl_count)
        {
          if(my_b_write(to_file,
                        src + (offset_for_packing ?
                               rec_length - res_length :  // sort length
                               wr_offset),
                        bytes_to_write))
            goto err;                           /* purecov: inspected */
        }
        if (cmp)
        {
          memcpy(unique_buff, buffpek->current_key(), rec_length);
          if (min_dupl_count)
            memcpy(&dupl_count, unique_buff+dupl_count_ofs,
                   sizeof(dupl_count));
        }
        if (!--max_rows)
        {
          /* Nothing more to do */
          goto end;                               /* purecov: inspected */
        }
      }
    skip_duplicate:
      buffpek->advance_current_key(rec_length);
      buffpek->decrement_mem_count();

      if (buffpek->mem_count() == 0)
      {
        if (unlikely(!(bytes_read= read_to_buffer(from_file, buffpek,
                                                  param, packed_format))))
        {
          (void) queue_remove_top(&queue);
          reuse_freed_buff(&queue, buffpek, rec_length);
          break;                        /* One buffer have been removed */
        }
        else if (unlikely(bytes_read == (ulong) -1))
          goto err;                        /* purecov: inspected */
      }
      queue_replace_top(&queue);   	/* Top element has been replaced */
    }
  }
  buffpek= (Merge_chunk*) queue_top(&queue);
  buffpek->set_buffer(sort_buffer.array(),
                      sort_buffer.array() + sort_buffer.size());
  buffpek->set_max_keys(param->max_keys_per_buffer);

  /*
    As we know all entries in the buffer are unique, we only have to
    check if the first one is the same as the last one we wrote
  */
  if (cmp)
  {
    uchar *current_key= buffpek->current_key();
    if (!(*cmp)(first_cmp_arg, &unique_buff, &current_key))
    {
      if (min_dupl_count)
      {
        element_count cnt;
        memcpy(&cnt, buffpek->current_key() + dupl_count_ofs, sizeof(cnt));
        dupl_count+= cnt;
      }
      buffpek->advance_current_key(rec_length);
      buffpek->decrement_mem_count();
    }

    if (min_dupl_count)
      memcpy(unique_buff+dupl_count_ofs, &dupl_count,
             sizeof(dupl_count));

    if (!check_dupl_count || dupl_count >= min_dupl_count)
    {
      src= unique_buff;
      if (my_b_write(to_file, src+wr_offset, wr_len))
        goto err;                             /* purecov: inspected */
      if (!--max_rows)
        goto end;                             
    }   
  }

  do
  {
    if (buffpek->mem_count() > max_rows)
    {                                        /* Don't write too many records */
      buffpek->set_mem_count(max_rows);
      buffpek->set_rowcount(0);                        /* Don't read more */
    }
    max_rows-= buffpek->mem_count();
    for (uint ix= 0; ix <  buffpek->mem_count(); ++ix)
    {
      uchar *src= buffpek->current_key();
      param->get_rec_and_res_len(src,
                                 &rec_length, &res_length);
      const uint bytes_to_write= (flag == 0) ? rec_length : res_length;
      if (check_dupl_count)
      {
        memcpy((uchar *) &dupl_count,
               buffpek->current_key() + offset + dupl_count_ofs,
               sizeof(dupl_count));
        if (dupl_count < min_dupl_count)
          continue;
      }
      if(my_b_write(to_file,
                    src + (offset_for_packing ?
                           rec_length - res_length :     // sort length
                           wr_offset),
                    bytes_to_write))
        goto err;
      buffpek->advance_current_key(rec_length);
    }
  }
  while (likely(!(error=
                  (bytes_read= read_to_buffer(from_file, buffpek, param,
                                           packed_format)) == (ulong) -1)) &&
         bytes_read != 0);

end:
  lastbuff->set_rowcount(MY_MIN(org_max_rows-max_rows, param->max_rows));
  lastbuff->set_file_position(to_start_filepos);

cleanup:
  delete_queue(&queue);
  DBUG_RETURN(error);

err:
  error= 1;
  goto cleanup;

} /* merge_buffers */


	/* Do a merge to output-file (save only positions) */

int merge_index(Sort_param *param, Sort_buffer sort_buffer,
                Merge_chunk *buffpek, uint maxbuffer,
                IO_CACHE *tempfile, IO_CACHE *outfile)
{
  DBUG_ENTER("merge_index");
  if (merge_buffers(param, tempfile, outfile, sort_buffer, buffpek, buffpek,
                    buffpek + maxbuffer, 1))
    DBUG_RETURN(1);				/* purecov: inspected */
  DBUG_RETURN(0);
} /* merge_index */


static uint suffix_length(ulong string_length)
{
  if (string_length < 256)
    return 1;
  if (string_length < 256L*256L)
    return 2;
  if (string_length < 256L*256L*256L)
    return 3;
  return 4;                                     // Can't sort longer than 4G
}


void
Type_handler_string_result::sort_length(THD *thd,
                                       const Type_std_attributes *item,
                                       SORT_FIELD_ATTR *sortorder) const
{
  CHARSET_INFO *cs;
  sortorder->set_length_and_original_length(thd, item->max_length);

  if (use_strnxfrm((cs= item->collation.collation)))
  {
    sortorder->length= (uint) cs->strnxfrmlen(sortorder->length);
  }
  else if (cs == &my_charset_bin)
  {
    /* Store length last to be able to sort blob/varbinary */
    sortorder->suffix_length= suffix_length(item->max_length);
    DBUG_ASSERT(sortorder->length <= UINT_MAX32 - sortorder->suffix_length);
    sortorder->length+= sortorder->suffix_length;
    if (sortorder->original_length >= UINT_MAX32 - sortorder->suffix_length)
      sortorder->original_length= UINT_MAX32;
    else
      sortorder->original_length+= sortorder->suffix_length;
  }
}


void
Type_handler_temporal_result::sort_length(THD *thd,
                                          const Type_std_attributes *item,
                                          SORT_FIELD_ATTR *sortorder) const
{
  sortorder->original_length= sortorder->length= 8; // Sizof intern longlong
}


void
Type_handler_timestamp_common::sort_length(THD *thd,
                                           const Type_std_attributes *item,
                                           SORT_FIELD_ATTR *sortorder) const
{
  sortorder->length= my_timestamp_binary_length(item->decimals);
  sortorder->original_length= sortorder->length;
}


void
Type_handler_int_result::sort_length(THD *thd,
                                        const Type_std_attributes *item,
                                        SORT_FIELD_ATTR *sortorder) const
{
  sortorder->original_length= sortorder->length= 8; // Sizof intern longlong
}


void
Type_handler_real_result::sort_length(THD *thd,
                                        const Type_std_attributes *item,
                                        SORT_FIELD_ATTR *sortorder) const
{
  sortorder->original_length= sortorder->length= sizeof(double);
}


void
Type_handler_decimal_result::sort_length(THD *thd,
                                         const Type_std_attributes *item,
                                         SORT_FIELD_ATTR *sortorder) const
{
  sortorder->length=
    my_decimal_get_binary_size(item->max_length - (item->decimals ? 1 : 0),
                               item->decimals);  
  sortorder->original_length= sortorder->length;
}


/**
  Calculate length of sort key.

  @param thd			  Thread handler
  @param sortorder		  Order of items to sort
  @param s_length	          Number of items to sort
  @param allow_packing_for_sortkeys [out]  set to false if packing sort keys is not
                                     allowed

  @note
   * sortorder->length and other members are updated for each sort item.
   * TODO what is the meaning of this value if some fields are using packing while
     others are not?

  @return
    Total length of sort buffer in bytes
*/

static uint
sortlength(THD *thd, Sort_keys *sort_keys, bool *allow_packing_for_sortkeys)
{
  uint length;
  *allow_packing_for_sortkeys= true;
  bool allow_packing_for_keys= true;

  length=0;
  uint nullable_cols=0;

  if (sort_keys->is_parameters_computed())
  {
    *allow_packing_for_sortkeys= sort_keys->using_packed_sortkeys();
    return sort_keys->get_sort_length_with_memcmp_values();
  }

  for (SORT_FIELD *sortorder= sort_keys->begin();
       sortorder != sort_keys->end();
       sortorder++)
  {
    sortorder->suffix_length= 0;
    sortorder->length_bytes= 0;
    if (sortorder->field)
    {
      Field *field= sortorder->field;
      CHARSET_INFO *cs= sortorder->field->sort_charset();
      sortorder->type= field->is_packable() ?
                       SORT_FIELD_ATTR::VARIABLE_SIZE :
                       SORT_FIELD_ATTR::FIXED_SIZE;
      sortorder->set_length_and_original_length(thd, field->sort_length());
      sortorder->suffix_length= sortorder->field->sort_suffix_length();
      sortorder->cs= cs;

      if (use_strnxfrm((cs=sortorder->field->sort_charset())))
        sortorder->length= (uint) cs->strnxfrmlen(sortorder->length);

      if (sortorder->is_variable_sized() && allow_packing_for_keys)
      {
        allow_packing_for_keys= sortorder->check_if_packing_possible(thd);
        sortorder->length_bytes=
          number_storage_requirement(MY_MIN(sortorder->original_length,
                                            thd->variables.max_sort_length));
      }

      if ((sortorder->maybe_null= sortorder->field->maybe_null()))
        nullable_cols++;				// Place for NULL marker
    }
    else
    {
      sortorder->type= sortorder->item->type_handler()->is_packable() ?
                       SORT_FIELD_ATTR::VARIABLE_SIZE :
                       SORT_FIELD_ATTR::FIXED_SIZE;
      sortorder->item->type_handler()->sort_length(thd, sortorder->item,
                                                   sortorder);
      sortorder->cs= sortorder->item->collation.collation;
      if (sortorder->is_variable_sized() && allow_packing_for_keys)
      {
        allow_packing_for_keys= sortorder->check_if_packing_possible(thd);
        sortorder->length_bytes=
          number_storage_requirement(MY_MIN(sortorder->original_length,
                                            thd->variables.max_sort_length));
      }

      if ((sortorder->maybe_null= sortorder->item->maybe_null()))
        nullable_cols++;				// Place for NULL marker
    }
    if (sortorder->is_variable_sized())
    {
      set_if_smaller(sortorder->length, thd->variables.max_sort_length);
      set_if_smaller(sortorder->original_length, thd->variables.max_sort_length);
    }
    length+=sortorder->length;

    sort_keys->increment_size_of_packable_fields(sortorder->length_bytes);
    sort_keys->increment_original_sort_length(sortorder->original_length);
  }
  // add bytes for nullable_cols
  sort_keys->increment_original_sort_length(nullable_cols);
  *allow_packing_for_sortkeys= allow_packing_for_keys;
  sort_keys->set_sort_length_with_memcmp_values(length + nullable_cols);
  sort_keys->set_parameters_computed(true);
  DBUG_PRINT("info",("sort_length: %d",length));
  return length + nullable_cols;
}


/*
  Check whether addon fields can be used or not.

  @param table                  Table structure
  @param sortlength             Length of sort key [strxfrm form]
  @param length [OUT]           Max length of addon fields
  @param fields [OUT]           Number of addon fields
  @param null_fields [OUT]      Number of nullable addon fields
  @param packable_length [OUT]  Max length of addon fields that can be
                                packed

  @retval
    TRUE     Addon fields can be used
    FALSE    Otherwise
*/

bool filesort_use_addons(TABLE *table, uint sortlength,
                         uint *length, uint *fields, uint *null_fields,
                         uint *packable_length)
{
  Field **pfield, *field;
  *length= *fields= *null_fields= *packable_length= 0;
  uint field_length=0;

  for (pfield= table->field; (field= *pfield) ; pfield++)
  {
    if (!bitmap_is_set(table->read_set, field->field_index))
      continue;
    if (field->flags & BLOB_FLAG)
      return false;
    field_length= field->max_packed_col_length(field->pack_length());
    (*length)+= field_length;

    if (field->maybe_null() || field->is_packable())
      (*packable_length)+= field_length;

    if (field->maybe_null())
      (*null_fields)++;
    (*fields)++;
  }
  if (!*fields)
    return false;
  (*length)+= (*null_fields+7)/8;

  /*
    sortlength used here is unpacked key length (the strxfrm form). This is
    done because unpacked key length is a good upper bound for packed sort
    key length.
    But for some collations the max packed length may be greater than the
    length obtained from the strxfrm form.
    Example: for utf8_general_ci, the original string form can be longer than
    its mem-comparable form (note that this is rarely achieved in practice).
  */
  return *length + sortlength <
         table->in_use->variables.max_length_for_sort_data;
}

/**
  Get descriptors of fields appended to sorted fields and
  calculate its total length.

  The function first finds out what fields are used in the result set.
  Then it calculates the length of the buffer to store the values of
  these fields together with the value of sort values.
  If the calculated length is not greater than max_length_for_sort_data
  the function allocates memory for an array of descriptors containing
  layouts for the values of the non-sorted fields in the buffer and
  fills them.

  @param table                     Table structure
  @param sortlength                Total length of sorted fields
  @param addon_length [OUT]        Length of addon fields
  @param m_packable_length [OUT]   Length of the addon fields that can be
                                   packed
  @note
    The null bits for the appended values are supposed to be put together
    and stored the buffer just ahead of the value of the first field.

  @return
    Pointer to the layout descriptors for the appended fields, if any
  @retval
    NULL   if we do not store field values with sort data.
*/

static Addon_fields*
get_addon_fields(TABLE *table, uint sortlength,
                 uint *addon_length, uint *m_packable_length)
{
  Field **pfield;
  Field *field;
  uint length, fields, null_fields, packable_length;
  MY_BITMAP *read_set= table->read_set;
  DBUG_ENTER("get_addon_fields");

  /*
    If there is a reference to a field in the query add it
    to the the set of appended fields.
    Note for future refinement:
    This this a too strong condition.
    Actually we need only the fields referred in the
    result set. And for some of them it makes sense to use
    the values directly from sorted fields.
    But beware the case when item->cmp_type() != item->result_type()
  */

  // see remove_const() for HA_SLOW_RND_POS explanation
  if (table->file->ha_table_flags() & HA_SLOW_RND_POS)
    sortlength= 0;

  void *raw_mem_addon_field, *raw_mem;

  if (!filesort_use_addons(table, sortlength, &length, &fields, &null_fields,
                            &packable_length) ||
       !(my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME | MY_THREAD_SPECIFIC),
                         &raw_mem, sizeof(Addon_fields),
                         &raw_mem_addon_field,
                         sizeof(SORT_ADDON_FIELD) * fields,
                         NullS)))
    DBUG_RETURN(0);

  Addon_fields_array
      addon_array(static_cast<SORT_ADDON_FIELD*>(raw_mem_addon_field), fields);
  Addon_fields *addon_fields= new (raw_mem) Addon_fields(addon_array);

  DBUG_ASSERT(addon_fields);

  (*addon_length)= length;
  (*m_packable_length)= packable_length;

  length= (null_fields+7)/8;
  null_fields= 0;
  SORT_ADDON_FIELD* addonf= addon_fields->begin();
  for (pfield= table->field; (field= *pfield) ; pfield++)
  {
    if (!bitmap_is_set(read_set, field->field_index))
      continue;
    addonf->field= field;
    addonf->offset= length;
    if (field->maybe_null())
    {
      addonf->null_offset= null_fields/8;
      addonf->null_bit= 1<<(null_fields & 7);
      null_fields++;
    }
    else
    {
      addonf->null_offset= 0;
      addonf->null_bit= 0;
    }
    addonf->length= field->max_packed_col_length(field->pack_length());
    length+= addonf->length;
    addonf++;
  }

  DBUG_PRINT("info",("addon_length: %d",length));
  DBUG_RETURN(addon_fields);
}


/*
** functions to change a double or float to a sortable string
** The following should work for IEEE
*/

#define DBL_EXP_DIG (sizeof(double)*8-DBL_MANT_DIG)

void change_double_for_sort(double nr,uchar *to)
{
  uchar *tmp=(uchar*) to;
  if (nr == 0.0)
  {						/* Change to zero string */
    tmp[0]=(uchar) 128;
    memset(tmp+1, 0, sizeof(nr)-1);
  }
  else
  {
#ifdef WORDS_BIGENDIAN
    memcpy(tmp, &nr, sizeof(nr));
#else
    {
      uchar *ptr= (uchar*) &nr;
#if defined(__FLOAT_WORD_ORDER) && (__FLOAT_WORD_ORDER == __BIG_ENDIAN)
      tmp[0]= ptr[3]; tmp[1]=ptr[2]; tmp[2]= ptr[1]; tmp[3]=ptr[0];
      tmp[4]= ptr[7]; tmp[5]=ptr[6]; tmp[6]= ptr[5]; tmp[7]=ptr[4];
#else
      tmp[0]= ptr[7]; tmp[1]=ptr[6]; tmp[2]= ptr[5]; tmp[3]=ptr[4];
      tmp[4]= ptr[3]; tmp[5]=ptr[2]; tmp[6]= ptr[1]; tmp[7]=ptr[0];
#endif
    }
#endif
    if (tmp[0] & 128)				/* Negative */
    {						/* make complement */
      uint i;
      for (i=0 ; i < sizeof(nr); i++)
	tmp[i]=tmp[i] ^ (uchar) 255;
    }
    else
    {					/* Set high and move exponent one up */
      ushort exp_part=(((ushort) tmp[0] << 8) | (ushort) tmp[1] |
		       (ushort) 32768);
      exp_part+= (ushort) 1 << (16-1-DBL_EXP_DIG);
      tmp[0]= (uchar) (exp_part >> 8);
      tmp[1]= (uchar) exp_part;
    }
  }
}

bool SORT_INFO::using_packed_addons()
{
  return addon_fields != NULL && addon_fields->using_packed_addons();
}

void SORT_INFO::free_addon_buff()
{
  if (addon_fields)
    addon_fields->free_addon_buff();
}

/*
  Check if packed sortkeys are used or not
*/
bool SORT_INFO::using_packed_sortkeys()
{
  return sort_keys != NULL && sort_keys->using_packed_sortkeys();
}

/**
   Free SORT_INFO
*/

SORT_INFO::~SORT_INFO()
{
  DBUG_ENTER("~SORT_INFO::SORT_INFO()");
  free_data();
  DBUG_VOID_RETURN;
}


void Sort_param::try_to_pack_sortkeys()
{
  #ifdef WITHOUT_PACKED_SORT_KEYS
    return;
  #endif

  uint size_of_packable_fields= sort_keys->get_size_of_packable_fields();

  /*
    Disable packing when all fields are fixed-size fields.
  */
  if (size_of_packable_fields == 0)
    return;

  const uint sz= Sort_keys::size_of_length_field;
  uint sort_len= sort_keys->get_sort_length_with_original_values();

  /*
    Heuristic introduced, skip packing sort keys if saving less than 128 bytes
  */

  if (sort_len < 128 + sz + size_of_packable_fields)
    return;

  sort_keys->set_using_packed_sortkeys(true);
  m_packed_format= true;
  m_using_packed_sortkeys= true;
  sort_length= sort_len + sz + size_of_packable_fields +
               (using_addon_fields() ?  0 : res_length);
  /* Only the record length needs to be updated, the res_length does not need
     to be updated
  */
  rec_length= sort_length + addon_length;
}


uint
Type_handler_string_result::make_packed_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  CHARSET_INFO *cs= item->collation.collation;
  bool maybe_null= item->maybe_null();

  if (maybe_null)
    *to++= 1;

  Binary_string *res= item->str_result(&param->tmp_buffer);
  if (!res)
  {
    if (maybe_null)
    {
      *(to-1)= 0;
      return 0;
    }
    else
    {
      /* purecov: begin deadcode */
      /*
        This should only happen during extreme conditions if we run out
        of memory or have an item marked not null when it can be null.
        This code is here mainly to avoid a hard crash in this case.
      */
      DBUG_ASSERT(0);
      DBUG_PRINT("warning",
                 ("Got null on something that shouldn't be null"));
      memset(to, 0, sort_field->length);  // Avoid crash
      /* purecov: end */
      return sort_field->original_length;
    }
  }
  return sort_field->pack_sort_string(to, res, cs);
}


uint
Type_handler_int_result::make_packed_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  longlong value= item->val_int_result();
  return make_packed_sort_key_longlong(to, item->maybe_null(),
                                       item->null_value, item->unsigned_flag,
                                       value, sort_field);
}


uint
Type_handler_decimal_result::make_packed_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  my_decimal dec_buf, *dec_val= item->val_decimal_result(&dec_buf);
  if (item->maybe_null())
  {
    if (item->null_value)
    {
      *to++=0;
      return 0;
    }
    *to++= 1;
  }
  dec_val->to_binary(to, item->max_length - (item->decimals ? 1 : 0),
                     item->decimals);
  DBUG_ASSERT(sort_field->original_length == sort_field->length);
  return sort_field->original_length;
}


uint
Type_handler_real_result::make_packed_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  double value= item->val_result();
  if (item->maybe_null())
  {
    if (item->null_value)
    {
      *to++=0;
      return 0;
    }
    *to++= 1;
  }
  change_double_for_sort(value, to);
  DBUG_ASSERT(sort_field->original_length == sort_field->length);
  return sort_field->original_length;
}


uint
Type_handler_temporal_result::make_packed_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  MYSQL_TIME buf;
  // This is a temporal type. No nanoseconds. Rounding mode is not important.
  DBUG_ASSERT(item->cmp_type() == TIME_RESULT);
  static const Temporal::Options opt(TIME_INVALID_DATES, TIME_FRAC_NONE);
  if (item->get_date_result(current_thd, &buf, opt))
  {
    DBUG_ASSERT(item->maybe_null());
    DBUG_ASSERT(item->null_value);
    return make_packed_sort_key_longlong(to, item->maybe_null(), true,
                                         item->unsigned_flag, 0, sort_field);
  }
  return make_packed_sort_key_longlong(to, item->maybe_null(), false,
                                       item->unsigned_flag, pack_time(&buf),
                                       sort_field);
}


uint
Type_handler_timestamp_common::make_packed_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
 THD *thd= current_thd;
  uint binlen= my_timestamp_binary_length(item->decimals);
  Timestamp_or_zero_datetime_native_null native(thd, item);
  if (native.is_null() || native.is_zero_datetime())
  {
    // NULL or '0000-00-00 00:00:00'
    if (item->maybe_null())
    {
      *to++=0;
      return 0;
    }
    else
    {
      bzero(to, binlen);
      return binlen;
    }
  }
  else
  {
    if (item->maybe_null())
      *to++= 1;
    if (native.length() != binlen)
    {
      /*
        Some items can return native representation with a different
        number of fractional digits, e.g.: GREATEST(ts_3, ts_4) can
        return a value with 3 fractional digits, although its fractional
        precision is 4. Re-pack with a proper precision now.
      */
      Timestamp(native).to_native(&native, item->datetime_precision(thd));
    }
    DBUG_ASSERT(native.length() == binlen);
    memcpy((char *) to, native.ptr(), binlen);
    return binlen;
  }
}


/*
  @brief
    Reverse the key for DESC clause
  @param to                   buffer where values are written
  @param maybe_null           nullability of a column
  @param sort_field           Sort field structure
   @details
     used for mem-comparable sort keys
*/

void reverse_key(uchar *to, const SORT_FIELD_ATTR *sort_field)
{
  uint length;
  if (sort_field->maybe_null && (to[-1]= !to[-1]))
  {
    to+= sort_field->length; // don't waste the time reversing all 0's
    return;
  }
  length=sort_field->length;
  while (length--)
  {
    *to = (uchar) (~ *to);
    to++;
  }
}


/*
  @brief
    Check if packing sort keys is allowed
  @param THD                 thread structure
  @retval
    TRUE  packing allowed
    FALSE packing not allowed
*/
bool SORT_FIELD_ATTR::check_if_packing_possible(THD *thd) const
{
  /*
    Packing not allowed when original length is greater than max_sort_length
    and we have a complex collation because cutting a prefix is not safe in
    such a case
  */
  if (original_length > thd->variables.max_sort_length &&
      cs->state & MY_CS_NON1TO1)
    return false;
  return true;
}


void SORT_FIELD_ATTR::set_length_and_original_length(THD *thd, uint length_arg)
{
  length= length_arg;
  if (is_variable_sized())
    set_if_smaller(length, thd->variables.max_sort_length);
  original_length= length_arg;
}


/*
  Compare function used for packing sort keys
*/

qsort2_cmp get_packed_keys_compare_ptr()
{
  return (qsort2_cmp) compare_packed_sort_keys;
}


/*
  Compare two varstrings.

  The strings are in this data format:

    [null_byte] [length of string + suffix_bytes] [the string] [suffix_bytes]

  suffix_bytes are used only for binary columns.
*/

int SORT_FIELD_ATTR::compare_packed_varstrings(uchar *a, size_t *a_len,
                                               uchar *b, size_t *b_len)
{
  int retval;
  size_t a_length, b_length;
  if (maybe_null)
  {
    *a_len= *b_len= 1; // NULL bytes are always stored
    if (*a != *b)
    {
      // Note we don't return a proper value in *{a|b}_len for the non-NULL
      // value but that's ok
      if (*a == 0)
        return -1;
      else
        return 1;
    }
    else
    {
      if (*a == 0)
        return 0;
    }
    a++;
    b++;
  }
  else
    *a_len= *b_len= 0;

  a_length= read_keypart_length(a, length_bytes);
  b_length= read_keypart_length(b, length_bytes);

  *a_len+= length_bytes + a_length;
  *b_len+= length_bytes + b_length;

  retval= cs->strnncollsp(a + length_bytes,
                          a_length - suffix_length,
                          b + length_bytes,
                          b_length - suffix_length);

  if (!retval && suffix_length)
  {
    DBUG_ASSERT(cs == &my_charset_bin);
    // comparing the length stored in suffix bytes for binary strings
    a= a + length_bytes + a_length - suffix_length;
    b= b + length_bytes + b_length - suffix_length;
    retval= memcmp(a, b, suffix_length);
  }

  return retval;
}


/*
  A value comparison function that has a signature that's suitable for
  comparing packed values, but actually compares fixed-size values with memcmp.

  This is used for ordering fixed-size columns when the sorting procedure used
  packed-value format.
*/

int SORT_FIELD_ATTR::compare_packed_fixed_size_vals(uchar *a, size_t *a_len,
                                                    uchar *b, size_t *b_len)
{
  if (maybe_null)
  {
    *a_len=1;
    *b_len=1;
    if (*a != *b)
    {
      if (*a == 0)
        return -1;
      else
        return 1;
    }
    else
    {
      if (*a == 0)
        return 0;
    }
    a++;
    b++;
  }
  else
    *a_len= *b_len= 0;

  *a_len+= length;
  *b_len+= length;
  return memcmp(a,b, length);
}


/*
  @brief
    Comparison function to compare two packed sort keys

  @param sort_param        cmp argument
  @param a_ptr             packed sort key
  @param b_ptr             packed sort key

  @retval
    >0   key a_ptr greater than b_ptr
    =0   key a_ptr equal to b_ptr
    <0   key a_ptr less than b_ptr

*/

int compare_packed_sort_keys(void *sort_param,
                             unsigned char **a_ptr, unsigned char **b_ptr)
{
  int retval= 0;
  size_t a_len, b_len;
  Sort_param *param= (Sort_param*)sort_param;
  Sort_keys *sort_keys= param->sort_keys;
  uchar *a= *a_ptr;
  uchar *b= *b_ptr;

  a+= Sort_keys::size_of_length_field;
  b+= Sort_keys::size_of_length_field;
  for (SORT_FIELD *sort_field= sort_keys->begin();
       sort_field != sort_keys->end(); sort_field++)
  {
    retval= sort_field->is_variable_sized() ?
            sort_field->compare_packed_varstrings(a, &a_len, b, &b_len) :
            sort_field->compare_packed_fixed_size_vals(a, &a_len, b, &b_len);

    if (retval)
      return sort_field->reverse ? -retval : retval;

    a+= a_len;
    b+= b_len;

  }
  /*
    this comparison is done for the case when the sort keys is appended with
    the ROW_ID pointer. For such cases we don't have addon fields
    so we can make a memcmp check over both the sort keys
  */
  if (!param->using_addon_fields())
    retval= memcmp(a, b, param->res_length);
  return retval;
}


/*
  @brief
    Store a packed string in the buffer

  @param to               buffer
  @param str              packed string value
  @param cs               character set

  @details
    This function writes to the buffer the packed value of a key_part
    of the sort key.

    The values written to the buffer are in this order
      - value for null byte
      - length of the string
      - value of the string
      - suffix length (for binary character set)
*/

uint
SORT_FIELD_ATTR::pack_sort_string(uchar *to, const Binary_string *str,
                                  CHARSET_INFO *cs) const
{
  uchar *orig_to= to;
  uint32 length, data_length;
  DBUG_ASSERT(str->length() <= UINT32_MAX);
  length= (uint32) str->length();

  if (length + suffix_length <= original_length)
    data_length= length;
  else
    data_length= original_length - suffix_length;

  // length stored in lowendian form
  store_key_part_length(data_length + suffix_length, to, length_bytes);
  to+= length_bytes;
  // copying data length bytes to the buffer
  memcpy(to, (uchar*)str->ptr(), data_length);
  to+= data_length;

  if (cs == &my_charset_bin && suffix_length)
  {
    // suffix length stored in bigendian form
    store_bigendian(length, to, suffix_length);
    to+= suffix_length;
  }
  return static_cast<uint>(to - orig_to);
}


/*
  @brief
    Create a mem-comparable sort key

  @param  param          sort param structure
  @param  to             buffer where values are written

  @retval
    length of the bytes written including the NULL bytes
*/

static uint make_sortkey(Sort_param *param, uchar *to)
{
  Field *field;
  SORT_FIELD *sort_field;
  uchar *orig_to= to;

  for (sort_field=param->local_sortorder.begin() ;
       sort_field != param->local_sortorder.end() ;
       sort_field++)
  {
    bool maybe_null=0;
    if ((field=sort_field->field))
    {
      // Field
      field->make_sort_key_part(to, sort_field->length);
      if ((maybe_null= field->maybe_null()))
        to++;
    }
    else
    {           // Item
      sort_field->item->type_handler()->make_sort_key_part(to,
                                                           sort_field->item,
                                                           sort_field, param);
      if ((maybe_null= sort_field->item->maybe_null()))
        to++;
    }

    if (sort_field->reverse)
      reverse_key(to, sort_field);
    to+= sort_field->length;
  }

  DBUG_ASSERT(static_cast<uint>(to - orig_to) <= param->sort_length);
  return static_cast<uint>(to - orig_to);
}


/*
  @brief
    create a compact sort key which can be compared with a comparison
    function. They are called packed sort keys

  @param  param          sort param structure
  @param  to             buffer where values are written

  @retval
    length of the bytes written including the NULL bytes
*/

static uint make_packed_sortkey(Sort_param *param, uchar *to)
{
  Field *field;
  SORT_FIELD *sort_field;
  uint length;
  uchar *orig_to= to;

  to+= Sort_keys::size_of_length_field;

  for (sort_field=param->local_sortorder.begin() ;
       sort_field != param->local_sortorder.end() ;
       sort_field++)
  {
    bool maybe_null=0;
    if ((field=sort_field->field))
    {
      // Field
      length= field->make_packed_sort_key_part(to, sort_field);
      if ((maybe_null= field->maybe_null()))
        to++;
    }
    else
    {           // Item
      Item *item= sort_field->item;
      length= item->type_handler()->make_packed_sort_key_part(to, item,
                                                              sort_field,
                                                              param);
      if ((maybe_null= sort_field->item->maybe_null()))
        to++;
    }
    to+= length;
  }

  length= static_cast<int>(to - orig_to);
  DBUG_ASSERT(length <= param->sort_length);
  Sort_keys::store_sortkey_length(orig_to, length);
  return length;
}
