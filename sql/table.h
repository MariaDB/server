#ifndef TABLE_INCLUDED
#define TABLE_INCLUDED
/* Copyright (c) 2000, 2017, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB

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

#include "sql_plist.h"
#include "sql_list.h"                           /* Sql_alloc */
#include "mdl.h"
#include "datadict.h"
#include "sql_string.h"                         /* String */
#include "lex_string.h"

#ifndef MYSQL_CLIENT

#include "my_cpu.h"                             /* LF_BACKOFF() */
#include "hash.h"                               /* HASH */
#include "handler.h"                /* row_type, ha_choice, handler */
#include "mysql_com.h"              /* enum_field_types */
#include "thr_lock.h"                  /* thr_lock_type */
#include "filesort_utils.h"
#include "parse_file.h"
#include "sql_i_s.h"
#include "sql_type.h"               /* vers_kind_t */
#include "privilege.h"              /* privilege_t */

/*
  Buffer for unix timestamp in microseconds:
  9,223,372,036,854,775,807 (signed int64 maximal value)
  1 234 567 890 123 456 789

  Note: we can use unsigned for calculation, but practically they
  are the same by probability to overflow them (signed int64 in
  microseconds is enough for almost 3e5 years) and signed allow to
  avoid increasing the buffer (the old buffer for human readable
  date was 19+1).
*/
#define MICROSECOND_TIMESTAMP_BUFFER_SIZE (19 + 1)

/* Structs that defines the TABLE */

class Item;				/* Needed by ORDER */
typedef Item (*Item_ptr);
class Item_subselect;
class Item_field;
class Item_func_hash;
class GRANT_TABLE;
class st_select_lex_unit;
class st_select_lex;
class partition_info;
class COND_EQUAL;
class Security_context;
struct TABLE_LIST;
class ACL_internal_schema_access;
class ACL_internal_table_access;
class Field;
class Copy_field;
class Table_statistics;
class With_element;
struct TDC_element;
class Virtual_column_info;
class Table_triggers_list;
class TMP_TABLE_PARAM;
class SEQUENCE;
class Range_rowid_filter_cost_info;
class derived_handler;
class Pushdown_derived;
struct Name_resolution_context;
class Table_function_json_table;
class Open_table_context;
class MYSQL_LOG;

/*
  Used to identify NESTED_JOIN structures within a join (applicable only to
  structures that have not been simplified away and embed more the one
  element)
*/
typedef ulonglong nested_join_map;

#define VIEW_MD5_LEN 32


#define tmp_file_prefix "#sql"			/**< Prefix for tmp tables */
#define tmp_file_prefix_length 4
#define TMP_TABLE_KEY_EXTRA 8
#define ROCKSDB_DIRECTORY_NAME "#rocksdb"

/**
  Enumerate possible types of a table from re-execution
  standpoint.
  TABLE_LIST class has a member of this type.
  At prepared statement prepare, this member is assigned a value
  as of the current state of the database. Before (re-)execution
  of a prepared statement, we check that the value recorded at
  prepare matches the type of the object we obtained from the
  table definition cache.

  @sa check_and_update_table_version()
  @sa Execute_observer
  @sa Prepared_statement::reprepare()
*/

enum enum_table_ref_type
{
  /** Initial value set by the parser */
  TABLE_REF_NULL= 0,
  TABLE_REF_VIEW,
  TABLE_REF_BASE_TABLE,
  TABLE_REF_I_S_TABLE,
  TABLE_REF_TMP_TABLE
};


/*************************************************************************/

/**
 Object_creation_ctx -- interface for creation context of database objects
 (views, stored routines, events, triggers). Creation context -- is a set
 of attributes, that should be fixed at the creation time and then be used
 each time the object is parsed or executed.
*/

class Object_creation_ctx
{
public:
  Object_creation_ctx *set_n_backup(THD *thd);

  void restore_env(THD *thd, Object_creation_ctx *backup_ctx);

protected:
  Object_creation_ctx() = default;
  virtual Object_creation_ctx *create_backup_ctx(THD *thd) const = 0;

  virtual void change_env(THD *thd) const = 0;

public:
  virtual ~Object_creation_ctx() = default;
};

/*************************************************************************/

/**
 Default_object_creation_ctx -- default implementation of
 Object_creation_ctx.
*/

class Default_object_creation_ctx : public Object_creation_ctx
{
public:
  CHARSET_INFO *get_client_cs()
  {
    return m_client_cs;
  }

  CHARSET_INFO *get_connection_cl()
  {
    return m_connection_cl;
  }

protected:
  Default_object_creation_ctx(THD *thd);

  Default_object_creation_ctx(CHARSET_INFO *client_cs,
                              CHARSET_INFO *connection_cl);

protected:
  virtual Object_creation_ctx *create_backup_ctx(THD *thd) const;

  virtual void change_env(THD *thd) const;

protected:
  /**
    client_cs stores the value of character_set_client session variable.
    The only character set attribute is used.

    Client character set is included into query context, because we save
    query in the original character set, which is client character set. So,
    in order to parse the query properly we have to switch client character
    set on parsing.
  */
  CHARSET_INFO *m_client_cs;

  /**
    connection_cl stores the value of collation_connection session
    variable. Both character set and collation attributes are used.

    Connection collation is included into query context, becase it defines
    the character set and collation of text literals in internal
    representation of query (item-objects).
  */
  CHARSET_INFO *m_connection_cl;
};

class Query_arena;

/*************************************************************************/

/**
 View_creation_ctx -- creation context of view objects.
*/

class View_creation_ctx : public Default_object_creation_ctx,
                          public Sql_alloc
{
public:
  static View_creation_ctx *create(THD *thd);

  static View_creation_ctx *create(THD *thd,
                                   TABLE_LIST *view);

private:
  View_creation_ctx(THD *thd)
    : Default_object_creation_ctx(thd)
  { }
};

/*************************************************************************/

/* Order clause list element */

typedef int (*fast_field_copier)(Field *to, Field *from);


typedef struct st_order {
  struct st_order *next;
  Item	 **item;			/* Point at item in select fields */
  Item	 *item_ptr;			/* Storage for initial item */
  /*
    Reference to the function we are trying to optimize copy to
    a temporary table
  */
  fast_field_copier fast_field_copier_func;
  /* Field for which above optimizer function setup */
  Field  *fast_field_copier_setup;
  int    counter;                       /* position in SELECT list, correct
                                           only if counter_used is true*/
  enum enum_order {
    ORDER_NOT_RELEVANT,
    ORDER_ASC,
    ORDER_DESC
  };

  enum_order direction;                 /* Requested direction of ordering */
  bool	 in_field_list;			/* true if in select field list */
  bool   counter_used;                  /* parameter was counter of columns */
  Field  *field;			/* If tmp-table group */
  char	 *buff;				/* If tmp-table group */
  table_map used; /* NOTE: the below is only set to 0 but is still used by eq_ref_table */
  table_map depend_map;
} ORDER;

/**
  State information for internal tables grants.
  This structure is part of the TABLE_LIST, and is updated
  during the ACL check process.
  @sa GRANT_INFO
*/
struct st_grant_internal_info
{
  /** True if the internal lookup by schema name was done. */
  bool m_schema_lookup_done;
  /** Cached internal schema access. */
  const ACL_internal_schema_access *m_schema_access;
  /** True if the internal lookup by table name was done. */
  bool m_table_lookup_done;
  /** Cached internal table access. */
  const ACL_internal_table_access *m_table_access;
};
typedef struct st_grant_internal_info GRANT_INTERNAL_INFO;

/**
   @brief The current state of the privilege checking process for the current
   user, SQL statement and SQL object.

   @details The privilege checking process is divided into phases depending on
   the level of the privilege to be checked and the type of object to be
   accessed. Due to the mentioned scattering of privilege checking
   functionality, it is necessary to keep track of the state of the
   process. This information is stored in privilege, want_privilege, and
   orig_want_privilege.

   A GRANT_INFO also serves as a cache of the privilege hash tables. Relevant
   members are grant_table and version.
 */
typedef struct st_grant_info
{
  /**
     @brief A copy of the privilege information regarding the current host,
     database, object and user.

     @details The version of this copy is found in GRANT_INFO::version.
   */
  GRANT_TABLE *grant_table_user;
  GRANT_TABLE *grant_table_role;
  GRANT_TABLE *grant_public;
  /**
     @brief Used for cache invalidation when caching privilege information.

     @details The privilege information is stored on disk, with dedicated
     caches residing in memory: table-level and column-level privileges,
     respectively, have their own dedicated caches.

     The GRANT_INFO works as a level 1 cache with this member updated to the
     current value of the global variable @c grant_version (@c static variable
     in sql_acl.cc). It is updated Whenever the GRANT_INFO is refreshed from
     the level 2 cache. The level 2 cache is the @c column_priv_hash structure
     (@c static variable in sql_acl.cc)

     @see grant_version
   */
  uint version;
  /**
     @brief The set of privileges that the current user has fulfilled for a
     certain host, database, and object.
     
     @details This field is continually updated throughout the access checking
     process. In each step the "wanted privilege" is checked against the
     fulfilled privileges. When/if the intersection of these sets is empty,
     access is granted.

     The set is implemented as a bitmap, with the bits defined in sql_acl.h.
   */
  privilege_t privilege;
  /**
     @brief the set of privileges that the current user needs to fulfil in
     order to carry out the requested operation.
   */
  privilege_t want_privilege;
  /**
    Stores the requested access acl of top level tables list. Is used to
    check access rights to the underlying tables of a view.
  */
  privilege_t orig_want_privilege;
  /** The grant state for internal tables. */
  GRANT_INTERNAL_INFO m_internal;

  st_grant_info()
   :privilege(NO_ACL),
    want_privilege(NO_ACL),
    orig_want_privilege(NO_ACL)
  { }

  void read(const Security_context *sctx, const char *db,
            const char *table);

  inline void refresh(const Security_context *sctx, const char *db,
                     const char *table);
  inline privilege_t aggregate_privs();
  inline privilege_t aggregate_cols();

  /* OR table and all column privileges */
  privilege_t all_privilege();
} GRANT_INFO;

enum tmp_table_type
{
  NO_TMP_TABLE= 0, NON_TRANSACTIONAL_TMP_TABLE, TRANSACTIONAL_TMP_TABLE,
  INTERNAL_TMP_TABLE, SYSTEM_TMP_TABLE
};
enum release_type { RELEASE_NORMAL, RELEASE_WAIT_FOR_DROP };


enum vcol_init_mode
{
  VCOL_INIT_DEPENDENCY_FAILURE_IS_WARNING= 1,
  VCOL_INIT_DEPENDENCY_FAILURE_IS_ERROR= 2
  /*
    There may be new flags here.
    e.g. to automatically remove sql_mode dependency:
      GENERATED ALWAYS AS (char_col) ->
      GENERATED ALWAYS AS (RTRIM(char_col))
  */
};


enum enum_vcol_update_mode
{
  VCOL_UPDATE_FOR_READ= 0,
  VCOL_UPDATE_FOR_WRITE,
  VCOL_UPDATE_FOR_DELETE,
  VCOL_UPDATE_INDEXED,
  VCOL_UPDATE_INDEXED_FOR_UPDATE,
  VCOL_UPDATE_FOR_REPLACE
};

/* Field visibility enums */

enum __attribute__((packed)) field_visibility_t {
  VISIBLE= 0,
  INVISIBLE_USER,
  /* automatically added by the server. Can be queried explicitly
  in SELECT, otherwise invisible from anything" */
  INVISIBLE_SYSTEM,
  INVISIBLE_FULL
};

#define INVISIBLE_MAX_BITS              3
#define HA_HASH_FIELD_LENGTH            8
#define HA_HASH_KEY_LENGTH_WITHOUT_NULL 8
#define HA_HASH_KEY_LENGTH_WITH_NULL    9


int fields_in_hash_keyinfo(KEY *keyinfo);

void setup_keyinfo_hash(KEY *key_info);

void re_setup_keyinfo_hash(KEY *key_info);

/**
  Category of table found in the table share.
*/
enum enum_table_category
{
  /**
    Unknown value.
  */
  TABLE_UNKNOWN_CATEGORY=0,

  /**
    Temporary table.
    The table is visible only in the session.
    Therefore,
    - FLUSH TABLES WITH READ LOCK
    - SET GLOBAL READ_ONLY = ON
    do not apply to this table.
    Note that LOCK TABLE t FOR READ/WRITE
    can be used on temporary tables.
    Temporary tables are not part of the table cache.
  */
  TABLE_CATEGORY_TEMPORARY=1,

  /**
    User table.
    These tables do honor:
    - LOCK TABLE t FOR READ/WRITE
    - FLUSH TABLES WITH READ LOCK
    - SET GLOBAL READ_ONLY = ON
    User tables are cached in the table cache.
  */
  TABLE_CATEGORY_USER=2,

  /**
    System table, maintained by the server.
    These tables do honor:
    - LOCK TABLE t FOR READ/WRITE
    - FLUSH TABLES WITH READ LOCK
    - SET GLOBAL READ_ONLY = ON
    Typically, writes to system tables are performed by
    the server implementation, not explicitly be a user.
    System tables are cached in the table cache.
  */
  TABLE_CATEGORY_SYSTEM=3,

  /**
    Log tables.
    These tables are an interface provided by the system
    to inspect the system logs.
    These tables do *not* honor:
    - LOCK TABLE t FOR READ/WRITE
    - FLUSH TABLES WITH READ LOCK
    - SET GLOBAL READ_ONLY = ON
    as there is no point in locking explicitly
    a LOG table.
    An example of LOG tables are:
    - mysql.slow_log
    - mysql.general_log,
    which *are* updated even when there is either
    a GLOBAL READ LOCK or a GLOBAL READ_ONLY in effect.
    User queries do not write directly to these tables
    (there are exceptions for log tables).
    The server implementation perform writes.
    Log tables are cached in the table cache.
  */
  TABLE_CATEGORY_LOG=4,

  /*
    Types below are read only tables, not affected by FLUSH TABLES or
    MDL locks.
  */
  /**
    Information schema tables.
    These tables are an interface provided by the system
    to inspect the system metadata.
    These tables do *not* honor:
    - LOCK TABLE t FOR READ/WRITE
    - FLUSH TABLES WITH READ LOCK
    - SET GLOBAL READ_ONLY = ON
    as there is no point in locking explicitly
    an INFORMATION_SCHEMA table.
    Nothing is directly written to information schema tables.
    Note that this value is not used currently,
    since information schema tables are not shared,
    but implemented as session specific temporary tables.
  */
  /*
    TODO: Fixing the performance issues of I_S will lead
    to I_S tables in the table cache, which should use
    this table type.
  */
  TABLE_CATEGORY_INFORMATION=5,

