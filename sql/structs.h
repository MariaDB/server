#ifndef STRUCTS_INCLUDED
#define STRUCTS_INCLUDED

/* Copyright (c) 2000, 2010, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB Corporation.

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



/* The old structures from unireg */

#include "sql_plugin.h"                         /* plugin_ref */
#include "sql_const.h"                          /* MAX_REFLENGTH */
#include "my_time.h"                   /* enum_mysql_timestamp_type */
#include "thr_lock.h"                  /* thr_lock_type */
#include "my_base.h"                   /* ha_rows, ha_key_alg */
#include <mysql_com.h>                  /* USERNAME_LENGTH */
#include "sql_bitmap.h"
#include "lex_charset.h"

struct TABLE;
class Type_handler;
class Field;
class Index_statistics;

class THD;

/* Array index type for table.field[] */
typedef uint16 field_index_t;

typedef struct st_date_time_format {
  uchar positions[8];
  char  time_separator;			/* Separator between hour and minute */
  uint flag;				/* For future */
  LEX_CSTRING format;
} DATE_TIME_FORMAT;


typedef struct st_keyfile_info {	/* used with ha_info() */
  uchar ref[MAX_REFLENGTH];		/* Pointer to current row */
  uchar dupp_ref[MAX_REFLENGTH];	/* Pointer to dupp row */
  uint ref_length;			/* Length of ref (1-8) */
  uint block_size;			/* index block size */
  File filenr;				/* (uniq) filenr for table */
  ha_rows records;			/* Records i datafilen */
  ha_rows deleted;			/* Deleted records */
  ulonglong data_file_length;		/* Length off data file */
  ulonglong max_data_file_length;	/* Length off data file */
  ulonglong index_file_length;
  ulonglong max_index_file_length;
  ulonglong delete_length;		/* Free bytes */
  ulonglong auto_increment_value;
  int errkey,sortkey;			/* Last errorkey and sorted by */
  time_t create_time;			/* When table was created */
  time_t check_time;
  time_t update_time;
  ulong mean_rec_length;		/* physical reclength */
} KEYFILE_INFO;


typedef struct st_key_part_info {	/* Info about a key part */
  Field *field;                         /* the Field object for the indexed
                                           prefix of the original table Field.
                                           NOT necessarily the original Field */
  uint  offset;                         /* Offset in record (from 0) */
  uint  null_offset;                    /* Offset to null_bit in record */
  /* Length of key part in bytes, excluding NULL flag and length bytes */
  uint length;
  /* 
    Number of bytes required to store the keypart value. This may be
    different from the "length" field as it also counts
     - possible NULL-flag byte (see HA_KEY_NULL_LENGTH)
     - possible HA_KEY_BLOB_LENGTH bytes needed to store actual value length.
  */
  uint store_length;
  uint16 key_type;
  field_index_t fieldnr;                /* Fieldnr begins counting from 1 */
  uint16 key_part_flag;                 /* 0 or HA_REVERSE_SORT */
  uint8 type;
  uint8 null_bit;                       /* Position to null_bit */
} KEY_PART_INFO ;

class engine_option_value;
struct ha_index_option_struct;

