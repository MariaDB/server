#ifndef HA_PARTITION_INCLUDED
#define HA_PARTITION_INCLUDED

/*
   Copyright (c) 2005, 2012, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB Corporation.

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

#include "sql_partition.h"      /* part_id_range, partition_element */
#include "queues.h"             /* QUEUE */

struct Ordered_blob_storage
{
  String blob;
  bool set_read_value;
  Ordered_blob_storage() : set_read_value(false)
  {}
};

#define PAR_EXT ".par"
#define PARTITION_BYTES_IN_POS 2
#define ORDERED_PART_NUM_OFFSET sizeof(Ordered_blob_storage **)
#define ORDERED_REC_OFFSET (ORDERED_PART_NUM_OFFSET + PARTITION_BYTES_IN_POS)


/** Struct used for partition_name_hash */
typedef struct st_part_name_def
{
  uchar *partition_name;
  uint length;
  uint32 part_id;
  my_bool is_subpart;
} PART_NAME_DEF;

/** class where to save partitions Handler_share's */
class Parts_share_refs
{
public:
  uint num_parts;                              /**< Size of ha_share array */
  Handler_share **ha_shares;                   /**< Storage for each part */
  Parts_share_refs()
  {
    num_parts= 0;
    ha_shares= NULL;
  }
  ~Parts_share_refs()
  {
    uint i;
    for (i= 0; i < num_parts; i++)
      delete ha_shares[i];
    delete[] ha_shares;
  }
  bool init(uint arg_num_parts)
  {
    DBUG_ASSERT(!num_parts && !ha_shares);
    num_parts= arg_num_parts;
    /* Allocate an array of Handler_share pointers */
    ha_shares= new Handler_share *[num_parts];
    if (!ha_shares)
    {
      num_parts= 0;
      return true;
    }
    memset(ha_shares, 0, sizeof(Handler_share*) * num_parts);
    return false;
  }
};

class ha_partition;

/* Partition Full Text Search info */
struct st_partition_ft_info
{
  struct _ft_vft        *please;
  st_partition_ft_info  *next;
  ha_partition          *file;
  FT_INFO               **part_ft_info;
};


#ifdef HAVE_PSI_MUTEX_INTERFACE
extern PSI_mutex_key key_partition_auto_inc_mutex;
#endif

/**
  Partition specific Handler_share.
*/
class Partition_share : public Handler_share
{
public:
  bool auto_inc_initialized;
  mysql_mutex_t auto_inc_mutex;                /**< protecting auto_inc val */
  ulonglong next_auto_inc_val;                 /**< first non reserved value */
  /**
    Hash of partition names. Initialized in the first ha_partition::open()
    for the table_share. After that it is read-only, i.e. no locking required.
  */
  bool partition_name_hash_initialized;
  HASH partition_name_hash;
  const char *partition_engine_name;
  /** Storage for each partitions Handler_share */
  Parts_share_refs partitions_share_refs;
  Partition_share()
    : auto_inc_initialized(false),
    next_auto_inc_val(0),
    partition_name_hash_initialized(false),
    partition_engine_name(NULL),
    partition_names(NULL)
  {
    mysql_mutex_init(key_partition_auto_inc_mutex,
                    &auto_inc_mutex,
                    MY_MUTEX_INIT_FAST);
  }

  ~Partition_share()
  {
    mysql_mutex_destroy(&auto_inc_mutex);
    if (partition_names)
    {
      my_free(partition_names);
    }
    if (partition_name_hash_initialized)
    {
      my_hash_free(&partition_name_hash);
    }
  }
  
  bool init(uint num_parts);

  /**
    Release reserved auto increment values not used.
    @param thd             Thread.
    @param table_share     Table Share
    @param next_insert_id  Next insert id (first non used auto inc value).
    @param max_reserved    End of reserved auto inc range.
  */
  void release_auto_inc_if_possible(THD *thd, TABLE_SHARE *table_share,
                                    const ulonglong next_insert_id,
                                    const ulonglong max_reserved);

  /** lock mutex protecting auto increment value next_auto_inc_val. */
  inline void lock_auto_inc()
  {
    mysql_mutex_lock(&auto_inc_mutex);
  }
  /** unlock mutex protecting auto increment value next_auto_inc_val. */
  inline void unlock_auto_inc()
  {
    mysql_mutex_unlock(&auto_inc_mutex);
  }
  /**
    Populate partition_name_hash with partition and subpartition names
    from part_info.
    @param part_info  Partition info containing all partitions metadata.

    @return Operation status.
      @retval false Success.
      @retval true  Failure.
  */
  bool populate_partition_name_hash(partition_info *part_info);
  /** Get partition name.

  @param part_id  Partition id (for subpartitioned table only subpartition
                  names will be returned.)

  @return partition name or NULL if error.
  */
  const char *get_partition_name(size_t part_id) const;
private:
  const uchar **partition_names;
  /**
    Insert [sub]partition name into  partition_name_hash
    @param name        Partition name.
    @param part_id     Partition id.
    @param is_subpart  True if subpartition else partition.

    @return Operation status.
      @retval false Success.
      @retval true  Failure.
  */
  bool insert_partition_name_in_hash(const char *name,
                                     uint part_id,
                                     bool is_subpart);
};


/*
  List of ranges to be scanned by ha_partition's MRR implementation

  This object is
   - A KEY_MULTI_RANGE structure (the MRR range)
   - Storage for the range endpoints that the KEY_MULTI_RANGE has pointers to
   - list of such ranges (connected through the "next" pointer).
*/

typedef struct st_partition_key_multi_range
{
  /*
    Number of the range. The ranges are numbered in the order RANGE_SEQ_IF has
    emitted them, starting from 1. The numbering in used by ordered MRR scans.
  */
  uint id;
  uchar *key[2];
  /*
    Sizes of allocated memory in key[]. These may be larger then the actual
    values as this structure is reused across MRR scans
  */
  uint length[2];

  /*
    The range.
    key_multi_range.ptr is a pointer to the this PARTITION_KEY_MULTI_RANGE
    object
  */
  KEY_MULTI_RANGE key_multi_range;

  // Range id from the SQL layer
  range_id_t ptr;

  // The next element in the list of MRR ranges.
  st_partition_key_multi_range *next;
} PARTITION_KEY_MULTI_RANGE;


/*
  List of ranges to be scanned in a certain [sub]partition

  The idea is that there's a list of ranges to be scanned in the table
  (formed by PARTITION_KEY_MULTI_RANGE structures),
  and for each [sub]partition, we only need to scan a subset of that list.

     PKMR1 --> PKMR2 --> PKMR3 -->... // list of PARTITION_KEY_MULTI_RANGE
       ^                   ^
       |                   |
     PPKMR1 ----------> PPKMR2 -->... // list of PARTITION_PART_KEY_MULTI_RANGE

  This way, per-partition lists of PARTITION_PART_KEY_MULTI_RANGE have pointers
  to the elements of the global list of PARTITION_KEY_MULTI_RANGE.
*/

typedef struct st_partition_part_key_multi_range
{
  PARTITION_KEY_MULTI_RANGE *partition_key_multi_range;
  st_partition_part_key_multi_range *next;
} PARTITION_PART_KEY_MULTI_RANGE;