  /**
    Performance schema tables.
    These tables are an interface provided by the system
    to inspect the system performance data.
    These tables do *not* honor:
    - LOCK TABLE t FOR READ/WRITE
    - FLUSH TABLES WITH READ LOCK
    - SET GLOBAL READ_ONLY = ON
    as there is no point in locking explicitly
    a PERFORMANCE_SCHEMA table.
    An example of PERFORMANCE_SCHEMA tables are:
    - performance_schema.*
    which *are* updated (but not using the handler interface)
    even when there is either
    a GLOBAL READ LOCK or a GLOBAL READ_ONLY in effect.
    User queries do not write directly to these tables
    (there are exceptions for SETUP_* tables).
    The server implementation perform writes.
    Performance tables are cached in the table cache.
  */
  TABLE_CATEGORY_PERFORMANCE=6
};

typedef enum enum_table_category TABLE_CATEGORY;

TABLE_CATEGORY get_table_category(const LEX_CSTRING *db,
                                  const LEX_CSTRING *name);


typedef struct st_table_field_type
{
  LEX_CSTRING name;
  LEX_CSTRING type;
  LEX_CSTRING cset;
} TABLE_FIELD_TYPE;


typedef struct st_table_field_def
{
  uint count;
  const TABLE_FIELD_TYPE *field;
  uint primary_key_parts;
  const uint *primary_key_columns;
} TABLE_FIELD_DEF;


class Table_check_intact
{
protected:
  bool has_keys;
  virtual void report_error(uint code, const char *fmt, ...)= 0;

public:
  Table_check_intact(bool keys= false) : has_keys(keys) {}
  virtual ~Table_check_intact() = default;

  /** Checks whether a table is intact. */
  bool check(TABLE *table, const TABLE_FIELD_DEF *table_def);
};


/*
  If the table isn't valid, report the error to the server log only.
*/
class Table_check_intact_log_error : public Table_check_intact
{
protected:
  void report_error(uint, const char *fmt, ...);
public:
  Table_check_intact_log_error() : Table_check_intact(true) {}
};


/**
  Class representing the fact that some thread waits for table
  share to be flushed. Is used to represent information about
  such waits in MDL deadlock detector.
*/

class Wait_for_flush : public MDL_wait_for_subgraph
{
  MDL_context *m_ctx;
  TABLE_SHARE *m_share;
  uint m_deadlock_weight;
public:
  Wait_for_flush(MDL_context *ctx_arg, TABLE_SHARE *share_arg,
               uint deadlock_weight_arg)
    : m_ctx(ctx_arg), m_share(share_arg),
      m_deadlock_weight(deadlock_weight_arg)
  {}

  MDL_context *get_ctx() const { return m_ctx; }

  virtual bool accept_visitor(MDL_wait_for_graph_visitor *dvisitor);

  virtual uint get_deadlock_weight() const;

  /**
    Pointers for participating in the list of waiters for table share.
  */
  Wait_for_flush *next_in_share;
  Wait_for_flush **prev_in_share;
};


typedef I_P_List <Wait_for_flush,
                  I_P_List_adapter<Wait_for_flush,
                                   &Wait_for_flush::next_in_share,
                                   &Wait_for_flush::prev_in_share> >
                 Wait_for_flush_list;


enum open_frm_error {
  OPEN_FRM_OK = 0,
  OPEN_FRM_OPEN_ERROR,
  OPEN_FRM_READ_ERROR,
  OPEN_FRM_CORRUPTED,
  OPEN_FRM_DISCOVER,
  OPEN_FRM_ERROR_ALREADY_ISSUED,
  OPEN_FRM_NOT_A_VIEW,
  OPEN_FRM_NOT_A_TABLE,
  OPEN_FRM_NEEDS_REBUILD
};

/**
  Control block to access table statistics loaded 
  from persistent statistical tables
*/


#define TABLE_STAT_NO_STATS    0
#define TABLE_STAT_TABLE       1
#define TABLE_STAT_COLUMN      2
#define TABLE_STAT_INDEX       4
#define TABLE_STAT_HISTOGRAM   8

/*
  EITS statistics information for a table.

  This data is loaded from mysql.{table|index|column}_stats tables and
  then most of the time is owned by table's TABLE_SHARE object.

  Individual TABLE objects also have pointer to this object, and we do
  reference counting to know when to free it. See
  TABLE::update_engine_stats(), TABLE::free_engine_stats(),
  TABLE_SHARE::update_engine_stats(), TABLE_SHARE::destroy().
  These implement a "shared pointer"-like functionality.

  When new statistics is loaded, we create new TABLE_STATISTICS_CB and make
  the TABLE_SHARE point to it. Some TABLE object may still be using older
  TABLE_STATISTICS_CB objects.  Reference counting allows to free
  TABLE_STATISTICS_CB when it is no longer used.
*/

class TABLE_STATISTICS_CB
{
  uint usage_count;                             // Instances of this stat

public:
  TABLE_STATISTICS_CB();
  ~TABLE_STATISTICS_CB();
  MEM_ROOT  mem_root; /* MEM_ROOT to allocate statistical data for the table */
  Table_statistics *table_stats; /* Structure to access the statistical data */
  uint  stats_available;
  bool  histograms_exists_on_disk;

  bool histograms_exists() const
  {
    return histograms_exists_on_disk;
  }
  bool unused()
  {
    return usage_count == 0;
  }
  /* Copy (latest) state from TABLE_SHARE to TABLE */
  void update_stats_in_table(TABLE *table);
  friend struct TABLE;
  friend struct TABLE_SHARE;
};

/**
  This structure is shared between different table objects. There is one
  instance of table share per one table in the database.
*/

struct TABLE_SHARE
{
  TABLE_SHARE() = default;                    /* Remove gcc warning */

  /** Category of this table. */
  TABLE_CATEGORY table_category;

  /* hash of field names (contains pointers to elements of field array) */
  HASH	name_hash;			/* hash of field names */
  MEM_ROOT mem_root;
  TYPELIB keynames;			/* Pointers to keynames */
  TYPELIB fieldnames;			/* Pointer to fieldnames */
  TYPELIB *intervals;			/* pointer to interval info */
  mysql_mutex_t LOCK_ha_data;           /* To protect access to ha_data */
  mysql_mutex_t LOCK_share;             /* To protect TABLE_SHARE */
  mysql_mutex_t LOCK_statistics;        /* To protect against concurrent load */

  TDC_element *tdc;

  LEX_CUSTRING tabledef_version;

  engine_option_value *option_list;     /* text options for table */
  ha_table_option_struct *option_struct; /* structure with parsed options */

  /* The following is copied to each TABLE on OPEN */
  Field **field;
  Field **found_next_number_field;
  KEY  *key_info;			/* data of keys in database */
  Virtual_column_info **check_constraints;
  uint	*blob_field;			/* Index to blobs in Field arrray*/
  LEX_CUSTRING vcol_defs;              /* definitions of generated columns */

  /*
    EITS statistics data from the last time the table was opened or ANALYZE
    table was run.
    This is typically same as any related TABLE::stats_cb until ANALYZE
    table is run.
    This pointer is only to be de-referenced under LOCK_share as the
    pointer can change by another thread running ANALYZE TABLE.
    Without using a LOCK_share one can check if the statistics has been
    updated by checking if TABLE::stats_cb != TABLE_SHARE::stats_cb.
  */
  TABLE_STATISTICS_CB *stats_cb;

  uchar	*default_values;		/* row with default values */
  LEX_CSTRING comment;			/* Comment about table */
  CHARSET_INFO *table_charset;		/* Default charset of string fields */

  MY_BITMAP *check_set;                 /* Fields used by check constrant */
  MY_BITMAP all_set;
  /*
    Key which is used for looking-up table in table cache and in the list
    of thread's temporary tables. Has the form of:
      "database_name\0table_name\0" + optional part for temporary tables.

    Note that all three 'table_cache_key', 'db' and 'table_name' members
    must be set (and be non-zero) for tables in table cache. They also
    should correspond to each other.
    To ensure this one can use set_table_cache() methods.
  */
  LEX_CSTRING table_cache_key;
  LEX_CSTRING db;                        /* Pointer to db */
  LEX_CSTRING table_name;                /* Table name (for open) */
  LEX_CSTRING path;                	/* Path to .frm file (from datadir) */
  LEX_CSTRING normalized_path;		/* unpack_filename(path) */
  LEX_CSTRING connect_string;

  /* 
     Set of keys in use, implemented as a Bitmap.
     Excludes keys disabled by ALTER TABLE ... DISABLE KEYS.
  */
  key_map keys_in_use;

  /* The set of ignored indexes for a table. */
  key_map ignored_indexes;

  key_map keys_for_keyread;
  ha_rows min_rows, max_rows;		/* create information */
  ulong   avg_row_length;		/* create information */
  ulong   mysql_version;		/* 0 if .frm is created before 5.0 */
  ulong   reclength;			/* Recordlength */
  /* Stored record length. No generated-only virtual fields are included */
  ulong   stored_rec_length;            

  plugin_ref db_plugin;			/* storage engine plugin */
  inline handlerton *db_type() const	/* table_type for handler */
  { 
    return is_view   ? view_pseudo_hton :
           db_plugin ? plugin_hton(db_plugin) : NULL;
  }
  OPTIMIZER_COSTS optimizer_costs;      /* Copy of get_optimizer_costs() */
  enum row_type row_type;		/* How rows are stored */
  enum Table_type table_type;
  enum tmp_table_type tmp_table;

  /** Transactional or not. */
  enum ha_choice transactional;
  /** Per-page checksums or not. */
  enum ha_choice page_checksum;

  uint key_block_size;			/* create key_block_size, if used */
  uint stats_sample_pages;		/* number of pages to sample during
					stats estimation, if used, otherwise 0. */
  enum_stats_auto_recalc stats_auto_recalc; /* Automatic recalc of stats. */
  uint null_bytes, last_null_bit_pos;
  /*
    Same as null_bytes, except that if there is only a 'delete-marker' in
    the record then this value is 0.
  */
  uint null_bytes_for_compare;
  uint fields;                          /* number of fields */
  /* number of stored fields, purely virtual not included */
  uint stored_fields;
  uint virtual_fields;                  /* number of purely virtual fields */
  /* number of purely virtual not stored blobs */
  uint virtual_not_stored_blob_fields;
  uint null_fields;                     /* number of null fields */
  uint blob_fields;                     /* number of blob fields */
  uint varchar_fields;                  /* number of varchar fields */
  uint default_fields;                  /* number of default fields */
  uint visible_fields;                  /* number of visible fields */

  uint default_expressions;
  uint table_check_constraints, field_check_constraints;

  uint rec_buff_length;                 /* Size of table->record[] buffer */
  uint keys, key_parts;
  uint ext_key_parts;       /* Total number of key parts in extended keys */
  uint max_key_length, max_unique_length;

  /*
    Older versions had TABLE_SHARE::uniques but now it is replaced with
    per-index HA_UNIQUE_HASH flag
  */
  bool have_unique_constraint() const
  {
    for (uint i=0; i < keys; i++)
      if (key_info[i].flags & HA_UNIQUE_HASH)
        return true;
    return false;
  }
  uint db_create_options;		/* Create options from database */
  uint db_options_in_use;		/* Options in use */
  uint db_record_offset;		/* if HA_REC_IN_SEQ */
  uint rowid_field_offset;		/* Field_nr +1 to rowid field */
  /* Primary key index number, used in TABLE::key_info[] */
  uint primary_key;                     
  uint next_number_index;               /* autoincrement key number */
  uint next_number_key_offset;          /* autoinc keypart offset in a key */
  uint next_number_keypart;             /* autoinc keypart number in a key */
  enum open_frm_error error;            /* error from open_table_def() */
  uint open_errno;                      /* error from open_table_def() */
  uint column_bitmap_size;
  uchar frm_version;

  enum enum_v_keys { NOT_INITIALIZED=0, NO_V_KEYS, V_KEYS };
  enum_v_keys check_set_initialized;

  bool use_ext_keys;                    /* Extended keys can be used */
  bool null_field_first;
  bool system;                          /* Set if system table (one record) */
  bool not_usable_by_query_cache;
  bool online_backup;                   /* Set if on-line backup supported */
  /*
    This is used by log tables, for tables that have their own internal
    binary logging or for tables that doesn't support statement or row logging
   */
  bool no_replicate;
  bool crashed;
  bool is_view;
  bool can_cmp_whole_record;
  /* This is set for temporary tables where CREATE was binary logged */
  bool table_creation_was_logged;
  bool non_determinstic_insert;
  bool has_update_default_function;
  bool can_do_row_logging;              /* 1 if table supports RBR */
  bool long_unique_table;
  /* 1 if frm version cannot be updated as part of upgrade */
  bool keep_original_mysql_version;
  bool optimizer_costs_inited;

  ulong table_map_id;                   /* for row-based replication */

  /*
    Things that are incompatible between the stored version and the
    current version. This is a set of HA_CREATE... bits that can be used
    to modify create_info->used_fields for ALTER TABLE.
  */
  ulong incompatible_version;

  /**
    For shares representing views File_parser object with view
    definition read from .FRM file.
  */
  const File_parser *view_def;

  /* For sequence tables, the current sequence state */
  SEQUENCE *sequence;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  /* filled in when reading from frm */
  bool auto_partitioned;
  char *partition_info_str;
  uint  partition_info_str_len;
  uint  partition_info_buffer_size;
  plugin_ref default_part_plugin;
#endif

#ifdef HAVE_REPLICATION
  Cache_flip_event_log *online_alter_binlog;
#endif

  /**
    System versioning and application-time periods support.
  */
  struct period_info_t
  {
    field_index_t start_fieldno;
    field_index_t end_fieldno;
    Lex_ident name;
    Lex_ident constr_name;
    uint unique_keys;
    Field *start_field(TABLE_SHARE *s) const
    {
      return s->field[start_fieldno];
    }
    Field *end_field(TABLE_SHARE *s) const
    {
      return s->field[end_fieldno];
    }
  };

  vers_kind_t versioned;
  period_info_t vers;
  period_info_t period;
  /*
      Protect multiple threads from repeating partition auto-create over
      single share.

      TODO: remove it when partitioning metadata will be in TABLE_SHARE.
  */
  bool          vers_skip_auto_create;

  bool init_period_from_extra2(period_info_t *period, const uchar *data,
                               const uchar *end);

  Field *vers_start_field()
  {
    DBUG_ASSERT(versioned);
    return field[vers.start_fieldno];
  }

  Field *vers_end_field()
  {
    DBUG_ASSERT(versioned);
    return field[vers.end_fieldno];
  }

  Field *period_start_field() const
  {
    DBUG_ASSERT(period.name);
    return field[period.start_fieldno];
  }

  Field *period_end_field() const
  {
    DBUG_ASSERT(period.name);
    return field[period.end_fieldno];
  }

