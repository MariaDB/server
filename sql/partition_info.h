#ifndef PARTITION_INFO_INCLUDED
#define PARTITION_INFO_INCLUDED

/* Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "sql_class.h"
#include "partition_element.h"
#include "sql_partition.h"

class partition_info;
struct TABLE_LIST;
/* Some function typedefs */
typedef int (*get_part_id_func)(partition_info *part_info,
                                 uint32 *part_id,
                                 longlong *func_value);
typedef int (*get_subpart_id_func)(partition_info *part_info,
                                   uint32 *part_id);
 
struct st_ddl_log_memory_entry;

struct Vers_part_info : public Sql_alloc
{
  Vers_part_info() :
    interval(0),
    limit(0),
    now_part(NULL),
    hist_part(NULL),
    stat_serial(0)
  {
  }
  Vers_part_info(Vers_part_info &src) :
    interval(src.interval),
    limit(src.limit),
    now_part(NULL),
    hist_part(NULL),
    stat_serial(src.stat_serial)
  {
  }
  bool initialized(bool fully= true)
  {
    if (now_part)
    {
      DBUG_ASSERT(now_part->id != UINT_MAX32);
      DBUG_ASSERT(now_part->type() == partition_element::CURRENT);
      DBUG_ASSERT(!fully || (bool) hist_part);
      DBUG_ASSERT(!hist_part || (
          hist_part->id != UINT_MAX32 &&
          hist_part->type() == partition_element::HISTORY));
      return true;
    }
    return false;
  }
  my_time_t interval;
  ulonglong limit;
  partition_element *now_part;
  partition_element *hist_part;
  ulonglong stat_serial;
};

class partition_info : public Sql_alloc
{
public:
  /*
   * Here comes a set of definitions needed for partitioned table handlers.
   */
  List<partition_element> partitions;
  List<partition_element> temp_partitions;

  List<const char> part_field_list;
  List<const char> subpart_field_list;
  
  /* 
    If there is no subpartitioning, use only this func to get partition ids.
    If there is subpartitioning, use the this func to get partition id when
    you have both partition and subpartition fields.
  */
  get_part_id_func get_partition_id;

  /* Get partition id when we don't have subpartition fields */
  get_part_id_func get_part_partition_id;

  /* 
    Get subpartition id when we have don't have partition fields by we do
    have subpartition ids.
    Mikael said that for given constant tuple 
    {subpart_field1, ..., subpart_fieldN} the subpartition id will be the
    same in all subpartitions
  */
  get_subpart_id_func get_subpartition_id;

  /*
    When we have various string fields we might need some preparation
    before and clean-up after calling the get_part_id_func's. We need
    one such method for get_part_partition_id and one for
    get_subpartition_id.
  */
  get_part_id_func get_part_partition_id_charset;
  get_subpart_id_func get_subpartition_id_charset;

  /* NULL-terminated array of fields used in partitioned expression */
  Field **part_field_array;
  Field **subpart_field_array;
  Field **part_charset_field_array;
  Field **subpart_charset_field_array;
  /* 
    Array of all fields used in partition and subpartition expression,
    without duplicates, NULL-terminated.
  */
  Field **full_part_field_array;
  /*
    Set of all fields used in partition and subpartition expression.
    Required for testing of partition fields in write_set when
    updating. We need to set all bits in read_set because the row may
    need to be inserted in a different [sub]partition.
  */
  MY_BITMAP full_part_field_set;

  /*
    When we have a field that requires transformation before calling the
    partition functions we must allocate field buffers for the field of
    the fields in the partition function.
  */
  uchar **part_field_buffers;
  uchar **subpart_field_buffers;
  uchar **restore_part_field_ptrs;
  uchar **restore_subpart_field_ptrs;

  Item *part_expr;
  Item *subpart_expr;

  Item *item_free_list;

  struct st_ddl_log_memory_entry *first_log_entry;
  struct st_ddl_log_memory_entry *exec_log_entry;
  struct st_ddl_log_memory_entry *frm_log_entry;