class ha_partition;

/*
  The structure holding information about range sequence to be used with one
  partition.
  (pointer to this is used as seq_init_param for RANGE_SEQ_IF structure when
   invoking MRR for an individual partition)
*/

typedef struct st_partition_part_key_multi_range_hld
{
  /* Owner object */
  ha_partition *partition;

  /* id of the the partition this structure is for */
  uint32 part_id;

  /* Current range we're iterating through */
  PARTITION_PART_KEY_MULTI_RANGE *partition_part_key_multi_range;
} PARTITION_PART_KEY_MULTI_RANGE_HLD;


extern "C" int cmp_key_part_id(void *key_p, uchar *ref1, uchar *ref2);
extern "C" int cmp_key_rowid_part_id(void *ptr, uchar *ref1, uchar *ref2);

class ha_partition final :public handler
{
private:
  enum partition_index_scan_type
  {
    partition_index_read= 0,
    partition_index_first= 1,
    partition_index_last= 3,
    partition_index_read_last= 4,
    partition_read_range = 5,
    partition_no_index_scan= 6,
    partition_read_multi_range = 7,
    partition_ft_read= 8
  };
  /* Data for the partition handler */
  int  m_mode;                          // Open mode
  uint m_open_test_lock;                // Open test_if_locked
  uchar *m_file_buffer;                 // Content of the .par file
  char *m_name_buffer_ptr;		// Pointer to first partition name
  MEM_ROOT m_mem_root;
  plugin_ref *m_engine_array;           // Array of types of the handlers
  handler **m_file;                     // Array of references to handler inst.
  uint m_file_tot_parts;                // Debug
  handler **m_new_file;                 // Array of references to new handlers
  handler **m_reorged_file;             // Reorganised partitions
  handler **m_added_file;               // Added parts kept for errors
  LEX_CSTRING *m_connect_string;
  partition_info *m_part_info;          // local reference to partition
  Field **m_part_field_array;           // Part field array locally to save acc
  uchar *m_ordered_rec_buffer;          // Row and key buffer for ord. idx scan
  st_partition_ft_info *ft_first;
  st_partition_ft_info *ft_current;
  /*
    Current index.
    When used in key_rec_cmp: If clustered pk, index compare
    must compare pk if given index is same for two rows.
    So normally m_curr_key_info[0]= current index and m_curr_key[1]= NULL,
    and if clustered pk, [0]= current index, [1]= pk, [2]= NULL
  */
  KEY *m_curr_key_info[3];              // Current index
  uchar *m_rec0;                        // table->record[0]
  const uchar *m_err_rec;               // record which gave error
  QUEUE m_queue;                        // Prio queue used by sorted read

  /*
    Length of an element in m_ordered_rec_buffer. The elements are composed of

      [part_no] [table->record copy] [underlying_table_rowid]

    underlying_table_rowid is only stored when the table has no extended keys.
  */
  size_t m_priority_queue_rec_len;

  /*
    If true, then sorting records by key value also sorts them by their
    underlying_table_rowid.
  */
  bool m_using_extended_keys;

  /*
    Since the partition handler is a handler on top of other handlers, it
    is necessary to keep information about what the underlying handler
    characteristics is. It is not possible to keep any handler instances
    for this since the MySQL Server sometimes allocating the handler object
    without freeing them.
  */
  enum enum_handler_status
  {
    handler_not_initialized= 0,
    handler_initialized,
    handler_opened,
    handler_closed
  };
  enum_handler_status m_handler_status;

  uint m_reorged_parts;                  // Number of reorganised parts
  uint m_tot_parts;                      // Total number of partitions;
  uint m_num_locks;                       // For engines like ha_blackhole, which needs no locks
  uint m_last_part;                      // Last file that we update,write,read
  part_id_range m_part_spec;             // Which parts to scan
  uint m_scan_value;                     // Value passed in rnd_init
                                         // call
  uint m_ref_length;                     // Length of position in this
                                         // handler object
  key_range m_start_key;                 // index read key range
  enum partition_index_scan_type m_index_scan_type;// What type of index
                                                   // scan
  uint m_top_entry;                      // Which partition is to
                                         // deliver next result
  uint m_rec_length;                     // Local copy of record length

  bool m_ordered;                        // Ordered/Unordered index scan
  bool m_create_handler;                 // Handler used to create table
  bool m_is_sub_partitioned;             // Is subpartitioned
  bool m_ordered_scan_ongoing;
  bool m_rnd_init_and_first;
  bool m_ft_init_and_first;

  /*
    If set, this object was created with ha_partition::clone and doesn't
    "own" the m_part_info structure.
  */
  ha_partition *m_is_clone_of;
  MEM_ROOT *m_clone_mem_root;

  /*
    We keep track if all underlying handlers are MyISAM since MyISAM has a
    great number of extra flags not needed by other handlers.
  */
  bool m_myisam;                         // Are all underlying handlers
                                         // MyISAM
  /*
    We keep track of InnoDB handlers below since it requires proper setting
    of query_id in fields at index_init and index_read calls.
  */
  bool m_innodb;                        // Are all underlying handlers
                                        // InnoDB
  bool m_myisammrg;                     // Are any of the handlers of type MERGE
  /*
    When calling extra(HA_EXTRA_CACHE) we do not pass this to the underlying
    handlers immediately. Instead we cache it and call the underlying
    immediately before starting the scan on the partition. This is to
    prevent allocating a READ CACHE for each partition in parallel when
    performing a full table scan on MyISAM partitioned table.
    This state is cleared by extra(HA_EXTRA_NO_CACHE).
  */
  bool m_extra_cache;
  uint m_extra_cache_size;
  /* The same goes for HA_EXTRA_PREPARE_FOR_UPDATE */
  bool m_extra_prepare_for_update;
  /* Which partition has active cache */
  uint m_extra_cache_part_id;

  void init_handler_variables();
  /*
    Variables for lock structures.
  */

  bool auto_increment_lock;             /**< lock reading/updating auto_inc */
  /**
    Flag to keep the auto_increment lock through out the statement.
    This to ensure it will work with statement based replication.
  */
  bool auto_increment_safe_stmt_log_lock;
  /** For optimizing ha_start_bulk_insert calls */
  MY_BITMAP m_bulk_insert_started;
  ha_rows   m_bulk_inserted_rows;
  /** used for prediction of start_bulk_insert rows */
  enum_monotonicity_info m_part_func_monotonicity_info;
  part_id_range m_direct_update_part_spec;
  bool                m_pre_calling;
  bool                m_pre_call_use_parallel;
  /* Keep track of bulk access requests */
  bool                bulk_access_executing;