typedef struct st_key {
  uint	key_length;			/* total length of user defined key parts  */
  ulong flags;                          /* dupp key and pack flags */
  uint	user_defined_key_parts;	   /* How many key_parts */
  uint	usable_key_parts; /* Should normally be = user_defined_key_parts */
  uint ext_key_parts;              /* Number of key parts in extended key */
  ulong ext_key_flags;             /* Flags for extended key              */
  /*
    Parts of primary key that are in the extension of this index. 

    Example: if this structure describes idx1, which is defined as 
      INDEX idx1 (pk2, col2)
    and pk is defined as:
      PRIMARY KEY (pk1, pk2)
    then 
      pk1 is in the extension idx1, ext_key_part_map.is_set(0) == true
      pk2 is explicitly present in idx1, it is not in the extension, so
      ext_key_part_map.is_set(1) == false
  */
  key_part_map ext_key_part_map;
  /*
    Bitmap of indexes having common parts with this index
    (only key parts from key definitions are taken into account)
  */
  key_map overlapped;
  /* Set of keys constraint correlated with this key */
  key_map constraint_correlated;
  LEX_CSTRING name;
  uint  block_size;
  enum  ha_key_alg algorithm;
  /* 
    The flag is on if statistical data for the index prefixes
    has to be taken from the system statistical tables.
  */
  bool is_statistics_from_stat_tables;
  /*
    Note that parser is used when the table is opened for use, and
    parser_name is used when the table is being created.
  */
  union
  {
    plugin_ref parser;                  /* Fulltext [pre]parser */
    LEX_CSTRING *parser_name;           /* Fulltext [pre]parser name */
  };
  KEY_PART_INFO *key_part;
  /* Unique name for cache;  db + \0 + table_name + \0 + key_name + \0 */
  uchar *cache_name;
  /*
    Array of AVG(#records with the same field value) for 1st ... Nth key part.
    0 means 'not known'.
    For temporary heap tables this member is NULL.
  */
  ulong *rec_per_key;

  /*
    This structure is used for statistical data on the index
    that has been read from the statistical table index_stat
  */ 
  Index_statistics *read_stats;
  /*
    This structure is used for statistical data on the index that
    is collected by the function collect_statistics_for_table
  */
  Index_statistics *collected_stats;
 
  TABLE *table;
  LEX_CSTRING comment;
  /** reference to the list of options or NULL */
  engine_option_value *option_list;
  ha_index_option_struct *option_struct;                  /* structure with parsed options */

  double actual_rec_per_key(uint i);

  bool without_overlaps;
  /*
    TRUE if index needs to be ignored
  */
  bool is_ignored;
} KEY;


struct st_join_table;

typedef struct st_reginfo {		/* Extra info about reg */
  struct st_join_table *join_tab;	/* Used by SELECT() */
  enum thr_lock_type lock_type;		/* How database is used */
  bool skip_locked;
  bool not_exists_optimize;
  /*
    TRUE <=> range optimizer found that there is no rows satisfying
    table conditions.
  */
  bool impossible_range;
} REGINFO;


/*
  Originally MySQL used MYSQL_TIME structure inside server only, but since
  4.1 it's exported to user in the new client API. Define aliases for
  new names to keep existing code simple.
*/

typedef enum enum_mysql_timestamp_type timestamp_type;


typedef struct {
  ulong year,month,day,hour;
  ulonglong minute,second,second_part;
  bool neg;
} INTERVAL;


typedef struct st_known_date_time_format {
  const char *format_name;
  const char *date_format;
  const char *datetime_format;
  const char *time_format;
} KNOWN_DATE_TIME_FORMAT;

extern const char *show_comp_option_name[];

typedef int *(*update_var)(THD *, struct st_mysql_show_var *);

struct USER_AUTH : public Sql_alloc
{
  LEX_CSTRING plugin, auth_str, pwtext;
  USER_AUTH *next;
  USER_AUTH() : next(NULL)
  {
    plugin.str= auth_str.str= "";
    pwtext.str= NULL;
    plugin.length= auth_str.length= pwtext.length= 0;
  }
};

struct AUTHID
{
  LEX_CSTRING user, host;
  void init() { memset(this, 0, sizeof(*this)); }
  void copy(MEM_ROOT *root, const LEX_CSTRING *usr, const LEX_CSTRING *host);
  bool is_role() const { return user.str[0] && !host.str[0]; }
  void set_lex_string(LEX_CSTRING *l, char *buf)
  {
    if (is_role())
      *l= user;
    else
    {
      l->str= buf;
      l->length= strxmov(buf, user.str, "@", host.str, NullS) - buf;
    }
  }
  void parse(const char *str, size_t length);
  bool read_from_mysql_proc_row(THD *thd, TABLE *table);
};