  /* 
    Bitmaps of partitions used by the current query. 
    * read_partitions  - partitions to be used for reading.
    * lock_partitions  - partitions that must be locked (read or write).
    Usually read_partitions is the same set as lock_partitions, but
    in case of UPDATE the WHERE clause can limit the read_partitions set,
    but not neccesarily the lock_partitions set.
    Usage pattern:
    * Initialized in ha_partition::open().
    * read+lock_partitions is set  according to explicit PARTITION,
      WL#5217, in open_and_lock_tables().
    * Bits in read_partitions can be cleared in prune_partitions()
      in the optimizing step.
      (WL#4443 is about allowing prune_partitions() to affect lock_partitions
      and be done before locking too).
    * When the partition enabled handler get an external_lock call it locks
      all partitions in lock_partitions (and remembers which partitions it
      locked, so that it can unlock them later). In case of LOCK TABLES it will
      lock all partitions, and keep them locked while lock_partitions can
      change for each statement under LOCK TABLES.
    * Freed at the same time item_free_list is freed.
  */
  MY_BITMAP read_partitions;
  MY_BITMAP lock_partitions;
  bool bitmaps_are_initialized;

  union {
    longlong *range_int_array;
    LIST_PART_ENTRY *list_array;
    part_column_list_val *range_col_array;
    part_column_list_val *list_col_array;
  };

  Vers_part_info *vers_info;
  
  /********************************************
   * INTERVAL ANALYSIS
   ********************************************/
  /*
    Partitioning interval analysis function for partitioning, or NULL if 
    interval analysis is not supported for this kind of partitioning.
  */
  get_partitions_in_range_iter get_part_iter_for_interval;
  /*
    Partitioning interval analysis function for subpartitioning, or NULL if
    interval analysis is not supported for this kind of partitioning.
  */
  get_partitions_in_range_iter get_subpart_iter_for_interval;
  
  /********************************************
   * INTERVAL ANALYSIS ENDS 
   ********************************************/

  longlong err_value;
  char* part_info_string;

  partition_element *curr_part_elem;     // part or sub part
  partition_element *current_partition;  // partition
  part_elem_value *curr_list_val;
  uint curr_list_object;
  uint num_columns;

  TABLE *table;
  /*
    These key_map's are used for Partitioning to enable quick decisions
    on whether we can derive more information about which partition to
    scan just by looking at what index is used.
  */
  key_map all_fields_in_PF, all_fields_in_PPF, all_fields_in_SPF;
  key_map some_fields_in_PF;

  handlerton *default_engine_type;
  partition_type part_type;
  partition_type subpart_type;

  uint part_info_len;

  uint num_parts;
  uint num_subparts;
  uint count_curr_subparts;                  // used during parsing

  uint num_list_values;

  uint num_part_fields;
  uint num_subpart_fields;
  uint num_full_part_fields;

  uint has_null_part_id;
  uint32 default_partition_id;
  /*
    This variable is used to calculate the partition id when using
    LINEAR KEY/HASH. This functionality is kept in the MySQL Server
    but mainly of use to handlers supporting partitioning.
  */
  uint16 linear_hash_mask;
  /*
    PARTITION BY KEY ALGORITHM=N
    Which algorithm to use for hashing the fields.
    N = 1 - Use 5.1 hashing (numeric fields are hashed as binary)
    N = 2 - Use 5.5 hashing (numeric fields are hashed like latin1 bytes)
  */
  enum enum_key_algorithm
    {
      KEY_ALGORITHM_NONE= 0,
      KEY_ALGORITHM_51= 1,
      KEY_ALGORITHM_55= 2
    };
  enum_key_algorithm key_algorithm;

  /* Only the number of partitions defined (uses default names and options). */
  bool use_default_partitions;
  bool use_default_num_partitions;
  /* Only the number of subpartitions defined (uses default names etc.). */
  bool use_default_subpartitions;
  bool use_default_num_subpartitions;
  bool default_partitions_setup;
  bool defined_max_value;
  inline bool has_default_partititon()
  {
    return (part_type == LIST_PARTITION && defined_max_value);
  }
  bool list_of_part_fields;                  // KEY or COLUMNS PARTITIONING
  bool list_of_subpart_fields;               // KEY SUBPARTITIONING
  bool linear_hash_ind;                      // LINEAR HASH/KEY
  bool fixed;
  bool is_auto_partitioned;
  bool has_null_value;
  bool column_list;                          // COLUMNS PARTITIONING, 5.5+