  /** keep track of locked partitions */
  MY_BITMAP m_locked_partitions;
  /** Stores shared auto_increment etc. */
  Partition_share *part_share;
  void sum_copy_info(handler *file);
  void sum_copy_infos();
  void reset_copy_info() override;
  /** Temporary storage for new partitions Handler_shares during ALTER */
  List<Parts_share_refs> m_new_partitions_share_refs;
  /** Sorted array of partition ids in descending order of number of rows. */
  uint32 *m_part_ids_sorted_by_num_of_records;
  /* Compare function for my_qsort2, for reversed order. */
  static int compare_number_of_records(ha_partition *me,
                                       const uint32 *a,
                                       const uint32 *b);
  /** keep track of partitions to call ha_reset */
  MY_BITMAP m_partitions_to_reset;
  /** partitions that returned HA_ERR_KEY_NOT_FOUND. */
  MY_BITMAP m_key_not_found_partitions;
  bool m_key_not_found;
  List<String> *m_partitions_to_open;
  MY_BITMAP m_opened_partitions;
  /** This is one of the m_file-s that it guaranteed to be opened. */
  /**  It is set in open_read_partitions() */
  handler *m_file_sample;
public:
  handler **get_child_handlers()
  {
    return m_file;
  }
  ha_partition *get_clone_source()
  {
    return m_is_clone_of;
  }
  virtual part_id_range *get_part_spec()
  {
    return &m_part_spec;
  }
  virtual uint get_no_current_part_id()
  {
    return NO_CURRENT_PART_ID;
  }
  Partition_share *get_part_share() { return part_share; }
  handler *clone(const char *name, MEM_ROOT *mem_root) override;
  void set_part_info(partition_info *part_info) override
  {
     m_part_info= part_info;
     m_is_sub_partitioned= part_info->is_sub_partitioned();
  }

  void return_record_by_parent() override;

  bool vers_can_native(THD *thd) override
  {
    if (thd->lex->part_info)
    {
      // PARTITION BY SYSTEM_TIME is not supported for now
      return thd->lex->part_info->part_type != VERSIONING_PARTITION;
    }
    else
    {
      bool can= true;
      for (uint i= 0; i < m_tot_parts && can; i++)
        can= can && m_file[i]->vers_can_native(thd);
      return can;
    }
  }

  /*
    -------------------------------------------------------------------------
    MODULE create/delete handler object
    -------------------------------------------------------------------------
    Object create/delete method. Normally called when a table object
    exists. There is also a method to create the handler object with only
    partition information. This is used from mysql_create_table when the
    table is to be created and the engine type is deduced to be the
    partition handler.
    -------------------------------------------------------------------------
  */
    ha_partition(handlerton *hton, TABLE_SHARE * table);
    ha_partition(handlerton *hton, partition_info * part_info);
    ha_partition(handlerton *hton, TABLE_SHARE *share,
                 partition_info *part_info_arg,
                 ha_partition *clone_arg,
                 MEM_ROOT *clone_mem_root_arg);
   ~ha_partition();
   void ha_partition_init();
  /*
    A partition handler has no characteristics in itself. It only inherits
    those from the underlying handlers. Here we set-up those constants to
    enable later calls of the methods to retrieve constants from the under-
    lying handlers. Returns false if not successful.
  */
   bool initialize_partition(MEM_ROOT *mem_root);

  /*
    -------------------------------------------------------------------------
    MODULE meta data changes
    -------------------------------------------------------------------------
    Meta data routines to CREATE, DROP, RENAME table and often used at
    ALTER TABLE (update_create_info used from ALTER TABLE and SHOW ..).

    create_partitioning_metadata is called before opening a new handler object
    with openfrm to call create. It is used to create any local handler
    object needed in opening the object in openfrm
    -------------------------------------------------------------------------
  */
  int delete_table(const char *from) override;
  int rename_table(const char *from, const char *to) override;
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info) override;
  int create_partitioning_metadata(const char *name,
                                   const char *old_name,
                                   chf_create_flags action_flag)
    override;
  bool check_if_updates_are_ignored(const char *op) const override;
  void update_create_info(HA_CREATE_INFO *create_info) override;
  int change_partitions(HA_CREATE_INFO *create_info, const char *path,
                        ulonglong * const copied, ulonglong * const deleted,
                        const uchar *pack_frm_data, size_t pack_frm_len)
    override;
  int drop_partitions(const char *path) override;
  int rename_partitions(const char *path) override;
  bool get_no_parts(const char *, uint *num_parts) override
  {
    DBUG_ENTER("ha_partition::get_no_parts");
    *num_parts= m_tot_parts;
    DBUG_RETURN(0);
  }
  void change_table_ptr(TABLE *table_arg, TABLE_SHARE *share) override;
  bool check_if_incompatible_data(HA_CREATE_INFO *create_info,
                                  uint table_changes) override;
  void update_part_create_info(HA_CREATE_INFO *create_info, uint part_id)
  {
    m_file[part_id]->update_create_info(create_info);
  }
private:
  int copy_partitions(ulonglong * const copied, ulonglong * const deleted);
  void cleanup_new_partition(uint part_count);
  int prepare_new_partition(TABLE *table, HA_CREATE_INFO *create_info,
                            handler *file, const char *part_name,
                            partition_element *p_elem);
  /*
    delete_table and rename_table uses very similar logic which
    is packed into this routine.
  */
  uint del_ren_table(const char *from, const char *to);
  /*
    One method to create the table_name.par file containing the names of the
    underlying partitions, their engine and the number of partitions.
    And one method to read it in.
  */
  bool create_handler_file(const char *name);
  bool setup_engine_array(MEM_ROOT *mem_root, handlerton *first_engine);
  int read_par_file(const char *name);
  handlerton *get_def_part_engine(const char *name);
  bool get_from_handler_file(const char *name, MEM_ROOT *mem_root,
                             bool is_clone);
  bool re_create_par_file(const char *name);
  bool new_handlers_from_part_info(MEM_ROOT *mem_root);
  bool create_handlers(MEM_ROOT *mem_root);
  void clear_handler_file();
  int set_up_table_before_create(TABLE *table_arg,
                                 const char *partition_name_with_path,
                                 HA_CREATE_INFO *info,
                                 partition_element *p_elem);
  partition_element *find_partition_element(uint part_id);
  bool insert_partition_name_in_hash(const char *name, uint part_id,
                                     bool is_subpart);
  bool populate_partition_name_hash();
  Partition_share *get_share();
  bool set_ha_share_ref(Handler_share **ha_share) override;
  void fix_data_dir(char* path);
  bool init_partition_bitmaps();
  void free_partition_bitmaps();

public:

  /*
    -------------------------------------------------------------------------
    MODULE open/close object
    -------------------------------------------------------------------------
    Open and close handler object to ensure all underlying files and
    objects allocated and deallocated for query handling is handled
    properly.
    -------------------------------------------------------------------------

    A handler object is opened as part of its initialisation and before
    being used for normal queries (not before meta-data changes always.
    If the object was opened it will also be closed before being deleted.
  */
  int open(const char *name, int mode, uint test_if_locked) override;
  int close() override;

  /*
    -------------------------------------------------------------------------
    MODULE start/end statement
    -------------------------------------------------------------------------
    This module contains methods that are used to understand start/end of
    statements, transaction boundaries, and aid for proper concurrency
    control.
    The partition handler need not implement abort and commit since this
    will be handled by any underlying handlers implementing transactions.
    There is only one call to each handler type involved per transaction
    and these go directly to the handlers supporting transactions
    -------------------------------------------------------------------------
  */
  THR_LOCK_DATA **store_lock(THD * thd, THR_LOCK_DATA ** to,
                             enum thr_lock_type lock_type) override;
  int external_lock(THD * thd, int lock_type) override;
  LEX_CSTRING *engine_name() override { return hton_name(partition_ht()); }
  /*
    When table is locked a statement is started by calling start_stmt
    instead of external_lock
  */
  int start_stmt(THD * thd, thr_lock_type lock_type) override;
  /*
    Lock count is number of locked underlying handlers (I assume)
  */
  uint lock_count() const override;
  /*
    Call to unlock rows not to be updated in transaction
  */
  void unlock_row() override;
  /*
    Check if semi consistent read
  */
  bool was_semi_consistent_read() override;
  /*
    Call to hint about semi consistent read
  */
  void try_semi_consistent_read(bool) override;

  /*
    NOTE: due to performance and resource issues with many partitions,
    we only use the m_psi on the ha_partition handler, excluding all
    partitions m_psi.
  */