struct LEX_USER: public AUTHID
{
  USER_AUTH *auth;
  bool has_auth()
  {
    return auth && (auth->plugin.length || auth->auth_str.length || auth->pwtext.length);
  }
};

/*
  This structure specifies the maximum amount of resources which
  can be consumed by each account. Zero value of a member means
  there is no limit.
*/
typedef struct user_resources {
  /* Maximum number of queries/statements per hour. */
  uint questions;
  /*
     Maximum number of updating statements per hour (which statements are
     updating is defined by sql_command_flags array).
  */
  uint updates;
  /* Maximum number of connections established per hour. */
  uint conn_per_hour;
  /*
    Maximum number of concurrent connections. If -1 then no new
    connections allowed
  */
  int user_conn;
  /* Max query timeout */
  double max_statement_time;

  /*
     Values of this enum and specified_limits member are used by the
     parser to store which user limits were specified in GRANT statement.
  */
  enum {QUERIES_PER_HOUR= 1, UPDATES_PER_HOUR= 2, CONNECTIONS_PER_HOUR= 4,
        USER_CONNECTIONS= 8, MAX_STATEMENT_TIME= 16};
  uint specified_limits;
} USER_RESOURCES;


/*
  This structure is used for counting resources consumed and for checking
  them against specified user limits.
*/
typedef struct  user_conn {
  /*
     Pointer to user+host key (pair separated by '\0') defining the entity
     for which resources are counted (By default it is user account thus
     priv_user/priv_host pair is used. If --old-style-user-limits option
     is enabled, resources are counted for each user+host separately).
  */
  char *user;
  /* Pointer to host part of the key. */
  char *host;
  /**
     The moment of time when per hour counters were reset last time
     (i.e. start of "hour" for conn_per_hour, updates, questions counters).
  */
  ulonglong reset_utime;
  /* Total length of the key. */
  uint len;
  /* Current amount of concurrent connections for this account. */
  int connections;
  /*
     Current number of connections per hour, number of updating statements
     per hour and total number of statements per hour for this account.
  */
  uint conn_per_hour, updates, questions;
  /* Maximum amount of resources which account is allowed to consume. */
  USER_RESOURCES user_resources;
} USER_CONN;

typedef struct st_user_stats
{
  char user[MY_MAX(USERNAME_LENGTH, LIST_PROCESS_HOST_LEN) + 1];
  // Account name the user is mapped to when this is a user from mapped_user.
  // Otherwise, the same value as user.
  char priv_user[MY_MAX(USERNAME_LENGTH, LIST_PROCESS_HOST_LEN) + 1];
  uint user_name_length;
  uint total_connections;
  uint total_ssl_connections;
  uint concurrent_connections;
  time_t connected_time;  // in seconds
  ha_rows rows_read, rows_sent;
  ha_rows rows_updated, rows_deleted, rows_inserted;
  ulonglong bytes_received;
  ulonglong bytes_sent;
  ulonglong binlog_bytes_written;
  ulonglong select_commands, update_commands, other_commands;
  ulonglong commit_trans, rollback_trans;
  ulonglong denied_connections, lost_connections, max_statement_time_exceeded;
  ulonglong access_denied_errors;
  ulonglong empty_queries;
  double busy_time;       // in seconds
  double cpu_time;        // in seconds
} USER_STATS;

typedef struct st_table_stats
{
  char table[NAME_LEN * 2 + 2];  // [db] + '\0' + [table] + '\0'
  size_t table_name_length;
  ulonglong rows_read, rows_changed;
  ulonglong rows_changed_x_indexes;
  /* Stores enum db_type, but forward declarations cannot be done */
  int engine_type;
} TABLE_STATS;