  partition_info()
  : get_partition_id(NULL), get_part_partition_id(NULL),
    get_subpartition_id(NULL),
    part_field_array(NULL), subpart_field_array(NULL),
    part_charset_field_array(NULL),
    subpart_charset_field_array(NULL),
    full_part_field_array(NULL),
    part_field_buffers(NULL), subpart_field_buffers(NULL),
    restore_part_field_ptrs(NULL), restore_subpart_field_ptrs(NULL),
    part_expr(NULL), subpart_expr(NULL), item_free_list(NULL),
    first_log_entry(NULL), exec_log_entry(NULL), frm_log_entry(NULL),
    bitmaps_are_initialized(FALSE),
    list_array(NULL), vers_info(NULL), err_value(0),
    part_info_string(NULL),
    curr_part_elem(NULL), current_partition(NULL),
    curr_list_object(0), num_columns(0), table(NULL),
    default_engine_type(NULL),
    part_type(NOT_A_PARTITION), subpart_type(NOT_A_PARTITION),
    part_info_len(0),
    num_parts(0), num_subparts(0),
    count_curr_subparts(0),
    num_list_values(0), num_part_fields(0), num_subpart_fields(0),
    num_full_part_fields(0), has_null_part_id(0), linear_hash_mask(0),
    key_algorithm(KEY_ALGORITHM_NONE),
    use_default_partitions(TRUE), use_default_num_partitions(TRUE),
    use_default_subpartitions(TRUE), use_default_num_subpartitions(TRUE),
    default_partitions_setup(FALSE), defined_max_value(FALSE),
    list_of_part_fields(FALSE), list_of_subpart_fields(FALSE),
    linear_hash_ind(FALSE), fixed(FALSE),
    is_auto_partitioned(FALSE),
    has_null_value(FALSE), column_list(FALSE)
  {
    all_fields_in_PF.clear_all();
    all_fields_in_PPF.clear_all();
    all_fields_in_SPF.clear_all();
    some_fields_in_PF.clear_all();
    partitions.empty();
    temp_partitions.empty();
    part_field_list.empty();
    subpart_field_list.empty();
  }
  ~partition_info() {}

  partition_info *get_clone(THD *thd);
  bool set_named_partition_bitmap(const char *part_name, size_t length);
  bool set_partition_bitmaps(List<String> *partition_names);
  bool set_partition_bitmaps_from_table(TABLE_LIST *table_list);
  /* Answers the question if subpartitioning is used for a certain table */
  bool is_sub_partitioned()
  {
    return (subpart_type == NOT_A_PARTITION ?  FALSE : TRUE);
  }

  /* Returns the total number of partitions on the leaf level */
  uint get_tot_partitions()
  {
    return num_parts * (is_sub_partitioned() ? num_subparts : 1);
  }

  bool set_up_defaults_for_partitioning(THD *thd, handler *file,
                                        HA_CREATE_INFO *info,
                                        uint start_no);
  const char *find_duplicate_field();
  char *find_duplicate_name();
  bool check_engine_mix(handlerton *engine_type, bool default_engine);
  bool check_range_constants(THD *thd, bool alloc= true);
  bool check_list_constants(THD *thd);
  bool check_partition_info(THD *thd, handlerton **eng_type,
                            handler *file, HA_CREATE_INFO *info,
                            partition_info *add_or_reorg_part= NULL);
  void print_no_partition_found(TABLE *table, myf errflag);
  void print_debug(const char *str, uint*);
  Item* get_column_item(Item *item, Field *field);
  int fix_partition_values(THD *thd,
                           part_elem_value *val,
                           partition_element *part_elem);
  bool fix_column_value_functions(THD *thd,
                                  part_elem_value *val,
                                  uint part_id);
  bool fix_parser_data(THD *thd);
  int add_max_value(THD *thd);
  void init_col_val(part_column_list_val *col_val, Item *item);
  int reorganize_into_single_field_col_val(THD *thd);
  part_column_list_val *add_column_value(THD *thd);
  bool set_part_expr(THD *thd, char *start_token, Item *item_ptr,
                     char *end_token, bool is_subpart);
  static int compare_column_values(const void *a, const void *b);
  bool set_up_charset_field_preps(THD *thd);
  bool check_partition_field_length();
  bool init_column_part(THD *thd);
  bool add_column_list_value(THD *thd, Item *item);
  partition_element *get_part_elem(const char *partition_name, char *file_name,
                                   size_t file_name_size, uint32 *part_id);
  void report_part_expr_error(bool use_subpart_expr);
  bool has_same_partitioning(partition_info *new_part_info);
  bool error_if_requires_values() const;
private:
  static int list_part_cmp(const void* a, const void* b);
  bool set_up_default_partitions(THD *thd, handler *file, HA_CREATE_INFO *info,
                                 uint start_no);
  bool set_up_default_subpartitions(THD *thd, handler *file,
                                    HA_CREATE_INFO *info);
  char *create_default_partition_names(THD *thd, uint part_no, uint num_parts,
                                       uint start_no);
  char *create_default_subpartition_name(THD *thd, uint subpart_no,
                                         const char *part_name);
  // FIXME: prune_partition_bitmaps() is duplicate of set_read_partitions()
  bool prune_partition_bitmaps(List<String> *partition_names);
  bool add_named_partition(const char *part_name, size_t length);
public:
  bool set_read_partitions(List<char> *partition_names);
  bool has_unique_name(partition_element *element);