  /**
    Cache the checked structure of this table.

    The pointer data is used to describe the structure that
    a instance of the table must have. Each element of the
    array specifies a field that must exist on the table.

    The pointer is cached in order to perform the check only
    once -- when the table is loaded from the disk.
  */
  const TABLE_FIELD_DEF *table_field_def_cache;

  /** Main handler's share */
  Handler_share *ha_share;

  /** Instrumentation for this table share. */
  PSI_table_share *m_psi;

  inline void reset() { bzero((void*)this, sizeof(*this)); }

  /*
    Set share's table cache key and update its db and table name appropriately.

    SYNOPSIS
      set_table_cache_key()
        key_buff    Buffer with already built table cache key to be
                    referenced from share.
        key_length  Key length.

    NOTES
      Since 'key_buff' buffer will be referenced from share it should has same
      life-time as share itself.
      This method automatically ensures that TABLE_SHARE::table_name/db have
      appropriate values by using table cache key as their source.
  */

  void set_table_cache_key(char *key_buff, uint key_length)
  {
    table_cache_key.str= key_buff;
    table_cache_key.length= key_length;
    /*
      Let us use the fact that the key is "db/0/table_name/0" + optional
      part for temporary tables.
    */
    db.str=            table_cache_key.str;
    db.length=         strlen(db.str);
    table_name.str=    db.str + db.length + 1;
    table_name.length= strlen(table_name.str);
  }


  /*
    Set share's table cache key and update its db and table name appropriately.

    SYNOPSIS
      set_table_cache_key()
        key_buff    Buffer to be used as storage for table cache key
                    (should be at least key_length bytes).
        key         Value for table cache key.
        key_length  Key length.

    NOTE
      Since 'key_buff' buffer will be used as storage for table cache key
      it should has same life-time as share itself.
  */

  void set_table_cache_key(char *key_buff, const char *key, uint key_length)
  {
    memcpy(key_buff, key, key_length);
    set_table_cache_key(key_buff, key_length);
  }

  inline bool require_write_privileges()
  {
    return (table_category == TABLE_CATEGORY_LOG);
  }

  inline ulong get_table_def_version()
  {
    return table_map_id;
  }

  /**
    Convert unrelated members of TABLE_SHARE to one enum
    representing its type.

    @todo perhaps we need to have a member instead of a function.
  */
  enum enum_table_ref_type get_table_ref_type() const
  {
    if (is_view)
      return TABLE_REF_VIEW;
    switch (tmp_table) {
    case NO_TMP_TABLE:
      return TABLE_REF_BASE_TABLE;
    case SYSTEM_TMP_TABLE:
      return TABLE_REF_I_S_TABLE;
    default:
      return TABLE_REF_TMP_TABLE;
    }
  }
  /**
    Return a table metadata version.
     * for base tables and views, we return table_map_id.
       It is assigned from a global counter incremented for each
       new table loaded into the table definition cache (TDC).
     * for temporary tables it's table_map_id again. But for
       temporary tables table_map_id is assigned from
       thd->query_id. The latter is assigned from a thread local
       counter incremented for every new SQL statement. Since
       temporary tables are thread-local, each temporary table
       gets a unique id.
     * for everything else (e.g. information schema tables),
       the version id is zero.

   This choice of version id is a large compromise
   to have a working prepared statement validation in 5.1. In
   future version ids will be persistent, as described in WL#4180.

   Let's try to explain why and how this limited solution allows
   to validate prepared statements.

   Firstly, sets (in mathematical sense) of version numbers
   never intersect for different table types. Therefore,
   version id of a temporary table is never compared with
   a version id of a view, and vice versa.

   Secondly, for base tables and views, we know that each DDL flushes
   the respective share from the TDC. This ensures that whenever
   a table is altered or dropped and recreated, it gets a new
   version id.
   Unfortunately, since elements of the TDC are also flushed on
   LRU basis, this choice of version ids leads to false positives.
   E.g. when the TDC size is too small, we may have a SELECT
   * FROM INFORMATION_SCHEMA.TABLES flush all its elements, which
   in turn will lead to a validation error and a subsequent
   reprepare of all prepared statements.  This is
   considered acceptable, since as long as prepared statements are
   automatically reprepared, spurious invalidation is only
   a performance hit. Besides, no better simple solution exists.

   For temporary tables, using thd->query_id ensures that if
   a temporary table was altered or recreated, a new version id is
   assigned. This suits validation needs very well and will perhaps
   never change.

   Metadata of information schema tables never changes.
   Thus we can safely assume 0 for a good enough version id.

   Finally, by taking into account table type, we always
   track that a change has taken place when a view is replaced
   with a base table, a base table is replaced with a temporary
   table and so on.

   @sa TABLE_LIST::is_the_same_definition()
  */
  ulong get_table_ref_version() const
  {
    return (tmp_table == SYSTEM_TMP_TABLE) ? 0 : table_map_id;
  }

  bool visit_subgraph(Wait_for_flush *waiting_ticket,
                      MDL_wait_for_graph_visitor *gvisitor);

  bool wait_for_old_version(THD *thd, struct timespec *abstime,
                            uint deadlock_weight);
  /** Release resources and free memory occupied by the table share. */
  void destroy();

  void set_use_ext_keys_flag(bool fl) 
  {
    use_ext_keys= fl;
  }
  
  uint actual_n_key_parts(THD *thd);

  LEX_CUSTRING *frm_image; ///< only during CREATE TABLE (@sa ha_create_table)

  /*
    populates TABLE_SHARE from the table description in the binary frm image.
    if 'write' is true, this frm image is also written into a corresponding
    frm file, that serves as a persistent metadata cache to avoid
    discovering the table over and over again
  */
  int init_from_binary_frm_image(THD *thd, bool write,
                                 const uchar *frm_image, size_t frm_length,
                                 const uchar *par_image=0,
                                 size_t par_length=0);

  /*
    populates TABLE_SHARE from the table description, specified as the
    complete CREATE TABLE sql statement.
    if 'write' is true, this frm image is also written into a corresponding
    frm file, that serves as a persistent metadata cache to avoid
    discovering the table over and over again
  */
  int init_from_sql_statement_string(THD *thd, bool write,
                                     const char *sql, size_t sql_length);
  /*
    writes the frm image to an frm file, corresponding to this table
  */
  bool write_frm_image(const uchar *frm_image, size_t frm_length);
  bool write_par_image(const uchar *par_image, size_t par_length);

  /* Only used by S3 */
  bool write_frm_image(void)
  { return frm_image ? write_frm_image(frm_image->str, frm_image->length) : 0; }

  /*
    returns an frm image for this table.
    the memory is allocated and must be freed later
  */
  bool read_frm_image(const uchar **frm_image, size_t *frm_length);

  /* frees the memory allocated in read_frm_image */
  void free_frm_image(const uchar *frm);

  void set_overlapped_keys();
  void set_ignored_indexes();
  key_map usable_indexes(THD *thd);
  bool old_long_hash_function() const
  {
    return mysql_version < 100428 ||
           (mysql_version >= 100500 && mysql_version < 100519) ||
           (mysql_version >= 100600 && mysql_version < 100612) ||
           (mysql_version >= 100700 && mysql_version < 100708) ||
           (mysql_version >= 100800 && mysql_version < 100807) ||
           (mysql_version >= 100900 && mysql_version < 100905) ||
           (mysql_version >= 101000 && mysql_version < 101003) ||
           (mysql_version >= 101100 && mysql_version < 101102);
  }
  Item_func_hash *make_long_hash_func(THD *thd,
                                      MEM_ROOT *mem_root,
                                      List<Item> *field_list) const;
  void update_optimizer_costs(handlerton *hton);
  void update_engine_independent_stats(TABLE_STATISTICS_CB *stat);
  bool histograms_exists();
};

/* not NULL, but cannot be dereferenced */
#define UNUSABLE_TABLE_SHARE ((TABLE_SHARE*)1)

/**
   Class is used as a BLOB field value storage for
   intermediate GROUP_CONCAT results. Used only for
   GROUP_CONCAT with  DISTINCT or ORDER BY options.
 */

class Blob_mem_storage: public Sql_alloc
{
private:
  MEM_ROOT storage;
  /**
    Sign that some values were cut
    during saving into the storage.
  */
  bool truncated_value;
public:
  Blob_mem_storage() :truncated_value(false)
  {
    init_alloc_root(key_memory_blob_mem_storage,
                    &storage, MAX_FIELD_VARCHARLENGTH, 0, MYF(0));
  }
  ~ Blob_mem_storage()
  {
    free_root(&storage, MYF(0));
  }
  void reset()
  {
    free_root(&storage, MYF(MY_MARK_BLOCKS_FREE));
    truncated_value= false;
  }
  /**
     Fuction creates duplicate of 'from'
     string in 'storage' MEM_ROOT.

     @param from           string to copy
     @param length         string length

     @retval Pointer to the copied string.
     @retval 0 if an error occurred.
  */
  char *store(const char *from, size_t length)
  {
    return (char*) memdup_root(&storage, from, length);
  }
  void set_truncated_value(bool is_truncated_value)
  {
    truncated_value= is_truncated_value;
  }
  bool is_truncated_value() { return truncated_value; }
};


/* Information for one open table */
enum index_hint_type
{
  INDEX_HINT_IGNORE,
  INDEX_HINT_USE,
  INDEX_HINT_FORCE
};

struct st_cond_statistic;

#define      CHECK_ROW_FOR_NULLS_TO_REJECT   (1 << 0)
#define      REJECT_ROW_DUE_TO_NULL_FIELDS   (1 << 1)

class SplM_opt_info;

struct vers_select_conds_t;

struct TABLE
{
  TABLE() = default;                               /* Remove gcc warning */

  TABLE_SHARE	*s;
  handler	*file;
  TABLE *next, *prev;

private:
  /**
     Links for the list of all TABLE objects for this share.
     Declared as private to avoid direct manipulation with those objects.
     One should use methods of I_P_List template instead.
  */
  TABLE *share_all_next, **share_all_prev;
  TABLE *global_free_next, **global_free_prev;
  friend struct All_share_tables;
  friend struct Table_cache_instance;

public:

  uint32 instance; /** Table cache instance this TABLE is belonging to */
  THD	*in_use;                        /* Which thread uses this */

  uchar *record[3];			/* Pointer to records */
  uchar *write_row_record;		/* Used as optimisation in
					   THD::write_row */
  uchar *insert_values;                  /* used by INSERT ... UPDATE */
  /* 
    Map of keys that can be used to retrieve all data from this table 
    needed by the query without reading the row.
  */
  key_map covering_keys, intersect_keys;
  /*
    A set of keys that can be used in the query that references this
    table.

    All indexes disabled on the table's TABLE_SHARE (see TABLE::s) will be 
    subtracted from this set upon instantiation. Thus for any TABLE t it holds
    that t.keys_in_use_for_query is a subset of t.s.keys_in_use. Generally we 
    must not introduce any new keys here (see setup_tables).

    The set is implemented as a bitmap.
  */
  key_map keys_in_use_for_query;
  /* Map of keys that can be used to calculate GROUP BY without sorting */
  key_map keys_in_use_for_group_by;
  /* Map of keys that can be used to calculate ORDER BY without sorting */
  key_map keys_in_use_for_order_by;
  /* Map of keys dependent on some constraint */
  key_map constraint_dependent_keys;
  KEY  *key_info;			/* data of keys in database */

  Field **field;                        /* Pointer to fields */
  Field **vfield;                       /* Pointer to virtual fields*/
  Field **default_field;                /* Fields with non-constant DEFAULT */
  Field *next_number_field;		/* Set if next_number is activated */
  Field *found_next_number_field;	/* Set on open */
  Virtual_column_info **check_constraints;

  /* Table's triggers, 0 if there are no of them */
  Table_triggers_list *triggers;
  TABLE_LIST *pos_in_table_list;/* Element referring to this table */
  /* Position in thd->locked_table_list under LOCK TABLES */
  TABLE_LIST *pos_in_locked_tables;
  /* Tables used in DEFAULT and CHECK CONSTRAINT (normally sequence tables) */
  TABLE_LIST *internal_tables;

  /*
    Not-null for temporary tables only. Non-null values means this table is
    used to compute GROUP BY, it has a unique of GROUP BY columns.
    (set by create_tmp_table)
  */
  ORDER		*group;
  String	alias;            	  /* alias or table name */
  uchar		*null_flags;
  MY_BITMAP     def_read_set, def_write_set, tmp_set;
  MY_BITMAP     def_rpl_write_set;
  MY_BITMAP     eq_join_set;         /* used to mark equi-joined fields */
  MY_BITMAP     cond_set;   /* used to mark fields from sargable conditions*/
  /* Active column sets */
  MY_BITMAP     *read_set, *write_set, *rpl_write_set;
  /* On INSERT: fields that the user specified a value for */
  MY_BITMAP	has_value_set;

  /*
   The ID of the query that opened and is using this table. Has different
   meanings depending on the table type.

   Temporary tables:

   table->query_id is set to thd->query_id for the duration of a statement
   and is reset to 0 once it is closed by the same statement. A non-zero
   table->query_id means that a statement is using the table even if it's
   not the current statement (table is in use by some outer statement).

   Non-temporary tables:

   Under pre-locked or LOCK TABLES mode: query_id is set to thd->query_id
   for the duration of a statement and is reset to 0 once it is closed by
   the same statement. A non-zero query_id is used to control which tables
   in the list of pre-opened and locked tables are actually being used.
  */
  query_id_t	query_id;

  /*
    This structure is used for statistical data on the table that
    is collected by the function collect_statistics_for_table
  */
  Table_statistics *collected_stats;

  /* The estimate of the number of records in the table used by optimizer */ 
  ha_rows used_stat_records;

  key_map opt_range_keys;
  /* 
    The following structure is filled for each key that has
    opt_range_keys.is_set(key) == TRUE
  */
  struct OPT_RANGE
  {
    uint        key_parts;
    uint        ranges;
    ha_rows     rows, max_index_blocks, max_row_blocks;
    Cost_estimate cost;
    /* Selectivity, in case of filters */
    double      selectivity;
    bool        first_key_part_has_only_one_value;

    /*
      Cost of fetching keys with index only read and returning them to the
      sql level.
    */
    double index_only_fetch_cost(TABLE *table);
    void get_costs(ALL_READ_COST *cost);
  } *opt_range;
  /* 
     Bitmaps of key parts that =const for the duration of join execution. If
     we're in a subquery, then the constant may be different across subquery
     re-executions.
  */
  key_part_map *const_key_parts;

  /* 
    Estimate of number of records that satisfy SARGable part of the table
    condition, or table->file->records if no SARGable condition could be
    constructed.
    This value is used by join optimizer as an estimate of number of records
    that will pass the table condition (condition that depends on fields of 
    this table and constants)
  */
  ha_rows       opt_range_condition_rows;

  double cond_selectivity;
  List<st_cond_statistic> *cond_selectivity_sampling_explain;