typedef struct st_index_stats
{
  // [db] + '\0' + [table] + '\0' + [index] + '\0'
  char index[NAME_LEN * 3 + 3];
  size_t index_name_length;                       /* Length of 'index' */
  ulonglong rows_read;
} INDEX_STATS;


	/* Bits in form->update */
#define REG_MAKE_DUPP		1U	/* Make a copy of record when read */
#define REG_NEW_RECORD		2U	/* Write a new record if not found */
#define REG_UPDATE		4U	/* Uppdate record */
#define REG_DELETE		8U	/* Delete found record */
#define REG_PROG		16U	/* User is updating database */
#define REG_CLEAR_AFTER_WRITE	32U
#define REG_MAY_BE_UPDATED	64U
#define REG_AUTO_UPDATE		64U	/* Used in D-forms for scroll-tables */
#define REG_OVERWRITE		128U
#define REG_SKIP_DUP		256U

	/* Bits in form->status */
#define STATUS_NO_RECORD	(1U+2U)	/* Record isn't usable */
#define STATUS_GARBAGE		1U
#define STATUS_NOT_FOUND	2U	/* No record in database when needed */
#define STATUS_NO_PARENT	4U	/* Parent record wasn't found */
#define STATUS_NOT_READ		8U	/* Record isn't read */
#define STATUS_UPDATED		16U	/* Record is updated by formula */
#define STATUS_NULL_ROW		32U	/* table->null_row is set */
#define STATUS_DELETED		64U

/*
  Such interval is "discrete": it is the set of
  { auto_inc_interval_min + k * increment,
    0 <= k <= (auto_inc_interval_values-1) }
  Where "increment" is maintained separately by the user of this class (and is
  currently only thd->variables.auto_increment_increment).
  It mustn't derive from Sql_alloc, because SET INSERT_ID needs to
  allocate memory which must stay allocated for use by the next statement.
*/
class Discrete_interval {
private:
  ulonglong interval_min;
  ulonglong interval_values;
  ulonglong  interval_max;    // excluded bound. Redundant.
public:
  Discrete_interval *next;    // used when linked into Discrete_intervals_list
  void replace(ulonglong start, ulonglong val, ulonglong incr)
  {
    interval_min=    start;
    interval_values= val;
    interval_max=    (val == ULONGLONG_MAX) ? val : start + val * incr;
  }
  Discrete_interval(ulonglong start, ulonglong val, ulonglong incr) :
    next(NULL) { replace(start, val, incr); };
  Discrete_interval() : next(NULL) { replace(0, 0, 0); };
  ulonglong minimum() const { return interval_min;    };
  ulonglong values()  const { return interval_values; };
  ulonglong maximum() const { return interval_max;    };
  /*
    If appending [3,5] to [1,2], we merge both in [1,5] (they should have the
    same increment for that, user of the class has to ensure that). That is
    just a space optimization. Returns 0 if merge succeeded.
  */
  bool merge_if_contiguous(ulonglong start, ulonglong val, ulonglong incr)
  {
    if (interval_max == start)
    {
      if (val == ULONGLONG_MAX)
      {
        interval_values=   interval_max= val;
      }
      else
      {
        interval_values+=  val;
        interval_max=      start + val * incr;
      }
      return 0;
    }
    return 1;
  };
};