  bool vers_init_info(THD *thd);
  bool vers_set_interval(const INTERVAL &i);
  bool vers_set_limit(ulonglong limit);
  partition_element* vers_part_rotate(THD *thd);
  bool vers_set_expression(THD *thd, partition_element *el, MYSQL_TIME &t);
  bool vers_setup_expression(THD *thd, uint32 alter_add= 0); /* Stage 1. */
  bool vers_setup_stats(THD *thd, bool is_create_table_ind); /* Stage 2. */
  bool vers_scan_min_max(THD *thd, partition_element *part);
  void vers_update_col_vals(THD *thd, partition_element *el0, partition_element *el1);

  partition_element *vers_hist_part()
  {
    DBUG_ASSERT(table && table->s);
    DBUG_ASSERT(vers_info && vers_info->initialized());
    DBUG_ASSERT(table->s->hist_part_id != UINT_MAX32);
    if (table->s->hist_part_id == vers_info->hist_part->id)
      return vers_info->hist_part;

    List_iterator<partition_element> it(partitions);
    partition_element *el;
    while ((el= it++))
    {
      DBUG_ASSERT(el->type() != partition_element::CONVENTIONAL);
      if (el->type() == partition_element::HISTORY &&
        el->id == table->s->hist_part_id)
      {
        vers_info->hist_part= el;
        return vers_info->hist_part;
      }
    }
    DBUG_ASSERT(0);
    return NULL;
  }
  partition_element *get_partition(uint part_id)
  {
    List_iterator<partition_element> it(partitions);
    partition_element *el;
    while ((el= it++))
    {
      if (el->id == part_id)
        return el;
    }
    return NULL;
  }
  bool vers_limit_exceed(partition_element *part= NULL)
  {
    DBUG_ASSERT(vers_info);
    if (!vers_info->limit)
      return false;
    if (!part)
    {
      DBUG_ASSERT(vers_info->initialized());
      part= vers_hist_part();
    }
    // TODO: cache thread-shared part_recs and increment on INSERT
    return table->file->part_records(part) >= vers_info->limit;
  }
  Vers_min_max_stats& vers_stat_trx(stat_trx_field fld, uint32 part_element_id)
  {
    DBUG_ASSERT(table && table->s && table->s->stat_trx);
    Vers_min_max_stats* res= table->s->stat_trx[part_element_id * num_columns + fld];
    DBUG_ASSERT(res);
    return *res;
  }
  Vers_min_max_stats& vers_stat_trx(stat_trx_field fld, partition_element *part)
  {
    DBUG_ASSERT(part);
    return vers_stat_trx(fld, part->id);
  }
  bool vers_interval_exceed(my_time_t max_time, partition_element *part= NULL)
  {
    DBUG_ASSERT(vers_info);
    if (!vers_info->interval)
      return false;
    if (!part)
    {
      DBUG_ASSERT(vers_info->initialized());
      part= vers_hist_part();
    }
    my_time_t min_time= vers_stat_trx(STAT_TRX_END, part).min_time();
    return max_time - min_time > vers_info->interval;
  }
  bool vers_interval_exceed(partition_element *part)
  {
    return vers_interval_exceed(vers_stat_trx(STAT_TRX_END, part).max_time(), part);
  }
  bool vers_interval_exceed()
  {
    return vers_interval_exceed(vers_hist_part());
  }
  bool vers_trx_id_to_ts(THD *thd, Field *in_trx_id, Field_timestamp &out_ts);
  void vers_update_stats(THD *thd, partition_element *el)
  {
    DBUG_ASSERT(vers_info && vers_info->initialized());
    DBUG_ASSERT(table && table->s);
    DBUG_ASSERT(el && el->type() == partition_element::HISTORY);
    bool updated;
    mysql_rwlock_wrlock(&table->s->LOCK_stat_serial);
    el->empty= false;
    if (table->versioned(VERS_TRX_ID))
    {
      // transaction is not yet pushed to VTQ, so we use now-time
      my_time_t end_ts= my_time_t(0);

      uchar buf[8];
      Field_timestampf fld(buf, NULL, 0, Field::NONE, &table->vers_end_field()->field_name, NULL, 6);
      fld.store_TIME(end_ts, 0);
      updated=
        vers_stat_trx(STAT_TRX_END, el->id).update(&fld);
    }
    else
    {
      updated=
        vers_stat_trx(STAT_TRX_END, el->id).update(table->vers_end_field());
    }
    if (updated)
      table->s->stat_serial++;
    mysql_rwlock_unlock(&table->s->LOCK_stat_serial);
    if (updated)
    {
      vers_update_col_vals(thd,
        el->id > 0 ? get_partition(el->id - 1) : NULL,
        el);
    }
  }
  void vers_update_stats(THD *thd, uint part_id)
  {
    DBUG_ASSERT(vers_info && vers_info->initialized());
    uint lpart_id= num_subparts ? part_id / num_subparts : part_id;
    if (lpart_id < vers_info->now_part->id)
      vers_update_stats(thd, get_partition(lpart_id));
  }
  bool vers_update_range_constants(THD *thd)
  {
    DBUG_ASSERT(vers_info && vers_info->initialized());
    DBUG_ASSERT(table && table->s);

    mysql_rwlock_rdlock(&table->s->LOCK_stat_serial);
    if (vers_info->stat_serial == table->s->stat_serial)
    {
      mysql_rwlock_unlock(&table->s->LOCK_stat_serial);
      return false;
    }

    bool result= false;
    for (uint i= 0; i < num_columns; ++i)
    {
      Field *f= part_field_array[i];
      bitmap_set_bit(f->table->write_set, f->field_index);
    }
    MEM_ROOT *old_root= thd->mem_root;
    thd->mem_root= &table->mem_root;
    result= check_range_constants(thd, false);
    thd->mem_root= old_root;
    vers_info->stat_serial= table->s->stat_serial;
    mysql_rwlock_unlock(&table->s->LOCK_stat_serial);
    return result;
  }
};

uint32 get_next_partition_id_range(struct st_partition_iter* part_iter);
bool check_partition_dirs(partition_info *part_info);

/* Initialize the iterator to return a single partition with given part_id */

static inline void init_single_partition_iterator(uint32 part_id,
                                           PARTITION_ITERATOR *part_iter)
{
  part_iter->part_nums.start= part_iter->part_nums.cur= part_id;
  part_iter->part_nums.end= part_id+1;
  part_iter->ret_null_part= part_iter->ret_null_part_orig= FALSE;
  part_iter->ret_default_part= part_iter->ret_default_part_orig= FALSE;
  part_iter->get_next= get_next_partition_id_range;
}

/* Initialize the iterator to enumerate all partitions */
static inline
void init_all_partitions_iterator(partition_info *part_info,
                                  PARTITION_ITERATOR *part_iter)
{
  part_iter->part_nums.start= part_iter->part_nums.cur= 0;
  part_iter->part_nums.end= part_info->num_parts;
  part_iter->ret_null_part= part_iter->ret_null_part_orig= FALSE;
  part_iter->ret_default_part= part_iter->ret_default_part_orig= FALSE;
  part_iter->get_next= get_next_partition_id_range;
}

#endif /* PARTITION_INFO_INCLUDED */