  table_map	map;                    /* ID bit of table (1,2,4,8,16...) */

  uint          lock_position;          /* Position in MYSQL_LOCK.table */
  uint          lock_data_start;        /* Start pos. in MYSQL_LOCK.locks */
  uint          lock_count;             /* Number of locks */
  uint		tablenr,used_fields;
  uint          temp_pool_slot;		/* Used by intern temp tables */
  uint		status;                 /* What's in record[0] */
  uint		db_stat;		/* mode of file as in handler.h */
  /* number of select if it is derived table */
  uint          derived_select_number;
  /*
    Possible values:
     - 0 by default
     - JOIN_TYPE_{LEFT|RIGHT} if the table is inner w.r.t an outer join
       operation
     - 1 if the SELECT has mixed_implicit_grouping=1. example:
       select max(col1), col2 from t1. In this case, the query produces
       one row with all columns having NULL values.

    Interpetation: If maybe_null!=0, all fields of the table are considered
    NULLable (and have NULL values when null_row=true)
  */
  uint maybe_null;
  int		current_lock;           /* Type of lock on table */
  bool copy_blobs;			/* copy_blobs when storing */
  /*
    Set if next_number_field is in the UPDATE fields of INSERT ... ON DUPLICATE
    KEY UPDATE.
  */
  bool next_number_field_updated;

  /*
    If true, the current table row is considered to have all columns set to 
    NULL, including columns declared as "not null" (see maybe_null).
  */
  bool null_row;
  /*
    No rows that contain null values can be placed into this table.
    Currently this flag can be set to true only for a temporary table
    that used to store the result of materialization of a subquery.
  */
  bool no_rows_with_nulls;
  /*
    This field can contain two bit flags: 
      CHECK_ROW_FOR_NULLS_TO_REJECT
      REJECT_ROW_DUE_TO_NULL_FIELDS
    The first flag is set for the dynamic contexts where it is prohibited
    to write any null into the table.
    The second flag is set only if the first flag is set on.
    The informs the outer scope that there was an attept to write null
    into a field of the table in the context where it is prohibited.
    This flag should be set off as soon as the first flag is set on.
    Currently these flags are used only the tables tno_rows_with_nulls set
    to true. 
  */       
  uint8 null_catch_flags;

  /*
    TODO: Each of the following flags take up 8 bits. They can just as easily
    be put into one single unsigned long and instead of taking up 18
    bytes, it would take up 4.
  */
  bool force_index;

  /* Flag set when the statement contains FORCE INDEX FOR JOIN */
  bool force_index_join;

  /**
    Flag set when the statement contains FORCE INDEX FOR ORDER BY
    See TABLE_LIST::process_index_hints().
  */
  bool force_index_order;

  /**
    Flag set when the statement contains FORCE INDEX FOR GROUP BY
    See TABLE_LIST::process_index_hints().
  */
  bool force_index_group;
  /*
    TRUE<=> this table was created with create_tmp_table(... distinct=TRUE..)
    call
  */
  bool distinct;
  bool const_table,no_rows, used_for_duplicate_elimination;
  /**
    Forces DYNAMIC Aria row format for internal temporary tables.
  */
  bool keep_row_order;

  bool no_keyread;
  /**
    If set, indicate that the table is not replicated by the server.
  */
  bool locked_by_logger;
  bool locked_by_name;
  bool fulltext_searched;
  bool no_cache;
  /* To signal that the table is associated with a HANDLER statement */
  bool open_by_handler;
  /*
    To indicate that a non-null value of the auto_increment field
    was provided by the user or retrieved from the current record.
    Used only in the MODE_NO_AUTO_VALUE_ON_ZERO mode.
  */
  bool auto_increment_field_not_null;
  bool insert_or_update;             /* Can be used by the handler */
  /*
     NOTE: alias_name_used is only a hint! It works only in need_correct_ident()
     condition. On other cases it is FALSE even if table_name is alias.

     E.g. in update t1 as x set a = 1
  */
  bool alias_name_used;              /* true if table_name is alias */
  bool get_fields_in_item_tree;      /* Signal to fix_field */
  List<Virtual_column_info> vcol_refix_list;
private:
  bool m_needs_reopen;
  bool created;    /* For tmp tables. TRUE <=> tmp table was actually created.*/
public:
#ifdef HAVE_REPLICATION
  /* used in RBR Triggers */
  bool master_had_triggers;
#endif

  REGINFO reginfo;			/* field connections */
  MEM_ROOT mem_root;
  /**
     Initialized in Item_func_group_concat::setup for appropriate
     temporary table if GROUP_CONCAT is used with ORDER BY | DISTINCT
     and BLOB field count > 0.
   */
  Blob_mem_storage *blob_storage;
  GRANT_INFO grant;
  /*
    The arena which the items for expressions from the table definition
    are associated with.  
    Currently only the items of the expressions for virtual columns are
    associated with this arena.
    TODO: To attach the partitioning expressions to this arena.  
  */
  Query_arena *expr_arena;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info;            /* Partition related information */
  /* If true, all partitions have been pruned away */
  bool all_partitions_pruned_away;
#endif
  uint max_keys; /* Size of allocated key_info array. */
  bool stats_is_read;     /* Persistent statistics is read for the table */
  bool histograms_are_read;
  MDL_ticket *mdl_ticket;

  /*
    This is used only for potentially splittable materialized tables and it
    points to the info used by the optimizer to apply splitting optimization
  */
  SplM_opt_info *spl_opt_info;
  key_map keys_usable_for_splitting;

  /*
    Conjunction of the predicates of the form IS NOT NULL(f) where f refers to
    a column of this TABLE such that they can be inferred from the condition
    of the  WHERE clause or from some ON expression of the processed select
    and can be useful for range optimizer.
  */
  Item *notnull_cond;
  TABLE_STATISTICS_CB *stats_cb;

  online_alter_cache_data *online_alter_cache;

  inline void reset() { bzero((void*)this, sizeof(*this)); }
  void init(THD *thd, TABLE_LIST *tl);
  bool fill_item_list(List<Item> *item_list) const;
  void reset_item_list(List<Item> *item_list, uint skip) const;
  void clear_column_bitmaps(void);
  void prepare_for_position(void);
  MY_BITMAP *prepare_for_keyread(uint index, MY_BITMAP *map);
  MY_BITMAP *prepare_for_keyread(uint index)
  { return prepare_for_keyread(index, &tmp_set); }
  void mark_index_columns(uint index, MY_BITMAP *bitmap);
  void mark_index_columns_no_reset(uint index, MY_BITMAP *bitmap);
  void mark_index_columns_for_read(uint index);
  void restore_column_maps_after_keyread(MY_BITMAP *backup);
  void mark_auto_increment_column(void);
  void mark_columns_needed_for_update(void);
  void mark_columns_needed_for_delete(void);
  void mark_columns_needed_for_insert(void);
  void mark_columns_per_binlog_row_image(void);
  inline bool mark_column_with_deps(Field *field);
  inline bool mark_virtual_column_with_deps(Field *field);
  inline void mark_virtual_column_deps(Field *field);
  bool mark_virtual_columns_for_write(bool insert_fl);
  bool check_virtual_columns_marked_for_read();
  bool check_virtual_columns_marked_for_write();
  void mark_default_fields_for_write(bool insert_fl);
  void mark_columns_used_by_virtual_fields(void);
  void mark_check_constraint_columns_for_read(void);
  int verify_constraints(bool ignore_failure);
  void free_engine_stats();
  void update_engine_independent_stats();
  inline void column_bitmaps_set(MY_BITMAP *read_set_arg)
  {
    read_set= read_set_arg;
    if (file)
      file->column_bitmaps_signal();
  }
  inline void column_bitmaps_set(MY_BITMAP *read_set_arg,
                                 MY_BITMAP *write_set_arg)
  {
    read_set= read_set_arg;
    write_set= write_set_arg;
    if (file)
      file->column_bitmaps_signal();
  }
  inline void column_bitmaps_set_no_signal(MY_BITMAP *read_set_arg,
                                           MY_BITMAP *write_set_arg)
  {
    read_set= read_set_arg;
    write_set= write_set_arg;
  }
  inline void use_all_columns()
  {
    column_bitmaps_set(&s->all_set, &s->all_set);
  }
  inline void use_all_stored_columns();
  inline void default_column_bitmaps()
  {
    read_set= &def_read_set;
    write_set= &def_write_set;
    rpl_write_set= 0;
  }
  /** Should this instance of the table be reopened? */
  inline bool needs_reopen()
  { return !db_stat || m_needs_reopen; }
  /*
    Mark that all current connection instances of the table should be
    reopen at end of statement
  */
  void mark_table_for_reopen();
  /* Should only be called from Locked_tables_list::mark_table_for_reopen() */
  void internal_set_needs_reopen(bool value)
  {
    m_needs_reopen= value;
  }

  bool init_expr_arena(MEM_ROOT *mem_root);

  bool alloc_keys(uint key_count);
  bool check_tmp_key(uint key, uint key_parts,
                     uint (*next_field_no) (uchar *), uchar *arg);
  bool add_tmp_key(uint key, uint key_parts,
                   uint (*next_field_no) (uchar *), uchar *arg,
                   bool unique);
  void create_key_part_by_field(KEY_PART_INFO *key_part_info,
                                Field *field, uint fieldnr);
  void use_index(int key_to_save, key_map *map_to_update);
  void set_table_map(table_map map_arg, uint tablenr_arg)
  {
    map= map_arg;
    tablenr= tablenr_arg;
  }

  /// Return true if table is instantiated, and false otherwise.
  bool is_created() const
  {
    DBUG_ASSERT(!created || file != 0);
    return created;
  }

  /**
    Set the table as "created", and enable flags in storage engine
    that could not be enabled without an instantiated table.
  */
  void set_created()
  {
    if (created)
      return;
    if (file->keyread_enabled())
      file->extra(HA_EXTRA_KEYREAD);
    created= true;
  }

  void reset_created()
  {
    created= 0;
  }

  /*
    Returns TRUE if the table is filled at execution phase (and so, the
    optimizer must not do anything that depends on the contents of the table,
    like range analysis or constant table detection)
  */
  bool is_filled_at_execution();

  bool update_const_key_parts(COND *conds);

  inline void initialize_opt_range_structures();

  my_ptrdiff_t default_values_offset() const
  { return (my_ptrdiff_t) (s->default_values - record[0]); }

  void move_fields(Field **ptr, const uchar *to, const uchar *from);
  void remember_blob_values(String *blob_storage);
  void restore_blob_values(String *blob_storage);

  uint actual_n_key_parts(KEY *keyinfo);
  ulong actual_key_flags(KEY *keyinfo);
  int update_virtual_field(Field *vf, bool ignore_warnings);
  inline size_t key_storage_length(uint index)
  {
    if (is_clustering_key(index))
      return s->stored_rec_length;
    return key_info[index].key_length + file->ref_length;
  }
  int update_virtual_fields(handler *h, enum_vcol_update_mode update_mode);
  int update_default_fields(bool ignore_errors);
  void evaluate_update_default_function();
  void reset_default_fields();
  inline ha_rows stat_records() { return used_stat_records; }

  void prepare_triggers_for_insert_stmt_or_event();
  bool prepare_triggers_for_delete_stmt_or_event();
  bool prepare_triggers_for_update_stmt_or_event();

  Field **field_to_fill();
  bool validate_default_values_of_unset_fields(THD *thd) const;

  // Check if the value list is assignable to the explicit field list
  static bool check_assignability_explicit_fields(List<Item> fields,
                                                  List<Item> values,
                                                  bool ignore);
  // Check if the value list is assignable to all visible fields
  bool check_assignability_all_visible_fields(List<Item> &values,
                                              bool ignore) const;
  /*
    Check if the value list is assignable to:
    - The explicit field list if fields.elements > 0, e.g.
        INSERT INTO t1 (a,b) VALUES (1,2);
    - All visible fields, if fields.elements==0, e.g.
        INSERT INTO t1 VALUES (1,2);
  */
  bool check_assignability_opt_fields(List<Item> fields,
                                      List<Item> values,
                                      bool ignore) const
  {
    DBUG_ASSERT(values.elements);
    return fields.elements ?
           check_assignability_explicit_fields(fields, values, ignore) :
           check_assignability_all_visible_fields(values, ignore);
  }

  bool insert_all_rows_into_tmp_table(THD *thd, 
                                      TABLE *tmp_table,
                                      TMP_TABLE_PARAM *tmp_table_param,
                                      bool with_cleanup);
  bool vcol_fix_expr(THD *thd);
  bool vcol_cleanup_expr(THD *thd);
  Field *find_field_by_name(const LEX_CSTRING *str) const;
  bool export_structure(THD *thd, class Row_definition_list *defs);
  bool is_splittable() { return spl_opt_info != NULL; }
  void set_spl_opt_info(SplM_opt_info *spl_info);
  void deny_splitting();
  double get_materialization_cost(); // Now used only if is_splittable()==true
  void add_splitting_info_for_key_field(struct KEY_FIELD *key_field);

  key_map with_impossible_ranges;

  /* Number of cost info elements for possible range filters */
  uint range_rowid_filter_cost_info_elems;
  /* Pointer to the array of cost info elements for range filters */
  Range_rowid_filter_cost_info *range_rowid_filter_cost_info;
  /* The array of pointers to cost info elements for range filters */
  Range_rowid_filter_cost_info **range_rowid_filter_cost_info_ptr;

  void init_cost_info_for_usable_range_rowid_filters(THD *thd);
  void prune_range_rowid_filters();
  void trace_range_rowid_filters(THD *thd) const;
  Range_rowid_filter_cost_info *
  best_range_rowid_filter(uint access_key_no,
                          double records,
                          double fetch_cost,
                          double index_only_cost,
                          double prev_records,
                          double *records_out);
  /**
    System Versioning support
   */
  bool vers_write;

  bool versioned() const
  {
    return s->versioned;
  }

  bool versioned(vers_kind_t type) const
  {
    DBUG_ASSERT(type);
    return s->versioned == type;
  }

  bool versioned_write() const
  {
    DBUG_ASSERT(versioned() || !vers_write);
    return versioned() ? vers_write : false;
  }

  bool versioned_write(vers_kind_t type) const
  {
    DBUG_ASSERT(type);
    DBUG_ASSERT(versioned() || !vers_write);
    return versioned(type) ? vers_write : false;
  }

  Field *vers_start_field() const
  {
    DBUG_ASSERT(s->versioned);
    return field[s->vers.start_fieldno];
  }

  Field *vers_end_field() const
  {
    DBUG_ASSERT(s->versioned);
    return field[s->vers.end_fieldno];
  }

  Field *period_start_field() const
  {
    DBUG_ASSERT(s->period.name);
    return field[s->period.start_fieldno];
  }