/* List of Discrete_interval objects */
class Discrete_intervals_list {
private:
  Discrete_interval        *head;
  Discrete_interval        *tail;
  /*
    When many intervals are provided at the beginning of the execution of a
    statement (in a replication slave or SET INSERT_ID), "current" points to
    the interval being consumed by the thread now (so "current" goes from
    "head" to "tail" then to NULL).
  */
  Discrete_interval        *current;
  uint                  elements; // number of elements
  void set_members(Discrete_interval *h, Discrete_interval *t,
                   Discrete_interval *c, uint el)
  {  
    head= h;
    tail= t;
    current= c;
    elements= el;
  }
  void operator=(Discrete_intervals_list &);  /* prevent use of these */
  Discrete_intervals_list(const Discrete_intervals_list &);

public:
  Discrete_intervals_list() : head(NULL), current(NULL), elements(0) {};
  void empty_no_free()
  {
    set_members(NULL, NULL, NULL, 0);
  }
  void empty()
  {
    for (Discrete_interval *i= head; i;)
    {
      Discrete_interval *next= i->next;
      delete i;
      i= next;
    }
    empty_no_free();
  }
  void copy_shallow(const Discrete_intervals_list * dli)
  {
    head= dli->get_head();
    tail= dli->get_tail();
    current= dli->get_current();
    elements= dli->nb_elements();
  }
  void swap (Discrete_intervals_list * dli)
  {
    Discrete_interval *h, *t, *c;
    uint el;
    h= dli->get_head();
    t= dli->get_tail();
    c= dli->get_current();
    el= dli->nb_elements();
    dli->copy_shallow(this);
    set_members(h, t, c, el);
  }
  const Discrete_interval* get_next()
  {
    Discrete_interval *tmp= current;
    if (current != NULL)
      current= current->next;
    return tmp;
  }
  ~Discrete_intervals_list() { empty(); };
  bool append(ulonglong start, ulonglong val, ulonglong incr);
  bool append(Discrete_interval *interval);
  ulonglong minimum()     const { return (head ? head->minimum() : 0); };
  ulonglong maximum()     const { return (head ? tail->maximum() : 0); };
  uint      nb_elements() const { return elements; }
  Discrete_interval* get_head() const { return head; };
  Discrete_interval* get_tail() const { return tail; };
  Discrete_interval* get_current() const { return current; };
};


/*
  DDL options:
  - CREATE IF NOT EXISTS
  - DROP IF EXISTS
  - CREATE LIKE
  - REPLACE
*/
struct DDL_options_st
{
public:
  enum Options
  {
    OPT_NONE= 0,
    OPT_IF_NOT_EXISTS= 2,              // CREATE TABLE IF NOT EXISTS
    OPT_LIKE= 4,                       // CREATE TABLE LIKE
    OPT_OR_REPLACE= 16,                // CREATE OR REPLACE TABLE
    OPT_OR_REPLACE_SLAVE_GENERATED= 32,// REPLACE was added on slave, it was
                                       // not in the original query on master.
    OPT_IF_EXISTS= 64,
    OPT_CREATE_SELECT= 128             // CREATE ... SELECT
  };

private:
  Options m_options;

public:
  Options create_like_options() const
  {
    return (DDL_options_st::Options)
           (((uint) m_options) & (OPT_IF_NOT_EXISTS | OPT_OR_REPLACE));
  }
  void init() { m_options= OPT_NONE; }
  void init(Options options) { m_options= options; }
  void set(Options other)
  {
    m_options= other;
  }
  void set(const DDL_options_st other)
  {
    m_options= other.m_options;
  }
  bool if_not_exists() const { return m_options & OPT_IF_NOT_EXISTS; }
  bool or_replace() const { return m_options & OPT_OR_REPLACE; }
  bool or_replace_slave_generated() const
  { return m_options & OPT_OR_REPLACE_SLAVE_GENERATED; }
  bool like() const { return m_options & OPT_LIKE; }
  bool if_exists() const { return m_options & OPT_IF_EXISTS; }
  bool is_create_select() const { return m_options & OPT_CREATE_SELECT; }

  void add(const DDL_options_st::Options other)
  {
    m_options= (Options) ((uint) m_options | (uint) other);
  }
  void add(const DDL_options_st &other)
  {
    add(other.m_options);
  }
  DDL_options_st operator|(const DDL_options_st &other)
  {
    add(other.m_options);
    return *this;
  }
  DDL_options_st operator|=(DDL_options_st::Options other)
  {
    add(other);
    return *this;
  }
};