#ifdef HAVE_M_PSI_PER_PARTITION
  /*
    Bind the table/handler thread to track table i/o.
  */
  virtual void unbind_psi();
  virtual int rebind();
#endif
  int discover_check_version() override;
  /*
    -------------------------------------------------------------------------
    MODULE change record
    -------------------------------------------------------------------------
    This part of the handler interface is used to change the records
    after INSERT, DELETE, UPDATE, REPLACE method calls but also other
    special meta-data operations as ALTER TABLE, LOAD DATA, TRUNCATE.
    -------------------------------------------------------------------------

    These methods are used for insert (write_row), update (update_row)
    and delete (delete_row). All methods to change data always work on
    one row at a time. update_row and delete_row also contains the old
    row.
    delete_all_rows will delete all rows in the table in one call as a
    special optimisation for DELETE from table;

    Bulk inserts are supported if all underlying handlers support it.
    start_bulk_insert and end_bulk_insert is called before and after a
    number of calls to write_row.
  */
  int write_row(const uchar * buf) override;
  bool start_bulk_update() override;
  int exec_bulk_update(ha_rows *dup_key_found) override;
  int end_bulk_update() override;
  int bulk_update_row(const uchar *old_data, const uchar *new_data,
                      ha_rows *dup_key_found) override;
  int update_row(const uchar * old_data, const uchar * new_data) override;
  int direct_update_rows_init(List<Item> *update_fields) override;
  int pre_direct_update_rows_init(List<Item> *update_fields) override;
  int direct_update_rows(ha_rows *update_rows, ha_rows *found_rows) override;
  int pre_direct_update_rows() override;
  bool start_bulk_delete() override;
  int end_bulk_delete() override;
  int delete_row(const uchar * buf) override;
  int direct_delete_rows_init() override;
  int pre_direct_delete_rows_init() override;
  int direct_delete_rows(ha_rows *delete_rows) override;
  int pre_direct_delete_rows() override;
  int delete_all_rows() override;
  int truncate() override;
  void start_bulk_insert(ha_rows rows, uint flags) override;
  int end_bulk_insert() override;
private:
  ha_rows guess_bulk_insert_rows();
  void start_part_bulk_insert(THD *thd, uint part_id);
  long estimate_read_buffer_size(long original_size);
public:

  /*
    Method for truncating a specific partition.
    (i.e. ALTER TABLE t1 TRUNCATE PARTITION p).

    @remark This method is a partitioning-specific hook
            and thus not a member of the general SE API.
  */
  int truncate_partition(Alter_info *, bool *binlog_stmt);

  bool is_fatal_error(int error, uint flags) override
  {
    if (!handler::is_fatal_error(error, flags) ||
        error == HA_ERR_NO_PARTITION_FOUND ||
        error == HA_ERR_NOT_IN_LOCK_PARTITIONS)
      return FALSE;
    return TRUE;
  }


  /*
    -------------------------------------------------------------------------
    MODULE full table scan
    -------------------------------------------------------------------------
    This module is used for the most basic access method for any table
    handler. This is to fetch all data through a full table scan. No
    indexes are needed to implement this part.
    It contains one method to start the scan (rnd_init) that can also be
    called multiple times (typical in a nested loop join). Then proceeding
    to the next record (rnd_next) and closing the scan (rnd_end).
    To remember a record for later access there is a method (position)
    and there is a method used to retrieve the record based on the stored
    position.
    The position can be a file position, a primary key, a ROWID dependent
    on the handler below.
    -------------------------------------------------------------------------
  */
  /*
    unlike index_init(), rnd_init() can be called two times
    without rnd_end() in between (it only makes sense if scan=1).
    then the second call should prepare for the new table scan
    (e.g if rnd_init allocates the cursor, second call should
    position it to the start of the table, no need to deallocate
    and allocate it again
  */
  int rnd_init(bool scan) override;
  int rnd_end() override;
  int rnd_next(uchar * buf) override;
  int rnd_pos(uchar * buf, uchar * pos) override;
  int rnd_pos_by_record(uchar *record) override;
  void position(const uchar * record) override;

  /*
    -------------------------------------------------------------------------
    MODULE index scan
    -------------------------------------------------------------------------
    This part of the handler interface is used to perform access through
    indexes. The interface is defined as a scan interface but the handler
    can also use key lookup if the index is a unique index or a primary
    key index.
    Index scans are mostly useful for SELECT queries but are an important
    part also of UPDATE, DELETE, REPLACE and CREATE TABLE table AS SELECT
    and so forth.
    Naturally an index is needed for an index scan and indexes can either
    be ordered, hash based. Some ordered indexes can return data in order
    but not necessarily all of them.
    There are many flags that define the behavior of indexes in the
    various handlers. These methods are found in the optimizer module.
    -------------------------------------------------------------------------

    index_read is called to start a scan of an index. The find_flag defines
    the semantics of the scan. These flags are defined in
    include/my_base.h
    index_read_idx is the same but also initializes index before calling doing
    the same thing as index_read. Thus it is similar to index_init followed
    by index_read. This is also how we implement it.

    index_read/index_read_idx does also return the first row. Thus for
    key lookups, the index_read will be the only call to the handler in
    the index scan.

    index_init initializes an index before using it and index_end does
    any end processing needed.
  */
  int index_read_map(uchar * buf, const uchar * key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_init(uint idx, bool sorted) override;
  int index_end() override;

  /**
    @breif
    Positions an index cursor to the index specified in the handle. Fetches the
    row if available. If the key value is null, begin at first key of the
    index.
  */
  int index_read_idx_map(uchar *buf, uint index, const uchar *key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override;
  /*
    These methods are used to jump to next or previous entry in the index
    scan. There are also methods to jump to first and last entry.
  */
  int index_next(uchar * buf) override;
  int index_prev(uchar * buf) override;
  int index_first(uchar * buf) override;
  int index_last(uchar * buf) override;
  int index_next_same(uchar * buf, const uchar * key, uint keylen) override;

  int index_read_last_map(uchar *buf,
                          const uchar *key,
                          key_part_map keypart_map) override;

  /*
    read_first_row is virtual method but is only implemented by
    handler.cc, no storage engine has implemented it so neither
    will the partition handler.

    int read_first_row(uchar *buf, uint primary_key) override;
  */


  int read_range_first(const key_range * start_key,
                       const key_range * end_key,
                       bool eq_range, bool sorted) override;
  int read_range_next() override;


  HANDLER_BUFFER *m_mrr_buffer;
  uint *m_mrr_buffer_size;
  uchar *m_mrr_full_buffer;
  uint m_mrr_full_buffer_size;
  uint m_mrr_new_full_buffer_size;
  MY_BITMAP m_mrr_used_partitions;
  uint *m_stock_range_seq;
  /* not used: uint m_current_range_seq; */

  /* Value of mrr_mode passed to ha_partition::multi_range_read_init */
  uint m_mrr_mode;

  /* Value of n_ranges passed to ha_partition::multi_range_read_init */
  uint m_mrr_n_ranges;

  /*
    Ordered MRR mode:  m_range_info[N] has the range_id of the last record that
    we've got from partition N
  */
  range_id_t *m_range_info;

  /*
    TRUE <=> This ha_partition::multi_range_read_next() call is the first one
  */
  bool m_multi_range_read_first;

  /* not used: uint m_mrr_range_init_flags; */

  /* Number of elements in the list pointed by m_mrr_range_first. Not used */
  uint m_mrr_range_length;

  /* Linked list of ranges to scan */
  PARTITION_KEY_MULTI_RANGE *m_mrr_range_first;
  PARTITION_KEY_MULTI_RANGE *m_mrr_range_current;

  /*
    For each partition: number of ranges MRR scan will scan in the partition
  */
  uint *m_part_mrr_range_length;

  /* For each partition: List of ranges to scan in this partition */
  PARTITION_PART_KEY_MULTI_RANGE **m_part_mrr_range_first;
  PARTITION_PART_KEY_MULTI_RANGE **m_part_mrr_range_current;
  PARTITION_PART_KEY_MULTI_RANGE_HLD *m_partition_part_key_multi_range_hld;

  /*
    Sequence of ranges to be scanned (TODO: why not store this in
    handler::mrr_{iter,funcs}?)
  */
  range_seq_t m_seq;
  RANGE_SEQ_IF *m_seq_if;

  /* Range iterator structure to be supplied to partitions */
  RANGE_SEQ_IF m_part_seq_if;

  virtual int multi_range_key_create_key(
    RANGE_SEQ_IF *seq,
    range_seq_t seq_it
  );
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param,
                                      uint n_ranges, uint *bufsz,
                                      uint *mrr_mode, ha_rows limit,
                                      Cost_estimate *cost) override;
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint key_parts, uint *bufsz,
                                uint *mrr_mode, Cost_estimate *cost) override;
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mrr_mode,
                            HANDLER_BUFFER *buf) override;
  int multi_range_read_next(range_id_t *range_info) override;
  int multi_range_read_explain_info(uint mrr_mode, char *str, size_t size)
    override;
  uint last_part() { return m_last_part; }