  Field *period_end_field() const
  {
    DBUG_ASSERT(s->period.name);
    return field[s->period.end_fieldno];
  }
  inline void set_cond_selectivity(double selectivity)
  {
    DBUG_ASSERT(selectivity >= 0.0 && selectivity <= 1.0);
    cond_selectivity= selectivity;
    DBUG_PRINT("info", ("cond_selectivity: %g", cond_selectivity));
  }
  inline void multiply_cond_selectivity(double selectivity)
  {
    DBUG_ASSERT(selectivity >= 0.0 && selectivity <= 1.0);
    cond_selectivity*= selectivity;
    DBUG_PRINT("info", ("cond_selectivity: %g", cond_selectivity));
  }
  inline void set_opt_range_condition_rows(ha_rows rows)
  {
    if (opt_range_condition_rows > rows)
      opt_range_condition_rows= rows;
  }

  /* Return true if the key is a clustered key */
  inline bool is_clustering_key(uint index) const
  {
    return key_info[index].index_flags & HA_CLUSTERED_INDEX;
  }

  /*
    Return true if we can use rowid filter with this index
    rowid filter can be used if
    - filter pushdown is supported by the engine for the index. If this is set then
      file->ha_table_flags() should not contain HA_NON_COMPARABLE_ROWID!
    - The index is not a clustered primary index
  */

  inline bool can_use_rowid_filter(uint index) const
  {
    return ((key_info[index].index_flags &
             (HA_DO_RANGE_FILTER_PUSHDOWN | HA_CLUSTERED_INDEX)) ==
            HA_DO_RANGE_FILTER_PUSHDOWN);
  }

  ulonglong vers_start_id() const;
  ulonglong vers_end_id() const;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  bool vers_switch_partition(THD *thd, TABLE_LIST *table_list,
                             Open_table_context *ot_ctx);
#endif

  int update_generated_fields();
  int period_make_insert(Item *src, Field *dst);
  int insert_portion_of_time(THD *thd, const vers_select_conds_t &period_conds,
                             ha_rows *rows_inserted);
  bool vers_check_update(List<Item> &items);
  static bool check_period_overlaps(const KEY &key, const uchar *lhs, const uchar *rhs);
  int delete_row();
  /* Used in majority of DML (called from fill_record()) */
  bool vers_update_fields();
  /* Used in DELETE, DUP REPLACE and insert history row */
  void vers_update_end();
  void find_constraint_correlated_indexes();

/** Number of additional fields used in versioned tables */
#define VERSIONING_FIELDS 2
};


/**
   Helper class which specifies which members of TABLE are used for
   participation in the list of used/unused TABLE objects for the share.
*/

struct TABLE_share
{
  static inline TABLE **next_ptr(TABLE *l)
  {
    return &l->next;
  }
  static inline TABLE ***prev_ptr(TABLE *l)
  {
    return (TABLE ***) &l->prev;
  }
};

struct All_share_tables
{
  static inline TABLE **next_ptr(TABLE *l)
  {
    return &l->share_all_next;
  }
  static inline TABLE ***prev_ptr(TABLE *l)
  {
    return &l->share_all_prev;
  }
};

typedef I_P_List <TABLE, All_share_tables> All_share_tables_list;

enum enum_schema_table_state
{ 
  NOT_PROCESSED= 0,
  PROCESSED_BY_CREATE_SORT_INDEX,
  PROCESSED_BY_JOIN_EXEC
};

enum enum_fk_option { FK_OPTION_UNDEF, FK_OPTION_RESTRICT, FK_OPTION_NO_ACTION,
  FK_OPTION_CASCADE, FK_OPTION_SET_NULL, FK_OPTION_SET_DEFAULT };

typedef struct st_foreign_key_info
{
  LEX_CSTRING *foreign_id;
  LEX_CSTRING *foreign_db;
  LEX_CSTRING *foreign_table;
  LEX_CSTRING *referenced_db;
  LEX_CSTRING *referenced_table;
  enum_fk_option update_method;
  enum_fk_option delete_method;
  LEX_CSTRING *referenced_key_name;
  List<LEX_CSTRING> foreign_fields;
  List<LEX_CSTRING> referenced_fields;
} FOREIGN_KEY_INFO;

LEX_CSTRING *fk_option_name(enum_fk_option opt);
static inline bool fk_modifies_child(enum_fk_option opt)
{
  return opt >= FK_OPTION_CASCADE;
}


class IS_table_read_plan;

/*
  Types of derived tables. The ending part is a bitmap of phases that are
  applicable to a derived table of the type.
*/
#define DTYPE_ALGORITHM_UNDEFINED    0U
#define DTYPE_VIEW                   1U
#define DTYPE_TABLE                  2U
#define DTYPE_MERGE                  4U
#define DTYPE_MATERIALIZE            8U
#define DTYPE_MULTITABLE             16U
#define DTYPE_IN_PREDICATE           32U
#define DTYPE_MASK                   (DTYPE_VIEW|DTYPE_TABLE|DTYPE_MULTITABLE|DTYPE_IN_PREDICATE)

/*
  Phases of derived tables/views handling, see sql_derived.cc
  Values are used as parts of a bitmap attached to derived table types.
*/
#define DT_INIT             1U
#define DT_PREPARE          2U
#define DT_OPTIMIZE         4U
#define DT_MERGE            8U
#define DT_MERGE_FOR_INSERT 16U
#define DT_CREATE           32U
#define DT_FILL             64U
#define DT_REINIT           128U
#define DT_PHASES           8U
/* Phases that are applicable to all derived tables. */
#define DT_COMMON       (DT_INIT + DT_PREPARE + DT_REINIT + DT_OPTIMIZE)
/* Phases that are applicable only to materialized derived tables. */
#define DT_MATERIALIZE  (DT_CREATE + DT_FILL)

#define DT_PHASES_MERGE (DT_COMMON | DT_MERGE | DT_MERGE_FOR_INSERT)
#define DT_PHASES_MATERIALIZE (DT_COMMON | DT_MATERIALIZE)

#define VIEW_ALGORITHM_UNDEFINED 0
/* Special value for ALTER VIEW: inherit original algorithm. */
#define VIEW_ALGORITHM_INHERIT   DTYPE_VIEW
#define VIEW_ALGORITHM_MERGE    (DTYPE_VIEW | DTYPE_MERGE)
#define VIEW_ALGORITHM_TMPTABLE (DTYPE_VIEW | DTYPE_MATERIALIZE)

/*
  View algorithm values as stored in the FRM. Values differ from in-memory
  representation for backward compatibility.
*/

#define VIEW_ALGORITHM_UNDEFINED_FRM  0U
#define VIEW_ALGORITHM_MERGE_FRM      1U
#define VIEW_ALGORITHM_TMPTABLE_FRM   2U

#define JOIN_TYPE_LEFT	1U
#define JOIN_TYPE_RIGHT	2U
#define JOIN_TYPE_OUTER 4U	/* Marker that this is an outer join */

/* view WITH CHECK OPTION parameter options */
#define VIEW_CHECK_NONE       0
#define VIEW_CHECK_LOCAL      1
#define VIEW_CHECK_CASCADED   2

/* result of view WITH CHECK OPTION parameter check */
#define VIEW_CHECK_OK         0
#define VIEW_CHECK_ERROR      1
#define VIEW_CHECK_SKIP       2

/** The threshold size a blob field buffer before it is freed */
#define MAX_TDC_BLOB_SIZE 65536

/** number of bytes used by field positional indexes in frm */
constexpr uint frm_fieldno_size= 2;
/** number of bytes used by key position number in frm */
constexpr uint frm_keyno_size= 2;
static inline field_index_t read_frm_fieldno(const uchar *data)
{ return uint2korr(data); }
static inline void store_frm_fieldno(uchar *data, field_index_t fieldno)
{ int2store(data, fieldno); }
static inline uint16 read_frm_keyno(const uchar *data)
{ return uint2korr(data); }
static inline void store_frm_keyno(uchar *data, uint16 keyno)
{ int2store(data, keyno); }
static inline size_t extra2_str_size(size_t len)
{ return (len > 255 ? 3 : 1) + len; }

class select_unit;
class TMP_TABLE_PARAM;

Item *create_view_field(THD *thd, TABLE_LIST *view, Item **field_ref,
                        LEX_CSTRING *name);

struct Field_translator
{
  Item *item;
  LEX_CSTRING name;
};


/*
  Column reference of a NATURAL/USING join. Since column references in
  joins can be both from views and stored tables, may point to either a
  Field (for tables), or a Field_translator (for views).
*/

class Natural_join_column: public Sql_alloc
{
public:
  Field_translator *view_field;  /* Column reference of merge view. */
  Item_field       *table_field; /* Column reference of table or temp view. */
  TABLE_LIST *table_ref; /* Original base table/view reference. */
  /*
    True if a common join column of two NATURAL/USING join operands. Notice
    that when we have a hierarchy of nested NATURAL/USING joins, a column can
    be common at some level of nesting but it may not be common at higher
    levels of nesting. Thus this flag may change depending on at which level
    we are looking at some column.
  */
  bool is_common;
public:
  Natural_join_column(Field_translator *field_param, TABLE_LIST *tab);
  Natural_join_column(Item_field *field_param, TABLE_LIST *tab);
  LEX_CSTRING *name();
  Item *create_item(THD *thd);
  Field *field();
  const char *safe_table_name();
  const char *safe_db_name();
  GRANT_INFO *grant();
};


/**
   Type of table which can be open for an element of table list.
*/

enum enum_open_type
{
  OT_TEMPORARY_OR_BASE= 0, OT_TEMPORARY_ONLY, OT_BASE_ONLY
};


class SJ_MATERIALIZATION_INFO;
class Index_hint;
class Item_in_subselect;

/* trivial class, for %union in sql_yacc.yy */
struct vers_history_point_t
{
  vers_kind_t unit;
  Item *item;
};

class Vers_history_point : public vers_history_point_t
{
  void fix_item();

public:
  Vers_history_point() { empty(); }
  Vers_history_point(vers_kind_t unit_arg, Item *item_arg)
  {
    unit= unit_arg;
    item= item_arg;
    fix_item();
  }
  Vers_history_point(vers_history_point_t p)
  {
    unit= p.unit;
    item= p.item;
    fix_item();
  }
  void empty() { unit= VERS_TIMESTAMP; item= NULL; }
  void print(String *str, enum_query_type, const char *prefix, size_t plen) const;
  bool check_unit(THD *thd);
  void bad_expression_data_type_error(const char *type) const;
  bool eq(const vers_history_point_t &point) const;
};

struct vers_select_conds_t
{
  vers_system_time_t type;
  vers_system_time_t orig_type;
  bool used:1;
  bool delete_history:1;
  Vers_history_point start;
  Vers_history_point end;
  Lex_ident name;

  Item_field *field_start;
  Item_field *field_end;

  const TABLE_SHARE::period_info_t *period;

  void empty()
  {
    type= SYSTEM_TIME_UNSPECIFIED;
    orig_type= SYSTEM_TIME_UNSPECIFIED;
    used= false;
    delete_history= false;
    start.empty();
    end.empty();
  }

  void init(vers_system_time_t _type,
            Vers_history_point _start= Vers_history_point(),
            Vers_history_point _end= Vers_history_point(),
            Lex_ident          _name= "SYSTEM_TIME")
  {
    type= _type;
    orig_type= _type;
    used= false;
    delete_history= (type == SYSTEM_TIME_HISTORY ||
      type == SYSTEM_TIME_BEFORE);
    start= _start;
    end= _end;
    name= _name;
  }

  void set_all()
  {
    type= SYSTEM_TIME_ALL;
    name= "SYSTEM_TIME";
  }

  void print(String *str, enum_query_type query_type) const;

  bool init_from_sysvar(THD *thd);

  bool is_set() const
  {
    return type != SYSTEM_TIME_UNSPECIFIED;
  }
  bool check_units(THD *thd);
  bool was_set() const
  {
    return orig_type != SYSTEM_TIME_UNSPECIFIED;
  }
  bool need_setup() const
  {
    return type != SYSTEM_TIME_UNSPECIFIED && type != SYSTEM_TIME_ALL;
  }
  bool eq(const vers_select_conds_t &conds) const;
};

/*
  Table reference in the FROM clause.

  These table references can be of several types that correspond to
  different SQL elements. Below we list all types of TABLE_LISTs with
  the necessary conditions to determine when a TABLE_LIST instance
  belongs to a certain type.

  1) table (TABLE_LIST::view == NULL)
     - base table
       (TABLE_LIST::derived == NULL)
     - FROM-clause subquery - TABLE_LIST::table is a temp table
       (TABLE_LIST::derived != NULL)
     - information schema table
       (TABLE_LIST::schema_table != NULL)
       NOTICE: for schema tables TABLE_LIST::field_translation may be != NULL
  2) view (TABLE_LIST::view != NULL)
     - merge    (TABLE_LIST::effective_algorithm == VIEW_ALGORITHM_MERGE)
           also (TABLE_LIST::field_translation != NULL)
     - tmptable (TABLE_LIST::effective_algorithm == VIEW_ALGORITHM_TMPTABLE)
           also (TABLE_LIST::field_translation == NULL)
  2.5) TODO: Add derived tables description here
  3) nested table reference (TABLE_LIST::nested_join != NULL)
     - table sequence - e.g. (t1, t2, t3)
       TODO: how to distinguish from a JOIN?
     - general JOIN
       TODO: how to distinguish from a table sequence?
     - NATURAL JOIN
       (TABLE_LIST::natural_join != NULL)
       - JOIN ... USING
         (TABLE_LIST::join_using_fields != NULL)
     - semi-join nest (sj_on_expr!= NULL && sj_subq_pred!=NULL)
  4) jtbm semi-join (jtbm_subselect != NULL)
*/

/** last_leaf_for_name_resolutioning support. */

struct LEX;
class Index_hint;

/*
  @struct TABLE_CHAIN
  @brief Subchain of global chain of table references

  The structure contains a pointer to the address of the next_global
  pointer to the first TABLE_LIST objectof the subchain and the address
  of the next_global pointer to the element right after the last
  TABLE_LIST object of the subchain.  For an empty subchain both pointers
  have the same value.
*/

struct TABLE_CHAIN
{
  TABLE_CHAIN() = default;

  TABLE_LIST **start_pos;
  TABLE_LIST ** end_pos;

  void set_start_pos(TABLE_LIST **pos) { start_pos= pos; }
  void set_end_pos(TABLE_LIST **pos) { end_pos= pos; }
};

struct TABLE_LIST
{
  TABLE_LIST() = default;                          /* Remove gcc warning */

  enum prelocking_types
  {
    PRELOCK_NONE, PRELOCK_ROUTINE, PRELOCK_FK
  };