class DDL_options: public DDL_options_st
{
public:
  DDL_options() { init(); }
  DDL_options(Options options) { init(options); }
  DDL_options(const DDL_options_st &options)
  { DDL_options_st::operator=(options); }
};


struct Lex_length_and_dec_st
{
protected:
  uint32 m_length;
  uint8  m_dec;
  uint8  m_collation_type:LEX_CHARSET_COLLATION_TYPE_BITS;
  bool   m_has_explicit_length:1;
  bool   m_has_explicit_dec:1;
  bool   m_length_overflowed:1;
  bool   m_dec_overflowed:1;

  static_assert(LEX_CHARSET_COLLATION_TYPE_BITS <= 8,
                "Lex_length_and_dec_st::m_collation_type bits check");

public:
  void reset()
  {
    m_length= 0;
    m_dec= 0;
    m_collation_type= 0;
    m_has_explicit_length= false;
    m_has_explicit_dec= false;
    m_length_overflowed= false;
    m_dec_overflowed= false;
  }
  void set_length_only(uint32 length)
  {
    m_length= length;
    m_dec= 0;
    m_collation_type= 0;
    m_has_explicit_length= true;
    m_has_explicit_dec= false;
    m_length_overflowed= false;
    m_dec_overflowed= false;
  }
  void set_dec_only(uint8 dec)
  {
    m_length= 0;
    m_dec= dec;
    m_collation_type= 0;
    m_has_explicit_length= false;
    m_has_explicit_dec= true;
    m_length_overflowed= false;
    m_dec_overflowed= false;
  }
  void set_length_and_dec(uint32 length, uint8 dec)
  {
    m_length= length;
    m_dec= dec;
    m_collation_type= 0;
    m_has_explicit_length= true;
    m_has_explicit_dec= true;
    m_length_overflowed= false;
    m_dec_overflowed= false;
  }
  void set(const char *length, const char *dec);
  uint32 length() const
  {
    return m_length;
  }
  uint8 dec() const
  {
    return m_dec;
  }
  bool has_explicit_length() const
  {
    return m_has_explicit_length;
  }
  bool has_explicit_dec() const
  {
    return m_has_explicit_dec;
  }
  bool length_overflowed()  const
  {
    return m_length_overflowed;
  }
  bool dec_overflowed()  const
  {
    return m_dec_overflowed;
  }
};


struct Lex_field_type_st: public Lex_length_and_dec_st
{
private:
  const Type_handler *m_handler;
  CHARSET_INFO *m_ci;
public:
  void set(const Type_handler *handler,
           Lex_length_and_dec_st length_and_dec,
           CHARSET_INFO *cs= NULL)
  {
    m_handler= handler;
    m_ci= cs;
    Lex_length_and_dec_st::operator=(length_and_dec);
  }
  void set(const Type_handler *handler,
           const Lex_length_and_dec_st &length_and_dec,
           const Lex_charset_collation_st &coll)
  {
    m_handler= handler;
    m_ci= coll.charset_collation();
    Lex_length_and_dec_st::operator=(length_and_dec);
    m_collation_type= ((uint8) coll.type()) & 0x3;
  }
  void set(const Type_handler *handler, const Lex_charset_collation_st &coll)
  {
    m_handler= handler;
    m_ci= coll.charset_collation();
    Lex_length_and_dec_st::reset();
    m_collation_type= ((uint8) coll.type()) & 0x3;
  }
  void set(const Type_handler *handler, CHARSET_INFO *cs= NULL)
  {
    m_handler= handler;
    m_ci= cs;
    Lex_length_and_dec_st::reset();
  }
  void set_handler_length_flags(const Type_handler *handler,
                                const Lex_length_and_dec_st &length,
                                uint32 flags);
  void set_handler_length(const Type_handler *handler, uint32 length)
  {
    m_handler= handler;
    m_ci= NULL;
    Lex_length_and_dec_st::set_length_only(length);
  }
  void set_handler(const Type_handler *handler)
  {
    m_handler= handler;
  }
  const Type_handler *type_handler() const { return m_handler; }
  CHARSET_INFO *charset_collation() const { return m_ci; }
  Lex_charset_collation lex_charset_collation() const
  {
    return Lex_charset_collation(m_ci,
                                 (Lex_charset_collation_st::Type)
                                 m_collation_type);
  }
};