private:
  bool init_record_priority_queue();
  void destroy_record_priority_queue();
  int common_index_read(uchar * buf, bool have_start_key);
  int common_first_last(uchar * buf);
  int partition_scan_set_up(uchar * buf, bool idx_read_flag);
  bool check_parallel_search();
  int handle_pre_scan(bool reverse_order, bool use_parallel);
  int handle_unordered_next(uchar * buf, bool next_same);
  int handle_unordered_scan_next_partition(uchar * buf);
  int handle_ordered_index_scan(uchar * buf, bool reverse_order);
  int handle_ordered_index_scan_key_not_found();
  int handle_ordered_next(uchar * buf, bool next_same);
  int handle_ordered_prev(uchar * buf);
  void return_top_record(uchar * buf);
  void swap_blobs(uchar* rec_buf, Ordered_blob_storage ** storage, bool restore);
public:
  /*
    -------------------------------------------------------------------------
    MODULE information calls
    -------------------------------------------------------------------------
    This calls are used to inform the handler of specifics of the ongoing
    scans and other actions. Most of these are used for optimisation
    purposes.
    -------------------------------------------------------------------------
  */
  int info(uint) override;
  void get_dynamic_partition_info(PARTITION_STATS *stat_info, uint part_id)
    override;
  void set_partitions_to_open(List<String> *partition_names) override;
  int change_partitions_to_open(List<String> *partition_names) override;
  int open_read_partitions(char *name_buff, size_t name_buff_size);
  int extra(enum ha_extra_function operation) override;
  int extra_opt(enum ha_extra_function operation, ulong arg) override;
  int reset() override;
  uint count_query_cache_dependant_tables(uint8 *tables_type) override;
  my_bool register_query_cache_dependant_tables(THD *thd,
                                                Query_cache *cache,
                                                Query_cache_block_table **block,
                                                uint *n) override;

private:
  typedef int handler_callback(handler *, void *);

  my_bool reg_query_cache_dependant_table(THD *thd,
                                          char *engine_key,
                                          uint engine_key_len,
                                          char *query_key, uint query_key_len,
                                          uint8 type,
                                          Query_cache *cache,
                                          Query_cache_block_table
                                          **block_table,
                                          handler *file, uint *n);
  static const uint NO_CURRENT_PART_ID= NOT_A_PARTITION_ID;
  int loop_partitions(handler_callback callback, void *param);
  int loop_partitions_over_map(const MY_BITMAP *map,
                               handler_callback callback,
                               void *param);
  int loop_read_partitions(handler_callback callback, void *param);
  int loop_extra_alter(enum ha_extra_function operations);
  void late_extra_cache(uint partition_id);
  void late_extra_no_cache(uint partition_id);
  void prepare_extra_cache(uint cachesize);
  handler *get_open_file_sample() const { return m_file_sample; }
public:

  /*
    -------------------------------------------------------------------------
    MODULE optimiser support
    -------------------------------------------------------------------------
    -------------------------------------------------------------------------
  */

  /*
    NOTE !!!!!!
     -------------------------------------------------------------------------
     -------------------------------------------------------------------------
     One important part of the public handler interface that is not depicted in
     the methods is the attribute records

     which is defined in the base class. This is looked upon directly and is
     set by calling info(HA_STATUS_INFO) ?
     -------------------------------------------------------------------------
  */

private:
  /* Helper functions for optimizer hints. */
  ha_rows min_rows_for_estimate();
  uint get_biggest_used_partition(uint *part_index);