  /**
    Prepare TABLE_LIST that consists of one table instance to use in
    open_and_lock_tables
  */
  inline void reset() { bzero((void*)this, sizeof(*this)); }
  inline void init_one_table(const LEX_CSTRING *db_arg,
                             const LEX_CSTRING *table_name_arg,
                             const LEX_CSTRING *alias_arg,
                             enum thr_lock_type lock_type_arg)
  {
    enum enum_mdl_type mdl_type;
    if (lock_type_arg >= TL_FIRST_WRITE)
      mdl_type= MDL_SHARED_WRITE;
    else if (lock_type_arg == TL_READ_NO_INSERT)
      mdl_type= MDL_SHARED_NO_WRITE;
    else
      mdl_type= MDL_SHARED_READ;

    reset();
    DBUG_ASSERT(!db_arg->str || strlen(db_arg->str) == db_arg->length);
    DBUG_ASSERT(!table_name_arg->str || strlen(table_name_arg->str) == table_name_arg->length);
    DBUG_ASSERT(!alias_arg || strlen(alias_arg->str) == alias_arg->length);
    db= *db_arg;
    table_name= *table_name_arg;
    alias= (alias_arg ? *alias_arg : *table_name_arg);
    lock_type= lock_type_arg;
    updating= lock_type >= TL_FIRST_WRITE;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, db.str, table_name.str,
                     mdl_type, MDL_TRANSACTION);
  }

  TABLE_LIST(const LEX_CSTRING *db_arg,
             const LEX_CSTRING *table_name_arg,
             const LEX_CSTRING *alias_arg,
             enum thr_lock_type lock_type_arg)
  {
    init_one_table(db_arg, table_name_arg, alias_arg, lock_type_arg);
  }

  TABLE_LIST(TABLE *table_arg, thr_lock_type lock_type)
    : TABLE_LIST(&table_arg->s->db, &table_arg->s->table_name, NULL, lock_type)
  {
    DBUG_ASSERT(table_arg->s);
    table= table_arg;
    vers_conditions.name= table->s->vers.name;
  }

  inline void init_one_table_for_prelocking(const LEX_CSTRING *db_arg,
                                            const LEX_CSTRING *table_name_arg,
                                            const LEX_CSTRING *alias_arg,
                                            enum thr_lock_type lock_type_arg,
                                            prelocking_types prelocking_type,
                                            TABLE_LIST *belong_to_view_arg,
                                            uint8 trg_event_map_arg,
                                            TABLE_LIST ***last_ptr,
                                            my_bool insert_data)

  {
    init_one_table(db_arg, table_name_arg, alias_arg, lock_type_arg);
    cacheable_table= 1;
    prelocking_placeholder= prelocking_type;
    open_type= (prelocking_type == PRELOCK_ROUTINE ?
                OT_TEMPORARY_OR_BASE :
                OT_BASE_ONLY);
    belong_to_view= belong_to_view_arg;
    trg_event_map= trg_event_map_arg;
    /* MDL is enough for read-only FK checks, we don't need the table */
    if (prelocking_type == PRELOCK_FK && lock_type < TL_FIRST_WRITE)
      open_strategy= OPEN_STUB;

    **last_ptr= this;
    prev_global= *last_ptr;
    *last_ptr= &next_global;
    for_insert_data= insert_data;
  }


  /*
    List of tables local to a subquery (used by SQL_I_List). Considers
    views as leaves (unlike 'next_leaf' below). Created at parse time
    in st_select_lex::add_table_to_list() -> table_list.link_in_list().
  */
  TABLE_LIST *next_local;
  /* link in a global list of all queries tables */
  TABLE_LIST *next_global, **prev_global;
  LEX_CSTRING   db;
  LEX_CSTRING   table_name;
  LEX_CSTRING   schema_table_name;
  LEX_CSTRING   alias;
  const char    *option;                /* Used by cache index  */
  Item		*on_expr;		/* Used with outer join */
  Name_resolution_context *on_context;  /* For ON expressions */
  Table_function_json_table *table_function; /* If it's the table function. */

  Item          *sj_on_expr;
  /*
    (Valid only for semi-join nests) Bitmap of tables that are within the
    semi-join (this is different from bitmap of all nest's children because
    tables that were pulled out of the semi-join nest remain listed as
    nest's children).
  */
  table_map     sj_inner_tables;
  /* Number of IN-compared expressions */
  uint          sj_in_exprs;
  
  /* If this is a non-jtbm semi-join nest: corresponding subselect predicate */
  Item_in_subselect  *sj_subq_pred;

  table_map     original_subq_pred_used_tables;

  /* If this is a jtbm semi-join object: corresponding subselect predicate */
  Item_in_subselect  *jtbm_subselect;
  /* TODO: check if this can be joined with tablenr_exec */
  uint jtbm_table_no;

  SJ_MATERIALIZATION_INFO *sj_mat_info;

  /*
    The structure of ON expression presented in the member above
    can be changed during certain optimizations. This member
    contains a snapshot of AND-OR structure of the ON expression
    made after permanent transformations of the parse tree, and is
    used to restore ON clause before every reexecution of a prepared
    statement or stored procedure.
  */
  Item          *prep_on_expr;
  COND_EQUAL    *cond_equal;            /* Used with outer join */
  /*
    During parsing - left operand of NATURAL/USING join where 'this' is
    the right operand. After parsing (this->natural_join == this) iff
    'this' represents a NATURAL or USING join operation. Thus after
    parsing 'this' is a NATURAL/USING join iff (natural_join != NULL).
  */
  TABLE_LIST *natural_join;
  /*
    True if 'this' represents a nested join that is a NATURAL JOIN.
    For one of the operands of 'this', the member 'natural_join' points
    to the other operand of 'this'.
  */
  bool is_natural_join;
  /* Field names in a USING clause for JOIN ... USING. */
  List<String> *join_using_fields;
  /*
    Explicitly store the result columns of either a NATURAL/USING join or
    an operand of such a join.
  */
  List<Natural_join_column> *join_columns;
  /* TRUE if join_columns contains all columns of this table reference. */
  bool is_join_columns_complete;

  /*
    List of nodes in a nested join tree, that should be considered as
    leaves with respect to name resolution. The leaves are: views,
    top-most nodes representing NATURAL/USING joins, subqueries, and
    base tables. All of these TABLE_LIST instances contain a
    materialized list of columns. The list is local to a subquery.
  */
  TABLE_LIST *next_name_resolution_table;
  /* Index names in a "... JOIN ... USE/IGNORE INDEX ..." clause. */
  List<Index_hint> *index_hints;
  TABLE        *table;                          /* opened table */
  ulonglong         table_id; /* table id (from binlog) for opened table */
  /*
    select_result for derived table to pass it from table creation to table
    filling procedure
  */
  select_unit  *derived_result;
  /* Stub used for materialized derived tables. */
  bool delete_while_scanning;
  table_map	map;                    /* ID bit of table (1,2,4,8,16...) */
  table_map get_map()
  {
    return jtbm_subselect? table_map(1) << jtbm_table_no : table->map;
  }
  uint get_tablenr()
  {
    return jtbm_subselect? jtbm_table_no : table->tablenr;
  }
  void set_tablenr(uint new_tablenr)
  {
    if (jtbm_subselect)
    {
      jtbm_table_no= new_tablenr;
    }
    if (table)
    {
      table->tablenr= new_tablenr;
      table->map= table_map(1) << new_tablenr;
    }
  }
  /*
    Reference from aux_tables to local list entry of main select of
    multi-delete statement:
    delete t1 from t2,t1 where t1.a<'B' and t2.b=t1.b;
    here it will be reference of first occurrence of t1 to second (as you
    can see this lists can't be merged)
  */
  TABLE_LIST	*correspondent_table;
  /**
     @brief Normally, this field is non-null for anonymous derived tables only.

     @details This field is set to non-null for 
     
     - Anonymous derived tables, In this case it points to the SELECT_LEX_UNIT
     representing the derived table. E.g. for a query
     
     @verbatim SELECT * FROM (SELECT a FROM t1) b @endverbatim
     
     For the @c TABLE_LIST representing the derived table @c b, @c derived
     points to the SELECT_LEX_UNIT representing the result of the query within
     parenteses.
     
     - Views. This is set for views with @verbatim ALGORITHM = TEMPTABLE
     @endverbatim by mysql_make_view().
     
     @note Inside views, a subquery in the @c FROM clause is not allowed.
     @note Do not use this field to separate views/base tables/anonymous
     derived tables. Use TABLE_LIST::is_anonymous_derived_table().
  */
  st_select_lex_unit *derived;		/* SELECT_LEX_UNIT of derived table */
  With_element *with;          /* With element defining this table (if any) */
  /* Bitmap of the defining with element */
  table_map with_internal_reference_map;
  TABLE_LIST * next_with_rec_ref;
  bool is_derived_with_recursive_reference;
  bool block_handle_derived;
  /* The interface employed to materialize the table by a foreign engine */
  derived_handler *dt_handler;
  /*
    The object used to organize execution of the query that specifies
    the derived table by a foreign engine
  */
  Pushdown_derived *pushdown_derived;
  ST_SCHEMA_TABLE *schema_table;        /* Information_schema table */
  st_select_lex	*schema_select_lex;
  /*
    True when the view field translation table is used to convert
    schema table fields for backwards compatibility with SHOW command.
  */
  bool schema_table_reformed;
  TMP_TABLE_PARAM *schema_table_param;
  /* link to select_lex where this table was used */
  st_select_lex	*select_lex;
  LEX *view;                    /* link on VIEW lex for merging */
  Field_translator *field_translation;	/* array of VIEW fields */
  /* pointer to element after last one in translation table above */
  Field_translator *field_translation_end;
  bool field_translation_updated;
  /*
    List (based on next_local) of underlying tables of this view. I.e. it
    does not include the tables of subqueries used in the view. Is set only
    for merged views.
  */
  TABLE_LIST	*merge_underlying_list;
  /*
    - 0 for base tables
    - in case of the view it is the list of all (not only underlying
    tables but also used in subquery ones) tables of the view.
  */
  List<TABLE_LIST> *view_tables;
  /* most upper view this table belongs to */
  TABLE_LIST	*belong_to_view;
  /* A derived table this table belongs to */
  TABLE_LIST    *belong_to_derived;
  /*
    The view directly referencing this table
    (non-zero only for merged underlying tables of a view).
  */
  TABLE_LIST	*referencing_view;

  table_map view_used_tables;
  table_map     map_exec;
  /* TODO: check if this can be joined with jtbm_table_no */
  uint          tablenr_exec;
  uint          maybe_null_exec;

  /* Ptr to parent MERGE table list item. See top comment in ha_myisammrg.cc */
  TABLE_LIST    *parent_l;
  /*
    Security  context (non-zero only for tables which belong
    to view with SQL SECURITY DEFINER)
  */
  Security_context *security_ctx;
  uchar tabledef_version_buf[MY_UUID_SIZE >
                               MICROSECOND_TIMESTAMP_BUFFER_SIZE-1 ?
                             MY_UUID_SIZE + 1 :
                             MICROSECOND_TIMESTAMP_BUFFER_SIZE];
  LEX_CUSTRING tabledef_version;

  /*
    This view security context (non-zero only for views with
    SQL SECURITY DEFINER)
  */
  Security_context *view_sctx;
  bool allowed_show;
  Item          *where;                 /* VIEW WHERE clause condition */
  Item          *check_option;          /* WITH CHECK OPTION condition */
  LEX_STRING	select_stmt;		/* text of (CREATE/SELECT) statement */
  LEX_CSTRING	md5;			/* md5 of query text */
  LEX_CSTRING	source;			/* source of CREATE VIEW */
  LEX_CSTRING	view_db;		/* saved view database */
  LEX_CSTRING	view_name;		/* saved view name */
  LEX_STRING	hr_timestamp;           /* time stamp of last operation */
  LEX_USER      definer;                /* definer of view */
  ulonglong	file_version;		/* version of file's field set */
  ulonglong	mariadb_version;	/* version of server on creation */
  ulonglong     updatable_view;         /* VIEW can be updated */
  /** 
      @brief The declared algorithm, if this is a view.
      @details One of
      - VIEW_ALGORITHM_UNDEFINED
      - VIEW_ALGORITHM_TMPTABLE
      - VIEW_ALGORITHM_MERGE
      @to do Replace with an enum 
  */
  ulonglong	algorithm;
  ulonglong     view_suid;              /* view is suid (TRUE dy default) */
  ulonglong     with_check;             /* WITH CHECK OPTION */
  /*
    effective value of WITH CHECK OPTION (differ for temporary table
    algorithm)
  */
  uint8         effective_with_check;
  /** 
      @brief The view algorithm that is actually used, if this is a view.
      @details One of
      - VIEW_ALGORITHM_UNDEFINED
      - VIEW_ALGORITHM_TMPTABLE
      - VIEW_ALGORITHM_MERGE
      @to do Replace with an enum 
  */
  uint8         derived_type;
  GRANT_INFO	grant;
  /* data need by some engines in query cache*/
  ulonglong     engine_data;
  /* call back function for asking handler about caching in query cache */
  qc_engine_callback callback_func;
  thr_lock_type lock_type;

  /*
    Two fields below are set during parsing this table reference in the cases
    when the table reference can be potentially a reference to a CTE table.
    In this cases the fact that the reference is a reference to a CTE or not
    will be ascertained at the very end of parsing of the query when referencies
    to CTE are resolved. For references to CTE and to derived tables no mdl
    requests are needed while for other table references they are. If a request
    is possibly postponed the info that allows to issue this request must be
    saved in 'mdl_type' and 'table_options'.
  */
  enum_mdl_type mdl_type;
  ulong         table_options;

  uint		outer_join;		/* Which join type */
  uint		shared;			/* Used in multi-upd */
  bool          updatable;		/* VIEW/TABLE can be updated now */
  bool          straight;		/* optimize with prev table */
  bool          updating;               /* for replicate-do/ignore table */
  bool          ignore_leaves;          /* preload only non-leaf nodes */
  bool          crashed;                /* Table was found crashed */
  bool          skip_locked;            /* Skip locked in view defination */
  table_map     dep_tables;             /* tables the table depends on      */
  table_map     on_expr_dep_tables;     /* tables on expression depends on  */
  struct st_nested_join *nested_join;   /* if the element is a nested join  */
  TABLE_LIST *embedding;             /* nested join containing the table */
  List<TABLE_LIST> *join_list;/* join list the table belongs to   */
  bool          lifted;               /* set to true when the table is moved to
                                         the upper level at the parsing stage */
  bool		cacheable_table;	/* stop PS caching */
  /* used in multi-upd/views privilege check */
  bool		table_in_first_from_clause;
  /**
     Specifies which kind of table should be open for this element
     of table list.
  */
  enum enum_open_type open_type;
  /* TRUE if this merged view contain auto_increment field */
  bool          contain_auto_increment;
  bool          compact_view_format;    /* Use compact format for SHOW CREATE VIEW */
  /* view where processed */
  bool          where_processed;
  /* TRUE <=> VIEW CHECK OPTION expression has been processed */
  bool          check_option_processed;
  /* TABLE_TYPE_UNKNOWN if any type is acceptable */
  Table_type    required_type;
  handlerton	*db_type;		/* table_type for handler */
  char		timestamp_buffer[MICROSECOND_TIMESTAMP_BUFFER_SIZE];
  /*
    This TABLE_LIST object is just placeholder for prelocking, it will be
    used for implicit LOCK TABLES only and won't be used in real statement.
  */
  prelocking_types prelocking_placeholder;
  /**
     Indicates that if TABLE_LIST object corresponds to the table/view
     which requires special handling.
  */
  enum enum_open_strategy
  {
    /* Normal open. */
    OPEN_NORMAL= 0,
    /* Associate a table share only if the the table exists. */
    OPEN_IF_EXISTS,
    /* Don't associate a table share. */
    OPEN_STUB
  } open_strategy;
  /** TRUE if an alias for this table was specified in the SQL. */
  bool          is_alias;
  /** TRUE if the table is referred to in the statement using a fully
      qualified name (<db_name>.<table_name>).
  */
  bool          is_fqtn;

  /* TRUE <=> derived table should be filled right after optimization. */
  bool          fill_me;
  /* TRUE <=> view/DT is merged. */
  /* TODO: replace with derived_type */
  bool          merged;
  bool          merged_for_insert;
  bool          sequence;  /* Part of NEXTVAL/CURVAL/LASTVAL */
  /*
      Protect single thread from repeating partition auto-create over
      multiple share instances (as the share is closed on backoff action).

      Skips auto-create only for one given query id.
  */
  query_id_t    vers_skip_create;

  /*
    Items created by create_view_field and collected to change them in case
    of materialization of the view/derived table
  */
  List<Item>    used_items;
  /* Sublist (tail) of persistent used_items */
  List<Item>    persistent_used_items;

  /* View creation context. */

  View_creation_ctx *view_creation_ctx;

  /*
    Attributes to save/load view creation context in/from frm-file.

    Ther are required only to be able to use existing parser to load
    view-definition file. As soon as the parser parsed the file, view
    creation context is initialized and the attributes become redundant.

    These attributes MUST NOT be used for any purposes but the parsing.
  */

  LEX_CSTRING view_client_cs_name;
  LEX_CSTRING view_connection_cl_name;

  /*
    View definition (SELECT-statement) in the UTF-form.
  */

  LEX_CSTRING view_body_utf8;

   /* End of view definition context. */

  /**
    Indicates what triggers we need to pre-load for this TABLE_LIST
    when opening an associated TABLE. This is filled after
    the parsed tree is created.

    slave_fk_event_map is filled on the slave side with bitmaps value
    representing row-based event operation to help find and prelock
    possible FK constrain-related child tables.
  */
  uint8 trg_event_map, slave_fk_event_map;
  /* TRUE <=> this table is a const one and was optimized away. */
  bool optimized_away;

  /**
    TRUE <=> already materialized. Valid only for materialized derived
    tables/views.
  */
  bool materialized;
  /* I_S: Flags to open_table (e.g. OPEN_TABLE_ONLY or OPEN_VIEW_ONLY) */
  uint i_s_requested_object;

  bool prohibit_cond_pushdown;

  /*
    I_S: how to read the tables (SKIP_OPEN_TABLE/OPEN_FRM_ONLY/OPEN_FULL_TABLE)
  */
  uint table_open_method;
  /*
    I_S: where the schema table was filled
    (this is a hack. The code should be able to figure out whether reading
    from I_S should be done by create_sort_index() or by JOIN::exec.)
  */
  enum enum_schema_table_state schema_table_state;

  /* Something like a "query plan" for reading INFORMATION_SCHEMA table */
  IS_table_read_plan *is_table_read_plan;

  MDL_request mdl_request;