struct Lex_dyncol_type_st: public Lex_length_and_dec_st
{
private:
  int m_type; // enum_dynamic_column_type is not visible here, so use int
  CHARSET_INFO *m_ci;
public:
  void set(int type, Lex_length_and_dec_st length_and_dec,
           CHARSET_INFO *cs= NULL)
  {
    m_type= type;
    m_ci= cs;
    Lex_length_and_dec_st::operator=(length_and_dec);
  }
  void set(int type)
  {
    m_type= type;
    m_ci= NULL;
    Lex_length_and_dec_st::reset();
  }
  void set(int type, CHARSET_INFO *cs)
  {
    m_type= type;
    m_ci= cs;
    Lex_length_and_dec_st::reset();
  }
  bool set(int type, const Lex_charset_collation_st &collation,
           CHARSET_INFO *charset)
  {
    CHARSET_INFO *tmp= collation.resolved_to_character_set(charset);
    if (!tmp)
      return true;
    set(type, tmp);
    return false;
  }
  int dyncol_type() const { return m_type; }
  CHARSET_INFO *charset_collation() const { return m_ci; }
};


struct Lex_spblock_handlers_st
{
public:
  int hndlrs;
  void init(int count) { hndlrs= count; }
};


struct Lex_spblock_st: public Lex_spblock_handlers_st
{
public:
  int vars;
  int conds;
  int curs;
  void init()
  {
    vars= conds= hndlrs= curs= 0;
  }
  void init_using_vars(uint nvars)
  {
    vars= nvars;
    conds= hndlrs= curs= 0;
  }
  void join(const Lex_spblock_st &b1, const Lex_spblock_st &b2)
  {
    vars= b1.vars + b2.vars;
    conds= b1.conds + b2.conds;
    hndlrs= b1.hndlrs + b2.hndlrs;
    curs= b1.curs + b2.curs;
  }
};


class Lex_spblock: public Lex_spblock_st
{
public:
  Lex_spblock() { init(); }
  Lex_spblock(const Lex_spblock_handlers_st &other)
  {
    vars= conds= curs= 0;
    hndlrs= other.hndlrs;
  }
};


struct Lex_for_loop_bounds_st
{
public:
  class sp_assignment_lex *m_index;  // The first iteration value (or cursor)
  class sp_assignment_lex *m_target_bound; // The last iteration value
  int8 m_direction;
  bool m_implicit_cursor;
  bool is_for_loop_cursor() const { return m_target_bound == NULL; }
};


class Lex_for_loop_bounds_intrange: public Lex_for_loop_bounds_st
{
public:
  Lex_for_loop_bounds_intrange(int8 direction,
                               class sp_assignment_lex *left_expr,
                               class sp_assignment_lex *right_expr)
  {
    m_direction= direction;
    m_index=        direction > 0 ? left_expr  : right_expr;
    m_target_bound= direction > 0 ? right_expr : left_expr;
    m_implicit_cursor= false;
  }
};


struct Lex_for_loop_st
{
public:
  class sp_variable *m_index;  // The first iteration value (or cursor)
  class sp_variable *m_target_bound; // The last iteration value
  int m_cursor_offset;
  int8 m_direction;
  bool m_implicit_cursor;
  void init()
  {
    m_index= 0;
    m_target_bound= 0;
    m_direction= 0;
    m_implicit_cursor= false;
  }
  void init(const Lex_for_loop_st &other)
  {
    *this= other;
  }
  bool is_for_loop_cursor() const { return m_target_bound == NULL; }
  bool is_for_loop_explicit_cursor() const
  {
    return is_for_loop_cursor() && !m_implicit_cursor;
  }
};