public:

  /*
    keys_to_use_for_scanning can probably be implemented as the
    intersection of all underlying handlers if mixed handlers are used.
    This method is used to derive whether an index can be used for
    index-only scanning when performing an ORDER BY query.
    Only called from one place in sql_select.cc
  */
  const key_map *keys_to_use_for_scanning() override;

  /*
    Called in test_quick_select to determine if indexes should be used.
  */
  IO_AND_CPU_COST scan_time() override;

  IO_AND_CPU_COST key_scan_time(uint inx, ha_rows rows) override;

  IO_AND_CPU_COST keyread_time(uint inx, ulong ranges, ha_rows rows,
                               ulonglong blocks) override;
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override;

  /*
    For the given range how many records are estimated to be in this range.
    Used by optimiser to calculate cost of using a particular index.
  */
  ha_rows records_in_range(uint inx,
                           const key_range * min_key,
                           const key_range * max_key,
                           page_range *pages) override;

  /*
    Upper bound of number records returned in scan is sum of all
    underlying handlers.
  */
  ha_rows estimate_rows_upper_bound() override;

  /*
    table_cache_type is implemented by the underlying handler but all
    underlying handlers must have the same implementation for it to work.
  */
  uint8 table_cache_type() override;
  ha_rows records() override;

  /* Calculate hash value for PARTITION BY KEY tables. */
  static uint32 calculate_key_hash_value(Field **field_array);

  /*
    -------------------------------------------------------------------------
    MODULE print messages
    -------------------------------------------------------------------------
    This module contains various methods that returns text messages for
    table types, index type and error messages.
    -------------------------------------------------------------------------
  */
  /*
    The name of the index type that will be used for display
    Here we must ensure that all handlers use the same index type
    for each index created.
  */
  const char *index_type(uint inx) override;

  /* The name of the table type that will be used for display purposes */
  const char *real_table_type() const override;
  /* The name of the row type used for the underlying tables. */
  enum row_type get_row_type() const override;

  /*
     Handler specific error messages
  */
  void print_error(int error, myf errflag) override;
  bool get_error_message(int error, String * buf) override;
  /*
   -------------------------------------------------------------------------
    MODULE handler characteristics
    -------------------------------------------------------------------------
    This module contains a number of methods defining limitations and
    characteristics of the handler. The partition handler will calculate
    this characteristics based on underlying handler characteristics.
    -------------------------------------------------------------------------

    This is a list of flags that says what the storage engine
    implements. The current table flags are documented in handler.h
    The partition handler will support whatever the underlying handlers
    support except when specifically mentioned below about exceptions
    to this rule.
    NOTE: This cannot be cached since it can depend on TRANSACTION ISOLATION
    LEVEL which is dynamic, see bug#39084.

    HA_TABLE_SCAN_ON_INDEX:
    Used to avoid scanning full tables on an index. If this flag is set then
    the handler always has a primary key (hidden if not defined) and this
    index is used for scanning rather than a full table scan in all
    situations.
    (InnoDB, Federated)

    HA_REC_NOT_IN_SEQ:
    This flag is set for handlers that cannot guarantee that the rows are
    returned according to incremental positions (0, 1, 2, 3...).
    This also means that rnd_next() should return HA_ERR_RECORD_DELETED
    if it finds a deleted row.
    (MyISAM (not fixed length row), HEAP, InnoDB)

    HA_CAN_GEOMETRY:
    Can the storage engine handle spatial data.
    Used to check that no spatial attributes are declared unless
    the storage engine is capable of handling it.
    (MyISAM)

    HA_FAST_KEY_READ:
    Setting this flag indicates that the handler is equally fast in
    finding a row by key as by position.
    This flag is used in a very special situation in conjunction with
    filesort's. For further explanation see intro to init_read_record.
    (HEAP, InnoDB)

    HA_NULL_IN_KEY:
    Is NULL values allowed in indexes.
    If this is not allowed then it is not possible to use an index on a
    NULLable field.
    (HEAP, MyISAM, InnoDB)

    HA_DUPLICATE_POS:
    Tells that we can the position for the conflicting duplicate key
    record is stored in table->file->dupp_ref. (insert uses rnd_pos() on
    this to find the duplicated row)
    (MyISAM)

    HA_CAN_INDEX_BLOBS:
    Is the storage engine capable of defining an index of a prefix on
    a BLOB attribute.
    (Federated, MyISAM, InnoDB)

    HA_AUTO_PART_KEY:
    Auto increment fields can be part of a multi-part key. For second part
    auto-increment keys, the auto_incrementing is done in handler.cc
    (Federated, MyISAM)

    HA_REQUIRE_PRIMARY_KEY:
    Can't define a table without primary key (and cannot handle a table
    with hidden primary key)
    (No handler has this limitation currently)

    HA_STATS_RECORDS_IS_EXACT:
    Does the counter of records after the info call specify an exact
    value or not. If it does this flag is set.
    Only MyISAM and HEAP uses exact count.

    HA_CAN_INSERT_DELAYED:
    Can the storage engine support delayed inserts.
    To start with the partition handler will not support delayed inserts.
    Further investigation needed.
    (HEAP, MyISAM)

    HA_PRIMARY_KEY_IN_READ_INDEX:
    This parameter is set when the handler will also return the primary key
    when doing read-only-key on another index.

    HA_NOT_DELETE_WITH_CACHE:
    Seems to be an old MyISAM feature that is no longer used. No handler
    has it defined but it is checked in init_read_record.
    Further investigation needed.
    (No handler defines it)

    HA_NO_PREFIX_CHAR_KEYS:
    Indexes on prefixes of character fields is not allowed.
    (Federated)

    HA_CAN_FULLTEXT:
    Does the storage engine support fulltext indexes
    The partition handler will start by not supporting fulltext indexes.
    (MyISAM)

    HA_CAN_SQL_HANDLER:
    Can the HANDLER interface in the MySQL API be used towards this
    storage engine.
    (MyISAM, InnoDB)

    HA_NO_AUTO_INCREMENT:
    Set if the storage engine does not support auto increment fields.
    (Currently not set by any handler)

    HA_HAS_CHECKSUM:
    Special MyISAM feature. Has special SQL support in CREATE TABLE.
    No special handling needed by partition handler.
    (MyISAM)

    HA_FILE_BASED:
    Should file names always be in lower case (used by engines
    that map table names to file names.
    Since partition handler has a local file this flag is set.
    (Federated, MyISAM)

    HA_CAN_BIT_FIELD:
    Is the storage engine capable of handling bit fields?
    (MyISAM)

    HA_NEED_READ_RANGE_BUFFER:
    Is Read Multi-Range supported => need multi read range buffer
    This parameter specifies whether a buffer for read multi range
    is needed by the handler. Whether the handler supports this
    feature or not is dependent of whether the handler implements
    read_multi_range* calls or not. The only handler currently
    supporting this feature is NDB so the partition handler need
    not handle this call. There are methods in handler.cc that will
    transfer those calls into index_read and other calls in the
    index scan module.
    (No handler defines it)

    HA_PRIMARY_KEY_REQUIRED_FOR_POSITION:
    Does the storage engine need a PK for position?
    (InnoDB)

    HA_FILE_BASED is always set for partition handler since we use a
    special file for handling names of partitions, engine types.
    HA_REC_NOT_IN_SEQ is always set for partition handler since we cannot
    guarantee that the records will be returned in sequence.
    HA_DUPLICATE_POS,
    HA_CAN_INSERT_DELAYED, HA_PRIMARY_KEY_REQUIRED_FOR_POSITION is disabled
    until further investigated.
  */
  Table_flags table_flags() const override;

  /*
    This is a bitmap of flags that says how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero
    here.

    part is the key part to check. First key part is 0
    If all_parts it's set, MySQL want to know the flags for the combined
    index up to and including 'part'.

    HA_READ_NEXT:
    Does the index support read next, this is assumed in the server
    code and never checked so all indexes must support this.
    Note that the handler can be used even if it doesn't have any index.
    (HEAP, MyISAM, Federated, InnoDB)

    HA_READ_PREV:
    Can the index be used to scan backwards.
    (HEAP, MyISAM, InnoDB)

    HA_READ_ORDER:
    Can the index deliver its record in index order. Typically true for
    all ordered indexes and not true for hash indexes.
    In first step this is not true for partition handler until a merge
    sort has been implemented in partition handler.
    Used to set keymap part_of_sortkey
    This keymap is only used to find indexes usable for resolving an ORDER BY
    in the query. Thus in most cases index_read will work just fine without
    order in result production. When this flag is set it is however safe to
    order all output started by index_read since most engines do this. With
    read_multi_range calls there is a specific flag setting order or not
    order so in those cases ordering of index output can be avoided.
    (InnoDB, HEAP, MyISAM)

    HA_READ_RANGE:
    Specify whether index can handle ranges, typically true for all
    ordered indexes and not true for hash indexes.
    Used by optimiser to check if ranges (as key >= 5) can be optimised
    by index.
    (InnoDB, MyISAM, HEAP)

    HA_ONLY_WHOLE_INDEX:
    Can't use part key searches. This is typically true for hash indexes
    and typically not true for ordered indexes.
    (Federated, HEAP)

    HA_KEYREAD_ONLY:
    Does the storage engine support index-only scans on this index.
    Enables use of HA_EXTRA_KEYREAD and HA_EXTRA_NO_KEYREAD
    Used to set key_map keys_for_keyread and to check in optimiser for
    index-only scans.  When doing a read under HA_EXTRA_KEYREAD the handler
    only have to fill in the columns the key covers. If
    HA_PRIMARY_KEY_IN_READ_INDEX is set then also the PRIMARY KEY columns
    must be updated in the row.
    (InnoDB, MyISAM)
  */
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    /*
      The following code is not safe if you are using different
      storage engines or different index types per partition.
    */
    ulong part_flags= m_file[0]->index_flags(inx, part, all_parts);

    /*
      The underlying storage engine might support Rowid Filtering. But
      ha_partition does not forward the needed SE API calls, so the feature
      will not be used.

      Note: It's the same with IndexConditionPushdown, except for its variant
      of IndexConditionPushdown+BatchedKeyAccess (that one works). Because of
      that, we do not clear HA_DO_INDEX_COND_PUSHDOWN here.
    */
    return part_flags & ~HA_DO_RANGE_FILTER_PUSHDOWN;
  }

  /**
    wrapper function for handlerton alter_table_flags, since
    the ha_partition_hton cannot know all its capabilities
  */
  alter_table_operations alter_table_flags(alter_table_operations flags)
    override;
  /*
    unireg.cc will call the following to make sure that the storage engine
    can handle the data it is about to send.

    The maximum supported values is the minimum of all handlers in the table
  */
  uint min_of_the_max_uint(uint (handler::*operator_func)(void) const) const;
  uint max_supported_record_length() const override;
  uint max_supported_keys() const override;
  uint max_supported_key_parts() const override;
  uint max_supported_key_length() const override;
  uint max_supported_key_part_length() const override;
  uint min_record_length(uint options) const override;

  /*
    -------------------------------------------------------------------------
    MODULE compare records
    -------------------------------------------------------------------------
    cmp_ref checks if two references are the same. For most handlers this is
    a simple memcmp of the reference. However some handlers use primary key
    as reference and this can be the same even if memcmp says they are
    different. This is due to character sets and end spaces and so forth.
    For the partition handler the reference is first two bytes providing the
    partition identity of the referred record and then the reference of the
    underlying handler.
    Thus cmp_ref for the partition handler always returns FALSE for records
    not in the same partition and uses cmp_ref on the underlying handler
    to check whether the rest of the reference part is also the same.
    -------------------------------------------------------------------------
  */
  int cmp_ref(const uchar * ref1, const uchar * ref2) override;
  /*
    -------------------------------------------------------------------------
    MODULE auto increment
    -------------------------------------------------------------------------
    This module is used to handle the support of auto increments.

    This variable in the handler is used as part of the handler interface
    It is maintained by the parent handler object and should not be
    touched by child handler objects (see handler.cc for its use).

    auto_increment_column_changed
     -------------------------------------------------------------------------
  */
  bool need_info_for_auto_inc() override;
  bool can_use_for_auto_inc_init() override;
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  void release_auto_increment() override;
private:
  int reset_auto_increment(ulonglong value) override;
  int update_next_auto_inc_val();
  virtual void lock_auto_increment()
  {
    /* lock already taken */
    if (auto_increment_safe_stmt_log_lock)
      return;
    if (table_share->tmp_table == NO_TMP_TABLE)
    {
      part_share->lock_auto_inc();
      DBUG_ASSERT(!auto_increment_lock);
      auto_increment_lock= TRUE;
    }
  }
  virtual void unlock_auto_increment()
  {
    /*
      If auto_increment_safe_stmt_log_lock is true, we have to keep the lock.
      It will be set to false and thus unlocked at the end of the statement by
      ha_partition::release_auto_increment.
    */
    if (auto_increment_lock && !auto_increment_safe_stmt_log_lock)
    {
      auto_increment_lock= FALSE;
      part_share->unlock_auto_inc();
    }
  }
  virtual void set_auto_increment_if_higher(Field *field)
  {
    ulonglong nr= (((Field_num*) field)->unsigned_flag ||
                   field->val_int() > 0) ? field->val_int() : 0;
    update_next_auto_inc_val();
    lock_auto_increment();
    /* must check when the mutex is taken */
    if (nr >= part_share->next_auto_inc_val)
      part_share->next_auto_inc_val= nr + 1;
    unlock_auto_increment();
  }

  void check_insert_or_replace_autoincrement()
  {
    /*
      If we INSERT or REPLACE into the table having the AUTO_INCREMENT column,
      we have to read all partitions for the next autoincrement value
      unless we already did it.
    */
    if (!part_share->auto_inc_initialized &&
        (ha_thd()->lex->sql_command == SQLCOM_INSERT ||
         ha_thd()->lex->sql_command == SQLCOM_INSERT_SELECT ||
         ha_thd()->lex->sql_command == SQLCOM_REPLACE ||
         ha_thd()->lex->sql_command == SQLCOM_REPLACE_SELECT) &&
        table->found_next_number_field)
      bitmap_set_all(&m_part_info->read_partitions);
  }