#ifdef WITH_PARTITION_STORAGE_ENGINE
  /* List to carry partition names from PARTITION (...) clause in statement */
  List<String> *partition_names;
#endif /* WITH_PARTITION_STORAGE_ENGINE */

  void calc_md5(char *buffer);
  int view_check_option(THD *thd, bool ignore_failure);
  bool create_field_translation(THD *thd);
  bool setup_underlying(THD *thd);
  void cleanup_items();
  bool placeholder()
  {
    return derived || view || schema_table || !table || table_function;
  }
  void print(THD *thd, table_map eliminated_tables, String *str, 
             enum_query_type query_type);
  void print_leaf_tables(THD *thd, String *str,
                         enum_query_type query_type);
  bool check_single_table(TABLE_LIST **table, table_map map,
                          TABLE_LIST *view);
  bool set_insert_values(MEM_ROOT *mem_root);
  void hide_view_error(THD *thd);
  TABLE_LIST *find_underlying_table(TABLE *table);
  TABLE_LIST *first_leaf_for_name_resolution();
  TABLE_LIST *last_leaf_for_name_resolution();

  /* System Versioning */
  vers_select_conds_t vers_conditions;
  vers_select_conds_t period_conditions;

  bool has_period() const
  {
    return period_conditions.is_set();
  }

  my_bool for_insert_data;

  /**
     @brief
       Find the bottom in the chain of embedded table VIEWs.

     @detail
       This is used for single-table UPDATE/DELETE when they are modifying a
       single-table VIEW.
  */
  TABLE_LIST *find_table_for_update()
  {
    TABLE_LIST *tbl= this;
    while(!tbl->is_multitable() && tbl->single_table_updatable() &&
        tbl->merge_underlying_list)
    {
      tbl= tbl->merge_underlying_list;
    }
    return tbl;
  }
  TABLE *get_real_join_table();
  bool is_leaf_for_name_resolution();
  inline TABLE_LIST *top_table()
    { return belong_to_view ? belong_to_view : this; }
  inline bool prepare_check_option(THD *thd)
  {
    bool res= FALSE;
    if (effective_with_check)
      res= prep_check_option(thd, effective_with_check);
    return res;
  }
  inline bool prepare_where(THD *thd, Item **conds,
                            bool no_where_clause)
  {
    if (!view || is_merged_derived())
      return prep_where(thd, conds, no_where_clause);
    return FALSE;
  }

  void register_want_access(privilege_t want_access);
  bool prepare_security(THD *thd);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *find_view_security_context(THD *thd);
  bool prepare_view_security_context(THD *thd, bool upgrade_check);
#endif
  /*
    Cleanup for re-execution in a prepared statement or a stored
    procedure.
  */
  void reinit_before_use(THD *thd);
  Item_subselect *containing_subselect();

  /* 
    Compiles the tagged hints list and fills up TABLE::keys_in_use_for_query,
    TABLE::keys_in_use_for_group_by, TABLE::keys_in_use_for_order_by,
    TABLE::force_index and TABLE::covering_keys.
  */
  bool process_index_hints(TABLE *table);

  bool is_the_same_definition(THD *thd, TABLE_SHARE *s);
  /**
    Record the value of metadata version of the corresponding
    table definition cache element in this parse tree node.

    @sa check_and_update_table_version()
  */
  inline void set_table_ref_id(TABLE_SHARE *s)
  { set_table_ref_id(s->get_table_ref_type(), s->get_table_ref_version()); }

  inline void set_table_ref_id(enum_table_ref_type table_ref_type_arg,
                        ulong table_ref_version_arg)
  {
    m_table_ref_type= table_ref_type_arg;
    m_table_ref_version= table_ref_version_arg;
  }

  void set_table_id(TABLE_SHARE *s)
  {
    set_table_ref_id(s);
    set_tabledef_version(s);
  }

  void set_tabledef_version(TABLE_SHARE *s)
  {
    if (!tabledef_version.length && s->tabledef_version.length)
    {
      DBUG_ASSERT(s->tabledef_version.length <
                  sizeof(tabledef_version_buf));
      tabledef_version.str= tabledef_version_buf;
      memcpy(tabledef_version_buf, s->tabledef_version.str,
             (tabledef_version.length= s->tabledef_version.length));
      // safety
      tabledef_version_buf[tabledef_version.length]= 0;
    }
  }

  /* Set of functions returning/setting state of a derived table/view. */
  bool is_non_derived() const { return (!derived_type); }
  bool is_view_or_derived() const { return derived_type; }
  bool is_view() const { return (derived_type & DTYPE_VIEW); }
  bool is_derived() const { return (derived_type & DTYPE_TABLE); }
  bool is_with_table();
  bool is_recursive_with_table();
  bool is_with_table_recursive_reference();
  void register_as_derived_with_rec_ref(With_element *rec_elem);
  bool is_nonrecursive_derived_with_rec_ref();
  bool fill_recursive(THD *thd);

  inline void set_view()
  {
    derived_type= DTYPE_VIEW;
  }
  inline void set_derived()
  {
    derived_type= DTYPE_TABLE;
  }
  bool is_merged_derived() const { return (derived_type & DTYPE_MERGE); }
  inline void set_merged_derived()
  {
    DBUG_ENTER("set_merged_derived");
    DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                        (alias.str ? alias.str : "<NULL>"),
                         get_unit()));
    derived_type= static_cast<uint8>((derived_type & DTYPE_MASK) | DTYPE_MERGE);
    set_check_merged();
    DBUG_VOID_RETURN;
  }
  bool is_materialized_derived() const
  {
    return (derived_type & DTYPE_MATERIALIZE);
  }
  void set_materialized_derived()
  {
    DBUG_ENTER("set_materialized_derived");
    DBUG_PRINT("enter", ("Alias: '%s'  Unit: %p",
                        (alias.str ? alias.str : "<NULL>"),
                         get_unit()));
    derived_type= static_cast<uint8>((derived_type &
                                      (derived ? DTYPE_MASK : DTYPE_VIEW)) |
                                     DTYPE_MATERIALIZE);
    set_check_materialized();
    DBUG_VOID_RETURN;
  }
  bool is_multitable() const { return (derived_type & DTYPE_MULTITABLE); }
  inline void set_multitable()
  {
    derived_type|= DTYPE_MULTITABLE;
  }
  bool set_as_with_table(THD *thd, With_element *with_elem);
  void reset_const_table();
  bool handle_derived(LEX *lex, uint phases);

  /**
     @brief True if this TABLE_LIST represents an anonymous derived table,
     i.e.  the result of a subquery.
  */
  bool is_anonymous_derived_table() const { return derived && !view; }

  /**
     @brief Returns the name of the database that the referenced table belongs
     to.
  */
  const char *get_db_name() const { return view != NULL ? view_db.str : db.str; }

  /**
     @brief Returns the name of the table that this TABLE_LIST represents.

     @details The unqualified table name or view name for a table or view,
     respectively.
   */
  const char *get_table_name() const { return view != NULL ? view_name.str : table_name.str; }
  bool is_active_sjm();
  bool is_sjm_scan_table();
  bool is_jtbm() { return MY_TEST(jtbm_subselect != NULL); }
  st_select_lex_unit *get_unit();
  st_select_lex *get_single_select();
  void wrap_into_nested_join(List<TABLE_LIST> &join_list);
  bool init_derived(THD *thd, bool init_view);
  int fetch_number_of_rows();
  bool change_refs_to_fields();

  bool single_table_updatable();

  bool is_inner_table_of_outer_join()
  {
    for (TABLE_LIST *tbl= this; tbl; tbl= tbl->embedding)
    {
      if (tbl->outer_join)
        return true;
    }
    return false;
  } 
  void set_lock_type(THD* thd, enum thr_lock_type lock);

  derived_handler *find_derived_handler(THD *thd);
  TABLE_LIST *get_first_table();

  void remove_join_columns()
  {
    if (join_columns)
    {
      join_columns->empty();
      join_columns= NULL;
      is_join_columns_complete= FALSE;
    }
  }

  inline void set_view_def_version(LEX_STRING *version)
  {
    m_table_ref_type= TABLE_REF_VIEW;
    tabledef_version.str= (const uchar *) version->str;
    tabledef_version.length= version->length;
  }
private:
  bool prep_check_option(THD *thd, uint8 check_opt_type);
  bool prep_where(THD *thd, Item **conds, bool no_where_clause);
  void set_check_materialized();
#ifndef DBUG_OFF
  void set_check_merged();
#else
  inline void set_check_merged() {}
#endif
  /** See comments for set_table_ref_id() */
  enum enum_table_ref_type m_table_ref_type;
  /** See comments for set_table_ref_id() */
  ulong m_table_ref_version;
};

class Item;

/*
  Iterator over the fields of a generic table reference.
*/

class Field_iterator: public Sql_alloc
{
public:
  Field_iterator() = default;                         /* Remove gcc warning */
  virtual ~Field_iterator() = default;
  virtual void set(TABLE_LIST *)= 0;
  virtual void next()= 0;
  virtual bool end_of_fields()= 0;              /* Return 1 at end of list */
  virtual LEX_CSTRING *name()= 0;
  virtual Item *create_item(THD *)= 0;
  virtual Field *field()= 0;
};


/* 
  Iterator over the fields of a base table, view with temporary
  table, or subquery.
*/

class Field_iterator_table: public Field_iterator
{
  Field **ptr;
public:
  Field_iterator_table() :ptr(0) {}
  void set(TABLE_LIST *table) { ptr= table->table->field; }
  void set_table(TABLE *table) { ptr= table->field; }
  void next() { ptr++; }
  bool end_of_fields() { return *ptr == 0; }
  LEX_CSTRING *name();
  Item *create_item(THD *thd);
  Field *field() { return *ptr; }
};


/* Iterator over the fields of a merge view. */

class Field_iterator_view: public Field_iterator
{
  Field_translator *ptr, *array_end;
  TABLE_LIST *view;
public:
  Field_iterator_view() :ptr(0), array_end(0) {}
  void set(TABLE_LIST *table);
  void next() { ptr++; }
  bool end_of_fields() { return ptr == array_end; }
  LEX_CSTRING *name();
  Item *create_item(THD *thd);
  Item **item_ptr() {return &ptr->item; }
  Field *field() { return 0; }
  inline Item *item() { return ptr->item; }
  Field_translator *field_translator() { return ptr; }
};


/*
  Field_iterator interface to the list of materialized fields of a
  NATURAL/USING join.
*/

class Field_iterator_natural_join: public Field_iterator
{
  List_iterator_fast<Natural_join_column> column_ref_it;
  Natural_join_column *cur_column_ref;
public:
  Field_iterator_natural_join() :cur_column_ref(NULL) {}
  ~Field_iterator_natural_join() = default;
  void set(TABLE_LIST *table);
  void next();
  bool end_of_fields() { return !cur_column_ref; }
  LEX_CSTRING *name() { return cur_column_ref->name(); }
  Item *create_item(THD *thd) { return cur_column_ref->create_item(thd); }
  Field *field() { return cur_column_ref->field(); }
  Natural_join_column *column_ref() { return cur_column_ref; }
};