enum trim_spec { TRIM_LEADING, TRIM_TRAILING, TRIM_BOTH };

struct Lex_trim_st
{
  Item *m_remove;
  Item *m_source;
  trim_spec m_spec;
public:
  void set(trim_spec spec, Item *remove, Item *source)
  {
    m_spec= spec;
    m_remove= remove;
    m_source= source;
  }
  void set(trim_spec spec, Item *source)
  {
    set(spec, NULL, source);
  }
  Item *make_item_func_trim_std(THD *thd) const;
  Item *make_item_func_trim_oracle(THD *thd) const;
  Item *make_item_func_trim(THD *thd) const;
};


class Lex_trim: public Lex_trim_st
{
public:
  Lex_trim(trim_spec spec, Item *source) { set(spec, source); }
};


class st_select_lex;

class Lex_select_lock
{
public:
  struct
  {
    uint defined_lock:1;
    uint update_lock:1;
    uint defined_timeout:1;
    uint skip_locked:1;
  };
  ulong timeout;


  void empty()
  {
    defined_lock= update_lock= defined_timeout= skip_locked= FALSE;
    timeout= 0;
  }
  void set_to(st_select_lex *sel);
};

class Lex_select_limit
{
public:
  /* explicit LIMIT clause was used */
  bool explicit_limit;
  bool with_ties;
  Item *select_limit, *offset_limit;

  void clear()
  {
    explicit_limit= FALSE;    // No explicit limit given by user
    with_ties= FALSE;         // No use of WITH TIES operator
    select_limit= NULL;       // denotes the default limit = HA_POS_ERROR
    offset_limit= NULL;       // denotes the default offset = 0
  }
};

struct st_order;

class Load_data_param
{
protected:
  CHARSET_INFO *m_charset;   // Character set of the file
  ulonglong m_fixed_length;  // Sum of target field lengths for fixed format
  bool m_is_fixed_length;
  bool m_use_blobs;
public:
  Load_data_param(CHARSET_INFO *cs, bool is_fixed_length):
    m_charset(cs),
    m_fixed_length(0),
    m_is_fixed_length(is_fixed_length),
    m_use_blobs(false)
  { }
  bool add_outvar_field(THD *thd, const Field *field);
  bool add_outvar_user_var(THD *thd);
  CHARSET_INFO *charset() const { return m_charset; }
  bool is_fixed_length() const { return m_is_fixed_length; }
  bool use_blobs() const { return m_use_blobs; }
};


class Load_data_outvar
{
public:
  virtual ~Load_data_outvar() {}
  virtual bool load_data_set_null(THD *thd, const Load_data_param *param)= 0;
  virtual bool load_data_set_value(THD *thd, const char *pos, uint length,
                                   const Load_data_param *param)= 0;
  virtual bool load_data_set_no_data(THD *thd, const Load_data_param *param)= 0;
  virtual void load_data_print_for_log_event(THD *thd, class String *to) const= 0;
  virtual bool load_data_add_outvar(THD *thd, Load_data_param *param) const= 0;
  virtual uint load_data_fixed_length() const= 0;
};


class Timeval: public timeval
{
protected:
  Timeval() { }
public:
  Timeval(my_time_t sec, ulong usec)
  {
    tv_sec= sec;
    /*
      Since tv_usec is not always of type ulong, cast usec parameter
      explicitly to uint to avoid compiler warnings about losing
      integer precision.
    */
    DBUG_ASSERT(usec < 1000000);
    tv_usec= (uint)usec;
  }
  explicit Timeval(const timeval &tv)
   :timeval(tv)
  { }
};


#endif /* STRUCTS_INCLUDED */