public:

  /*
     -------------------------------------------------------------------------
     MODULE initialize handler for HANDLER call
     -------------------------------------------------------------------------
     This method is a special InnoDB method called before a HANDLER query.
     -------------------------------------------------------------------------
  */
  void init_table_handle_for_HANDLER() override;

  /*
    The remainder of this file defines the handler methods not implemented
    by the partition handler
  */

  /*
    -------------------------------------------------------------------------
    MODULE foreign key support
    -------------------------------------------------------------------------
    The following methods are used to implement foreign keys as supported by
    InnoDB. Implement this ??
    get_foreign_key_create_info is used by SHOW CREATE TABLE to get a textual
    description of how the CREATE TABLE part to define FOREIGN KEY's is done.
    free_foreign_key_create_info is used to free the memory area that provided
    this description.
    can_switch_engines checks if it is ok to switch to a new engine based on
    the foreign key info in the table.
    -------------------------------------------------------------------------

    virtual char* get_foreign_key_create_info()
    virtual void free_foreign_key_create_info(char* str)

    virtual int get_foreign_key_list(THD *thd,
    List<FOREIGN_KEY_INFO> *f_key_list)
    virtual uint referenced_by_foreign_key()
  */
    bool can_switch_engines() override;
  /*
    -------------------------------------------------------------------------
    MODULE fulltext index
    -------------------------------------------------------------------------
  */
    void ft_close_search(FT_INFO *handler);
    int ft_init() override;
    int pre_ft_init() override;
    void ft_end() override;
    int pre_ft_end() override;
    FT_INFO *ft_init_ext(uint flags, uint inx, String *key) override;
    int ft_read(uchar *buf) override;
    int pre_ft_read(bool use_parallel) override;

  /*
     -------------------------------------------------------------------------
     MODULE restart full table scan at position (MyISAM)
     -------------------------------------------------------------------------
     The following method is only used by MyISAM when used as
     temporary tables in a join.
     int restart_rnd_next(uchar *buf, uchar *pos) override;
  */

  /*
    -------------------------------------------------------------------------
    MODULE in-place ALTER TABLE
    -------------------------------------------------------------------------
    These methods are in the handler interface. (used by innodb-plugin)
    They are used for in-place alter table:
    -------------------------------------------------------------------------
  */
    enum_alter_inplace_result
      check_if_supported_inplace_alter(TABLE *altered_table,
                                       Alter_inplace_info *ha_alter_info)
      override;
    bool prepare_inplace_alter_table(TABLE *altered_table,
                                     Alter_inplace_info *ha_alter_info)
      override;
    bool inplace_alter_table(TABLE *altered_table,
                            Alter_inplace_info *ha_alter_info) override;
    bool commit_inplace_alter_table(TABLE *altered_table,
                                    Alter_inplace_info *ha_alter_info,
                                    bool commit) override;
  /*
    -------------------------------------------------------------------------
    MODULE tablespace support
    -------------------------------------------------------------------------
    Admin of table spaces is not applicable to the partition handler (InnoDB)
    This means that the following method is not implemented:
    -------------------------------------------------------------------------
    virtual int discard_or_import_tablespace(my_bool discard)
  */

  /*
    -------------------------------------------------------------------------
    MODULE admin MyISAM
    -------------------------------------------------------------------------

    -------------------------------------------------------------------------
      OPTIMIZE TABLE, CHECK TABLE, ANALYZE TABLE and REPAIR TABLE are
      mapped to a routine that handles looping over a given set of
      partitions and those routines send a flag indicating to execute on
      all partitions.
    -------------------------------------------------------------------------
  */
    int optimize(THD* thd, HA_CHECK_OPT *check_opt) override;
    int analyze(THD* thd, HA_CHECK_OPT *check_opt) override;
    int check(THD* thd, HA_CHECK_OPT *check_opt) override;
    int repair(THD* thd, HA_CHECK_OPT *check_opt) override;
    bool check_and_repair(THD *thd) override;
    bool auto_repair(int error) const override;
    bool is_crashed() const override;
    int check_for_upgrade(HA_CHECK_OPT *check_opt) override;

  /*
    -------------------------------------------------------------------------
    MODULE condition pushdown
    -------------------------------------------------------------------------
  */
    const COND *cond_push(const COND *cond) override;
    void cond_pop() override;
    int info_push(uint info_type, void *info) override;

    private:
    int handle_opt_partitions(THD *thd, HA_CHECK_OPT *check_opt, uint flags);
    int handle_opt_part(THD *thd, HA_CHECK_OPT *check_opt, uint part_id,
                        uint flag);
    /**
      Check if the rows are placed in the correct partition.  If the given
      argument is true, then move the rows to the correct partition.
    */
    int check_misplaced_rows(uint read_part_id, bool repair);
    void append_row_to_str(String &str);
    public:

    int pre_calculate_checksum() override;
    int calculate_checksum() override;

  /* Enabled keycache for performance reasons, WL#4571 */
    int assign_to_keycache(THD* thd, HA_CHECK_OPT *check_opt) override;
    int preload_keys(THD* thd, HA_CHECK_OPT* check_opt) override;
    TABLE_LIST *get_next_global_for_child() override;

  /*
    -------------------------------------------------------------------------
    MODULE enable/disable indexes
    -------------------------------------------------------------------------
    Enable/Disable Indexes are only supported by HEAP and MyISAM.
    -------------------------------------------------------------------------
  */
    int disable_indexes(key_map map, bool persist) override;
    int enable_indexes(key_map map, bool persist) override;
    int indexes_are_disabled() override;

  /*
    -------------------------------------------------------------------------
    MODULE append_create_info
    -------------------------------------------------------------------------
    append_create_info is only used by MyISAM MERGE tables and the partition
    handler will not support this handler as underlying handler.
    Implement this??
    -------------------------------------------------------------------------
    virtual void append_create_info(String *packet)
  */

  /*
    the following heavily relies on the fact that all partitions
    are in the same storage engine.

    When this limitation is lifted, the following hack should go away,
    and a proper interface for engines needs to be introduced:

      an PARTITION_SHARE structure that has a pointer to the TABLE_SHARE.
      is given to engines everywhere where TABLE_SHARE is used now
      has members like option_struct, ha_data
      perhaps TABLE needs to be split the same way too...

    this can also be done before partition will support a mix of engines,
    but preferably together with other incompatible API changes.
  */
  handlerton *partition_ht() const override
  {
    handlerton *h= m_file[0]->ht;
    for (uint i=1; i < m_tot_parts; i++)
      DBUG_ASSERT(h == m_file[i]->ht);
    return h;
  }

  bool partition_engine() override { return 1;}

  /**
     Get the number of records in part_elem and its subpartitions, if any.
  */
  ha_rows part_records(partition_element *part_elem)
  {
    DBUG_ASSERT(m_part_info);
    uint32 sub_factor= m_part_info->num_subparts ? m_part_info->num_subparts : 1;
    uint32 part_id= part_elem->id * sub_factor;
    uint32 part_id_end= part_id + sub_factor;
    DBUG_ASSERT(part_id_end <= m_tot_parts);
    ha_rows part_recs= 0;
    for (; part_id < part_id_end; ++part_id)
    {
      handler *file= m_file[part_id];
      file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK | HA_STATUS_OPEN);
      part_recs+= file->stats.records;
    }
    return part_recs;
  }

  int notify_tabledef_changed(LEX_CSTRING *db, LEX_CSTRING *table,
                              LEX_CUSTRING *frm, LEX_CUSTRING *version);

  friend int cmp_key_rowid_part_id(void *ptr, uchar *ref1, uchar *ref2);
  friend int cmp_key_part_id(void *key_p, uchar *ref1, uchar *ref2);

  bool can_convert_nocopy(const Field &field,
                          const Column_definition &new_field) const override;
  void handler_stats_updated() override;
  void set_optimizer_costs(THD *thd) override;
  void update_optimizer_costs(OPTIMIZER_COSTS *costs) override;
  virtual ulonglong index_blocks(uint index, uint ranges, ha_rows rows) override;
  virtual ulonglong row_blocks() override;
};

#endif /* HA_PARTITION_INCLUDED */