/*
  Generic iterator over the fields of an arbitrary table reference.

  DESCRIPTION
    This class unifies the various ways of iterating over the columns
    of a table reference depending on the type of SQL entity it
    represents. If such an entity represents a nested table reference,
    this iterator encapsulates the iteration over the columns of the
    members of the table reference.

  IMPLEMENTATION
    The implementation assumes that all underlying NATURAL/USING table
    references already contain their result columns and are linked into
    the list TABLE_LIST::next_name_resolution_table.
*/

class Field_iterator_table_ref: public Field_iterator
{
  TABLE_LIST *table_ref, *first_leaf, *last_leaf;
  Field_iterator_table        table_field_it;
  Field_iterator_view         view_field_it;
  Field_iterator_natural_join natural_join_it;
  Field_iterator *field_it;
  void set_field_iterator();
public:
  Field_iterator_table_ref() :field_it(NULL) {}
  void set(TABLE_LIST *table);
  void next();
  bool end_of_fields()
  { return (table_ref == last_leaf && field_it->end_of_fields()); }
  LEX_CSTRING *name() { return field_it->name(); }
  const char *get_table_name();
  const char *get_db_name();
  GRANT_INFO *grant();
  Item *create_item(THD *thd) { return field_it->create_item(thd); }
  Field *field() { return field_it->field(); }
  Natural_join_column *get_or_create_column_ref(THD *thd, TABLE_LIST *parent_table_ref);
  Natural_join_column *get_natural_column_ref();
};


#define JOIN_OP_NEST       1
#define REBALANCED_NEST    2

typedef struct st_nested_join
{
  List<TABLE_LIST>  join_list;       /* list of elements in the nested join */
  /*
    Currently the valid values for nest type are:
    JOIN_OP_NEST - for nest created for JOIN operation used as an operand in
    a join expression, contains 2 elements;
    JOIN_OP_NEST | REBALANCED_NEST -  nest created after tree re-balancing
    in st_select_lex::add_cross_joined_table(), contains 1 element;
    0 - for all other nests.
    Examples:
    1.  SELECT * FROM t1 JOIN t2 LEFT JOIN t3 ON t2.a=t3.a;
    Here the nest created for LEFT JOIN at first has nest_type==JOIN_OP_NEST.
    After re-balancing in st_select_lex::add_cross_joined_table() this nest
    has nest_type==JOIN_OP_NEST | REBALANCED_NEST. The nest for JOIN created
    in st_select_lex::add_cross_joined_table() has nest_type== JOIN_OP_NEST.
    2.  SELECT * FROM t1 JOIN (t2 LEFT JOIN t3 ON t2.a=t3.a)
    Here the nest created for LEFT JOIN has nest_type==0, because it's not
    an operand in a join expression. The nest created for JOIN has nest_type
    set to JOIN_OP_NEST.
  */
  uint nest_type;
  /* 
    Bitmap of tables within this nested join (including those embedded within
    its children), including tables removed by table elimination.
  */
  table_map         used_tables;
  table_map         not_null_tables; /* tables that rejects nulls           */
  /**
    Used for pointing out the first table in the plan being covered by this
    join nest. It is used exclusively within make_outerjoin_info().
   */
  struct st_join_table *first_nested;
  /* 
    Used to count tables in the nested join in 2 isolated places:
    1. In make_outerjoin_info(). 
    2. check_interleaving_with_nj/restore_prev_nj_state (these are called
       by the join optimizer. 
    Before each use the counters are zeroed by reset_nj_counters.
  */
  uint              counter;

  /*
    Number of elements in join_list that participate in the join plan choice:
    - Base tables that were not removed by table elimination
    - Join nests that were not removed by mark_join_nest_as_const
  */
  uint              n_tables;
  nested_join_map   nj_map;          /* Bit used to identify this nested join*/
  /*
    (Valid only for semi-join nests) Bitmap of tables outside the semi-join
    that are used within the semi-join's ON condition.
  */
  table_map         sj_depends_on;
  /* Outer non-trivially correlated tables */
  table_map         sj_corr_tables;
  table_map         direct_children_map;
  List<Item_ptr>    sj_outer_expr_list;
  /**
     True if this join nest node is completely covered by the query execution
     plan. This means two things.

     1. All tables on its @c join_list are covered by the plan.

     2. All child join nest nodes are fully covered.
   */
  bool is_fully_covered() const { return n_tables == counter; }
} NESTED_JOIN;


typedef struct st_changed_table_list
{
  struct	st_changed_table_list *next;
  char		*key;
  size_t  key_length;
} CHANGED_TABLE_LIST;


typedef struct st_open_table_list{
  struct st_open_table_list *next;
  char	*db,*table;
  uint32 in_use,locked;
} OPEN_TABLE_LIST;


static inline MY_BITMAP *tmp_use_all_columns(TABLE *table,
                                             MY_BITMAP **bitmap)
{
  MY_BITMAP *old= *bitmap;
  *bitmap= &table->s->all_set;
  return old;
}


static inline void tmp_restore_column_map(MY_BITMAP **bitmap,
                                          MY_BITMAP *old)
{
  *bitmap= old;
}

/* The following is only needed for debugging */

static inline MY_BITMAP *dbug_tmp_use_all_columns(TABLE *table,
                                                      MY_BITMAP **bitmap)
{
#ifdef DBUG_ASSERT_EXISTS
  return tmp_use_all_columns(table, bitmap);
#else
  return 0;
#endif
}

static inline void dbug_tmp_restore_column_map(MY_BITMAP **bitmap,
                                               MY_BITMAP *old)
{
#ifdef DBUG_ASSERT_EXISTS
  tmp_restore_column_map(bitmap, old);
#endif
}


/* 
  Variant of the above : handle both read and write sets.
  Provide for the possiblity of the read set being the same as the write set
*/
static inline void dbug_tmp_use_all_columns(TABLE *table,
                                            MY_BITMAP **save,
                                            MY_BITMAP **read_set,
                                            MY_BITMAP **write_set)
{
#ifdef DBUG_ASSERT_EXISTS
  save[0]= *read_set;
  save[1]= *write_set;
  (void) tmp_use_all_columns(table, read_set);
  (void) tmp_use_all_columns(table, write_set);
#endif
}


static inline void dbug_tmp_restore_column_maps(MY_BITMAP **read_set,
                                                MY_BITMAP **write_set,
                                                MY_BITMAP **old)
{
#ifdef DBUG_ASSERT_EXISTS
  tmp_restore_column_map(read_set, old[0]);
  tmp_restore_column_map(write_set, old[1]);
#endif
}

bool ok_for_lower_case_names(const char *names);

enum get_table_share_flags {
  GTS_TABLE                = 1,
  GTS_VIEW                 = 2,
  GTS_NOLOCK               = 4,
  GTS_USE_DISCOVERY        = 8,
  GTS_FORCE_DISCOVERY      = 16
};

size_t max_row_length(TABLE *table, MY_BITMAP const *cols, const uchar *data);

void init_mdl_requests(TABLE_LIST *table_list);

enum open_frm_error open_table_from_share(THD *thd, TABLE_SHARE *share,
                       const LEX_CSTRING *alias, uint db_stat, uint prgflag,
                       uint ha_open_flags, TABLE *outparam,
                       bool is_create_table,
                       List<String> *partitions_to_open= NULL);
bool copy_keys_from_share(TABLE *outparam, MEM_ROOT *root);
bool parse_vcol_defs(THD *thd, MEM_ROOT *mem_root, TABLE *table,
                     bool *error_reported, vcol_init_mode expr);
TABLE_SHARE *alloc_table_share(const char *db, const char *table_name,
                               const char *key, uint key_length);
void init_tmp_table_share(THD *thd, TABLE_SHARE *share, const char *key,
                          uint key_length,
                          const char *table_name, const char *path);
void free_table_share(TABLE_SHARE *share);
enum open_frm_error open_table_def(THD *thd, TABLE_SHARE *share,
                                   uint flags = GTS_TABLE);

void open_table_error(TABLE_SHARE *share, enum open_frm_error error,
                      int db_errno);
void update_create_info_from_table(HA_CREATE_INFO *info, TABLE *form);
bool check_db_name(LEX_STRING *db);
bool check_column_name(const char *name);
bool check_period_name(const char *name);
bool check_table_name(const char *name, size_t length, bool check_for_path_chars);
int rename_file_ext(const char * from,const char * to,const char * ext);
char *get_field(MEM_ROOT *mem, Field *field);
bool get_field(MEM_ROOT *mem, Field *field, class String *res);

bool validate_comment_length(THD *thd, LEX_CSTRING *comment, size_t max_len,
                             uint err_code, const char *name);

int closefrm(TABLE *table);
void free_blobs(TABLE *table);
void free_field_buffers_larger_than(TABLE *table, uint32 size);
ulong get_form_pos(File file, uchar *head, TYPELIB *save_names);
void append_unescaped(String *res, const char *pos, size_t length);
void prepare_frm_header(THD *thd, uint reclength, uchar *fileinfo,
                        HA_CREATE_INFO *create_info, uint keys, KEY *key_info);
const char *fn_frm_ext(const char *name);

/* Check that the integer is in the internal */
static inline int set_zone(int nr,int min_zone,int max_zone)
{
  if (nr <= min_zone)
    return min_zone;
  if (nr >= max_zone)
    return max_zone;
  return nr;
}

/* performance schema */
extern LEX_CSTRING PERFORMANCE_SCHEMA_DB_NAME;

extern LEX_CSTRING GENERAL_LOG_NAME;
extern LEX_CSTRING SLOW_LOG_NAME;
extern LEX_CSTRING TRANSACTION_REG_NAME;

/* information schema */
extern LEX_CSTRING INFORMATION_SCHEMA_NAME;
extern LEX_CSTRING MYSQL_SCHEMA_NAME;

/* table names */
extern LEX_CSTRING MYSQL_PROC_NAME;

inline bool is_infoschema_db(const LEX_CSTRING *name)
{
  return lex_string_eq(&INFORMATION_SCHEMA_NAME, name);
}

inline bool is_perfschema_db(const LEX_CSTRING *name)
{
  return lex_string_eq(&PERFORMANCE_SCHEMA_DB_NAME, name);
}

inline void mark_as_null_row(TABLE *table)
{
  table->null_row=1;
  table->status|=STATUS_NULL_ROW;
  if (table->s->null_bytes)
    bfill(table->null_flags,table->s->null_bytes,255);
}

/*
  Restore table to state before mark_as_null_row() call.
  This assumes that the caller has restored table->null_flags,
  as is done in unclear_tables().
*/

inline void unmark_as_null_row(TABLE *table)
{
  table->null_row= 0;
  table->status&= ~STATUS_NULL_ROW;
}

bool is_simple_order(ORDER *order);

class Open_tables_backup;

/** Transaction Registry Table (TRT)

    This table holds transaction IDs, their corresponding times and other
    transaction-related data which is used for transaction order resolution.
    When versioned table marks its records lifetime with transaction IDs,
    TRT is used to get their actual timestamps. */

class TR_table: public TABLE_LIST
{
  THD *thd;
  Open_tables_backup *open_tables_backup;

public:
  enum field_id_t {
    FLD_TRX_ID= 0,
    FLD_COMMIT_ID,
    FLD_BEGIN_TS,
    FLD_COMMIT_TS,
    FLD_ISO_LEVEL,
    FIELD_COUNT
  };

  enum enabled {NO, MAYBE, YES};
  static enum enabled use_transaction_registry;

  /**
     @param[in,out] Thread handle
     @param[in] Current transaction is read-write.
   */
  TR_table(THD *_thd, bool rw= false);
  /**
     Opens a transaction_registry table.

     @retval true on error, false otherwise.
   */
  bool open();
  ~TR_table();
  /**
     @retval current thd
  */
  THD *get_thd() const { return thd; }
  /**
     Stores value to internal transaction_registry TABLE object.

     @param[in] field number in a TABLE
     @param[in] value to store
   */
  void store(uint field_id, ulonglong val);
  /**
     Stores value to internal transaction_registry TABLE object.

     @param[in] field number in a TABLE
     @param[in] value to store
   */
  void store(uint field_id, timeval ts);
  /**
    Update the transaction_registry right before commit.
    @param start_id    transaction identifier at start
    @param end_id      transaction identifier at commit

    @retval false      on success
    @retval true       on error (the transaction must be rolled back)
  */
  bool update(ulonglong start_id, ulonglong end_id);
  // return true if found; false if not found or error
  bool query(ulonglong trx_id);
  /**
     Gets a row from transaction_registry with the closest commit_timestamp to
     first argument. We can search for a value which a lesser or greater than
     first argument. Also loads a row into an internal TABLE object.

     @param[in] timestamp
     @param[in] true if we search for a lesser timestamp, false if greater
     @retval true if exists, false it not exists or an error occurred
   */
  bool query(MYSQL_TIME &commit_time, bool backwards);
  /**
     Checks whether transaction1 sees transaction0.

     @param[out] true if transaction1 sees transaction0, undefined on error and
       when transaction1=transaction0 and false otherwise
     @param[in] transaction_id of transaction1
     @param[in] transaction_id of transaction0
     @param[in] commit time of transaction1 or 0 if we want it to be queried
     @param[in] isolation level (from handler.h) of transaction1
     @param[in] commit time of transaction0 or 0 if we want it to be queried
     @retval true on error, false otherwise
   */
  bool query_sees(bool &result, ulonglong trx_id1, ulonglong trx_id0,
                  ulonglong commit_id1= 0,
                  enum_tx_isolation iso_level1= ISO_READ_UNCOMMITTED,
                  ulonglong commit_id0= 0);

  /**
     @retval transaction isolation level of a row from internal TABLE object.
   */
  enum_tx_isolation iso_level() const;
  /**
     Stores transactioin isolation level to internal TABLE object.
   */
  void store_iso_level(enum_tx_isolation iso_level)
  {
    DBUG_ASSERT(iso_level <= ISO_SERIALIZABLE);
    store(FLD_ISO_LEVEL, iso_level + 1);
  }

  /**
     Writes a message to MariaDB log about incorrect transaction_registry schema.

     @param[in] a message explained what's incorrect in schema
   */
  void warn_schema_incorrect(const char *reason);
  /**
     Checks whether transaction_registry table has a correct schema.

     @retval true if schema is incorrect and false otherwise
   */
  bool check(bool error);

  TABLE * operator-> () const
  {
    return table;
  }
  Field * operator[] (uint field_id) const
  {
    DBUG_ASSERT(field_id < FIELD_COUNT);
    return table->field[field_id];
  }
  operator bool () const
  {
    return table;
  }
  bool operator== (const TABLE_LIST &subj) const
  {
    return (!cmp(&db, &subj.db) && !cmp(&table_name, &subj.table_name));
  }
  bool operator!= (const TABLE_LIST &subj) const
  {
    return !(*this == subj);
  }
};

#endif /* MYSQL_CLIENT */

#endif /* TABLE_INCLUDED */
