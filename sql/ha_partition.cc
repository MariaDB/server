/*
  Copyright (c) 2005, 2017, Oracle and/or its affiliates.
  Copyright (c) 2009, 2018, MariaDB

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*
  This handler was developed by Mikael Ronstrom for version 5.1 of MySQL.
  It is an abstraction layer on top of other handlers such as MyISAM,
  InnoDB, Federated, Berkeley DB and so forth. Partitioned tables can also
  be handled by a storage engine. The current example of this is NDB
  Cluster that has internally handled partitioning. This have benefits in
  that many loops needed in the partition handler can be avoided.

  Partitioning has an inherent feature which in some cases is positive and
  in some cases is negative. It splits the data into chunks. This makes
  the data more manageable, queries can easily be parallelised towards the
  parts and indexes are split such that there are less levels in the
  index trees. The inherent disadvantage is that to use a split index
  one has to scan all index parts which is ok for large queries but for
  small queries it can be a disadvantage.

  Partitioning lays the foundation for more manageable databases that are
  extremely large. It does also lay the foundation for more parallelism
  in the execution of queries. This functionality will grow with later
  versions of MySQL/MariaDB.

  The partition is setup to use table locks. It implements an partition "SHARE"
  that is inserted into a hash by table name. You can use this to store
  information of state that any partition handler object will be able to see
  if it is using the same table.

  Please read the object definition in ha_partition.h before reading the rest
  if this file.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_parse.h"                          // append_file_to_dir
#include "create_options.h"

#ifdef WITH_PARTITION_STORAGE_ENGINE
#include "ha_partition.h"
#include "sql_table.h"                        // tablename_to_filename
#include "key.h"
#include "sql_plugin.h"
#include "sql_show.h"                        // append_identifier
#include "sql_admin.h"                       // SQL_ADMIN_MSG_TEXT_SIZE
#include "sql_select.h"

#include "debug_sync.h"

/* First 4 bytes in the .par file is the number of 32-bit words in the file */
#define PAR_WORD_SIZE 4
/* offset to the .par file checksum */
#define PAR_CHECKSUM_OFFSET 4
/* offset to the total number of partitions */
#define PAR_NUM_PARTS_OFFSET 8
/* offset to the engines array */
#define PAR_ENGINES_OFFSET 12
#define PARTITION_ENABLED_TABLE_FLAGS (HA_FILE_BASED | \
                                       HA_REC_NOT_IN_SEQ | \
                                       HA_CAN_REPAIR)
#define PARTITION_DISABLED_TABLE_FLAGS (HA_CAN_GEOMETRY | \
                                        HA_DUPLICATE_POS | \
                                        HA_CAN_INSERT_DELAYED | \
                                        HA_READ_BEFORE_WRITE_REMOVAL |\
                                        HA_CAN_TABLES_WITHOUT_ROLLBACK)

static const char *ha_par_ext= ".par";

/****************************************************************************
                MODULE create/delete handler object
****************************************************************************/

static handler *partition_create_handler(handlerton *hton,
                                         TABLE_SHARE *share,
                                         MEM_ROOT *mem_root);
static uint partition_flags();
static alter_table_operations alter_table_flags(alter_table_operations flags);

/*
  If frm_error() is called then we will use this to to find out what file
  extensions exist for the storage engine. This is also used by the default
  rename_table and delete_table method in handler.cc.
*/
static const char *ha_partition_ext[]=
{
  ha_par_ext, NullS
};


#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key key_partition_auto_inc_mutex;

static PSI_mutex_info all_partition_mutexes[]=
{
  { &key_partition_auto_inc_mutex, "Partition_share::auto_inc_mutex", 0}
};

static void init_partition_psi_keys(void)
{
  const char* category= "partition";
  int count;

  count= array_elements(all_partition_mutexes);
  mysql_mutex_register(category, all_partition_mutexes, count);
}
#endif /* HAVE_PSI_INTERFACE */

static int partition_initialize(void *p)
{
  handlerton *partition_hton;
  partition_hton= (handlerton *)p;

  partition_hton->state= SHOW_OPTION_YES;
  partition_hton->db_type= DB_TYPE_PARTITION_DB;
  partition_hton->create= partition_create_handler;
  partition_hton->partition_flags= partition_flags;
  partition_hton->alter_table_flags= alter_table_flags;
  partition_hton->flags= HTON_NOT_USER_SELECTABLE |
                         HTON_HIDDEN |
                         HTON_TEMPORARY_NOT_SUPPORTED;
  partition_hton->tablefile_extensions= ha_partition_ext;

#ifdef HAVE_PSI_INTERFACE
  init_partition_psi_keys();
#endif
  return 0;
}


/**
  Initialize and allocate space for partitions shares.

  @param num_parts  Number of partitions to allocate storage for.

  @return Operation status.
    @retval true  Failure (out of memory).
    @retval false Success.
*/

bool Partition_share::init(uint num_parts)
{
  DBUG_ENTER("Partition_share::init");
  auto_inc_initialized= false;
  partition_name_hash_initialized= false;
  next_auto_inc_val= 0;
  if (partitions_share_refs.init(num_parts))
  {
    DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/*
  Create new partition handler

  SYNOPSIS
    partition_create_handler()
    table                       Table object

  RETURN VALUE
    New partition object
*/

static handler *partition_create_handler(handlerton *hton,
                                         TABLE_SHARE *share,
                                         MEM_ROOT *mem_root)
{
  ha_partition *file= new (mem_root) ha_partition(hton, share);
  if (file && file->initialize_partition(mem_root))
  {
    delete file;
    file= 0;
  }
  return file;
}

/*
  HA_CAN_PARTITION:
  Used by storage engines that can handle partitioning without this
  partition handler
  (Partition, NDB)

  HA_CAN_UPDATE_PARTITION_KEY:
  Set if the handler can update fields that are part of the partition
  function.

  HA_CAN_PARTITION_UNIQUE:
  Set if the handler can handle unique indexes where the fields of the
  unique key are not part of the fields of the partition function. Thus
  a unique key can be set on all fields.

  HA_USE_AUTO_PARTITION
  Set if the handler sets all tables to be partitioned by default.
*/

static uint partition_flags()
{
  return HA_CAN_PARTITION;
}

static alter_table_operations alter_table_flags(alter_table_operations flags __attribute__((unused)))
{
  return (HA_PARTITION_FUNCTION_SUPPORTED |
          HA_FAST_CHANGE_PARTITION);
}

/*
  Constructor method

  SYNOPSIS
    ha_partition()
    table                       Table object

  RETURN VALUE
    NONE
*/

ha_partition::ha_partition(handlerton *hton, TABLE_SHARE *share)
  :handler(hton, share)
{
  DBUG_ENTER("ha_partition::ha_partition(table)");
  ha_partition_init();
  DBUG_VOID_RETURN;
}


/* Initialize all partition variables */

void ha_partition::ha_partition_init()
{
  init_alloc_root(&m_mem_root, "ha_partition", 512, 512, MYF(0));
  init_handler_variables();
}

/*
  Constructor method

  SYNOPSIS
    ha_partition()
    part_info                       Partition info

  RETURN VALUE
    NONE
*/

ha_partition::ha_partition(handlerton *hton, partition_info *part_info)
  :handler(hton, NULL)
{
  DBUG_ENTER("ha_partition::ha_partition(part_info)");
  DBUG_ASSERT(part_info);
  ha_partition_init();
  m_part_info= part_info;
  m_create_handler= TRUE;
  m_is_sub_partitioned= m_part_info->is_sub_partitioned();
  DBUG_VOID_RETURN;
}

/**
  ha_partition constructor method used by ha_partition::clone()

  @param hton               Handlerton (partition_hton)
  @param share              Table share object
  @param part_info_arg      partition_info to use
  @param clone_arg          ha_partition to clone
  @param clme_mem_root_arg  MEM_ROOT to use

  @return New partition handler
*/

ha_partition::ha_partition(handlerton *hton, TABLE_SHARE *share,
                           partition_info *part_info_arg,
                           ha_partition *clone_arg,
                           MEM_ROOT *clone_mem_root_arg)
  :handler(hton, share)
{
  DBUG_ENTER("ha_partition::ha_partition(clone)");
  ha_partition_init();
  m_part_info= part_info_arg;
  m_create_handler= TRUE;
  m_is_sub_partitioned= m_part_info->is_sub_partitioned();
  m_is_clone_of= clone_arg;
  m_clone_mem_root= clone_mem_root_arg;
  part_share= clone_arg->part_share;
  m_tot_parts= clone_arg->m_tot_parts;
  m_pkey_is_clustered= clone_arg->primary_key_is_clustered();
  DBUG_VOID_RETURN;
}

/*
  Initialize handler object

  SYNOPSIS
    init_handler_variables()

  RETURN VALUE
    NONE
*/

void ha_partition::init_handler_variables()
{
  active_index= MAX_KEY;
  m_mode= 0;
  m_open_test_lock= 0;
  m_file_buffer= NULL;
  m_name_buffer_ptr= NULL;
  m_engine_array= NULL;
  m_connect_string= NULL;
  m_file= NULL;
  m_file_tot_parts= 0;
  m_reorged_file= NULL;
  m_new_file= NULL;
  m_reorged_parts= 0;
  m_added_file= NULL;
  m_tot_parts= 0;
  m_pkey_is_clustered= 0;
  m_part_spec.start_part= NO_CURRENT_PART_ID;
  m_scan_value= 2;
  m_ref_length= 0;
  m_part_spec.end_part= NO_CURRENT_PART_ID;
  m_index_scan_type= partition_no_index_scan;
  m_start_key.key= NULL;
  m_start_key.length= 0;
  m_myisam= FALSE;
  m_innodb= FALSE;
  m_extra_cache= FALSE;
  m_extra_cache_size= 0;
  m_extra_prepare_for_update= FALSE;
  m_extra_cache_part_id= NO_CURRENT_PART_ID;
  m_handler_status= handler_not_initialized;
  m_part_field_array= NULL;
  m_ordered_rec_buffer= NULL;
  m_top_entry= NO_CURRENT_PART_ID;
  m_rec_length= 0;
  m_last_part= 0;
  m_rec0= 0;
  m_err_rec= NULL;
  m_curr_key_info[0]= NULL;
  m_curr_key_info[1]= NULL;
  m_part_func_monotonicity_info= NON_MONOTONIC;
  m_key_not_found= FALSE;
  auto_increment_lock= FALSE;
  auto_increment_safe_stmt_log_lock= FALSE;
  /*
    this allows blackhole to work properly
  */
  m_num_locks= 0;
  m_part_info= NULL;
  m_create_handler= FALSE;
  m_is_sub_partitioned= 0;
  m_is_clone_of= NULL;
  m_clone_mem_root= NULL;
  part_share= NULL;
  m_new_partitions_share_refs.empty();
  m_part_ids_sorted_by_num_of_records= NULL;
  m_partitions_to_open= NULL;

  m_range_info= NULL;
  m_mrr_full_buffer_size= 0;
  m_mrr_new_full_buffer_size= 0;
  m_mrr_full_buffer= NULL;
  m_mrr_range_first= NULL;

  m_pre_calling= FALSE;
  m_pre_call_use_parallel= FALSE;

  ft_first= ft_current=  NULL;
  bulk_access_executing= FALSE;                 // For future

  /*
    Clear bitmaps to allow on one to call my_bitmap_free() on them at any time
  */
  my_bitmap_clear(&m_bulk_insert_started);
  my_bitmap_clear(&m_locked_partitions);
  my_bitmap_clear(&m_partitions_to_reset);
  my_bitmap_clear(&m_key_not_found_partitions);
  my_bitmap_clear(&m_mrr_used_partitions);
  my_bitmap_clear(&m_opened_partitions);
  m_file_sample= NULL;

#ifdef DONT_HAVE_TO_BE_INITALIZED
  m_start_key.flag= 0;
  m_ordered= TRUE;
#endif
}


const char *ha_partition::table_type() const
{
  // we can do this since we only support a single engine type
  return m_file[0]->table_type();
}


/*
  Destructor method

  SYNOPSIS
    ~ha_partition()

  RETURN VALUE
    NONE
*/

ha_partition::~ha_partition()
{
  DBUG_ENTER("ha_partition::~ha_partition()");
  if (m_new_partitions_share_refs.elements)
    m_new_partitions_share_refs.delete_elements();
  if (m_file != NULL)
  {
    uint i;
    for (i= 0; i < m_tot_parts; i++)
      delete m_file[i];
  }
  destroy_record_priority_queue();
  my_free(m_part_ids_sorted_by_num_of_records);

  if (m_added_file)
  {
    for (handler **ph= m_added_file; *ph; ph++)
      delete (*ph);
  }
  clear_handler_file();
  free_root(&m_mem_root, MYF(0));

  DBUG_VOID_RETURN;
}


/*
  Initialize partition handler object

  SYNOPSIS
    initialize_partition()
    mem_root			Allocate memory through this

  RETURN VALUE
    1                         Error
    0                         Success

  DESCRIPTION

  The partition handler is only a layer on top of other engines. Thus it
  can't really perform anything without the underlying handlers. Thus we
  add this method as part of the allocation of a handler object.

  1) Allocation of underlying handlers
     If we have access to the partition info we will allocate one handler
     instance for each partition.
  2) Allocation without partition info
     The cases where we don't have access to this information is when called
     in preparation for delete_table and rename_table and in that case we
     only need to set HA_FILE_BASED. In that case we will use the .par file
     that contains information about the partitions and their engines and
     the names of each partition.
  3) Table flags initialisation
     We need also to set table flags for the partition handler. This is not
     static since it depends on what storage engines are used as underlying
     handlers.
     The table flags is set in this routine to simulate the behaviour of a
     normal storage engine
     The flag HA_FILE_BASED will be set independent of the underlying handlers
  4) Index flags initialisation
     When knowledge exists on the indexes it is also possible to initialize the
     index flags. Again the index flags must be initialized by using the under-
     lying handlers since this is storage engine dependent.
     The flag HA_READ_ORDER will be reset for the time being to indicate no
     ordered output is available from partition handler indexes. Later a merge
     sort will be performed using the underlying handlers.
  5) primary_key_is_clustered and has_transactions are
     calculated here.

*/

bool ha_partition::initialize_partition(MEM_ROOT *mem_root)
{
  handler **file_array, *file;
  ulonglong check_table_flags;
  DBUG_ENTER("ha_partition::initialize_partition");

  if (m_create_handler)
  {
    m_tot_parts= m_part_info->get_tot_partitions();
    DBUG_ASSERT(m_tot_parts > 0);
    if (new_handlers_from_part_info(mem_root))
      DBUG_RETURN(1);
  }
  else if (!table_share || !table_share->normalized_path.str)
  {
    /*
      Called with dummy table share (delete, rename and alter table).
      Don't need to set-up anything.
    */
    DBUG_RETURN(0);
  }
  else if (get_from_handler_file(table_share->normalized_path.str,
                                 mem_root, false))
  {
    my_error(ER_FAILED_READ_FROM_PAR_FILE, MYF(0));
    DBUG_RETURN(1);
  }
  /*
    We create all underlying table handlers here. We do it in this special
    method to be able to report allocation errors.

    Set up primary_key_is_clustered and
    has_transactions since they are called often in all kinds of places,
    other parameters are calculated on demand.
    Verify that all partitions have the same table_flags.
  */
  check_table_flags= m_file[0]->ha_table_flags();
  m_pkey_is_clustered= TRUE;
  file_array= m_file;
  do
  {
    file= *file_array;
    if (!file->primary_key_is_clustered())
      m_pkey_is_clustered= FALSE;
    if (check_table_flags != file->ha_table_flags())
    {
      my_error(ER_MIX_HANDLER_ERROR, MYF(0));
      DBUG_RETURN(1);
    }
  } while (*(++file_array));
  m_handler_status= handler_initialized;
  DBUG_RETURN(0);
}

/****************************************************************************
                MODULE meta data changes
****************************************************************************/
/*
  Delete a table

  SYNOPSIS
    delete_table()
    name                    Full path of table name

  RETURN VALUE
    >0                        Error
    0                         Success

  DESCRIPTION
    Used to delete a table. By the time delete_table() has been called all
    opened references to this table will have been closed (and your globally
    shared references released. The variable name will just be the name of
    the table. You will need to remove any files you have created at this
    point.

    If you do not implement this, the default delete_table() is called from
    handler.cc and it will delete all files with the file extentions returned
    by bas_ext().

    Called from handler.cc by delete_table and  ha_create_table(). Only used
    during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
    the storage engine.
*/

int ha_partition::delete_table(const char *name)
{
  DBUG_ENTER("ha_partition::delete_table");

  DBUG_RETURN(del_ren_table(name, NULL));
}


/*
  Rename a table

  SYNOPSIS
    rename_table()
    from                      Full path of old table name
    to                        Full path of new table name

  RETURN VALUE
    >0                        Error
    0                         Success

  DESCRIPTION
    Renames a table from one name to another from alter table call.

    If you do not implement this, the default rename_table() is called from
    handler.cc and it will rename all files with the file extentions returned
    by bas_ext().

    Called from sql_table.cc by mysql_rename_table().
*/

int ha_partition::rename_table(const char *from, const char *to)
{
  DBUG_ENTER("ha_partition::rename_table");

  DBUG_RETURN(del_ren_table(from, to));
}


/*
  Create the handler file (.par-file)

  SYNOPSIS
    create_partitioning_metadata()
    name                              Full path of table name
    create_info                       Create info generated for CREATE TABLE

  RETURN VALUE
    >0                        Error
    0                         Success

  DESCRIPTION
    create_partitioning_metadata is called to create any handler specific files
    before opening the file with openfrm to later call ::create on the
    file object.
    In the partition handler this is used to store the names of partitions
    and types of engines in the partitions.
*/

int ha_partition::create_partitioning_metadata(const char *path,
                                       const char *old_path,
                                       int action_flag)
{
  DBUG_ENTER("ha_partition::create_partitioning_metadata()");

  /*
    We need to update total number of parts since we might write the handler
    file as part of a partition management command
  */
  if (action_flag == CHF_DELETE_FLAG ||
      action_flag == CHF_RENAME_FLAG)
  {
    char name[FN_REFLEN];
    char old_name[FN_REFLEN];

    strxmov(name, path, ha_par_ext, NullS);
    strxmov(old_name, old_path, ha_par_ext, NullS);
    if ((action_flag == CHF_DELETE_FLAG &&
         mysql_file_delete(key_file_partition, name, MYF(MY_WME))) ||
        (action_flag == CHF_RENAME_FLAG &&
         mysql_file_rename(key_file_partition, old_name, name, MYF(MY_WME))))
    {
      DBUG_RETURN(TRUE);
    }
  }
  else if (action_flag == CHF_CREATE_FLAG)
  {
    if (create_handler_file(path))
    {
      my_error(ER_CANT_CREATE_HANDLER_FILE, MYF(0));
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


/*
  Create a partitioned table

  SYNOPSIS
    create()
    name                              Full path of table name
    table_arg                         Table object
    create_info                       Create info generated for CREATE TABLE

  RETURN VALUE
    >0                        Error
    0                         Success

  DESCRIPTION
    create() is called to create a table. The variable name will have the name
    of the table. When create() is called you do not need to worry about
    opening the table. Also, the FRM file will have already been created so
    adjusting create_info will not do you any good. You can overwrite the frm
    file at this point if you wish to change the table definition, but there
    are no methods currently provided for doing that.

    Called from handler.cc by ha_create_table().
*/

int ha_partition::create(const char *name, TABLE *table_arg,
			 HA_CREATE_INFO *create_info)
{
  int error;
  char name_buff[FN_REFLEN + 1], name_lc_buff[FN_REFLEN];
  char *name_buffer_ptr;
  const char *path;
  uint i;
  List_iterator_fast <partition_element> part_it(m_part_info->partitions);
  partition_element *part_elem;
  handler **file, **abort_file;
  DBUG_ENTER("ha_partition::create");
  DBUG_PRINT("enter", ("name: '%s'", name));

  DBUG_ASSERT(!fn_frm_ext(name));

  /* Not allowed to create temporary partitioned tables */
  if (create_info && create_info->tmp_table())
  {
    my_error(ER_PARTITION_NO_TEMPORARY, MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (get_from_handler_file(name, ha_thd()->mem_root, false))
    DBUG_RETURN(TRUE);
  DBUG_ASSERT(m_file_buffer);
  name_buffer_ptr= m_name_buffer_ptr;
  file= m_file;
  /*
    Since ha_partition has HA_FILE_BASED, it must alter underlying table names
    if they do not have HA_FILE_BASED and lower_case_table_names == 2.
    See Bug#37402, for Mac OS X.
    The appended #P#<partname>[#SP#<subpartname>] will remain in current case.
    Using the first partitions handler, since mixing handlers is not allowed.
  */
  path= get_canonical_filename(*file, name, name_lc_buff);
  for (i= 0; i < m_part_info->num_parts; i++)
  {
    part_elem= part_it++;
    if (m_is_sub_partitioned)
    {
      uint j;
      List_iterator_fast <partition_element> sub_it(part_elem->subpartitions);
      for (j= 0; j < m_part_info->num_subparts; j++)
      {
        part_elem= sub_it++;
        if (unlikely((error= create_partition_name(name_buff,
                                                   sizeof(name_buff), path,
                                                   name_buffer_ptr,
                                                   NORMAL_PART_NAME, FALSE))))
          goto create_error;
        if (unlikely((error= set_up_table_before_create(table_arg, name_buff,
                                                        create_info,
                                                        part_elem)) ||
                     ((error= (*file)->ha_create(name_buff, table_arg,
                                                 create_info)))))
          goto create_error;

        name_buffer_ptr= strend(name_buffer_ptr) + 1;
        file++;
      }
    }
    else
    {
      if (unlikely((error= create_partition_name(name_buff, sizeof(name_buff),
                                                 path, name_buffer_ptr,
                                                 NORMAL_PART_NAME, FALSE))))
        goto create_error;
      if (unlikely((error= set_up_table_before_create(table_arg, name_buff,
                                                      create_info,
                                                      part_elem)) ||
                   ((error= (*file)->ha_create(name_buff, table_arg,
                                               create_info)))))
        goto create_error;

      name_buffer_ptr= strend(name_buffer_ptr) + 1;
      file++;
    }
  }
  DBUG_RETURN(0);

create_error:
  name_buffer_ptr= m_name_buffer_ptr;
  for (abort_file= file, file= m_file; file < abort_file; file++)
  {
    if (!create_partition_name(name_buff, sizeof(name_buff), path,
                               name_buffer_ptr, NORMAL_PART_NAME, FALSE))
      (void) (*file)->ha_delete_table((const char*) name_buff);
    name_buffer_ptr= strend(name_buffer_ptr) + 1;
  }
  handler::delete_table(name);
  DBUG_RETURN(error);
}


/*
  Drop partitions as part of ALTER TABLE of partitions

  SYNOPSIS
    drop_partitions()
    path                        Complete path of db and table name

  RETURN VALUE
    >0                          Failure
    0                           Success

  DESCRIPTION
    Use part_info object on handler object to deduce which partitions to
    drop (each partition has a state attached to it)
*/

int ha_partition::drop_partitions(const char *path)
{
  List_iterator<partition_element> part_it(m_part_info->partitions);
  char part_name_buff[FN_REFLEN + 1];
  uint num_parts= m_part_info->partitions.elements;
  uint num_subparts= m_part_info->num_subparts;
  uint i= 0;
  uint name_variant;
  int  ret_error;
  int  error= 0;
  DBUG_ENTER("ha_partition::drop_partitions");

  /*
    Assert that it works without HA_FILE_BASED and lower_case_table_name = 2.
    We use m_file[0] as long as all partitions have the same storage engine.
  */
  DBUG_ASSERT(!strcmp(path, get_canonical_filename(m_file[0], path,
                                                   part_name_buff)));
  do
  {
    partition_element *part_elem= part_it++;
    if (part_elem->part_state == PART_TO_BE_DROPPED)
    {
      handler *file;
      /*
        This part is to be dropped, meaning the part or all its subparts.
      */
      name_variant= NORMAL_PART_NAME;
      if (m_is_sub_partitioned)
      {
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        uint j= 0, part;
        do
        {
          partition_element *sub_elem= sub_it++;
          part= i * num_subparts + j;
          if (unlikely((ret_error=
                        create_subpartition_name(part_name_buff,
                                                 sizeof(part_name_buff), path,
                                                 part_elem->partition_name,
                                                 sub_elem->partition_name,
                                                 name_variant))))
            error= ret_error;
          file= m_file[part];
          DBUG_PRINT("info", ("Drop subpartition %s", part_name_buff));
          if (unlikely((ret_error= file->ha_delete_table(part_name_buff))))
            error= ret_error;
          if (unlikely(deactivate_ddl_log_entry(sub_elem->log_entry->
                                                entry_pos)))
            error= 1;
        } while (++j < num_subparts);
      }
      else
      {
        if ((ret_error= create_partition_name(part_name_buff,
                          sizeof(part_name_buff), path,
                          part_elem->partition_name, name_variant, TRUE)))
          error= ret_error;
        else
        {
          file= m_file[i];
          DBUG_PRINT("info", ("Drop partition %s", part_name_buff));
          if (unlikely((ret_error= file->ha_delete_table(part_name_buff))))
            error= ret_error;
          if (unlikely(deactivate_ddl_log_entry(part_elem->log_entry->
                                                entry_pos)))
            error= 1;
        }
      }
      if (part_elem->part_state == PART_IS_CHANGED)
        part_elem->part_state= PART_NORMAL;
      else
        part_elem->part_state= PART_IS_DROPPED;
    }
  } while (++i < num_parts);
  (void) sync_ddl_log();
  DBUG_RETURN(error);
}


/*
  Rename partitions as part of ALTER TABLE of partitions

  SYNOPSIS
    rename_partitions()
    path                        Complete path of db and table name

  RETURN VALUE
    TRUE                        Failure
    FALSE                       Success

  DESCRIPTION
    When reorganising partitions, adding hash partitions and coalescing
    partitions it can be necessary to rename partitions while holding
    an exclusive lock on the table.
    Which partitions to rename is given by state of partitions found by the
    partition info struct referenced from the handler object
*/

int ha_partition::rename_partitions(const char *path)
{
  List_iterator<partition_element> part_it(m_part_info->partitions);
  List_iterator<partition_element> temp_it(m_part_info->temp_partitions);
  char part_name_buff[FN_REFLEN + 1];
  char norm_name_buff[FN_REFLEN + 1];
  uint num_parts= m_part_info->partitions.elements;
  uint part_count= 0;
  uint num_subparts= m_part_info->num_subparts;
  uint i= 0;
  uint j= 0;
  int error= 0;
  int ret_error;
  uint temp_partitions= m_part_info->temp_partitions.elements;
  handler *file;
  partition_element *part_elem, *sub_elem;
  DBUG_ENTER("ha_partition::rename_partitions");

  /*
    Assert that it works without HA_FILE_BASED and lower_case_table_name = 2.
    We use m_file[0] as long as all partitions have the same storage engine.
  */
  DBUG_ASSERT(!strcmp(path, get_canonical_filename(m_file[0], path,
                                                   norm_name_buff)));

  DEBUG_SYNC(ha_thd(), "before_rename_partitions");
  if (temp_partitions)
  {
    /*
      These are the reorganised partitions that have already been copied.
      We delete the partitions and log the delete by inactivating the
      delete log entry in the table log. We only need to synchronise
      these writes before moving to the next loop since there is no
      interaction among reorganised partitions, they cannot have the
      same name.
    */
    do
    {
      part_elem= temp_it++;
      if (m_is_sub_partitioned)
      {
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        j= 0;
        do
        {
          sub_elem= sub_it++;
          file= m_reorged_file[part_count++];
          if (unlikely((ret_error=
                        create_subpartition_name(norm_name_buff,
                                                 sizeof(norm_name_buff), path,
                                                 part_elem->partition_name,
                                                 sub_elem->partition_name,
                                                 NORMAL_PART_NAME))))
            error= ret_error;
          DBUG_PRINT("info", ("Delete subpartition %s", norm_name_buff));
          if (unlikely((ret_error= file->ha_delete_table(norm_name_buff))))
            error= ret_error;
          else if (unlikely(deactivate_ddl_log_entry(sub_elem->log_entry->
                                                     entry_pos)))
            error= 1;
          else
            sub_elem->log_entry= NULL; /* Indicate success */
        } while (++j < num_subparts);
      }
      else
      {
        file= m_reorged_file[part_count++];
        if (unlikely((ret_error=
                      create_partition_name(norm_name_buff,
                                            sizeof(norm_name_buff), path,
                                            part_elem->partition_name,
                                            NORMAL_PART_NAME, TRUE))))
          error= ret_error;
        else
        {
          DBUG_PRINT("info", ("Delete partition %s", norm_name_buff));
          if (unlikely((ret_error= file->ha_delete_table(norm_name_buff))))
            error= ret_error;
          else if (unlikely(deactivate_ddl_log_entry(part_elem->log_entry->
                                                     entry_pos)))
            error= 1;
          else
            part_elem->log_entry= NULL; /* Indicate success */
        }
      }
    } while (++i < temp_partitions);
    (void) sync_ddl_log();
  }
  i= 0;
  do
  {
    /*
       When state is PART_IS_CHANGED it means that we have created a new
       TEMP partition that is to be renamed to normal partition name and
       we are to delete the old partition with currently the normal name.

       We perform this operation by
       1) Delete old partition with normal partition name
       2) Signal this in table log entry
       3) Synch table log to ensure we have consistency in crashes
       4) Rename temporary partition name to normal partition name
       5) Signal this to table log entry
       It is not necessary to synch the last state since a new rename
       should not corrupt things if there was no temporary partition.

       The only other parts we need to cater for are new parts that
       replace reorganised parts. The reorganised parts were deleted
       by the code above that goes through the temp_partitions list.
       Thus the synch above makes it safe to simply perform step 4 and 5
       for those entries.
    */
    part_elem= part_it++;
    if (part_elem->part_state == PART_IS_CHANGED ||
        part_elem->part_state == PART_TO_BE_DROPPED ||
        (part_elem->part_state == PART_IS_ADDED && temp_partitions))
    {
      if (m_is_sub_partitioned)
      {
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        uint part;

        j= 0;
        do
        {
          sub_elem= sub_it++;
          part= i * num_subparts + j;
          if (unlikely((ret_error=
                        create_subpartition_name(norm_name_buff,
                                                 sizeof(norm_name_buff), path,
                                                 part_elem->partition_name,
                                                 sub_elem->partition_name,
                                                 NORMAL_PART_NAME))))
            error= ret_error;
          if (part_elem->part_state == PART_IS_CHANGED)
          {
            file= m_reorged_file[part_count++];
            DBUG_PRINT("info", ("Delete subpartition %s", norm_name_buff));
            if (unlikely((ret_error= file->ha_delete_table(norm_name_buff))))
              error= ret_error;
            else if (unlikely(deactivate_ddl_log_entry(sub_elem->log_entry->
                                                       entry_pos)))
              error= 1;
            (void) sync_ddl_log();
          }
          file= m_new_file[part];
          if (unlikely((ret_error=
                        create_subpartition_name(part_name_buff,
                                                 sizeof(part_name_buff), path,
                                                 part_elem->partition_name,
                                                 sub_elem->partition_name,
                                                 TEMP_PART_NAME))))
            error= ret_error;
          DBUG_PRINT("info", ("Rename subpartition from %s to %s",
                     part_name_buff, norm_name_buff));
          if (unlikely((ret_error= file->ha_rename_table(part_name_buff,
                                                         norm_name_buff))))
            error= ret_error;
          else if (unlikely(deactivate_ddl_log_entry(sub_elem->log_entry->
                                                     entry_pos)))
            error= 1;
          else
            sub_elem->log_entry= NULL;
        } while (++j < num_subparts);
      }
      else
      {
        if (unlikely((ret_error=
                      create_partition_name(norm_name_buff,
                                            sizeof(norm_name_buff), path,
                                            part_elem->partition_name,
                                            NORMAL_PART_NAME, TRUE)) ||
                     (ret_error= create_partition_name(part_name_buff,
                                                       sizeof(part_name_buff),
                                                       path,
                                                       part_elem->
                                                       partition_name,
                                                       TEMP_PART_NAME, TRUE))))
          error= ret_error;
        else
        {
          if (part_elem->part_state == PART_IS_CHANGED)
          {
            file= m_reorged_file[part_count++];
            DBUG_PRINT("info", ("Delete partition %s", norm_name_buff));
            if (unlikely((ret_error= file->ha_delete_table(norm_name_buff))))
              error= ret_error;
            else if (unlikely(deactivate_ddl_log_entry(part_elem->log_entry->
                                                       entry_pos)))
              error= 1;
            (void) sync_ddl_log();
          }
          file= m_new_file[i];
          DBUG_PRINT("info", ("Rename partition from %s to %s",
                     part_name_buff, norm_name_buff));
          if (unlikely((ret_error= file->ha_rename_table(part_name_buff,
                                                         norm_name_buff))))
            error= ret_error;
          else if (unlikely(deactivate_ddl_log_entry(part_elem->log_entry->
                                                     entry_pos)))
            error= 1;
          else
            part_elem->log_entry= NULL;
        }
      }
    }
  } while (++i < num_parts);
  (void) sync_ddl_log();
  DBUG_RETURN(error);
}


#define OPTIMIZE_PARTS 1
#define ANALYZE_PARTS 2
#define CHECK_PARTS   3
#define REPAIR_PARTS 4
#define ASSIGN_KEYCACHE_PARTS 5
#define PRELOAD_KEYS_PARTS 6

static const char *opt_op_name[]= {NULL,
                                   "optimize", "analyze", "check", "repair",
                                   "assign_to_keycache", "preload_keys"};

/*
  Optimize table

  SYNOPSIS
    optimize()
    thd               Thread object
    check_opt         Check/analyze/repair/optimize options

  RETURN VALUES
    >0                Error
    0                 Success
*/

int ha_partition::optimize(THD *thd, HA_CHECK_OPT *check_opt)
{
  DBUG_ENTER("ha_partition::optimize");

  DBUG_RETURN(handle_opt_partitions(thd, check_opt, OPTIMIZE_PARTS));
}


/*
  Analyze table

  SYNOPSIS
    analyze()
    thd               Thread object
    check_opt         Check/analyze/repair/optimize options

  RETURN VALUES
    >0                Error
    0                 Success
*/

int ha_partition::analyze(THD *thd, HA_CHECK_OPT *check_opt)
{
  DBUG_ENTER("ha_partition::analyze");

  DBUG_RETURN(handle_opt_partitions(thd, check_opt, ANALYZE_PARTS));
}


/*
  Check table

  SYNOPSIS
    check()
    thd               Thread object
    check_opt         Check/analyze/repair/optimize options

  RETURN VALUES
    >0                Error
    0                 Success
*/

int ha_partition::check(THD *thd, HA_CHECK_OPT *check_opt)
{
  DBUG_ENTER("ha_partition::check");

  DBUG_RETURN(handle_opt_partitions(thd, check_opt, CHECK_PARTS));
}


/*
  Repair table

  SYNOPSIS
    repair()
    thd               Thread object
    check_opt         Check/analyze/repair/optimize options

  RETURN VALUES
    >0                Error
    0                 Success
*/

int ha_partition::repair(THD *thd, HA_CHECK_OPT *check_opt)
{
  DBUG_ENTER("ha_partition::repair");

  int res= handle_opt_partitions(thd, check_opt, REPAIR_PARTS);
  DBUG_RETURN(res);
}

/**
  Assign to keycache

  @param thd          Thread object
  @param check_opt    Check/analyze/repair/optimize options

  @return
    @retval >0        Error
    @retval 0         Success
*/

int ha_partition::assign_to_keycache(THD *thd, HA_CHECK_OPT *check_opt)
{
  DBUG_ENTER("ha_partition::assign_to_keycache");

  DBUG_RETURN(handle_opt_partitions(thd, check_opt, ASSIGN_KEYCACHE_PARTS));
}


/**
  Preload to keycache

  @param thd          Thread object
  @param check_opt    Check/analyze/repair/optimize options

  @return
    @retval >0        Error
    @retval 0         Success
*/

int ha_partition::preload_keys(THD *thd, HA_CHECK_OPT *check_opt)
{
  DBUG_ENTER("ha_partition::preload_keys");

  DBUG_RETURN(handle_opt_partitions(thd, check_opt, PRELOAD_KEYS_PARTS));
}


/*
  Handle optimize/analyze/check/repair of one partition

  SYNOPSIS
    handle_opt_part()
    thd                      Thread object
    check_opt                Options
    file                     Handler object of partition
    flag                     Optimize/Analyze/Check/Repair flag

  RETURN VALUE
    >0                        Failure
    0                         Success
*/

int ha_partition::handle_opt_part(THD *thd, HA_CHECK_OPT *check_opt,
                                  uint part_id, uint flag)
{
  int error;
  handler *file= m_file[part_id];
  DBUG_ENTER("handle_opt_part");
  DBUG_PRINT("enter", ("flag: %u", flag));

  if (flag == OPTIMIZE_PARTS)
    error= file->ha_optimize(thd, check_opt);
  else if (flag == ANALYZE_PARTS)
    error= file->ha_analyze(thd, check_opt);
  else if (flag == CHECK_PARTS)
  {
    error= file->ha_check(thd, check_opt);
    if (!error ||
        error == HA_ADMIN_ALREADY_DONE ||
        error == HA_ADMIN_NOT_IMPLEMENTED)
    {
      if (check_opt->flags & (T_MEDIUM | T_EXTEND))
        error= check_misplaced_rows(part_id, false);
    }
  }
  else if (flag == REPAIR_PARTS)
  {
    error= file->ha_repair(thd, check_opt);
    if (!error ||
        error == HA_ADMIN_ALREADY_DONE ||
        error == HA_ADMIN_NOT_IMPLEMENTED)
    {
      if (check_opt->flags & (T_MEDIUM | T_EXTEND))
        error= check_misplaced_rows(part_id, true);
    }
  }
  else if (flag == ASSIGN_KEYCACHE_PARTS)
    error= file->assign_to_keycache(thd, check_opt);
  else if (flag == PRELOAD_KEYS_PARTS)
    error= file->preload_keys(thd, check_opt);
  else
  {
    DBUG_ASSERT(FALSE);
    error= 1;
  }
  if (error == HA_ADMIN_ALREADY_DONE)
    error= 0;
  DBUG_RETURN(error);
}


/*
   print a message row formatted for ANALYZE/CHECK/OPTIMIZE/REPAIR TABLE
   (modelled after mi_check_print_msg)
   TODO: move this into the handler, or rewrite mysql_admin_table.
*/
bool print_admin_msg(THD* thd, uint len,
                            const char* msg_type,
                            const char* db_name, String &table_name,
                            const char* op_name, const char *fmt, ...)
  ATTRIBUTE_FORMAT(printf, 7, 8);
bool print_admin_msg(THD* thd, uint len,
                            const char* msg_type,
                            const char* db_name, String &table_name,
                            const char* op_name, const char *fmt, ...)
{
  va_list args;
  Protocol *protocol= thd->protocol;
  size_t length;
  size_t msg_length;
  char name[NAME_LEN*2+2];
  char *msgbuf;
  bool error= true;

  if (!(msgbuf= (char*) my_malloc(len, MYF(0))))
    return true;
  va_start(args, fmt);
  msg_length= my_vsnprintf(msgbuf, len, fmt, args);
  va_end(args);
  if (msg_length >= (len - 1))
    goto err;
  msgbuf[len - 1]= 0; // healthy paranoia


  if (!thd->vio_ok())
  {
    sql_print_error("%s", msgbuf);
    goto err;
  }

  length=(size_t)(strxmov(name, db_name, ".", table_name.c_ptr_safe(), NullS) - name);
  /*
     TODO: switch from protocol to push_warning here. The main reason we didn't
     it yet is parallel repair, which threads have no THD object accessible via
     current_thd.

     Also we likely need to lock mutex here (in both cases with protocol and
     push_warning).
  */
  DBUG_PRINT("info",("print_admin_msg:  %s, %s, %s, %s", name, op_name,
                     msg_type, msgbuf));
  protocol->prepare_for_resend();
  protocol->store(name, length, system_charset_info);
  protocol->store(op_name, system_charset_info);
  protocol->store(msg_type, system_charset_info);
  protocol->store(msgbuf, msg_length, system_charset_info);
  if (protocol->write())
  {
    sql_print_error("Failed on my_net_write, writing to stderr instead: %s\n",
                    msgbuf);
    goto err;
  }
  error= false;
err:
  my_free(msgbuf);
  return error;
}


/*
  Handle optimize/analyze/check/repair of partitions

  SYNOPSIS
    handle_opt_partitions()
    thd                      Thread object
    check_opt                Options
    flag                     Optimize/Analyze/Check/Repair flag

  RETURN VALUE
    >0                        Failure
    0                         Success
*/

int ha_partition::handle_opt_partitions(THD *thd, HA_CHECK_OPT *check_opt,
                                        uint flag)
{
  List_iterator<partition_element> part_it(m_part_info->partitions);
  uint num_parts= m_part_info->num_parts;
  uint num_subparts= m_part_info->num_subparts;
  uint i= 0;
  int error;
  DBUG_ENTER("ha_partition::handle_opt_partitions");
  DBUG_PRINT("enter", ("flag= %u", flag));

  do
  {
    partition_element *part_elem= part_it++;
    /*
      when ALTER TABLE <CMD> PARTITION ...
      it should only do named partitions, otherwise all partitions
    */
    if (!(thd->lex->alter_info.partition_flags & ALTER_PARTITION_ADMIN) ||
        part_elem->part_state == PART_ADMIN)
    {
      if (m_is_sub_partitioned)
      {
        List_iterator<partition_element> subpart_it(part_elem->subpartitions);
        partition_element *sub_elem;
        uint j= 0, part;
        do
        {
          sub_elem= subpart_it++;
          part= i * num_subparts + j;
          DBUG_PRINT("info", ("Optimize subpartition %u (%s)",
                     part, sub_elem->partition_name));
          if (unlikely((error= handle_opt_part(thd, check_opt, part, flag))))
          {
            /* print a line which partition the error belongs to */
            if (error != HA_ADMIN_NOT_IMPLEMENTED &&
                error != HA_ADMIN_ALREADY_DONE &&
                error != HA_ADMIN_TRY_ALTER)
            {
	      print_admin_msg(thd, MYSQL_ERRMSG_SIZE, "error",
                              table_share->db.str, table->alias,
                              opt_op_name[flag],
                              "Subpartition %s returned error",
                              sub_elem->partition_name);
            }
            /* reset part_state for the remaining partitions */
            do
            {
              if (part_elem->part_state == PART_ADMIN)
                part_elem->part_state= PART_NORMAL;
            } while ((part_elem= part_it++));
            DBUG_RETURN(error);
          }
        } while (++j < num_subparts);
      }
      else
      {
        DBUG_PRINT("info", ("Optimize partition %u (%s)", i,
                            part_elem->partition_name));
        if (unlikely((error= handle_opt_part(thd, check_opt, i, flag))))
        {
          /* print a line which partition the error belongs to */
          if (error != HA_ADMIN_NOT_IMPLEMENTED &&
              error != HA_ADMIN_ALREADY_DONE &&
              error != HA_ADMIN_TRY_ALTER)
          {
	    print_admin_msg(thd, MYSQL_ERRMSG_SIZE, "error",
                            table_share->db.str, table->alias,
                            opt_op_name[flag], "Partition %s returned error",
                            part_elem->partition_name);
          }
          /* reset part_state for the remaining partitions */
          do
          {
            if (part_elem->part_state == PART_ADMIN)
              part_elem->part_state= PART_NORMAL;
          } while ((part_elem= part_it++));
          DBUG_RETURN(error);
        }
      }
      part_elem->part_state= PART_NORMAL;
    }
  } while (++i < num_parts);
  DBUG_RETURN(FALSE);
}


/**
  @brief Check and repair the table if neccesary

  @param thd    Thread object

  @retval TRUE  Error/Not supported
  @retval FALSE Success

  @note Called if open_table_from_share fails and ::is_crashed().
*/

bool ha_partition::check_and_repair(THD *thd)
{
  handler **file= m_file;
  DBUG_ENTER("ha_partition::check_and_repair");

  do
  {
    if ((*file)->ha_check_and_repair(thd))
      DBUG_RETURN(TRUE);
  } while (*(++file));
  DBUG_RETURN(FALSE);
}


/**
  @breif Check if the table can be automatically repaired

  @retval TRUE  Can be auto repaired
  @retval FALSE Cannot be auto repaired
*/

bool ha_partition::auto_repair(int error) const
{
  DBUG_ENTER("ha_partition::auto_repair");

  /*
    As long as we only support one storage engine per table,
    we can use the first partition for this function.
  */
  DBUG_RETURN(m_file[0]->auto_repair(error));
}


/**
  @breif Check if the table is crashed

  @retval TRUE  Crashed
  @retval FALSE Not crashed
*/

bool ha_partition::is_crashed() const
{
  handler **file= m_file;
  DBUG_ENTER("ha_partition::is_crashed");

  do
  {
    if ((*file)->is_crashed())
      DBUG_RETURN(TRUE);
  } while (*(++file));
  DBUG_RETURN(FALSE);
}


/*
  Prepare by creating a new partition

  SYNOPSIS
    prepare_new_partition()
    table                      Table object
    create_info                Create info from CREATE TABLE
    file                       Handler object of new partition
    part_name                  partition name

  RETURN VALUE
    >0                         Error
    0                          Success
*/

int ha_partition::prepare_new_partition(TABLE *tbl,
                                        HA_CREATE_INFO *create_info,
                                        handler *file, const char *part_name,
                                        partition_element *p_elem,
                                        uint disable_non_uniq_indexes)
{
  int error;
  DBUG_ENTER("prepare_new_partition");

  /*
    This call to set_up_table_before_create() is done for an alter table.
    So this may be the second time around for this partition_element,
    depending on how many partitions and subpartitions there were before,
    and how many there are now.
    The first time, on the CREATE, data_file_name and index_file_name
    came from the parser.  They did not have the file name attached to
    the end.  But if this partition is less than the total number of
    previous partitions, it's data_file_name has the filename attached.
    So we need to take the partition filename off if it exists.
    That file name may be different from part_name, which will be
    attached in append_file_to_dir().
  */
  truncate_partition_filename((char*) p_elem->data_file_name);
  truncate_partition_filename((char*) p_elem->index_file_name);

  if (unlikely((error= set_up_table_before_create(tbl, part_name, create_info,
                                                  p_elem))))
    goto error_create;

  if (!(file->ht->flags & HTON_CAN_READ_CONNECT_STRING_IN_PARTITION))
    tbl->s->connect_string= p_elem->connect_string;
  if ((error= file->ha_create(part_name, tbl, create_info)))
  {
    /*
      Added for safety, InnoDB reports HA_ERR_FOUND_DUPP_KEY
      if the table/partition already exists.
      If we return that error code, then print_error would try to
      get_dup_key on a non-existing partition.
      So return a more reasonable error code.
    */
    if (error == HA_ERR_FOUND_DUPP_KEY)
      error= HA_ERR_TABLE_EXIST;
    goto error_create;
  }
  DBUG_PRINT("info", ("partition %s created", part_name));
  if (unlikely((error= file->ha_open(tbl, part_name, m_mode,
                                     m_open_test_lock | HA_OPEN_NO_PSI_CALL))))
    goto error_open;
  DBUG_PRINT("info", ("partition %s opened", part_name));

  /*
    Note: if you plan to add another call that may return failure,
    better to do it before external_lock() as cleanup_new_partition()
    assumes that external_lock() is last call that may fail here.
    Otherwise see description for cleanup_new_partition().
  */
  if (unlikely((error= file->ha_external_lock(ha_thd(), F_WRLCK))))
    goto error_external_lock;
  DBUG_PRINT("info", ("partition %s external locked", part_name));

  if (disable_non_uniq_indexes)
    file->ha_disable_indexes(HA_KEY_SWITCH_NONUNIQ_SAVE);

  DBUG_RETURN(0);
error_external_lock:
  (void) file->ha_close();
error_open:
  (void) file->ha_delete_table(part_name);
error_create:
  DBUG_RETURN(error);
}


/*
  Cleanup by removing all created partitions after error

  SYNOPSIS
    cleanup_new_partition()
    part_count             Number of partitions to remove

  RETURN VALUE
    NONE

  DESCRIPTION
    This function is called immediately after prepare_new_partition() in
    case the latter fails.

    In prepare_new_partition() last call that may return failure is
    external_lock(). That means if prepare_new_partition() fails,
    partition does not have external lock. Thus no need to call
    external_lock(F_UNLCK) here.

  TODO:
    We must ensure that in the case that we get an error during the process
    that we call external_lock with F_UNLCK, close the table and delete the
    table in the case where we have been successful with prepare_handler.
    We solve this by keeping an array of successful calls to prepare_handler
    which can then be used to undo the call.
*/

void ha_partition::cleanup_new_partition(uint part_count)
{
  DBUG_ENTER("ha_partition::cleanup_new_partition");

  if (m_added_file)
  {
    THD *thd= ha_thd();
    handler **file= m_added_file;
    while ((part_count > 0) && (*file))
    {
      (*file)->ha_external_lock(thd, F_UNLCK);
      (*file)->ha_close();

      /* Leave the (*file)->ha_delete_table(part_name) to the ddl-log */

      file++;
      part_count--;
    }
    m_added_file= NULL;
  }
  DBUG_VOID_RETURN;
}

/*
  Implement the partition changes defined by ALTER TABLE of partitions

  SYNOPSIS
    change_partitions()
    create_info                 HA_CREATE_INFO object describing all
                                fields and indexes in table
    path                        Complete path of db and table name
    out: copied                 Output parameter where number of copied
                                records are added
    out: deleted                Output parameter where number of deleted
                                records are added
    pack_frm_data               Reference to packed frm file
    pack_frm_len                Length of packed frm file

  RETURN VALUE
    >0                        Failure
    0                         Success

  DESCRIPTION
    Add and copy if needed a number of partitions, during this operation
    no other operation is ongoing in the server. This is used by
    ADD PARTITION all types as well as by REORGANIZE PARTITION. For
    one-phased implementations it is used also by DROP and COALESCE
    PARTITIONs.
    One-phased implementation needs the new frm file, other handlers will
    get zero length and a NULL reference here.
*/

int ha_partition::change_partitions(HA_CREATE_INFO *create_info,
                                    const char *path,
                                    ulonglong * const copied,
                                    ulonglong * const deleted,
                                    const uchar *pack_frm_data
                                    __attribute__((unused)),
                                    size_t pack_frm_len
                                    __attribute__((unused)))
{
  List_iterator<partition_element> part_it(m_part_info->partitions);
  List_iterator <partition_element> t_it(m_part_info->temp_partitions);
  char part_name_buff[FN_REFLEN + 1];
  uint num_parts= m_part_info->partitions.elements;
  uint num_subparts= m_part_info->num_subparts;
  uint i= 0;
  uint num_remain_partitions, part_count, orig_count;
  handler **new_file_array;
  int error= 1;
  bool first;
  uint temp_partitions= m_part_info->temp_partitions.elements;
  THD *thd= ha_thd();
  DBUG_ENTER("ha_partition::change_partitions");

  /*
    Assert that it works without HA_FILE_BASED and lower_case_table_name = 2.
    We use m_file[0] as long as all partitions have the same storage engine.
  */
  DBUG_ASSERT(!strcmp(path, get_canonical_filename(m_file[0], path,
                                                   part_name_buff)));
  m_reorged_parts= 0;
  if (!m_part_info->is_sub_partitioned())
    num_subparts= 1;

  /*
    Step 1:
      Calculate number of reorganised partitions and allocate space for
      their handler references.
  */
  if (temp_partitions)
  {
    m_reorged_parts= temp_partitions * num_subparts;
  }
  else
  {
    do
    {
      partition_element *part_elem= part_it++;
      if (part_elem->part_state == PART_CHANGED ||
          part_elem->part_state == PART_REORGED_DROPPED)
      {
        m_reorged_parts+= num_subparts;
      }
    } while (++i < num_parts);
  }
  if (m_reorged_parts &&
      !(m_reorged_file= (handler**) thd->calloc(sizeof(handler*)*
                                                (m_reorged_parts + 1))))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  /*
    Step 2:
      Calculate number of partitions after change and allocate space for
      their handler references.
  */
  num_remain_partitions= 0;
  if (temp_partitions)
  {
    num_remain_partitions= num_parts * num_subparts;
  }
  else
  {
    part_it.rewind();
    i= 0;
    do
    {
      partition_element *part_elem= part_it++;
      if (part_elem->part_state == PART_NORMAL ||
          part_elem->part_state == PART_TO_BE_ADDED ||
          part_elem->part_state == PART_CHANGED)
      {
        num_remain_partitions+= num_subparts;
      }
    } while (++i < num_parts);
  }
  if (!(new_file_array= ((handler**)
                         thd->calloc(sizeof(handler*)*
                                     (2*(num_remain_partitions + 1))))))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  m_added_file= &new_file_array[num_remain_partitions + 1];

  /*
    Step 3:
      Fill m_reorged_file with handler references and NULL at the end
  */
  if (m_reorged_parts)
  {
    i= 0;
    part_count= 0;
    first= TRUE;
    part_it.rewind();
    do
    {
      partition_element *part_elem= part_it++;
      if (part_elem->part_state == PART_CHANGED ||
          part_elem->part_state == PART_REORGED_DROPPED)
      {
        memcpy((void*)&m_reorged_file[part_count],
               (void*)&m_file[i*num_subparts],
               sizeof(handler*)*num_subparts);
        part_count+= num_subparts;
      }
      else if (first && temp_partitions &&
               part_elem->part_state == PART_TO_BE_ADDED)
      {
        /*
          When doing an ALTER TABLE REORGANIZE PARTITION a number of
          partitions is to be reorganised into a set of new partitions.
          The reorganised partitions are in this case in the temp_partitions
          list. We copy all of them in one batch and thus we only do this
          until we find the first partition with state PART_TO_BE_ADDED
          since this is where the new partitions go in and where the old
          ones used to be.
        */
        first= FALSE;
        DBUG_ASSERT(((i*num_subparts) + m_reorged_parts) <= m_file_tot_parts);
        memcpy((void*)m_reorged_file, &m_file[i*num_subparts],
               sizeof(handler*)*m_reorged_parts);
      }
    } while (++i < num_parts);
  }

  /*
    Step 4:
      Fill new_array_file with handler references. Create the handlers if
      needed.
  */
  i= 0;
  part_count= 0;
  orig_count= 0;
  first= TRUE;
  part_it.rewind();
  do
  {
    partition_element *part_elem= part_it++;
    if (part_elem->part_state == PART_NORMAL)
    {
      DBUG_ASSERT(orig_count + num_subparts <= m_file_tot_parts);
      memcpy((void*)&new_file_array[part_count], (void*)&m_file[orig_count],
             sizeof(handler*)*num_subparts);
      part_count+= num_subparts;
      orig_count+= num_subparts;
    }
    else if (part_elem->part_state == PART_CHANGED ||
             part_elem->part_state == PART_TO_BE_ADDED)
    {
      uint j= 0;
      Parts_share_refs *p_share_refs;
      /*
        The Handler_shares for each partition's handler can be allocated
        within this handler, since there will not be any more instances of the
        new partitions, until the table is reopened after the ALTER succeeded.
      */
      p_share_refs= new Parts_share_refs;
      if (!p_share_refs)
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      if (p_share_refs->init(num_subparts))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      if (m_new_partitions_share_refs.push_back(p_share_refs, thd->mem_root))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      do
      {
        handler **new_file= &new_file_array[part_count++];
        if (!(*new_file=
              get_new_handler(table->s,
                              thd->mem_root,
                              part_elem->engine_type)))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
        if ((*new_file)->set_ha_share_ref(&p_share_refs->ha_shares[j]))
        {
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        }
      } while (++j < num_subparts);
      if (part_elem->part_state == PART_CHANGED)
        orig_count+= num_subparts;
      else if (temp_partitions && first)
      {
        orig_count+= (num_subparts * temp_partitions);
        first= FALSE;
      }
    }
  } while (++i < num_parts);
  first= FALSE;
  /*
    Step 5:
      Create the new partitions and also open, lock and call external_lock
      on them to prepare them for copy phase and also for later close
      calls
  */

  /*
     Before creating new partitions check whether indexes are disabled
     in the  partitions.
  */

  uint disable_non_uniq_indexes= indexes_are_disabled();

  i= 0;
  part_count= 0;
  part_it.rewind();
  do
  {
    partition_element *part_elem= part_it++;
    if (part_elem->part_state == PART_TO_BE_ADDED ||
        part_elem->part_state == PART_CHANGED)
    {
      /*
        A new partition needs to be created PART_TO_BE_ADDED means an
        entirely new partition and PART_CHANGED means a changed partition
        that will still exist with either more or less data in it.
      */
      uint name_variant= NORMAL_PART_NAME;
      if (part_elem->part_state == PART_CHANGED ||
          (part_elem->part_state == PART_TO_BE_ADDED && temp_partitions))
        name_variant= TEMP_PART_NAME;
      if (m_part_info->is_sub_partitioned())
      {
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        uint j= 0, part;
        do
        {
          partition_element *sub_elem= sub_it++;
          if (unlikely((error=
                        create_subpartition_name(part_name_buff,
                                                 sizeof(part_name_buff), path,
                                                 part_elem->partition_name,
                                                 sub_elem->partition_name,
                                                 name_variant))))
          {
            cleanup_new_partition(part_count);
            DBUG_RETURN(error);
          }
          part= i * num_subparts + j;
          DBUG_PRINT("info", ("Add subpartition %s", part_name_buff));
          if (unlikely((error=
                        prepare_new_partition(table, create_info,
                                              new_file_array[part],
                                              (const char *)part_name_buff,
                                              sub_elem,
                                              disable_non_uniq_indexes))))
          {
            cleanup_new_partition(part_count);
            DBUG_RETURN(error);
          }

          m_added_file[part_count++]= new_file_array[part];
        } while (++j < num_subparts);
      }
      else
      {
        if (unlikely((error=
                      create_partition_name(part_name_buff,
                                            sizeof(part_name_buff), path,
                                            part_elem->partition_name,
                                            name_variant, TRUE))))
        {
          cleanup_new_partition(part_count);
          DBUG_RETURN(error);
        }

        DBUG_PRINT("info", ("Add partition %s", part_name_buff));
        if (unlikely((error=
                      prepare_new_partition(table, create_info,
                                            new_file_array[i],
                                            (const char *)part_name_buff,
                                            part_elem,
                                            disable_non_uniq_indexes))))
        {
          cleanup_new_partition(part_count);
          DBUG_RETURN(error);
        }

        m_added_file[part_count++]= new_file_array[i];
      }
    }
  } while (++i < num_parts);

  /*
    Step 6:
      State update to prepare for next write of the frm file.
  */
  i= 0;
  part_it.rewind();
  do
  {
    partition_element *part_elem= part_it++;
    if (part_elem->part_state == PART_TO_BE_ADDED)
      part_elem->part_state= PART_IS_ADDED;
    else if (part_elem->part_state == PART_CHANGED)
      part_elem->part_state= PART_IS_CHANGED;
    else if (part_elem->part_state == PART_REORGED_DROPPED)
      part_elem->part_state= PART_TO_BE_DROPPED;
  } while (++i < num_parts);
  for (i= 0; i < temp_partitions; i++)
  {
    partition_element *part_elem= t_it++;
    DBUG_ASSERT(part_elem->part_state == PART_TO_BE_REORGED);
    part_elem->part_state= PART_TO_BE_DROPPED;
  }
  m_new_file= new_file_array;
  if (unlikely((error= copy_partitions(copied, deleted))))
  {
    /*
      Close and unlock the new temporary partitions.
      They will later be deleted through the ddl-log.
    */
    cleanup_new_partition(part_count);
  }
  DBUG_RETURN(error);
}


/*
  Copy partitions as part of ALTER TABLE of partitions

  SYNOPSIS
    copy_partitions()
    out:copied                 Number of records copied
    out:deleted                Number of records deleted

  RETURN VALUE
    >0                         Error code
    0                          Success

  DESCRIPTION
    change_partitions has done all the preparations, now it is time to
    actually copy the data from the reorganised partitions to the new
    partitions.
*/

int ha_partition::copy_partitions(ulonglong * const copied,
                                  ulonglong * const deleted)
{
  uint reorg_part= 0;
  int result= 0;
  longlong func_value;
  DBUG_ENTER("ha_partition::copy_partitions");

  if (m_part_info->linear_hash_ind)
  {
    if (m_part_info->part_type == HASH_PARTITION)
      set_linear_hash_mask(m_part_info, m_part_info->num_parts);
    else
      set_linear_hash_mask(m_part_info, m_part_info->num_subparts);
  }
  else if (m_part_info->part_type == VERSIONING_PARTITION)
  {
    if (m_part_info->check_constants(ha_thd(), m_part_info))
      goto init_error;
  }

  while (reorg_part < m_reorged_parts)
  {
    handler *file= m_reorged_file[reorg_part];
    uint32 new_part;

    late_extra_cache(reorg_part);
    if (unlikely((result= file->ha_rnd_init_with_error(1))))
      goto init_error;
    while (TRUE)
    {
      if ((result= file->ha_rnd_next(m_rec0)))
      {
        if (result != HA_ERR_END_OF_FILE)
          goto error;
        /*
          End-of-file reached, break out to continue with next partition or
          end the copy process.
        */
        break;
      }
      /* Found record to insert into new handler */
      if (m_part_info->get_partition_id(m_part_info, &new_part,
                                        &func_value))
      {
        /*
           This record is in the original table but will not be in the new
           table since it doesn't fit into any partition any longer due to
           changed partitioning ranges or list values.
        */
        (*deleted)++;
      }
      else
      {
        THD *thd= ha_thd();
        /* Copy record to new handler */
        (*copied)++;
        tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
        result= m_new_file[new_part]->ha_write_row(m_rec0);
        reenable_binlog(thd);
        if (result)
          goto error;
      }
    }
    late_extra_no_cache(reorg_part);
    file->ha_rnd_end();
    reorg_part++;
  }
  DBUG_RETURN(FALSE);
error:
  m_reorged_file[reorg_part]->ha_rnd_end();
init_error:
  DBUG_RETURN(result);
}

/*
  Update create info as part of ALTER TABLE

  SYNOPSIS
    update_create_info()
    create_info                   Create info from ALTER TABLE

  RETURN VALUE
    NONE

  DESCRIPTION
  Forward this handler call to the storage engine foreach
  partition handler.  The data_file_name for each partition may
  need to be reset if the tablespace was moved.  Use a dummy
  HA_CREATE_INFO structure and transfer necessary data.
*/

void ha_partition::update_create_info(HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_partition::update_create_info");

  /*
    Fix for bug#38751, some engines needs info-calls in ALTER.
    Archive need this since it flushes in ::info.
    HA_STATUS_AUTO is optimized so it will not always be forwarded
    to all partitions, but HA_STATUS_VARIABLE will.
  */
  info(HA_STATUS_VARIABLE | HA_STATUS_OPEN);

  info(HA_STATUS_AUTO);

  if (!(create_info->used_fields & HA_CREATE_USED_AUTO))
    create_info->auto_increment_value= stats.auto_increment_value;

  /*
    DATA DIRECTORY and INDEX DIRECTORY are never applied to the whole
    partitioned table, only its parts.
  */
  my_bool from_alter= (create_info->data_file_name == (const char*) -1);
  create_info->data_file_name= create_info->index_file_name= NULL;

  if (!(m_file[0]->ht->flags & HTON_CAN_READ_CONNECT_STRING_IN_PARTITION))
    create_info->connect_string= null_clex_str;

  /*
    We do not need to update the individual partition DATA DIRECTORY settings
    since they can be changed by ALTER TABLE ... REORGANIZE PARTITIONS.
  */
  if (from_alter)
    DBUG_VOID_RETURN;

  /*
    send Handler::update_create_info() to the storage engine for each
    partition that currently has a handler object.  Using a dummy
    HA_CREATE_INFO structure to collect DATA and INDEX DIRECTORYs.
  */

  List_iterator<partition_element> part_it(m_part_info->partitions);
  partition_element *part_elem, *sub_elem;
  uint num_subparts= m_part_info->num_subparts;
  uint num_parts= (num_subparts ? m_file_tot_parts / num_subparts :
                   m_file_tot_parts);
  HA_CREATE_INFO dummy_info;
  memset(&dummy_info, 0, sizeof(dummy_info));

  /*
    Since update_create_info() can be called from mysql_prepare_alter_table()
    when not all handlers are set up, we look for that condition first.
    If all handlers are not available, do not call update_create_info for any.
  */
  uint i, j, part;
  for (i= 0; i < num_parts; i++)
  {
    part_elem= part_it++;
    if (!part_elem)
      DBUG_VOID_RETURN;
    if (m_is_sub_partitioned)
    {
      List_iterator<partition_element> subpart_it(part_elem->subpartitions);
      for (j= 0; j < num_subparts; j++)
      {
        sub_elem= subpart_it++;
        if (!sub_elem)
          DBUG_VOID_RETURN;
        part= i * num_subparts + j;
        if (part >= m_file_tot_parts || !m_file[part])
          DBUG_VOID_RETURN;
      }
    }
    else
    {
      if (!m_file[i])
        DBUG_VOID_RETURN;
    }
  }
  part_it.rewind();

  for (i= 0; i < num_parts; i++)
  {
    part_elem= part_it++;
    DBUG_ASSERT(part_elem);
    if (m_is_sub_partitioned)
    {
      List_iterator<partition_element> subpart_it(part_elem->subpartitions);
      for (j= 0; j < num_subparts; j++)
      {
        sub_elem= subpart_it++;
        DBUG_ASSERT(sub_elem);
        part= i * num_subparts + j;
        DBUG_ASSERT(part < m_file_tot_parts && m_file[part]);
        dummy_info.data_file_name= dummy_info.index_file_name = NULL;
        m_file[part]->update_create_info(&dummy_info);
        sub_elem->data_file_name = (char*) dummy_info.data_file_name;
        sub_elem->index_file_name = (char*) dummy_info.index_file_name;
      }
    }
    else
    {
      DBUG_ASSERT(m_file[i]);
      dummy_info.data_file_name= dummy_info.index_file_name= NULL;
      m_file[i]->update_create_info(&dummy_info);
      part_elem->data_file_name = (char*) dummy_info.data_file_name;
      part_elem->index_file_name = (char*) dummy_info.index_file_name;
    }
  }
  DBUG_VOID_RETURN;
}


/**
  Change the internal TABLE_SHARE pointer

  @param table_arg    TABLE object
  @param share        New share to use

  @note Is used in error handling in ha_delete_table.
  All handlers should exist (lock_partitions should not be used)
*/

void ha_partition::change_table_ptr(TABLE *table_arg, TABLE_SHARE *share)
{
  handler **file_array;
  table= table_arg;
  table_share= share;
  /*
    m_file can be NULL when using an old cached table in DROP TABLE, when the
    table just has REMOVED PARTITIONING, see Bug#42438
  */
  if (m_file)
  {
    file_array= m_file;
    DBUG_ASSERT(*file_array);
    do
    {
      (*file_array)->change_table_ptr(table_arg, share);
    } while (*(++file_array));
  }

  if (m_added_file && m_added_file[0])
  {
    /* if in middle of a drop/rename etc */
    file_array= m_added_file;
    do
    {
      (*file_array)->change_table_ptr(table_arg, share);
    } while (*(++file_array));
  }
}

/*
  Change comments specific to handler

  SYNOPSIS
    update_table_comment()
    comment                       Original comment

  RETURN VALUE
    new comment

  DESCRIPTION
    No comment changes so far
*/

char *ha_partition::update_table_comment(const char *comment)
{
  return (char*) comment;                       /* Nothing to change */
}


/**
  Handle delete and rename table

    @param from         Full path of old table
    @param to           Full path of new table

  @return Operation status
    @retval >0  Error
    @retval 0   Success

  @note  Common routine to handle delete_table and rename_table.
  The routine uses the partition handler file to get the
  names of the partition instances. Both these routines
  are called after creating the handler without table
  object and thus the file is needed to discover the
  names of the partitions and the underlying storage engines.
*/

uint ha_partition::del_ren_table(const char *from, const char *to)
{
  int save_error= 0;
  int error;
  char from_buff[FN_REFLEN + 1], to_buff[FN_REFLEN + 1],
       from_lc_buff[FN_REFLEN], to_lc_buff[FN_REFLEN];
  char *name_buffer_ptr;
  const char *from_path;
  const char *to_path= NULL;
  uint i;
  handler **file, **abort_file;
  DBUG_ENTER("ha_partition::del_ren_table");

  if (get_from_handler_file(from, ha_thd()->mem_root, false))
    DBUG_RETURN(TRUE);
  DBUG_ASSERT(m_file_buffer);
  DBUG_PRINT("enter", ("from: (%s) to: (%s)", from, to ? to : "(nil)"));
  name_buffer_ptr= m_name_buffer_ptr;
  file= m_file;
  if (to == NULL)
  {
    /*
      Delete table, start by delete the .par file. If error, break, otherwise
      delete as much as possible.
    */
    if (unlikely((error= handler::delete_table(from))))
      DBUG_RETURN(error);
  }
  /*
    Since ha_partition has HA_FILE_BASED, it must alter underlying table names
    if they do not have HA_FILE_BASED and lower_case_table_names == 2.
    See Bug#37402, for Mac OS X.
    The appended #P#<partname>[#SP#<subpartname>] will remain in current case.
    Using the first partitions handler, since mixing handlers is not allowed.
  */
  from_path= get_canonical_filename(*file, from, from_lc_buff);
  if (to != NULL)
    to_path= get_canonical_filename(*file, to, to_lc_buff);
  i= 0;
  do
  {
    if (unlikely((error= create_partition_name(from_buff, sizeof(from_buff),
                                               from_path, name_buffer_ptr,
                                               NORMAL_PART_NAME, FALSE))))
      goto rename_error;

    if (to != NULL)
    {                                           // Rename branch
      if (unlikely((error= create_partition_name(to_buff, sizeof(to_buff),
                                                 to_path, name_buffer_ptr,
                                                 NORMAL_PART_NAME, FALSE))))
        goto rename_error;
      error= (*file)->ha_rename_table(from_buff, to_buff);
      if (unlikely(error))
        goto rename_error;
    }
    else                                        // delete branch
    {
      error= (*file)->ha_delete_table(from_buff);
    }
    name_buffer_ptr= strend(name_buffer_ptr) + 1;
    if (unlikely(error))
      save_error= error;
    i++;
  } while (*(++file));
  if (to != NULL)
  {
    if (unlikely((error= handler::rename_table(from, to))))
    {
      /* Try to revert everything, ignore errors */
      (void) handler::rename_table(to, from);
      goto rename_error;
    }
  }
  DBUG_RETURN(save_error);
rename_error:
  name_buffer_ptr= m_name_buffer_ptr;
  for (abort_file= file, file= m_file; file < abort_file; file++)
  {
    /* Revert the rename, back from 'to' to the original 'from' */
    if (!create_partition_name(from_buff, sizeof(from_buff), from_path,
                               name_buffer_ptr, NORMAL_PART_NAME, FALSE) &&
        !create_partition_name(to_buff, sizeof(to_buff), to_path,
                               name_buffer_ptr, NORMAL_PART_NAME, FALSE))
    {
      /* Ignore error here */
      (void) (*file)->ha_rename_table(to_buff, from_buff);
    }
    name_buffer_ptr= strend(name_buffer_ptr) + 1;
  }
  DBUG_RETURN(error);
}

uint ha_partition::count_query_cache_dependant_tables(uint8 *tables_type)
{
  DBUG_ENTER("ha_partition::count_query_cache_dependant_tables");
  /* Here we rely on the fact that all tables are of the same type */
  uint8 type= m_file[0]->table_cache_type();
  (*tables_type)|= type;
  DBUG_PRINT("enter", ("cnt: %u", (uint) m_tot_parts));
  /*
    We need save underlying tables only for HA_CACHE_TBL_ASKTRANSACT:
    HA_CACHE_TBL_NONTRANSACT - because all changes goes through partition table
    HA_CACHE_TBL_NOCACHE - because will not be cached
    HA_CACHE_TBL_TRANSACT - QC need to know that such type present
  */
  DBUG_RETURN(type == HA_CACHE_TBL_ASKTRANSACT ? m_tot_parts : 0);
}

my_bool ha_partition::
reg_query_cache_dependant_table(THD *thd,
                                char *engine_key, uint engine_key_len,
                                char *cache_key, uint cache_key_len,
                                uint8 type,
                                Query_cache *cache,
                                Query_cache_block_table **block_table,
                                handler *file,
                                uint *n)
{
  DBUG_ENTER("ha_partition::reg_query_cache_dependant_table");
  qc_engine_callback engine_callback;
  ulonglong engine_data;
  /* ask undelying engine */
  if (!file->register_query_cache_table(thd, engine_key,
                                        engine_key_len,
                                        &engine_callback,
                                        &engine_data))
  {
    DBUG_PRINT("qcache", ("Handler does not allow caching for %.*s",
                          engine_key_len, engine_key));
    /*
      As this can change from call to call, don't reset set
      thd->lex->safe_to_cache_query
    */
    thd->query_cache_is_applicable= 0;        // Query can't be cached
    DBUG_RETURN(TRUE);
  }
  (++(*block_table))->n= ++(*n);
  if (!cache->insert_table(thd, cache_key_len,
                           cache_key, (*block_table),
                           (uint32) table_share->db.length,
                           (uint8) (cache_key_len -
                                    table_share->table_cache_key.length),
                           type,
                           engine_callback, engine_data,
                           FALSE))
    DBUG_RETURN(TRUE);
  DBUG_RETURN(FALSE);
}


my_bool ha_partition::
register_query_cache_dependant_tables(THD *thd,
                                      Query_cache *cache,
                                      Query_cache_block_table **block_table,
                                      uint *n)
{
  char *engine_key_end, *query_cache_key_end;
  uint i;
  uint num_parts= m_part_info->num_parts;
  uint num_subparts= m_part_info->num_subparts;
  int diff_length;
  List_iterator<partition_element> part_it(m_part_info->partitions);
  char engine_key[FN_REFLEN], query_cache_key[FN_REFLEN];
  DBUG_ENTER("ha_partition::register_query_cache_dependant_tables");

  /* see ha_partition::count_query_cache_dependant_tables */
  if (m_file[0]->table_cache_type() != HA_CACHE_TBL_ASKTRANSACT)
    DBUG_RETURN(FALSE); // nothing to register

  /* prepare static part of the key */
  memcpy(engine_key, table_share->normalized_path.str,
         table_share->normalized_path.length);
  memcpy(query_cache_key, table_share->table_cache_key.str,
         table_share->table_cache_key.length);

  diff_length= ((int) table_share->table_cache_key.length -
                (int) table_share->normalized_path.length -1);

  engine_key_end= engine_key + table_share->normalized_path.length;
  query_cache_key_end= query_cache_key + table_share->table_cache_key.length -1;

  engine_key_end[0]= engine_key_end[2]= query_cache_key_end[0]=
    query_cache_key_end[2]= '#';
  query_cache_key_end[1]= engine_key_end[1]= 'P';
  engine_key_end+= 3;
  query_cache_key_end+= 3;

  i= 0;
  do
  {
    partition_element *part_elem= part_it++;
    char *engine_pos= strmov(engine_key_end, part_elem->partition_name);
    if (m_is_sub_partitioned)
    {
      List_iterator<partition_element> subpart_it(part_elem->subpartitions);
      partition_element *sub_elem;
      uint j= 0, part;
      engine_pos[0]= engine_pos[3]= '#';
      engine_pos[1]= 'S';
      engine_pos[2]= 'P';
      engine_pos += 4;
      do
      {
        char *end;
        uint length;
        sub_elem= subpart_it++;
        part= i * num_subparts + j;
        /* we store the end \0 as part of the key */
        end= strmov(engine_pos, sub_elem->partition_name);
        length= (uint)(end - engine_key);
        /* Copy the suffix also to query cache key */
        memcpy(query_cache_key_end, engine_key_end, (end - engine_key_end));
        if (reg_query_cache_dependant_table(thd, engine_key, length,
                                            query_cache_key,
                                            length + diff_length,
                                            m_file[part]->table_cache_type(),
                                            cache,
                                            block_table, m_file[part],
                                            n))
          DBUG_RETURN(TRUE);
      } while (++j < num_subparts);
    }
    else
    {
      char *end= engine_pos+1;                  // copy end \0
      uint length= (uint)(end - engine_key);
      /* Copy the suffix also to query cache key */
      memcpy(query_cache_key_end, engine_key_end, (end - engine_key_end));
      if (reg_query_cache_dependant_table(thd, engine_key, length,
                                          query_cache_key,
                                          length + diff_length,
                                          m_file[i]->table_cache_type(),
                                          cache,
                                          block_table, m_file[i],
                                          n))
        DBUG_RETURN(TRUE);
    }
  } while (++i < num_parts);
  DBUG_PRINT("info", ("cnt: %u", (uint)m_tot_parts));
  DBUG_RETURN(FALSE);
}


/**
  Set up table share object before calling create on underlying handler

  @param table             Table object
  @param info              Create info
  @param part_elem[in,out] Pointer to used partition_element, searched if NULL

  @return    status
    @retval  TRUE  Error
    @retval  FALSE Success

  @details
    Set up
    1) Comment on partition
    2) MAX_ROWS, MIN_ROWS on partition
    3) Index file name on partition
    4) Data file name on partition
*/

int ha_partition::set_up_table_before_create(TABLE *tbl,
                    const char *partition_name_with_path,
                    HA_CREATE_INFO *info,
                    partition_element *part_elem)
{
  int error= 0;
  LEX_CSTRING part_name;
  THD *thd= ha_thd();
  DBUG_ENTER("set_up_table_before_create");

  DBUG_ASSERT(part_elem);

  if (!part_elem)
    DBUG_RETURN(1);
  tbl->s->max_rows= part_elem->part_max_rows;
  tbl->s->min_rows= part_elem->part_min_rows;
  part_name.str= strrchr(partition_name_with_path, FN_LIBCHAR)+1;
  part_name.length= strlen(part_name.str);
  if ((part_elem->index_file_name &&
      (error= append_file_to_dir(thd,
                                 (const char**)&part_elem->index_file_name,
                                 &part_name))) ||
      (part_elem->data_file_name &&
      (error= append_file_to_dir(thd,
                                 (const char**)&part_elem->data_file_name,
                                 &part_name))))
  {
    DBUG_RETURN(error);
  }
  info->index_file_name= part_elem->index_file_name;
  info->data_file_name= part_elem->data_file_name;
  info->connect_string= part_elem->connect_string;
  if (info->connect_string.length)
    info->used_fields|= HA_CREATE_USED_CONNECTION;
  tbl->s->connect_string= part_elem->connect_string;
  DBUG_RETURN(0);
}


/*
  Add two names together

  SYNOPSIS
    name_add()
    out:dest                          Destination string
    first_name                        First name
    sec_name                          Second name

  RETURN VALUE
    >0                                Error
    0                                 Success

  DESCRIPTION
    Routine used to add two names with '_' in between then. Service routine
    to create_handler_file
    Include the NULL in the count of characters since it is needed as separator
    between the partition names.
*/

static uint name_add(char *dest, const char *first_name, const char *sec_name)
{
  return (uint) (strxmov(dest, first_name, "#SP#", sec_name, NullS) -dest) + 1;
}


/**
  Create the special .par file

  @param name  Full path of table name

  @return Operation status
    @retval FALSE  Error code
    @retval TRUE   Success

  @note
    Method used to create handler file with names of partitions, their
    engine types and the number of partitions.
*/

bool ha_partition::create_handler_file(const char *name)
{
  partition_element *part_elem, *subpart_elem;
  size_t i, j, part_name_len, subpart_name_len;
  size_t tot_partition_words, tot_name_len, num_parts;
  size_t tot_parts= 0;
  size_t tot_len_words, tot_len_byte, chksum, tot_name_words;
  char *name_buffer_ptr;
  uchar *file_buffer, *engine_array;
  bool result= TRUE;
  char file_name[FN_REFLEN];
  char part_name[FN_REFLEN];
  char subpart_name[FN_REFLEN];
  File file;
  List_iterator_fast <partition_element> part_it(m_part_info->partitions);
  DBUG_ENTER("create_handler_file");

  num_parts= m_part_info->partitions.elements;
  DBUG_PRINT("enter", ("table name: %s  num_parts: %zu", name, num_parts));
  tot_name_len= 0;
  for (i= 0; i < num_parts; i++)
  {
    part_elem= part_it++;
    if (part_elem->part_state != PART_NORMAL &&
        part_elem->part_state != PART_TO_BE_ADDED &&
        part_elem->part_state != PART_CHANGED)
      continue;
    tablename_to_filename(part_elem->partition_name, part_name,
                          FN_REFLEN);
    part_name_len= strlen(part_name);
    if (!m_is_sub_partitioned)
    {
      tot_name_len+= part_name_len + 1;
      tot_parts++;
    }
    else
    {
      List_iterator_fast <partition_element> sub_it(part_elem->subpartitions);
      for (j= 0; j < m_part_info->num_subparts; j++)
      {
	subpart_elem= sub_it++;
        tablename_to_filename(subpart_elem->partition_name,
                              subpart_name,
                              FN_REFLEN);
	subpart_name_len= strlen(subpart_name);
	tot_name_len+= part_name_len + subpart_name_len + 5;
        tot_parts++;
      }
    }
  }
  /*
     File format:
     Length in words              4 byte
     Checksum                     4 byte
     Total number of partitions   4 byte
     Array of engine types        n * 4 bytes where
     n = (m_tot_parts + 3)/4
     Length of name part in bytes 4 bytes
     (Names in filename format)
     Name part                    m * 4 bytes where
     m = ((length_name_part + 3)/4)*4

     All padding bytes are zeroed
  */
  tot_partition_words= (tot_parts + PAR_WORD_SIZE - 1) / PAR_WORD_SIZE;
  tot_name_words= (tot_name_len + PAR_WORD_SIZE - 1) / PAR_WORD_SIZE;
  /* 4 static words (tot words, checksum, tot partitions, name length) */
  tot_len_words= 4 + tot_partition_words + tot_name_words;
  tot_len_byte= PAR_WORD_SIZE * tot_len_words;
  if (!(file_buffer= (uchar *) my_malloc(tot_len_byte, MYF(MY_ZEROFILL))))
    DBUG_RETURN(TRUE);
  engine_array= (file_buffer + PAR_ENGINES_OFFSET);
  name_buffer_ptr= (char*) (engine_array + tot_partition_words * PAR_WORD_SIZE
                            + PAR_WORD_SIZE);
  part_it.rewind();
  for (i= 0; i < num_parts; i++)
  {
    part_elem= part_it++;
    if (part_elem->part_state != PART_NORMAL &&
        part_elem->part_state != PART_TO_BE_ADDED &&
        part_elem->part_state != PART_CHANGED)
      continue;
    if (!m_is_sub_partitioned)
    {
      tablename_to_filename(part_elem->partition_name, part_name, FN_REFLEN);
      name_buffer_ptr= strmov(name_buffer_ptr, part_name)+1;
      *engine_array= (uchar) ha_legacy_type(part_elem->engine_type);
      DBUG_PRINT("info", ("engine: %u", *engine_array));
      engine_array++;
    }
    else
    {
      List_iterator_fast <partition_element> sub_it(part_elem->subpartitions);
      for (j= 0; j < m_part_info->num_subparts; j++)
      {
	subpart_elem= sub_it++;
        tablename_to_filename(part_elem->partition_name, part_name,
                              FN_REFLEN);
        tablename_to_filename(subpart_elem->partition_name, subpart_name,
                              FN_REFLEN);
	name_buffer_ptr+= name_add(name_buffer_ptr,
				   part_name,
				   subpart_name);
        *engine_array= (uchar) ha_legacy_type(subpart_elem->engine_type);
        DBUG_PRINT("info", ("engine: %u", *engine_array));
	engine_array++;
      }
    }
  }
  chksum= 0;
  int4store(file_buffer, tot_len_words);
  int4store(file_buffer + PAR_NUM_PARTS_OFFSET, tot_parts);
  int4store(file_buffer + PAR_ENGINES_OFFSET +
            (tot_partition_words * PAR_WORD_SIZE),
            tot_name_len);
  for (i= 0; i < tot_len_words; i++)
    chksum^= uint4korr(file_buffer + PAR_WORD_SIZE * i);
  int4store(file_buffer + PAR_CHECKSUM_OFFSET, chksum);
  /*
    Add .par extension to the file name.
    Create and write and close file
    to be used at open, delete_table and rename_table
  */
  fn_format(file_name, name, "", ha_par_ext, MY_APPEND_EXT);
  if ((file= mysql_file_create(key_file_partition,
                               file_name, CREATE_MODE, O_RDWR | O_TRUNC,
                               MYF(MY_WME))) >= 0)
  {
    result= mysql_file_write(file, (uchar *) file_buffer, tot_len_byte,
                             MYF(MY_WME | MY_NABP)) != 0;

    /* Write connection information (for federatedx engine) */
    part_it.rewind();
    for (i= 0; i < num_parts && !result; i++)
    {
      uchar buffer[4];
      part_elem= part_it++;
      size_t length= part_elem->connect_string.length;
      int4store(buffer, length);
      if (my_write(file, buffer, 4, MYF(MY_WME | MY_NABP)) ||
          my_write(file, (uchar *) part_elem->connect_string.str, length,
                   MYF(MY_WME | MY_NABP)))
      {
        result= TRUE;
        break;
      }
    }
    (void) mysql_file_close(file, MYF(0));
  }
  else
    result= TRUE;
  my_free(file_buffer);
  DBUG_RETURN(result);
}


/**
  Clear handler variables and free some memory
*/

void ha_partition::clear_handler_file()
{
  if (m_engine_array)
    plugin_unlock_list(NULL, m_engine_array, m_tot_parts);
  free_root(&m_mem_root, MYF(MY_KEEP_PREALLOC));
  m_file_buffer= NULL;
  m_engine_array= NULL;
  m_connect_string= NULL;
}


/**
  Create underlying handler objects

  @param mem_root  Allocate memory through this

  @return Operation status
    @retval TRUE   Error
    @retval FALSE  Success
*/

bool ha_partition::create_handlers(MEM_ROOT *mem_root)
{
  uint i;
  uint alloc_len= (m_tot_parts + 1) * sizeof(handler*);
  handlerton *hton0;
  DBUG_ENTER("create_handlers");

  if (!(m_file= (handler **) alloc_root(mem_root, alloc_len)))
    DBUG_RETURN(TRUE);
  m_file_tot_parts= m_tot_parts;
  bzero((char*) m_file, alloc_len);
  for (i= 0; i < m_tot_parts; i++)
  {
    handlerton *hton= plugin_data(m_engine_array[i], handlerton*);
    if (!(m_file[i]= get_new_handler(table_share, mem_root, hton)))
      DBUG_RETURN(TRUE);
    DBUG_PRINT("info", ("engine_type: %u", hton->db_type));
  }
  /* For the moment we only support partition over the same table engine */
  hton0= plugin_data(m_engine_array[0], handlerton*);
  if (hton0 == myisam_hton)
  {
    DBUG_PRINT("info", ("MyISAM"));
    m_myisam= TRUE;
  }
  /* INNODB may not be compiled in... */
  else if (ha_legacy_type(hton0) == DB_TYPE_INNODB)
  {
    DBUG_PRINT("info", ("InnoDB"));
    m_innodb= TRUE;
  }
  DBUG_RETURN(FALSE);
}


/*
  Create underlying handler objects from partition info

  SYNOPSIS
    new_handlers_from_part_info()
    mem_root		Allocate memory through this

  RETURN VALUE
    TRUE                  Error
    FALSE                 Success
*/

bool ha_partition::new_handlers_from_part_info(MEM_ROOT *mem_root)
{
  uint i, j, part_count;
  partition_element *part_elem;
  uint alloc_len= (m_tot_parts + 1) * sizeof(handler*);
  List_iterator_fast <partition_element> part_it(m_part_info->partitions);
  DBUG_ENTER("ha_partition::new_handlers_from_part_info");

  if (!(m_file= (handler **) alloc_root(mem_root, alloc_len)))
    goto error;

  m_file_tot_parts= m_tot_parts;
  bzero((char*) m_file, alloc_len);
  DBUG_ASSERT(m_part_info->num_parts > 0);

  i= 0;
  part_count= 0;
  /*
    Don't know the size of the underlying storage engine, invent a number of
    bytes allocated for error message if allocation fails
  */
  do
  {
    part_elem= part_it++;
    if (m_is_sub_partitioned)
    {
      for (j= 0; j < m_part_info->num_subparts; j++)
      {
	if (!(m_file[part_count++]= get_new_handler(table_share, mem_root,
                                                    part_elem->engine_type)))
          goto error;
	DBUG_PRINT("info", ("engine_type: %u",
                   (uint) ha_legacy_type(part_elem->engine_type)));
      }
    }
    else
    {
      if (!(m_file[part_count++]= get_new_handler(table_share, mem_root,
                                                  part_elem->engine_type)))
        goto error;
      DBUG_PRINT("info", ("engine_type: %u",
                 (uint) ha_legacy_type(part_elem->engine_type)));
    }
  } while (++i < m_part_info->num_parts);
  if (part_elem->engine_type == myisam_hton)
  {
    DBUG_PRINT("info", ("MyISAM"));
    m_myisam= TRUE;
  }
  DBUG_RETURN(FALSE);
error:
  DBUG_RETURN(TRUE);
}


/**
  Read the .par file to get the partitions engines and names

  @param name  Name of table file (without extention)

  @return Operation status
    @retval true   Failure
    @retval false  Success

  @note On success, m_file_buffer is allocated and must be
  freed by the caller. m_name_buffer_ptr and m_tot_parts is also set.
*/

bool ha_partition::read_par_file(const char *name)
{
  char buff[FN_REFLEN];
  uchar *tot_name_len_offset;
  File file;
  uchar *file_buffer;
  uint i, len_bytes, len_words, tot_partition_words, tot_name_words, chksum;
  DBUG_ENTER("ha_partition::read_par_file");
  DBUG_PRINT("enter", ("table name: '%s'", name));

  if (m_file_buffer)
    DBUG_RETURN(false);
  fn_format(buff, name, "", ha_par_ext, MY_APPEND_EXT);

  /* Following could be done with mysql_file_stat to read in whole file */
  if ((file= mysql_file_open(key_file_partition,
                             buff, O_RDONLY | O_SHARE, MYF(0))) < 0)
    DBUG_RETURN(TRUE);
  if (mysql_file_read(file, (uchar *) &buff[0], PAR_WORD_SIZE, MYF(MY_NABP)))
    goto err1;
  len_words= uint4korr(buff);
  len_bytes= PAR_WORD_SIZE * len_words;
  if (mysql_file_seek(file, 0, MY_SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR)
    goto err1;
  if (!(file_buffer= (uchar*) alloc_root(&m_mem_root, len_bytes)))
    goto err1;
  if (mysql_file_read(file, file_buffer, len_bytes, MYF(MY_NABP)))
    goto err2;

  chksum= 0;
  for (i= 0; i < len_words; i++)
    chksum ^= uint4korr((file_buffer) + PAR_WORD_SIZE * i);
  if (chksum)
    goto err2;
  m_tot_parts= uint4korr((file_buffer) + PAR_NUM_PARTS_OFFSET);
  DBUG_PRINT("info", ("No of parts: %u", m_tot_parts));
  tot_partition_words= (m_tot_parts + PAR_WORD_SIZE - 1) / PAR_WORD_SIZE;

  tot_name_len_offset= file_buffer + PAR_ENGINES_OFFSET +
                       PAR_WORD_SIZE * tot_partition_words;
  tot_name_words= (uint4korr(tot_name_len_offset) + PAR_WORD_SIZE - 1) /
                  PAR_WORD_SIZE;
  /*
    Verify the total length = tot size word, checksum word, num parts word +
    engines array + name length word + name array.
  */
  if (len_words != (tot_partition_words + tot_name_words + 4))
    goto err2;
  m_file_buffer= file_buffer;          // Will be freed in clear_handler_file()
  m_name_buffer_ptr= (char*) (tot_name_len_offset + PAR_WORD_SIZE);

  if (!(m_connect_string= (LEX_CSTRING*)
        alloc_root(&m_mem_root, m_tot_parts * sizeof(LEX_CSTRING))))
    goto err2;
  bzero(m_connect_string, m_tot_parts * sizeof(LEX_CSTRING));

  /* Read connection arguments (for federated X engine) */
  for (i= 0; i < m_tot_parts; i++)
  {
    LEX_CSTRING connect_string;
    uchar buffer[4];
    char *tmp;
    if (my_read(file, buffer, 4, MYF(MY_NABP)))
    {
      /* No extra options; Probably not a federatedx engine */
      break;
    }
    connect_string.length= uint4korr(buffer);
    connect_string.str= tmp= (char*) alloc_root(&m_mem_root,
                                                connect_string.length+1);
    if (my_read(file, (uchar*) connect_string.str, connect_string.length,
                MYF(MY_NABP)))
      break;
    tmp[connect_string.length]= 0;
    m_connect_string[i]= connect_string;
  }

  (void) mysql_file_close(file, MYF(0));
  DBUG_RETURN(false);

err2:
err1:
  (void) mysql_file_close(file, MYF(0));
  DBUG_RETURN(true);
}


/**
  Setup m_engine_array

  @param mem_root  MEM_ROOT to use for allocating new handlers

  @return Operation status
    @retval false  Success
    @retval true   Failure
*/

bool ha_partition::setup_engine_array(MEM_ROOT *mem_root)
{
  uint i;
  uchar *buff;
  handlerton **engine_array, *first_engine;
  enum legacy_db_type db_type, first_db_type;

  DBUG_ASSERT(!m_file);
  DBUG_ENTER("ha_partition::setup_engine_array");
  engine_array= (handlerton **) my_alloca(m_tot_parts * sizeof(handlerton*));
  if (!engine_array)
    DBUG_RETURN(true);

  buff= (uchar *) (m_file_buffer + PAR_ENGINES_OFFSET);
  first_db_type= (enum legacy_db_type) buff[0];
  first_engine= ha_resolve_by_legacy_type(ha_thd(), first_db_type);
  if (!first_engine)
    goto err;

  if (!(m_engine_array= (plugin_ref*)
        alloc_root(&m_mem_root, m_tot_parts * sizeof(plugin_ref))))
    goto err;

  for (i= 0; i < m_tot_parts; i++)
  {
    db_type= (enum legacy_db_type) buff[i];
    if (db_type != first_db_type)
    {
      DBUG_PRINT("error", ("partition %u engine %d is not same as "
                           "first partition %d", i, db_type,
                           (int) first_db_type));
      DBUG_ASSERT(0);
      clear_handler_file();
      goto err;
    }
    m_engine_array[i]= ha_lock_engine(NULL, first_engine);
    if (!m_engine_array[i])
    {
      clear_handler_file();
      goto err;
    }
  }

  my_afree(engine_array);

  if (create_handlers(mem_root))
  {
    clear_handler_file();
    DBUG_RETURN(true);
  }

  DBUG_RETURN(false);

err:
  my_afree(engine_array);
  DBUG_RETURN(true);
}


/**
  Get info about partition engines and their names from the .par file

  @param name      Full path of table name
  @param mem_root  Allocate memory through this
  @param is_clone  If it is a clone, don't create new handlers

  @return Operation status
    @retval true   Error
    @retval false  Success

  @note Open handler file to get partition names, engine types and number of
  partitions.
*/

bool ha_partition::get_from_handler_file(const char *name, MEM_ROOT *mem_root,
                                         bool is_clone)
{
  DBUG_ENTER("ha_partition::get_from_handler_file");
  DBUG_PRINT("enter", ("table name: '%s'", name));

  if (m_file_buffer)
    DBUG_RETURN(false);

  if (read_par_file(name))
    DBUG_RETURN(true);

  if (!is_clone && setup_engine_array(mem_root))
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}


/****************************************************************************
                MODULE open/close object
****************************************************************************/

/**
  Get the partition name.

  @param       part   Struct containing name and length
  @param[out]  length Length of the name

  @return Partition name
*/

static uchar *get_part_name(PART_NAME_DEF *part, size_t *length,
                            my_bool not_used __attribute__((unused)))
{
  *length= part->length;
  return part->partition_name;
}


/**
  Insert a partition name in the partition_name_hash.

  @param name        Name of partition
  @param part_id     Partition id (number)
  @param is_subpart  Set if the name belongs to a subpartition

  @return Operation status
    @retval true   Failure
    @retval false  Sucess
*/

bool ha_partition::insert_partition_name_in_hash(const char *name, uint part_id,
                                                 bool is_subpart)
{
  PART_NAME_DEF *part_def;
  uchar *part_name;
  size_t part_name_length;
  DBUG_ENTER("ha_partition::insert_partition_name_in_hash");
  /*
    Calculate and store the length here, to avoid doing it when
    searching the hash.
  */
  part_name_length= strlen(name);
  /*
    Must use memory that lives as long as table_share.
    Freed in the Partition_share destructor.
    Since we use my_multi_malloc, then my_free(part_def) will also free
    part_name, as a part of my_hash_free.
  */
  if (!my_multi_malloc(MY_WME,
                       &part_def, sizeof(PART_NAME_DEF),
                       &part_name, part_name_length + 1,
                       NULL))
    DBUG_RETURN(true);
  memcpy(part_name, name, part_name_length + 1);
  part_def->partition_name= part_name;
  part_def->length= (uint)part_name_length;
  part_def->part_id= part_id;
  part_def->is_subpart= is_subpart;
  if (my_hash_insert(&part_share->partition_name_hash, (uchar *) part_def))
  {
    my_free(part_def);
    DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/**
  Populate the partition_name_hash in part_share.
*/

bool ha_partition::populate_partition_name_hash()
{
  List_iterator<partition_element> part_it(m_part_info->partitions);
  uint num_parts= m_part_info->num_parts;
  uint num_subparts= m_is_sub_partitioned ? m_part_info->num_subparts : 1;
  uint tot_names;
  uint i= 0;
  DBUG_ASSERT(part_share);

  DBUG_ENTER("ha_partition::populate_partition_name_hash");

  /*
    partition_name_hash is only set once and never changed
    -> OK to check without locking.
  */

  if (part_share->partition_name_hash_initialized)
    DBUG_RETURN(false);
  lock_shared_ha_data();
  if (part_share->partition_name_hash_initialized)
  {
    unlock_shared_ha_data();
    DBUG_RETURN(false);
  }
  tot_names= m_is_sub_partitioned ? m_tot_parts + num_parts : num_parts;
  if (my_hash_init(&part_share->partition_name_hash,
                   system_charset_info, tot_names, 0, 0,
                   (my_hash_get_key) get_part_name,
                   my_free, HASH_UNIQUE))
  {
    unlock_shared_ha_data();
    DBUG_RETURN(TRUE);
  }

  do
  {
    partition_element *part_elem= part_it++;
    DBUG_ASSERT(part_elem->part_state == PART_NORMAL);
    if (part_elem->part_state == PART_NORMAL)
    {
      if (insert_partition_name_in_hash(part_elem->partition_name,
                                        i * num_subparts, false))
        goto err;
      if (m_is_sub_partitioned)
      {
        List_iterator<partition_element>
                                    subpart_it(part_elem->subpartitions);
        partition_element *sub_elem;
        uint j= 0;
        do
        {
          sub_elem= subpart_it++;
          if (insert_partition_name_in_hash(sub_elem->partition_name,
                                            i * num_subparts + j, true))
            goto err;

        } while (++j < num_subparts);
      }
    }
  } while (++i < num_parts);

  part_share->partition_name_hash_initialized= true;
  unlock_shared_ha_data();

  DBUG_RETURN(FALSE);
err:
  my_hash_free(&part_share->partition_name_hash);
  unlock_shared_ha_data();

  DBUG_RETURN(TRUE);
}


/**
  Set Handler_share pointer and allocate Handler_share pointers
  for each partition and set those.

  @param ha_share_arg  Where to store/retrieve the Partitioning_share pointer
                       to be shared by all instances of the same table.

  @return Operation status
    @retval true  Failure
    @retval false Sucess
*/

bool ha_partition::set_ha_share_ref(Handler_share **ha_share_arg)
{
  Handler_share **ha_shares;
  uint i;
  DBUG_ENTER("ha_partition::set_ha_share_ref");

  DBUG_ASSERT(!part_share);
  DBUG_ASSERT(table_share);
  DBUG_ASSERT(!m_is_clone_of);
  DBUG_ASSERT(m_tot_parts);
  if (handler::set_ha_share_ref(ha_share_arg))
    DBUG_RETURN(true);
  if (!(part_share= get_share()))
    DBUG_RETURN(true);
  DBUG_ASSERT(part_share->partitions_share_refs.num_parts >= m_tot_parts);
  ha_shares= part_share->partitions_share_refs.ha_shares;
  for (i= 0; i < m_tot_parts; i++)
  {
    if (m_file[i]->set_ha_share_ref(&ha_shares[i]))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/**
  Get the PARTITION_SHARE for the table.

  @return Operation status
    @retval true   Error
    @retval false  Success

  @note Gets or initializes the Partition_share object used by partitioning.
  The Partition_share is used for handling the auto_increment etc.
*/

Partition_share *ha_partition::get_share()
{
  Partition_share *tmp_share;
  DBUG_ENTER("ha_partition::get_share");
  DBUG_ASSERT(table_share);

  lock_shared_ha_data();
  if (!(tmp_share= static_cast<Partition_share*>(get_ha_share_ptr())))
  {
    tmp_share= new Partition_share;
    if (!tmp_share)
      goto err;
    if (tmp_share->init(m_tot_parts))
    {
      delete tmp_share;
      tmp_share= NULL;
      goto err;
    }
    set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  DBUG_RETURN(tmp_share);
}



/**
  Helper function for freeing all internal bitmaps.
*/

void ha_partition::free_partition_bitmaps()
{
  /* Initialize the bitmap we use to minimize ha_start_bulk_insert calls */
  my_bitmap_free(&m_bulk_insert_started);
  my_bitmap_free(&m_locked_partitions);
  my_bitmap_free(&m_partitions_to_reset);
  my_bitmap_free(&m_key_not_found_partitions);
  my_bitmap_free(&m_opened_partitions);
  my_bitmap_free(&m_mrr_used_partitions);
}


/**
  Helper function for initializing all internal bitmaps.

  Note:
  All bitmaps, including partially allocated, are freed in
  free_partion_bitmaps()
*/

bool ha_partition::init_partition_bitmaps()
{
  DBUG_ENTER("ha_partition::init_partition_bitmaps");

  /* Initialize the bitmap we use to minimize ha_start_bulk_insert calls */
  if (my_bitmap_init(&m_bulk_insert_started, NULL, m_tot_parts + 1, FALSE))
    DBUG_RETURN(true);

  /* Initialize the bitmap we use to keep track of locked partitions */
  if (my_bitmap_init(&m_locked_partitions, NULL, m_tot_parts, FALSE))
    DBUG_RETURN(true);

  /*
    Initialize the bitmap we use to keep track of partitions which may have
    something to reset in ha_reset().
  */
  if (my_bitmap_init(&m_partitions_to_reset, NULL, m_tot_parts, FALSE))
    DBUG_RETURN(true);

  /*
    Initialize the bitmap we use to keep track of partitions which returned
    HA_ERR_KEY_NOT_FOUND from index_read_map.
  */
  if (my_bitmap_init(&m_key_not_found_partitions, NULL, m_tot_parts, FALSE))
    DBUG_RETURN(true);

  if (bitmap_init(&m_mrr_used_partitions, NULL, m_tot_parts, TRUE))
    DBUG_RETURN(true);

  if (my_bitmap_init(&m_opened_partitions, NULL, m_tot_parts, FALSE))
    DBUG_RETURN(true);

  m_file_sample= NULL;

  /* Initialize the bitmap for read/lock_partitions */
  if (!m_is_clone_of)
  {
    DBUG_ASSERT(!m_clone_mem_root);
    if (m_part_info->set_partition_bitmaps(NULL))
      DBUG_RETURN(true);
  }
  DBUG_RETURN(false);
}


/*
  Open handler object

  SYNOPSIS
    open()
    name                  Full path of table name
    mode                  Open mode flags
    test_if_locked        ?

  RETURN VALUE
    >0                    Error
    0                     Success

  DESCRIPTION
    Used for opening tables. The name will be the name of the file.
    A table is opened when it needs to be opened. For instance
    when a request comes in for a select on the table (tables are not
    open and closed for each request, they are cached).

    Called from handler.cc by handler::ha_open(). The server opens all tables
    by calling ha_open() which then calls the handler specific open().
*/

int ha_partition::open(const char *name, int mode, uint test_if_locked)
{
  int error= HA_ERR_INITIALIZATION;
  handler **file;
  char name_buff[FN_REFLEN + 1];
  ulonglong check_table_flags;
  DBUG_ENTER("ha_partition::open");

  DBUG_ASSERT(table->s == table_share);
  ref_length= 0;
  m_mode= mode;
  m_open_test_lock= test_if_locked;
  m_part_field_array= m_part_info->full_part_field_array;
  if (get_from_handler_file(name, &table->mem_root, MY_TEST(m_is_clone_of)))
    DBUG_RETURN(error);
  if (populate_partition_name_hash())
  {
    DBUG_RETURN(HA_ERR_INITIALIZATION);
  }
  m_start_key.length= 0;
  m_rec0= table->record[0];
  m_rec_length= table_share->reclength;
  if (!m_part_ids_sorted_by_num_of_records)
  {
    if (!(m_part_ids_sorted_by_num_of_records=
            (uint32*) my_malloc(m_tot_parts * sizeof(uint32), MYF(MY_WME))))
      DBUG_RETURN(error);
    uint32 i;
    /* Initialize it with all partition ids. */
    for (i= 0; i < m_tot_parts; i++)
      m_part_ids_sorted_by_num_of_records[i]= i;
  }

  if (init_partition_bitmaps())
    goto err_alloc;

  if (unlikely((error=
                m_part_info->set_partition_bitmaps(m_partitions_to_open))))
    goto err_alloc;

  /* Allocate memory used with MMR */
  if (!(m_range_info= (void **)
        my_multi_malloc(MYF(MY_WME),
                        &m_range_info, sizeof(range_id_t) * m_tot_parts,
                        &m_stock_range_seq, sizeof(uint) * m_tot_parts,
                        &m_mrr_buffer, sizeof(HANDLER_BUFFER) * m_tot_parts,
                        &m_mrr_buffer_size, sizeof(uint) * m_tot_parts,
                        &m_part_mrr_range_length, sizeof(uint) * m_tot_parts,
                        &m_part_mrr_range_first,
                        sizeof(PARTITION_PART_KEY_MULTI_RANGE *) * m_tot_parts,
                        &m_part_mrr_range_current,
                        sizeof(PARTITION_PART_KEY_MULTI_RANGE *) * m_tot_parts,
                        &m_partition_part_key_multi_range_hld,
                        sizeof(PARTITION_PART_KEY_MULTI_RANGE_HLD) *
                        m_tot_parts,
                        NullS)))
    goto err_alloc;

  bzero(m_mrr_buffer, m_tot_parts * sizeof(HANDLER_BUFFER));
  bzero(m_part_mrr_range_first,
        sizeof(PARTITION_PART_KEY_MULTI_RANGE *) * m_tot_parts);

  if (m_is_clone_of)
  {
    uint i, alloc_len;
    char *name_buffer_ptr;
    DBUG_ASSERT(m_clone_mem_root);
    /* Allocate an array of handler pointers for the partitions handlers. */
    alloc_len= (m_tot_parts + 1) * sizeof(handler*);
    if (!(m_file= (handler **) alloc_root(m_clone_mem_root, alloc_len)))
    {
      error= HA_ERR_INITIALIZATION;
      goto err_alloc;
    }
    memset(m_file, 0, alloc_len);
    name_buffer_ptr= m_name_buffer_ptr;
    /*
      Populate them by cloning the original partitions. This also opens them.
      Note that file->ref is allocated too.
    */
    file= m_is_clone_of->m_file;
    for (i= 0; i < m_tot_parts; i++)
    {
      if (!bitmap_is_set(&m_is_clone_of->m_opened_partitions, i))
        continue;

      if (unlikely((error= create_partition_name(name_buff, sizeof(name_buff),
                                                 name, name_buffer_ptr,
                                                 NORMAL_PART_NAME, FALSE))))
        goto err_handler;
      /* ::clone() will also set ha_share from the original. */
      if (!(m_file[i]= file[i]->clone(name_buff, m_clone_mem_root)))
      {
        error= HA_ERR_INITIALIZATION;
        file= &m_file[i];
        goto err_handler;
      }
      if (!m_file_sample)
        m_file_sample= m_file[i];
      name_buffer_ptr+= strlen(name_buffer_ptr) + 1;
      bitmap_set_bit(&m_opened_partitions, i);
    }
  }
  else
  {
    if (unlikely((error= open_read_partitions(name_buff, sizeof(name_buff)))))
      goto err_handler;
    m_num_locks= m_file_sample->lock_count();
  }
  /*
    We want to know the upper bound for locks, to allocate enough memory.
    There is no performance lost if we simply return in lock_count() the
    maximum number locks needed, only some minor over allocation of memory
    in get_lock_data().
  */
  m_num_locks*= m_tot_parts;

  file= m_file;
  ref_length= get_open_file_sample()->ref_length;
  check_table_flags= ((get_open_file_sample()->ha_table_flags() &
                       ~(PARTITION_DISABLED_TABLE_FLAGS)) |
                      (PARTITION_ENABLED_TABLE_FLAGS));
  while (*(++file))
  {
    if (!bitmap_is_set(&m_opened_partitions, (uint)(file - m_file)))
      continue;
    /* MyISAM can have smaller ref_length for partitions with MAX_ROWS set */
    set_if_bigger(ref_length, ((*file)->ref_length));
    /*
      Verify that all partitions have the same set of table flags.
      Mask all flags that partitioning enables/disables.
    */
    if (check_table_flags != (((*file)->ha_table_flags() &
                               ~(PARTITION_DISABLED_TABLE_FLAGS)) |
                              (PARTITION_ENABLED_TABLE_FLAGS)))
    {
      error= HA_ERR_INITIALIZATION;
      /* set file to last handler, so all of them are closed */
      file= &m_file[m_tot_parts - 1];
      goto err_handler;
    }
  }
  key_used_on_scan= get_open_file_sample()->key_used_on_scan;
  implicit_emptied= get_open_file_sample()->implicit_emptied;
  /*
    Add 2 bytes for partition id in position ref length.
    ref_length=max_in_all_partitions(ref_length) + PARTITION_BYTES_IN_POS
  */
  ref_length+= PARTITION_BYTES_IN_POS;
  m_ref_length= ref_length;

  /*
    Release buffer read from .par file. It will not be reused again after
    being opened once.
  */
  clear_handler_file();

  /*
    Some handlers update statistics as part of the open call. This will in
    some cases corrupt the statistics of the partition handler and thus
    to ensure we have correct statistics we call info from open after
    calling open on all individual handlers.
  */
  m_handler_status= handler_opened;
  if (m_part_info->part_expr)
    m_part_func_monotonicity_info=
                            m_part_info->part_expr->get_monotonicity_info();
  else if (m_part_info->list_of_part_fields)
    m_part_func_monotonicity_info= MONOTONIC_STRICT_INCREASING;
  info(HA_STATUS_VARIABLE | HA_STATUS_CONST | HA_STATUS_OPEN);
  DBUG_RETURN(0);

err_handler:
  DEBUG_SYNC(ha_thd(), "partition_open_error");
  file= &m_file[m_tot_parts - 1];
  while (file-- != m_file)
  {
    if (bitmap_is_set(&m_opened_partitions, (uint)(file - m_file)))
      (*file)->ha_close();
  }
err_alloc:
  free_partition_bitmaps();
  my_free(m_range_info);
  m_range_info= 0;

  DBUG_RETURN(error);
}


/*
  Disabled since it is not possible to prune yet.
  without pruning, it need to rebind/unbind every partition in every
  statement which uses a table from the table cache. Will also use
  as many PSI_tables as there are partitions.
*/
#ifdef HAVE_M_PSI_PER_PARTITION
void ha_partition::unbind_psi()
{
  uint i;

  DBUG_ENTER("ha_partition::unbind_psi");
  handler::unbind_psi();
  for (i= 0; i < m_tot_parts; i++)
  {
    DBUG_ASSERT(m_file[i] != NULL);
    m_file[i]->unbind_psi();
  }
  DBUG_VOID_RETURN;
}

void ha_partition::rebind_psi()
{
  uint i;

  DBUG_ENTER("ha_partition::rebind_psi");
  handler::rebind_psi();
  for (i= 0; i < m_tot_parts; i++)
  {
    DBUG_ASSERT(m_file[i] != NULL);
    m_file[i]->rebind_psi();
  }
  DBUG_VOID_RETURN;
}
#endif /* HAVE_M_PSI_PER_PARTITION */


/**
  Clone the open and locked partitioning handler.

  @param  mem_root  MEM_ROOT to use.

  @return Pointer to the successfully created clone or NULL

  @details
  This function creates a new ha_partition handler as a clone/copy. The
  original (this) must already be opened and locked. The clone will use
  the originals m_part_info.
  It also allocates memory for ref + ref_dup.
  In ha_partition::open() it will clone its original handlers partitions
  which will allocate then on the correct MEM_ROOT and also open them.
*/

handler *ha_partition::clone(const char *name, MEM_ROOT *mem_root)
{
  ha_partition *new_handler;

  DBUG_ENTER("ha_partition::clone");
  new_handler= new (mem_root) ha_partition(ht, table_share, m_part_info,
                                           this, mem_root);
  if (!new_handler)
    DBUG_RETURN(NULL);

  /*
    We will not clone each partition's handler here, it will be done in
    ha_partition::open() for clones. Also set_ha_share_ref is not needed
    here, since 1) ha_share is copied in the constructor used above
    2) each partition's cloned handler will set it from its original.
  */

  /*
    Allocate new_handler->ref here because otherwise ha_open will allocate it
    on this->table->mem_root and we will not be able to reclaim that memory
    when the clone handler object is destroyed.
  */
  if (!(new_handler->ref= (uchar*) alloc_root(mem_root,
                                              ALIGN_SIZE(m_ref_length)*2)))
    goto err;

  if (new_handler->ha_open(table, name,
                           table->db_stat,
                           HA_OPEN_IGNORE_IF_LOCKED | HA_OPEN_NO_PSI_CALL))
    goto err;

  DBUG_RETURN((handler*) new_handler);

err:
  delete new_handler;
  DBUG_RETURN(NULL);
}


/*
  Close handler object

  SYNOPSIS
    close()

  RETURN VALUE
    >0                   Error code
    0                    Success

  DESCRIPTION
    Called from sql_base.cc, sql_select.cc, and table.cc.
    In sql_select.cc it is only used to close up temporary tables or during
    the process where a temporary table is converted over to being a
    myisam table.
    For sql_base.cc look at close_data_tables().
*/

int ha_partition::close(void)
{
  bool first= TRUE;
  handler **file;
  uint i;
  st_partition_ft_info *tmp_ft_info;
  DBUG_ENTER("ha_partition::close");
  DBUG_ASSERT(table->s == table_share);
  DBUG_ASSERT(m_part_info);

  destroy_record_priority_queue();

  for (; ft_first ; ft_first= tmp_ft_info)
  {
    tmp_ft_info= ft_first->next;
    my_free(ft_first);
  }

  /* Free active mrr_ranges */
  for (i= 0; i < m_tot_parts; i++)
  {
    if (m_part_mrr_range_first[i])
    {
      PARTITION_PART_KEY_MULTI_RANGE *tmp_mrr_range_first=
        m_part_mrr_range_first[i];
      do
      {
        PARTITION_PART_KEY_MULTI_RANGE *tmp_mrr_range_current;
        tmp_mrr_range_current= tmp_mrr_range_first;
        tmp_mrr_range_first= tmp_mrr_range_first->next;
        my_free(tmp_mrr_range_current);
      } while (tmp_mrr_range_first);
    }
  }
  if (m_mrr_range_first)
  {
    do
    {
      m_mrr_range_current= m_mrr_range_first;
      m_mrr_range_first= m_mrr_range_first->next;
      if (m_mrr_range_current->key[0])
        my_free(m_mrr_range_current->key[0]);
      if (m_mrr_range_current->key[1])
        my_free(m_mrr_range_current->key[1]);
      my_free(m_mrr_range_current);
    } while (m_mrr_range_first);
  }
  my_free(m_range_info);
  m_range_info= NULL;                           // Safety

  if (m_mrr_full_buffer)
  {
    my_free(m_mrr_full_buffer);
    m_mrr_full_buffer= NULL;
    m_mrr_full_buffer_size= 0;
  }
  file= m_file;

repeat:
  do
  {
    if (!first || bitmap_is_set(&m_opened_partitions, (uint)(file - m_file)))
      (*file)->ha_close();
  } while (*(++file));

  free_partition_bitmaps();

  if (first && m_added_file && m_added_file[0])
  {
    file= m_added_file;
    first= FALSE;
    goto repeat;
  }

  m_handler_status= handler_closed;
  DBUG_RETURN(0);
}

/****************************************************************************
                MODULE start/end statement
****************************************************************************/
/*
  A number of methods to define various constants for the handler. In
  the case of the partition handler we need to use some max and min
  of the underlying handlers in most cases.
*/

/*
  Set external locks on table

  SYNOPSIS
    external_lock()
    thd                    Thread object
    lock_type              Type of external lock

  RETURN VALUE
    >0                   Error code
    0                    Success

  DESCRIPTION
    First you should go read the section "locking functions for mysql" in
    lock.cc to understand this.
    This create a lock on the table. If you are implementing a storage engine
    that can handle transactions look at ha_berkeley.cc to see how you will
    want to go about doing this. Otherwise you should consider calling
    flock() here.
    Originally this method was used to set locks on file level to enable
    several MySQL Servers to work on the same data. For transactional
    engines it has been "abused" to also mean start and end of statements
    to enable proper rollback of statements and transactions. When LOCK
    TABLES has been issued the start_stmt method takes over the role of
    indicating start of statement but in this case there is no end of
    statement indicator(?).

    Called from lock.cc by lock_external() and unlock_external(). Also called
    from sql_table.cc by copy_data_between_tables().
*/

int ha_partition::external_lock(THD *thd, int lock_type)
{
  int error;
  uint i, first_used_partition;
  MY_BITMAP *used_partitions;
  DBUG_ENTER("ha_partition::external_lock");

  DBUG_ASSERT(!auto_increment_lock && !auto_increment_safe_stmt_log_lock);

  if (lock_type == F_UNLCK)
    used_partitions= &m_locked_partitions;
  else
    used_partitions= &(m_part_info->lock_partitions);

  first_used_partition= bitmap_get_first_set(used_partitions);

  for (i= first_used_partition;
       i < m_tot_parts;
       i= bitmap_get_next_set(used_partitions, i))
  {
    DBUG_PRINT("info", ("external_lock(thd, %d) part %u", lock_type, i));
    if (unlikely((error= m_file[i]->ha_external_lock(thd, lock_type))))
    {
      if (lock_type != F_UNLCK)
        goto err_handler;
    }
    DBUG_PRINT("info", ("external_lock part %u lock %d", i, lock_type));
    if (lock_type != F_UNLCK)
      bitmap_set_bit(&m_locked_partitions, i);
  }
  if (lock_type == F_UNLCK)
  {
    bitmap_clear_all(used_partitions);
  }
  else
  {
    /* Add touched partitions to be included in reset(). */
    bitmap_union(&m_partitions_to_reset, used_partitions);
  }

  if (m_added_file && m_added_file[0])
  {
    handler **file= m_added_file;
    DBUG_ASSERT(lock_type == F_UNLCK);
    do
    {
      (void) (*file)->ha_external_lock(thd, lock_type);
    } while (*(++file));
  }
  if (lock_type == F_WRLCK)
  {
    if (m_part_info->part_expr)
      m_part_info->part_expr->walk(&Item::register_field_in_read_map, 1, 0);
    if (m_part_info->part_type == VERSIONING_PARTITION)
      m_part_info->vers_set_hist_part(thd);
  }
  DBUG_RETURN(0);

err_handler:
  uint j;
  for (j= first_used_partition;
       j < i;
       j= bitmap_get_next_set(&m_locked_partitions, j))
  {
    (void) m_file[j]->ha_external_lock(thd, F_UNLCK);
  }
  bitmap_clear_all(&m_locked_partitions);
  DBUG_RETURN(error);
}


/*
  Get the lock(s) for the table and perform conversion of locks if needed

  SYNOPSIS
    store_lock()
    thd                   Thread object
    to                    Lock object array
    lock_type             Table lock type

  RETURN VALUE
    >0                   Error code
    0                    Success

  DESCRIPTION
    The idea with handler::store_lock() is the following:

    The statement decided which locks we should need for the table
    for updates/deletes/inserts we get WRITE locks, for SELECT... we get
    read locks.

    Before adding the lock into the table lock handler (see thr_lock.c)
    mysqld calls store lock with the requested locks.  Store lock can now
    modify a write lock to a read lock (or some other lock), ignore the
    lock (if we don't want to use MySQL table locks at all) or add locks
    for many tables (like we do when we are using a MERGE handler).

    Berkeley DB for partition  changes all WRITE locks to TL_WRITE_ALLOW_WRITE
    (which signals that we are doing WRITES, but we are still allowing other
    reader's and writer's.

    When releasing locks, store_lock() is also called. In this case one
    usually doesn't have to do anything.

    store_lock is called when holding a global mutex to ensure that only
    one thread at a time changes the locking information of tables.

    In some exceptional cases MySQL may send a request for a TL_IGNORE;
    This means that we are requesting the same lock as last time and this
    should also be ignored. (This may happen when someone does a flush
    table when we have opened a part of the tables, in which case mysqld
    closes and reopens the tables and tries to get the same locks as last
    time).  In the future we will probably try to remove this.

    Called from lock.cc by get_lock_data().
*/

THR_LOCK_DATA **ha_partition::store_lock(THD *thd,
					 THR_LOCK_DATA **to,
					 enum thr_lock_type lock_type)
{
  uint i;
  DBUG_ENTER("ha_partition::store_lock");
  DBUG_ASSERT(thd == current_thd);

  /*
    This can be called from get_lock_data() in mysql_lock_abort_for_thread(),
    even when thd != table->in_use. In that case don't use partition pruning,
    but use all partitions instead to avoid using another threads structures.
  */
  if (thd != table->in_use)
  {
    for (i= 0; i < m_tot_parts; i++)
      to= m_file[i]->store_lock(thd, to, lock_type);
  }
  else
  {
    for (i= bitmap_get_first_set(&(m_part_info->lock_partitions));
         i < m_tot_parts;
         i= bitmap_get_next_set(&m_part_info->lock_partitions, i))
    {
      DBUG_PRINT("info", ("store lock %u iteration", i));
      to= m_file[i]->store_lock(thd, to, lock_type);
    }
  }
  DBUG_RETURN(to);
}

/*
  Start a statement when table is locked

  SYNOPSIS
    start_stmt()
    thd                  Thread object
    lock_type            Type of external lock

  RETURN VALUE
    >0                   Error code
    0                    Success

  DESCRIPTION
    This method is called instead of external lock when the table is locked
    before the statement is executed.
*/

int ha_partition::start_stmt(THD *thd, thr_lock_type lock_type)
{
  int error= 0;
  uint i;
  /* Assert that read_partitions is included in lock_partitions */
  DBUG_ASSERT(bitmap_is_subset(&m_part_info->read_partitions,
                               &m_part_info->lock_partitions));
  /*
    m_locked_partitions is set in previous external_lock/LOCK TABLES.
    Current statement's lock requests must not include any partitions
    not previously locked.
  */
  DBUG_ASSERT(bitmap_is_subset(&m_part_info->lock_partitions,
                               &m_locked_partitions));
  DBUG_ENTER("ha_partition::start_stmt");

  for (i= bitmap_get_first_set(&(m_part_info->lock_partitions));
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->lock_partitions, i))
  {
    if (unlikely((error= m_file[i]->start_stmt(thd, lock_type))))
      break;
    /* Add partition to be called in reset(). */
    bitmap_set_bit(&m_partitions_to_reset, i);
  }
  if (lock_type == F_WRLCK && m_part_info->part_expr)
    m_part_info->part_expr->walk(&Item::register_field_in_read_map, 1, 0);
  DBUG_RETURN(error);
}


/**
  Get number of lock objects returned in store_lock

  @returns Number of locks returned in call to store_lock

  @desc
    Returns the maxinum possible number of store locks needed in call to
    store lock.
*/

uint ha_partition::lock_count() const
{
  DBUG_ENTER("ha_partition::lock_count");
  DBUG_RETURN(m_num_locks);
}


/*
  Unlock last accessed row

  SYNOPSIS
    unlock_row()

  RETURN VALUE
    NONE

  DESCRIPTION
    Record currently processed was not in the result set of the statement
    and is thus unlocked. Used for UPDATE and DELETE queries.
*/

void ha_partition::unlock_row()
{
  DBUG_ENTER("ha_partition::unlock_row");
  m_file[m_last_part]->unlock_row();
  DBUG_VOID_RETURN;
}

/**
  Check if semi consistent read was used

  SYNOPSIS
    was_semi_consistent_read()

  RETURN VALUE
    TRUE   Previous read was a semi consistent read
    FALSE  Previous read was not a semi consistent read

  DESCRIPTION
    See handler.h:
    In an UPDATE or DELETE, if the row under the cursor was locked by another
    transaction, and the engine used an optimistic read of the last
    committed row value under the cursor, then the engine returns 1 from this
    function. MySQL must NOT try to update this optimistic value. If the
    optimistic value does not match the WHERE condition, MySQL can decide to
    skip over this row. Currently only works for InnoDB. This can be used to
    avoid unnecessary lock waits.

    If this method returns nonzero, it will also signal the storage
    engine that the next read will be a locking re-read of the row.
*/
bool ha_partition::was_semi_consistent_read()
{
  DBUG_ENTER("ha_partition::was_semi_consistent_read");
  DBUG_ASSERT(m_last_part < m_tot_parts &&
              bitmap_is_set(&(m_part_info->read_partitions), m_last_part));
  DBUG_RETURN(m_file[m_last_part]->was_semi_consistent_read());
}

/**
  Use semi consistent read if possible

  SYNOPSIS
    try_semi_consistent_read()
    yes   Turn on semi consistent read

  RETURN VALUE
    NONE

  DESCRIPTION
    See handler.h:
    Tell the engine whether it should avoid unnecessary lock waits.
    If yes, in an UPDATE or DELETE, if the row under the cursor was locked
    by another transaction, the engine may try an optimistic read of
    the last committed row value under the cursor.
    Note: prune_partitions are already called before this call, so using
    pruning is OK.
*/
void ha_partition::try_semi_consistent_read(bool yes)
{
  uint i;
  DBUG_ENTER("ha_partition::try_semi_consistent_read");

  i= bitmap_get_first_set(&(m_part_info->read_partitions));
  DBUG_ASSERT(i != MY_BIT_NONE);
  for (;
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
  {
    m_file[i]->try_semi_consistent_read(yes);
  }
  DBUG_VOID_RETURN;
}


/****************************************************************************
                MODULE change record
****************************************************************************/

/*
  Insert a row to the table

  SYNOPSIS
    write_row()
    buf                        The row in MySQL Row Format

  RETURN VALUE
    >0                         Error code
    0                          Success

  DESCRIPTION
    write_row() inserts a row. buf() is a byte array of data, normally
    record[0].

    You can use the field information to extract the data from the native byte
    array type.

    Example of this would be:
    for (Field **field=table->field ; *field ; field++)
    {
      ...
    }

    See ha_tina.cc for a variant of extracting all of the data as strings.
    ha_berkeley.cc has a variant of how to store it intact by "packing" it
    for ha_berkeley's own native storage type.

    Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
    sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

    ADDITIONAL INFO:

    We have to set auto_increment fields, because those may be used in
    determining which partition the row should be written to.
*/

int ha_partition::write_row(uchar * buf)
{
  uint32 part_id;
  int error;
  longlong func_value;
  bool have_auto_increment= table->next_number_field && buf == table->record[0];
  my_bitmap_map *old_map;
  THD *thd= ha_thd();
  sql_mode_t saved_sql_mode= thd->variables.sql_mode;
  bool saved_auto_inc_field_not_null= table->auto_increment_field_not_null;
  DBUG_ENTER("ha_partition::write_row");
  DBUG_PRINT("enter", ("partition this: %p", this));

  /*
    If we have an auto_increment column and we are writing a changed row
    or a new row, then update the auto_increment value in the record.
  */
  if (have_auto_increment)
  {
    if (!table_share->next_number_keypart)
      update_next_auto_inc_val();
    error= update_auto_increment();

    /*
      If we have failed to set the auto-increment value for this row,
      it is highly likely that we will not be able to insert it into
      the correct partition. We must check and fail if neccessary.
    */
    if (unlikely(error))
      goto exit;

    /*
      Don't allow generation of auto_increment value the partitions handler.
      If a partitions handler would change the value, then it might not
      match the partition any longer.
      This can occur if 'SET INSERT_ID = 0; INSERT (NULL)',
      So allow this by adding 'MODE_NO_AUTO_VALUE_ON_ZERO' to sql_mode.
      The partitions handler::next_insert_id must always be 0. Otherwise
      we need to forward release_auto_increment, or reset it for all
      partitions.
    */
    if (table->next_number_field->val_int() == 0)
    {
      table->auto_increment_field_not_null= TRUE;
      thd->variables.sql_mode|= MODE_NO_AUTO_VALUE_ON_ZERO;
    }
  }

  old_map= dbug_tmp_use_all_columns(table, table->read_set);
  error= m_part_info->get_partition_id(m_part_info, &part_id, &func_value);
  dbug_tmp_restore_column_map(table->read_set, old_map);
  if (unlikely(error))
  {
    m_part_info->err_value= func_value;
    goto exit;
  }
  if (!bitmap_is_set(&(m_part_info->lock_partitions), part_id))
  {
    DBUG_PRINT("info", ("Write to non-locked partition %u (func_value: %ld)",
                        part_id, (long) func_value));
    error= HA_ERR_NOT_IN_LOCK_PARTITIONS;
    goto exit;
  }
  m_last_part= part_id;
  DBUG_PRINT("info", ("Insert in partition %u", part_id));
  start_part_bulk_insert(thd, part_id);

  tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
  error= m_file[part_id]->ha_write_row(buf);
  if (have_auto_increment && !table->s->next_number_keypart)
    set_auto_increment_if_higher(table->next_number_field);
  reenable_binlog(thd);

exit:
  thd->variables.sql_mode= saved_sql_mode;
  table->auto_increment_field_not_null= saved_auto_inc_field_not_null;
  DBUG_RETURN(error);
}


/*
  Update an existing row

  SYNOPSIS
    update_row()
    old_data                 Old record in MySQL Row Format
    new_data                 New record in MySQL Row Format

  RETURN VALUE
    >0                         Error code
    0                          Success

  DESCRIPTION
    Yes, update_row() does what you expect, it updates a row. old_data will
    have the previous row record in it, while new_data will have the newest
    data in it.
    Keep in mind that the server can do updates based on ordering if an
    ORDER BY clause was used. Consecutive ordering is not guarenteed.

    Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.
    new_data is always record[0]
    old_data is always record[1]
*/

int ha_partition::update_row(const uchar *old_data, const uchar *new_data)
{
  THD *thd= ha_thd();
  uint32 new_part_id, old_part_id= m_last_part;
  int error= 0;
  DBUG_ENTER("ha_partition::update_row");
  m_err_rec= NULL;

  // Need to read partition-related columns, to locate the row's partition:
  DBUG_ASSERT(bitmap_is_subset(&m_part_info->full_part_field_set,
                               table->read_set));
#ifndef DBUG_OFF
  /*
    The protocol for updating a row is:
    1) position the handler (cursor) on the row to be updated,
       either through the last read row (rnd or index) or by rnd_pos.
    2) call update_row with both old and new full records as arguments.

    This means that m_last_part should already be set to actual partition
    where the row was read from. And if that is not the same as the
    calculated part_id we found a misplaced row, we return an error to
    notify the user that something is broken in the row distribution
    between partitions! Since we don't check all rows on read, we return an
    error instead of correcting m_last_part, to make the user aware of the
    problem!

    Notice that HA_READ_BEFORE_WRITE_REMOVAL does not require this protocol,
    so this is not supported for this engine.
  */
  error= get_part_for_buf(old_data, m_rec0, m_part_info, &old_part_id);
  DBUG_ASSERT(!error);
  DBUG_ASSERT(old_part_id == m_last_part);
  DBUG_ASSERT(bitmap_is_set(&(m_part_info->read_partitions), old_part_id));
#endif

  if (unlikely((error= get_part_for_buf(new_data, m_rec0, m_part_info,
                                        &new_part_id))))
    goto exit;
  if (unlikely(!bitmap_is_set(&(m_part_info->lock_partitions), new_part_id)))
  {
    error= HA_ERR_NOT_IN_LOCK_PARTITIONS;
    goto exit;
  }


  m_last_part= new_part_id;
  start_part_bulk_insert(thd, new_part_id);
  if (new_part_id == old_part_id)
  {
    DBUG_PRINT("info", ("Update in partition %u", (uint) new_part_id));
    tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
    error= m_file[new_part_id]->ha_update_row(old_data, new_data);
    reenable_binlog(thd);
    goto exit;
  }
  else
  {
    Field *saved_next_number_field= table->next_number_field;
    /*
      Don't allow generation of auto_increment value for update.
      table->next_number_field is never set on UPDATE.
      But is set for INSERT ... ON DUPLICATE KEY UPDATE,
      and since update_row() does not generate or update an auto_inc value,
      we cannot have next_number_field set when moving a row
      to another partition with write_row(), since that could
      generate/update the auto_inc value.
      This gives the same behavior for partitioned vs non partitioned tables.
    */
    table->next_number_field= NULL;
    DBUG_PRINT("info", ("Update from partition %u to partition %u",
			(uint) old_part_id, (uint) new_part_id));
    tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
    error= m_file[new_part_id]->ha_write_row((uchar*) new_data);
    reenable_binlog(thd);
    table->next_number_field= saved_next_number_field;
    if (unlikely(error))
      goto exit;

    tmp_disable_binlog(thd); /* Do not replicate the low-level changes. */
    error= m_file[old_part_id]->ha_delete_row(old_data);
    reenable_binlog(thd);
    if (unlikely(error))
      goto exit;
  }

exit:
  /*
    if updating an auto_increment column, update
    part_share->next_auto_inc_val if needed.
    (not to be used if auto_increment on secondary field in a multi-column
    index)
    mysql_update does not set table->next_number_field, so we use
    table->found_next_number_field instead.
    Also checking that the field is marked in the write set.
  */
  if (table->found_next_number_field &&
      new_data == table->record[0] &&
      !table->s->next_number_keypart &&
      bitmap_is_set(table->write_set,
                    table->found_next_number_field->field_index))
  {
    update_next_auto_inc_val();
    /*
      The following call is safe as part_share->auto_inc_initialized
      (tested in the call) is guaranteed to be set for update statements.
    */
    set_auto_increment_if_higher(table->found_next_number_field);
  }
  DBUG_RETURN(error);
}


/*
  Remove an existing row

  SYNOPSIS
    delete_row
    buf                      Deleted row in MySQL Row Format

  RETURN VALUE
    >0                       Error Code
    0                        Success

  DESCRIPTION
    This will delete a row. buf will contain a copy of the row to be deleted.
    The server will call this right after the current row has been read
    (from either a previous rnd_xxx() or index_xxx() call).
    If you keep a pointer to the last row or can access a primary key it will
    make doing the deletion quite a bit easier.
    Keep in mind that the server does no guarentee consecutive deletions.
    ORDER BY clauses can be used.

    Called in sql_acl.cc and sql_udf.cc to manage internal table information.
    Called in sql_delete.cc, sql_insert.cc, and sql_select.cc. In sql_select
    it is used for removing duplicates while in insert it is used for REPLACE
    calls.

    buf is either record[0] or record[1]
*/

int ha_partition::delete_row(const uchar *buf)
{
  int error;
  THD *thd= ha_thd();
  DBUG_ENTER("ha_partition::delete_row");
  m_err_rec= NULL;

  DBUG_ASSERT(bitmap_is_subset(&m_part_info->full_part_field_set,
                               table->read_set));
#ifndef DBUG_OFF
  /*
    The protocol for deleting a row is:
    1) position the handler (cursor) on the row to be deleted,
       either through the last read row (rnd or index) or by rnd_pos.
    2) call delete_row with the full record as argument.

    This means that m_last_part should already be set to actual partition
    where the row was read from. And if that is not the same as the
    calculated part_id we found a misplaced row, we return an error to
    notify the user that something is broken in the row distribution
    between partitions! Since we don't check all rows on read, we return an
    error instead of forwarding the delete to the correct (m_last_part)
    partition!

    Notice that HA_READ_BEFORE_WRITE_REMOVAL does not require this protocol,
    so this is not supported for this engine.

    For partitions by system_time, get_part_for_buf() is always either current
    or last historical partition, but DELETE HISTORY can delete from any
    historical partition. So, skip the check in this case.
  */
  if (!thd->lex->vers_conditions.is_set()) // if not DELETE HISTORY
  {
    uint32 part_id;
    error= get_part_for_buf(buf, m_rec0, m_part_info, &part_id);
    DBUG_ASSERT(!error);
    DBUG_ASSERT(part_id == m_last_part);
  }
  DBUG_ASSERT(bitmap_is_set(&(m_part_info->read_partitions), m_last_part));
  DBUG_ASSERT(bitmap_is_set(&(m_part_info->lock_partitions), m_last_part));
#endif

  if (!bitmap_is_set(&(m_part_info->lock_partitions), m_last_part))
    DBUG_RETURN(HA_ERR_NOT_IN_LOCK_PARTITIONS);

  tmp_disable_binlog(thd);
  error= m_file[m_last_part]->ha_delete_row(buf);
  reenable_binlog(thd);
  DBUG_RETURN(error);
}


/*
  Delete all rows in a table

  SYNOPSIS
    delete_all_rows()

  RETURN VALUE
    >0                       Error Code
    0                        Success

  DESCRIPTION
    Used to delete all rows in a table. Both for cases of truncate and
    for cases where the optimizer realizes that all rows will be
    removed as a result of a SQL statement.

    Called from item_sum.cc by Item_func_group_concat::clear(),
    Item_sum_count::clear(), and Item_func_group_concat::clear().
    Called from sql_delete.cc by mysql_delete().
    Called from sql_select.cc by JOIN::reset().
    Called from sql_union.cc by st_select_lex_unit::exec().
*/

int ha_partition::delete_all_rows()
{
  int error;
  uint i;
  DBUG_ENTER("ha_partition::delete_all_rows");

  for (i= bitmap_get_first_set(&m_part_info->read_partitions);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
  {
    /* Can be pruned, like DELETE FROM t PARTITION (pX) */
    if (unlikely((error= m_file[i]->ha_delete_all_rows())))
      DBUG_RETURN(error);
  }
  DBUG_RETURN(0);
}


/**
  Manually truncate the table.

  @retval  0    Success.
  @retval  > 0  Error code.
*/

int ha_partition::truncate()
{
  int error;
  handler **file;
  DBUG_ENTER("ha_partition::truncate");

  /*
    TRUNCATE also means resetting auto_increment. Hence, reset
    it so that it will be initialized again at the next use.
  */
  lock_auto_increment();
  part_share->next_auto_inc_val= 0;
  part_share->auto_inc_initialized= false;
  unlock_auto_increment();

  file= m_file;
  do
  {
    if (unlikely((error= (*file)->ha_truncate())))
      DBUG_RETURN(error);
  } while (*(++file));
  DBUG_RETURN(0);
}


/**
  Truncate a set of specific partitions.

  @remark Auto increment value will be truncated in that partition as well!

  ALTER TABLE t TRUNCATE PARTITION ...
*/

int ha_partition::truncate_partition(Alter_info *alter_info, bool *binlog_stmt)
{
  int error= 0;
  List_iterator<partition_element> part_it(m_part_info->partitions);
  uint num_parts= m_part_info->num_parts;
  uint num_subparts= m_part_info->num_subparts;
  uint i= 0;
  DBUG_ENTER("ha_partition::truncate_partition");

  /* Only binlog when it starts any call to the partitions handlers */
  *binlog_stmt= false;

  if (set_part_state(alter_info, m_part_info, PART_ADMIN))
    DBUG_RETURN(HA_ERR_NO_PARTITION_FOUND);

  /*
    TRUNCATE also means resetting auto_increment. Hence, reset
    it so that it will be initialized again at the next use.
  */
  lock_auto_increment();
  part_share->next_auto_inc_val= 0;
  part_share->auto_inc_initialized= FALSE;
  unlock_auto_increment();

  *binlog_stmt= true;

  do
  {
    partition_element *part_elem= part_it++;
    if (part_elem->part_state == PART_ADMIN)
    {
      if (m_is_sub_partitioned)
      {
        List_iterator<partition_element>
                                    subpart_it(part_elem->subpartitions);
        partition_element *sub_elem;
        uint j= 0, part;
        do
        {
          sub_elem= subpart_it++;
          part= i * num_subparts + j;
          DBUG_PRINT("info", ("truncate subpartition %u (%s)",
                              part, sub_elem->partition_name));
          if (unlikely((error= m_file[part]->ha_truncate())))
            break;
          sub_elem->part_state= PART_NORMAL;
        } while (++j < num_subparts);
      }
      else
      {
        DBUG_PRINT("info", ("truncate partition %u (%s)", i,
                            part_elem->partition_name));
        error= m_file[i]->ha_truncate();
      }
      part_elem->part_state= PART_NORMAL;
    }
  } while (!error && (++i < num_parts));
  DBUG_RETURN(error);
}


/*
  Start a large batch of insert rows

  SYNOPSIS
    start_bulk_insert()
    rows                  Number of rows to insert
    flags                 Flags to control index creation

  RETURN VALUE
    NONE

  DESCRIPTION
    rows == 0 means we will probably insert many rows
*/
void ha_partition::start_bulk_insert(ha_rows rows, uint flags)
{
  DBUG_ENTER("ha_partition::start_bulk_insert");

  m_bulk_inserted_rows= 0;
  bitmap_clear_all(&m_bulk_insert_started);
  /* use the last bit for marking if bulk_insert_started was called */
  bitmap_set_bit(&m_bulk_insert_started, m_tot_parts);
  DBUG_VOID_RETURN;
}


/*
  Check if start_bulk_insert has been called for this partition,
  if not, call it and mark it called
*/
void ha_partition::start_part_bulk_insert(THD *thd, uint part_id)
{
  long old_buffer_size;
  if (!bitmap_is_set(&m_bulk_insert_started, part_id) &&
      bitmap_is_set(&m_bulk_insert_started, m_tot_parts))
  {
    DBUG_ASSERT(bitmap_is_set(&(m_part_info->lock_partitions), part_id));
    old_buffer_size= thd->variables.read_buff_size;
    /* Update read_buffer_size for this partition */
    thd->variables.read_buff_size= estimate_read_buffer_size(old_buffer_size);
    m_file[part_id]->ha_start_bulk_insert(guess_bulk_insert_rows());
    bitmap_set_bit(&m_bulk_insert_started, part_id);
    thd->variables.read_buff_size= old_buffer_size;
  }
  m_bulk_inserted_rows++;
}

/*
  Estimate the read buffer size for each partition.
  SYNOPSIS
    ha_partition::estimate_read_buffer_size()
    original_size  read buffer size originally set for the server
  RETURN VALUE
    estimated buffer size.
  DESCRIPTION
    If the estimated number of rows to insert is less than 10 (but not 0)
    the new buffer size is same as original buffer size.
    In case of first partition of when partition function is monotonic
    new buffer size is same as the original buffer size.
    For rest of the partition total buffer of 10*original_size is divided
    equally if number of partition is more than 10 other wise each partition
    will be allowed to use original buffer size.
*/
long ha_partition::estimate_read_buffer_size(long original_size)
{
  /*
    If number of rows to insert is less than 10, but not 0,
    return original buffer size.
  */
  if (estimation_rows_to_insert && (estimation_rows_to_insert < 10))
    return (original_size);
  /*
    If first insert/partition and monotonic partition function,
    allow using buffer size originally set.
   */
  if (!m_bulk_inserted_rows &&
      m_part_func_monotonicity_info != NON_MONOTONIC &&
      m_tot_parts > 1)
    return original_size;
  /*
    Allow total buffer used in all partition to go up to 10*read_buffer_size.
    11*read_buffer_size in case of monotonic partition function.
  */

  if (m_tot_parts < 10)
      return original_size;
  return (original_size * 10 / m_tot_parts);
}

/*
  Try to predict the number of inserts into this partition.

  If less than 10 rows (including 0 which means Unknown)
    just give that as a guess
  If monotonic partitioning function was used
    guess that 50 % of the inserts goes to the first partition
  For all other cases, guess on equal distribution between the partitions
*/
ha_rows ha_partition::guess_bulk_insert_rows()
{
  DBUG_ENTER("guess_bulk_insert_rows");

  if (estimation_rows_to_insert < 10)
    DBUG_RETURN(estimation_rows_to_insert);

  /* If first insert/partition and monotonic partition function, guess 50%.  */
  if (!m_bulk_inserted_rows &&
      m_part_func_monotonicity_info != NON_MONOTONIC &&
      m_tot_parts > 1)
    DBUG_RETURN(estimation_rows_to_insert / 2);

  /* Else guess on equal distribution (+1 is to avoid returning 0/Unknown) */
  if (m_bulk_inserted_rows < estimation_rows_to_insert)
    DBUG_RETURN(((estimation_rows_to_insert - m_bulk_inserted_rows)
                / m_tot_parts) + 1);
  /* The estimation was wrong, must say 'Unknown' */
  DBUG_RETURN(0);
}


/*
  Finish a large batch of insert rows

  SYNOPSIS
    end_bulk_insert()

  RETURN VALUE
    >0                      Error code
    0                       Success

  Note: end_bulk_insert can be called without start_bulk_insert
        being called, see bug#44108.

*/

int ha_partition::end_bulk_insert()
{
  int error= 0;
  uint i;
  DBUG_ENTER("ha_partition::end_bulk_insert");

  if (!bitmap_is_set(&m_bulk_insert_started, m_tot_parts))
    DBUG_RETURN(error);

  for (i= bitmap_get_first_set(&m_bulk_insert_started);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_bulk_insert_started, i))
  {
    int tmp;
    if ((tmp= m_file[i]->ha_end_bulk_insert()))
      error= tmp;
  }
  bitmap_clear_all(&m_bulk_insert_started);
  DBUG_RETURN(error);
}


/****************************************************************************
                MODULE full table scan
****************************************************************************/
/*
  Initialize engine for random reads

  SYNOPSIS
    ha_partition::rnd_init()
    scan	0  Initialize for random reads through rnd_pos()
		1  Initialize for random scan through rnd_next()

  RETURN VALUE
    >0          Error code
    0           Success

  DESCRIPTION
    rnd_init() is called when the server wants the storage engine to do a
    table scan or when the server wants to access data through rnd_pos.

    When scan is used we will scan one handler partition at a time.
    When preparing for rnd_pos we will init all handler partitions.
    No extra cache handling is needed when scannning is not performed.

    Before initialising we will call rnd_end to ensure that we clean up from
    any previous incarnation of a table scan.
    Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
    sql_table.cc, and sql_update.cc.
*/

int ha_partition::rnd_init(bool scan)
{
  int error;
  uint i= 0;
  uint32 part_id;
  DBUG_ENTER("ha_partition::rnd_init");

  /*
    For operations that may need to change data, we may need to extend
    read_set.
  */
  if (get_lock_type() == F_WRLCK)
  {
    /*
      If write_set contains any of the fields used in partition and
      subpartition expression, we need to set all bits in read_set because
      the row may need to be inserted in a different [sub]partition. In
      other words update_row() can be converted into write_row(), which
      requires a complete record.
    */
    if (bitmap_is_overlapping(&m_part_info->full_part_field_set,
                              table->write_set))
    {
      DBUG_PRINT("info", ("partition set full bitmap"));
      bitmap_set_all(table->read_set);
    }
    else
    {
      /*
        Some handlers only read fields as specified by the bitmap for the
        read set. For partitioned handlers we always require that the
        fields of the partition functions are read such that we can
        calculate the partition id to place updated and deleted records.
      */
      DBUG_PRINT("info", ("partition set part_field bitmap"));
      bitmap_union(table->read_set, &m_part_info->full_part_field_set);
    }
  }

  /* Now we see what the index of our first important partition is */
  DBUG_PRINT("info", ("m_part_info->read_partitions: %p",
                      m_part_info->read_partitions.bitmap));
  part_id= bitmap_get_first_set(&(m_part_info->read_partitions));
  DBUG_PRINT("info", ("m_part_spec.start_part: %u", (uint) part_id));

  if (part_id == MY_BIT_NONE)
  {
    error= 0;
    goto err1;
  }

  /*
    We have a partition and we are scanning with rnd_next
    so we bump our cache
  */
  DBUG_PRINT("info", ("rnd_init on partition: %u", (uint) part_id));
  if (scan)
  {
    /*
      rnd_end() is needed for partitioning to reset internal data if scan
      is already in use
    */
    rnd_end();
    late_extra_cache(part_id);

    m_index_scan_type= partition_no_index_scan;
  }

  for (i= part_id;
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
  {
    if (unlikely((error= m_file[i]->ha_rnd_init(scan))))
      goto err;
  }

  m_scan_value= scan;
  m_part_spec.start_part= part_id;
  m_part_spec.end_part= m_tot_parts - 1;
  m_rnd_init_and_first= TRUE;
  DBUG_PRINT("info", ("m_scan_value: %u", m_scan_value));
  DBUG_RETURN(0);

err:
  if (scan)
    late_extra_no_cache(part_id);

  /* Call rnd_end for all previously inited partitions. */
  for (;
       part_id < i;
       part_id= bitmap_get_next_set(&m_part_info->read_partitions, part_id))
  {
    m_file[part_id]->ha_rnd_end();
  }
err1:
  m_scan_value= 2;
  m_part_spec.start_part= NO_CURRENT_PART_ID;
  DBUG_RETURN(error);
}


/*
  End of a table scan

  SYNOPSIS
    rnd_end()

  RETURN VALUE
    >0          Error code
    0           Success
*/

int ha_partition::rnd_end()
{
  DBUG_ENTER("ha_partition::rnd_end");
  switch (m_scan_value) {
  case 2:                                       // Error
    break;
  case 1:                                       // Table scan
    if (m_part_spec.start_part != NO_CURRENT_PART_ID)
      late_extra_no_cache(m_part_spec.start_part);
    /* fall through */
  case 0:
    uint i;
    for (i= bitmap_get_first_set(&m_part_info->read_partitions);
         i < m_tot_parts;
         i= bitmap_get_next_set(&m_part_info->read_partitions, i))
    {
      m_file[i]->ha_rnd_end();
    }
    break;
  }
  m_scan_value= 2;
  m_part_spec.start_part= NO_CURRENT_PART_ID;
  DBUG_RETURN(0);
}


/*
  read next row during full table scan (scan in random row order)

  SYNOPSIS
    rnd_next()
    buf		buffer that should be filled with data

  RETURN VALUE
    >0          Error code
    0           Success

  DESCRIPTION
    This is called for each row of the table scan. When you run out of records
    you should return HA_ERR_END_OF_FILE.
    The Field structure for the table is the key to getting data into buf
    in a manner that will allow the server to understand it.

    Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
    sql_table.cc, and sql_update.cc.
*/

int ha_partition::rnd_next(uchar *buf)
{
  handler *file;
  int result= HA_ERR_END_OF_FILE, error;
  uint part_id= m_part_spec.start_part;
  DBUG_ENTER("ha_partition::rnd_next");
  DBUG_PRINT("enter", ("partition this: %p", this));

  /* upper level will increment this once again at end of call */
  decrement_statistics(&SSV::ha_read_rnd_next_count);

  if (part_id == NO_CURRENT_PART_ID)
  {
    /*
      The original set of partitions to scan was empty and thus we report
      the result here.
    */
    goto end;
  }

  DBUG_ASSERT(m_scan_value == 1);

  if (m_rnd_init_and_first)
  {
    m_rnd_init_and_first= FALSE;
    error= handle_pre_scan(FALSE, check_parallel_search());
    if (m_pre_calling || error)
      DBUG_RETURN(error);
  }

  file= m_file[part_id];

  while (TRUE)
  {
    result= file->ha_rnd_next(buf);
    if (!result)
    {
      m_last_part= part_id;
      DBUG_PRINT("info", ("partition m_last_part: %u", (uint) m_last_part));
      m_part_spec.start_part= part_id;
      table->status= 0;
      DBUG_RETURN(0);
    }

    /*
      if we get here, then the current partition ha_rnd_next returned failure
    */
    if (result != HA_ERR_END_OF_FILE)
      goto end_dont_reset_start_part;         // Return error

    /* End current partition */
    late_extra_no_cache(part_id);
    /* Shift to next partition */
    part_id= bitmap_get_next_set(&m_part_info->read_partitions, part_id);
    if (part_id >= m_tot_parts)
    {
      result= HA_ERR_END_OF_FILE;
      break;
    }
    m_last_part= part_id;
    DBUG_PRINT("info", ("partition m_last_part: %u", (uint) m_last_part));
    m_part_spec.start_part= part_id;
    file= m_file[part_id];
    late_extra_cache(part_id);
  }

end:
  DBUG_PRINT("exit", ("reset start_part"));
  m_part_spec.start_part= NO_CURRENT_PART_ID;
end_dont_reset_start_part:
  DBUG_RETURN(result);
}


/*
  Save position of current row

  SYNOPSIS
    position()
    record             Current record in MySQL Row Format

  RETURN VALUE
    NONE

  DESCRIPTION
    position() is called after each call to rnd_next() if the data needs
    to be ordered. You can do something like the following to store
    the position:
    ha_store_ptr(ref, ref_length, current_position);

    The server uses ref to store data. ref_length in the above case is
    the size needed to store current_position. ref is just a byte array
    that the server will maintain. If you are using offsets to mark rows, then
    current_position should be the offset. If it is a primary key like in
    BDB, then it needs to be a primary key.

    Called from filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc.
*/

void ha_partition::position(const uchar *record)
{
  handler *file= m_file[m_last_part];
  size_t pad_length;
  DBUG_ASSERT(bitmap_is_set(&(m_part_info->read_partitions), m_last_part));
  DBUG_ENTER("ha_partition::position");

  file->position(record);
  int2store(ref, m_last_part);
  memcpy((ref + PARTITION_BYTES_IN_POS), file->ref, file->ref_length);
  pad_length= m_ref_length - PARTITION_BYTES_IN_POS - file->ref_length;
  if (pad_length)
    memset((ref + PARTITION_BYTES_IN_POS + file->ref_length), 0, pad_length);

  DBUG_VOID_RETURN;
}


/*
  Read row using position

  SYNOPSIS
    rnd_pos()
    out:buf                     Row read in MySQL Row Format
    position                    Position of read row

  RETURN VALUE
    >0                          Error code
    0                           Success

  DESCRIPTION
    This is like rnd_next, but you are given a position to use
    to determine the row. The position will be of the type that you stored in
    ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
    or position you saved when position() was called.
    Called from filesort.cc records.cc sql_insert.cc sql_select.cc
    sql_update.cc.
*/

int ha_partition::rnd_pos(uchar * buf, uchar *pos)
{
  uint part_id;
  handler *file;
  DBUG_ENTER("ha_partition::rnd_pos");
  decrement_statistics(&SSV::ha_read_rnd_count);

  part_id= uint2korr((const uchar *) pos);
  DBUG_ASSERT(part_id < m_tot_parts);
  file= m_file[part_id];
  DBUG_ASSERT(bitmap_is_set(&(m_part_info->read_partitions), part_id));
  m_last_part= part_id;
  DBUG_RETURN(file->ha_rnd_pos(buf, (pos + PARTITION_BYTES_IN_POS)));
}


/*
  Read row using position using given record to find

  SYNOPSIS
    rnd_pos_by_record()
    record             Current record in MySQL Row Format

  RETURN VALUE
    >0                 Error code
    0                  Success

  DESCRIPTION
    this works as position()+rnd_pos() functions, but does some extra work,
    calculating m_last_part - the partition to where the 'record'
    should go.

    called from replication (log_event.cc)
*/

int ha_partition::rnd_pos_by_record(uchar *record)
{
  DBUG_ENTER("ha_partition::rnd_pos_by_record");

  if (unlikely(get_part_for_buf(record, m_rec0, m_part_info, &m_last_part)))
    DBUG_RETURN(1);

  int err= m_file[m_last_part]->rnd_pos_by_record(record);
  DBUG_RETURN(err);
}


/****************************************************************************
                MODULE index scan
****************************************************************************/
/*
  Positions an index cursor to the index specified in the handle. Fetches the
  row if available. If the key value is null, begin at the first key of the
  index.

  There are loads of optimisations possible here for the partition handler.
  The same optimisations can also be checked for full table scan although
  only through conditions and not from index ranges.
  Phase one optimisations:
    Check if the fields of the partition function are bound. If so only use
    the single partition it becomes bound to.
  Phase two optimisations:
    If it can be deducted through range or list partitioning that only a
    subset of the partitions are used, then only use those partitions.
*/


/**
  Setup the ordered record buffer and the priority queue.
*/

bool ha_partition::init_record_priority_queue()
{
  DBUG_ENTER("ha_partition::init_record_priority_queue");
  DBUG_ASSERT(!m_ordered_rec_buffer);
  /*
    Initialize the ordered record buffer.
  */
  if (!m_ordered_rec_buffer)
  {
    size_t alloc_len;
    uint used_parts= bitmap_bits_set(&m_part_info->read_partitions);
    DBUG_ASSERT(used_parts > 0);
    /* Allocate record buffer for each used partition. */
    m_priority_queue_rec_len= m_rec_length + PARTITION_BYTES_IN_POS;
    if (!m_using_extended_keys)
       m_priority_queue_rec_len += get_open_file_sample()->ref_length;
    alloc_len= used_parts * m_priority_queue_rec_len;
    /* Allocate a key for temporary use when setting up the scan. */
    alloc_len+= table_share->max_key_length;

    if (!(m_ordered_rec_buffer= (uchar*)my_malloc(alloc_len, MYF(MY_WME))))
      DBUG_RETURN(true);

    /*
      We set-up one record per partition and each record has 2 bytes in
      front where the partition id is written. This is used by ordered
      index_read.
      We also set-up a reference to the first record for temporary use in
      setting up the scan.
    */
    char *ptr= (char*) m_ordered_rec_buffer;
    uint i;
    for (i= bitmap_get_first_set(&m_part_info->read_partitions);
         i < m_tot_parts;
         i= bitmap_get_next_set(&m_part_info->read_partitions, i))
    {
      DBUG_PRINT("info", ("init rec-buf for part %u", i));
      int2store(ptr, i);
      ptr+= m_priority_queue_rec_len;
    }
    m_start_key.key= (const uchar*)ptr;

    /* Initialize priority queue, initialized to reading forward. */
    int (*cmp_func)(void *, uchar *, uchar *);
    void *cmp_arg= (void*) this;
    if (!m_using_extended_keys && !(table_flags() & HA_CMP_REF_IS_EXPENSIVE))
      cmp_func= cmp_key_rowid_part_id;
    else
      cmp_func= cmp_key_part_id;
    DBUG_PRINT("info", ("partition queue_init(1) used_parts: %u", used_parts));
    if (init_queue(&m_queue, used_parts, 0, 0, cmp_func, cmp_arg, 0, 0))
    {
      my_free(m_ordered_rec_buffer);
      m_ordered_rec_buffer= NULL;
      DBUG_RETURN(true);
    }
  }
  DBUG_RETURN(false);
}


/**
  Destroy the ordered record buffer and the priority queue.
*/

void ha_partition::destroy_record_priority_queue()
{
  DBUG_ENTER("ha_partition::destroy_record_priority_queue");
  if (m_ordered_rec_buffer)
  {
    delete_queue(&m_queue);
    my_free(m_ordered_rec_buffer);
    m_ordered_rec_buffer= NULL;
  }
  DBUG_VOID_RETURN;
}


/*
  Initialize handler before start of index scan

  SYNOPSIS
    index_init()
    inx                Index number
    sorted             Is rows to be returned in sorted order

  RETURN VALUE
    >0                 Error code
    0                  Success

  DESCRIPTION
    index_init is always called before starting index scans (except when
    starting through index_read_idx and using read_range variants).
*/

int ha_partition::index_init(uint inx, bool sorted)
{
  int error= 0;
  uint i;
  DBUG_ENTER("ha_partition::index_init");
  DBUG_PRINT("enter", ("partition this: %p  inx: %u  sorted: %u", this, inx, sorted));

  active_index= inx;
  m_part_spec.start_part= NO_CURRENT_PART_ID;
  m_start_key.length= 0;
  m_ordered= sorted;
  m_ordered_scan_ongoing= FALSE;
  m_curr_key_info[0]= table->key_info+inx;
  if (m_pkey_is_clustered && table->s->primary_key != MAX_KEY)
  {
    /*
      if PK is clustered, then the key cmp must use the pk to
      differentiate between equal key in given index.
    */
    DBUG_PRINT("info", ("Clustered pk, using pk as secondary cmp"));
    m_curr_key_info[1]= table->key_info+table->s->primary_key;
    m_curr_key_info[2]= NULL;
    m_using_extended_keys= TRUE;
  }
  else
  {
    m_curr_key_info[1]= NULL;
    m_using_extended_keys= FALSE;
  }

  if (init_record_priority_queue())
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  /*
    Some handlers only read fields as specified by the bitmap for the
    read set. For partitioned handlers we always require that the
    fields of the partition functions are read such that we can
    calculate the partition id to place updated and deleted records.
    But this is required for operations that may need to change data only.
  */
  if (get_lock_type() == F_WRLCK)
  {
    DBUG_PRINT("info", ("partition set part_field bitmap"));
    bitmap_union(table->read_set, &m_part_info->full_part_field_set);
  }
  if (sorted)
  {
    /*
      An ordered scan is requested. We must make sure all fields of the
      used index are in the read set, as partitioning requires them for
      sorting (see ha_partition::handle_ordered_index_scan).

      The SQL layer may request an ordered index scan without having index
      fields in the read set when
       - it needs to do an ordered scan over an index prefix.
       - it evaluates ORDER BY with SELECT COUNT(*) FROM t1.

      TODO: handle COUNT(*) queries via unordered scan.
    */
    KEY **key_info= m_curr_key_info;
    do
    {
      for (i= 0; i < (*key_info)->user_defined_key_parts; i++)
        bitmap_set_bit(table->read_set,
                       (*key_info)->key_part[i].field->field_index);
    } while (*(++key_info));
  }
  for (i= bitmap_get_first_set(&m_part_info->read_partitions);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
  {
    if (unlikely((error= m_file[i]->ha_index_init(inx, sorted))))
      goto err;

    DBUG_EXECUTE_IF("ha_partition_fail_index_init", {
      i++;
      error= HA_ERR_NO_PARTITION_FOUND;
      goto err;
    });
  }
err:
  if (unlikely(error))
  {
    /* End the previously initialized indexes. */
    uint j;
    for (j= bitmap_get_first_set(&m_part_info->read_partitions);
         j < i;
         j= bitmap_get_next_set(&m_part_info->read_partitions, j))
    {
      (void) m_file[j]->ha_index_end();
    }
    destroy_record_priority_queue();
  }
  DBUG_RETURN(error);
}


/*
  End of index scan

  SYNOPSIS
    index_end()

  RETURN VALUE
    >0                 Error code
    0                  Success

  DESCRIPTION
    index_end is called at the end of an index scan to clean up any
    things needed to clean up.
*/

int ha_partition::index_end()
{
  int error= 0;
  handler **file;
  DBUG_ENTER("ha_partition::index_end");

  active_index= MAX_KEY;
  m_part_spec.start_part= NO_CURRENT_PART_ID;
  file= m_file;
  do
  {
    if ((*file)->inited == INDEX)
    {
      int tmp;
      if ((tmp= (*file)->ha_index_end()))
        error= tmp;
    }
  } while (*(++file));
  destroy_record_priority_queue();
  DBUG_RETURN(error);
}


/*
  Read one record in an index scan and start an index scan

  SYNOPSIS
    index_read_map()
    buf                    Read row in MySQL Row Format
    key                    Key parts in consecutive order
    keypart_map            Which part of key is used
    find_flag              What type of key condition is used

  RETURN VALUE
    >0                 Error code
    0                  Success

  DESCRIPTION
    index_read_map starts a new index scan using a start key. The MySQL Server
    will check the end key on its own. Thus to function properly the
    partitioned handler need to ensure that it delivers records in the sort
    order of the MySQL Server.
    index_read_map can be restarted without calling index_end on the previous
    index scan and without calling index_init. In this case the index_read_map
    is on the same index as the previous index_scan. This is particularly
    used in conjuntion with multi read ranges.
*/

int ha_partition::index_read_map(uchar *buf, const uchar *key,
                                 key_part_map keypart_map,
                                 enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_partition::index_read_map");
  decrement_statistics(&SSV::ha_read_key_count);
  end_range= 0;
  m_index_scan_type= partition_index_read;
  m_start_key.key= key;
  m_start_key.keypart_map= keypart_map;
  m_start_key.flag= find_flag;
  DBUG_RETURN(common_index_read(buf, TRUE));
}


/* Compare two part_no partition numbers */
static int cmp_part_ids(uchar *ref1, uchar *ref2)
{
  uint32 diff2= uint2korr(ref2);
  uint32 diff1= uint2korr(ref1);
  if (diff2 > diff1)
    return -1;
  if (diff2 < diff1)
    return 1;
  return 0;
}


/*
  @brief
    Provide ordering by (key_value, part_no).
*/

extern "C" int cmp_key_part_id(void *ptr, uchar *ref1, uchar *ref2)
{
  ha_partition *file= (ha_partition*)ptr;
  int res;
  if ((res= key_rec_cmp(file->m_curr_key_info, ref1 + PARTITION_BYTES_IN_POS,
                        ref2 + PARTITION_BYTES_IN_POS)))
  {
    return res;
  }
  return cmp_part_ids(ref1, ref2);
}

/*
  @brief
    Provide ordering by (key_value, underying_table_rowid, part_no).
*/
extern "C" int cmp_key_rowid_part_id(void *ptr, uchar *ref1, uchar *ref2)
{
  ha_partition *file= (ha_partition*)ptr;
  int res;

  if ((res= key_rec_cmp(file->m_curr_key_info, ref1 + PARTITION_BYTES_IN_POS,
                        ref2 + PARTITION_BYTES_IN_POS)))
  {
    return res;
  }
  if ((res= file->m_file[0]->cmp_ref(ref1 + PARTITION_BYTES_IN_POS + file->m_rec_length,
                                     ref2 + PARTITION_BYTES_IN_POS + file->m_rec_length)))
  {
    return res;
  }
  return cmp_part_ids(ref1, ref2);
}


/**
  Common routine for a number of index_read variants

  @param buf             Buffer where the record should be returned.
  @param have_start_key  TRUE <=> the left endpoint is available, i.e.
                         we're in index_read call or in read_range_first
                         call and the range has left endpoint.
                         FALSE <=> there is no left endpoint (we're in
                         read_range_first() call and the range has no left
                         endpoint).

  @return Operation status
    @retval 0      OK
    @retval HA_ERR_END_OF_FILE   Whole index scanned, without finding the record.
    @retval HA_ERR_KEY_NOT_FOUND Record not found, but index cursor positioned.
    @retval other  error code.

  @details
    Start scanning the range (when invoked from read_range_first()) or doing
    an index lookup (when invoked from index_read_XXX):
     - If possible, perform partition selection
     - Find the set of partitions we're going to use
     - Depending on whether we need ordering:
        NO:  Get the first record from first used partition (see
             handle_unordered_scan_next_partition)
        YES: Fill the priority queue and get the record that is the first in
             the ordering
*/

int ha_partition::common_index_read(uchar *buf, bool have_start_key)
{
  int error;
  uint UNINIT_VAR(key_len); /* used if have_start_key==TRUE */
  bool reverse_order= FALSE;
  DBUG_ENTER("ha_partition::common_index_read");

  DBUG_PRINT("info", ("m_ordered %u m_ordered_scan_ong %u",
                      m_ordered, m_ordered_scan_ongoing));

  if (have_start_key)
  {
    m_start_key.length= key_len= calculate_key_len(table, active_index,
                                                   m_start_key.key,
                                                   m_start_key.keypart_map);
    DBUG_PRINT("info", ("have_start_key map %lu find_flag %u len %u",
                        m_start_key.keypart_map, m_start_key.flag, key_len));
    DBUG_ASSERT(key_len);
  }
  if (unlikely((error= partition_scan_set_up(buf, have_start_key))))
  {
    DBUG_RETURN(error);
  }

  if (have_start_key &&
      (m_start_key.flag == HA_READ_PREFIX_LAST ||
       m_start_key.flag == HA_READ_PREFIX_LAST_OR_PREV ||
       m_start_key.flag == HA_READ_BEFORE_KEY))
  {
    reverse_order= TRUE;
    m_ordered_scan_ongoing= TRUE;
  }
  DBUG_PRINT("info", ("m_ordered %u m_o_scan_ong %u have_start_key %u",
                      m_ordered, m_ordered_scan_ongoing, have_start_key));
  if (!m_ordered_scan_ongoing)
   {
    /*
      We use unordered index scan when read_range is used and flag
      is set to not use ordered.
      We also use an unordered index scan when the number of partitions to
      scan is only one.
      The unordered index scan will use the partition set created.
    */
    DBUG_PRINT("info", ("doing unordered scan"));
    error= handle_pre_scan(FALSE, FALSE);
    if (likely(!error))
      error= handle_unordered_scan_next_partition(buf);
  }
  else
  {
    /*
      In all other cases we will use the ordered index scan. This will use
      the partition set created by the get_partition_set method.
    */
    error= handle_ordered_index_scan(buf, reverse_order);
  }
  DBUG_RETURN(error);
}


/*
  Start an index scan from leftmost record and return first record

  SYNOPSIS
    index_first()
    buf                 Read row in MySQL Row Format

  RETURN VALUE
    >0                  Error code
    0                   Success

  DESCRIPTION
    index_first() asks for the first key in the index.
    This is similar to index_read except that there is no start key since
    the scan starts from the leftmost entry and proceeds forward with
    index_next.

    Called from opt_range.cc, opt_sum.cc, sql_handler.cc,
    and sql_select.cc.
*/

int ha_partition::index_first(uchar * buf)
{
  DBUG_ENTER("ha_partition::index_first");
  decrement_statistics(&SSV::ha_read_first_count);

  end_range= 0;
  m_index_scan_type= partition_index_first;
  DBUG_RETURN(common_first_last(buf));
}


/*
  Start an index scan from rightmost record and return first record

  SYNOPSIS
    index_last()
    buf                 Read row in MySQL Row Format

  RETURN VALUE
    >0                  Error code
    0                   Success

  DESCRIPTION
    index_last() asks for the last key in the index.
    This is similar to index_read except that there is no start key since
    the scan starts from the rightmost entry and proceeds forward with
    index_prev.

    Called from opt_range.cc, opt_sum.cc, sql_handler.cc,
    and sql_select.cc.
*/

int ha_partition::index_last(uchar * buf)
{
  DBUG_ENTER("ha_partition::index_last");
  decrement_statistics(&SSV::ha_read_last_count);

  m_index_scan_type= partition_index_last;
  DBUG_RETURN(common_first_last(buf));
}

/*
  Common routine for index_first/index_last

  SYNOPSIS
    ha_partition::common_first_last()

  see index_first for rest
*/

int ha_partition::common_first_last(uchar *buf)
{
  int error;

  if (unlikely((error= partition_scan_set_up(buf, FALSE))))
    return error;
  if (!m_ordered_scan_ongoing &&
      m_index_scan_type != partition_index_last)
  {
    if (unlikely((error= handle_pre_scan(FALSE, check_parallel_search()))))
      return error;
   return handle_unordered_scan_next_partition(buf);
  }
  return handle_ordered_index_scan(buf, FALSE);
}


/*
  Optimization of the default implementation to take advantage of dynamic
  partition pruning.
*/
int ha_partition::index_read_idx_map(uchar *buf, uint index,
                                     const uchar *key,
                                     key_part_map keypart_map,
                                     enum ha_rkey_function find_flag)
{
  int error= HA_ERR_KEY_NOT_FOUND;
  DBUG_ENTER("ha_partition::index_read_idx_map");

  if (find_flag == HA_READ_KEY_EXACT)
  {
    uint part;
    m_start_key.key= key;
    m_start_key.keypart_map= keypart_map;
    m_start_key.flag= find_flag;
    m_start_key.length= calculate_key_len(table, index, m_start_key.key,
                                          m_start_key.keypart_map);

    get_partition_set(table, buf, index, &m_start_key, &m_part_spec);

    /*
      We have either found exactly 1 partition
      (in which case start_part == end_part)
      or no matching partitions (start_part > end_part)
    */
    DBUG_ASSERT(m_part_spec.start_part >= m_part_spec.end_part);
    /* The start part is must be marked as used. */
    DBUG_ASSERT(m_part_spec.start_part > m_part_spec.end_part ||
                bitmap_is_set(&(m_part_info->read_partitions),
                              m_part_spec.start_part));

    for (part= m_part_spec.start_part;
         part <= m_part_spec.end_part;
         part= bitmap_get_next_set(&m_part_info->read_partitions, part))
    {
      error= m_file[part]->ha_index_read_idx_map(buf, index, key,
                                                 keypart_map, find_flag);
      if (likely(error != HA_ERR_KEY_NOT_FOUND &&
                 error != HA_ERR_END_OF_FILE))
        break;
    }
    if (part <= m_part_spec.end_part)
      m_last_part= part;
  }
  else
  {
    /*
      If not only used with READ_EXACT, we should investigate if possible
      to optimize for other find_flag's as well.
    */
    DBUG_ASSERT(0);
    /* fall back on the default implementation */
    error= handler::index_read_idx_map(buf, index, key, keypart_map, find_flag);
  }
  DBUG_RETURN(error);
}


/*
  Read next record in a forward index scan

  SYNOPSIS
    index_next()
    buf                   Read row in MySQL Row Format

  RETURN VALUE
    >0                    Error code
    0                     Success

  DESCRIPTION
    Used to read forward through the index.
*/

int ha_partition::index_next(uchar * buf)
{
  DBUG_ENTER("ha_partition::index_next");
  decrement_statistics(&SSV::ha_read_next_count);

  /*
    TODO(low priority):
    If we want partition to work with the HANDLER commands, we
    must be able to do index_last() -> index_prev() -> index_next()
    and if direction changes, we must step back those partitions in
    the record queue so we don't return a value from the wrong direction.
  */
  if (m_index_scan_type == partition_index_last)
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  if (!m_ordered_scan_ongoing)
  {
    DBUG_RETURN(handle_unordered_next(buf, FALSE));
  }
  DBUG_RETURN(handle_ordered_next(buf, FALSE));
}


/*
  Read next record special

  SYNOPSIS
    index_next_same()
    buf                   Read row in MySQL Row Format
    key                   Key
    keylen                Length of key

  RETURN VALUE
    >0                    Error code
    0                     Success

  DESCRIPTION
    This routine is used to read the next but only if the key is the same
    as supplied in the call.
*/

int ha_partition::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  DBUG_ENTER("ha_partition::index_next_same");
  decrement_statistics(&SSV::ha_read_next_count);

  DBUG_ASSERT(keylen == m_start_key.length);
  if (m_index_scan_type == partition_index_last)
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  if (!m_ordered_scan_ongoing)
    DBUG_RETURN(handle_unordered_next(buf, TRUE));
  DBUG_RETURN(handle_ordered_next(buf, TRUE));
}


int ha_partition::index_read_last_map(uchar *buf,
                                          const uchar *key,
                                          key_part_map keypart_map)
{
  DBUG_ENTER("ha_partition::index_read_last_map");

  m_ordered= true;                              // Safety measure
  end_range= NULL;
  m_index_scan_type= partition_index_read_last;
  m_start_key.key= key;
  m_start_key.keypart_map= keypart_map;
  m_start_key.flag= HA_READ_PREFIX_LAST;
  DBUG_RETURN(common_index_read(buf, true));
}


/*
  Read next record when performing index scan backwards

  SYNOPSIS
    index_prev()
    buf                   Read row in MySQL Row Format

  RETURN VALUE
    >0                    Error code
    0                     Success

  DESCRIPTION
    Used to read backwards through the index.
*/

int ha_partition::index_prev(uchar * buf)
{
  DBUG_ENTER("ha_partition::index_prev");
  decrement_statistics(&SSV::ha_read_prev_count);

  /* TODO: read comment in index_next */
  if (m_index_scan_type == partition_index_first)
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  DBUG_RETURN(handle_ordered_prev(buf));
}


/*
  Start a read of one range with start and end key

  SYNOPSIS
    read_range_first()
    start_key           Specification of start key
    end_key             Specification of end key
    eq_range_arg        Is it equal range
    sorted              Should records be returned in sorted order

  RETURN VALUE
    >0                    Error code
    0                     Success

  DESCRIPTION
    We reimplement read_range_first since we don't want the compare_key
    check at the end. This is already performed in the partition handler.
    read_range_next is very much different due to that we need to scan
    all underlying handlers.
*/

int ha_partition::read_range_first(const key_range *start_key,
				   const key_range *end_key,
				   bool eq_range_arg, bool sorted)
{
  int error;
  DBUG_ENTER("ha_partition::read_range_first");

  m_ordered= sorted;
  eq_range= eq_range_arg;
  set_end_range(end_key);

  range_key_part= m_curr_key_info[0]->key_part;
  if (start_key)
    m_start_key= *start_key;
  else
    m_start_key.key= NULL;

  m_index_scan_type= partition_read_range;
  error= common_index_read(m_rec0, MY_TEST(start_key));
  DBUG_RETURN(error);
}


/*
  Read next record in read of a range with start and end key

  SYNOPSIS
    read_range_next()

  RETURN VALUE
    >0                    Error code
    0                     Success
*/

int ha_partition::read_range_next()
{
  DBUG_ENTER("ha_partition::read_range_next");

  if (m_ordered_scan_ongoing)
  {
    DBUG_RETURN(handle_ordered_next(table->record[0], eq_range));
  }
  DBUG_RETURN(handle_unordered_next(table->record[0], eq_range));
}

/**
   Create a copy of all keys used by multi_range_read()

   @retval 0 ok
   @retval HA_ERR_END_OF_FILE no keys in range
   @retval other value: error

   TODO to save memory:
   - If (mrr_mode & HA_MRR_MATERIALIZED_KEYS) is set then the keys data is
     stable and we don't have to copy the keys, only store a pointer to the
     key.
   - When allocating key data, store things in a MEM_ROOT buffer instead of
     a malloc() per key. This will simplify and speed up the current code
     and use less memory.
*/

int ha_partition::multi_range_key_create_key(RANGE_SEQ_IF *seq,
                                             range_seq_t seq_it)
{
  uint i, length;
  key_range *start_key, *end_key;
  KEY_MULTI_RANGE *range;
  DBUG_ENTER("ha_partition::multi_range_key_create_key");

  bitmap_clear_all(&m_mrr_used_partitions);
  m_mrr_range_length= 0;
  bzero(m_part_mrr_range_length,
        sizeof(*m_part_mrr_range_length) * m_tot_parts);
  if (!m_mrr_range_first)
  {
    if (!(m_mrr_range_first= (PARTITION_KEY_MULTI_RANGE *)
          my_multi_malloc(MYF(MY_WME),
                          &m_mrr_range_current,
                          sizeof(PARTITION_KEY_MULTI_RANGE),
                          NullS)))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

    m_mrr_range_first->id= 1;
    m_mrr_range_first->key[0]= NULL;
    m_mrr_range_first->key[1]= NULL;
    m_mrr_range_first->next= NULL;
  }
  else
    m_mrr_range_current= m_mrr_range_first;

  for (i= 0; i < m_tot_parts; i++)
  {
    if (!m_part_mrr_range_first[i])
    {
      if (!(m_part_mrr_range_first[i]= (PARTITION_PART_KEY_MULTI_RANGE *)
            my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
                            &m_part_mrr_range_current[i],
                            sizeof(PARTITION_PART_KEY_MULTI_RANGE),
                            NullS)))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    else
    {
      m_part_mrr_range_current[i]= m_part_mrr_range_first[i];
      m_part_mrr_range_current[i]->partition_key_multi_range= NULL;
    }
  }
  m_mrr_range_current->key_multi_range.start_key.key= NULL;
  m_mrr_range_current->key_multi_range.end_key.key= NULL;

  while (!seq->next(seq_it, &m_mrr_range_current->key_multi_range))
  {
    m_mrr_range_length++;
    range= &m_mrr_range_current->key_multi_range;

    /* Copy start key */
    start_key= &range->start_key;
    DBUG_PRINT("info",("partition range->range_flag: %u", range->range_flag));
    DBUG_PRINT("info",("partition start_key->key: %p", start_key->key));
    DBUG_PRINT("info",("partition start_key->length: %u", start_key->length));
    DBUG_PRINT("info",("partition start_key->keypart_map: %lu",
                       start_key->keypart_map));
    DBUG_PRINT("info",("partition start_key->flag: %u", start_key->flag));

    if (start_key->key)
    {
      length= start_key->length;
      if (!m_mrr_range_current->key[0] ||
          m_mrr_range_current->length[0] < length)
      {
        if (m_mrr_range_current->key[0])
          my_free(m_mrr_range_current->key[0]);
        if (!(m_mrr_range_current->key[0]=
              (uchar *) my_malloc(length, MYF(MY_WME))))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        m_mrr_range_current->length[0]= length;
      }
      memcpy(m_mrr_range_current->key[0], start_key->key, length);
      start_key->key= m_mrr_range_current->key[0];
    }

    /* Copy end key */
    end_key= &range->end_key;
    DBUG_PRINT("info",("partition end_key->key: %p", end_key->key));
    DBUG_PRINT("info",("partition end_key->length: %u", end_key->length));
    DBUG_PRINT("info",("partition end_key->keypart_map: %lu",
                       end_key->keypart_map));
    DBUG_PRINT("info",("partition end_key->flag: %u", end_key->flag));
    if (end_key->key)
    {
      length= end_key->length;
      if (!m_mrr_range_current->key[1] ||
          m_mrr_range_current->length[1] < length)
      {
        if (m_mrr_range_current->key[1])
          my_free(m_mrr_range_current->key[1]);
        if (!(m_mrr_range_current->key[1]=
              (uchar *) my_malloc(length, MYF(MY_WME))))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        m_mrr_range_current->length[1]= length;
      }
      memcpy(m_mrr_range_current->key[1], end_key->key, length);
      end_key->key= m_mrr_range_current->key[1];
    }

    m_mrr_range_current->ptr= m_mrr_range_current->key_multi_range.ptr;
    m_mrr_range_current->key_multi_range.ptr= m_mrr_range_current;

    if (start_key->key && (start_key->flag & HA_READ_KEY_EXACT))
      get_partition_set(table, table->record[0], active_index,
                        start_key, &m_part_spec);
    else
    {
      m_part_spec.start_part= 0;
      m_part_spec.end_part= m_tot_parts - 1;
    }

    /* Copy key to those partitions that needs it */
    for (i= m_part_spec.start_part; i <= m_part_spec.end_part; i++)
    {
      if (bitmap_is_set(&(m_part_info->read_partitions), i))
      {
        bitmap_set_bit(&m_mrr_used_partitions, i);
        m_part_mrr_range_length[i]++;
        m_part_mrr_range_current[i]->partition_key_multi_range=
          m_mrr_range_current;

        if (!m_part_mrr_range_current[i]->next)
        {
          PARTITION_PART_KEY_MULTI_RANGE *tmp_part_mrr_range;
          if (!(tmp_part_mrr_range= (PARTITION_PART_KEY_MULTI_RANGE *)
                my_malloc(sizeof(PARTITION_PART_KEY_MULTI_RANGE),
                          MYF(MY_WME | MY_ZEROFILL))))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);

          m_part_mrr_range_current[i]->next= tmp_part_mrr_range;
          m_part_mrr_range_current[i]= tmp_part_mrr_range;
        }
        else
        {
          m_part_mrr_range_current[i]= m_part_mrr_range_current[i]->next;
          m_part_mrr_range_current[i]->partition_key_multi_range= NULL;
        }
      }
    }

    if (!m_mrr_range_current->next)
    {
      /* Add end of range sentinel */
      PARTITION_KEY_MULTI_RANGE *tmp_mrr_range;
      if (!(tmp_mrr_range= (PARTITION_KEY_MULTI_RANGE *)
            my_malloc(sizeof(PARTITION_KEY_MULTI_RANGE), MYF(MY_WME))))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);

      tmp_mrr_range->id= m_mrr_range_current->id + 1;
      tmp_mrr_range->key[0]= NULL;
      tmp_mrr_range->key[1]= NULL;
      tmp_mrr_range->next= NULL;
      m_mrr_range_current->next= tmp_mrr_range;
    }
    m_mrr_range_current= m_mrr_range_current->next;
  }

  if (!m_mrr_range_length)
  {
    DBUG_PRINT("Warning",("No keys to use for mrr"));
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }

  /* set start and end part */
  m_part_spec.start_part= bitmap_get_first_set(&m_mrr_used_partitions);

  for (i= m_tot_parts; i-- > 0;)
  {
    if (bitmap_is_set(&m_mrr_used_partitions, i))
    {
      m_part_spec.end_part= i;
      break;
    }
  }
  for (i= 0; i < m_tot_parts; i++)
  {
    m_partition_part_key_multi_range_hld[i].partition= this;
    m_partition_part_key_multi_range_hld[i].part_id= i;
    m_partition_part_key_multi_range_hld[i].partition_part_key_multi_range=
      m_part_mrr_range_first[i];
  }
  DBUG_PRINT("return",("OK"));
  DBUG_RETURN(0);
}


static void partition_multi_range_key_get_key_info(void *init_params,
                                                   uint *length,
                                                   key_part_map *map)
{
  PARTITION_PART_KEY_MULTI_RANGE_HLD *hld=
    (PARTITION_PART_KEY_MULTI_RANGE_HLD *)init_params;
  ha_partition *partition= hld->partition;
  key_range *start_key= (&partition->m_mrr_range_first->
                         key_multi_range.start_key);
  DBUG_ENTER("partition_multi_range_key_get_key_info");
  *length= start_key->length;
  *map= start_key->keypart_map;
  DBUG_VOID_RETURN;
}


static range_seq_t partition_multi_range_key_init(void *init_params,
                                                  uint n_ranges,
                                                  uint flags)
{
  PARTITION_PART_KEY_MULTI_RANGE_HLD *hld=
    (PARTITION_PART_KEY_MULTI_RANGE_HLD *)init_params;
  ha_partition *partition= hld->partition;
  uint i= hld->part_id;
  DBUG_ENTER("partition_multi_range_key_init");
  partition->m_mrr_range_init_flags= flags;
  hld->partition_part_key_multi_range= partition->m_part_mrr_range_first[i];
  DBUG_RETURN(init_params);
}


static bool partition_multi_range_key_next(range_seq_t seq,
                                           KEY_MULTI_RANGE *range)
{
  PARTITION_PART_KEY_MULTI_RANGE_HLD *hld=
    (PARTITION_PART_KEY_MULTI_RANGE_HLD *)seq;
  PARTITION_KEY_MULTI_RANGE *partition_key_multi_range=
    hld->partition_part_key_multi_range->partition_key_multi_range;
  DBUG_ENTER("partition_multi_range_key_next");
  if (!partition_key_multi_range)
    DBUG_RETURN(TRUE);
  *range= partition_key_multi_range->key_multi_range;
  hld->partition_part_key_multi_range=
    hld->partition_part_key_multi_range->next;
  DBUG_RETURN(FALSE);
}


static bool partition_multi_range_key_skip_record(range_seq_t seq,
                                                  range_id_t range_info,
                                                  uchar *rowid)
{
  PARTITION_PART_KEY_MULTI_RANGE_HLD *hld=
    (PARTITION_PART_KEY_MULTI_RANGE_HLD *)seq;
  DBUG_ENTER("partition_multi_range_key_skip_record");
  DBUG_RETURN(hld->partition->m_seq_if->skip_record(hld->partition->m_seq,
                                                    range_info, rowid));
}


static bool partition_multi_range_key_skip_index_tuple(range_seq_t seq,
                                                       range_id_t range_info)
{
  PARTITION_PART_KEY_MULTI_RANGE_HLD *hld=
    (PARTITION_PART_KEY_MULTI_RANGE_HLD *)seq;
  DBUG_ENTER("partition_multi_range_key_skip_index_tuple");
  DBUG_RETURN(hld->partition->m_seq_if->skip_index_tuple(hld->partition->m_seq,
                                                         range_info));
}

ha_rows ha_partition::multi_range_read_info_const(uint keyno,
                                                  RANGE_SEQ_IF *seq,
                                                  void *seq_init_param,
                                                  uint n_ranges, uint *bufsz,
                                                  uint *mrr_mode,
                                                  Cost_estimate *cost)
{
  int error;
  uint i;
  handler **file;
  ha_rows rows= 0;
  uint ret_mrr_mode= 0;
  range_seq_t seq_it;
  part_id_range save_part_spec;
  DBUG_ENTER("ha_partition::multi_range_read_info_const");
  DBUG_PRINT("enter", ("partition this: %p", this));

  m_mrr_new_full_buffer_size= 0;
  save_part_spec= m_part_spec;

  seq_it= seq->init(seq_init_param, n_ranges, *mrr_mode);
  if (unlikely((error= multi_range_key_create_key(seq, seq_it))))
  {
    if (likely(error == HA_ERR_END_OF_FILE))    // No keys in range
    {
      rows= 0;
      goto calc_cost;
    }
    /*
      This error means that we can't do multi_range_read for the moment
      (probably running out of memory) and we need to fallback to
      normal reads
    */
    m_part_spec= save_part_spec;
    DBUG_RETURN(HA_POS_ERROR);
  }
  m_part_seq_if.get_key_info=
    seq->get_key_info ? partition_multi_range_key_get_key_info : NULL;
  m_part_seq_if.init= partition_multi_range_key_init;
  m_part_seq_if.next= partition_multi_range_key_next;
  m_part_seq_if.skip_record= (seq->skip_record ?
                              partition_multi_range_key_skip_record : NULL);
  m_part_seq_if.skip_index_tuple= (seq->skip_index_tuple ?
                                   partition_multi_range_key_skip_index_tuple :
                                   NULL);
  file= m_file;
  do
  {
    i= (uint)(file - m_file);
    DBUG_PRINT("info",("partition part_id: %u", i));
    if (bitmap_is_set(&m_mrr_used_partitions, i))
    {
      ha_rows tmp_rows;
      uint tmp_mrr_mode;
      m_mrr_buffer_size[i]= 0;
      tmp_mrr_mode= *mrr_mode;
      tmp_rows= (*file)->
        multi_range_read_info_const(keyno, &m_part_seq_if,
                                    &m_partition_part_key_multi_range_hld[i],
                                    m_part_mrr_range_length[i],
                                    &m_mrr_buffer_size[i],
                                    &tmp_mrr_mode, cost);
      if (tmp_rows == HA_POS_ERROR)
      {
        m_part_spec= save_part_spec;
        DBUG_RETURN(HA_POS_ERROR);
      }
      rows+= tmp_rows;
      ret_mrr_mode|= tmp_mrr_mode;
      m_mrr_new_full_buffer_size+= m_mrr_buffer_size[i];
    }
  } while (*(++file));
  *mrr_mode= ret_mrr_mode;

calc_cost:
  m_part_spec= save_part_spec;
  cost->reset();
  cost->avg_io_cost= 1;
  if ((*mrr_mode & HA_MRR_INDEX_ONLY) && rows > 2)
    cost->io_count= keyread_time(keyno, n_ranges, (uint) rows);
  else
    cost->io_count= read_time(keyno, n_ranges, rows);
  cost->cpu_cost= (double) rows / TIME_FOR_COMPARE + 0.01;
  DBUG_RETURN(rows);
}


ha_rows ha_partition::multi_range_read_info(uint keyno, uint n_ranges,
                                            uint keys,
                                            uint key_parts, uint *bufsz,
                                            uint *mrr_mode,
                                            Cost_estimate *cost)
{
  uint i;
  handler **file;
  ha_rows rows;
  DBUG_ENTER("ha_partition::multi_range_read_info");
  DBUG_PRINT("enter", ("partition this: %p", this));

  m_mrr_new_full_buffer_size= 0;
  file= m_file;
  do
  {
    i= (uint)(file - m_file);
    if (bitmap_is_set(&(m_part_info->read_partitions), (i)))
    {
      m_mrr_buffer_size[i]= 0;
      if ((rows= (*file)->multi_range_read_info(keyno, n_ranges, keys,
                                                key_parts,
                                                &m_mrr_buffer_size[i],
                                                mrr_mode, cost)))
        DBUG_RETURN(rows);
      m_mrr_new_full_buffer_size+= m_mrr_buffer_size[i];
    }
  } while (*(++file));

  cost->reset();
  cost->avg_io_cost= 1;
  if (*mrr_mode & HA_MRR_INDEX_ONLY)
    cost->io_count= keyread_time(keyno, n_ranges, (uint) rows);
  else
    cost->io_count= read_time(keyno, n_ranges, rows);
  DBUG_RETURN(0);
}


int ha_partition::multi_range_read_init(RANGE_SEQ_IF *seq,
                                        void *seq_init_param,
                                        uint n_ranges, uint mrr_mode,
                                        HANDLER_BUFFER *buf)
{
  int error;
  uint i;
  handler **file;
  uchar *tmp_buffer;
  DBUG_ENTER("ha_partition::multi_range_read_init");
  DBUG_PRINT("enter", ("partition this: %p", this));

  m_seq_if= seq;
  m_seq= seq->init(seq_init_param, n_ranges, mrr_mode);
  if (unlikely((error= multi_range_key_create_key(seq, m_seq))))
    DBUG_RETURN(0);

  m_part_seq_if.get_key_info= (seq->get_key_info ?
                               partition_multi_range_key_get_key_info :
                               NULL);
  m_part_seq_if.init= partition_multi_range_key_init;
  m_part_seq_if.next= partition_multi_range_key_next;
  m_part_seq_if.skip_record= (seq->skip_record ?
                              partition_multi_range_key_skip_record :
                              NULL);
  m_part_seq_if.skip_index_tuple= (seq->skip_index_tuple ?
                                   partition_multi_range_key_skip_index_tuple :
                                   NULL);

  /* m_mrr_new_full_buffer_size was calculated in multi_range_read_info */
  if (m_mrr_full_buffer_size < m_mrr_new_full_buffer_size)
  {
    if (m_mrr_full_buffer)
      my_free(m_mrr_full_buffer);
    if (!(m_mrr_full_buffer=
          (uchar *) my_malloc(m_mrr_new_full_buffer_size, MYF(MY_WME))))
    {
      m_mrr_full_buffer_size= 0;
      error= HA_ERR_OUT_OF_MEM;
      goto error;
    }
    m_mrr_full_buffer_size= m_mrr_new_full_buffer_size;
  }

  tmp_buffer= m_mrr_full_buffer;
  file= m_file;
  do
  {
    i= (uint)(file - m_file);
    DBUG_PRINT("info",("partition part_id: %u", i));
    if (bitmap_is_set(&m_mrr_used_partitions, i))
    {
      if (m_mrr_new_full_buffer_size)
      {
        if (m_mrr_buffer_size[i])
        {
          m_mrr_buffer[i].buffer= tmp_buffer;
          m_mrr_buffer[i].end_of_used_area= tmp_buffer;
          tmp_buffer+= m_mrr_buffer_size[i];
          m_mrr_buffer[i].buffer_end= tmp_buffer;
        }
      }
      else
        m_mrr_buffer[i]= *buf;

      if (unlikely((error= (*file)->
                    multi_range_read_init(&m_part_seq_if,
                                          &m_partition_part_key_multi_range_hld[i],
                                          m_part_mrr_range_length[i],
                                          mrr_mode,
                                          &m_mrr_buffer[i]))))
        goto error;
      m_stock_range_seq[i]= 0;
    }
  } while (*(++file));

  m_multi_range_read_first= TRUE;
  m_mrr_range_current= m_mrr_range_first;
  m_index_scan_type= partition_read_multi_range;
  m_mrr_mode= mrr_mode;
  m_mrr_n_ranges= n_ranges;
  DBUG_RETURN(0);

error:
  DBUG_RETURN(error);
}


int ha_partition::multi_range_read_next(range_id_t *range_info)
{
  int error;
  DBUG_ENTER("ha_partition::multi_range_read_next");
  DBUG_PRINT("enter", ("partition this: %p  partition m_mrr_mode: %u",
                       this, m_mrr_mode));

  if ((m_mrr_mode & HA_MRR_SORTED))
  {
    if (m_multi_range_read_first)
    {
      if (unlikely((error= handle_ordered_index_scan(table->record[0],
                                                     FALSE))))
        DBUG_RETURN(error);
      if (!m_pre_calling)
        m_multi_range_read_first= FALSE;
    }
    else if (unlikely((error= handle_ordered_next(table->record[0],
                                                  eq_range))))
      DBUG_RETURN(error);
    *range_info= m_mrr_range_current->ptr;
  }
  else
  {
    if (unlikely(m_multi_range_read_first))
    {
      if (unlikely((error=
                    handle_unordered_scan_next_partition(table->record[0]))))
        DBUG_RETURN(error);
      if (!m_pre_calling)
        m_multi_range_read_first= FALSE;
    }
    else if (unlikely((error= handle_unordered_next(table->record[0], FALSE))))
      DBUG_RETURN(error);

    *range_info=
      ((PARTITION_KEY_MULTI_RANGE *) m_range_info[m_last_part])->ptr;
  }
  DBUG_RETURN(0);
}


int ha_partition::multi_range_read_explain_info(uint mrr_mode, char *str,
                                                size_t size)
{
  DBUG_ENTER("ha_partition::multi_range_read_explain_info");
  DBUG_RETURN(get_open_file_sample()->
                multi_range_read_explain_info(mrr_mode, str, size));
}


/**
  Find and retrieve the Full Text Search relevance ranking for a search string
  in a full text index.

  @param  handler           Full Text Search handler
  @param  record            Search string
  @param  length            Length of the search string

  @retval                   Relevance value
*/

float partition_ft_find_relevance(FT_INFO *handler,
                                  uchar *record, uint length)
{
  st_partition_ft_info *info= (st_partition_ft_info *)handler;
  uint m_last_part= ((ha_partition*) info->file)->last_part();
  FT_INFO *m_handler= info->part_ft_info[m_last_part];
  DBUG_ENTER("partition_ft_find_relevance");
  if (!m_handler)
    DBUG_RETURN((float)-1.0);
  DBUG_RETURN(m_handler->please->find_relevance(m_handler, record, length));
}


/**
  Retrieve the Full Text Search relevance ranking for the current
  full text search.

  @param  handler           Full Text Search handler

  @retval                   Relevance value
*/

float partition_ft_get_relevance(FT_INFO *handler)
{
  st_partition_ft_info *info= (st_partition_ft_info *)handler;
  uint m_last_part= ((ha_partition*) info->file)->last_part();
  FT_INFO *m_handler= info->part_ft_info[m_last_part];
  DBUG_ENTER("partition_ft_get_relevance");
  if (!m_handler)
    DBUG_RETURN((float)-1.0);
  DBUG_RETURN(m_handler->please->get_relevance(m_handler));
}


/**
  Free the memory for a full text search handler.

  @param  handler           Full Text Search handler
*/

void partition_ft_close_search(FT_INFO *handler)
{
  st_partition_ft_info *info= (st_partition_ft_info *)handler;
  info->file->ft_close_search(handler);
}


/**
  Free the memory for a full text search handler.

  @param  handler           Full Text Search handler
*/

void ha_partition::ft_close_search(FT_INFO *handler)
{
  uint i;
  st_partition_ft_info *info= (st_partition_ft_info *)handler;
  DBUG_ENTER("ha_partition::ft_close_search");

  for (i= 0; i < m_tot_parts; i++)
  {
    FT_INFO *m_handler= info->part_ft_info[i];
    DBUG_ASSERT(!m_handler ||
                (m_handler->please && m_handler->please->close_search));
    if (m_handler &&
        m_handler->please &&
        m_handler->please->close_search)
      m_handler->please->close_search(m_handler);
  }
  DBUG_VOID_RETURN;
}


/* Partition Full Text search function table */
_ft_vft partition_ft_vft =
{
  NULL, // partition_ft_read_next
  partition_ft_find_relevance,
  partition_ft_close_search,
  partition_ft_get_relevance,
  NULL  // partition_ft_reinit_search
};


/**
  Initialize a full text search.
*/

int ha_partition::ft_init()
{
  int error;
  uint i= 0;
  uint32 part_id;
  DBUG_ENTER("ha_partition::ft_init");
  DBUG_PRINT("info", ("partition this: %p", this));

  /*
    For operations that may need to change data, we may need to extend
    read_set.
  */
  if (get_lock_type() == F_WRLCK)
  {
    /*
      If write_set contains any of the fields used in partition and
      subpartition expression, we need to set all bits in read_set because
      the row may need to be inserted in a different [sub]partition. In
      other words update_row() can be converted into write_row(), which
      requires a complete record.
    */
    if (bitmap_is_overlapping(&m_part_info->full_part_field_set,
                              table->write_set))
      bitmap_set_all(table->read_set);
    else
    {
      /*
        Some handlers only read fields as specified by the bitmap for the
        read set. For partitioned handlers we always require that the
        fields of the partition functions are read such that we can
        calculate the partition id to place updated and deleted records.
      */
      bitmap_union(table->read_set, &m_part_info->full_part_field_set);
    }
  }

  /* Now we see what the index of our first important partition is */
  DBUG_PRINT("info", ("m_part_info->read_partitions: %p",
             (void *) m_part_info->read_partitions.bitmap));
  part_id= bitmap_get_first_set(&(m_part_info->read_partitions));
  DBUG_PRINT("info", ("m_part_spec.start_part %u", (uint) part_id));

  if (part_id == MY_BIT_NONE)
  {
    error= 0;
    goto err1;
  }

  DBUG_PRINT("info", ("ft_init on partition %u", (uint) part_id));
  /*
    ft_end() is needed for partitioning to reset internal data if scan
    is already in use
  */
  if (m_pre_calling)
  {
    if (unlikely((error= pre_ft_end())))
      goto err1;
  }
  else
    ft_end();
  m_index_scan_type= partition_ft_read;
  for (i= part_id; i < m_tot_parts; i++)
  {
    if (bitmap_is_set(&(m_part_info->read_partitions), i))
    {
      error= m_pre_calling ? m_file[i]->pre_ft_init() : m_file[i]->ft_init();
      if (unlikely(error))
        goto err2;
    }
  }
  m_scan_value= 1;
  m_part_spec.start_part= part_id;
  m_part_spec.end_part= m_tot_parts - 1;
  m_ft_init_and_first= TRUE;
  DBUG_PRINT("info", ("m_scan_value: %u", m_scan_value));
  DBUG_RETURN(0);

err2:
  late_extra_no_cache(part_id);
  while ((int)--i >= (int)part_id)
  {
    if (bitmap_is_set(&(m_part_info->read_partitions), i))
    {
      if (m_pre_calling)
        m_file[i]->pre_ft_end();
      else
        m_file[i]->ft_end();
    }
  }
err1:
  m_scan_value= 2;
  m_part_spec.start_part= NO_CURRENT_PART_ID;
  DBUG_RETURN(error);
}


/**
  Initialize a full text search during a bulk access request.
*/

int ha_partition::pre_ft_init()
{
  bool save_m_pre_calling;
  int error;
  DBUG_ENTER("ha_partition::pre_ft_init");
  save_m_pre_calling= m_pre_calling;
  m_pre_calling= TRUE;
  error= ft_init();
  m_pre_calling= save_m_pre_calling;
  DBUG_RETURN(error);
}


/**
  Terminate a full text search.
*/

void ha_partition::ft_end()
{
  handler **file;
  DBUG_ENTER("ha_partition::ft_end");
  DBUG_PRINT("info", ("partition this: %p", this));

  switch (m_scan_value) {
  case 2:                                       // Error
    break;
  case 1:                                       // Table scan
    if (NO_CURRENT_PART_ID != m_part_spec.start_part)
      late_extra_no_cache(m_part_spec.start_part);
    file= m_file;
    do
    {
      if (bitmap_is_set(&(m_part_info->read_partitions), (uint)(file - m_file)))
      {
        if (m_pre_calling)
          (*file)->pre_ft_end();
        else
          (*file)->ft_end();
      }
    } while (*(++file));
    break;
  }
  m_scan_value= 2;
  m_part_spec.start_part= NO_CURRENT_PART_ID;
  ft_current= 0;
  DBUG_VOID_RETURN;
}


/**
  Terminate a full text search during a bulk access request.
*/

int ha_partition::pre_ft_end()
{
  bool save_m_pre_calling;
  DBUG_ENTER("ha_partition::pre_ft_end");
  save_m_pre_calling= m_pre_calling;
  m_pre_calling= TRUE;
  ft_end();
  m_pre_calling= save_m_pre_calling;
  DBUG_RETURN(0);
}


/**
  Initialize a full text search using the extended API.

  @param  flags             Search flags
  @param  inx               Key number
  @param  key               Key value

  @return FT_INFO structure if successful
          NULL              otherwise
*/

FT_INFO *ha_partition::ft_init_ext(uint flags, uint inx, String *key)
{
  FT_INFO *ft_handler;
  handler **file;
  st_partition_ft_info *ft_target, **parent;
  DBUG_ENTER("ha_partition::ft_init_ext");

  if (ft_current)
    parent= &ft_current->next;
  else
    parent= &ft_first;

  if (!(ft_target= *parent))
  {
    FT_INFO **tmp_ft_info;
    if (!(ft_target= (st_partition_ft_info *)
          my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
                          &ft_target,
                          sizeof(st_partition_ft_info),
                          &tmp_ft_info,
                          sizeof(FT_INFO *) * m_tot_parts,
                          NullS)))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(ME_FATALERROR));
      DBUG_RETURN(NULL);
    }
    ft_target->part_ft_info= tmp_ft_info;
    (*parent)= ft_target;
  }

  ft_current= ft_target;
  file= m_file;
  do
  {
    if (bitmap_is_set(&(m_part_info->read_partitions), (uint)(file - m_file)))
    {
      if ((ft_handler= (*file)->ft_init_ext(flags, inx, key)))
        (*file)->ft_handler= ft_handler;
      else
        (*file)->ft_handler= NULL;
      ft_target->part_ft_info[file - m_file]= ft_handler;
    }
    else
    {
      (*file)->ft_handler= NULL;
      ft_target->part_ft_info[file - m_file]= NULL;
    }
  } while (*(++file));

  ft_target->please= &partition_ft_vft;
  ft_target->file= this;
  DBUG_RETURN((FT_INFO*)ft_target);
}


/**
  Return the next record from the FT result set during an ordered index
  pre-scan

  @param  use_parallel      Is it a parallel search

  @return >0                Error code
          0                 Success
*/

int ha_partition::pre_ft_read(bool use_parallel)
{
  bool save_m_pre_calling;
  int error;
  DBUG_ENTER("ha_partition::pre_ft_read");
  DBUG_PRINT("info", ("partition this: %p", this));
  save_m_pre_calling= m_pre_calling;
  m_pre_calling= TRUE;
  m_pre_call_use_parallel= use_parallel;
  error= ft_read(table->record[0]);
  m_pre_calling= save_m_pre_calling;
  DBUG_RETURN(error);
}


/**
  Return the first or next record in a full text search.

  @param  buf               Buffer where the record should be returned

  @return >0                Error code
          0                 Success
*/

int ha_partition::ft_read(uchar *buf)
{
  handler *file;
  int result= HA_ERR_END_OF_FILE, error;
  uint part_id= m_part_spec.start_part;
  DBUG_ENTER("ha_partition::ft_read");
  DBUG_PRINT("info", ("partition this: %p", this));
  DBUG_PRINT("info", ("part_id: %u", part_id));

  if (part_id == NO_CURRENT_PART_ID)
  {
    /*
      The original set of partitions to scan was empty and thus we report
      the result here.
    */
    DBUG_PRINT("info", ("NO_CURRENT_PART_ID"));
    goto end;
  }

  DBUG_ASSERT(m_scan_value == 1);

  if (m_ft_init_and_first)                      // First call to ft_read()
  {
    m_ft_init_and_first= FALSE;
    if (!bulk_access_executing)
    {
      error= handle_pre_scan(FALSE, check_parallel_search());
      if (m_pre_calling || error)
        DBUG_RETURN(error);
    }
    late_extra_cache(part_id);
  }

  file= m_file[part_id];

  while (TRUE)
  {
    if (!(result= file->ft_read(buf)))
    {
      /* Found row: remember position and return it. */
      m_part_spec.start_part= m_last_part= part_id;
      table->status= 0;
      DBUG_RETURN(0);
    }

    /*
      if we get here, then the current partition ft_next returned failure
    */
    if (result != HA_ERR_END_OF_FILE)
      goto end_dont_reset_start_part;         // Return error

    /* End current partition */
    late_extra_no_cache(part_id);
    DBUG_PRINT("info", ("stopping using partition %u", (uint) part_id));

    /* Shift to next partition */
    while (++part_id < m_tot_parts &&
           !bitmap_is_set(&(m_part_info->read_partitions), part_id))
      ;
    if (part_id >= m_tot_parts)
    {
      result= HA_ERR_END_OF_FILE;
      break;
    }
    m_part_spec.start_part= m_last_part= part_id;
    file= m_file[part_id];
    DBUG_PRINT("info", ("now using partition %u", (uint) part_id));
    late_extra_cache(part_id);
  }

end:
  m_part_spec.start_part= NO_CURRENT_PART_ID;
end_dont_reset_start_part:
  table->status= STATUS_NOT_FOUND;
  DBUG_RETURN(result);
}


/*
  Common routine to set up index scans

  SYNOPSIS
    ha_partition::partition_scan_set_up()
      buf            Buffer to later return record in (this function
                     needs it to calculcate partitioning function
                     values)

      idx_read_flag  TRUE <=> m_start_key has range start endpoint which
                     probably can be used to determine the set of partitions
                     to scan.
                     FALSE <=> there is no start endpoint.

  DESCRIPTION
    Find out which partitions we'll need to read when scanning the specified
    range.

    If we need to scan only one partition, set m_ordered_scan_ongoing=FALSE
    as we will not need to do merge ordering.

  RETURN VALUE
    >0                    Error code
    0                     Success
*/

int ha_partition::partition_scan_set_up(uchar * buf, bool idx_read_flag)
{
  DBUG_ENTER("ha_partition::partition_scan_set_up");

  if (idx_read_flag)
    get_partition_set(table, buf, active_index, &m_start_key, &m_part_spec);
  else
  {
    m_part_spec.start_part= 0;
    m_part_spec.end_part= m_tot_parts - 1;
  }
  if (m_part_spec.start_part > m_part_spec.end_part)
  {
    /*
      We discovered a partition set but the set was empty so we report
      key not found.
    */
    DBUG_PRINT("info", ("scan with no partition to scan"));
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  if (m_part_spec.start_part == m_part_spec.end_part)
  {
    /*
      We discovered a single partition to scan, this never needs to be
      performed using the ordered index scan.
    */
    DBUG_PRINT("info", ("index scan using the single partition %u",
			(uint) m_part_spec.start_part));
    m_ordered_scan_ongoing= FALSE;
  }
  else
  {
    /*
      Set m_ordered_scan_ongoing according how the scan should be done
      Only exact partitions are discovered atm by get_partition_set.
      Verify this, also bitmap must have at least one bit set otherwise
      the result from this table is the empty set.
    */
    uint start_part= bitmap_get_first_set(&(m_part_info->read_partitions));
    if (start_part == MY_BIT_NONE)
    {
      DBUG_PRINT("info", ("scan with no partition to scan"));
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    if (start_part > m_part_spec.start_part)
      m_part_spec.start_part= start_part;
    DBUG_ASSERT(m_part_spec.start_part < m_tot_parts);
    m_ordered_scan_ongoing= m_ordered;
  }
  DBUG_ASSERT(m_part_spec.start_part < m_tot_parts &&
              m_part_spec.end_part < m_tot_parts);
  DBUG_RETURN(0);
}

/**
  Check if we can search partitions in parallel

  @retval TRUE  yes
  @retval FALSE no
*/

bool ha_partition::check_parallel_search()
{
  TABLE_LIST *table_list= table->pos_in_table_list;
  st_select_lex *select_lex;
  JOIN *join;
  DBUG_ENTER("ha_partition::check_parallel_search");
  if (!table_list)
    goto not_parallel;

  while (table_list->parent_l)
    table_list= table_list->parent_l;

  select_lex= table_list->select_lex;
  DBUG_PRINT("info",("partition select_lex: %p", select_lex));
  if (!select_lex)
    goto not_parallel;
  if (!select_lex->explicit_limit)
  {
    DBUG_PRINT("info",("partition not using explicit_limit"));
    goto parallel;
  }

  join= select_lex->join;
  DBUG_PRINT("info",("partition join: %p", join));
  if (join && join->skip_sort_order)
  {
    DBUG_PRINT("info",("partition order_list.elements: %u",
                       select_lex->order_list.elements));
    if (select_lex->order_list.elements)
    {
      Item *item= *select_lex->order_list.first->item;
      DBUG_PRINT("info",("partition item: %p", item));
      DBUG_PRINT("info",("partition item->type(): %u", item->type()));
      DBUG_PRINT("info",("partition m_part_info->part_type: %u",
                         m_part_info->part_type));
      DBUG_PRINT("info",("partition m_is_sub_partitioned: %s",
                         m_is_sub_partitioned ? "TRUE" : "FALSE"));
      DBUG_PRINT("info",("partition m_part_info->part_expr: %p",
                         m_part_info->part_expr));
      if (item->type() == Item::FIELD_ITEM &&
          m_part_info->part_type == RANGE_PARTITION &&
          !m_is_sub_partitioned &&
          (!m_part_info->part_expr ||
           m_part_info->part_expr->type() == Item::FIELD_ITEM))
      {
        Field *order_field= ((Item_field *)item)->field;
        DBUG_PRINT("info",("partition order_field: %p", order_field));
        if (order_field && order_field->table == table_list->table)
        {
          Field *part_field= m_part_info->full_part_field_array[0];
          if (set_top_table_fields)
            order_field= top_table_field[order_field->field_index];
          DBUG_PRINT("info",("partition order_field: %p", order_field));
          DBUG_PRINT("info",("partition part_field: %p", part_field));
          if (part_field == order_field)
          {
            /*
              We are using ORDER BY partition_field LIMIT #
              In this case, let's not do things in parallel as it's
              likely that the query can be satisfied from the first
              partition
            */
            DBUG_PRINT("info",("partition with ORDER on partition field"));
            goto not_parallel;
          }
        }
      }
      DBUG_PRINT("info",("partition have order"));
      goto parallel;
    }

    DBUG_PRINT("info",("partition group_list.elements: %u",
                       select_lex->group_list.elements));
    if (select_lex->group_list.elements)
    {
      Item *item= *select_lex->group_list.first->item;
      DBUG_PRINT("info",("partition item: %p", item));
      DBUG_PRINT("info",("partition item->type(): %u", item->type()));
      DBUG_PRINT("info",("partition m_part_info->part_type: %u",
                         m_part_info->part_type));
      DBUG_PRINT("info",("partition m_is_sub_partitioned: %s",
                         m_is_sub_partitioned ? "TRUE" : "FALSE"));
      DBUG_PRINT("info",("partition m_part_info->part_expr: %p",
                         m_part_info->part_expr));
      if (item->type() == Item::FIELD_ITEM &&
          m_part_info->part_type == RANGE_PARTITION &&
          !m_is_sub_partitioned &&
          (!m_part_info->part_expr ||
           m_part_info->part_expr->type() == Item::FIELD_ITEM))
      {
        Field *group_field= ((Item_field *)item)->field;
        DBUG_PRINT("info",("partition group_field: %p", group_field));
        if (group_field && group_field->table == table_list->table)
        {
          Field *part_field= m_part_info->full_part_field_array[0];
          if (set_top_table_fields)
            group_field= top_table_field[group_field->field_index];
          DBUG_PRINT("info",("partition group_field: %p", group_field));
          DBUG_PRINT("info",("partition part_field: %p", part_field));
          if (part_field == group_field)
          {
            DBUG_PRINT("info",("partition with GROUP BY on partition field"));
            goto not_parallel;
          }
        }
      }
      DBUG_PRINT("info",("partition with GROUP BY"));
      goto parallel;
    }
  }
  else if (select_lex->order_list.elements ||
           select_lex->group_list.elements)
  {
    DBUG_PRINT("info",("partition is not skip_order"));
    DBUG_PRINT("info",("partition order_list.elements: %u",
                       select_lex->order_list.elements));
    DBUG_PRINT("info",("partition group_list.elements: %u",
                       select_lex->group_list.elements));
    goto parallel;
  }
  DBUG_PRINT("info",("partition is not skip_order"));

not_parallel:
  DBUG_PRINT("return",("partition FALSE"));
  DBUG_RETURN(FALSE);

parallel:
  DBUG_PRINT("return",("partition TRUE"));
  DBUG_RETURN(TRUE);
}


int ha_partition::handle_pre_scan(bool reverse_order, bool use_parallel)
{
  uint i;
  DBUG_ENTER("ha_partition::handle_pre_scan");
  DBUG_PRINT("enter",
             ("m_part_spec.start_part: %u  m_part_spec.end_part: %u",
              (uint) m_part_spec.start_part, (uint) m_part_spec.end_part));

  for (i= m_part_spec.start_part; i <= m_part_spec.end_part; i++)
  {
    if (!(bitmap_is_set(&(m_part_info->read_partitions), i)))
      continue;
    int error;
    handler *file= m_file[i];

    switch (m_index_scan_type) {
    case partition_index_read:
      error= file->pre_index_read_map(m_start_key.key,
                                  m_start_key.keypart_map,
                                  m_start_key.flag,
                                  use_parallel);
      break;
    case partition_index_first:
      error= file->pre_index_first(use_parallel);
      break;
    case partition_index_last:
      error= file->pre_index_last(use_parallel);
      break;
    case partition_index_read_last:
      error= file->pre_index_read_last_map(m_start_key.key,
                                       m_start_key.keypart_map,
                                       use_parallel);
      break;
    case partition_read_range:
      error= file->pre_read_range_first(m_start_key.key? &m_start_key: NULL,
                                    end_range, eq_range, TRUE, use_parallel);
      break;
    case partition_read_multi_range:
      if (!bitmap_is_set(&m_mrr_used_partitions, i))
        continue;
      error= file->pre_multi_range_read_next(use_parallel);
      break;
    case partition_ft_read:
      error= file->pre_ft_read(use_parallel);
      break;
    case partition_no_index_scan:
      error= file->pre_rnd_next(use_parallel);
      break;
    default:
      DBUG_ASSERT(FALSE);
      DBUG_RETURN(0);
    }
    if (error == HA_ERR_END_OF_FILE)
      error= 0;
    if (unlikely(error))
      DBUG_RETURN(error);
  }
  table->status= 0;
  DBUG_RETURN(0);
}


/****************************************************************************
  Unordered Index Scan Routines
****************************************************************************/
/*
  Common routine to handle index_next with unordered results

  SYNOPSIS
    handle_unordered_next()
    out:buf                       Read row in MySQL Row Format
    next_same                     Called from index_next_same

  RETURN VALUE
    HA_ERR_END_OF_FILE            End of scan
    0                             Success
    other                         Error code

  DESCRIPTION
    These routines are used to scan partitions without considering order.
    This is performed in two situations.
    1) In read_multi_range this is the normal case
    2) When performing any type of index_read, index_first, index_last where
    all fields in the partition function is bound. In this case the index
    scan is performed on only one partition and thus it isn't necessary to
    perform any sort.
*/

int ha_partition::handle_unordered_next(uchar *buf, bool is_next_same)
{
  handler *file;
  int error;
  DBUG_ENTER("ha_partition::handle_unordered_next");

  if (m_part_spec.start_part >= m_tot_parts)
  {
    /* Should never happen! */
    DBUG_ASSERT(0);
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
  file= m_file[m_part_spec.start_part];

  /*
    We should consider if this should be split into three functions as
    partition_read_range is_next_same are always local constants
  */

  if (m_index_scan_type == partition_read_multi_range)
  {
    if (likely(!(error= file->
                 multi_range_read_next(&m_range_info[m_part_spec.start_part]))))
    {
      m_last_part= m_part_spec.start_part;
      DBUG_RETURN(0);
    }
  }
  else if (m_index_scan_type == partition_read_range)
  {
    if (likely(!(error= file->read_range_next())))
    {
      m_last_part= m_part_spec.start_part;
      DBUG_RETURN(0);
    }
  }
  else if (is_next_same)
  {
    if (likely(!(error= file->ha_index_next_same(buf, m_start_key.key,
                                                 m_start_key.length))))
    {
      m_last_part= m_part_spec.start_part;
      DBUG_RETURN(0);
    }
  }
  else
  {
    if (likely(!(error= file->ha_index_next(buf))))
    {
      m_last_part= m_part_spec.start_part;
      DBUG_RETURN(0);                           // Row was in range
    }
  }

    if (unlikely(error == HA_ERR_END_OF_FILE))
  {
    m_part_spec.start_part++;                    // Start using next part
    error= handle_unordered_scan_next_partition(buf);
  }
  DBUG_RETURN(error);
}


/*
  Handle index_next when changing to new partition

  SYNOPSIS
    handle_unordered_scan_next_partition()
    buf                       Read row in MariaDB Row Format

  RETURN VALUE
    HA_ERR_END_OF_FILE            End of scan
    0                             Success
    other                         Error code

  DESCRIPTION
    This routine is used to start the index scan on the next partition.
    Both initial start and after completing scan on one partition.
*/

int ha_partition::handle_unordered_scan_next_partition(uchar * buf)
{
  uint i= m_part_spec.start_part;
  int saved_error= HA_ERR_END_OF_FILE;
  DBUG_ENTER("ha_partition::handle_unordered_scan_next_partition");

  /* Read next partition that includes start_part */
  if (i)
    i= bitmap_get_next_set(&m_part_info->read_partitions, i - 1);
  else
    i= bitmap_get_first_set(&m_part_info->read_partitions);

  for (;
       i <= m_part_spec.end_part;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
  {
    int error;
    handler *file= m_file[i];
    m_part_spec.start_part= i;

    switch (m_index_scan_type) {
    case partition_read_multi_range:
      if (!bitmap_is_set(&m_mrr_used_partitions, i))
        continue;
      DBUG_PRINT("info", ("read_multi_range on partition %u", i));
      error= file->multi_range_read_next(&m_range_info[i]);
      break;
    case partition_read_range:
      DBUG_PRINT("info", ("read_range_first on partition %u", i));
      error= file->read_range_first(m_start_key.key? &m_start_key: NULL,
                                    end_range, eq_range, FALSE);
      break;
    case partition_index_read:
      DBUG_PRINT("info", ("index_read on partition %u", i));
      error= file->ha_index_read_map(buf, m_start_key.key,
                                     m_start_key.keypart_map,
                                     m_start_key.flag);
      break;
    case partition_index_first:
      DBUG_PRINT("info", ("index_first on partition %u", i));
      error= file->ha_index_first(buf);
      break;
    default:
      DBUG_ASSERT(FALSE);
      DBUG_RETURN(1);
    }
    if (likely(!error))
    {
      m_last_part= i;
      DBUG_RETURN(0);
    }
    if (likely((error != HA_ERR_END_OF_FILE) &&
               (error != HA_ERR_KEY_NOT_FOUND)))
      DBUG_RETURN(error);

    /*
      If HA_ERR_KEY_NOT_FOUND, we must return that error instead of
      HA_ERR_END_OF_FILE, to be able to continue search.
    */
    if (saved_error != HA_ERR_KEY_NOT_FOUND)
      saved_error= error;
    DBUG_PRINT("info", ("END_OF_FILE/KEY_NOT_FOUND on partition %u", i));
  }
  if (saved_error == HA_ERR_END_OF_FILE)
    m_part_spec.start_part= NO_CURRENT_PART_ID;
  DBUG_RETURN(saved_error);
}


/**
  Common routine to start index scan with ordered results.

  @param[out] buf  Read row in MariaDB Row Format

  @return Operation status
    @retval HA_ERR_END_OF_FILE  End of scan
    @retval HA_ERR_KEY_NOT_FOUNE  End of scan
    @retval 0                   Success
    @retval other               Error code

  @details
    This part contains the logic to handle index scans that require ordered
    output. This includes all except those started by read_range_first with
    the flag ordered set to FALSE. Thus most direct index_read and all
    index_first and index_last.

    We implement ordering by keeping one record plus a key buffer for each
    partition. Every time a new entry is requested we will fetch a new
    entry from the partition that is currently not filled with an entry.
    Then the entry is put into its proper sort position.

    Returning a record is done by getting the top record, copying the
    record to the request buffer and setting the partition as empty on
    entries.
*/

int ha_partition::handle_ordered_index_scan(uchar *buf, bool reverse_order)
{
  int error;
  uint i;
  uint j= queue_first_element(&m_queue);
  uint smallest_range_seq= 0;
  bool found= FALSE;
  uchar *part_rec_buf_ptr= m_ordered_rec_buffer;
  int saved_error= HA_ERR_END_OF_FILE;
  DBUG_ENTER("ha_partition::handle_ordered_index_scan");
  DBUG_PRINT("enter", ("partition this: %p", this));

   if (m_pre_calling)
     error= handle_pre_scan(reverse_order, m_pre_call_use_parallel);
   else
     error= handle_pre_scan(reverse_order, check_parallel_search());
   if (unlikely(error))
    DBUG_RETURN(error);

  if (m_key_not_found)
  {
    /* m_key_not_found was set in the previous call to this function */
    m_key_not_found= false;
    bitmap_clear_all(&m_key_not_found_partitions);
  }
  m_top_entry= NO_CURRENT_PART_ID;
  DBUG_PRINT("info", ("partition queue_remove_all(1)"));
  queue_remove_all(&m_queue);
  DBUG_ASSERT(bitmap_is_set(&m_part_info->read_partitions,
                            m_part_spec.start_part));

  /*
    Position part_rec_buf_ptr to point to the first used partition >=
    start_part. There may be partitions marked by used_partitions,
    but is before start_part. These partitions has allocated record buffers
    but is dynamically pruned, so those buffers must be skipped.
  */
  for (i= bitmap_get_first_set(&m_part_info->read_partitions);
       i < m_part_spec.start_part;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
  {
    part_rec_buf_ptr+= m_priority_queue_rec_len;
  }
  DBUG_PRINT("info", ("m_part_spec.start_part %u first_used_part %u",
                      m_part_spec.start_part, i));
  for (/* continue from above */ ;
       i <= m_part_spec.end_part ;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i),
       part_rec_buf_ptr+= m_priority_queue_rec_len)
  {
    DBUG_PRINT("info", ("reading from part %u (scan_type: %u)",
                        i, m_index_scan_type));
    DBUG_ASSERT(i == uint2korr(part_rec_buf_ptr));
    uchar *rec_buf_ptr= part_rec_buf_ptr + PARTITION_BYTES_IN_POS;
    handler *file= m_file[i];

    switch (m_index_scan_type) {
    case partition_index_read:
      error= file->ha_index_read_map(rec_buf_ptr,
                                     m_start_key.key,
                                     m_start_key.keypart_map,
                                     m_start_key.flag);
      /* Caller has specified reverse_order */
      break;
    case partition_index_first:
      error= file->ha_index_first(rec_buf_ptr);
      reverse_order= FALSE;
      break;
    case partition_index_last:
      error= file->ha_index_last(rec_buf_ptr);
      reverse_order= TRUE;
      break;
    case partition_read_range:
    {
      /*
        This can only read record to table->record[0], as it was set when
        the table was being opened. We have to memcpy data ourselves.
      */
      error= file->read_range_first(m_start_key.key? &m_start_key: NULL,
                                    end_range, eq_range, TRUE);
      if (likely(!error))
        memcpy(rec_buf_ptr, table->record[0], m_rec_length);
      reverse_order= FALSE;
      break;
    }
    case partition_read_multi_range:
    {
      if (!bitmap_is_set(&m_mrr_used_partitions, i))
        continue;
      DBUG_PRINT("info", ("partition %u", i));
      error= file->multi_range_read_next(&m_range_info[i]);
      DBUG_PRINT("info", ("error: %d", error));
      if (error == HA_ERR_KEY_NOT_FOUND || error == HA_ERR_END_OF_FILE)
      {
        bitmap_clear_bit(&m_mrr_used_partitions, i);
        continue;
      }
      if (likely(!error))
      {
        memcpy(rec_buf_ptr, table->record[0], m_rec_length);
        reverse_order= FALSE;
        m_stock_range_seq[i]= (((PARTITION_KEY_MULTI_RANGE *)
                                m_range_info[i])->id);
        /* Test if the key is in the first key range */
        if (m_stock_range_seq[i] != m_mrr_range_current->id)
        {
          /*
            smallest_range_seq contains the smallest key range we have seen
            so far
          */
          if (!smallest_range_seq || smallest_range_seq > m_stock_range_seq[i])
            smallest_range_seq= m_stock_range_seq[i];
          continue;
        }
      }
      break;
    }
    default:
      DBUG_ASSERT(FALSE);
      DBUG_RETURN(HA_ERR_END_OF_FILE);
    }
    if (likely(!error))
    {
      found= TRUE;
      if (!m_using_extended_keys)
      {
        file->position(rec_buf_ptr);
        memcpy(rec_buf_ptr + m_rec_length, file->ref, file->ref_length);
      }
      /*
        Initialize queue without order first, simply insert
      */
      queue_element(&m_queue, j++)= part_rec_buf_ptr;
    }
    else if (error == HA_ERR_KEY_NOT_FOUND)
    {
      DBUG_PRINT("info", ("HA_ERR_KEY_NOT_FOUND from partition %u", i));
      bitmap_set_bit(&m_key_not_found_partitions, i);
      m_key_not_found= true;
      saved_error= error;
    }
    else if (error != HA_ERR_END_OF_FILE)
    {
      DBUG_RETURN(error);
    }
  }

  if (!found && smallest_range_seq)
  {
    /* We know that there is an existing row based on code above */
    found= TRUE;
    part_rec_buf_ptr= m_ordered_rec_buffer;

    /*
      No key found in the first key range
      Collect all partitions that has a key in smallest_range_seq
     */
    DBUG_PRINT("info", ("partition !found && smallest_range_seq"));
    for (i= bitmap_get_first_set(&m_part_info->read_partitions);
         i <= m_part_spec.end_part;
         i= bitmap_get_next_set(&m_part_info->read_partitions, i))
    {
      DBUG_PRINT("info", ("partition current_part: %u", i));
      if (i < m_part_spec.start_part)
      {
        part_rec_buf_ptr+= m_priority_queue_rec_len;
        DBUG_PRINT("info", ("partition i < m_part_spec.start_part"));
        continue;
      }
      if (!bitmap_is_set(&m_mrr_used_partitions, i))
      {
        part_rec_buf_ptr+= m_priority_queue_rec_len;
        DBUG_PRINT("info", ("partition !bitmap_is_set(&m_mrr_used_partitions, i)"));
        continue;
      }
      DBUG_ASSERT(i == uint2korr(part_rec_buf_ptr));
      if (smallest_range_seq == m_stock_range_seq[i])
      {
        m_stock_range_seq[i]= 0;
        queue_element(&m_queue, j++)= (uchar *) part_rec_buf_ptr;
        DBUG_PRINT("info", ("partition smallest_range_seq == m_stock_range_seq[i]"));
      }
      part_rec_buf_ptr+= m_priority_queue_rec_len;
    }

    /* Update global m_mrr_range_current to the current range */
    while (m_mrr_range_current->id < smallest_range_seq)
      m_mrr_range_current= m_mrr_range_current->next;
  }
  if (found)
  {
    /*
      We found at least one partition with data, now sort all entries and
      after that read the first entry and copy it to the buffer to return in.
    */
    queue_set_max_at_top(&m_queue, reverse_order);
    queue_set_cmp_arg(&m_queue, (void*) this);
    m_queue.elements= j - queue_first_element(&m_queue);
    queue_fix(&m_queue);
    return_top_record(buf);
    DBUG_PRINT("info", ("Record returned from partition %u", m_top_entry));
    DBUG_RETURN(0);
  }
  DBUG_RETURN(saved_error);
}


/*
  Return the top record in sort order

  SYNOPSIS
    return_top_record()
    out:buf                  Row returned in MySQL Row Format

  RETURN VALUE
    NONE
*/

void ha_partition::return_top_record(uchar *buf)
{
  uint part_id;
  uchar *key_buffer= queue_top(&m_queue);
  uchar *rec_buffer= key_buffer + PARTITION_BYTES_IN_POS;
  DBUG_ENTER("ha_partition::return_top_record");
  DBUG_PRINT("enter", ("partition this: %p", this));

  part_id= uint2korr(key_buffer);
  memcpy(buf, rec_buffer, m_rec_length);
  m_last_part= part_id;
  DBUG_PRINT("info", ("partition m_last_part: %u", m_last_part));
  m_top_entry= part_id;
  table->status= 0;                             // Found an existing row
  m_file[part_id]->return_record_by_parent();
  DBUG_VOID_RETURN;
}

/*
  This function is only used if the partitioned table has own partitions.
  This can happen if the partitioned VP engine is used (part of spider).
*/

void ha_partition::return_record_by_parent()
{
  m_file[m_last_part]->return_record_by_parent();
  DBUG_ASSERT(0);
}


/**
  Add index_next/prev from partitions without exact match.

  If there where any partitions that returned HA_ERR_KEY_NOT_FOUND when
  ha_index_read_map was done, those partitions must be included in the
  following index_next/prev call.
*/

int ha_partition::handle_ordered_index_scan_key_not_found()
{
  int error;
  uint i, old_elements= m_queue.elements;
  uchar *part_buf= m_ordered_rec_buffer;
  uchar *curr_rec_buf= NULL;
  DBUG_ENTER("ha_partition::handle_ordered_index_scan_key_not_found");
  DBUG_PRINT("enter", ("partition this: %p", this));
  DBUG_ASSERT(m_key_not_found);
  /*
    Loop over all used partitions to get the correct offset
    into m_ordered_rec_buffer.
  */
  for (i= bitmap_get_first_set(&m_part_info->read_partitions);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
  {
    if (bitmap_is_set(&m_key_not_found_partitions, i))
    {
      /*
        This partition is used and did return HA_ERR_KEY_NOT_FOUND
        in index_read_map.
      */
      curr_rec_buf= part_buf + PARTITION_BYTES_IN_POS;
      error= m_file[i]->ha_index_next(curr_rec_buf);
      /* HA_ERR_KEY_NOT_FOUND is not allowed from index_next! */
      DBUG_ASSERT(error != HA_ERR_KEY_NOT_FOUND);
      if (likely(!error))
      {
        DBUG_PRINT("info", ("partition queue_insert(1)"));
        queue_insert(&m_queue, part_buf);
      }
      else if (error != HA_ERR_END_OF_FILE && error != HA_ERR_KEY_NOT_FOUND)
        DBUG_RETURN(error);
    }
    part_buf += m_priority_queue_rec_len;
  }
  DBUG_ASSERT(curr_rec_buf);
  bitmap_clear_all(&m_key_not_found_partitions);
  m_key_not_found= false;

  if (m_queue.elements > old_elements)
  {
    /* Update m_top_entry, which may have changed. */
    uchar *key_buffer= queue_top(&m_queue);
    m_top_entry= uint2korr(key_buffer);
  }
  DBUG_RETURN(0);
}


/*
  Common routine to handle index_next with ordered results

  SYNOPSIS
    handle_ordered_next()
    out:buf                       Read row in MySQL Row Format
    next_same                     Called from index_next_same

  RETURN VALUE
    HA_ERR_END_OF_FILE            End of scan
    0                             Success
    other                         Error code
*/

int ha_partition::handle_ordered_next(uchar *buf, bool is_next_same)
{
  int error;
  DBUG_ENTER("ha_partition::handle_ordered_next");

  if (m_top_entry == NO_CURRENT_PART_ID)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  uint part_id= m_top_entry;
  uchar *rec_buf= queue_top(&m_queue) + PARTITION_BYTES_IN_POS;
  handler *file;

  if (m_key_not_found)
  {
    if (is_next_same)
    {
      /* Only rows which match the key. */
      m_key_not_found= false;
      bitmap_clear_all(&m_key_not_found_partitions);
    }
    else
    {
      /* There are partitions not included in the index record queue. */
      uint old_elements= m_queue.elements;
      if (unlikely((error= handle_ordered_index_scan_key_not_found())))
        DBUG_RETURN(error);
      /*
        If the queue top changed, i.e. one of the partitions that gave
        HA_ERR_KEY_NOT_FOUND in index_read_map found the next record,
        return it.
        Otherwise replace the old with a call to index_next (fall through).
      */
      if (old_elements != m_queue.elements && part_id != m_top_entry)
      {
        return_top_record(buf);
        DBUG_RETURN(0);
      }
    }
  }
  if (part_id >= m_tot_parts)
  {
    /* This should never happen! */
    DBUG_ASSERT(0);
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }

  file= m_file[part_id];

  if (m_index_scan_type == partition_read_range)
  {
    error= file->read_range_next();
    memcpy(rec_buf, table->record[0], m_rec_length);
  }
  else if (m_index_scan_type == partition_read_multi_range)
  {
    DBUG_PRINT("info", ("partition_read_multi_range route"));
    DBUG_PRINT("info", ("part_id: %u", part_id));
    bool get_next= FALSE;
    error= file->multi_range_read_next(&m_range_info[part_id]);
    DBUG_PRINT("info", ("error: %d", error));
    if (unlikely(error == HA_ERR_KEY_NOT_FOUND))
      error= HA_ERR_END_OF_FILE;
    if (unlikely(error == HA_ERR_END_OF_FILE))
    {
      bitmap_clear_bit(&m_mrr_used_partitions, part_id);
      DBUG_PRINT("info", ("partition m_queue.elements: %u", m_queue.elements));
      if (m_queue.elements)
      {
        DBUG_PRINT("info", ("partition queue_remove_top(1)"));
        queue_remove_top(&m_queue);
        if (m_queue.elements)
        {
          return_top_record(buf);
          DBUG_PRINT("info", ("Record returned from partition %u (3)",
                              m_top_entry));
          DBUG_RETURN(0);
        }
      }
      get_next= TRUE;
    }
    else if (likely(!error))
    {
      DBUG_PRINT("info", ("m_range_info[%u])->id: %u", part_id,
                          ((PARTITION_KEY_MULTI_RANGE *)
                           m_range_info[part_id])->id));
      DBUG_PRINT("info", ("m_mrr_range_current->id: %u",
                          m_mrr_range_current->id));
      memcpy(rec_buf, table->record[0], m_rec_length);
      if (((PARTITION_KEY_MULTI_RANGE *) m_range_info[part_id])->id !=
          m_mrr_range_current->id)
      {
        m_stock_range_seq[part_id]=
          ((PARTITION_KEY_MULTI_RANGE *) m_range_info[part_id])->id;
        DBUG_PRINT("info", ("partition queue_remove_top(2)"));
        queue_remove_top(&m_queue);
        if (!m_queue.elements)
          get_next= TRUE;
      }
    }
    if (get_next)
    {
      DBUG_PRINT("info", ("get_next route"));
      uint i, j= 0, smallest_range_seq= UINT_MAX32;
      for (i= m_part_spec.start_part; i <= m_part_spec.end_part; i++)
      {
        if (!(bitmap_is_set(&(m_part_info->read_partitions), i)))
          continue;
        if (!bitmap_is_set(&m_mrr_used_partitions, i))
          continue;
        if (smallest_range_seq > m_stock_range_seq[i])
          smallest_range_seq= m_stock_range_seq[i];
      }

      DBUG_PRINT("info", ("smallest_range_seq: %u", smallest_range_seq));
      if (smallest_range_seq != UINT_MAX32)
      {
        uchar *part_rec_buf_ptr= m_ordered_rec_buffer;
        DBUG_PRINT("info", ("partition queue_remove_all(2)"));
        queue_remove_all(&m_queue);
        DBUG_PRINT("info", ("m_part_spec.start_part: %u",
          m_part_spec.start_part));

        for (i= bitmap_get_first_set(&m_part_info->read_partitions);
             i <= m_part_spec.end_part;
             i= bitmap_get_next_set(&m_part_info->read_partitions, i),
               part_rec_buf_ptr+= m_priority_queue_rec_len)
        {
          DBUG_PRINT("info",("partition part_id: %u", i));
          if (i < m_part_spec.start_part)
          {
            DBUG_PRINT("info",("partition i < m_part_spec.start_part"));
            continue;
          }
          if (!bitmap_is_set(&m_mrr_used_partitions, i))
          {
            DBUG_PRINT("info",("partition !bitmap_is_set(&m_mrr_used_partitions, i)"));
            continue;
          }
          DBUG_PRINT("info",("partition uint2korr: %u",
                             uint2korr(part_rec_buf_ptr)));
          DBUG_ASSERT(i == uint2korr(part_rec_buf_ptr));
          DBUG_PRINT("info", ("partition m_stock_range_seq[%u]: %u",
                              i, m_stock_range_seq[i]));
          if (smallest_range_seq == m_stock_range_seq[i])
          {
            m_stock_range_seq[i]= 0;
            DBUG_PRINT("info", ("partition queue_insert(2)"));
            queue_insert(&m_queue, part_rec_buf_ptr);
            j++;
          }
        }
        while (m_mrr_range_current->id < smallest_range_seq)
          m_mrr_range_current= m_mrr_range_current->next;

        DBUG_PRINT("info",("partition m_mrr_range_current: %p",
                           m_mrr_range_current));
        DBUG_PRINT("info",("partition m_mrr_range_current->id: %u",
                           m_mrr_range_current ? m_mrr_range_current->id : 0));
        queue_set_max_at_top(&m_queue, FALSE);
        queue_set_cmp_arg(&m_queue, (void*) this);
        m_queue.elements= j;
        queue_fix(&m_queue);
        return_top_record(buf);
        DBUG_PRINT("info", ("Record returned from partition %u (4)",
                            m_top_entry));
        DBUG_RETURN(0);
      }
    }
  }
  else if (!is_next_same)
    error= file->ha_index_next(rec_buf);
  else
    error= file->ha_index_next_same(rec_buf, m_start_key.key,
                                    m_start_key.length);

  if (unlikely(error))
  {
    if (error == HA_ERR_END_OF_FILE && m_queue.elements)
    {
      /* Return next buffered row */
      DBUG_PRINT("info", ("partition queue_remove_top(3)"));
      queue_remove_top(&m_queue);
      if (m_queue.elements)
      {
         return_top_record(buf);
         DBUG_PRINT("info", ("Record returned from partition %u (2)",
                     m_top_entry));
         error= 0;
      }
    }
    DBUG_RETURN(error);
  }

  if (!m_using_extended_keys)
  {
    file->position(rec_buf);
    memcpy(rec_buf + m_rec_length, file->ref, file->ref_length);
  }

  queue_replace_top(&m_queue);
  return_top_record(buf);
  DBUG_PRINT("info", ("Record returned from partition %u", m_top_entry));
  DBUG_RETURN(0);
}


/*
  Common routine to handle index_prev with ordered results

  SYNOPSIS
    handle_ordered_prev()
    out:buf                       Read row in MySQL Row Format

  RETURN VALUE
    HA_ERR_END_OF_FILE            End of scan
    0                             Success
    other                         Error code
*/

int ha_partition::handle_ordered_prev(uchar *buf)
{
  int error;
  DBUG_ENTER("ha_partition::handle_ordered_prev");
  DBUG_PRINT("enter", ("partition: %p", this));

  if (m_top_entry == NO_CURRENT_PART_ID)
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  uint part_id= m_top_entry;
  uchar *rec_buf= queue_top(&m_queue) + PARTITION_BYTES_IN_POS;
  handler *file= m_file[part_id];

  if (unlikely((error= file->ha_index_prev(rec_buf))))
  {
    if (error == HA_ERR_END_OF_FILE && m_queue.elements)
    {
      DBUG_PRINT("info", ("partition queue_remove_top(4)"));
      queue_remove_top(&m_queue);
      if (m_queue.elements)
      {
	return_top_record(buf);
	DBUG_PRINT("info", ("Record returned from partition %u (2)",
			    m_top_entry));
        error= 0;
      }
    }
    DBUG_RETURN(error);
  }
  queue_replace_top(&m_queue);
  return_top_record(buf);
  DBUG_PRINT("info", ("Record returned from partition %u", m_top_entry));
  DBUG_RETURN(0);
}


/****************************************************************************
                MODULE information calls
****************************************************************************/

/*
  These are all first approximations of the extra, info, scan_time
  and read_time calls
*/

/**
  Helper function for sorting according to number of rows in descending order.
*/

int ha_partition::compare_number_of_records(ha_partition *me,
                                            const uint32 *a,
                                            const uint32 *b)
{
  handler **file= me->m_file;
  /* Note: sorting in descending order! */
  if (file[*a]->stats.records > file[*b]->stats.records)
    return -1;
  if (file[*a]->stats.records < file[*b]->stats.records)
    return 1;
  return 0;
}


/*
  General method to gather info from handler

  SYNOPSIS
    info()
    flag              Specifies what info is requested

  RETURN VALUE
    NONE

  DESCRIPTION
    ::info() is used to return information to the optimizer.
    Currently this table handler doesn't implement most of the fields
    really needed. SHOW also makes use of this data
    Another note, if your handler doesn't provide exact record count,
    you will probably want to have the following in your code:
    if (records < 2)
      records = 2;
    The reason is that the server will optimize for cases of only a single
    record. If in a table scan you don't know the number of records
    it will probably be better to set records to two so you can return
    as many records as you need.

    Along with records a few more variables you may wish to set are:
      records
      deleted
      data_file_length
      index_file_length
      delete_length
      check_time
    Take a look at the public variables in handler.h for more information.

    Called in:
      filesort.cc
      ha_heap.cc
      item_sum.cc
      opt_sum.cc
      sql_delete.cc
     sql_delete.cc
     sql_derived.cc
      sql_select.cc
      sql_select.cc
      sql_select.cc
      sql_select.cc
      sql_select.cc
      sql_show.cc
      sql_show.cc
      sql_show.cc
      sql_show.cc
      sql_table.cc
      sql_union.cc
      sql_update.cc

    Some flags that are not implemented
      HA_STATUS_POS:
        This parameter is never used from the MySQL Server. It is checked in a
        place in MyISAM so could potentially be used by MyISAM specific
        programs.
      HA_STATUS_NO_LOCK:
      This is declared and often used. It's only used by MyISAM.
      It means that MySQL doesn't need the absolute latest statistics
      information. This may save the handler from doing internal locks while
      retrieving statistics data.
*/

int ha_partition::info(uint flag)
{
  uint no_lock_flag= flag & HA_STATUS_NO_LOCK;
  uint extra_var_flag= flag & HA_STATUS_VARIABLE_EXTRA;
  DBUG_ENTER("ha_partition::info");

#ifndef DBUG_OFF
  if (bitmap_is_set_all(&(m_part_info->read_partitions)))
    DBUG_PRINT("info", ("All partitions are used"));
#endif /* DBUG_OFF */
  if (flag & HA_STATUS_AUTO)
  {
    bool auto_inc_is_first_in_idx= (table_share->next_number_keypart == 0);
    DBUG_PRINT("info", ("HA_STATUS_AUTO"));
    if (!table->found_next_number_field)
      stats.auto_increment_value= 0;
    else if (part_share->auto_inc_initialized)
    {
      lock_auto_increment();
      stats.auto_increment_value= part_share->next_auto_inc_val;
      unlock_auto_increment();
    }
    else
    {
      lock_auto_increment();
      /* to avoid two concurrent initializations, check again when locked */
      if (part_share->auto_inc_initialized)
        stats.auto_increment_value= part_share->next_auto_inc_val;
      else
      {
        /*
          The auto-inc mutex in the table_share is locked, so we do not need
          to have the handlers locked.
          HA_STATUS_NO_LOCK is not checked, since we cannot skip locking
          the mutex, because it is initialized.
        */
        handler *file, **file_array;
        ulonglong auto_increment_value= 0;
        file_array= m_file;
        DBUG_PRINT("info",
                   ("checking all partitions for auto_increment_value"));
        do
        {
          file= *file_array;
          file->info(HA_STATUS_AUTO | no_lock_flag);
          set_if_bigger(auto_increment_value,
                        file->stats.auto_increment_value);
        } while (*(++file_array));

        DBUG_ASSERT(auto_increment_value);
        stats.auto_increment_value= auto_increment_value;
        if (auto_inc_is_first_in_idx)
        {
          set_if_bigger(part_share->next_auto_inc_val,
                        auto_increment_value);
          if (can_use_for_auto_inc_init())
            part_share->auto_inc_initialized= true;
          DBUG_PRINT("info", ("initializing next_auto_inc_val to %lu",
                       (ulong) part_share->next_auto_inc_val));
        }
      }
      unlock_auto_increment();
    }
  }
  if (flag & HA_STATUS_VARIABLE)
  {
    uint i;
    DBUG_PRINT("info", ("HA_STATUS_VARIABLE"));
    /*
      Calculates statistical variables
      records:           Estimate of number records in table
      We report sum (always at least 2 if not empty)
      deleted:           Estimate of number holes in the table due to
      deletes
      We report sum
      data_file_length:  Length of data file, in principle bytes in table
      We report sum
      index_file_length: Length of index file, in principle bytes in
      indexes in the table
      We report sum
      delete_length: Length of free space easily used by new records in table
      We report sum
      mean_record_length:Mean record length in the table
      We calculate this
      check_time:        Time of last check (only applicable to MyISAM)
      We report last time of all underlying handlers
    */
    handler *file;
    stats.records= 0;
    stats.deleted= 0;
    stats.data_file_length= 0;
    stats.index_file_length= 0;
    stats.check_time= 0;
    stats.delete_length= 0;
    for (i= bitmap_get_first_set(&m_part_info->read_partitions);
         i < m_tot_parts;
         i= bitmap_get_next_set(&m_part_info->read_partitions, i))
    {
      file= m_file[i];
      file->info(HA_STATUS_VARIABLE | no_lock_flag | extra_var_flag);
      stats.records+= file->stats.records;
      stats.deleted+= file->stats.deleted;
      stats.data_file_length+= file->stats.data_file_length;
      stats.index_file_length+= file->stats.index_file_length;
      stats.delete_length+= file->stats.delete_length;
      if (file->stats.check_time > stats.check_time)
        stats.check_time= file->stats.check_time;
    }
    if (stats.records && stats.records < 2 &&
        !(m_file[0]->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT))
      stats.records= 2;
    if (stats.records > 0)
      stats.mean_rec_length= (ulong) (stats.data_file_length / stats.records);
    else
      stats.mean_rec_length= 0;
  }
  if (flag & HA_STATUS_CONST)
  {
    DBUG_PRINT("info", ("HA_STATUS_CONST"));
    /*
      Recalculate loads of constant variables. MyISAM also sets things
      directly on the table share object.

      Check whether this should be fixed since handlers should not
      change things directly on the table object.

      Monty comment: This should NOT be changed!  It's the handlers
      responsibility to correct table->s->keys_xxxx information if keys
      have been disabled.

      The most important parameters set here is records per key on
      all indexes. block_size and primar key ref_length.

      For each index there is an array of rec_per_key.
      As an example if we have an index with three attributes a,b and c
      we will have an array of 3 rec_per_key.
      rec_per_key[0] is an estimate of number of records divided by
      number of unique values of the field a.
      rec_per_key[1] is an estimate of the number of records divided
      by the number of unique combinations of the fields a and b.
      rec_per_key[2] is an estimate of the number of records divided
      by the number of unique combinations of the fields a,b and c.

      Many handlers only set the value of rec_per_key when all fields
      are bound (rec_per_key[2] in the example above).

      If the handler doesn't support statistics, it should set all of the
      above to 0.

      We first scans through all partitions to get the one holding most rows.
      We will then allow the handler with the most rows to set
      the rec_per_key and use this as an estimate on the total table.

      max_data_file_length:     Maximum data file length
      We ignore it, is only used in
      SHOW TABLE STATUS
      max_index_file_length:    Maximum index file length
      We ignore it since it is never used
      block_size:               Block size used
      We set it to the value of the first handler
      ref_length:               We set this to the value calculated
      and stored in local object
      create_time:              Creation time of table

      So we calculate these constants by using the variables from the
      handler with most rows.
    */
    handler *file, **file_array;
    ulonglong max_records= 0;
    uint32 i= 0;
    uint32 handler_instance= 0;

    file_array= m_file;
    do
    {
      file= *file_array;
      if (bitmap_is_set(&(m_opened_partitions), (uint)(file_array - m_file)))
      {
        /* Get variables if not already done */
        if (!(flag & HA_STATUS_VARIABLE) ||
            !bitmap_is_set(&(m_part_info->read_partitions),
                           (uint) (file_array - m_file)))
          file->info(HA_STATUS_VARIABLE | no_lock_flag | extra_var_flag);
        if (file->stats.records > max_records)
        {
          max_records= file->stats.records;
          handler_instance= i;
        }
      }
      i++;
    } while (*(++file_array));
    /*
      Sort the array of part_ids by number of records in
      in descending order.
    */
    my_qsort2((void*) m_part_ids_sorted_by_num_of_records,
              m_tot_parts,
              sizeof(uint32),
              (qsort2_cmp) compare_number_of_records,
              this);

    file= m_file[handler_instance];
    file->info(HA_STATUS_CONST | no_lock_flag);
    stats.block_size= file->stats.block_size;
    stats.create_time= file->stats.create_time;
    ref_length= m_ref_length;
  }
  if (flag & HA_STATUS_ERRKEY)
  {
    handler *file= m_file[m_last_part];
    DBUG_PRINT("info", ("info: HA_STATUS_ERRKEY"));
    /*
      This flag is used to get index number of the unique index that
      reported duplicate key
      We will report the errkey on the last handler used and ignore the rest
      Note: all engines does not support HA_STATUS_ERRKEY, so set errkey.
    */
    file->errkey= errkey;
    file->info(HA_STATUS_ERRKEY | no_lock_flag);
    errkey= file->errkey;
  }
  if (flag & HA_STATUS_TIME)
  {
    handler *file, **file_array;
    DBUG_PRINT("info", ("info: HA_STATUS_TIME"));
    /*
      This flag is used to set the latest update time of the table.
      Used by SHOW commands
      We will report the maximum of these times
    */
    stats.update_time= 0;
    file_array= m_file;
    do
    {
      file= *file_array;
      file->info(HA_STATUS_TIME | no_lock_flag);
      if (file->stats.update_time > stats.update_time)
	stats.update_time= file->stats.update_time;
    } while (*(++file_array));
  }
  DBUG_RETURN(0);
}


void ha_partition::get_dynamic_partition_info(PARTITION_STATS *stat_info,
                                              uint part_id)
{
  handler *file= m_file[part_id];
  DBUG_ASSERT(bitmap_is_set(&(m_part_info->read_partitions), part_id));
  file->info(HA_STATUS_TIME | HA_STATUS_VARIABLE |
             HA_STATUS_VARIABLE_EXTRA | HA_STATUS_NO_LOCK);

  stat_info->records=              file->stats.records;
  stat_info->mean_rec_length=      file->stats.mean_rec_length;
  stat_info->data_file_length=     file->stats.data_file_length;
  stat_info->max_data_file_length= file->stats.max_data_file_length;
  stat_info->index_file_length=    file->stats.index_file_length;
  stat_info->max_index_file_length= file->stats.max_index_file_length;
  stat_info->delete_length=        file->stats.delete_length;
  stat_info->create_time=          file->stats.create_time;
  stat_info->update_time=          file->stats.update_time;
  stat_info->check_time=           file->stats.check_time;
  stat_info->check_sum= 0;
  if (file->ha_table_flags() & (HA_HAS_OLD_CHECKSUM | HA_HAS_NEW_CHECKSUM))
    stat_info->check_sum= file->checksum();
  return;
}


void ha_partition::set_partitions_to_open(List<String> *partition_names)
{
  m_partitions_to_open= partition_names;
}


int ha_partition::open_read_partitions(char *name_buff, size_t name_buff_size)
{
  handler **file;
  char *name_buffer_ptr;
  int error= 0;

  name_buffer_ptr= m_name_buffer_ptr;
  file= m_file;
  m_file_sample= NULL;
  do
  {
    int n_file= (int)(file-m_file);
    int is_open= bitmap_is_set(&m_opened_partitions, n_file);
    int should_be_open= bitmap_is_set(&m_part_info->read_partitions, n_file);

    /*
      TODO: we can close some opened partitions if they're not
      used in the query. It probably should be syncronized with the
      table_open_cache value.

      if (is_open && !should_be_open)
      {
        if (unlikely((error= (*file)->ha_close())))
          goto err_handler;
        bitmap_clear_bit(&m_opened_partitions, n_file);
      }
      else
    */
    if (!is_open && should_be_open)
    {
      LEX_CSTRING save_connect_string= table->s->connect_string;
      if (unlikely((error=
                    create_partition_name(name_buff, name_buff_size,
                                          table->s->normalized_path.str,
                                          name_buffer_ptr, NORMAL_PART_NAME,
                                          FALSE))))
        goto err_handler;
      if (!((*file)->ht->flags & HTON_CAN_READ_CONNECT_STRING_IN_PARTITION))
        table->s->connect_string= m_connect_string[(uint)(file-m_file)];
      error= (*file)->ha_open(table, name_buff, m_mode,
                              m_open_test_lock | HA_OPEN_NO_PSI_CALL);
      table->s->connect_string= save_connect_string;
      if (error)
        goto err_handler;
      bitmap_set_bit(&m_opened_partitions, n_file);
      m_last_part= n_file;
    }
    if (!m_file_sample && should_be_open)
      m_file_sample= *file;
    name_buffer_ptr+= strlen(name_buffer_ptr) + 1;
  } while (*(++file));
  
err_handler:
  return error;
}


int ha_partition::change_partitions_to_open(List<String> *partition_names)
{
  char name_buff[FN_REFLEN+1];
  int error= 0;

  if (m_is_clone_of)
    return 0;

  m_partitions_to_open= partition_names;
  if (unlikely((error= m_part_info->set_partition_bitmaps(partition_names))))
    goto err_handler;

  if (m_lock_type != F_UNLCK)
  {
    /*
      That happens after the LOCK TABLE statement.
      Do nothing in this case.
    */
    return 0;
  }

  if (bitmap_cmp(&m_opened_partitions, &m_part_info->read_partitions) != 0)
    return 0;

  if (unlikely((error= read_par_file(table->s->normalized_path.str)) ||
               (error= open_read_partitions(name_buff, sizeof(name_buff)))))
    goto err_handler;

  clear_handler_file();

err_handler:
  return error;
}


/**
  General function to prepare handler for certain behavior.

  @param[in]    operation       operation to execute

  @return       status
    @retval     0               success
    @retval     >0              error code

  @detail

  extra() is called whenever the server wishes to send a hint to
  the storage engine. The MyISAM engine implements the most hints.

  We divide the parameters into the following categories:
  1) Operations used by most handlers
  2) Operations used by some non-MyISAM handlers
  3) Operations used only by MyISAM
  4) Operations only used by temporary tables for query processing
  5) Operations only used by MyISAM internally
  6) Operations not used at all
  7) Operations only used by federated tables for query processing
  8) Operations only used by NDB
  9) Operations only used by MERGE

  The partition handler need to handle category 1), 2) and 3).

  1) Operations used by most handlers
  -----------------------------------
  HA_EXTRA_RESET:
    This option is used by most handlers and it resets the handler state
    to the same state as after an open call. This includes releasing
    any READ CACHE or WRITE CACHE or other internal buffer used.

    It is called from the reset method in the handler interface. There are
    three instances where this is called.
    1) After completing a INSERT ... SELECT ... query the handler for the
       table inserted into is reset
    2) It is called from close_thread_table which in turn is called from
       close_thread_tables except in the case where the tables are locked
       in which case ha_commit_stmt is called instead.
       It is only called from here if refresh_version hasn't changed and the
       table is not an old table when calling close_thread_table.
       close_thread_tables is called from many places as a general clean up
       function after completing a query.
    3) It is called when deleting the QUICK_RANGE_SELECT object if the
       QUICK_RANGE_SELECT object had its own handler object. It is called
       immediatley before close of this local handler object.
  HA_EXTRA_KEYREAD:
  HA_EXTRA_NO_KEYREAD:
    These parameters are used to provide an optimisation hint to the handler.
    If HA_EXTRA_KEYREAD is set it is enough to read the index fields, for
    many handlers this means that the index-only scans can be used and it
    is not necessary to use the real records to satisfy this part of the
    query. Index-only scans is a very important optimisation for disk-based
    indexes. For main-memory indexes most indexes contain a reference to the
    record and thus KEYREAD only says that it is enough to read key fields.
    HA_EXTRA_NO_KEYREAD disables this for the handler, also HA_EXTRA_RESET
    will disable this option.
    The handler will set HA_KEYREAD_ONLY in its table flags to indicate this
    feature is supported.
  HA_EXTRA_FLUSH:
    Indication to flush tables to disk, is supposed to be used to
    ensure disk based tables are flushed at end of query execution.
    Currently is never used.

  HA_EXTRA_FORCE_REOPEN:
    Only used by MyISAM and Archive, called when altering table,
    closing tables to enforce a reopen of the table files.

  2) Operations used by some non-MyISAM handlers
  ----------------------------------------------
  HA_EXTRA_KEYREAD_PRESERVE_FIELDS:
    This is a strictly InnoDB feature that is more or less undocumented.
    When it is activated InnoDB copies field by field from its fetch
    cache instead of all fields in one memcpy. Have no idea what the
    purpose of this is.
    Cut from include/my_base.h:
    When using HA_EXTRA_KEYREAD, overwrite only key member fields and keep
    other fields intact. When this is off (by default) InnoDB will use memcpy
    to overwrite entire row.
  HA_EXTRA_IGNORE_DUP_KEY:
  HA_EXTRA_NO_IGNORE_DUP_KEY:
    Informs the handler to we will not stop the transaction if we get an
    duplicate key errors during insert/upate.
    Always called in pair, triggered by INSERT IGNORE and other similar
    SQL constructs.
    Not used by MyISAM.

  3) Operations used only by MyISAM
  ---------------------------------
  HA_EXTRA_NORMAL:
    Only used in MyISAM to reset quick mode, not implemented by any other
    handler. Quick mode is also reset in MyISAM by HA_EXTRA_RESET.

    It is called after completing a successful DELETE query if the QUICK
    option is set.

  HA_EXTRA_QUICK:
    When the user does DELETE QUICK FROM table where-clause; this extra
    option is called before the delete query is performed and
    HA_EXTRA_NORMAL is called after the delete query is completed.
    Temporary tables used internally in MySQL always set this option

    The meaning of quick mode is that when deleting in a B-tree no merging
    of leafs is performed. This is a common method and many large DBMS's
    actually only support this quick mode since it is very difficult to
    merge leaves in a tree used by many threads concurrently.

  HA_EXTRA_CACHE:
    This flag is usually set with extra_opt along with a cache size.
    The size of this buffer is set by the user variable
    record_buffer_size. The value of this cache size is the amount of
    data read from disk in each fetch when performing a table scan.
    This means that before scanning a table it is normal to call
    extra with HA_EXTRA_CACHE and when the scan is completed to call
    HA_EXTRA_NO_CACHE to release the cache memory.

    Some special care is taken when using this extra parameter since there
    could be a write ongoing on the table in the same statement. In this
    one has to take special care since there might be a WRITE CACHE as
    well. HA_EXTRA_CACHE specifies using a READ CACHE and using
    READ CACHE and WRITE CACHE at the same time is not possible.

    Only MyISAM currently use this option.

    It is set when doing full table scans using rr_sequential and
    reset when completing such a scan with end_read_record
    (resetting means calling extra with HA_EXTRA_NO_CACHE).

    It is set in filesort.cc for MyISAM internal tables and it is set in
    a multi-update where HA_EXTRA_CACHE is called on a temporary result
    table and after that ha_rnd_init(0) on table to be updated
    and immediately after that HA_EXTRA_NO_CACHE on table to be updated.

    Apart from that it is always used from init_read_record but not when
    used from UPDATE statements. It is not used from DELETE statements
    with ORDER BY and LIMIT but it is used in normal scan loop in DELETE
    statements. The reason here is that DELETE's in MyISAM doesn't move
    existings data rows.

    It is also set in copy_data_between_tables when scanning the old table
    to copy over to the new table.
    And it is set in join_init_read_record where quick objects are used
    to perform a scan on the table. In this case the full table scan can
    even be performed multiple times as part of the nested loop join.

    For purposes of the partition handler it is obviously necessary to have
    special treatment of this extra call. If we would simply pass this
    extra call down to each handler we would allocate
    cache size * no of partitions amount of memory and this is not
    necessary since we will only scan one partition at a time when doing
    full table scans.

    Thus we treat it by first checking whether we have MyISAM handlers in
    the table, if not we simply ignore the call and if we have we will
    record the call but will not call any underlying handler yet. Then
    when performing the sequential scan we will check this recorded value
    and call extra_opt whenever we start scanning a new partition.

  HA_EXTRA_NO_CACHE:
    When performing a UNION SELECT HA_EXTRA_NO_CACHE is called from the
    flush method in the select_union class.
    It is used to some extent when insert delayed inserts.
    See HA_EXTRA_RESET_STATE for use in conjunction with delete_all_rows().

    It should be ok to call HA_EXTRA_NO_CACHE on all underlying handlers
    if they are MyISAM handlers. Other handlers we can ignore the call
    for. If no cache is in use they will quickly return after finding
    this out. And we also ensure that all caches are disabled and no one
    is left by mistake.
    In the future this call will probably be deleted and we will instead call
    ::reset();

  HA_EXTRA_WRITE_CACHE:
    See above, called from various places. It is mostly used when we
    do INSERT ... SELECT
    No special handling to save cache space is developed currently.

  HA_EXTRA_PREPARE_FOR_UPDATE:
    This is called as part of a multi-table update. When the table to be
    updated is also scanned then this informs MyISAM handler to drop any
    caches if dynamic records are used (fixed size records do not care
    about this call). We pass this along to the first partition to scan, and
    flag that it is to be called after HA_EXTRA_CACHE when moving to the next
    partition to scan.

  HA_EXTRA_PREPARE_FOR_DROP:
    Only used by MyISAM, called in preparation for a DROP TABLE.
    It's used mostly by Windows that cannot handle dropping an open file.
    On other platforms it has the same effect as HA_EXTRA_FORCE_REOPEN.

  HA_EXTRA_PREPARE_FOR_RENAME:
    Informs the handler we are about to attempt a rename of the table.
    For handlers that have share open files (MyISAM key-file and
    Archive writer) they must close the files before rename is possible
    on Windows.

  HA_EXTRA_READCHECK:
  HA_EXTRA_NO_READCHECK:
    Only one call to HA_EXTRA_NO_READCHECK from ha_open where it says that
    this is not needed in SQL. The reason for this call is that MyISAM sets
    the READ_CHECK_USED in the open call so the call is needed for MyISAM
    to reset this feature.
    The idea with this parameter was to inform of doing/not doing a read
    check before applying an update. Since SQL always performs a read before
    applying the update No Read Check is needed in MyISAM as well.

    This is a cut from Docs/myisam.txt
     Sometimes you might want to force an update without checking whether
     another user has changed the record since you last read it. This is
     somewhat dangerous, so it should ideally not be used. That can be
     accomplished by wrapping the mi_update() call in two calls to mi_extra(),
     using these functions:
     HA_EXTRA_NO_READCHECK=5                 No readcheck on update
     HA_EXTRA_READCHECK=6                    Use readcheck (def)

  HA_EXTRA_REMEMBER_POS:
  HA_EXTRA_RESTORE_POS:
    System versioning needs this for MyISAM and Aria tables.
    On DELETE using PRIMARY KEY:
    1) handler::ha_index_read_map() saves rowid used for row delete/update
    2) handler::ha_update_row() can rewrite saved rowid
    3) handler::ha_delete_row()/handler::ha_update_row() expects saved but got
       different rowid and operation fails
    Using those flags prevents harmful side effect of 2)

  4) Operations only used by temporary tables for query processing
  ----------------------------------------------------------------
  HA_EXTRA_RESET_STATE:
    Same as reset() except that buffers are not released. If there is
    a READ CACHE it is reinit'ed. A cache is reinit'ed to restart reading
    or to change type of cache between READ CACHE and WRITE CACHE.

    This extra function is always called immediately before calling
    delete_all_rows on the handler for temporary tables.
    There are cases however when HA_EXTRA_RESET_STATE isn't called in
    a similar case for a temporary table in sql_union.cc and in two other
    cases HA_EXTRA_NO_CACHE is called before and HA_EXTRA_WRITE_CACHE
    called afterwards.
    The case with HA_EXTRA_NO_CACHE and HA_EXTRA_WRITE_CACHE means
    disable caching, delete all rows and enable WRITE CACHE. This is
    used for temporary tables containing distinct sums and a
    functional group.

    The only case that delete_all_rows is called on non-temporary tables
    is in sql_delete.cc when DELETE FROM table; is called by a user.
    In this case no special extra calls are performed before or after this
    call.

    The partition handler should not need to bother about this one. It
    should never be called.

  HA_EXTRA_NO_ROWS:
    Don't insert rows indication to HEAP and MyISAM, only used by temporary
    tables used in query processing.
    Not handled by partition handler.

  5) Operations only used by MyISAM internally
  --------------------------------------------
  HA_EXTRA_REINIT_CACHE:
    This call reinitializes the READ CACHE described above if there is one
    and otherwise the call is ignored.

    We can thus safely call it on all underlying handlers if they are
    MyISAM handlers. It is however never called so we don't handle it at all.
  HA_EXTRA_FLUSH_CACHE:
    Flush WRITE CACHE in MyISAM. It is only from one place in the code.
    This is in sql_insert.cc where it is called if the table_flags doesn't
    contain HA_DUPLICATE_POS. The only handler having the HA_DUPLICATE_POS
    set is the MyISAM handler and so the only handler not receiving this
    call is MyISAM.
    Thus in effect this call is called but never used. Could be removed
    from sql_insert.cc
  HA_EXTRA_NO_USER_CHANGE:
    Only used by MyISAM, never called.
    Simulates lock_type as locked.
  HA_EXTRA_WAIT_LOCK:
  HA_EXTRA_WAIT_NOLOCK:
    Only used by MyISAM, called from MyISAM handler but never from server
    code on top of the handler.
    Sets lock_wait on/off
  HA_EXTRA_NO_KEYS:
    Only used MyISAM, only used internally in MyISAM handler, never called
    from server level.
  HA_EXTRA_KEYREAD_CHANGE_POS:
  HA_EXTRA_PRELOAD_BUFFER_SIZE:
  HA_EXTRA_CHANGE_KEY_TO_DUP:
  HA_EXTRA_CHANGE_KEY_TO_UNIQUE:
    Only used by MyISAM, never called.

  6) Operations not used at all
  -----------------------------
  HA_EXTRA_KEY_CACHE:
  HA_EXTRA_NO_KEY_CACHE:
    This parameters are no longer used and could be removed.

  7) Operations only used by federated tables for query processing
  ----------------------------------------------------------------
  HA_EXTRA_INSERT_WITH_UPDATE:
    Inform handler that an "INSERT...ON DUPLICATE KEY UPDATE" will be
    executed. This condition is unset by HA_EXTRA_NO_IGNORE_DUP_KEY.

  8) Operations only used by NDB
  ------------------------------
  HA_EXTRA_DELETE_CANNOT_BATCH:
  HA_EXTRA_UPDATE_CANNOT_BATCH:
    Inform handler that delete_row()/update_row() cannot batch deletes/updates
    and should perform them immediately. This may be needed when table has
    AFTER DELETE/UPDATE triggers which access to subject table.
    These flags are reset by the handler::extra(HA_EXTRA_RESET) call.

  9) Operations only used by MERGE
  ------------------------------
  HA_EXTRA_ADD_CHILDREN_LIST:
  HA_EXTRA_ATTACH_CHILDREN:
  HA_EXTRA_IS_ATTACHED_CHILDREN:
  HA_EXTRA_DETACH_CHILDREN:
    Special actions for MERGE tables. Ignore.
*/

int ha_partition::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_partition:extra");
  DBUG_PRINT("enter", ("operation: %d", (int) operation));

  switch (operation) {
    /* Category 1), used by most handlers */
  case HA_EXTRA_KEYREAD:
  case HA_EXTRA_NO_KEYREAD:
  case HA_EXTRA_FLUSH:
  case HA_EXTRA_PREPARE_FOR_FORCED_CLOSE:
    DBUG_RETURN(loop_extra(operation));
  case HA_EXTRA_PREPARE_FOR_RENAME:
  case HA_EXTRA_FORCE_REOPEN:
    DBUG_RETURN(loop_extra_alter(operation));
    break;

    /* Category 2), used by non-MyISAM handlers */
  case HA_EXTRA_IGNORE_DUP_KEY:
  case HA_EXTRA_NO_IGNORE_DUP_KEY:
  case HA_EXTRA_KEYREAD_PRESERVE_FIELDS:
  {
    if (!m_myisam)
      DBUG_RETURN(loop_extra(operation));
  }
  break;

  /* Category 3), used by MyISAM handlers */
  case HA_EXTRA_PREPARE_FOR_UPDATE:
    /*
      Needs to be run on the first partition in the range now, and
      later in late_extra_cache, when switching to a new partition to scan.
    */
    m_extra_prepare_for_update= TRUE;
    if (m_part_spec.start_part != NO_CURRENT_PART_ID)
    {
      if (!m_extra_cache)
        m_extra_cache_part_id= m_part_spec.start_part;
      DBUG_ASSERT(m_extra_cache_part_id == m_part_spec.start_part);
      (void) m_file[m_part_spec.start_part]->extra(HA_EXTRA_PREPARE_FOR_UPDATE);
    }
    break;
  case HA_EXTRA_NORMAL:
  case HA_EXTRA_QUICK:
  case HA_EXTRA_PREPARE_FOR_DROP:
  case HA_EXTRA_FLUSH_CACHE:
  case HA_EXTRA_PREPARE_FOR_ALTER_TABLE:
  case HA_EXTRA_REMEMBER_POS:
  case HA_EXTRA_RESTORE_POS:
  {
    DBUG_RETURN(loop_extra(operation));
  }
  case HA_EXTRA_NO_READCHECK:
  {
    /*
      This is only done as a part of ha_open, which is also used in
      ha_partition::open, so no need to do anything.
    */
    break;
  }
  case HA_EXTRA_CACHE:
  {
    prepare_extra_cache(0);
    break;
  }
  case HA_EXTRA_NO_CACHE:
  {
    int ret= 0;
    if (m_extra_cache_part_id != NO_CURRENT_PART_ID)
      ret= m_file[m_extra_cache_part_id]->extra(HA_EXTRA_NO_CACHE);
    m_extra_cache= FALSE;
    m_extra_cache_size= 0;
    m_extra_prepare_for_update= FALSE;
    m_extra_cache_part_id= NO_CURRENT_PART_ID;
    DBUG_RETURN(ret);
  }
  case HA_EXTRA_WRITE_CACHE:
  {
    m_extra_cache= FALSE;
    m_extra_cache_size= 0;
    m_extra_prepare_for_update= FALSE;
    m_extra_cache_part_id= NO_CURRENT_PART_ID;
    DBUG_RETURN(loop_extra(operation));
  }
  case HA_EXTRA_IGNORE_NO_KEY:
  case HA_EXTRA_NO_IGNORE_NO_KEY:
  {
    /*
      Ignore as these are specific to NDB for handling
      idempotency
     */
    break;
  }
  case HA_EXTRA_WRITE_CAN_REPLACE:
  case HA_EXTRA_WRITE_CANNOT_REPLACE:
  {
    /*
      Informs handler that write_row() can replace rows which conflict
      with row being inserted by PK/unique key without reporting error
      to the SQL-layer.

      At this time, this is safe by limitation of ha_partition
    */
    DBUG_RETURN(loop_extra(operation));
  }
    /* Category 7), used by federated handlers */
  case HA_EXTRA_INSERT_WITH_UPDATE:
    DBUG_RETURN(loop_extra(operation));
    /* Category 8) Operations only used by NDB */
  case HA_EXTRA_DELETE_CANNOT_BATCH:
  case HA_EXTRA_UPDATE_CANNOT_BATCH:
  {
    /* Currently only NDB use the *_CANNOT_BATCH */
    break;
  }
    /* Category 9) Operations only used by MERGE */
  case HA_EXTRA_ADD_CHILDREN_LIST:
    DBUG_RETURN(loop_extra(operation));
  case HA_EXTRA_ATTACH_CHILDREN:
  {
    int result;
    uint num_locks;
    handler **file;
    if ((result= loop_extra(operation)))
      DBUG_RETURN(result);

    /* Recalculate lock count as each child may have different set of locks */
    num_locks= 0;
    file= m_file;
    do
    {
      num_locks+= (*file)->lock_count();
    } while (*(++file));

    m_num_locks= num_locks;
    break;
  }
  case HA_EXTRA_IS_ATTACHED_CHILDREN:
    DBUG_RETURN(loop_extra(operation));
  case HA_EXTRA_DETACH_CHILDREN:
    DBUG_RETURN(loop_extra(operation));
  case HA_EXTRA_MARK_AS_LOG_TABLE:
  /*
    http://dev.mysql.com/doc/refman/5.1/en/partitioning-limitations.html
    says we no longer support logging to partitioned tables, so we fail
    here.
  */
    DBUG_RETURN(ER_UNSUPORTED_LOG_ENGINE);
  case HA_EXTRA_STARTING_ORDERED_INDEX_SCAN:
  case HA_EXTRA_BEGIN_ALTER_COPY:
  case HA_EXTRA_END_ALTER_COPY:
  case HA_EXTRA_FAKE_START_STMT:
    DBUG_RETURN(loop_extra(operation));
  default:
  {
    /* Temporary crash to discover what is wrong */
    DBUG_ASSERT(0);
    break;
  }
  }
  DBUG_RETURN(0);
}


/**
  Special extra call to reset extra parameters

  @return Operation status.
    @retval >0 Error code
    @retval 0  Success

  @note Called at end of each statement to reset buffers.
  To avoid excessive calls, the m_partitions_to_reset bitmap keep records
  of which partitions that have been used in extra(), external_lock() or
  start_stmt() and is needed to be called.
*/

int ha_partition::reset(void)
{
  int result= 0;
  int tmp;
  uint i;
  DBUG_ENTER("ha_partition::reset");

  for (i= bitmap_get_first_set(&m_partitions_to_reset);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_partitions_to_reset, i))
  {
    if (bitmap_is_set(&m_opened_partitions, i) &&
        (tmp= m_file[i]->ha_reset()))
      result= tmp;
  }
  bitmap_clear_all(&m_partitions_to_reset);
  m_extra_prepare_for_update= FALSE;
  DBUG_RETURN(result);
}

/*
  Special extra method for HA_EXTRA_CACHE with cachesize as extra parameter

  SYNOPSIS
    extra_opt()
    operation                      Must be HA_EXTRA_CACHE
    cachesize                      Size of cache in full table scan

  RETURN VALUE
    >0                   Error code
    0                    Success
*/

int ha_partition::extra_opt(enum ha_extra_function operation, ulong cachesize)
{
  DBUG_ENTER("ha_partition::extra_opt()");

  DBUG_ASSERT(HA_EXTRA_CACHE == operation);
  prepare_extra_cache(cachesize);
  DBUG_RETURN(0);
}


/*
  Call extra on handler with HA_EXTRA_CACHE and cachesize

  SYNOPSIS
    prepare_extra_cache()
    cachesize                Size of cache for full table scan

  RETURN VALUE
    NONE
*/

void ha_partition::prepare_extra_cache(uint cachesize)
{
  DBUG_ENTER("ha_partition::prepare_extra_cache()");
  DBUG_PRINT("enter", ("cachesize %u", cachesize));

  m_extra_cache= TRUE;
  m_extra_cache_size= cachesize;
  if (m_part_spec.start_part != NO_CURRENT_PART_ID)
  {
    DBUG_ASSERT(bitmap_is_set(&m_partitions_to_reset,
                              m_part_spec.start_part));
    bitmap_set_bit(&m_partitions_to_reset, m_part_spec.start_part);
    late_extra_cache(m_part_spec.start_part);
  }
  DBUG_VOID_RETURN;
}


/**
  Prepares our new and reorged handlers for rename or delete.

  @param operation Operation to forward

  @return Operation status
    @retval 0  Success
    @retval !0 Error
*/

int ha_partition::loop_extra_alter(enum ha_extra_function operation)
{
  int result= 0, tmp;
  handler **file;
  DBUG_ENTER("ha_partition::loop_extra_alter()");
  DBUG_ASSERT(operation == HA_EXTRA_PREPARE_FOR_RENAME ||
              operation == HA_EXTRA_FORCE_REOPEN);

  if (m_new_file != NULL)
  {
    for (file= m_new_file; *file; file++)
      if ((tmp= (*file)->extra(operation)))
        result= tmp;
  }
  if (m_reorged_file != NULL)
  {
    for (file= m_reorged_file; *file; file++)
      if ((tmp= (*file)->extra(operation)))
        result= tmp;
  }
  if ((tmp= loop_extra(operation)))
    result= tmp;
  DBUG_RETURN(result);
}

/*
  Call extra on all partitions

  SYNOPSIS
    loop_extra()
    operation             extra operation type

  RETURN VALUE
    >0                    Error code
    0                     Success
*/

int ha_partition::loop_extra(enum ha_extra_function operation)
{
  int result= 0, tmp;
  uint i;
  DBUG_ENTER("ha_partition::loop_extra()");

  for (i= bitmap_get_first_set(&m_part_info->lock_partitions);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->lock_partitions, i))
  {
    /*
      This can be called after an error in ha_open.
      In this case calling 'extra' can crash.
    */
    if (bitmap_is_set(&m_opened_partitions, i) &&
        (tmp= m_file[i]->extra(operation)))
      result= tmp;
  }
  /* Add all used partitions to be called in reset(). */
  bitmap_union(&m_partitions_to_reset, &m_part_info->lock_partitions);
  DBUG_RETURN(result);
}


/*
  Call extra(HA_EXTRA_CACHE) on next partition_id

  SYNOPSIS
    late_extra_cache()
    partition_id               Partition id to call extra on

  RETURN VALUE
    NONE
*/

void ha_partition::late_extra_cache(uint partition_id)
{
  handler *file;
  DBUG_ENTER("ha_partition::late_extra_cache");
  DBUG_PRINT("enter", ("extra_cache %u prepare %u partid %u size %u",
                       m_extra_cache, m_extra_prepare_for_update,
                       partition_id, m_extra_cache_size));

  if (!m_extra_cache && !m_extra_prepare_for_update)
    DBUG_VOID_RETURN;
  file= m_file[partition_id];
  if (m_extra_cache)
  {
    if (m_extra_cache_size == 0)
      (void) file->extra(HA_EXTRA_CACHE);
    else
      (void) file->extra_opt(HA_EXTRA_CACHE, m_extra_cache_size);
  }
  if (m_extra_prepare_for_update)
  {
    DBUG_ASSERT(m_extra_cache);
    (void) file->extra(HA_EXTRA_PREPARE_FOR_UPDATE);
  }
  m_extra_cache_part_id= partition_id;
  DBUG_VOID_RETURN;
}


/*
  Call extra(HA_EXTRA_NO_CACHE) on next partition_id

  SYNOPSIS
    late_extra_no_cache()
    partition_id               Partition id to call extra on

  RETURN VALUE
    NONE
*/

void ha_partition::late_extra_no_cache(uint partition_id)
{
  handler *file;
  DBUG_ENTER("ha_partition::late_extra_no_cache");

  if (!m_extra_cache && !m_extra_prepare_for_update)
    DBUG_VOID_RETURN;
  file= m_file[partition_id];
  (void) file->extra(HA_EXTRA_NO_CACHE);
  DBUG_ASSERT(partition_id == m_extra_cache_part_id);
  m_extra_cache_part_id= NO_CURRENT_PART_ID;
  DBUG_VOID_RETURN;
}


/****************************************************************************
                MODULE optimiser support
****************************************************************************/

/**
  Get keys to use for scanning.

  @return key_map of keys usable for scanning

  @note No need to use read_partitions here, since it does not depend on
  which partitions is used, only which storage engine used.
*/

const key_map *ha_partition::keys_to_use_for_scanning()
{
  DBUG_ENTER("ha_partition::keys_to_use_for_scanning");
  DBUG_RETURN(get_open_file_sample()->keys_to_use_for_scanning());
}


/**
  Minimum number of rows to base optimizer estimate on.
*/

ha_rows ha_partition::min_rows_for_estimate()
{
  uint i, max_used_partitions, tot_used_partitions;
  DBUG_ENTER("ha_partition::min_rows_for_estimate");

  tot_used_partitions= bitmap_bits_set(&m_part_info->read_partitions);

  /*
    All partitions might have been left as unused during partition pruning
    due to, for example, an impossible WHERE condition. Nonetheless, the
    optimizer might still attempt to perform (e.g. range) analysis where an
    estimate of the the number of rows is calculated using records_in_range.
    Hence, to handle this and other possible cases, use zero as the minimum
    number of rows to base the estimate on if no partition is being used.
  */
  if (!tot_used_partitions)
    DBUG_RETURN(0);

  /*
    Allow O(log2(tot_partitions)) increase in number of used partitions.
    This gives O(tot_rows/log2(tot_partitions)) rows to base the estimate on.
    I.e when the total number of partitions doubles, allow one more
    partition to be checked.
  */
  i= 2;
  max_used_partitions= 1;
  while (i < m_tot_parts)
  {
    max_used_partitions++;
    i= i << 1;
  }
  if (max_used_partitions > tot_used_partitions)
    max_used_partitions= tot_used_partitions;

  /* stats.records is already updated by the info(HA_STATUS_VARIABLE) call. */
  DBUG_PRINT("info", ("max_used_partitions: %u tot_rows: %lu",
                      max_used_partitions,
                      (ulong) stats.records));
  DBUG_PRINT("info", ("tot_used_partitions: %u min_rows_to_check: %lu",
                      tot_used_partitions,
                      (ulong) stats.records * max_used_partitions
                              / tot_used_partitions));
  DBUG_RETURN(stats.records * max_used_partitions / tot_used_partitions);
}


/**
  Get the biggest used partition.

  Starting at the N:th biggest partition and skips all non used
  partitions, returning the biggest used partition found

  @param[in,out] part_index  Skip the *part_index biggest partitions

  @return The biggest used partition with index not lower than *part_index.
    @retval NO_CURRENT_PART_ID     No more partition used.
    @retval != NO_CURRENT_PART_ID  partition id of biggest used partition with
                                   index >= *part_index supplied. Note that
                                   *part_index will be updated to the next
                                   partition index to use.
*/

uint ha_partition::get_biggest_used_partition(uint *part_index)
{
  uint part_id;
  while ((*part_index) < m_tot_parts)
  {
    part_id= m_part_ids_sorted_by_num_of_records[(*part_index)++];
    if (bitmap_is_set(&m_part_info->read_partitions, part_id))
      return part_id;
  }
  return NO_CURRENT_PART_ID;
}


/*
  Return time for a scan of the table

  SYNOPSIS
    scan_time()

  RETURN VALUE
    time for scan
*/

double ha_partition::scan_time()
{
  double scan_time= 0;
  uint i;
  DBUG_ENTER("ha_partition::scan_time");

  for (i= bitmap_get_first_set(&m_part_info->read_partitions);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
    scan_time+= m_file[i]->scan_time();
  DBUG_RETURN(scan_time);
}


/**
  Find number of records in a range.
  @param inx      Index number
  @param min_key  Start of range
  @param max_key  End of range

  @return Number of rows in range.

  Given a starting key, and an ending key estimate the number of rows that
  will exist between the two. max_key may be empty which in case determine
  if start_key matches any rows.
*/

ha_rows ha_partition::records_in_range(uint inx, key_range *min_key,
				       key_range *max_key)
{
  ha_rows min_rows_to_check, rows, estimated_rows=0, checked_rows= 0;
  uint partition_index= 0, part_id;
  DBUG_ENTER("ha_partition::records_in_range");

  min_rows_to_check= min_rows_for_estimate();

  while ((part_id= get_biggest_used_partition(&partition_index))
         != NO_CURRENT_PART_ID)
  {
    rows= m_file[part_id]->records_in_range(inx, min_key, max_key);

    DBUG_PRINT("info", ("part %u match %lu rows of %lu", part_id, (ulong) rows,
                        (ulong) m_file[part_id]->stats.records));

    if (rows == HA_POS_ERROR)
      DBUG_RETURN(HA_POS_ERROR);
    estimated_rows+= rows;
    checked_rows+= m_file[part_id]->stats.records;
    /*
      Returning 0 means no rows can be found, so we must continue
      this loop as long as we have estimated_rows == 0.
      Also many engines return 1 to indicate that there may exist
      a matching row, we do not normalize this by dividing by number of
      used partitions, but leave it to be returned as a sum, which will
      reflect that we will need to scan each partition's index.

      Note that this statistics may not always be correct, so we must
      continue even if the current partition has 0 rows, since we might have
      deleted rows from the current partition, or inserted to the next
      partition.
    */
    if (estimated_rows && checked_rows &&
        checked_rows >= min_rows_to_check)
    {
      DBUG_PRINT("info",
                 ("records_in_range(inx %u): %lu (%lu * %lu / %lu)",
                  inx,
                  (ulong) (estimated_rows * stats.records / checked_rows),
                  (ulong) estimated_rows,
                  (ulong) stats.records,
                  (ulong) checked_rows));
      DBUG_RETURN(estimated_rows * stats.records / checked_rows);
    }
  }
  DBUG_PRINT("info", ("records_in_range(inx %u): %lu",
                      inx,
                      (ulong) estimated_rows));
  DBUG_RETURN(estimated_rows);
}


/**
  Estimate upper bound of number of rows.

  @return Number of rows.
*/

ha_rows ha_partition::estimate_rows_upper_bound()
{
  ha_rows rows, tot_rows= 0;
  handler **file= m_file;
  DBUG_ENTER("ha_partition::estimate_rows_upper_bound");

  do
  {
    if (bitmap_is_set(&(m_part_info->read_partitions), (uint)(file - m_file)))
    {
      rows= (*file)->estimate_rows_upper_bound();
      if (rows == HA_POS_ERROR)
        DBUG_RETURN(HA_POS_ERROR);
      tot_rows+= rows;
    }
  } while (*(++file));
  DBUG_RETURN(tot_rows);
}


/*
  Get time to read

  SYNOPSIS
    read_time()
    index                Index number used
    ranges               Number of ranges
    rows                 Number of rows

  RETURN VALUE
    time for read

  DESCRIPTION
    This will be optimised later to include whether or not the index can
    be used with partitioning. To achieve we need to add another parameter
    that specifies how many of the index fields that are bound in the ranges.
    Possibly added as a new call to handlers.
*/

double ha_partition::read_time(uint index, uint ranges, ha_rows rows)
{
  DBUG_ENTER("ha_partition::read_time");

  DBUG_RETURN(get_open_file_sample()->read_time(index, ranges, rows));
}


/**
  Number of rows in table. see handler.h

  @return Number of records in the table (after pruning!)
*/

ha_rows ha_partition::records()
{
  int error;
  ha_rows tot_rows= 0;
  uint i;
  DBUG_ENTER("ha_partition::records");

  for (i= bitmap_get_first_set(&m_part_info->read_partitions);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->read_partitions, i))
  {
    ha_rows rows;
    if (unlikely((error= m_file[i]->pre_records()) ||
                 (rows= m_file[i]->records()) == HA_POS_ERROR))
      DBUG_RETURN(HA_POS_ERROR);
    tot_rows+= rows;
  }
  DBUG_PRINT("exit", ("records: %lld", (longlong) tot_rows));
  DBUG_RETURN(tot_rows);
}


/*
  Is it ok to switch to a new engine for this table

  SYNOPSIS
    can_switch_engine()

  RETURN VALUE
    TRUE                  Ok
    FALSE                 Not ok

  DESCRIPTION
    Used to ensure that tables with foreign key constraints are not moved
    to engines without foreign key support.
*/

bool ha_partition::can_switch_engines()
{
  handler **file;
  DBUG_ENTER("ha_partition::can_switch_engines");

  file= m_file;
  do
  {
    if (!(*file)->can_switch_engines())
      DBUG_RETURN(FALSE);
  } while (*(++file));
  DBUG_RETURN(TRUE);
}


/*
  Is table cache supported

  SYNOPSIS
    table_cache_type()

*/

uint8 ha_partition::table_cache_type()
{
  DBUG_ENTER("ha_partition::table_cache_type");

  DBUG_RETURN(m_file[0]->table_cache_type());
}


/**
  Calculate hash value for KEY partitioning using an array of fields.

  @param field_array   An array of the fields in KEY partitioning

  @return hash_value calculated

  @note Uses the hash function on the character set of the field.
  Integer and floating point fields use the binary character set by default.
*/

uint32 ha_partition::calculate_key_hash_value(Field **field_array)
{
  ulong nr1= 1;
  ulong nr2= 4;
  bool use_51_hash;
  use_51_hash= MY_TEST((*field_array)->table->part_info->key_algorithm ==
                       partition_info::KEY_ALGORITHM_51);

  do
  {
    Field *field= *field_array;
    if (use_51_hash)
    {
      switch (field->real_type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_NEWDATE:
        {
          if (field->is_null())
          {
            nr1^= (nr1 << 1) | 1;
            continue;
          }
          /* Force this to my_hash_sort_bin, which was used in 5.1! */
          uint len= field->pack_length();
          my_charset_bin.coll->hash_sort(&my_charset_bin, field->ptr, len,
                                         &nr1, &nr2);
          /* Done with this field, continue with next one. */
          continue;
        }
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_BIT:
        /* Not affected, same in 5.1 and 5.5 */
        break;
      /*
        ENUM/SET uses my_hash_sort_simple in 5.1 (i.e. my_charset_latin1)
        and my_hash_sort_bin in 5.5!
      */
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
        {
          if (field->is_null())
          {
            nr1^= (nr1 << 1) | 1;
            continue;
          }
          /* Force this to my_hash_sort_bin, which was used in 5.1! */
          uint len= field->pack_length();
          my_charset_latin1.coll->hash_sort(&my_charset_latin1, field->ptr,
                                            len, &nr1, &nr2);
          continue;
        }
      /* New types in mysql-5.6. */
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIME2:
      case MYSQL_TYPE_TIMESTAMP2:
        /* Not affected, 5.6+ only! */
        break;

      /* These types should not be allowed for partitioning! */
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_GEOMETRY:
        /* fall through */
      default:
        DBUG_ASSERT(0);                    // New type?
        /* Fall through for default hashing (5.5). */
      }
      /* fall through, use collation based hashing. */
    }
    field->hash(&nr1, &nr2);
  } while (*(++field_array));
  return (uint32) nr1;
}


/****************************************************************************
                MODULE print messages
****************************************************************************/

const char *ha_partition::index_type(uint inx)
{
  uint first_used_partition;
  DBUG_ENTER("ha_partition::index_type");

  first_used_partition= bitmap_get_first_set(&(m_part_info->read_partitions));

  if (first_used_partition == MY_BIT_NONE)
  {
    DBUG_ASSERT(0);                             // How can this happen?
    DBUG_RETURN(handler::index_type(inx));
  }

  DBUG_RETURN(m_file[first_used_partition]->index_type(inx));
}


enum row_type ha_partition::get_row_type() const
{
  uint i;
  enum row_type type;
  DBUG_ENTER("ha_partition::get_row_type");

  i= bitmap_get_first_set(&m_part_info->read_partitions);
  DBUG_ASSERT(i < m_tot_parts);
  if (i >= m_tot_parts)
    DBUG_RETURN(ROW_TYPE_NOT_USED);

  type= m_file[i]->get_row_type();
  DBUG_PRINT("info", ("partition %u, row_type: %d", i, type));

  for (i= bitmap_get_next_set(&m_part_info->lock_partitions, i);
       i < m_tot_parts;
       i= bitmap_get_next_set(&m_part_info->lock_partitions, i))
  {
    enum row_type part_type= m_file[i]->get_row_type();
    DBUG_PRINT("info", ("partition %u, row_type: %d", i, type));
    if (part_type != type)
      DBUG_RETURN(ROW_TYPE_NOT_USED);
  }

  DBUG_RETURN(type);
}


void ha_partition::append_row_to_str(String &str)
{
  const uchar *rec;
  bool is_rec0= !m_err_rec || m_err_rec == table->record[0];
  if (is_rec0)
    rec= table->record[0];
  else
    rec= m_err_rec;
  // If PK, use full PK instead of full part field array!
  if (table->s->primary_key != MAX_KEY)
  {
    KEY *key= table->key_info + table->s->primary_key;
    KEY_PART_INFO *key_part=     key->key_part;
    KEY_PART_INFO *key_part_end= key_part + key->user_defined_key_parts;
    if (!is_rec0)
      set_key_field_ptr(key, rec, table->record[0]);
    for (; key_part != key_part_end; key_part++)
    {
      Field *field= key_part->field;
      str.append(" ");
      str.append(&field->field_name);
      str.append(":");
      field_unpack(&str, field, rec, 0, false);
    }
    if (!is_rec0)
      set_key_field_ptr(key, table->record[0], rec);
  }
  else
  {
    Field **field_ptr;
    if (!is_rec0)
      table->move_fields(m_part_info->full_part_field_array, rec,
                    table->record[0]);
    /* No primary key, use full partition field array. */
    for (field_ptr= m_part_info->full_part_field_array;
         *field_ptr;
         field_ptr++)
    {
      Field *field= *field_ptr;
      str.append(" ");
      str.append(&field->field_name);
      str.append(":");
      field_unpack(&str, field, rec, 0, false);
    }
    if (!is_rec0)
      table->move_fields(m_part_info->full_part_field_array, table->record[0],
                    rec);
  }
}


void ha_partition::print_error(int error, myf errflag)
{
  THD *thd= ha_thd();
  DBUG_ENTER("ha_partition::print_error");
  DBUG_PRINT("enter", ("error: %d", error));

  /* Should probably look for my own errors first */
  if ((error == HA_ERR_NO_PARTITION_FOUND) &&
      ! (thd->lex->alter_info.partition_flags & ALTER_PARTITION_TRUNCATE))
  {
    m_part_info->print_no_partition_found(table, errflag);
    DBUG_VOID_RETURN;
  }
  else if (error == HA_ERR_ROW_IN_WRONG_PARTITION)
  {
    /* Should only happen on DELETE or UPDATE! */
    DBUG_ASSERT(thd_sql_command(thd) == SQLCOM_DELETE ||
                thd_sql_command(thd) == SQLCOM_DELETE_MULTI ||
                thd_sql_command(thd) == SQLCOM_UPDATE ||
                thd_sql_command(thd) == SQLCOM_UPDATE_MULTI);
    DBUG_ASSERT(m_err_rec);
    if (m_err_rec)
    {
      uint max_length;
      char buf[MAX_KEY_LENGTH];
      String str(buf,sizeof(buf),system_charset_info);
      uint32 part_id;
      str.length(0);
      str.append("(");
      str.append_ulonglong(m_last_part);
      str.append(" != ");
      if (get_part_for_buf(m_err_rec, m_rec0, m_part_info, &part_id))
        str.append("?");
      else
        str.append_ulonglong(part_id);
      str.append(")");
      append_row_to_str(str);

      /* Log this error, so the DBA can notice it and fix it! */
      sql_print_error("Table '%-192s' corrupted: row in wrong partition: %s\n"
                      "Please REPAIR the table!",
                      table->s->table_name.str,
                      str.c_ptr_safe());

      max_length= (MYSQL_ERRMSG_SIZE -
                   (uint) strlen(ER_THD(thd, ER_ROW_IN_WRONG_PARTITION)));
      if (str.length() >= max_length)
      {
        str.length(max_length-4);
        str.append(STRING_WITH_LEN("..."));
      }
      my_error(ER_ROW_IN_WRONG_PARTITION, MYF(0), str.c_ptr_safe());
      m_err_rec= NULL;
      DBUG_VOID_RETURN;
    }
    /* fall through to generic error handling. */
  }

  /* In case m_file has not been initialized, like in bug#42438 */
  if (m_file)
  {
    if (m_last_part >= m_tot_parts)
    {
      DBUG_ASSERT(0);
      m_last_part= 0;
    }
    m_file[m_last_part]->print_error(error, errflag);
  }
  else
    handler::print_error(error, errflag);
  DBUG_VOID_RETURN;
}


bool ha_partition::get_error_message(int error, String *buf)
{
  DBUG_ENTER("ha_partition::get_error_message");

  /* Should probably look for my own errors first */

  /* In case m_file has not been initialized, like in bug#42438 */
  if (m_file)
    DBUG_RETURN(m_file[m_last_part]->get_error_message(error, buf));
  DBUG_RETURN(handler::get_error_message(error, buf));

}


/****************************************************************************
                MODULE in-place ALTER
****************************************************************************/
/**
  Get table flags.
*/

handler::Table_flags ha_partition::table_flags() const
{
  uint first_used_partition= 0;
  DBUG_ENTER("ha_partition::table_flags");
  if (m_handler_status < handler_initialized ||
      m_handler_status >= handler_closed)
    DBUG_RETURN(PARTITION_ENABLED_TABLE_FLAGS);

  if (get_lock_type() != F_UNLCK)
  {
    /*
      The flags are cached after external_lock, and may depend on isolation
      level. So we should use a locked partition to get the correct flags.
    */
    first_used_partition= bitmap_get_first_set(&m_part_info->lock_partitions);
    if (first_used_partition == MY_BIT_NONE)
      first_used_partition= 0;
  }
  DBUG_RETURN((m_file[first_used_partition]->ha_table_flags() &
                 ~(PARTITION_DISABLED_TABLE_FLAGS)) |
                 (PARTITION_ENABLED_TABLE_FLAGS));
}


/**
  alter_table_flags must be on handler/table level, not on hton level
  due to the ha_partition hton does not know what the underlying hton is.
*/

alter_table_operations ha_partition::alter_table_flags(alter_table_operations flags)
{
  alter_table_operations flags_to_return;
  DBUG_ENTER("ha_partition::alter_table_flags");

  flags_to_return= ht->alter_table_flags(flags);
  flags_to_return|= m_file[0]->alter_table_flags(flags);

  DBUG_RETURN(flags_to_return);
}


/**
  check if copy of data is needed in alter table.
*/
bool ha_partition::check_if_incompatible_data(HA_CREATE_INFO *create_info,
                                              uint table_changes)
{
  /*
    The check for any partitioning related changes have already been done
    in mysql_alter_table (by fix_partition_func), so it is only up to
    the underlying handlers.
  */
  List_iterator<partition_element> part_it(m_part_info->partitions);
  HA_CREATE_INFO dummy_info= *create_info;
  uint i=0;
  while (partition_element *part_elem= part_it++)
  {
    if (m_is_sub_partitioned)
    {
      List_iterator<partition_element> subpart_it(part_elem->subpartitions);
      while (partition_element *sub_elem= subpart_it++)
      {
        dummy_info.data_file_name= sub_elem->data_file_name;
        dummy_info.index_file_name= sub_elem->index_file_name;
        if (m_file[i++]->check_if_incompatible_data(&dummy_info, table_changes))
          return COMPATIBLE_DATA_NO;
      }
    }
    else
    {
      dummy_info.data_file_name= part_elem->data_file_name;
      dummy_info.index_file_name= part_elem->index_file_name;
      if (m_file[i++]->check_if_incompatible_data(&dummy_info, table_changes))
        return COMPATIBLE_DATA_NO;
    }
  }
  return COMPATIBLE_DATA_YES;
}


/**
  Support of in-place alter table.
*/

/**
  Helper class for in-place alter, see handler.h
*/

class ha_partition_inplace_ctx : public inplace_alter_handler_ctx
{
public:
  inplace_alter_handler_ctx **handler_ctx_array;
private:
  uint m_tot_parts;

public:
  ha_partition_inplace_ctx(THD *thd, uint tot_parts)
    : inplace_alter_handler_ctx(),
      handler_ctx_array(NULL),
      m_tot_parts(tot_parts)
  {}

  ~ha_partition_inplace_ctx()
  {
    if (handler_ctx_array)
    {
      for (uint index= 0; index < m_tot_parts; index++)
        delete handler_ctx_array[index];
    }
  }
};


enum_alter_inplace_result
ha_partition::check_if_supported_inplace_alter(TABLE *altered_table,
                                               Alter_inplace_info *ha_alter_info)
{
  uint index= 0;
  enum_alter_inplace_result result= HA_ALTER_INPLACE_NO_LOCK;
  ha_partition_inplace_ctx *part_inplace_ctx;
  bool first_is_set= false;
  THD *thd= ha_thd();

  DBUG_ENTER("ha_partition::check_if_supported_inplace_alter");
  /*
    Support inplace change of KEY () -> KEY ALGORITHM = N ().
    Any other change would set partition_changed in
    prep_alter_part_table() in mysql_alter_table().
  */
  if (ha_alter_info->alter_info->partition_flags == ALTER_PARTITION_INFO)
  {
    DBUG_ASSERT(ha_alter_info->alter_info->flags == 0);
    DBUG_RETURN(HA_ALTER_INPLACE_NO_LOCK);
  }

  part_inplace_ctx=
    new (thd->mem_root) ha_partition_inplace_ctx(thd, m_tot_parts);
  if (!part_inplace_ctx)
    DBUG_RETURN(HA_ALTER_ERROR);

  part_inplace_ctx->handler_ctx_array= (inplace_alter_handler_ctx **)
    thd->alloc(sizeof(inplace_alter_handler_ctx *) * (m_tot_parts + 1));
  if (!part_inplace_ctx->handler_ctx_array)
    DBUG_RETURN(HA_ALTER_ERROR);

  /* Set all to NULL, including the terminating one. */
  for (index= 0; index <= m_tot_parts; index++)
    part_inplace_ctx->handler_ctx_array[index]= NULL;

  ha_alter_info->handler_flags |= ALTER_PARTITIONED;
  for (index= 0; index < m_tot_parts; index++)
  {
    enum_alter_inplace_result p_result=
      m_file[index]->check_if_supported_inplace_alter(altered_table,
                                                      ha_alter_info);
    part_inplace_ctx->handler_ctx_array[index]= ha_alter_info->handler_ctx;

    if (index == 0)
    {
      first_is_set= (ha_alter_info->handler_ctx != NULL);
    }
    else if (first_is_set != (ha_alter_info->handler_ctx != NULL))
    {
      /* Either none or all partitions must set handler_ctx! */
      DBUG_ASSERT(0);
      DBUG_RETURN(HA_ALTER_ERROR);
    }
    if (p_result < result)
      result= p_result;
    if (result == HA_ALTER_ERROR)
      break;
  }

  ha_alter_info->handler_ctx= part_inplace_ctx;
  /*
    To indicate for future inplace calls that there are several
    partitions/handlers that need to be committed together,
    we set group_commit_ctx to the NULL terminated array of
    the partitions handlers.
  */
  ha_alter_info->group_commit_ctx= part_inplace_ctx->handler_ctx_array;

  DBUG_RETURN(result);
}


bool ha_partition::prepare_inplace_alter_table(TABLE *altered_table,
                                               Alter_inplace_info *ha_alter_info)
{
  uint index= 0;
  bool error= false;
  ha_partition_inplace_ctx *part_inplace_ctx;

  DBUG_ENTER("ha_partition::prepare_inplace_alter_table");

  /*
    Changing to similar partitioning, only update metadata.
    Non allowed changes would be catched in prep_alter_part_table().
  */
  if (ha_alter_info->alter_info->partition_flags == ALTER_PARTITION_INFO)
  {
    DBUG_ASSERT(ha_alter_info->alter_info->flags == 0);
    DBUG_RETURN(false);
  }

  part_inplace_ctx=
    static_cast<class ha_partition_inplace_ctx*>(ha_alter_info->handler_ctx);

  for (index= 0; index < m_tot_parts && !error; index++)
  {
    ha_alter_info->handler_ctx= part_inplace_ctx->handler_ctx_array[index];
    if (m_file[index]->ha_prepare_inplace_alter_table(altered_table,
                                                      ha_alter_info))
      error= true;
    part_inplace_ctx->handler_ctx_array[index]= ha_alter_info->handler_ctx;
  }
  ha_alter_info->handler_ctx= part_inplace_ctx;

  DBUG_RETURN(error);
}


bool ha_partition::inplace_alter_table(TABLE *altered_table,
                                       Alter_inplace_info *ha_alter_info)
{
  uint index= 0;
  bool error= false;
  ha_partition_inplace_ctx *part_inplace_ctx;

  DBUG_ENTER("ha_partition::inplace_alter_table");

  /*
    Changing to similar partitioning, only update metadata.
    Non allowed changes would be catched in prep_alter_part_table().
  */
  if (ha_alter_info->alter_info->partition_flags == ALTER_PARTITION_INFO)
  {
    DBUG_ASSERT(ha_alter_info->alter_info->flags == 0);
    DBUG_RETURN(false);
  }

  part_inplace_ctx=
    static_cast<class ha_partition_inplace_ctx*>(ha_alter_info->handler_ctx);

  for (index= 0; index < m_tot_parts && !error; index++)
  {
    ha_alter_info->handler_ctx= part_inplace_ctx->handler_ctx_array[index];
    if (m_file[index]->ha_inplace_alter_table(altered_table,
                                              ha_alter_info))
      error= true;
    part_inplace_ctx->handler_ctx_array[index]= ha_alter_info->handler_ctx;
  }
  ha_alter_info->handler_ctx= part_inplace_ctx;

  DBUG_RETURN(error);
}


/*
  Note that this function will try rollback failed ADD INDEX by
  executing DROP INDEX for the indexes that were committed (if any)
  before the error occurred. This means that the underlying storage
  engine must be able to drop index in-place with X-lock held.
  (As X-lock will be held here if new indexes are to be committed)
*/
bool ha_partition::commit_inplace_alter_table(TABLE *altered_table,
                                              Alter_inplace_info *ha_alter_info,
                                              bool commit)
{
  ha_partition_inplace_ctx *part_inplace_ctx;
  bool error= false;

  DBUG_ENTER("ha_partition::commit_inplace_alter_table");

  /*
    Changing to similar partitioning, only update metadata.
    Non allowed changes would be catched in prep_alter_part_table().
  */
  if (ha_alter_info->alter_info->partition_flags == ALTER_PARTITION_INFO)
  {
    DBUG_ASSERT(ha_alter_info->alter_info->flags == 0);
    DBUG_RETURN(false);
  }

  part_inplace_ctx=
    static_cast<class ha_partition_inplace_ctx*>(ha_alter_info->handler_ctx);

  if (commit)
  {
    DBUG_ASSERT(ha_alter_info->group_commit_ctx ==
                part_inplace_ctx->handler_ctx_array);
    ha_alter_info->handler_ctx= part_inplace_ctx->handler_ctx_array[0];
    error= m_file[0]->ha_commit_inplace_alter_table(altered_table,
                                                    ha_alter_info, commit);
    if (unlikely(error))
      goto end;
    if (ha_alter_info->group_commit_ctx)
    {
      /*
        If ha_alter_info->group_commit_ctx is not set to NULL,
        then the engine did only commit the first partition!
        The engine is probably new, since both innodb and the default
        implementation of handler::commit_inplace_alter_table sets it to NULL
        and simply return false, since it allows metadata changes only.
        Loop over all other partitions as to follow the protocol!
      */
      uint i;
      DBUG_ASSERT(0);
      for (i= 1; i < m_tot_parts; i++)
      {
        ha_alter_info->handler_ctx= part_inplace_ctx->handler_ctx_array[i];
        error|= m_file[i]->ha_commit_inplace_alter_table(altered_table,
                                                         ha_alter_info,
                                                         true);
      }
  }
    }
  else
  {
    uint i;
    for (i= 0; i < m_tot_parts; i++)
    {
      /* Rollback, commit == false,  is done for each partition! */
      ha_alter_info->handler_ctx= part_inplace_ctx->handler_ctx_array[i];
      if (m_file[i]->ha_commit_inplace_alter_table(altered_table,
                                                   ha_alter_info, false))
        error= true;
      }
    }
end:
  ha_alter_info->handler_ctx= part_inplace_ctx;

  DBUG_RETURN(error);
}


void ha_partition::notify_table_changed()
{
  handler **file;

  DBUG_ENTER("ha_partition::notify_table_changed");

  for (file= m_file; *file; file++)
    (*file)->ha_notify_table_changed();

  DBUG_VOID_RETURN;
}


uint ha_partition::min_of_the_max_uint(
                       uint (handler::*operator_func)(void) const) const
{
  handler **file;
  uint min_of_the_max= ((*m_file)->*operator_func)();

  for (file= m_file+1; *file; file++)
  {
    uint tmp= ((*file)->*operator_func)();
    set_if_smaller(min_of_the_max, tmp);
  }
  return min_of_the_max;
}


uint ha_partition::max_supported_key_parts() const
{
  return min_of_the_max_uint(&handler::max_supported_key_parts);
}


uint ha_partition::max_supported_key_length() const
{
  return min_of_the_max_uint(&handler::max_supported_key_length);
}


uint ha_partition::max_supported_key_part_length() const
{
  return min_of_the_max_uint(&handler::max_supported_key_part_length);
}


uint ha_partition::max_supported_record_length() const
{
  return min_of_the_max_uint(&handler::max_supported_record_length);
}


uint ha_partition::max_supported_keys() const
{
  return min_of_the_max_uint(&handler::max_supported_keys);
}


uint ha_partition::min_record_length(uint options) const
{
  handler **file;
  uint max= (*m_file)->min_record_length(options);

  for (file= m_file, file++; *file; file++)
    if (max < (*file)->min_record_length(options))
      max= (*file)->min_record_length(options);
  return max;
}

/****************************************************************************
                MODULE compare records
****************************************************************************/
/*
  Compare two positions

  SYNOPSIS
    cmp_ref()
    ref1                   First position
    ref2                   Second position

  RETURN VALUE
    <0                     ref1 < ref2
    0                      Equal
    >0                     ref1 > ref2

  DESCRIPTION
    We get two references and need to check if those records are the same.
    If they belong to different partitions we decide that they are not
    the same record. Otherwise we use the particular handler to decide if
    they are the same. Sort in partition id order if not equal.

  MariaDB note:
    Please don't merge the code from MySQL that does this:

    We get two references and need to check if those records are the same.
    If they belong to different partitions we decide that they are not
    the same record. Otherwise we use the particular handler to decide if
    they are the same. Sort in partition id order if not equal.

    It is incorrect, MariaDB has an alternative fix.
*/

int ha_partition::cmp_ref(const uchar *ref1, const uchar *ref2)
{
  int cmp;
  uint32 diff1, diff2;
  DBUG_ENTER("ha_partition::cmp_ref");

  cmp= get_open_file_sample()->cmp_ref((ref1 + PARTITION_BYTES_IN_POS),
                                       (ref2 + PARTITION_BYTES_IN_POS));
  if (cmp)
    DBUG_RETURN(cmp);

  diff2= uint2korr(ref2);
  diff1= uint2korr(ref1);

  if (diff1 == diff2)
  {
   /* This means that the references are same and are in same partition.*/
    DBUG_RETURN(0);
  }

  /*
    In Innodb we compare with either primary key value or global DB_ROW_ID so
    it is not possible that the two references are equal and are in different
    partitions, but in myisam it is possible since we are comparing offsets.
    Remove this assert if DB_ROW_ID is changed to be per partition.
  */
  DBUG_ASSERT(!m_innodb);
  DBUG_RETURN(diff2 > diff1 ? -1 : 1);
}


/****************************************************************************
                MODULE auto increment
****************************************************************************/


/**
   Retreive new values for part_share->next_auto_inc_val if needed

   This is needed if the value has not been initialized or if one of
   the underlying partitions require that the value should be re-calculated
*/

void ha_partition::update_next_auto_inc_val()
{
  if (!part_share->auto_inc_initialized ||
      need_info_for_auto_inc())
    info(HA_STATUS_AUTO);
}


/**
  Determine whether a partition needs auto-increment initialization.

  @return
    TRUE                    A  partition needs auto-increment initialization
    FALSE                   No partition needs auto-increment initialization

  Resets part_share->auto_inc_initialized if next auto_increment needs to be
  recalculated.
*/

bool ha_partition::need_info_for_auto_inc()
{
  handler **file= m_file;
  DBUG_ENTER("ha_partition::need_info_for_auto_inc");

  do
  {
    if ((*file)->need_info_for_auto_inc())
    {
      /* We have to get new auto_increment values from handler */
      part_share->auto_inc_initialized= FALSE;
      DBUG_RETURN(TRUE);
    }
  } while (*(++file));
  DBUG_RETURN(FALSE);
}


/**
  Determine if all partitions can use the current auto-increment value for
  auto-increment initialization.

  @return
    TRUE                    All partitions can use the current auto-increment
                            value for auto-increment initialization
    FALSE                   All partitions cannot use the current
                            auto-increment value for auto-increment
                            initialization

  Notes
    This function is only called for ::info(HA_STATUS_AUTO) and is
    mainly used by the Spider engine, which returns false
    except in the case of DROP TABLE or ALTER TABLE when it returns TRUE.
    Other engines always returns TRUE for this call.
*/

bool ha_partition::can_use_for_auto_inc_init()
{
  handler **file= m_file;
  DBUG_ENTER("ha_partition::can_use_for_auto_inc_init");

  do
  {
    if (!(*file)->can_use_for_auto_inc_init())
      DBUG_RETURN(FALSE);
  } while (*(++file));
  DBUG_RETURN(TRUE);
}


int ha_partition::reset_auto_increment(ulonglong value)
{
  handler **file= m_file;
  int res;
  DBUG_ENTER("ha_partition::reset_auto_increment");
  lock_auto_increment();
  part_share->auto_inc_initialized= false;
  part_share->next_auto_inc_val= 0;
  do
  {
    if ((res= (*file)->ha_reset_auto_increment(value)) != 0)
      break;
  } while (*(++file));
  unlock_auto_increment();
  DBUG_RETURN(res);
}


/**
  This method is called by update_auto_increment which in turn is called
  by the individual handlers as part of write_row. We use the
  part_share->next_auto_inc_val, or search all
  partitions for the highest auto_increment_value if not initialized or
  if auto_increment field is a secondary part of a key, we must search
  every partition when holding a mutex to be sure of correctness.
*/

void ha_partition::get_auto_increment(ulonglong offset, ulonglong increment,
                                      ulonglong nb_desired_values,
                                      ulonglong *first_value,
                                      ulonglong *nb_reserved_values)
{
  DBUG_ENTER("ha_partition::get_auto_increment");
  DBUG_PRINT("enter", ("offset: %lu  inc: %lu  desired_values: %lu  "
                       "first_value: %lu", (ulong) offset, (ulong) increment,
                      (ulong) nb_desired_values, (ulong) *first_value));
  DBUG_ASSERT(increment && nb_desired_values);
  *first_value= 0;
  if (table->s->next_number_keypart)
  {
    /*
      next_number_keypart is != 0 if the auto_increment column is a secondary
      column in the index (it is allowed in MyISAM)
    */
    DBUG_PRINT("info", ("next_number_keypart != 0"));
    ulonglong nb_reserved_values_part;
    ulonglong first_value_part, max_first_value;
    handler **file= m_file;
    first_value_part= max_first_value= *first_value;
    /* Must find highest value among all partitions. */
    do
    {
      /* Only nb_desired_values = 1 makes sense */
      (*file)->get_auto_increment(offset, increment, 1,
                                 &first_value_part, &nb_reserved_values_part);
      if (unlikely(first_value_part == ULONGLONG_MAX)) // error in one partition
      {
        *first_value= first_value_part;
        /* log that the error was between table/partition handler */
        sql_print_error("Partition failed to reserve auto_increment value");
        DBUG_VOID_RETURN;
      }
      DBUG_PRINT("info", ("first_value_part: %lu", (ulong) first_value_part));
      set_if_bigger(max_first_value, first_value_part);
    } while (*(++file));
    *first_value= max_first_value;
    *nb_reserved_values= 1;
  }
  else
  {
    THD *thd= ha_thd();
    /*
      This is initialized in the beginning of the first write_row call.
    */
    DBUG_ASSERT(part_share->auto_inc_initialized);
    /*
      Get a lock for handling the auto_increment in part_share
      for avoiding two concurrent statements getting the same number.
    */

    lock_auto_increment();

    /*
      In a multi-row insert statement like INSERT SELECT and LOAD DATA
      where the number of candidate rows to insert is not known in advance
      we must hold a lock/mutex for the whole statement if we have statement
      based replication. Because the statement-based binary log contains
      only the first generated value used by the statement, and slaves assumes
      all other generated values used by this statement were consecutive to
      this first one, we must exclusively lock the generator until the
      statement is done.
    */
    if (!auto_increment_safe_stmt_log_lock &&
        thd->lex->sql_command != SQLCOM_INSERT &&
        mysql_bin_log.is_open() &&
        !thd->is_current_stmt_binlog_format_row() &&
        (thd->variables.option_bits & OPTION_BIN_LOG))
    {
      DBUG_PRINT("info", ("locking auto_increment_safe_stmt_log_lock"));
      auto_increment_safe_stmt_log_lock= TRUE;
    }

    /* this gets corrected (for offset/increment) in update_auto_increment */
    *first_value= part_share->next_auto_inc_val;
    part_share->next_auto_inc_val+= nb_desired_values * increment;

    unlock_auto_increment();
    DBUG_PRINT("info", ("*first_value: %lu", (ulong) *first_value));
    *nb_reserved_values= nb_desired_values;
  }
  DBUG_VOID_RETURN;
}

void ha_partition::release_auto_increment()
{
  DBUG_ENTER("ha_partition::release_auto_increment");

  if (table->s->next_number_keypart)
  {
    uint i;
    for (i= bitmap_get_first_set(&m_part_info->lock_partitions);
         i < m_tot_parts;
         i= bitmap_get_next_set(&m_part_info->lock_partitions, i))
    {
      m_file[i]->ha_release_auto_increment();
    }
  }
  else if (next_insert_id)
  {
    ulonglong next_auto_inc_val;
    lock_auto_increment();
    next_auto_inc_val= part_share->next_auto_inc_val;
    /*
      If the current auto_increment values is lower than the reserved
      value, and the reserved value was reserved by this thread,
      we can lower the reserved value.
    */
    if (next_insert_id < next_auto_inc_val &&
        auto_inc_interval_for_cur_row.maximum() >= next_auto_inc_val)
    {
      THD *thd= ha_thd();
      /*
        Check that we do not lower the value because of a failed insert
        with SET INSERT_ID, i.e. forced/non generated values.
      */
      if (thd->auto_inc_intervals_forced.maximum() < next_insert_id)
        part_share->next_auto_inc_val= next_insert_id;
    }
    DBUG_PRINT("info", ("part_share->next_auto_inc_val: %lu",
                        (ulong) part_share->next_auto_inc_val));

    /* Unlock the multi row statement lock taken in get_auto_increment */
    if (auto_increment_safe_stmt_log_lock)
    {
      auto_increment_safe_stmt_log_lock= FALSE;
      DBUG_PRINT("info", ("unlocking auto_increment_safe_stmt_log_lock"));
    }

    unlock_auto_increment();
  }
  DBUG_VOID_RETURN;
}

/****************************************************************************
                MODULE initialize handler for HANDLER call
****************************************************************************/

void ha_partition::init_table_handle_for_HANDLER()
{
  return;
}


/**
  Return the checksum of the table (all partitions)
*/

uint ha_partition::checksum() const
{
  ha_checksum sum= 0;

  DBUG_ENTER("ha_partition::checksum");
  if ((table_flags() & (HA_HAS_OLD_CHECKSUM | HA_HAS_NEW_CHECKSUM)))
  {
    handler **file= m_file;
    do
    {
      sum+= (*file)->checksum();
    } while (*(++file));
  }
  DBUG_RETURN(sum);
}


/****************************************************************************
                MODULE enable/disable indexes
****************************************************************************/

/*
  Disable indexes for a while
  SYNOPSIS
    disable_indexes()
    mode                      Mode
  RETURN VALUES
    0                         Success
    != 0                      Error
*/

int ha_partition::disable_indexes(uint mode)
{
  handler **file;
  int error= 0;

  DBUG_ASSERT(bitmap_is_set_all(&(m_part_info->lock_partitions)));
  for (file= m_file; *file; file++)
  {
    if (unlikely((error= (*file)->ha_disable_indexes(mode))))
      break;
  }
  return error;
}


/*
  Enable indexes again
  SYNOPSIS
    enable_indexes()
    mode                      Mode
  RETURN VALUES
    0                         Success
    != 0                      Error
*/

int ha_partition::enable_indexes(uint mode)
{
  handler **file;
  int error= 0;

  DBUG_ASSERT(bitmap_is_set_all(&(m_part_info->lock_partitions)));
  for (file= m_file; *file; file++)
  {
    if (unlikely((error= (*file)->ha_enable_indexes(mode))))
      break;
  }
  return error;
}


/*
  Check if indexes are disabled
  SYNOPSIS
    indexes_are_disabled()

  RETURN VALUES
    0                      Indexes are enabled
    != 0                   Indexes are disabled
*/

int ha_partition::indexes_are_disabled(void)
{
  handler **file;
  int error= 0;

  DBUG_ASSERT(bitmap_is_set_all(&(m_part_info->lock_partitions)));
  for (file= m_file; *file; file++)
  {
    if (unlikely((error= (*file)->indexes_are_disabled())))
      break;
  }
  return error;
}


/**
  Check/fix misplaced rows.

  @param read_part_id  Partition to check/fix.
  @param repair        If true, move misplaced rows to correct partition.

  @return Operation status.
    @retval 0     Success
    @retval != 0  Error
*/

int ha_partition::check_misplaced_rows(uint read_part_id, bool do_repair)
{
  int result= 0;
  uint32 correct_part_id;
  longlong func_value;
  longlong num_misplaced_rows= 0;

  DBUG_ENTER("ha_partition::check_misplaced_rows");

  DBUG_ASSERT(m_file);

  if (do_repair)
  {
    /* We must read the full row, if we need to move it! */
    bitmap_set_all(table->read_set);
    bitmap_set_all(table->write_set);
  }
  else
  {
    /* Only need to read the partitioning fields. */
    bitmap_union(table->read_set, &m_part_info->full_part_field_set);
    if (table->vcol_set)
      bitmap_union(table->vcol_set, &m_part_info->full_part_field_set);
  }

  if ((result= m_file[read_part_id]->ha_rnd_init(1)))
    DBUG_RETURN(result);

  while (true)
  {
    if ((result= m_file[read_part_id]->ha_rnd_next(m_rec0)))
    {
      if (result != HA_ERR_END_OF_FILE)
        break;

      if (num_misplaced_rows > 0)
      {
	print_admin_msg(ha_thd(), MYSQL_ERRMSG_SIZE, "warning",
                        table_share->db.str, table->alias,
                        opt_op_name[REPAIR_PARTS],
                        "Moved %lld misplaced rows",
                        num_misplaced_rows);
      }
      /* End-of-file reached, all rows are now OK, reset result and break. */
      result= 0;
      break;
    }

    result= m_part_info->get_partition_id(m_part_info, &correct_part_id,
                                          &func_value);
    if (result)
      break;

    if (correct_part_id != read_part_id)
    {
      num_misplaced_rows++;
      if (!do_repair)
      {
        /* Check. */
	print_admin_msg(ha_thd(), MYSQL_ERRMSG_SIZE, "error",
                        table_share->db.str, table->alias,
                        opt_op_name[CHECK_PARTS],
                        "Found a misplaced row");
        /* Break on first misplaced row! */
        result= HA_ADMIN_NEEDS_UPGRADE;
        break;
      }
      else
      {
        DBUG_PRINT("info", ("Moving row from partition %u to %u",
                            (uint) read_part_id, (uint) correct_part_id));

        /*
          Insert row into correct partition. Notice that there are no commit
          for every N row, so the repair will be one large transaction!
        */
        if ((result= m_file[correct_part_id]->ha_write_row(m_rec0)))
        {
          /*
            We have failed to insert a row, it might have been a duplicate!
          */
          char buf[MAX_KEY_LENGTH];
          String str(buf,sizeof(buf),system_charset_info);
          str.length(0);
          if (result == HA_ERR_FOUND_DUPP_KEY)
          {
            str.append("Duplicate key found, "
                       "please update or delete the record:\n");
            result= HA_ADMIN_CORRUPT;
          }
          m_err_rec= NULL;
          append_row_to_str(str);

          /*
            If the engine supports transactions, the failure will be
            rollbacked.
          */
          if (!m_file[correct_part_id]->has_transactions())
          {
            /* Log this error, so the DBA can notice it and fix it! */
            sql_print_error("Table '%-192s' failed to move/insert a row"
                            " from part %u into part %u:\n%s",
                            table->s->table_name.str,
                            (uint) read_part_id,
                            (uint) correct_part_id,
                            str.c_ptr_safe());
          }
	  print_admin_msg(ha_thd(), MYSQL_ERRMSG_SIZE, "error",
                          table_share->db.str, table->alias,
                          opt_op_name[REPAIR_PARTS],
                          "Failed to move/insert a row"
                          " from part %u into part %u:\n%s",
                          (uint) read_part_id,
                          (uint) correct_part_id,
                          str.c_ptr_safe());
          break;
        }

        /* Delete row from wrong partition. */
        if ((result= m_file[read_part_id]->ha_delete_row(m_rec0)))
        {
          if (m_file[correct_part_id]->has_transactions())
            break;
          /*
            We have introduced a duplicate, since we failed to remove it
            from the wrong partition.
          */
          char buf[MAX_KEY_LENGTH];
          String str(buf,sizeof(buf),system_charset_info);
          str.length(0);
          m_err_rec= NULL;
          append_row_to_str(str);

          /* Log this error, so the DBA can notice it and fix it! */
          sql_print_error("Table '%-192s': Delete from part %u failed with"
                          " error %d. But it was already inserted into"
                          " part %u, when moving the misplaced row!"
                          "\nPlease manually fix the duplicate row:\n%s",
                          table->s->table_name.str,
                          (uint) read_part_id,
                          result,
                          (uint) correct_part_id,
                          str.c_ptr_safe());
          break;
        }
      }
    }
  }

  int tmp_result= m_file[read_part_id]->ha_rnd_end();
  DBUG_RETURN(result ? result : tmp_result);
}


#define KEY_PARTITIONING_CHANGED_STR \
  "KEY () partitioning changed, please run:\n" \
  "ALTER TABLE %s.%s ALGORITHM = INPLACE %s"

int ha_partition::check_for_upgrade(HA_CHECK_OPT *check_opt)
{
  int error= HA_ADMIN_NEEDS_CHECK;
  DBUG_ENTER("ha_partition::check_for_upgrade");

  /*
    This is called even without FOR UPGRADE,
    if the .frm version is lower than the current version.
    In that case return that it needs checking!
  */
  if (!(check_opt->sql_flags & TT_FOR_UPGRADE))
    DBUG_RETURN(error);

  /*
    Partitions will be checked for during their ha_check!

    Check if KEY (sub)partitioning was used and any field's hash calculation
    differs from 5.1, see bug#14521864.
  */
  if (table->s->mysql_version < 50503 &&              // 5.1 table (<5.5.3)
      ((m_part_info->part_type == HASH_PARTITION &&   // KEY partitioned
        m_part_info->list_of_part_fields) ||
       (m_is_sub_partitioned &&                       // KEY subpartitioned
        m_part_info->list_of_subpart_fields)))
  {
    Field **field;
    if (m_is_sub_partitioned)
    {
      field= m_part_info->subpart_field_array;
    }
    else
    {
      field= m_part_info->part_field_array;
    }
    for (; *field; field++)
    {
      switch ((*field)->real_type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
        {
          THD *thd= ha_thd();
          char *part_buf;
          String db_name, table_name;
          uint part_buf_len;
          bool skip_generation= false;
          partition_info::enum_key_algorithm old_algorithm;
          old_algorithm= m_part_info->key_algorithm;
          error= HA_ADMIN_FAILED;
          append_identifier(ha_thd(), &db_name, &table_share->db);
          append_identifier(ha_thd(), &table_name, &table_share->table_name);
          if (m_part_info->key_algorithm != partition_info::KEY_ALGORITHM_NONE)
          {
            /*
              Only possible when someone tampered with .frm files,
              like during tests :)
            */
            skip_generation= true;
          }
          m_part_info->key_algorithm= partition_info::KEY_ALGORITHM_51;
          if (skip_generation ||
              !(part_buf= generate_partition_syntax(thd, m_part_info,
                                                    &part_buf_len,
                                                    true,
                                                    NULL,
                                                    NULL)) ||
	      print_admin_msg(thd, SQL_ADMIN_MSG_TEXT_SIZE + 1, "error",
	                      table_share->db.str,
	                      table->alias,
                              opt_op_name[CHECK_PARTS],
                              KEY_PARTITIONING_CHANGED_STR,
                              db_name.c_ptr_safe(),
                              table_name.c_ptr_safe(),
                              part_buf))
	  {
	    /* Error creating admin message (too long string?). */
	    print_admin_msg(thd, MYSQL_ERRMSG_SIZE, "error",
                            table_share->db.str, table->alias,
                            opt_op_name[CHECK_PARTS],
                            KEY_PARTITIONING_CHANGED_STR,
                            db_name.c_ptr_safe(), table_name.c_ptr_safe(),
                            "<old partition clause>, but add ALGORITHM = 1"
                            " between 'KEY' and '(' to change the metadata"
                            " without the need of a full table rebuild.");
          }
          m_part_info->key_algorithm= old_algorithm;
          DBUG_RETURN(error);
        }
      default:
        /* Not affected! */
        ;
      }
    }
  }

  DBUG_RETURN(error);
}


TABLE_LIST *ha_partition::get_next_global_for_child()
{
  handler **file;
  DBUG_ENTER("ha_partition::get_next_global_for_child");
  for (file= m_file; *file; file++)
  {
    TABLE_LIST *table_list;
    if ((table_list= (*file)->get_next_global_for_child()))
      DBUG_RETURN(table_list);
  }
  DBUG_RETURN(0);
}


const COND *ha_partition::cond_push(const COND *cond)
{
  handler **file= m_file;
  COND *res_cond= NULL;
  DBUG_ENTER("ha_partition::cond_push");

  if (set_top_table_fields)
  {
    /*
      We want to do this in a separate loop to not come into a situation
      where we have only done cond_push() to some of the tables
    */
    do
    {
      if (((*file)->set_top_table_and_fields(top_table,
                                             top_table_field,
                                             top_table_fields)))
        DBUG_RETURN(cond);                      // Abort cond push, no error
    } while (*(++file));
    file= m_file;
  }

  do
  {
    if ((*file)->pushed_cond != cond)
    {
      if ((*file)->cond_push(cond))
        res_cond= (COND *) cond;
      else
        (*file)->pushed_cond= cond;
    }
  } while (*(++file));
  DBUG_RETURN(res_cond);
}


void ha_partition::cond_pop()
{
  handler **file= m_file;
  DBUG_ENTER("ha_partition::cond_push");

  do
  {
    (*file)->cond_pop();
  } while (*(++file));
  DBUG_VOID_RETURN;
}


/**
  Perform bulk update preparation on each partition.

  SYNOPSIS
    start_bulk_update()

  RETURN VALUE
    TRUE                      Error
    FALSE                     Success
*/

bool ha_partition::start_bulk_update()
{
  handler **file= m_file;
  DBUG_ENTER("ha_partition::start_bulk_update");

  if (bitmap_is_overlapping(&m_part_info->full_part_field_set,
                            table->write_set))
    DBUG_RETURN(TRUE);

  do
  {
    if ((*file)->start_bulk_update())
      DBUG_RETURN(TRUE);
  } while (*(++file));
  DBUG_RETURN(FALSE);
}


/**
  Perform bulk update execution on each partition.  A bulk update allows
  a handler to batch the updated rows instead of performing the updates
  one row at a time.

  SYNOPSIS
    exec_bulk_update()

  RETURN VALUE
    TRUE                      Error
    FALSE                     Success
*/

int ha_partition::exec_bulk_update(ha_rows *dup_key_found)
{
  int error;
  handler **file= m_file;
  DBUG_ENTER("ha_partition::exec_bulk_update");

  do
  {
    if (unlikely((error= (*file)->exec_bulk_update(dup_key_found))))
      DBUG_RETURN(error);
  } while (*(++file));
  DBUG_RETURN(0);
}


/**
  Perform bulk update cleanup on each partition.

  SYNOPSIS
    end_bulk_update()

  RETURN VALUE
    NONE
*/

int ha_partition::end_bulk_update()
{
  int error= 0;
  handler **file= m_file;
  DBUG_ENTER("ha_partition::end_bulk_update");

  do
  {
    int tmp;
    if ((tmp= (*file)->end_bulk_update()))
      error= tmp;
  } while (*(++file));
  DBUG_RETURN(error);
}


/**
  Add the row to the bulk update on the partition on which the row is stored.
  A bulk update allows a handler to batch the updated rows instead of
  performing the updates one row at a time.

  SYNOPSIS
    bulk_update_row()
    old_data                  Old record
    new_data                  New record
    dup_key_found             Number of duplicate keys found

  RETURN VALUE
    >1                        Error
    1                         Bulk update not used, normal operation used
    0                         Bulk update used by handler
*/

int ha_partition::bulk_update_row(const uchar *old_data, const uchar *new_data,
                                  ha_rows *dup_key_found)
{
  int error= 0;
  uint32 part_id;
  longlong func_value;
  my_bitmap_map *old_map;
  DBUG_ENTER("ha_partition::bulk_update_row");

  old_map= dbug_tmp_use_all_columns(table, table->read_set);
  error= m_part_info->get_partition_id(m_part_info, &part_id,
                                       &func_value);
  dbug_tmp_restore_column_map(table->read_set, old_map);
  if (unlikely(error))
  {
    m_part_info->err_value= func_value;
    goto end;
  }

  error= m_file[part_id]->ha_bulk_update_row(old_data, new_data,
                                             dup_key_found);

end:
  DBUG_RETURN(error);
}


/**
  Perform bulk delete preparation on each partition.

  SYNOPSIS
    start_bulk_delete()

  RETURN VALUE
    TRUE                      Error
    FALSE                     Success
*/

bool ha_partition::start_bulk_delete()
{
  handler **file= m_file;
  DBUG_ENTER("ha_partition::start_bulk_delete");

  do
  {
    if ((*file)->start_bulk_delete())
      DBUG_RETURN(TRUE);
  } while (*(++file));
  DBUG_RETURN(FALSE);
}


/**
  Perform bulk delete cleanup on each partition.

  SYNOPSIS
    end_bulk_delete()

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::end_bulk_delete()
{
  int error= 0;
  handler **file= m_file;
  DBUG_ENTER("ha_partition::end_bulk_delete");

  do
  {
    int tmp;
    if ((tmp= (*file)->end_bulk_delete()))
      error= tmp;
  } while (*(++file));
  DBUG_RETURN(error);
}


/**
  Perform initialization for a direct update request.

  SYNOPSIS
    direct_update_rows_init()
    update fields             Pointer to the list of fields to update

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::direct_update_rows_init(List<Item> *update_fields)
{
  int error;
  uint i, found;
  handler *file;
  DBUG_ENTER("ha_partition::direct_update_rows_init");

  if (bitmap_is_overlapping(&m_part_info->full_part_field_set,
                            table->write_set))
  {
    DBUG_PRINT("info", ("partition FALSE by updating part_key"));
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }

  m_part_spec.start_part= 0;
  m_part_spec.end_part= m_tot_parts - 1;
  m_direct_update_part_spec= m_part_spec;

  found= 0;
  for (i= m_part_spec.start_part; i <= m_part_spec.end_part; i++)
  {
    if (bitmap_is_set(&(m_part_info->read_partitions), i) &&
        bitmap_is_set(&(m_part_info->lock_partitions), i))
    {
      file= m_file[i];
      if (unlikely((error= (m_pre_calling ?
                            file->pre_direct_update_rows_init(update_fields) :
                            file->direct_update_rows_init(update_fields)))))
      {
        DBUG_PRINT("info", ("partition FALSE by storage engine"));
        DBUG_RETURN(error);
      }
      found++;
    }
  }

  TABLE_LIST *table_list= table->pos_in_table_list;
  if (found != 1 && table_list)
  {
    while (table_list->parent_l)
      table_list= table_list->parent_l;
    st_select_lex *select_lex= table_list->select_lex;
    DBUG_PRINT("info", ("partition select_lex: %p", select_lex));
    if (select_lex && select_lex->explicit_limit)
    {
      DBUG_PRINT("info", ("partition explicit_limit=TRUE"));
      DBUG_PRINT("info", ("partition offset_limit: %p",
                          select_lex->offset_limit));
      DBUG_PRINT("info", ("partition select_limit: %p",
                          select_lex->select_limit));
      DBUG_PRINT("info", ("partition FALSE by select_lex"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
  DBUG_PRINT("info", ("partition OK"));
  DBUG_RETURN(0);
}


/**
  Do initialization for performing parallel direct update
  for a handlersocket update request.

  SYNOPSIS
    pre_direct_update_rows_init()
    update fields             Pointer to the list of fields to update

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::pre_direct_update_rows_init(List<Item> *update_fields)
{
  bool save_m_pre_calling;
  int error;
  DBUG_ENTER("ha_partition::pre_direct_update_rows_init");
  save_m_pre_calling= m_pre_calling;
  m_pre_calling= TRUE;
  error= direct_update_rows_init(update_fields);
  m_pre_calling= save_m_pre_calling;
  DBUG_RETURN(error);
}


/**
  Execute a direct update request.  A direct update request updates all
  qualified rows in a single operation, rather than one row at a time.
  The direct update operation is pushed down to each individual
  partition.

  SYNOPSIS
    direct_update_rows()
    update_rows               Number of updated rows

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::direct_update_rows(ha_rows *update_rows_result)
{
  int error;
  bool rnd_seq= FALSE;
  ha_rows update_rows= 0;
  uint32 i;
  DBUG_ENTER("ha_partition::direct_update_rows");

  /* If first call to direct_update_rows with RND scan */
  if ((m_pre_calling ? pre_inited : inited) == RND && m_scan_value == 1)
  {
    rnd_seq= TRUE;
    m_scan_value= 2;
  }

  *update_rows_result= 0;
  for (i= m_part_spec.start_part; i <= m_part_spec.end_part; i++)
  {
    handler *file= m_file[i];
    if (bitmap_is_set(&(m_part_info->read_partitions), i) &&
        bitmap_is_set(&(m_part_info->lock_partitions), i))
    {
      if (rnd_seq && (m_pre_calling ? file->pre_inited : file->inited) == NONE)
      {
        if (unlikely((error= (m_pre_calling ?
                              file->ha_pre_rnd_init(TRUE) :
                              file->ha_rnd_init(TRUE)))))
          DBUG_RETURN(error);
      }
      if (unlikely((error= (m_pre_calling ?
                            (file)->pre_direct_update_rows() :
                            (file)->ha_direct_update_rows(&update_rows)))))
      {
        if (rnd_seq)
        {
          if (m_pre_calling)
            file->ha_pre_rnd_end();
          else
            file->ha_rnd_end();
        }
        DBUG_RETURN(error);
      }
      *update_rows_result+= update_rows;
    }
    if (rnd_seq)
    {
      if (unlikely((error= (m_pre_calling ?
                            file->ha_pre_index_or_rnd_end() :
                            file->ha_index_or_rnd_end()))))
        DBUG_RETURN(error);
    }
  }
  DBUG_RETURN(0);
}


/**
  Start parallel execution of a direct update for a handlersocket update
  request.  A direct update request updates all qualified rows in a single
  operation, rather than one row at a time.  The direct update operation
  is pushed down to each individual partition.

  SYNOPSIS
    pre_direct_update_rows()

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::pre_direct_update_rows()
{
  bool save_m_pre_calling;
  int error;
  ha_rows not_used= 0;
  DBUG_ENTER("ha_partition::pre_direct_update_rows");
  save_m_pre_calling= m_pre_calling;
  m_pre_calling= TRUE;
  error= direct_update_rows(&not_used);
  m_pre_calling= save_m_pre_calling;
  DBUG_RETURN(error);
}


/**
  Perform initialization for a direct delete request.

  SYNOPSIS
    direct_delete_rows_init()

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::direct_delete_rows_init()
{
  int error;
  uint i, found;
  DBUG_ENTER("ha_partition::direct_delete_rows_init");

  m_part_spec.start_part= 0;
  m_part_spec.end_part= m_tot_parts - 1;
  m_direct_update_part_spec= m_part_spec;

  found= 0;
  for (i= m_part_spec.start_part; i <= m_part_spec.end_part; i++)
  {
    if (bitmap_is_set(&(m_part_info->read_partitions), i) &&
        bitmap_is_set(&(m_part_info->lock_partitions), i))
    {
      handler *file= m_file[i];
      if (unlikely((error= (m_pre_calling ?
                            file->pre_direct_delete_rows_init() :
                            file->direct_delete_rows_init()))))
      {
        DBUG_PRINT("exit", ("error in direct_delete_rows_init"));
        DBUG_RETURN(error);
      }
      found++;
    }
  }

  TABLE_LIST *table_list= table->pos_in_table_list;
  if (found != 1 && table_list)
  {
    while (table_list->parent_l)
      table_list= table_list->parent_l;
    st_select_lex *select_lex= table_list->select_lex;
    DBUG_PRINT("info", ("partition select_lex: %p", select_lex));
    if (select_lex && select_lex->explicit_limit)
    {
      DBUG_PRINT("info", ("partition explicit_limit: TRUE"));
      DBUG_PRINT("info", ("partition offset_limit: %p",
                          select_lex->offset_limit));
      DBUG_PRINT("info", ("partition select_limit: %p",
                          select_lex->select_limit));
      DBUG_PRINT("info", ("partition FALSE by select_lex"));
      DBUG_RETURN(HA_ERR_WRONG_COMMAND);
    }
  }
  DBUG_PRINT("exit", ("OK"));
  DBUG_RETURN(0);
}


/**
  Do initialization for performing parallel direct delete
  for a handlersocket delete request.

  SYNOPSIS
    pre_direct_delete_rows_init()

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::pre_direct_delete_rows_init()
{
  bool save_m_pre_calling;
  int error;
  DBUG_ENTER("ha_partition::pre_direct_delete_rows_init");
  save_m_pre_calling= m_pre_calling;
  m_pre_calling= TRUE;
  error= direct_delete_rows_init();
  m_pre_calling= save_m_pre_calling;
  DBUG_RETURN(error);
}


/**
  Execute a direct delete request.  A direct delete request deletes all
  qualified rows in a single operation, rather than one row at a time.
  The direct delete operation is pushed down to each individual
  partition.

  SYNOPSIS
    direct_delete_rows()
    delete_rows               Number of deleted rows

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::direct_delete_rows(ha_rows *delete_rows_result)
{
  int error;
  bool rnd_seq= FALSE;
  ha_rows delete_rows= 0;
  uint32 i;
  handler *file;
  DBUG_ENTER("ha_partition::direct_delete_rows");

  if ((m_pre_calling ? pre_inited : inited) == RND && m_scan_value == 1)
  {
    rnd_seq= TRUE;
    m_scan_value= 2;
  }

  *delete_rows_result= 0;
  m_part_spec= m_direct_update_part_spec;
  for (i= m_part_spec.start_part; i <= m_part_spec.end_part; i++)
  {
    file= m_file[i];
    if (bitmap_is_set(&(m_part_info->read_partitions), i) &&
        bitmap_is_set(&(m_part_info->lock_partitions), i))
    {
      if (rnd_seq && (m_pre_calling ? file->pre_inited : file->inited) == NONE)
      {
        if (unlikely((error= (m_pre_calling ?
                              file->ha_pre_rnd_init(TRUE) :
                              file->ha_rnd_init(TRUE)))))
          DBUG_RETURN(error);
      }
      if ((error= (m_pre_calling ?
                   file->pre_direct_delete_rows() :
                   file->ha_direct_delete_rows(&delete_rows))))
      {
        if (m_pre_calling)
          file->ha_pre_rnd_end();
        else
          file->ha_rnd_end();
        DBUG_RETURN(error);
      }
      delete_rows_result+= delete_rows;
    }
    if (rnd_seq)
    {
      if (unlikely((error= (m_pre_calling ?
                            file->ha_pre_index_or_rnd_end() :
                            file->ha_index_or_rnd_end()))))
        DBUG_RETURN(error);
    }
  }
  DBUG_RETURN(0);
}


/**
  Start parallel execution of a direct delete for a handlersocket delete
  request.  A direct delete request deletes all qualified rows in a single
  operation, rather than one row at a time.  The direct delete operation
  is pushed down to each individual partition.

  SYNOPSIS
    pre_direct_delete_rows()

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::pre_direct_delete_rows()
{
  bool save_m_pre_calling;
  int error;
  ha_rows not_used;
  DBUG_ENTER("ha_partition::pre_direct_delete_rows");
  save_m_pre_calling= m_pre_calling;
  m_pre_calling= TRUE;
  error= direct_delete_rows(&not_used);
  m_pre_calling= save_m_pre_calling;
  DBUG_RETURN(error);
}

/**
  Push metadata for the current operation down to each partition.

  SYNOPSIS
    info_push()

  RETURN VALUE
    >0                        Error
    0                         Success
*/

int ha_partition::info_push(uint info_type, void *info)
{
  int error= 0;
  handler **file= m_file;
  DBUG_ENTER("ha_partition::info_push");

  do
  {
    int tmp;
    if ((tmp= (*file)->info_push(info_type, info)))
      error= tmp;
  } while (*(++file));
  DBUG_RETURN(error);
}


void ha_partition::clear_top_table_fields()
{
  handler **file;
  DBUG_ENTER("ha_partition::clear_top_table_fields");

  if (set_top_table_fields)
  {
    set_top_table_fields= FALSE;
    top_table= NULL;
    top_table_field= NULL;
    top_table_fields= 0;
    for (file= m_file; *file; file++)
      (*file)->clear_top_table_fields();
  }
  DBUG_VOID_RETURN;
}


struct st_mysql_storage_engine partition_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(partition)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &partition_storage_engine,
  "partition",
  "Mikael Ronstrom, MySQL AB",
  "Partition Storage Engine Helper",
  PLUGIN_LICENSE_GPL,
  partition_initialize, /* Plugin Init */
  NULL, /* Plugin Deinit */
  0x0100, /* 1.0 */
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  "1.0",                      /* string version                  */
  MariaDB_PLUGIN_MATURITY_STABLE /* maturity                     */
}
maria_declare_plugin_end;

#endif
