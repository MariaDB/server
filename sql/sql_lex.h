/* Copyright (c) 2000, 2019, Oracle and/or its affiliates.
   Copyright (c) 2010, 2022, MariaDB Corporation.

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

/**
  @defgroup Semantic_Analysis Semantic Analysis
*/

#ifndef SQL_LEX_INCLUDED
#define SQL_LEX_INCLUDED

#include "violite.h"                            /* SSL_type */
#include "sql_trigger.h"
#include "thr_lock.h"                  /* thr_lock_type, TL_UNLOCK */
#include "mem_root_array.h"
#include "grant.h"
#include "sql_cmd.h"
#include "sql_alter.h"                // Alter_info
#include "sql_window.h"
#include "sql_trigger.h"
#include "sp.h"                       // enum enum_sp_type
#include "sql_tvc.h"
#include "item.h"
#include "sql_limit.h"                // Select_limit_counters
#include "json_table.h"               // Json_table_column
#include "sql_schema.h"
#include "table.h"
#include "sql_class.h"                // enum enum_column_usage
#include "select_handler.h"

/* Used for flags of nesting constructs */
#define SELECT_NESTING_MAP_SIZE 64
typedef Bitmap<SELECT_NESTING_MAP_SIZE> nesting_map;

/* YACC and LEX Definitions */


/**
  A string with metadata. Usually points to a string in the client
  character set, but unlike Lex_ident_cli_st (see below) it does not
  necessarily point to a query fragment. It can also point to memory
  of other kinds (e.g. an additional THD allocated memory buffer
  not overlapping with the current query text).

  We'll add more flags here eventually, to know if the string has, e.g.:
  - multi-byte characters
  - bad byte sequences
  - backslash escapes:   'a\nb'
  and reuse the original query fragments instead of making the string
  copy too early, in Lex_input_stream::get_text().
  This will allow to avoid unnecessary copying, as well as
  create more optimal Item types in sql_yacc.yy
*/
struct Lex_string_with_metadata_st: public LEX_CSTRING
{
private:
  bool m_is_8bit; // True if the string has 8bit characters
  char m_quote;   // Quote character, or 0 if not quoted
public:
  void set_8bit(bool is_8bit) { m_is_8bit= is_8bit; }
  void set_metadata(bool is_8bit, char quote)
  {
    m_is_8bit= is_8bit;
    m_quote= quote;
  }
  void set(const char *s, size_t len, bool is_8bit, char quote)
  {
    str= s;
    length= len;
    set_metadata(is_8bit, quote);
  }
  void set(const LEX_CSTRING *s, bool is_8bit, char quote)
  {
    ((LEX_CSTRING &)*this)= *s;
    set_metadata(is_8bit, quote);
  }
  bool is_8bit() const { return m_is_8bit; }
  bool is_quoted() const { return m_quote != '\0'; }
  char quote() const { return m_quote; }
  // Get string repertoire by the 8-bit flag and the character set
  my_repertoire_t repertoire(CHARSET_INFO *cs) const
  {
    return !m_is_8bit && my_charset_is_ascii_based(cs) ?
           MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
  }
  // Get string repertoire by the 8-bit flag, for ASCII-based character sets
  my_repertoire_t repertoire() const
  {
    return !m_is_8bit ? MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
  }
};


/*
  Used to store identifiers in the client character set.
  Points to a query fragment.
*/
struct Lex_ident_cli_st: public Lex_string_with_metadata_st
{
public:
  void set_keyword(const char *s, size_t len)
  {
    set(s, len, false, '\0');
  }
  void set_ident(const char *s, size_t len, bool is_8bit)
  {
    set(s, len, is_8bit, '\0');
  }
  void set_ident_quoted(const char *s, size_t len, bool is_8bit, char quote)
  {
    set(s, len, is_8bit, quote);
  }
  void set_unquoted(const LEX_CSTRING *s, bool is_8bit)
  {
    set(s, is_8bit, '\0');
  }
  const char *pos() const { return str - is_quoted(); }
  const char *end() const { return str + length + is_quoted(); }
};


class Lex_ident_cli: public Lex_ident_cli_st
{
public:
  Lex_ident_cli(const LEX_CSTRING *s, bool is_8bit)
  {
    set_unquoted(s, is_8bit);
  }
  Lex_ident_cli(const char *s, size_t len)
  {
    set_ident(s, len, false);
  }
};


struct Lex_ident_sys_st: public LEX_CSTRING
{
public:
  bool copy_ident_cli(const THD *thd, const Lex_ident_cli_st *str);
  bool copy_keyword(const THD *thd, const Lex_ident_cli_st *str);
  bool copy_sys(const THD *thd, const LEX_CSTRING *str);
  bool convert(const THD *thd, const LEX_CSTRING *str, CHARSET_INFO *cs);
  bool copy_or_convert(const THD *thd, const Lex_ident_cli_st *str,
                       CHARSET_INFO *cs);
  bool is_null() const { return str == NULL; }
  bool to_size_number(ulonglong *to) const;
  void set_valid_utf8(const LEX_CSTRING *name)
  {
    DBUG_ASSERT(Well_formed_prefix(system_charset_info, name->str,
                                   name->length).length() == name->length);
    str= name->str ; length= name->length;
  }
};


class Lex_ident_sys: public Lex_ident_sys_st
{
public:
  Lex_ident_sys(const THD *thd, const Lex_ident_cli_st *str)
  {
    if (copy_ident_cli(thd, str))
      ((LEX_CSTRING &) *this)= null_clex_str;
  }
  Lex_ident_sys()
  {
    ((LEX_CSTRING &) *this)= null_clex_str;
  }
  Lex_ident_sys(const char *name, size_t length)
  {
    LEX_CSTRING tmp= {name, length};
    set_valid_utf8(&tmp);
  }
  Lex_ident_sys & operator=(const Lex_ident_sys_st &name)
  {
    Lex_ident_sys_st::operator=(name);
    return *this;
  }
};


struct Lex_column_list_privilege_st
{
  List<Lex_ident_sys> *m_columns;
  privilege_t m_privilege;
};


class Lex_column_list_privilege: public Lex_column_list_privilege_st
{
public:
  Lex_column_list_privilege(List<Lex_ident_sys> *columns, privilege_t privilege)
  {
    m_columns= columns;
    m_privilege= privilege;
  }
};


/**
  ORDER BY ... LIMIT parameters;
*/
class Lex_order_limit_lock: public Sql_alloc
{
public:
  SQL_I_List<st_order> *order_list;   /* ORDER clause */
  Lex_select_lock lock;
  Lex_select_limit limit;

  Lex_order_limit_lock() :order_list(NULL)
  {}

  bool set_to(st_select_lex *sel);
};


enum sub_select_type
{
  UNSPECIFIED_TYPE,
  /* following 3 enums should be as they are*/
  UNION_TYPE, INTERSECT_TYPE, EXCEPT_TYPE,
  GLOBAL_OPTIONS_TYPE, DERIVED_TABLE_TYPE, OLAP_TYPE
};

enum set_op_type
{
  UNSPECIFIED,
  UNION_DISTINCT, UNION_ALL,
  EXCEPT_DISTINCT, EXCEPT_ALL,
  INTERSECT_DISTINCT, INTERSECT_ALL
};

inline int cmp_unit_op(enum sub_select_type op1, enum sub_select_type op2)
{
  DBUG_ASSERT(op1 >= UNION_TYPE && op1 <= EXCEPT_TYPE);
  DBUG_ASSERT(op2 >= UNION_TYPE && op2 <= EXCEPT_TYPE);
  return (op1 == INTERSECT_TYPE ? 1 : 0) - (op2 == INTERSECT_TYPE ? 1 : 0);
}

enum unit_common_op {OP_MIX, OP_UNION, OP_INTERSECT, OP_EXCEPT};

enum enum_view_suid
{
  VIEW_SUID_INVOKER= 0,
  VIEW_SUID_DEFINER= 1,
  VIEW_SUID_DEFAULT= 2
};


enum plsql_cursor_attr_t
{
  PLSQL_CURSOR_ATTR_ISOPEN,
  PLSQL_CURSOR_ATTR_FOUND,
  PLSQL_CURSOR_ATTR_NOTFOUND,
  PLSQL_CURSOR_ATTR_ROWCOUNT
};


enum enum_sp_suid_behaviour
{
  SP_IS_DEFAULT_SUID= 0,
  SP_IS_NOT_SUID,
  SP_IS_SUID
};


enum enum_sp_aggregate_type
{
  DEFAULT_AGGREGATE= 0,
  NOT_AGGREGATE,
  GROUP_AGGREGATE
};


/* These may not be declared yet */
class Table_ident;
class sql_exchange;
class LEX_COLUMN;
class sp_head;
class sp_name;
class sp_instr;
class sp_pcontext;
class sp_variable;
class sp_expr_lex;
class sp_assignment_lex;
class partition_info;
class Event_parse_data;
class set_var_base;
class sys_var;
class Item_func_match;
class File_parser;
class Key_part_spec;
class Item_window_func;
struct sql_digest_state;
class With_clause;
class my_var;
class select_handler;
class Pushdown_select;

#define ALLOC_ROOT_SET 1024

#ifdef MYSQL_SERVER
/*
  There are 8 different type of table access so there is no more than
  combinations 2^8 = 256:

  . STMT_READS_TRANS_TABLE

  . STMT_READS_NON_TRANS_TABLE

  . STMT_READS_TEMP_TRANS_TABLE

  . STMT_READS_TEMP_NON_TRANS_TABLE

  . STMT_WRITES_TRANS_TABLE

  . STMT_WRITES_NON_TRANS_TABLE

  . STMT_WRITES_TEMP_TRANS_TABLE

  . STMT_WRITES_TEMP_NON_TRANS_TABLE

  The unsafe conditions for each combination is represented within a byte
  and stores the status of the option --binlog-direct-non-trans-updates,
  whether the trx-cache is empty or not, and whether the isolation level
  is lower than ISO_REPEATABLE_READ:

  . option (OFF/ON)
  . trx-cache (empty/not empty)
  . isolation (>= ISO_REPEATABLE_READ / < ISO_REPEATABLE_READ)

  bits 0 : . OFF, . empty, . >= ISO_REPEATABLE_READ
  bits 1 : . OFF, . empty, . < ISO_REPEATABLE_READ
  bits 2 : . OFF, . not empty, . >= ISO_REPEATABLE_READ
  bits 3 : . OFF, . not empty, . < ISO_REPEATABLE_READ
  bits 4 : . ON, . empty, . >= ISO_REPEATABLE_READ
  bits 5 : . ON, . empty, . < ISO_REPEATABLE_READ
  bits 6 : . ON, . not empty, . >= ISO_REPEATABLE_READ
  bits 7 : . ON, . not empty, . < ISO_REPEATABLE_READ
*/
extern uint binlog_unsafe_map[256];
/*
  Initializes the array with unsafe combinations and its respective
  conditions.
*/
void binlog_unsafe_map_init();
#endif

#ifdef MYSQL_SERVER
/*
  The following hack is needed because yy_*.cc do not define
  YYSTYPE before including this file
*/
#ifdef MYSQL_YACC
#define LEX_YYSTYPE void *
#else
#include "lex_symbol.h"
#ifdef MYSQL_LEX
#include "item_func.h"            /* Cast_target used in yy_mariadb.hh */
#include "sql_get_diagnostics.h"  /* Types used in yy_mariadb.hh */
#include "sp_pcontext.h"
#include "yy_mariadb.hh"
#define LEX_YYSTYPE YYSTYPE *
#else
#define LEX_YYSTYPE void *
#endif
#endif
#endif

// describe/explain types
#define DESCRIBE_NORMAL         1
#define DESCRIBE_EXTENDED       2
/*
  This is not within #ifdef because we want "EXPLAIN PARTITIONS ..." to produce
  additional "partitions" column even if partitioning is not compiled in.
*/
#define DESCRIBE_PARTITIONS	4
#define DESCRIBE_EXTENDED2	8

#ifdef MYSQL_SERVER

extern const LEX_STRING  empty_lex_str;
extern const LEX_CSTRING empty_clex_str;
extern const LEX_CSTRING star_clex_str;
extern const LEX_CSTRING param_clex_str;

enum enum_sp_data_access
{
  SP_DEFAULT_ACCESS= 0,
  SP_CONTAINS_SQL,
  SP_NO_SQL,
  SP_READS_SQL_DATA,
  SP_MODIFIES_SQL_DATA
};

#define DERIVED_SUBQUERY        1
#define DERIVED_VIEW            2
#define DERIVED_WITH            4

enum enum_view_create_mode
{
  VIEW_CREATE_NEW,              // check that there are not such VIEW/table
  VIEW_ALTER,                   // check that VIEW .frm with such name exists
  VIEW_CREATE_OR_REPLACE        // check only that there are not such table
};


class Create_view_info: public Sql_alloc
{
public:
  LEX_CSTRING select;              // The SELECT statement of CREATE VIEW
  enum enum_view_create_mode mode;
  uint16 algorithm;
  uint8 check;
  enum enum_view_suid suid;
  Create_view_info(enum_view_create_mode mode_arg,
                   uint16 algorithm_arg,
                   enum_view_suid suid_arg)
   :select(null_clex_str),
    mode(mode_arg),
    algorithm(algorithm_arg),
    check(VIEW_CHECK_NONE),
    suid(suid_arg)
  { }
};


enum enum_drop_mode
{
  DROP_DEFAULT, // mode is not specified
  DROP_CASCADE, // CASCADE option
  DROP_RESTRICT // RESTRICT option
};

/* Options to add_table_to_list() */
#define TL_OPTION_UPDATING      1
#define TL_OPTION_FORCE_INDEX   2
#define TL_OPTION_IGNORE_LEAVES 4
#define TL_OPTION_ALIAS         8
#define TL_OPTION_SEQUENCE      16
#define TL_OPTION_TABLE_FUNCTION        32

typedef List<Item> List_item;
typedef Mem_root_array<ORDER*, true> Group_list_ptrs;

/* SERVERS CACHE CHANGES */
typedef struct st_lex_server_options
{
  long port;
  LEX_CSTRING server_name, host, db, username, password, scheme, socket, owner;
  void reset(LEX_CSTRING name)
  {
    server_name= name;
    host= db= username= password= scheme= socket= owner= null_clex_str;
    port= -1;
  }
} LEX_SERVER_OPTIONS;


/**
  Structure to hold parameters for CHANGE MASTER, START SLAVE, and STOP SLAVE.

  Remark: this should not be confused with Master_info (and perhaps
  would better be renamed to st_lex_replication_info).  Some fields,
  e.g., delay, are saved in Relay_log_info, not in Master_info.
*/
struct LEX_MASTER_INFO
{
  DYNAMIC_ARRAY repl_ignore_server_ids;
  DYNAMIC_ARRAY repl_do_domain_ids;
  DYNAMIC_ARRAY repl_ignore_domain_ids;
  const char *host, *user, *password, *log_file_name;
  const char *ssl_key, *ssl_cert, *ssl_ca, *ssl_capath, *ssl_cipher;
  const char *ssl_crl, *ssl_crlpath;
  const char *relay_log_name;
  LEX_CSTRING connection_name;
  /* Value in START SLAVE UNTIL master_gtid_pos=xxx */
  LEX_CSTRING gtid_pos_str;
  ulonglong pos;
  ulong relay_log_pos;
  ulong server_id;
  uint port, connect_retry;
  float heartbeat_period;
  int sql_delay;
  bool is_demotion_opt;
  /*
    Enum is used for making it possible to detect if the user
    changed variable or if it should be left at old value
   */
  enum {LEX_MI_UNCHANGED= 0, LEX_MI_DISABLE, LEX_MI_ENABLE}
    ssl, ssl_verify_server_cert, heartbeat_opt, repl_ignore_server_ids_opt,
    repl_do_domain_ids_opt, repl_ignore_domain_ids_opt;
  enum {
    LEX_GTID_UNCHANGED, LEX_GTID_NO, LEX_GTID_CURRENT_POS, LEX_GTID_SLAVE_POS
  } use_gtid_opt;

  void init()
  {
    bzero(this, sizeof(*this));
    my_init_dynamic_array(PSI_INSTRUMENT_ME, &repl_ignore_server_ids,
                          sizeof(::server_id), 0, 16, MYF(0));
    my_init_dynamic_array(PSI_INSTRUMENT_ME, &repl_do_domain_ids,
                          sizeof(ulong), 0, 16, MYF(0));
    my_init_dynamic_array(PSI_INSTRUMENT_ME, &repl_ignore_domain_ids,
                          sizeof(ulong), 0, 16, MYF(0));
    sql_delay= -1;
  }
  void reset(bool is_change_master)
  {
    if (unlikely(is_change_master))
    {
      delete_dynamic(&repl_ignore_server_ids);
      /* Free all the array elements. */
      delete_dynamic(&repl_do_domain_ids);
      delete_dynamic(&repl_ignore_domain_ids);
    }

    host= user= password= log_file_name= ssl_key= ssl_cert= ssl_ca=
      ssl_capath= ssl_cipher= ssl_crl= ssl_crlpath= relay_log_name= NULL;
    pos= relay_log_pos= server_id= port= connect_retry= 0;
    heartbeat_period= 0;
    ssl= ssl_verify_server_cert= heartbeat_opt=
      repl_ignore_server_ids_opt= repl_do_domain_ids_opt=
      repl_ignore_domain_ids_opt= LEX_MI_UNCHANGED;
    gtid_pos_str= null_clex_str;
    use_gtid_opt= LEX_GTID_UNCHANGED;
    sql_delay= -1;
    is_demotion_opt= 0;
  }
};

typedef struct st_lex_reset_slave
{
  bool all;
} LEX_RESET_SLAVE;

enum olap_type 
{
  UNSPECIFIED_OLAP_TYPE, CUBE_TYPE, ROLLUP_TYPE
};

/* 
  String names used to print a statement with index hints.
  Keep in sync with index_hint_type.
*/
extern const char * index_hint_type_name[];
typedef uchar index_clause_map;

/*
  Bits in index_clause_map : one for each possible FOR clause in
  USE/FORCE/IGNORE INDEX index hint specification
*/
#define INDEX_HINT_MASK_JOIN  (1)
#define INDEX_HINT_MASK_GROUP (1 << 1)
#define INDEX_HINT_MASK_ORDER (1 << 2)

#define INDEX_HINT_MASK_ALL (INDEX_HINT_MASK_JOIN | INDEX_HINT_MASK_GROUP | \
                             INDEX_HINT_MASK_ORDER)

class select_result_sink;

/* Single element of an USE/FORCE/IGNORE INDEX list specified as a SQL hint  */
class Index_hint : public Sql_alloc
{
public:
  /* The type of the hint : USE/FORCE/IGNORE */
  enum index_hint_type type;
  /* Where the hit applies to. A bitmask of INDEX_HINT_MASK_<place> values */
  index_clause_map clause;
  /* 
    The index name. Empty (str=NULL) name represents an empty list 
    USE INDEX () clause 
  */ 
  LEX_CSTRING key_name;

  Index_hint (enum index_hint_type type_arg, index_clause_map clause_arg,
              const char *str, size_t length) :
    type(type_arg), clause(clause_arg)
  {
    key_name.str= str;
    key_name.length= length;
  }

  void print(THD *thd, String *str);
}; 

/* 
  The state of the lex parsing for selects 
   
   master and slaves are pointers to select_lex.
   master is pointer to upper level node.
   slave is pointer to lower level node
   select_lex is a SELECT without union
   unit is container of either
     - One SELECT
     - UNION of selects
   select_lex and unit are both inherited form st_select_lex_node
   neighbors are two select_lex or units on the same level

   All select describing structures linked with following pointers:
   - list of neighbors (next/prev) (prev of first element point to slave
     pointer of upper structure)
     - For select this is a list of UNION's (or one element list)
     - For units this is a list of sub queries for the upper level select

   - pointer to master (master), which is
     If this is a unit
       - pointer to outer select_lex
     If this is a select_lex
       - pointer to outer unit structure for select

   - pointer to slave (slave), which is either:
     If this is a unit:
       - first SELECT that belong to this unit
     If this is a select_lex
       - first unit that belong to this SELECT (subquries or derived tables)

   - list of all select_lex (link_next/link_prev)
     This is to be used for things like derived tables creation, where we
     go through this list and create the derived tables.

   If unit contain several selects (UNION now, INTERSECT etc later)
   then it have special select_lex called fake_select_lex. It used for
   storing global parameters (like ORDER BY, LIMIT) and executing union.
   Subqueries used in global ORDER BY clause will be attached to this
   fake_select_lex, which will allow them correctly resolve fields of
   'upper' UNION and outer selects.

   For example for following query:

   select *
     from table1
     where table1.field IN (select * from table1_1_1 union
                            select * from table1_1_2)
     union
   select *
     from table2
     where table2.field=(select (select f1 from table2_1_1_1_1
                                   where table2_1_1_1_1.f2=table2_1_1.f3)
                           from table2_1_1
                           where table2_1_1.f1=table2.f2)
     union
   select * from table3;

   we will have following structure:

   select1: (select * from table1 ...)
   select2: (select * from table2 ...)
   select3: (select * from table3)
   select1.1.1: (select * from table1_1_1)
   ...

     main unit
     fake0
     select1 select2 select3
     |^^     |^
    s|||     ||master
    l|||     |+---------------------------------+
    a|||     +---------------------------------+|
    v|||master                         slave   ||
    e||+-------------------------+             ||
     V|            neighbor      |             V|
     unit1.1<+==================>unit1.2       unit2.1
     fake1.1
     select1.1.1 select 1.1.2    select1.2.1   select2.1.1
                                               |^
                                               ||
                                               V|
                                               unit2.1.1.1
                                               select2.1.1.1.1


   relation in main unit will be following:
   (bigger picture for:
      main unit
      fake0
      select1 select2 select3
   in the above picture)

         main unit
         |^^^^|fake_select_lex
         |||||+--------------------------------------------+
         ||||+--------------------------------------------+|
         |||+------------------------------+              ||
         ||+--------------+                |              ||
    slave||master         |                |              ||
         V|      neighbor |       neighbor |        master|V
         select1<========>select2<========>select3        fake0

    list of all select_lex will be following (as it will be constructed by
    parser):

    select1->select2->select3->select2.1.1->select 2.1.2->select2.1.1.1.1-+
                                                                          |
    +---------------------------------------------------------------------+
    |
    +->select1.1.1->select1.1.2

*/

/* 
    Base class for st_select_lex (SELECT_LEX) & 
    st_select_lex_unit (SELECT_LEX_UNIT)
*/
struct LEX;
class st_select_lex;
class st_select_lex_unit;


class st_select_lex_node {
protected:
  st_select_lex_node *next, **prev,   /* neighbor list */
    *master, *slave,                  /* vertical links */
    *link_next, **link_prev;          /* list of whole SELECT_LEX */
  enum sub_select_type linkage;

  void init_query_common();

public:
  ulonglong options;
  uint8 uncacheable;
  bool distinct:1;
  bool no_table_names_allowed:1; /* used for global order by */
  /*
    result of this query can't be cached, bit field, can be :
      UNCACHEABLE_DEPENDENT_GENERATED
      UNCACHEABLE_DEPENDENT_INJECTED
      UNCACHEABLE_RAND
      UNCACHEABLE_SIDEEFFECT
      UNCACHEABLE_EXPLAIN
      UNCACHEABLE_PREPARE
  */

  bool is_linkage_set() const
  {
    return linkage == UNION_TYPE || linkage == INTERSECT_TYPE || linkage == EXCEPT_TYPE;
  }
  enum sub_select_type get_linkage() { return linkage; }
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return (void*) alloc_root(mem_root, (uint) size); }
  static void operator delete(void *ptr,size_t size) { TRASH_FREE(ptr, size); }
  static void operator delete(void *ptr, MEM_ROOT *mem_root) {}

  // Ensures that at least all members used during cleanup() are initialized.
  st_select_lex_node()
    : next(NULL), prev(NULL),
      master(NULL), slave(NULL),
      link_next(NULL), link_prev(NULL),
      linkage(UNSPECIFIED_TYPE)
  {
  }

  inline st_select_lex_node* get_master() { return master; }
  void include_down(st_select_lex_node *upper);
  void attach_single(st_select_lex_node *slave_arg);
  void include_neighbour(st_select_lex_node *before);
  void link_chain_down(st_select_lex_node *first);
  void link_neighbour(st_select_lex_node *neighbour)
  {
    DBUG_ASSERT(next == NULL);
    DBUG_ASSERT(neighbour != NULL);
    next= neighbour;
    neighbour->prev= &next;
  }
  void cut_next() { next= NULL; }
  void include_standalone(st_select_lex_node *sel, st_select_lex_node **ref);
  void include_global(st_select_lex_node **plink);
  void exclude();
  void exclude_from_tree();
  void exclude_from_global()
  {
    if (!link_prev)
      return;
    if (((*link_prev)= link_next))
      link_next->link_prev= link_prev;
    link_next= NULL;
    link_prev= NULL;
  }
  void substitute_in_tree(st_select_lex_node *subst);

  void set_slave(st_select_lex_node *slave_arg) { slave= slave_arg; }
  void move_node(st_select_lex_node *where_to_move)
  {
    if (where_to_move == this)
      return;
    if (next)
      next->prev= prev;
    *prev= next;
    *where_to_move->prev= this;
    next= where_to_move;
  }
  st_select_lex_node *insert_chain_before(st_select_lex_node **ptr_pos_to_insert,
                                          st_select_lex_node *end_chain_node);
  void move_as_slave(st_select_lex_node *new_master);
  void set_linkage(enum sub_select_type l)
  {
    DBUG_ENTER("st_select_lex_node::set_linkage");
    DBUG_PRINT("info", ("node: %p  linkage: %d->%d", this, linkage, l));
    linkage= l;
    DBUG_VOID_RETURN;
  }
  /*
    This method created for reiniting LEX in mysql_admin_table() and can be
    used only if you are going remove all SELECT_LEX & units except belonger
    to LEX (LEX::unit & LEX::select, for other purposes there are
    SELECT_LEX_UNIT::exclude_level & SELECT_LEX_UNIT::exclude_tree.

    It is also used in parsing to detach builtin select.
  */
  void cut_subtree() { slave= 0; }
  friend class st_select_lex_unit;
  friend bool mysql_new_select(LEX *lex, bool move_down, SELECT_LEX *sel);
  friend bool mysql_make_view(THD *thd, TABLE_SHARE *share, TABLE_LIST *table,
                              bool open_view_no_parse);
  friend class st_select_lex;
private:
  void fast_exclude();
};
typedef class st_select_lex_node SELECT_LEX_NODE;

/* 
   SELECT_LEX_UNIT - unit of selects (UNION, INTERSECT, ...) group 
   SELECT_LEXs
*/
class THD;
class select_result;
class JOIN;
class select_unit;
class Procedure;
class Explain_query;

void delete_explain_query(LEX *lex);
void create_explain_query(LEX *lex, MEM_ROOT *mem_root);
void create_explain_query_if_not_exists(LEX *lex, MEM_ROOT *mem_root);
bool print_explain_for_slow_log(LEX *lex, THD *thd, String *str);


class st_select_lex_unit: public st_select_lex_node {
protected:
  TABLE_LIST result_table_list;
  select_unit *union_result;
  ulonglong found_rows_for_union;

  bool prepare_join(THD *thd, SELECT_LEX *sl, select_result *result,
                    ulonglong additional_options,
                    bool is_union_select);
  bool join_union_type_handlers(THD *thd,
                                class Type_holder *holders, uint count);
  bool join_union_type_attributes(THD *thd,
                                  class Type_holder *holders, uint count);
public:
  bool join_union_item_types(THD *thd, List<Item> &types, uint count);
  // Ensures that at least all members used during cleanup() are initialized.
  st_select_lex_unit()
    : union_result(NULL), table(NULL),  result(NULL), fake_select_lex(NULL),
      last_procedure(NULL),cleaned(false), bag_set_op_optimized(false),
      have_except_all_or_intersect_all(false), pushdown_unit(NULL)
  {
  }

  void set_query_result(select_result *res) { result= res; }

  TABLE *table; /* temporary table using for appending UNION results */
  select_result *result;
  st_select_lex *pre_last_parse;
  /*
    Node on which we should return current_select pointer after parsing
    subquery
  */
  st_select_lex *return_to;
  /* LIMIT clause runtime counters */
  Select_limit_counters lim;
  /* not NULL if unit used in subselect, point to subselect item */
  Item_subselect *item;
  /*
    TABLE_LIST representing this union in the embedding select. Used for
    derived tables/views handling.
  */
  TABLE_LIST *derived;
  /* With clause attached to this unit (if any) */
  With_clause *with_clause;
  /* With element where this unit is used as the specification (if any) */
  With_element *with_element;
  /* The unit used as a CTE specification from which this unit is cloned */
  st_select_lex_unit *cloned_from;
  /* thread handler */
  THD *thd;
  /*
    SELECT_LEX for hidden SELECT in union which process global
    ORDER BY and LIMIT
  */
  st_select_lex *fake_select_lex;
  /**
    SELECT_LEX that stores LIMIT and OFFSET for UNION ALL when noq
    fake_select_lex is used.
  */
  st_select_lex *saved_fake_select_lex;

  /* pointer to the last node before last subsequence of UNION ALL */
  st_select_lex *union_distinct;
  Procedure *last_procedure;     /* Pointer to procedure, if such exists */

  // list of fields which points to temporary table for union
  List<Item> item_list;
  /*
    list of types of items inside union (used for union & derived tables)
    
    Item_type_holders from which this list consist may have pointers to Field,
    pointers is valid only after preparing SELECTS of this unit and before
    any SELECT of this unit execution
  */
  List<Item> types;

  bool prepared:1; // prepare phase already performed for UNION (unit)
  bool optimized:1; // optimize phase already performed for UNION (unit)
  bool optimized_2:1;
  bool executed:1; // already executed
  bool cleaned:1;
  bool bag_set_op_optimized:1;
  bool optimize_started:1;
  bool have_except_all_or_intersect_all:1;

  /* The object used to organize execution of the UNIT by a foreign engine */
  select_handler *pushdown_unit;

  /**
     TRUE if the unit contained TVC at the top level that has been wrapped
     into SELECT:
     VALUES (v1) ... (vn) => SELECT * FROM (VALUES (v1) ... (vn)) as tvc
  */
  bool with_wrapped_tvc:1;
  bool is_view:1;
  bool describe:1; /* union exec() called for EXPLAIN */
  bool columns_are_renamed:1;

protected:
  /* This is bool, not bit, as it's used and set in many places */
  bool saved_error;
public:

  /**
    Pointer to 'last' select, or pointer to select where we stored
    global parameters for union.

    If this is a union of multiple selects, the parser puts the global
    parameters in fake_select_lex. If the union doesn't use a
    temporary table, st_select_lex_unit::prepare() nulls out
    fake_select_lex, but saves a copy in saved_fake_select_lex in
    order to preserve the global parameters.

    If it is not a union, first_select() is the last select.

    @return select containing the global parameters
  */
  inline st_select_lex *global_parameters()
  {
    if (fake_select_lex != NULL)
      return fake_select_lex;
    else if (saved_fake_select_lex != NULL)
      return saved_fake_select_lex;
    return first_select();
  };

  void init_query();
  st_select_lex* outer_select() const;
  const st_select_lex* first_select() const
  {
    return reinterpret_cast<const st_select_lex*>(slave);
  }
  st_select_lex* first_select()
  {
    return reinterpret_cast<st_select_lex*>(slave);
  }
  void set_with_clause(With_clause *with_cl);
  st_select_lex_unit* next_unit()
  {
    return reinterpret_cast<st_select_lex_unit*>(next);
  }
  st_select_lex* return_after_parsing() { return return_to; }
  void exclude_level();
  // void exclude_tree(); // it is not used for long time
  bool is_excluded() { return prev == NULL; }

  /* UNION methods */
  bool prepare(TABLE_LIST *derived_arg, select_result *sel_result,
               ulonglong additional_options);
  bool optimize();
  void optimize_bag_operation(bool is_outer_distinct);
  bool exec();
  bool exec_recursive();
  bool cleanup();
  inline void unclean() { cleaned= 0; }
  void reinit_exec_mechanism();

  void print(String *str, enum_query_type query_type);

  bool add_fake_select_lex(THD *thd);
  void init_prepare_fake_select_lex(THD *thd, bool first_execution);
  void set_prepared() { prepared = true; }
  inline bool is_prepared() { return prepared; }
  bool change_result(select_result_interceptor *result,
                     select_result_interceptor *old_result);
  void set_limit(st_select_lex *values);
  void set_thd(THD *thd_arg) { thd= thd_arg; }
  inline bool is_unit_op ();
  bool union_needs_tmp_table();

  void set_unique_exclude();
  bool check_distinct_in_union();

  friend struct LEX;
  friend int subselect_union_engine::exec();

  List<Item> *get_column_types(bool for_cursor);

  select_unit *get_union_result() { return union_result; }
  int save_union_explain(Explain_query *output);
  int save_union_explain_part2(Explain_query *output);
  unit_common_op common_op();

  bool explainable() const;

  void reset_distinct();
  void fix_distinct();

  void register_select_chain(SELECT_LEX *first_sel);

  bool set_nest_level(int new_nest_level);
  bool check_parameters(SELECT_LEX *main_select);

  bool set_lock_to_the_last_select(Lex_select_lock l);

  bool can_be_merged();

  friend class st_select_lex;

private:
  bool exec_inner();
  bool is_derived_eliminated() const;
  bool set_direct_union_result(select_result *sel_result);
  bool prepare_pushdown(bool use_direct_union_result,
                        select_result *sel_result);
};

typedef class st_select_lex_unit SELECT_LEX_UNIT;
typedef Bounds_checked_array<Item*> Ref_ptr_array;


/**
  Structure which consists of the field and the item that
  corresponds to this field.
*/

class Field_pair :public Sql_alloc
{
public:
  Field *field;
  Item *corresponding_item;
  Field_pair(Field *fld, Item *item)
    :field(fld), corresponding_item(item) {}
};

Field_pair *get_corresponding_field_pair(Item *item,
                                         List<Field_pair> pair_list);
Field_pair *find_matching_field_pair(Item *item, List<Field_pair> pair_list);


#define TOUCHED_SEL_COND 1/* WHERE/HAVING/ON should be reinited before use */
#define TOUCHED_SEL_DERIVED (1<<1)/* derived should be reinited before use */

#define UNIT_NEST_FL        1
/*
  SELECT_LEX - store information of parsed SELECT statment
*/
class st_select_lex: public st_select_lex_node
{
public:
  /*
    Currently the field first_nested is used only by parser.
    It containa either a reference to the first select
    of the nest of selects to which 'this' belongs to, or
    in the case of priority jump it contains a reference to
    the select to which the priority nest has to be attached to.
    If there is no priority jump then the first select of the
    nest contains the reference to itself in first_nested.
    Example:
      select1 union select2 intersect select
    Here we have a priority jump at select2.
    So select2->first_nested points to select1,
    while select3->first_nested points to select2 and
    select1->first_nested points to select1.
  */

  Name_resolution_context context;
  LEX_CSTRING db;

  /*
    Point to the LEX in which it was created, used in view subquery detection.

    TODO: make also st_select_lex::parent_stmt_lex (see LEX::stmt_lex)
    and use st_select_lex::parent_lex & st_select_lex::parent_stmt_lex
    instead of global (from THD) references where it is possible.
  */
  LEX *parent_lex;
  st_select_lex *first_nested;
  Item *where, *having;                         /* WHERE & HAVING clauses */
  Item *prep_where; /* saved WHERE clause for prepared statement processing */
  Item *prep_having;/* saved HAVING clause for prepared statement processing */
  Item *cond_pushed_into_where;  /* condition pushed into WHERE  */
  Item *cond_pushed_into_having; /* condition pushed into HAVING */
  Item *where_cond_after_prepare;

  /*
    nest_levels are local to the query or VIEW,
    and that view merge procedure does not re-calculate them.
    So we also have to remember unit against which we count levels.
  */
  SELECT_LEX_UNIT *nest_level_base;
  Item_sum *inner_sum_func_list; /* list of sum func in nested selects */ 
  /* 
    This is a copy of the original JOIN USING list that comes from
    the parser. The parser :
      1. Sets the natural_join of the second TABLE_LIST in the join
         and the st_select_lex::prev_join_using.
      2. Makes a parent TABLE_LIST and sets its is_natural_join/
       join_using_fields members.
      3. Uses the wrapper TABLE_LIST as a table in the upper level.
    We cannot assign directly to join_using_fields in the parser because
    at stage (1.) the parent TABLE_LIST is not constructed yet and
    the assignment will override the JOIN USING fields of the lower level
    joins on the right.
  */
  List<String> *prev_join_using;
  JOIN *join; /* after JOIN::prepare it is pointer to corresponding JOIN */
  TABLE_LIST *embedding;          /* table embedding to the above list   */
  table_value_constr *tvc;

  /* The object used to organize execution of the query by a foreign engine */
  select_handler *pushdown_select;
  List<TABLE_LIST> *join_list;    /* list for the currently parsed join  */
  st_select_lex *merged_into; /* select which this select is merged into */
                              /* (not 0 only for views/derived tables)   */
  const char *type;           /* type of select for EXPLAIN          */


  /* List of references to fields referenced from inner selects */
  List<Item_outer_ref> inner_refs_list;
  List<Item> attach_to_conds;
  /* Saved values of the WHERE and HAVING clauses*/
  Item::cond_result cond_value, having_value;
  /* 
    Usually it is pointer to ftfunc_list_alloc, but in union used to create
    fake select_lex for calling mysql_select under results of union
  */
  List<Item_func_match> *ftfunc_list;
  List<Item_func_match> ftfunc_list_alloc;
  /*
    The list of items to which MIN/MAX optimizations of opt_sum_query()
    have been applied. Used to rollback those optimizations if it's needed.
  */
  List<Item_sum> min_max_opt_list;
  List<TABLE_LIST> top_join_list; /* join list of the top level          */
  List<TABLE_LIST> sj_nests;      /* Semi-join nests within this join */
  /*
    Beginning of the list of leaves in a FROM clause, where the leaves
    inlcude all base tables including view tables. The tables are connected
    by TABLE_LIST::next_leaf, so leaf_tables points to the left-most leaf.

    List of all base tables local to a subquery including all view
    tables. Unlike 'next_local', this in this list views are *not*
    leaves. Created in setup_tables() -> make_leaves_list().
  */
  /* 
    Subqueries that will need to be converted to semi-join nests, including
    those converted to jtbm nests. The list is emptied when conversion is done.
  */
  List<Item_in_subselect> sj_subselects;
  /*
    List of IN-predicates in this st_select_lex that
    can be transformed into IN-subselect defined with TVC.
  */
  List<Item_func_in> in_funcs;
  List<TABLE_LIST> leaf_tables;
  List<TABLE_LIST> leaf_tables_exec;
  List<TABLE_LIST> leaf_tables_prep;

  /* current index hint kind. used in filling up index_hints */
  enum index_hint_type current_index_hint_type;

  /*
    FROM clause - points to the beginning of the TABLE_LIST::next_local list.
  */
  SQL_I_List<TABLE_LIST>  table_list;

  /*
    GROUP BY clause.
    This list may be mutated during optimization (by remove_const()),
    so for prepared statements, we keep a copy of the ORDER.next pointers in
    group_list_ptrs, and re-establish the original list before each execution.
  */
  SQL_I_List<ORDER>       group_list;
  SQL_I_List<ORDER>       save_group_list;
  Group_list_ptrs        *group_list_ptrs;

  List<Item>          item_list;  /* list of fields & expressions */
  List<Item>          pre_fix;    /* above list before fix_fields */
  List<Item>          fix_after_optimize;
  SQL_I_List<ORDER> order_list;   /* ORDER clause */
  SQL_I_List<ORDER> save_order_list;
  SQL_I_List<ORDER> gorder_list;
  Lex_select_limit limit_params;  /* LIMIT clause parameters */

  /* Structure to store fields that are used in the GROUP BY of this select */
  List<Field_pair> grouping_tmp_fields;
  List<udf_func>     udf_list;                  /* udf function calls stack */
  List<Index_hint> *index_hints;  /* list of USE/FORCE/IGNORE INDEX */
  List<List_item> save_many_values;
  List<Item> *save_insert_list;

  enum_column_usage   item_list_usage;
  bool                is_item_list_lookup:1;
  /*
    Needed to correctly generate 'PRIMARY' or 'SIMPLE' for select_type column
    of EXPLAIN
  */
  bool have_merged_subqueries:1;
  bool is_set_query_expr_tail:1;
  bool with_sum_func:1;   /* sum function indicator */
  bool with_rownum:1;     /* rownum() function indicator */
  bool braces:1;    /* SELECT ... UNION (SELECT ... ) <- this braces */
  bool automatic_brackets:1; /* dummy select for INTERSECT precedence */
  /* TRUE when having fix field called in processing of this SELECT */
  bool having_fix_field:1;
  /*
    TRUE when fix field is called for a new condition pushed into the
    HAVING clause of this SELECT
  */
  bool having_fix_field_for_pushed_cond:1;
  /*
    there are subquery in HAVING clause => we can't close tables before
    query processing end even if we use temporary table
  */
  bool subquery_in_having:1;
  /* TRUE <=> this SELECT is correlated w.r.t. some ancestor select */
  bool with_all_modifier:1;  /* used for selects in union */
  bool is_correlated:1;
  bool first_natural_join_processing:1;
  bool first_cond_optimization:1;
  /* do not wrap view fields with Item_ref */
  bool no_wrap_view_item:1;
  /* exclude this select from check of unique_table() */
  bool exclude_from_table_unique_test:1;
  bool in_tvc:1;
  bool skip_locked:1;
  bool m_non_agg_field_used:1;
  bool m_agg_func_used:1;
  bool m_custom_agg_func_used:1;
  /* the select is "service-select" and can not have tables */
  bool is_service_select:1;

  /// Array of pointers to top elements of all_fields list
  Ref_ptr_array ref_pointer_array;
  ulong table_join_options;

  /*
    number of items in select_list and HAVING clause used to get number
    bigger then can be number of entries that will be added to all item
    list during split_sum_func
  */
  uint select_n_having_items;
  uint cond_count;    /* number of sargable Items in where/having/on */
  uint between_count; /* number of between predicates in where/having/on */
  uint max_equal_elems; /* max number of elements in multiple equalities */   
  /*
    Number of fields used in select list or where clause of current select
    and all inner subselects.
  */
  uint select_n_where_fields;
  /* Total number of elements in group by and order by lists */
  uint order_group_num;
  /* reserved for exists 2 in */
  uint select_n_reserved;
  /*
   it counts the number of bit fields in the SELECT list. These are used when
   DISTINCT is converted to a GROUP BY involving BIT fields.
  */
  uint hidden_bit_fields;
  /*
    Number of fields used in the definition of all the windows functions.
    This includes:
      1) Fields in the arguments
      2) Fields in the PARTITION BY clause
      3) Fields in the ORDER BY clause
  */
  /*
    Number of current derived table made with TVC during the
    transformation of IN-predicate into IN-subquery for this
    st_select_lex.
  */
  uint curr_tvc_name;
  /* true <=> select has been created a TVC wrapper */
  bool is_tvc_wrapper;
  uint fields_in_window_functions;
  uint insert_tables;
  enum_parsing_place parsing_place; /* where we are parsing expression */
  enum_parsing_place save_parsing_place;
  enum_parsing_place context_analysis_place; /* where we are in prepare */
  enum leaf_list_state {UNINIT, READY, SAVED};
  enum leaf_list_state prep_leaf_list_state;
  enum olap_type olap;
  /* SELECT [FOR UPDATE/LOCK IN SHARE MODE] [SKIP LOCKED] */
  enum select_lock_type {NONE, IN_SHARE_MODE, FOR_UPDATE};
  enum select_lock_type select_lock;

  uint in_sum_expr;
  uint select_number; /* number of select (used for EXPLAIN) */
  uint with_wild;     /* item list contain '*' ; Counter */
  /* Number of Item_sum-derived objects in this SELECT */
  uint n_sum_items;
  /* Number of Item_sum-derived objects in children and descendant SELECTs */
  uint n_child_sum_items;
  uint versioned_tables;                 /* For versioning */
  int nest_level;     /* nesting level of select */
  /* index in the select list of the expression currently being fixed */
  int cur_pos_in_select_list;

  /*
    This array is used to note  whether we have any candidates for
    expression caching in the corresponding clauses
  */
  bool expr_cache_may_be_used[PARSING_PLACE_SIZE];
  uint8 nest_flags; 
  /*
    This variable is required to ensure proper work of subqueries and
    stored procedures. Generally, one should use the states of
    Query_arena to determine if it's a statement prepare or first
    execution of a stored procedure. However, in case when there was an
    error during the first execution of a stored procedure, the SP body
    is not expelled from the SP cache. Therefore, a deeply nested
    subquery might be left unoptimized. So we need this per-subquery
    variable to inidicate the optimization/execution state of every
    subquery. Prepared statements work OK in that regard, as in
    case of an error during prepare the PS is not created.
  */
  uint8 changed_elements; // see TOUCHED_SEL_*

  /**
    The set of those tables whose fields are referenced in the select list of
    this select level.
  */
  table_map select_list_tables;

  /* Set to 1 if any field in field list has ROWNUM() */
  bool rownum_in_field_list;

  /* namp of nesting SELECT visibility (for aggregate functions check) */
  nesting_map name_visibility_map;
  table_map with_dep;
  index_clause_map current_index_hint_clause;

  /* it is for correct printing SELECT options */
  thr_lock_type lock_type;
  
  /** System Versioning */
  int vers_setup_conds(THD *thd, TABLE_LIST *tables);
  /* push new Item_field into item_list */
  bool vers_push_field(THD *thd, TABLE_LIST *table,
                       const LEX_CSTRING field_name);

  int period_setup_conds(THD *thd, TABLE_LIST *table);
  void init_query();
  void init_select();
  st_select_lex_unit* master_unit() { return (st_select_lex_unit*) master; }
  inline void set_master_unit(st_select_lex_unit *master_unit)
  {
    master= (st_select_lex_node *)master_unit;
  }
  void set_master(st_select_lex *master_arg)
  {
    master= master_arg;
  }
  st_select_lex_unit* first_inner_unit() 
  { 
    return (st_select_lex_unit*) slave; 
  }
  st_select_lex* outer_select();
  bool is_query_topmost(THD *thd);
  st_select_lex* next_select() { return (st_select_lex*) next; }
  st_select_lex* next_select_in_list() 
  {
    return (st_select_lex*) link_next;
  }
  st_select_lex_node** next_select_in_list_addr()
  {
    return &link_next;
  }
  st_select_lex* return_after_parsing()
  {
    return master_unit()->return_after_parsing();
  }
  inline bool is_subquery_function() { return master_unit()->item != 0; }

  bool mark_as_dependent(THD *thd, st_select_lex *last,
                         Item_ident *dependency);

  void set_braces(bool value)
  {
    braces= value;
  }
  bool inc_in_sum_expr();
  uint get_in_sum_expr();

  bool add_item_to_list(THD *thd, Item *item);
  bool add_group_to_list(THD *thd, Item *item, bool asc);
  bool add_ftfunc_to_list(THD *thd, Item_func_match *func);
  bool add_order_to_list(THD *thd, Item *item, bool asc);
  bool add_gorder_to_list(THD *thd, Item *item, bool asc);
  TABLE_LIST* add_table_to_list(THD *thd, Table_ident *table,
                                LEX_CSTRING *alias,
                                ulong table_options,
                                thr_lock_type flags= TL_UNLOCK,
                                enum_mdl_type mdl_type= MDL_SHARED_READ,
                                List<Index_hint> *hints= 0,
                                List<String> *partition_names= 0,
                                LEX_STRING *option= 0);
  TABLE_LIST* get_table_list();
  bool init_nested_join(THD *thd);
  TABLE_LIST *end_nested_join(THD *thd);
  TABLE_LIST *nest_last_join(THD *thd);
  void add_joined_table(TABLE_LIST *table);
  bool add_cross_joined_table(TABLE_LIST *left_op, TABLE_LIST *right_op,
                              bool straight_fl);
  TABLE_LIST *convert_right_join();
  List<Item>* get_item_list();
  ulong get_table_join_options();
  void set_lock_for_tables(thr_lock_type lock_type, bool for_update,
                           bool skip_locks);
  /*
    This method created for reiniting LEX in mysql_admin_table() and can be
    used only if you are going remove all SELECT_LEX & units except belonger
    to LEX (LEX::unit & LEX::select, for other purposes there are
    SELECT_LEX_UNIT::exclude_level & SELECT_LEX_UNIT::exclude_tree
  */
  void cut_subtree() { slave= 0; }
  bool test_limit();
  /**
    Get offset for LIMIT.

    Evaluate offset item if necessary.

    @return Number of rows to skip.
  */
  ha_rows get_offset();
  /**
   Get limit.

   Evaluate limit item if necessary.

   @return Limit of rows in result.
  */
  ha_rows get_limit();

  friend struct LEX;
  st_select_lex() : group_list_ptrs(NULL), braces(0),
                    automatic_brackets(0), n_sum_items(0), n_child_sum_items(0)
  {}
  void make_empty_select()
  {
    init_query();
    init_select();
  }
  bool setup_ref_array(THD *thd, uint order_group_num);
  uint get_cardinality_of_ref_ptrs_slice(uint order_group_num_arg);
  void print(THD *thd, String *str, enum_query_type query_type);
  void print_item_list(THD *thd, String *str, enum_query_type query_type);
  void print_set_clause(THD *thd, String *str, enum_query_type query_type);
  void print_on_duplicate_key_clause(THD *thd, String *str,
                                     enum_query_type query_type);
  static void print_order(String *str,
                          ORDER *order,
                          enum_query_type query_type);
  void print_limit(THD *thd, String *str, enum_query_type query_type);
  void fix_prepare_information(THD *thd, Item **conds, Item **having_conds);
  /*
    Destroy the used execution plan (JOIN) of this subtree (this
    SELECT_LEX and all nested SELECT_LEXes and SELECT_LEX_UNITs).
  */
  bool cleanup();
  /*
    Recursively cleanup the join of this select lex and of all nested
    select lexes.
  */
  void cleanup_all_joins(bool full);

  void set_index_hint_type(enum index_hint_type type, index_clause_map clause);

  /* 
   Add a index hint to the tagged list of hints. The type and clause of the
   hint will be the current ones (set by set_index_hint()) 
  */
  bool add_index_hint (THD *thd, const char *str, size_t length);

  /* make a list to hold index hints */
  void alloc_index_hints (THD *thd);
  /* read and clear the index hints */
  List<Index_hint>* pop_index_hints(void) 
  {
    List<Index_hint> *hints= index_hints;
    index_hints= NULL;
    return hints;
  }

  inline void clear_index_hints(void) { index_hints= NULL; }
  bool is_part_of_union() { return master_unit()->is_unit_op(); }
  bool is_top_level_node() 
  { 
    return (select_number == 1) && !is_part_of_union();
  }
  bool optimize_unflattened_subqueries(bool const_only);
  /* Set the EXPLAIN type for this subquery. */
  void set_explain_type(bool on_the_fly);
  bool handle_derived(LEX *lex, uint phases);
  void append_table_to_list(TABLE_LIST *TABLE_LIST::*link, TABLE_LIST *table);
  bool get_free_table_map(table_map *map, uint *tablenr);
  void replace_leaf_table(TABLE_LIST *table, List<TABLE_LIST> &tbl_list);
  void remap_tables(TABLE_LIST *derived, table_map map,
                    uint tablenr, st_select_lex *parent_lex);
  bool merge_subquery(THD *thd, TABLE_LIST *derived, st_select_lex *subq_lex,
                      uint tablenr, table_map map);
  inline bool is_mergeable()
  {
    return (next_select() == 0 && group_list.elements == 0 &&
            having == 0 && with_sum_func == 0 && with_rownum == 0 &&
            table_list.elements >= 1 && !(options & SELECT_DISTINCT) &&
            limit_params.select_limit == 0);
  }
  void mark_as_belong_to_derived(TABLE_LIST *derived);
  void increase_derived_records(ha_rows records);
  void update_used_tables();
  void update_correlated_cache();
  void mark_const_derived(bool empty);

  bool save_leaf_tables(THD *thd);
  bool save_prep_leaf_tables(THD *thd);

  void set_unique_exclude();

  bool is_merged_child_of(st_select_lex *ancestor);

  /*
    For MODE_ONLY_FULL_GROUP_BY we need to maintain two flags:
     - Non-aggregated fields are used in this select.
     - Aggregate functions are used in this select.
    In MODE_ONLY_FULL_GROUP_BY only one of these may be true.
  */
  bool non_agg_field_used() const { return m_non_agg_field_used; }
  bool agg_func_used()      const { return m_agg_func_used; }
  bool custom_agg_func_used() const { return m_custom_agg_func_used; }

  void set_non_agg_field_used(bool val) { m_non_agg_field_used= val; }
  void set_agg_func_used(bool val)      { m_agg_func_used= val; }
  void set_custom_agg_func_used(bool val) { m_custom_agg_func_used= val; }
  inline void set_with_clause(With_clause *with_clause);
  With_clause *get_with_clause()
  {
    return master_unit()->with_clause;
  }
  With_element *get_with_element()
  {
    return master_unit()->cloned_from ?
           master_unit()->cloned_from->with_element :
           master_unit()->with_element;
  }
  With_element *find_table_def_in_with_clauses(TABLE_LIST *table);
  bool check_unrestricted_recursive(bool only_standard_compliant);
  bool check_subqueries_with_recursive_references();
  void collect_grouping_fields_for_derived(THD *thd, ORDER *grouping_list);
  bool collect_grouping_fields(THD *thd);
  bool collect_fields_equal_to_grouping(THD *thd);
  void check_cond_extraction_for_grouping_fields(THD *thd, Item *cond);
  Item *build_cond_for_grouping_fields(THD *thd, Item *cond,
                                       bool no_to_clones);
  
  List<Window_spec> window_specs;
  bool is_win_spec_list_built;
  void prepare_add_window_spec(THD *thd);
  bool add_window_def(THD *thd, LEX_CSTRING *win_name, LEX_CSTRING *win_ref,
                      SQL_I_List<ORDER> win_partition_list,
                      SQL_I_List<ORDER> win_order_list,
                      Window_frame *win_frame);
  bool add_window_spec(THD *thd, LEX_CSTRING *win_ref,
                       SQL_I_List<ORDER> win_partition_list,
                       SQL_I_List<ORDER> win_order_list,
                       Window_frame *win_frame);
  List<Item_window_func> window_funcs;
  bool add_window_func(Item_window_func *win_func);

  bool have_window_funcs() const { return (window_funcs.elements !=0); }
  ORDER *find_common_window_func_partition_fields(THD *thd);

  bool cond_pushdown_is_allowed() const
  { return !olap && !limit_params.explicit_limit && !tvc && !with_rownum; }
  
  bool build_pushable_cond_for_having_pushdown(THD *thd, Item *cond);
  void pushdown_cond_into_where_clause(THD *thd, Item *extracted_cond,
                                       Item **remaining_cond,
                                       Item_transformer transformer,
                                       uchar *arg);
  Item *pushdown_from_having_into_where(THD *thd, Item *having);

  bool is_set_op()
  {
    return linkage == UNION_TYPE || 
           linkage == EXCEPT_TYPE || 
           linkage == INTERSECT_TYPE;
  }

  inline void add_where_field(st_select_lex *sel)
  {
    DBUG_ASSERT(this != sel);
    select_n_where_fields+= sel->select_n_where_fields;
  }
  inline void set_linkage_and_distinct(enum sub_select_type l, bool d)
  {
    DBUG_ENTER("SELECT_LEX::set_linkage_and_distinct");
    DBUG_PRINT("info", ("select: %p  distinct %d", this, d));
    set_linkage(l);
    DBUG_ASSERT(l == UNION_TYPE ||
                l == INTERSECT_TYPE ||
                l == EXCEPT_TYPE);
    if (d && master_unit() && master_unit()->union_distinct != this)
      master_unit()->union_distinct= this;
    distinct= d;
    with_all_modifier= !distinct;
    DBUG_VOID_RETURN;
  }
  bool set_nest_level(int new_nest_level);
  bool check_parameters(SELECT_LEX *main_select);
  void mark_select()
  {
    DBUG_ENTER("st_select_lex::mark_select()");
    DBUG_PRINT("info", ("Select #%d", select_number));
    DBUG_VOID_RETURN;
  }
  void register_unit(SELECT_LEX_UNIT *unit,
                     Name_resolution_context *outer_context);
  SELECT_LEX_UNIT *attach_selects_chain(SELECT_LEX *sel,
                                        Name_resolution_context *context);
  void add_statistics(SELECT_LEX_UNIT *unit);
  bool make_unique_derived_name(THD *thd, LEX_CSTRING *alias);
  void lex_start(LEX *plex);
  bool is_unit_nest() { return (nest_flags & UNIT_NEST_FL); }
  void mark_as_unit_nest() { nest_flags= UNIT_NEST_FL; }
  bool is_sj_conversion_prohibited(THD *thd);
};
typedef class st_select_lex SELECT_LEX;

inline bool st_select_lex_unit::is_unit_op ()
{
  if (!first_select()->next_select())
  {
    if (first_select()->tvc)
      return 1;
    else
      return 0;
  }

  enum sub_select_type linkage= first_select()->next_select()->linkage;
  return linkage == UNION_TYPE || linkage == INTERSECT_TYPE ||
    linkage == EXCEPT_TYPE;
}


struct st_sp_chistics
{
  LEX_CSTRING comment;
  enum enum_sp_suid_behaviour suid;
  bool detistic;
  enum enum_sp_data_access daccess;
  enum enum_sp_aggregate_type agg_type;
  void init() { bzero(this, sizeof(*this)); }
  void set(const st_sp_chistics &other) { *this= other; }
  bool read_from_mysql_proc_row(THD *thd, TABLE *table);
};


class Sp_chistics: public st_sp_chistics
{
public:
  Sp_chistics() { init(); }
};


struct st_trg_chistics: public st_trg_execution_order
{
  enum trg_action_time_type action_time;
  enum trg_event_type event;

  const char *ordering_clause_begin;
  const char *ordering_clause_end;

};

enum xa_option_words {XA_NONE, XA_JOIN, XA_RESUME, XA_ONE_PHASE,
                      XA_SUSPEND, XA_FOR_MIGRATE};

class Sroutine_hash_entry;

/*
  Class representing list of all tables used by statement and other
  information which is necessary for opening and locking its tables,
  like SQL command for this statement.

  Also contains information about stored functions used by statement
  since during its execution we may have to add all tables used by its
  stored functions/triggers to this list in order to pre-open and lock
  them.

  Also used by LEX::reset_n_backup/restore_backup_query_tables_list()
  methods to save and restore this information.
*/

class Query_tables_list
{
public:
  /**
    SQL command for this statement. Part of this class since the
    process of opening and locking tables for the statement needs
    this information to determine correct type of lock for some of
    the tables.
  */
  enum_sql_command sql_command;
  /* Global list of all tables used by this statement */
  TABLE_LIST *query_tables;
  /* Pointer to next_global member of last element in the previous list. */
  TABLE_LIST **query_tables_last;
  /*
    If non-0 then indicates that query requires prelocking and points to
    next_global member of last own element in query table list (i.e. last
    table which was not added to it as part of preparation to prelocking).
    0 - indicates that this query does not need prelocking.
  */
  TABLE_LIST **query_tables_own_last;
  /*
    Set of stored routines called by statement.
    (Note that we use lazy-initialization for this hash).
  */
  enum { START_SROUTINES_HASH_SIZE= 16 };
  HASH sroutines;
  /*
    List linking elements of 'sroutines' set. Allows you to add new elements
    to this set as you iterate through the list of existing elements.
    'sroutines_list_own_last' is pointer to ::next member of last element of
    this list which represents routine which is explicitly used by query.
    'sroutines_list_own_elements' number of explicitly used routines.
    We use these two members for restoring of 'sroutines_list' to the state
    in which it was right after query parsing.
  */
  SQL_I_List<Sroutine_hash_entry> sroutines_list;
  Sroutine_hash_entry **sroutines_list_own_last;
  uint sroutines_list_own_elements;

  /**
    Locking state of tables in this particular statement.

    If we under LOCK TABLES or in prelocked mode we consider tables
    for the statement to be "locked" if there was a call to lock_tables()
    (which called handler::start_stmt()) for tables of this statement
    and there was no matching close_thread_tables() call.

    As result this state may differ significantly from one represented
    by Open_tables_state::lock/locked_tables_mode more, which are always
    "on" under LOCK TABLES or in prelocked mode.
  */
  enum enum_lock_tables_state { LTS_NOT_LOCKED = 0, LTS_LOCKED };
  enum_lock_tables_state lock_tables_state;
  bool is_query_tables_locked() const
  {
    return (lock_tables_state == LTS_LOCKED);
  }

   /*
    These constructor and destructor serve for creation/destruction
    of Query_tables_list instances which are used as backup storage.
  */
  Query_tables_list() : lock_tables_state(LTS_NOT_LOCKED) {}
  ~Query_tables_list() {}

  /* Initializes (or resets) Query_tables_list object for "real" use. */
  void reset_query_tables_list(bool init);
  void destroy_query_tables_list();
  void set_query_tables_list(Query_tables_list *state)
  {
    *this= *state;
  }

  /*
    Direct addition to the list of query tables.
    If you are using this function, you must ensure that the table
    object, in particular table->db member, is initialized.
  */
  void add_to_query_tables(TABLE_LIST *table)
  {
    *(table->prev_global= query_tables_last)= table;
    query_tables_last= &table->next_global;
  }
  bool requires_prelocking()
  {
    return MY_TEST(query_tables_own_last);
  }
  void mark_as_requiring_prelocking(TABLE_LIST **tables_own_last)
  {
    query_tables_own_last= tables_own_last;
  }
  /* Return pointer to first not-own table in query-tables or 0 */
  TABLE_LIST* first_not_own_table()
  {
    return ( query_tables_own_last ? *query_tables_own_last : 0);
  }
  void chop_off_not_own_tables()
  {
    if (query_tables_own_last)
    {
      *query_tables_own_last= 0;
      query_tables_last= query_tables_own_last;
      query_tables_own_last= 0;
    }
  }

  /** Return a pointer to the last element in query table list. */
  TABLE_LIST *last_table()
  {
    /* Don't use offsetof() macro in order to avoid warnings. */
    return query_tables ?
           (TABLE_LIST*) ((char*) query_tables_last -
                          ((char*) &(query_tables->next_global) -
                           (char*) query_tables)) :
           0;
  }

  /**
    Enumeration listing of all types of unsafe statement.

    @note The order of elements of this enumeration type must
    correspond to the order of the elements of the @c explanations
    array defined in the body of @c THD::issue_unsafe_warnings.
  */
  enum enum_binlog_stmt_unsafe {
    /**
      SELECT..LIMIT is unsafe because the set of rows returned cannot
      be predicted.
    */
    BINLOG_STMT_UNSAFE_LIMIT= 0,
    /**
      INSERT DELAYED is unsafe because the time when rows are inserted
      cannot be predicted.
    */
    BINLOG_STMT_UNSAFE_INSERT_DELAYED,
    /**
      Access to log tables is unsafe because slave and master probably
      log different things.
    */
    BINLOG_STMT_UNSAFE_SYSTEM_TABLE,
    /**
      Inserting into an autoincrement column in a stored routine is unsafe.
      Even with just one autoincrement column, if the routine is invoked more than 
      once slave is not guaranteed to execute the statement graph same way as 
      the master.
      And since it's impossible to estimate how many times a routine can be invoked at 
      the query pre-execution phase (see lock_tables), the statement is marked
      pessimistically unsafe. 
    */
    BINLOG_STMT_UNSAFE_AUTOINC_COLUMNS,
    /**
      Using a UDF (user-defined function) is unsafe.
    */
    BINLOG_STMT_UNSAFE_UDF,
    /**
      Using most system variables is unsafe, because slave may run
      with different options than master.
    */
    BINLOG_STMT_UNSAFE_SYSTEM_VARIABLE,
    /**
      Using some functions is unsafe (e.g., UUID).
    */
    BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION,

    /**
      Mixing transactional and non-transactional statements are unsafe if
      non-transactional reads or writes are occur after transactional
      reads or writes inside a transaction.
    */
    BINLOG_STMT_UNSAFE_NONTRANS_AFTER_TRANS,

    /**
      Mixing self-logging and non-self-logging engines in a statement
      is unsafe.
    */
    BINLOG_STMT_UNSAFE_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE,

    /**
      Statements that read from both transactional and non-transactional
      tables and write to any of them are unsafe.
    */
    BINLOG_STMT_UNSAFE_MIXED_STATEMENT,

    /**
      INSERT...IGNORE SELECT is unsafe because which rows are ignored depends
      on the order that rows are retrieved by SELECT. This order cannot be
      predicted and may differ on master and the slave.
    */
    BINLOG_STMT_UNSAFE_INSERT_IGNORE_SELECT,

    /**
      INSERT...SELECT...UPDATE is unsafe because which rows are updated depends
      on the order that rows are retrieved by SELECT. This order cannot be
      predicted and may differ on master and the slave.
    */
    BINLOG_STMT_UNSAFE_INSERT_SELECT_UPDATE,

    /**
     Query that writes to a table with auto_inc column after selecting from 
     other tables are unsafe as the order in which the rows are retrieved by
     select may differ on master and slave.
    */
    BINLOG_STMT_UNSAFE_WRITE_AUTOINC_SELECT,

    /**
      INSERT...REPLACE SELECT is unsafe because which rows are replaced depends
      on the order that rows are retrieved by SELECT. This order cannot be
      predicted and may differ on master and the slave.
    */
    BINLOG_STMT_UNSAFE_REPLACE_SELECT,

    /**
      CREATE TABLE... IGNORE... SELECT is unsafe because which rows are ignored
      depends on the order that rows are retrieved by SELECT. This order cannot
      be predicted and may differ on master and the slave.
    */
    BINLOG_STMT_UNSAFE_CREATE_IGNORE_SELECT,

    /**
      CREATE TABLE...REPLACE... SELECT is unsafe because which rows are replaced
      depends on the order that rows are retrieved from SELECT. This order
      cannot be predicted and may differ on master and the slave
    */
    BINLOG_STMT_UNSAFE_CREATE_REPLACE_SELECT,

    /**
      CREATE TABLE...SELECT on a table with auto-increment column is unsafe
      because which rows are replaced depends on the order that rows are
      retrieved from SELECT. This order cannot be predicted and may differ on
      master and the slave
    */
    BINLOG_STMT_UNSAFE_CREATE_SELECT_AUTOINC,

    /**
      UPDATE...IGNORE is unsafe because which rows are ignored depends on the
      order that rows are updated. This order cannot be predicted and may differ
      on master and the slave.
    */
    BINLOG_STMT_UNSAFE_UPDATE_IGNORE,

    /**
      INSERT... ON DUPLICATE KEY UPDATE on a table with more than one
      UNIQUE KEYS  is unsafe.
    */
    BINLOG_STMT_UNSAFE_INSERT_TWO_KEYS,

    /**
       INSERT into auto-inc field which is not the first part of composed
       primary key.
    */
    BINLOG_STMT_UNSAFE_AUTOINC_NOT_FIRST,

    /**
       Autoincrement lock mode is incompatible with STATEMENT binlog format.
    */
    BINLOG_STMT_UNSAFE_AUTOINC_LOCK_MODE,

    /**
       INSERT .. SELECT ... SKIP LOCKED is unlikely to have the same
       rows locked on the replica.
       primary key.
    */
    BINLOG_STMT_UNSAFE_SKIP_LOCKED,

    /* The last element of this enumeration type. */
    BINLOG_STMT_UNSAFE_COUNT
  };
  /**
    This has all flags from 0 (inclusive) to BINLOG_STMT_FLAG_COUNT
    (exclusive) set.
  */
  static const uint32 BINLOG_STMT_UNSAFE_ALL_FLAGS=
    ((1U << BINLOG_STMT_UNSAFE_COUNT) - 1);

  /**
    Maps elements of enum_binlog_stmt_unsafe to error codes.
  */
  static const int binlog_stmt_unsafe_errcode[BINLOG_STMT_UNSAFE_COUNT];

  /**
    Determine if this statement is marked as unsafe.

    @retval 0 if the statement is not marked as unsafe.
    @retval nonzero if the statement is marked as unsafe.
  */
  inline bool is_stmt_unsafe() const {
    return get_stmt_unsafe_flags() != 0;
  }

  inline bool is_stmt_unsafe(enum_binlog_stmt_unsafe unsafe)
  {
    return binlog_stmt_flags & (1 << unsafe);
  }

  /**
    Flag the current (top-level) statement as unsafe.
    The flag will be reset after the statement has finished.

    @param unsafe_type The type of unsafety: one of the @c
    BINLOG_STMT_FLAG_UNSAFE_* flags in @c enum_binlog_stmt_flag.
  */
  inline void set_stmt_unsafe(enum_binlog_stmt_unsafe unsafe_type) {
    DBUG_ENTER("set_stmt_unsafe");
    DBUG_ASSERT(unsafe_type >= 0 && unsafe_type < BINLOG_STMT_UNSAFE_COUNT);
    binlog_stmt_flags|= (1U << unsafe_type);
    DBUG_VOID_RETURN;
  }

  /**
    Set the bits of binlog_stmt_flags determining the type of
    unsafeness of the current statement.  No existing bits will be
    cleared, but new bits may be set.

    @param flags A binary combination of zero or more bits, (1<<flag)
    where flag is a member of enum_binlog_stmt_unsafe.
  */
  inline void set_stmt_unsafe_flags(uint32 flags) {
    DBUG_ENTER("set_stmt_unsafe_flags");
    DBUG_ASSERT((flags & ~BINLOG_STMT_UNSAFE_ALL_FLAGS) == 0);
    binlog_stmt_flags|= flags;
    DBUG_VOID_RETURN;
  }

  /**
    Return a binary combination of all unsafe warnings for the
    statement.  If the statement has been marked as unsafe by the
    'flag' member of enum_binlog_stmt_unsafe, then the return value
    from this function has bit (1<<flag) set to 1.
  */
  inline uint32 get_stmt_unsafe_flags() const {
    DBUG_ENTER("get_stmt_unsafe_flags");
    DBUG_RETURN(binlog_stmt_flags & BINLOG_STMT_UNSAFE_ALL_FLAGS);
  }

  /**
    Mark the current statement as safe; i.e., clear all bits in
    binlog_stmt_flags that correspond to elements of
    enum_binlog_stmt_unsafe.
  */
  inline void clear_stmt_unsafe() {
    DBUG_ENTER("clear_stmt_unsafe");
    binlog_stmt_flags&= ~BINLOG_STMT_UNSAFE_ALL_FLAGS;
    DBUG_VOID_RETURN;
  }

  /**
    Determine if this statement is a row injection.

    @retval 0 if the statement is not a row injection
    @retval nonzero if the statement is a row injection
  */
  inline bool is_stmt_row_injection() const {
    return binlog_stmt_flags & (1U << BINLOG_STMT_TYPE_ROW_INJECTION);
  }

  /**
    Flag the statement as a row injection.  A row injection is either
    a BINLOG statement, or a row event in the relay log executed by
    the slave SQL thread.
  */
  inline void set_stmt_row_injection() {
    DBUG_ENTER("set_stmt_row_injection");
    binlog_stmt_flags|= (1U << BINLOG_STMT_TYPE_ROW_INJECTION);
    DBUG_VOID_RETURN;
  }

  enum enum_stmt_accessed_table
  {
    /*
       If a transactional table is about to be read. Note that
       a write implies a read.
    */
    STMT_READS_TRANS_TABLE= 0,
    /*
       If a non-transactional table is about to be read. Note that
       a write implies a read.
    */
    STMT_READS_NON_TRANS_TABLE,
    /*
       If a temporary transactional table is about to be read. Note
       that a write implies a read.
    */
    STMT_READS_TEMP_TRANS_TABLE,
    /*
       If a temporary non-transactional table is about to be read. Note
      that a write implies a read.
    */
    STMT_READS_TEMP_NON_TRANS_TABLE,
    /*
       If a transactional table is about to be updated.
    */
    STMT_WRITES_TRANS_TABLE,
    /*
       If a non-transactional table is about to be updated.
    */
    STMT_WRITES_NON_TRANS_TABLE,
    /*
       If a temporary transactional table is about to be updated.
    */
    STMT_WRITES_TEMP_TRANS_TABLE,
    /*
       If a temporary non-transactional table is about to be updated.
    */
    STMT_WRITES_TEMP_NON_TRANS_TABLE,
    /*
      The last element of the enumeration. Please, if necessary add
      anything before this.
    */
    STMT_ACCESS_TABLE_COUNT
  };

#ifndef DBUG_OFF
  static inline const char *stmt_accessed_table_string(enum_stmt_accessed_table accessed_table)
  {
    switch (accessed_table)
    {
      case STMT_READS_TRANS_TABLE:
         return "STMT_READS_TRANS_TABLE";
      break;
      case STMT_READS_NON_TRANS_TABLE:
        return "STMT_READS_NON_TRANS_TABLE";
      break;
      case STMT_READS_TEMP_TRANS_TABLE:
        return "STMT_READS_TEMP_TRANS_TABLE";
      break;
      case STMT_READS_TEMP_NON_TRANS_TABLE:
        return "STMT_READS_TEMP_NON_TRANS_TABLE";
      break;  
      case STMT_WRITES_TRANS_TABLE:
        return "STMT_WRITES_TRANS_TABLE";
      break;
      case STMT_WRITES_NON_TRANS_TABLE:
        return "STMT_WRITES_NON_TRANS_TABLE";
      break;
      case STMT_WRITES_TEMP_TRANS_TABLE:
        return "STMT_WRITES_TEMP_TRANS_TABLE";
      break;
      case STMT_WRITES_TEMP_NON_TRANS_TABLE:
        return "STMT_WRITES_TEMP_NON_TRANS_TABLE";
      break;
      case STMT_ACCESS_TABLE_COUNT:
      default:
        DBUG_ASSERT(0);
      break;
    }
    MY_ASSERT_UNREACHABLE();
    return "";
  }
#endif  /* DBUG */
               
  #define BINLOG_DIRECT_ON 0xF0    /* unsafe when
                                      --binlog-direct-non-trans-updates
                                      is ON */

  #define BINLOG_DIRECT_OFF 0xF    /* unsafe when
                                      --binlog-direct-non-trans-updates
                                      is OFF */

  #define TRX_CACHE_EMPTY 0x33     /* unsafe when trx-cache is empty */

  #define TRX_CACHE_NOT_EMPTY 0xCC /* unsafe when trx-cache is not empty */

  #define IL_LT_REPEATABLE 0xAA    /* unsafe when < ISO_REPEATABLE_READ */

  #define IL_GTE_REPEATABLE 0x55   /* unsafe when >= ISO_REPEATABLE_READ */
  
  /**
    Sets the type of table that is about to be accessed while executing a
    statement.

    @param accessed_table Enumeration type that defines the type of table,
                           e.g. temporary, transactional, non-transactional.
  */
  inline void set_stmt_accessed_table(enum_stmt_accessed_table accessed_table)
  {
    DBUG_ENTER("LEX::set_stmt_accessed_table");

    DBUG_ASSERT(accessed_table >= 0 && accessed_table < STMT_ACCESS_TABLE_COUNT);
    stmt_accessed_table_flag |= (1U << accessed_table);

    DBUG_VOID_RETURN;
  }

  /**
    Checks if a type of table is about to be accessed while executing a
    statement.

    @param accessed_table Enumeration type that defines the type of table,
           e.g. temporary, transactional, non-transactional.

    @return
      @retval TRUE  if the type of the table is about to be accessed
      @retval FALSE otherwise
  */
  inline bool stmt_accessed_table(enum_stmt_accessed_table accessed_table)
  {
    DBUG_ENTER("LEX::stmt_accessed_table");

    DBUG_ASSERT(accessed_table >= 0 && accessed_table < STMT_ACCESS_TABLE_COUNT);

    DBUG_RETURN((stmt_accessed_table_flag & (1U << accessed_table)) != 0);
  }

  /**
    Checks either a trans/non trans temporary table is being accessed while
    executing a statement.

    @return
      @retval TRUE  if a temporary table is being accessed
      @retval FALSE otherwise
  */
  inline bool stmt_accessed_temp_table()
  {
    DBUG_ENTER("THD::stmt_accessed_temp_table");
    DBUG_RETURN(stmt_accessed_non_trans_temp_table() ||
                stmt_accessed_trans_temp_table());
  }

  /**
    Checks if a temporary transactional table is being accessed while executing
    a statement.

    @return
      @retval TRUE  if a temporary transactional table is being accessed
      @retval FALSE otherwise
  */
  inline bool stmt_accessed_trans_temp_table()
  {
    DBUG_ENTER("THD::stmt_accessed_trans_temp_table");

    DBUG_RETURN((stmt_accessed_table_flag &
                ((1U << STMT_READS_TEMP_TRANS_TABLE) |
                 (1U << STMT_WRITES_TEMP_TRANS_TABLE))) != 0);
  }
  inline bool stmt_writes_to_non_temp_table()
  {
    DBUG_ENTER("THD::stmt_writes_to_non_temp_table");

    DBUG_RETURN((stmt_accessed_table_flag &
                ((1U << STMT_WRITES_TRANS_TABLE) |
                 (1U << STMT_WRITES_NON_TRANS_TABLE))));
  }

  /**
    Checks if a temporary non-transactional table is about to be accessed
    while executing a statement.

    @return
      @retval TRUE  if a temporary non-transactional table is about to be
                    accessed
      @retval FALSE otherwise
  */
  inline bool stmt_accessed_non_trans_temp_table()
  {
    DBUG_ENTER("THD::stmt_accessed_non_trans_temp_table");

    DBUG_RETURN((stmt_accessed_table_flag &
                ((1U << STMT_READS_TEMP_NON_TRANS_TABLE) |
                 (1U << STMT_WRITES_TEMP_NON_TRANS_TABLE))) != 0);
  }

  /*
    Checks if a mixed statement is unsafe.

    
    @param in_multi_stmt_transaction_mode defines if there is an on-going
           multi-transactional statement.
    @param binlog_direct defines if --binlog-direct-non-trans-updates is
           active.
    @param trx_cache_is_not_empty defines if the trx-cache is empty or not.
    @param trx_isolation defines the isolation level.
 
    @return
      @retval TRUE if the mixed statement is unsafe
      @retval FALSE otherwise
  */
  inline bool is_mixed_stmt_unsafe(bool in_multi_stmt_transaction_mode,
                                   bool binlog_direct,
                                   bool trx_cache_is_not_empty,
                                   uint tx_isolation)
  {
    bool unsafe= FALSE;

    if (in_multi_stmt_transaction_mode)
    {
       uint condition=
         (binlog_direct ? BINLOG_DIRECT_ON : BINLOG_DIRECT_OFF) &
         (trx_cache_is_not_empty ? TRX_CACHE_NOT_EMPTY : TRX_CACHE_EMPTY) &
         (tx_isolation >= ISO_REPEATABLE_READ ? IL_GTE_REPEATABLE : IL_LT_REPEATABLE);

      unsafe= (binlog_unsafe_map[stmt_accessed_table_flag] & condition);

#if !defined(DBUG_OFF)
      DBUG_PRINT("LEX::is_mixed_stmt_unsafe", ("RESULT %02X %02X %02X", condition,
              binlog_unsafe_map[stmt_accessed_table_flag],
              (binlog_unsafe_map[stmt_accessed_table_flag] & condition)));
 
      int type_in= 0;
      for (; type_in < STMT_ACCESS_TABLE_COUNT; type_in++)
      {
        if (stmt_accessed_table((enum_stmt_accessed_table) type_in))
          DBUG_PRINT("LEX::is_mixed_stmt_unsafe", ("ACCESSED %s ",
                  stmt_accessed_table_string((enum_stmt_accessed_table) type_in)));
      }
#endif
    }

    if (stmt_accessed_table(STMT_WRITES_NON_TRANS_TABLE) &&
      stmt_accessed_table(STMT_READS_TRANS_TABLE) &&
      tx_isolation < ISO_REPEATABLE_READ)
      unsafe= TRUE;
    else if (stmt_accessed_table(STMT_WRITES_TEMP_NON_TRANS_TABLE) &&
      stmt_accessed_table(STMT_READS_TRANS_TABLE) &&
      tx_isolation < ISO_REPEATABLE_READ)
      unsafe= TRUE;

    return(unsafe);
  }

  /**
    true if the parsed tree contains references to stored procedures
    or functions, false otherwise
  */
  bool uses_stored_routines() const
  { return sroutines_list.elements != 0; }

  void set_date_funcs_used_flag()
  {
    date_funcs_used_flag= true;
  }

  /*
    Returns TRUE if date functions such as YEAR(), MONTH() or DATE()
    are used in this LEX
  */
  bool are_date_funcs_used() const
  {
    return date_funcs_used_flag;
  }

private:

  /**
    Enumeration listing special types of statements.

    Currently, the only possible type is ROW_INJECTION.
  */
  enum enum_binlog_stmt_type {
    /**
      The statement is a row injection (i.e., either a BINLOG
      statement or a row event executed by the slave SQL thread).
    */
    BINLOG_STMT_TYPE_ROW_INJECTION = BINLOG_STMT_UNSAFE_COUNT,

    /** The last element of this enumeration type. */
    BINLOG_STMT_TYPE_COUNT
  };

  /**
    Bit field indicating the type of statement.

    There are two groups of bits:

    - The low BINLOG_STMT_UNSAFE_COUNT bits indicate the types of
      unsafeness that the current statement has.

      - The next BINLOG_STMT_TYPE_COUNT-BINLOG_STMT_TYPE_COUNT bits indicate if
      the statement is of some special type.

    This must be a member of LEX, not of THD: each stored procedure
    needs to remember its unsafeness state between calls and each
    stored procedure has its own LEX object (but no own THD object).
  */
  uint32 binlog_stmt_flags;

  /**
    Bit field that determines the type of tables that are about to be
    be accessed while executing a statement.
  */
  uint32 stmt_accessed_table_flag;

  /*
    Flag indicating that date functions such as YEAR(), MONTH() or DATE() are
    used in this LEX
  */
  bool date_funcs_used_flag= false;
};


/*
  st_parsing_options contains the flags for constructions that are
  allowed in the current statement.
*/

struct st_parsing_options
{
  bool allows_variable;
  bool lookup_keywords_after_qualifier;

  st_parsing_options() { reset(); }
  void reset();
};


/**
  The state of the lexical parser, when parsing comments.
*/
enum enum_comment_state
{
  /**
    Not parsing comments.
  */
  NO_COMMENT,
  /**
    Parsing comments that need to be preserved.
    Typically, these are user comments '/' '*' ... '*' '/'.
  */
  PRESERVE_COMMENT,
  /**
    Parsing comments that need to be discarded.
    Typically, these are special comments '/' '*' '!' ... '*' '/',
    or '/' '*' '!' 'M' 'M' 'm' 'm' 'm' ... '*' '/', where the comment
    markers should not be expanded.
  */
  DISCARD_COMMENT
};


/**
  @brief This class represents the character input stream consumed during
  lexical analysis.

  In addition to consuming the input stream, this class performs some
  comment pre processing, by filtering out out of bound special text
  from the query input stream.
  Two buffers, with pointers inside each buffers, are maintained in
  parallel. The 'raw' buffer is the original query text, which may
  contain out-of-bound comments. The 'cpp' (for comments pre processor)
  is the pre-processed buffer that contains only the query text that
  should be seen once out-of-bound data is removed.
*/

class Lex_input_stream
{
  size_t unescape(CHARSET_INFO *cs, char *to,
                  const char *str, const char *end, int sep);
  my_charset_conv_wc_mb get_escape_func(THD *thd, my_wc_t sep) const;
public:
  Lex_input_stream() = default;

  ~Lex_input_stream() = default;

  /**
     Object initializer. Must be called before usage.

     @retval FALSE OK
     @retval TRUE  Error
  */
  bool init(THD *thd, char *buff, size_t length);

  void reset(char *buff, size_t length);

  /**
    The main method to scan the next token, with token contraction processing
    for LALR(2) resolution, e.g. translate "WITH" followed by "ROLLUP"
    to a single token WITH_ROLLUP_SYM.
  */
  int lex_token(union YYSTYPE *yylval, THD *thd);

  void reduce_digest_token(uint token_left, uint token_right);

private:
  /**
    Set the echo mode.

    When echo is true, characters parsed from the raw input stream are
    preserved. When false, characters parsed are silently ignored.
    @param echo the echo mode.
  */
  void set_echo(bool echo)
  {
    m_echo= echo;
  }

  void save_in_comment_state()
  {
    m_echo_saved= m_echo;
    in_comment_saved= in_comment;
  }

  void restore_in_comment_state()
  {
    m_echo= m_echo_saved;
    in_comment= in_comment_saved;
  }

  /**
    Skip binary from the input stream.
    @param n number of bytes to accept.
  */
  void skip_binary(int n)
  {
    if (m_echo)
    {
      memcpy(m_cpp_ptr, m_ptr, n);
      m_cpp_ptr += n;
    }
    m_ptr += n;
  }

  /**
    Get a character, and advance in the stream.
    @return the next character to parse.
  */
  unsigned char yyGet()
  {
    char c= *m_ptr++;
    if (m_echo)
      *m_cpp_ptr++ = c;
    return c;
  }

  /**
    Get the last character accepted.
    @return the last character accepted.
  */
  unsigned char yyGetLast() const
  {
    return m_ptr[-1];
  }

  /**
    Look at the next character to parse, but do not accept it.
  */
  unsigned char yyPeek() const
  {
    return m_ptr[0];
  }

  /**
    Look ahead at some character to parse.
    @param n offset of the character to look up
  */
  unsigned char yyPeekn(int n) const
  {
    return m_ptr[n];
  }

  /**
    Cancel the effect of the last yyGet() or yySkip().
    Note that the echo mode should not change between calls to yyGet / yySkip
    and yyUnget. The caller is responsible for ensuring that.
  */
  void yyUnget()
  {
    m_ptr--;
    if (m_echo)
      m_cpp_ptr--;
  }

  /**
    Accept a character, by advancing the input stream.
  */
  void yySkip()
  {
    if (m_echo)
      *m_cpp_ptr++ = *m_ptr++;
    else
      m_ptr++;
  }

  /**
    Accept multiple characters at once.
    @param n the number of characters to accept.
  */
  void yySkipn(int n)
  {
    if (m_echo)
    {
      memcpy(m_cpp_ptr, m_ptr, n);
      m_cpp_ptr += n;
    }
    m_ptr += n;
  }

  /**
    Puts a character back into the stream, canceling
    the effect of the last yyGet() or yySkip().
    Note that the echo mode should not change between calls
    to unput, get, or skip from the stream.
  */
  char *yyUnput(char ch)
  {
    *--m_ptr= ch;
    if (m_echo)
      m_cpp_ptr--;
    return m_ptr;
  }

  /**
    End of file indicator for the query text to parse.
    @param n number of characters expected
    @return true if there are less than n characters to parse
  */
  bool eof(int n) const
  {
    return ((m_ptr + n) >= m_end_of_query);
  }

  /** Mark the stream position as the start of a new token. */
  void start_token()
  {
    m_tok_start_prev= m_tok_start;
    m_tok_start= m_ptr;
    m_tok_end= m_ptr;

    m_cpp_tok_start_prev= m_cpp_tok_start;
    m_cpp_tok_start= m_cpp_ptr;
    m_cpp_tok_end= m_cpp_ptr;
  }

  /**
    Adjust the starting position of the current token.
    This is used to compensate for starting whitespace.
  */
  void restart_token()
  {
    m_tok_start= m_ptr;
    m_cpp_tok_start= m_cpp_ptr;
  }

  /**
    Get the maximum length of the utf8-body buffer.
    The utf8 body can grow because of the character set conversion and escaping.
  */
  size_t get_body_utf8_maximum_length(THD *thd) const;

  /** Get the length of the current token, in the raw buffer. */
  uint yyLength() const
  {
    /*
      The assumption is that the lexical analyser is always 1 character ahead,
      which the -1 account for.
    */
    DBUG_ASSERT(m_ptr > m_tok_start);
    return (uint) ((m_ptr - m_tok_start) - 1);
  }

  /**
    Test if a lookahead token was already scanned by lex_token(),
    for LALR(2) resolution.
  */
  bool has_lookahead() const
  {
    return lookahead_token >= 0;
  }

public:

  /**
    End of file indicator for the query text to parse.
    @return true if there are no more characters to parse
  */
  bool eof() const
  {
    return (m_ptr >= m_end_of_query);
  }

  /** Get the raw query buffer. */
  const char *get_buf() const
  {
    return m_buf;
  }

  /** Get the pre-processed query buffer. */
  const char *get_cpp_buf() const
  {
    return m_cpp_buf;
  }

  /** Get the end of the raw query buffer. */
  const char *get_end_of_query() const
  {
    return m_end_of_query;
  }

  /** Get the token start position, in the raw buffer. */
  const char *get_tok_start() const
  {
    return has_lookahead() ? m_tok_start_prev : m_tok_start;
  }

  void set_cpp_tok_start(const char *pos)
  {
    m_cpp_tok_start= pos;
  }

  /** Get the token end position, in the raw buffer. */
  const char *get_tok_end() const
  {
    return m_tok_end;
  }

  /** Get the current stream pointer, in the raw buffer. */
  const char *get_ptr() const
  {
    return m_ptr;
  }

  /** Get the token start position, in the pre-processed buffer. */
  const char *get_cpp_tok_start() const
  {
    return has_lookahead() ? m_cpp_tok_start_prev : m_cpp_tok_start;
  }

  /** Get the token end position, in the pre-processed buffer. */
  const char *get_cpp_tok_end() const
  {
    return m_cpp_tok_end;
  }

  /**
    Get the token end position in the pre-processed buffer,
    with trailing spaces removed.
  */
  const char *get_cpp_tok_end_rtrim() const
  {
    const char *p;
    for (p= m_cpp_tok_end;
         p > m_cpp_buf && my_isspace(system_charset_info, p[-1]);
         p--)
    { }
    return p;
  }

  /** Get the current stream pointer, in the pre-processed buffer. */
  const char *get_cpp_ptr() const
  {
    return m_cpp_ptr;
  }

  /**
    Get the current stream pointer, in the pre-processed buffer,
    with traling spaces removed.
  */
  const char *get_cpp_ptr_rtrim() const
  {
    const char *p;
    for (p= m_cpp_ptr;
         p > m_cpp_buf && my_isspace(system_charset_info, p[-1]);
         p--)
    { }
    return p;
  }
  /** Get the utf8-body string. */
  LEX_CSTRING body_utf8() const
  {
    return LEX_CSTRING({m_body_utf8, (size_t) (m_body_utf8_ptr - m_body_utf8)});
  }

  void body_utf8_start(THD *thd, const char *begin_ptr);
  void body_utf8_append(const char *ptr);
  void body_utf8_append(const char *ptr, const char *end_ptr);
  void body_utf8_append_ident(THD *thd,
                              const Lex_string_with_metadata_st *txt,
                              const char *end_ptr);
  void body_utf8_append_escape(THD *thd,
                               const LEX_CSTRING *txt,
                               CHARSET_INFO *txt_cs,
                               const char *end_ptr,
                               my_wc_t sep);

private:
  /**
    LALR(2) resolution, look ahead token.
    Value of the next token to return, if any,
    or -1, if no token was parsed in advance.
    Note: 0 is a legal token, and represents YYEOF.
  */
  int lookahead_token;

  /** LALR(2) resolution, value of the look ahead token.*/
  LEX_YYSTYPE lookahead_yylval;

  bool get_text(Lex_string_with_metadata_st *to,
                uint sep, int pre_skip, int post_skip);

  void add_digest_token(uint token, LEX_YYSTYPE yylval);

  bool consume_comment(int remaining_recursions_permitted);
  int lex_one_token(union YYSTYPE *yylval, THD *thd);
  int find_keyword(Lex_ident_cli_st *str, uint len, bool function) const;
  LEX_CSTRING get_token(uint skip, uint length);
  int scan_ident_sysvar(THD *thd, Lex_ident_cli_st *str);
  int scan_ident_start(THD *thd, Lex_ident_cli_st *str);
  int scan_ident_middle(THD *thd, Lex_ident_cli_st *str,
                        CHARSET_INFO **cs, my_lex_states *);
  int scan_ident_delimited(THD *thd, Lex_ident_cli_st *str, uchar quote_char);
  bool get_7bit_or_8bit_ident(THD *thd, uchar *last_char);

  /** Current thread. */
  THD *m_thd;

  /** Pointer to the current position in the raw input stream. */
  char *m_ptr;

  /** Starting position of the last token parsed, in the raw buffer. */
  const char *m_tok_start;

  /** Ending position of the previous token parsed, in the raw buffer. */
  const char *m_tok_end;

  /** End of the query text in the input stream, in the raw buffer. */
  const char *m_end_of_query;

  /** Starting position of the previous token parsed, in the raw buffer. */
  const char *m_tok_start_prev;

  /** Begining of the query text in the input stream, in the raw buffer. */
  const char *m_buf;

  /** Length of the raw buffer. */
  size_t m_buf_length;

  /** Echo the parsed stream to the pre-processed buffer. */
  bool m_echo:1;
  bool m_echo_saved:1;

  /** Pre-processed buffer. */
  char *m_cpp_buf;

  /** Pointer to the current position in the pre-processed input stream. */
  char *m_cpp_ptr;

  /**
    Starting position of the last token parsed,
    in the pre-processed buffer.
  */
  const char *m_cpp_tok_start;

  /**
    Starting position of the previous token parsed,
    in the pre-procedded buffer.
  */
  const char *m_cpp_tok_start_prev;

  /**
    Ending position of the previous token parsed,
    in the pre-processed buffer.
  */
  const char *m_cpp_tok_end;

  /** UTF8-body buffer created during parsing. */
  char *m_body_utf8;

  /** Pointer to the current position in the UTF8-body buffer. */
  char *m_body_utf8_ptr;

  /**
    Position in the pre-processed buffer. The query from m_cpp_buf to
    m_cpp_utf_processed_ptr is converted to UTF8-body.
  */
  const char *m_cpp_utf8_processed_ptr;

public:

  /** Current state of the lexical analyser. */
  enum my_lex_states next_state;

  /**
    Position of ';' in the stream, to delimit multiple queries.
    This delimiter is in the raw buffer.
  */
  const char *found_semicolon;

  /** SQL_MODE = IGNORE_SPACE. */
  bool ignore_space:1;

  /**
    TRUE if we're parsing a prepared statement: in this mode
    we should allow placeholders.
  */
  bool stmt_prepare_mode:1;
  /**
    TRUE if we should allow multi-statements.
  */
  bool multi_statements:1;

  /** Current line number. */
  uint yylineno;

  /**
    Current statement digest instrumentation.
  */
  sql_digest_state* m_digest;

private:
  /** State of the lexical analyser for comments. */
  enum_comment_state in_comment;
  enum_comment_state in_comment_saved;

  /**
    Starting position of the TEXT_STRING or IDENT in the pre-processed
    buffer.

    NOTE: this member must be used within MYSQLlex() function only.
  */
  const char *m_cpp_text_start;

  /**
    Ending position of the TEXT_STRING or IDENT in the pre-processed
    buffer.

    NOTE: this member must be used within MYSQLlex() function only.
    */
  const char *m_cpp_text_end;

  /**
    Character set specified by the character-set-introducer.

    NOTE: this member must be used within MYSQLlex() function only.
  */
  CHARSET_INFO *m_underscore_cs;
};


/**
  Abstract representation of a statement.
  This class is an interface between the parser and the runtime.
  The parser builds the appropriate sub classes of Sql_statement
  to represent a SQL statement in the parsed tree.
  The execute() method in the sub classes contain the runtime implementation.
  Note that this interface is used for SQL statement recently implemented,
  the code for older statements tend to load the LEX structure with more
  attributes instead.
  The recommended way to implement new statements is to sub-class
  Sql_statement, as this improves code modularity (see the 'big switch' in
  dispatch_command()), and decrease the total size of the LEX structure
  (therefore saving memory in stored programs).
*/
class Sql_statement : public Sql_alloc
{
public:
  /**
    Execute this SQL statement.
    @param thd the current thread.
    @return 0 on success.
  */
  virtual bool execute(THD *thd) = 0;

protected:
  /**
    Constructor.
    @param lex the LEX structure that represents parts of this statement.
  */
  Sql_statement(LEX *lex)
    : m_lex(lex)
  {}

  /** Destructor. */
  virtual ~Sql_statement()
  {
    /*
      Sql_statement objects are allocated in thd->mem_root.
      In MySQL, the C++ destructor is never called, the underlying MEM_ROOT is
      simply destroyed instead.
      Do not rely on the destructor for any cleanup.
    */
    DBUG_ASSERT(FALSE);
  }

protected:
  /**
    The legacy LEX structure for this statement.
    The LEX structure contains the existing properties of the parsed tree.
    TODO: with time, attributes from LEX should move to sub classes of
    Sql_statement, so that the parser only builds Sql_statement objects
    with the minimum set of attributes, instead of a LEX structure that
    contains the collection of every possible attribute.
  */
  LEX *m_lex;
};


class Delete_plan;
class SQL_SELECT;

class Explain_query;
class Explain_update;
class Explain_delete;

/* 
  Query plan of a single-table UPDATE.
  (This is actually a plan for single-table DELETE also)
*/

class Update_plan
{
protected:
  bool impossible_where;
  bool no_partitions;
public:
  /* Allocate things there */
  MEM_ROOT *mem_root;

  TABLE *table;
  SQL_SELECT *select;
  uint index;
  ha_rows scanned_rows;
  /*
    Top-level select_lex. Most of its fields are not used, we need it only to
    get to the subqueries.
  */
  SELECT_LEX *select_lex;
  
  key_map possible_keys;
  bool using_filesort;
  bool using_io_buffer;
  
  /* Set this plan to be a plan to do nothing because of impossible WHERE */
  void set_impossible_where() { impossible_where= true; }
  void set_no_partitions() { no_partitions= true; }

  Explain_update* save_explain_update_data(THD *thd, MEM_ROOT *mem_root);
protected:
  bool save_explain_data_intern(THD *thd, MEM_ROOT *mem_root, Explain_update *eu, bool is_analyze);
public:
  virtual ~Update_plan() = default;

  Update_plan(MEM_ROOT *mem_root_arg) : 
    impossible_where(false), no_partitions(false), 
    mem_root(mem_root_arg), 
    using_filesort(false), using_io_buffer(false)
  {}
};


/* Query plan of a single-table DELETE */
class Delete_plan : public Update_plan
{
  bool deleting_all_rows;
public:

  /* Construction functions */
  Delete_plan(MEM_ROOT *mem_root_arg) : 
    Update_plan(mem_root_arg), 
    deleting_all_rows(false)
  {}

  /* Set this query plan to be a plan to make a call to h->delete_all_rows() */
  void set_delete_all_rows(ha_rows rows_arg) 
  { 
    deleting_all_rows= true;
    scanned_rows= rows_arg;
  }
  void cancel_delete_all_rows()
  {
    deleting_all_rows= false;
  }

  Explain_delete* save_explain_delete_data(THD *thd, MEM_ROOT *mem_root);
};

enum account_lock_type
{
  ACCOUNTLOCK_UNSPECIFIED= 0,
  ACCOUNTLOCK_LOCKED,
  ACCOUNTLOCK_UNLOCKED
};

enum password_exp_type
{
  PASSWORD_EXPIRE_UNSPECIFIED= 0,
  PASSWORD_EXPIRE_NOW,
  PASSWORD_EXPIRE_NEVER,
  PASSWORD_EXPIRE_DEFAULT,
  PASSWORD_EXPIRE_INTERVAL
};

struct Account_options: public USER_RESOURCES
{
  Account_options() = default;

  void reset()
  {
    bzero(this, sizeof(*this));
    ssl_type= SSL_TYPE_NOT_SPECIFIED;
  }

  enum SSL_type ssl_type;                       // defined in violite.h
  LEX_CSTRING x509_subject, x509_issuer, ssl_cipher;
  account_lock_type account_locked;
  password_exp_type password_expire;
  longlong num_expiration_days;
};

class Query_arena_memroot;
/* The state of the lex parsing. This is saved in the THD struct */


class Lex_prepared_stmt
{
  Lex_ident_sys m_name; // Statement name (in all queries)
  Item *m_code;         // PREPARE or EXECUTE IMMEDIATE source expression
  List<Item> m_params;  // List of parameters for EXECUTE [IMMEDIATE]
public:

  Lex_prepared_stmt()
   :m_code(NULL)
  { }
  const Lex_ident_sys &name() const
  {
    return m_name;
  }
  uint param_count() const
  {
    return m_params.elements;
  }
  List<Item> &params()
  {
    return m_params;
  }
  void set(const Lex_ident_sys_st &ident, Item *code, List<Item> *params)
  {
    DBUG_ASSERT(m_params.elements == 0);
    m_name= ident;
    m_code= code;
    if (params)
      m_params= *params;
  }
  bool params_fix_fields(THD *thd)
  {
    // Fix Items in the EXECUTE..USING list
    List_iterator_fast<Item> param_it(m_params);
    while (Item *param= param_it++)
    {
      if (param->fix_fields_if_needed_for_scalar(thd, 0))
        return true;
    }
    return false;
  }
  bool get_dynamic_sql_string(THD *thd, LEX_CSTRING *dst, String *buffer);
  void lex_start()
  {
    m_params.empty();
  }
};


class Lex_grant_object_name: public Grant_object_name, public Sql_alloc
{
public:
  Lex_grant_object_name(Table_ident *table_ident)
   :Grant_object_name(table_ident)
  { }
  Lex_grant_object_name(const LEX_CSTRING &db, Type type)
   :Grant_object_name(db, type)
  { }
};


class Lex_grant_privilege: public Grant_privilege, public Sql_alloc
{
public:
  Lex_grant_privilege() {}
  Lex_grant_privilege(privilege_t grant, bool all_privileges= false)
   :Grant_privilege(grant, all_privileges)
  { }
};


class sp_lex_cursor;

struct LEX: public Query_tables_list
{
  SELECT_LEX_UNIT unit;                         /* most upper unit */
  SELECT_LEX *first_select_lex() { return unit.first_select(); }
  const SELECT_LEX *first_select_lex() const { return unit.first_select(); }

private:
  SELECT_LEX builtin_select;

public:
  /* current SELECT_LEX in parsing */
  SELECT_LEX *current_select;
  /* list of all SELECT_LEX */
  SELECT_LEX *all_selects_list;
  /* current with clause in parsing if any, otherwise 0*/
  With_clause *curr_with_clause;
  /* pointer to the first with clause in the current statement */
  With_clause *with_clauses_list;
  /*
    (*with_clauses_list_last_next) contains a pointer to the last
     with clause in the current statement
  */
  With_clause **with_clauses_list_last_next;
  /*
    When a copy of a with element is parsed this is set to the offset of
    the with element in the input string, otherwise it's set to 0
  */
  my_ptrdiff_t clone_spec_offset;

  Create_view_info *create_view;

  /* Query Plan Footprint of a currently running select  */
  Explain_query *explain;

  /*
    LEX which represents current statement (conventional, SP or PS)

    For example during view parsing THD::lex will point to the views LEX and
    lex::stmt_lex will point to LEX of the statement where the view will be
    included

    Currently it is used to have always correct select numbering inside
    statement (LEX::current_select_number) without storing and restoring a
    global counter which was THD::select_number.

    TODO: make some unified statement representation (now SP has different)
    to store such data like LEX::current_select_number.
  */
  LEX *stmt_lex;

  LEX_CSTRING name;
  const char *help_arg;
  const char *backup_dir;                       /* For RESTORE/BACKUP */
  const char* to_log;                           /* For PURGE MASTER LOGS TO */
  String *wild; /* Wildcard in SHOW {something} LIKE 'wild'*/ 
  sql_exchange *exchange;
  select_result *result;
  /**
    @c the two may also hold BINLOG arguments: either comment holds a
    base64-char string or both represent the BINLOG fragment user variables.
  */
  LEX_CSTRING comment, ident;
  LEX_USER *grant_user;
  XID *xid;
  THD *thd;

  /* maintain a list of used plugins for this LEX */
  DYNAMIC_ARRAY plugins;
  plugin_ref plugins_static_buffer[INITIAL_LEX_PLUGIN_LIST_SIZE];

  /** SELECT of CREATE VIEW statement */
  LEX_STRING create_view_select;

  /** Start of 'ON table', in trigger statements.  */
  const char* raw_trg_on_table_name_begin;
  /** End of 'ON table', in trigger statements. */
  const char* raw_trg_on_table_name_end;

  /* Partition info structure filled in by PARTITION BY parse part */
  partition_info *part_info;

  /*
    The definer of the object being created (view, trigger, stored routine).
    I.e. the value of DEFINER clause.
  */
  LEX_USER *definer;

  /* Used in ALTER/CREATE user to store account locking options */
  Account_options account_options;

  Table_type table_type;                        /* Used for SHOW CREATE */
  List<Key_part_spec> ref_list;
  List<LEX_USER>      users_list;
  List<Item>          *insert_list,field_list,value_list,update_list;
  List<List_item>     many_values;
  List<set_var_base>  var_list;
  List<set_var_base>  stmt_var_list; //SET_STATEMENT values
  List<set_var_base>  old_var_list; // SET STATEMENT old values
private:
  Query_arena_memroot *arena_for_set_stmt;
  MEM_ROOT *mem_root_for_set_stmt;
  bool sp_block_finalize(THD *thd, const Lex_spblock_st spblock,
                                   class sp_label **splabel);
  bool sp_change_context(THD *thd, const sp_pcontext *ctx, bool exclusive);
  bool sp_exit_block(THD *thd, sp_label *lab);
  bool sp_exit_block(THD *thd, sp_label *lab, Item *when,
                     const LEX_CSTRING &expr_str);

  bool sp_continue_loop(THD *thd, sp_label *lab);

  bool sp_for_loop_condition(THD *thd, const Lex_for_loop_st &loop);
  bool sp_for_loop_increment(THD *thd, const Lex_for_loop_st &loop);

  /*
    Check if Item_field and Item_ref are allowed in the current statement.
    @retval false OK (fields are allowed)
    @retval true  ERROR (fields are not allowed). Error is raised.
  */
  bool check_expr_allows_fields_or_error(THD *thd, const char *name) const;

protected:
  bool sp_continue_loop(THD *thd, sp_label *lab, Item *when,
                        const LEX_CSTRING &expr_str);

public:
  void parse_error(uint err_number= ER_SYNTAX_ERROR);
  inline bool is_arena_for_set_stmt() {return arena_for_set_stmt != 0;}
  bool set_arena_for_set_stmt(Query_arena *backup);
  void reset_arena_for_set_stmt(Query_arena *backup);
  void free_arena_for_set_stmt();

  void print(String *str, enum_query_type qtype);
  List<Item_func_set_user_var> set_var_list; // in-query assignment list
  List<Item_param>    param_list;
  List<LEX_CSTRING>   view_list; // view list (list of field names in view)
  List<LEX_STRING>   *column_list; // list of column names (in ANALYZE)
  List<LEX_STRING>   *index_list;  // list of index names (in ANALYZE)
  /*
    A stack of name resolution contexts for the query. This stack is used
    at parse time to set local name resolution contexts for various parts
    of a query. For example, in a JOIN ... ON (some_condition) clause the
    Items in 'some_condition' must be resolved only against the operands
    of the the join, and not against the whole clause. Similarly, Items in
    subqueries should be resolved against the subqueries (and outer queries).
    The stack is used in the following way: when the parser detects that
    all Items in some clause need a local context, it creates a new context
    and pushes it on the stack. All newly created Items always store the
    top-most context in the stack. Once the parser leaves the clause that
    required a local context, the parser pops the top-most context.
  */
  List<Name_resolution_context> context_stack;
  SELECT_LEX *select_stack[MAX_SELECT_NESTING + 1];
  uint select_stack_top;
  /*
    Usually this is set to 0, but for INSERT/REPLACE SELECT it is set to 1.
    When parsing such statements the pointer to the most outer select is placed
    into the second element of select_stack rather than into the first.
  */
  uint select_stack_outer_barrier;

  SQL_I_List<ORDER> proc_list;
  SQL_I_List<TABLE_LIST> auxiliary_table_list, save_list;
  Column_definition *last_field;
  Table_function_json_table *json_table;
  Item_sum *in_sum_func;
  udf_func udf;
  HA_CHECK_OPT   check_opt;                        // check/repair options
  Table_specification_st create_info;
  Key *last_key;
  LEX_MASTER_INFO mi;                              // used by CHANGE MASTER
  LEX_SERVER_OPTIONS server_options;
  LEX_CSTRING relay_log_connection_name;
  LEX_RESET_SLAVE reset_slave_info;
  ulonglong type;
  ulong next_binlog_file_number;
  /* The following is used by KILL */
  killed_state kill_signal;
  killed_type  kill_type;
  uint current_select_number; // valid for statment LEX (not view)

  /*
    The following bool variables should not be bit fields as they are not
    reset for every query
  */
  bool autocommit;          // Often used, better as bool
  bool sp_lex_in_use;       // Keep track on lex usage in SPs for error handling

  /* Bit fields, reset for every query */
  bool is_shutdown_wait_for_slaves:1;
  bool selects_allow_procedure:1;
  /*
    A special command "PARSE_VCOL_EXPR" is defined for the parser
    to translate a defining expression of a virtual column into an
    Item object.
    The following flag is used to prevent other applications to use
    this command.
  */
  bool parse_vcol_expr:1;
  bool analyze_stmt:1; /* TRUE<=> this is "ANALYZE $stmt" */
  bool explain_json:1;
  /*
    true <=> The parsed fragment requires resolution of references to CTE
    at the end of parsing. This name resolution process involves searching
    for possible dependencies between CTE defined in the parsed fragment and
    detecting possible recursive references.
    The flag is set to true if the fragment contains CTE definitions.
  */
  bool with_cte_resolution:1;
  /*
    true <=> only resolution of references to CTE are required in the parsed
    fragment, no checking of dependencies between CTE is required.
    This flag is used only when parsing clones of CTE specifications.
  */
  bool only_cte_resolution:1;
  bool local_file:1;
  bool check_exists:1;
  bool verbose:1, no_write_to_binlog:1;
  bool safe_to_cache_query:1;
  bool ignore:1;
  bool next_is_main:1; // use "main" SELECT_LEX for nrxt allocation;
  bool next_is_down:1; // use "main" SELECT_LEX for nrxt allocation;
  /*
    field_list was created for view and should be removed before PS/SP
    rexecuton
  */
  bool empty_field_list_on_rset:1;
  /**
    During name resolution search only in the table list given by
    Name_resolution_context::first_name_resolution_table and
    Name_resolution_context::last_name_resolution_table
    (see Item_field::fix_fields()).
  */
  bool use_only_table_context:1;
  bool escape_used:1;
  bool default_used:1;    /* using default() function */
  bool with_rownum:1;     /* Using rownum() function */
  bool is_lex_started:1;  /* If lex_start() did run. For debugging. */

  /*
    This variable is used in post-parse stage to declare that sum-functions,
    or functions which have sense only if GROUP BY is present, are allowed.
    For example in a query
    SELECT ... FROM ...WHERE MIN(i) == 1 GROUP BY ... HAVING MIN(i) > 2
    MIN(i) in the WHERE clause is not allowed in the opposite to MIN(i)
    in the HAVING clause. Due to possible nesting of select construct
    the variable can contain 0 or 1 for each nest level.
  */
  nesting_map allow_sum_func;

  Sql_cmd *m_sql_cmd;

  /*
    Usually `expr` rule of yacc is quite reused but some commands better
    not support subqueries which comes standard with this rule, like
    KILL, HA_READ, CREATE/ALTER EVENT etc. Set this to a non-NULL
    clause name to get an error.
  */
  const char *clause_that_disallows_subselect;

  enum enum_duplicates duplicates;
  enum enum_tx_isolation tx_isolation;
  enum enum_ha_read_modes ha_read_mode;
  union {
    enum ha_rkey_function ha_rkey_mode;
    enum xa_option_words xa_opt;
    bool with_admin_option;                     // GRANT role
    bool with_persistent_for_clause; // uses PERSISTENT FOR clause (in ANALYZE)
  };
  enum enum_var_type option_type;
  enum enum_drop_mode drop_mode;

  enum backup_stages backup_stage;
  enum Foreign_key::fk_match_opt fk_match_option;
  enum_fk_option fk_update_opt;
  enum_fk_option fk_delete_opt;
  enum enum_yes_no_unknown tx_chain, tx_release;
  st_parsing_options parsing_options;
  /*
    In sql_cache we store SQL_CACHE flag as specified by user to be
    able to restore SELECT statement from internal structures.
  */
  enum e_sql_cache { SQL_CACHE_UNSPECIFIED, SQL_NO_CACHE, SQL_CACHE };
  e_sql_cache sql_cache;

  uint slave_thd_opt, start_transaction_opt;
  uint profile_query_id;
  uint profile_options;
  int nest_level;

  /*
    In LEX representing update which were transformed to multi-update
    stores total number of tables. For LEX representing multi-delete
    holds number of tables from which we will delete records.
  */
  uint table_count_update;

  uint8 describe;
  /*
    A flag that indicates what kinds of derived tables are present in the
    query (0 if no derived tables, otherwise a combination of flags
    DERIVED_SUBQUERY and DERIVED_VIEW).
  */
  uint8 derived_tables;
  uint8 context_analysis_only;
  uint8 lex_options; // see OPTION_LEX_*

  Alter_info alter_info;
  Lex_prepared_stmt prepared_stmt;
  /*
    For CREATE TABLE statement last element of table list which is not
    part of SELECT or LIKE part (i.e. either element for table we are
    creating or last of tables referenced by foreign keys).
  */
  TABLE_LIST *create_last_non_select_table;
  sp_head *sphead;
  sp_name *spname;
  MEM_ROOT sp_mem_root, *sp_mem_root_ptr;

  sp_pcontext *spcont;

  st_sp_chistics sp_chistics;

  Event_parse_data *event_parse_data;

  /* Characterstics of trigger being created */
  st_trg_chistics trg_chistics;

  /*
    stmt_definition_begin is intended to point to the next word after
    DEFINER-clause in the following statements:
      - CREATE TRIGGER (points to "TRIGGER");
      - CREATE PROCEDURE (points to "PROCEDURE");
      - CREATE FUNCTION (points to "FUNCTION" or "AGGREGATE");
      - CREATE EVENT (points to "EVENT")

    This pointer is required to add possibly omitted DEFINER-clause to the
    DDL-statement before dumping it to the binlog.

    keyword_delayed_begin_offset is the offset to the beginning of the DELAYED
    keyword in INSERT DELAYED statement. keyword_delayed_end_offset is the
    offset to the character right after the DELAYED keyword.
  */
  union {
    const char *stmt_definition_begin;
    uint keyword_delayed_begin_offset;
  };

  union {
    const char *stmt_definition_end;
    uint keyword_delayed_end_offset;
  };

  /**
    Collects create options for KEY
  */
  engine_option_value *option_list;

  /**
    Helper pointer to the end of the list when parsing options for
      LEX::create_info.option_list (for table)
      LEX::last_field->option_list (for fields)
      LEX::option_list             (for indexes)
  */
  engine_option_value *option_list_last;


  /*
    The set of those tables whose fields are referenced in all subqueries
    of the query.
    TODO: possibly this it is incorrect to have used tables in LEX because
    with subquery, it is not clear what does the field mean. To fix this
    we should aggregate used tables information for selected expressions
    into the select_lex.
  */
  table_map  used_tables;
  /**
    Maximum number of rows and/or keys examined by the query, both read,
    changed or written. This is the argument of LIMIT ROWS EXAMINED.
    The limit is represented by two variables - the Item is needed because
    in case of parameters we have to delay its evaluation until execution.
    Once evaluated, its value is stored in examined_rows_limit_cnt.
  */
  Item *limit_rows_examined;
  ulonglong limit_rows_examined_cnt;
  /**
    Holds a set of domain_ids for deletion at FLUSH..DELETE_DOMAIN_ID
  */
  DYNAMIC_ARRAY delete_gtid_domain;
  static const ulong initial_gtid_domain_buffer_size= 16;
  uint32 gtid_domain_static_buffer[initial_gtid_domain_buffer_size];

  inline void set_limit_rows_examined()
  {
    if (limit_rows_examined)
      limit_rows_examined_cnt= limit_rows_examined->val_uint();
    else
      limit_rows_examined_cnt= ULONGLONG_MAX;
  }


  LEX_CSTRING *win_ref;
  Window_frame *win_frame;
  Window_frame_bound *frame_top_bound;
  Window_frame_bound *frame_bottom_bound;
  Window_spec *win_spec;

  Item *upd_del_where;

  /* System Versioning */
  vers_select_conds_t vers_conditions;
  vers_select_conds_t period_conditions;

  inline void free_set_stmt_mem_root()
  {
    DBUG_ASSERT(!is_arena_for_set_stmt());
    if (mem_root_for_set_stmt)
    {
      free_root(mem_root_for_set_stmt, MYF(0));
      delete mem_root_for_set_stmt;
      mem_root_for_set_stmt= 0;
    }
  }

  LEX();

  virtual ~LEX()
  {
    free_set_stmt_mem_root();
    destroy_query_tables_list();
    plugin_unlock_list(NULL, (plugin_ref *)plugins.buffer, plugins.elements);
    delete_dynamic(&plugins);
  }

  virtual class Query_arena *query_arena()
  {
    DBUG_ASSERT(0);
    return NULL;
  }

  void start(THD *thd);

  inline bool is_ps_or_view_context_analysis()
  {
    return (context_analysis_only &
            (CONTEXT_ANALYSIS_ONLY_PREPARE |
             CONTEXT_ANALYSIS_ONLY_VCOL_EXPR |
             CONTEXT_ANALYSIS_ONLY_VIEW));
  }

  inline bool is_view_context_analysis()
  {
    return (context_analysis_only & CONTEXT_ANALYSIS_ONLY_VIEW);
  }

  inline void uncacheable(uint8 cause)
  {
    safe_to_cache_query= 0;

    if (current_select) // initialisation SP variables has no SELECT
    {
      /*
        There are no sense to mark select_lex and union fields of LEX,
        but we should merk all subselects as uncacheable from current till
        most upper
      */
      SELECT_LEX *sl;
      SELECT_LEX_UNIT *un;
      for (sl= current_select, un= sl->master_unit();
           un && un != &unit;
           sl= sl->outer_select(), un= (sl ? sl->master_unit() : NULL))
      {
       sl->uncacheable|= cause;
       un->uncacheable|= cause;
      }
      if (sl)
        sl->uncacheable|= cause;
    }
    if (first_select_lex())
      first_select_lex()->uncacheable|= cause;
  }
  void set_trg_event_type_for_tables();

  TABLE_LIST *unlink_first_table(bool *link_to_local);
  void link_first_table_back(TABLE_LIST *first, bool link_to_local);
  void first_lists_tables_same();
  void fix_first_select_number();

  bool can_be_merged();
  bool can_use_merged();
  bool can_not_use_merged();
  bool only_view_structure();
  bool need_correct_ident();
  uint8 get_effective_with_check(TABLE_LIST *view);
  /*
    Is this update command where 'WHITH CHECK OPTION' clause is important

    SYNOPSIS
      LEX::which_check_option_applicable()

    RETURN
      TRUE   have to take 'WHITH CHECK OPTION' clause into account
      FALSE  'WHITH CHECK OPTION' clause do not need
  */
  inline bool which_check_option_applicable()
  {
    switch (sql_command) {
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
    case SQLCOM_DELETE:
    case SQLCOM_DELETE_MULTI:
    case SQLCOM_INSERT:
    case SQLCOM_INSERT_SELECT:
    case SQLCOM_REPLACE:
    case SQLCOM_REPLACE_SELECT:
    case SQLCOM_LOAD:
      return TRUE;
    default:
      return FALSE;
    }
  }

  void cleanup_after_one_table_open();

  bool push_context(Name_resolution_context *context);

  Name_resolution_context *pop_context();

  SELECT_LEX *select_stack_head()
  {
    if (likely(select_stack_top))
      return select_stack[select_stack_top - 1];
    return NULL;
  }

  bool push_select(SELECT_LEX *select_lex)
  {
    DBUG_ENTER("LEX::push_select");
    DBUG_PRINT("info", ("Top Select was %p (%d)  depth: %u  pushed: %p (%d)",
                        select_stack_head(),
                        select_stack_top,
                        (select_stack_top ?
                         select_stack_head()->select_number :
                         0),
                        select_lex, select_lex->select_number));
    if (unlikely(select_stack_top > MAX_SELECT_NESTING))
    {
      my_error(ER_TOO_HIGH_LEVEL_OF_NESTING_FOR_SELECT, MYF(0));
      DBUG_RETURN(TRUE);
    }
    if (push_context(&select_lex->context))
      DBUG_RETURN(TRUE);
    select_stack[select_stack_top++]= select_lex;
    current_select= select_lex;
    DBUG_RETURN(FALSE);
  }

  SELECT_LEX *pop_select()
  {
    DBUG_ENTER("LEX::pop_select");
    SELECT_LEX *select_lex;
    if (likely(select_stack_top))
      select_lex= select_stack[--select_stack_top];
    else
      select_lex= 0;
    DBUG_PRINT("info", ("Top Select is %p (%d)  depth: %u  poped: %p (%d)",
                        select_stack_head(),
                        select_stack_top,
                        (select_stack_top ?
                         select_stack_head()->select_number :
                         0),
                        select_lex,
                        (select_lex ? select_lex->select_number : 0)));
    DBUG_ASSERT(select_lex);

    pop_context();

    if (unlikely(!select_stack_top))
    {
      current_select= &builtin_select;
      DBUG_PRINT("info", ("Top Select is empty -> sel builtin: %p  service: %u",
                          current_select, builtin_select.is_service_select));
      builtin_select.is_service_select= false;
    }
    else
      current_select= select_stack[select_stack_top - 1];

    DBUG_RETURN(select_lex);
  }

  SELECT_LEX *current_select_or_default()
  {
    return current_select ? current_select : &builtin_select;
  }

  bool copy_db_to(LEX_CSTRING *to);

  void inc_select_stack_outer_barrier()
  {
    select_stack_outer_barrier++;
  }

  SELECT_LEX *parser_current_outer_select()
  {
    return select_stack_top - 1 == select_stack_outer_barrier ?
             0 : select_stack[select_stack_top - 2];
  }

  Name_resolution_context *current_context()
  {
    return context_stack.head();
  }

  /*
    Restore the LEX and THD in case of a parse error.
  */
  static void cleanup_lex_after_parse_error(THD *thd);

  void reset_n_backup_query_tables_list(Query_tables_list *backup);
  void restore_backup_query_tables_list(Query_tables_list *backup);

  bool table_or_sp_used();

  bool is_partition_management() const;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  bool part_values_current(THD *thd);
  bool part_values_history(THD *thd);
#endif

  /**
    @brief check if the statement is a single-level join
    @return result of the check
      @retval TRUE  The statement doesn't contain subqueries, unions and 
                    stored procedure calls.
      @retval FALSE There are subqueries, UNIONs or stored procedure calls.
  */
  bool is_single_level_stmt() 
  { 
    /* 
      This check exploits the fact that the last added to all_select_list is
      on its top. So select_lex (as the first added) will be at the tail 
      of the list.
    */ 
    if (first_select_lex() == all_selects_list && !sroutines.records)
    {
      return TRUE;
    }
    return FALSE;
  }

  bool save_prep_leaf_tables();

  int print_explain(select_result_sink *output, uint8 explain_flags,
                    bool is_analyze, bool is_json_format,
                    bool *printed_anything);
  bool restore_set_statement_var();

  void init_last_field(Column_definition *field, const LEX_CSTRING *name);
  bool last_field_generated_always_as_row_start_or_end(Lex_ident *p,
                                                       const char *type,
                                                       uint flags);
  bool last_field_generated_always_as_row_start();
  bool last_field_generated_always_as_row_end();

  bool new_sp_instr_stmt(THD *, const LEX_CSTRING &prefix,
                         const LEX_CSTRING &suffix);
  bool sp_proc_stmt_statement_finalize_buf(THD *, const LEX_CSTRING &qbuf);
  bool sp_proc_stmt_statement_finalize(THD *, bool no_lookahead);

  sp_variable *sp_param_init(LEX_CSTRING *name);
  bool sp_param_fill_definition(sp_variable *spvar,
                                const Lex_field_type_st &def);
  bool sf_return_fill_definition(const Lex_field_type_st &def);

  int case_stmt_action_then();
  bool setup_select_in_parentheses();
  bool set_names(const char *pos,
                 const Lex_exact_charset_opt_extended_collate &cs,
                 bool no_lookahead);
  bool set_trigger_new_row(const LEX_CSTRING *name, Item *val,
                           const LEX_CSTRING &expr_str);
  bool set_trigger_field(const LEX_CSTRING *name1, const LEX_CSTRING *name2,
                         Item *val, const LEX_CSTRING &expr_str);
  bool set_system_variable(enum_var_type var_type, sys_var *var,
                           const Lex_ident_sys_st *base_name, Item *val);
  bool set_system_variable(enum_var_type var_type,
                           const Lex_ident_sys_st *name, Item *val);
  bool set_system_variable(THD *thd, enum_var_type var_type,
                           const Lex_ident_sys_st *name1,
                           const Lex_ident_sys_st *name2,
                           Item *val);
  bool set_default_system_variable(enum_var_type var_type,
                                   const Lex_ident_sys_st *name,
                                   Item *val);
  bool set_user_variable(THD *thd, const LEX_CSTRING *name, Item *val);
  void set_stmt_init();
  sp_name *make_sp_name(THD *thd, const LEX_CSTRING *name);
  sp_name *make_sp_name(THD *thd, const LEX_CSTRING *name1,
                                  const LEX_CSTRING *name2);
  sp_name *make_sp_name_package_routine(THD *thd, const LEX_CSTRING *name);
  sp_head *make_sp_head(THD *thd, const sp_name *name, const Sp_handler *sph,
                        enum_sp_aggregate_type agg_type);
  sp_head *make_sp_head_no_recursive(THD *thd, const sp_name *name,
                                     const Sp_handler *sph,
                                     enum_sp_aggregate_type agg_type);
  bool sp_body_finalize_routine(THD *);
  bool sp_body_finalize_trigger(THD *);
  bool sp_body_finalize_event(THD *);
  bool sp_body_finalize_function(THD *);
  bool sp_body_finalize_procedure(THD *);
  bool sp_body_finalize_procedure_standalone(THD *, const sp_name *end_name);
  sp_package *create_package_start(THD *thd,
                                   enum_sql_command command,
                                   const Sp_handler *sph,
                                   const sp_name *name,
                                   DDL_options_st options);
  bool create_package_finalize(THD *thd,
                               const sp_name *name,
                               const sp_name *name2,
                               const char *cpp_body_end);
  bool call_statement_start(THD *thd, sp_name *name);
  bool call_statement_start(THD *thd, const Lex_ident_sys_st *name);
  bool call_statement_start(THD *thd, const Lex_ident_sys_st *name1,
                                      const Lex_ident_sys_st *name2);
  bool call_statement_start(THD *thd,
                            const Lex_ident_sys_st *db,
                            const Lex_ident_sys_st *pkg,
                            const Lex_ident_sys_st *proc);
  sp_variable *find_variable(const LEX_CSTRING *name,
                             sp_pcontext **ctx,
                             const Sp_rcontext_handler **rh) const;
  sp_variable *find_variable(const LEX_CSTRING *name,
                             const Sp_rcontext_handler **rh) const
  {
    sp_pcontext *not_used_ctx;
    return find_variable(name, &not_used_ctx, rh);
  }
  bool set_variable(const Lex_ident_sys_st *name, Item *item,
                    const LEX_CSTRING &expr_str);
  bool set_variable(const Lex_ident_sys_st *name1,
                    const Lex_ident_sys_st *name2, Item *item,
                    const LEX_CSTRING &expr_str);
  void sp_variable_declarations_init(THD *thd, int nvars);
  bool sp_variable_declarations_finalize(THD *thd, int nvars,
                                         const Column_definition *cdef,
                                         Item *def,
                                         const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_set_default(THD *thd, int nvars, Item *def,
                                            const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_row_finalize(THD *thd, int nvars,
                                             Row_definition_list *row,
                                             Item *def,
                                             const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_with_ref_finalize(THD *thd, int nvars,
                                                  Qualified_column_ident *col,
                                                  Item *def,
                                                  const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_rowtype_finalize(THD *thd, int nvars,
                                                 Qualified_column_ident *,
                                                 Item *def,
                                                 const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_cursor_rowtype_finalize(THD *thd, int nvars,
                                                        uint offset,
                                                        Item *def,
                                                        const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_table_rowtype_finalize(THD *thd, int nvars,
                                                       const LEX_CSTRING &db,
                                                       const LEX_CSTRING &table,
                                                       Item *def,
                                                       const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_column_type_finalize(THD *thd, int nvars,
                                                     Qualified_column_ident *ref,
                                                     Item *def,
                                                     const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_vartype_finalize(THD *thd, int nvars,
                                                 const LEX_CSTRING &name,
                                                 Item *def,
                                                 const LEX_CSTRING &expr_str);
  bool sp_variable_declarations_copy_type_finalize(THD *thd, int nvars,
                                                   const Column_definition &ref,
                                                   Row_definition_list *fields,
                                                   Item *def,
                                                   const LEX_CSTRING &expr_str);

  LEX_USER *current_user_for_set_password(THD *thd);
  bool sp_create_set_password_instr(THD *thd,
                                    LEX_USER *user,
                                    USER_AUTH *auth,
                                    bool no_lookahead);
  bool sp_create_set_password_instr(THD *thd,
                                    USER_AUTH *auth,
                                    bool no_lookahead)
  {
    LEX_USER *user;
    return !(user= current_user_for_set_password(thd)) ||
           sp_create_set_password_instr(thd, user, auth, no_lookahead);
  }

  bool sp_handler_declaration_init(THD *thd, int type);
  bool sp_handler_declaration_finalize(THD *thd, int type);

  bool sp_declare_cursor(THD *thd, const LEX_CSTRING *name,
                         class sp_lex_cursor *cursor_stmt,
                         sp_pcontext *param_ctx, bool add_cpush_instr);

  bool sp_open_cursor(THD *thd, const LEX_CSTRING *name,
                      List<sp_assignment_lex> *parameters);
  Item_splocal *create_item_for_sp_var(const Lex_ident_cli_st *name,
                                       sp_variable *spvar);

  Item *create_item_qualified_asterisk(THD *thd, const Lex_ident_sys_st *name);
  Item *create_item_qualified_asterisk(THD *thd,
                                       const Lex_ident_sys_st *a,
                                       const Lex_ident_sys_st *b);
  Item *create_item_qualified_asterisk(THD *thd, const Lex_ident_cli_st *cname)
  {
    Lex_ident_sys name(thd, cname);
    if (name.is_null())
      return NULL; // EOM
    return create_item_qualified_asterisk(thd, &name);
  }
  Item *create_item_qualified_asterisk(THD *thd,
                                       const Lex_ident_cli_st *ca,
                                       const Lex_ident_cli_st *cb)
  {
    Lex_ident_sys a(thd, ca), b(thd, cb);
    if (a.is_null() || b.is_null())
      return NULL; // EOM
    return create_item_qualified_asterisk(thd, &a, &b);
  }

  Item *create_item_ident_field(THD *thd,
                                const Lex_ident_sys_st &db,
                                const Lex_ident_sys_st &table,
                                const Lex_ident_sys_st &name);
  Item *create_item_ident_nosp(THD *thd, Lex_ident_sys_st *name)
  {
    return create_item_ident_field(thd, Lex_ident_sys(), Lex_ident_sys(), *name);
  }
  Item *create_item_ident_sp(THD *thd, Lex_ident_sys_st *name,
                             const char *start, const char *end);
  Item *create_item_ident(THD *thd, Lex_ident_cli_st *cname)
  {
    Lex_ident_sys name(thd, cname);
    if (name.is_null())
      return NULL; // EOM
    return sphead ?
           create_item_ident_sp(thd, &name, cname->pos(), cname->end()) :
           create_item_ident_nosp(thd, &name);
  }
  /*
    Create an Item corresponding to a qualified name: a.b
    when the parser is out of an SP context.
      @param THD        - THD, for mem_root
      @param a          - the first name
      @param b          - the second name
      @retval           - a pointer to a created item, or NULL on error.

    Possible Item types that can be created:
    - Item_trigger_field
    - Item_field
    - Item_ref
  */
  Item *create_item_ident_nospvar(THD *thd,
                                  const Lex_ident_sys_st *a,
                                  const Lex_ident_sys_st *b);
  /*
    Create an Item corresponding to a ROW field valiable:  var.field
      @param THD        - THD, for mem_root
      @param rh [OUT]   - the rcontext handler (local vs package variables)
      @param var        - the ROW variable name
      @param field      - the ROW variable field name
      @param spvar      - the variable that was previously found by name
                          using "var_name".
      @param start      - position in the query (for binary log)
      @param end        - end in the query (for binary log)
  */
  Item_splocal *create_item_spvar_row_field(THD *thd,
                                            const Sp_rcontext_handler *rh,
                                            const Lex_ident_sys *var,
                                            const Lex_ident_sys *field,
                                            sp_variable *spvar,
                                            const char *start,
                                            const char *end);
  /*
    Create an item from its qualified name.
    Depending on context, it can be either a ROW variable field,
    or trigger, table field, table field reference.
    See comments to create_item_spvar_row_field() and
    create_item_ident_nospvar().
      @param thd         - THD, for mem_root
      @param a           - the first name
      @param b           - the second name
      @retval            - NULL on error, or a pointer to a new Item.
  */
  Item *create_item_ident(THD *thd,
                          const Lex_ident_cli_st *a,
                          const Lex_ident_cli_st *b);
  /*
    Create an item from its qualified name.
    Depending on context, it can be a table field, a table field reference,
    or a sequence NEXTVAL and CURRVAL.
      @param thd         - THD, for mem_root
      @param a           - the first name
      @param b           - the second name
      @param c           - the third name
      @retval            - NULL on error, or a pointer to a new Item.
  */
  Item *create_item_ident(THD *thd,
                          const Lex_ident_sys_st *a,
                          const Lex_ident_sys_st *b,
                          const Lex_ident_sys_st *c);

  Item *create_item_ident(THD *thd,
                          const Lex_ident_cli_st *ca,
                          const Lex_ident_cli_st *cb,
                          const Lex_ident_cli_st *cc)
  {
    Lex_ident_sys b(thd, cb), c(thd, cc);
    if (b.is_null() || c.is_null())
      return NULL;
    if (ca->pos() == cb->pos())  // SELECT .t1.col1
    {
      DBUG_ASSERT(ca->length == 0);
      Lex_ident_sys none;
      return create_item_ident(thd, &none, &b, &c);
    }
    Lex_ident_sys a(thd, ca);
    return a.is_null() ? NULL : create_item_ident(thd, &a, &b, &c);
  }

  /*
    Create an item for "NEXT VALUE FOR sequence_name"
  */
  Item *create_item_func_nextval(THD *thd, Table_ident *ident);
  Item *create_item_func_nextval(THD *thd, const LEX_CSTRING *db,
                                           const LEX_CSTRING *name);
  /*
    Create an item for "PREVIOUS VALUE FOR sequence_name"
  */
  Item *create_item_func_lastval(THD *thd, Table_ident *ident);
  Item *create_item_func_lastval(THD *thd, const LEX_CSTRING *db,
                                           const LEX_CSTRING *name);
  
  /*
    Create an item for "SETVAL(sequence_name, value [, is_used [, round]])
  */
  Item *create_item_func_setval(THD *thd, Table_ident *ident, longlong value,
                                ulonglong round, bool is_used);

  /*
    Create an item for a name in LIMIT clause: LIMIT var
      @param THD         - THD, for mem_root
      @param var_name    - the variable name
      @retval            - a new Item corresponding to the SP variable,
                           or NULL on error
                           (non in SP, unknown variable, wrong data type).
  */
  Item *create_item_limit(THD *thd, const Lex_ident_cli_st *var_name);

  /*
    Create an item for a qualified name in LIMIT clause: LIMIT var.field
      @param THD         - THD, for mem_root
      @param var_name    - the variable name
      @param field_name  - the variable field name
      @param start       - start in the query (for binary log)
      @param end         - end in the query (for binary log)
      @retval            - a new Item corresponding to the SP variable,
                           or NULL on error
                           (non in SP, unknown variable, unknown ROW field,
                            wrong data type).
  */
  Item *create_item_limit(THD *thd,
                          const Lex_ident_cli_st *var_name,
                          const Lex_ident_cli_st *field_name);

  Item *create_item_query_expression(THD *thd, st_select_lex_unit *unit);

  Item *make_item_func_sysdate(THD *thd, uint fsp);
  Item *make_item_func_call_generic(THD *thd, Lex_ident_cli_st *db,
                                    Lex_ident_cli_st *name, List<Item> *args);
  Item *make_item_func_call_generic(THD *thd,
                                    Lex_ident_cli_st *db,
                                    Lex_ident_cli_st *pkg,
                                    Lex_ident_cli_st *name,
                                    List<Item> *args);
  Item *make_item_func_call_native_or_parse_error(THD *thd,
                                                  Lex_ident_cli_st &name,
                                                  List<Item> *args);
  my_var *create_outvar(THD *thd, const LEX_CSTRING *name);

  /*
    Create a my_var instance for a ROW field variable that was used
    as an OUT SP parameter: CALL p1(var.field);
      @param THD        - THD, for mem_root
      @param var_name   - the variable name
      @param field_name - the variable field name
  */
  my_var *create_outvar(THD *thd,
                        const LEX_CSTRING *var_name,
                        const LEX_CSTRING *field_name);

  bool is_trigger_new_or_old_reference(const LEX_CSTRING *name) const;

  Item *create_and_link_Item_trigger_field(THD *thd, const LEX_CSTRING *name,
                                           bool new_row);
  // For syntax with colon, e.g. :NEW.a  or :OLD.a
  Item *make_item_colon_ident_ident(THD *thd,
                                    const Lex_ident_cli_st *a,
                                    const Lex_ident_cli_st *b);
  // PLSQL: cursor%ISOPEN etc
  Item *make_item_plsql_cursor_attr(THD *thd, const LEX_CSTRING *name,
                                    plsql_cursor_attr_t attr);

  // For "SELECT @@var", "SELECT @@var.field"
  Item *make_item_sysvar(THD *thd,
                         enum_var_type type,
                         const LEX_CSTRING *name)
  {
    return make_item_sysvar(thd, type, name, &null_clex_str);
  }
  Item *make_item_sysvar(THD *thd,
                         enum_var_type type,
                         const LEX_CSTRING *name,
                         const LEX_CSTRING *component);
  void sp_block_init(THD *thd, const LEX_CSTRING *label);
  void sp_block_init(THD *thd)
  {
    // Unlabeled blocks get an empty label
    sp_block_init(thd, &empty_clex_str);
  }
  bool sp_block_finalize(THD *thd, const Lex_spblock_st spblock)
  {
    class sp_label *tmp;
    return sp_block_finalize(thd, spblock, &tmp);
  }
  bool sp_block_finalize(THD *thd)
  {
    return sp_block_finalize(thd, Lex_spblock());
  }
  bool sp_block_finalize(THD *thd, const Lex_spblock_st spblock,
                                   const LEX_CSTRING *end_label);
  bool sp_block_finalize(THD *thd, const LEX_CSTRING *end_label)
  {
    return sp_block_finalize(thd, Lex_spblock(), end_label);
  }
  bool sp_declarations_join(Lex_spblock_st *res,
                            const Lex_spblock_st b1,
                            const Lex_spblock_st b2) const
  {
    if ((b2.vars || b2.conds) && (b1.curs || b1.hndlrs))
    {
      my_error(ER_SP_VARCOND_AFTER_CURSHNDLR, MYF(0));
      return true;
    }
    if (b2.curs && b1.hndlrs)
    {
      my_error(ER_SP_CURSOR_AFTER_HANDLER, MYF(0));
      return true;
    }
    res->join(b1, b2);
    return false;
  }
  bool sp_block_with_exceptions_finalize_declarations(THD *thd);
  bool sp_block_with_exceptions_finalize_executable_section(THD *thd,
                                                  uint executable_section_ip);
  bool sp_block_with_exceptions_finalize_exceptions(THD *thd,
                                                  uint executable_section_ip,
                                                  uint exception_count);
  bool sp_block_with_exceptions_add_empty(THD *thd);
  bool sp_exit_statement(THD *thd, Item *when, const LEX_CSTRING &expr_str);
  bool sp_exit_statement(THD *thd, const LEX_CSTRING *label_name, Item *item,
                         const LEX_CSTRING &expr_str);
  bool sp_leave_statement(THD *thd, const LEX_CSTRING *label_name);
  bool sp_goto_statement(THD *thd, const LEX_CSTRING *label_name);

  bool sp_continue_statement(THD *thd);
  bool sp_continue_statement(THD *thd, const LEX_CSTRING *label_name);
  bool sp_iterate_statement(THD *thd, const LEX_CSTRING *label_name);

  bool maybe_start_compound_statement(THD *thd);
  bool sp_push_loop_label(THD *thd, const LEX_CSTRING *label_name);
  bool sp_push_loop_empty_label(THD *thd);
  bool sp_pop_loop_label(THD *thd, const LEX_CSTRING *label_name);
  void sp_pop_loop_empty_label(THD *thd);
  bool sp_while_loop_expression(THD *thd, Item *expr,
                                const LEX_CSTRING &expr_str);
  bool sp_while_loop_finalize(THD *thd);
  bool sp_if_after_statements(THD *thd);
  bool sp_push_goto_label(THD *thd, const LEX_CSTRING *label_name);

  Item_param *add_placeholder(THD *thd, const LEX_CSTRING *name,
                              const char *start, const char *end);

  /* Integer range FOR LOOP methods */
  sp_variable *sp_add_for_loop_variable(THD *thd, const LEX_CSTRING *name,
                                        Item *value,
                                        const LEX_CSTRING &expr_str);
  sp_variable *sp_add_for_loop_target_bound(THD *thd, Item *value,
                                            const LEX_CSTRING &expr_str)
  {
    LEX_CSTRING name= { STRING_WITH_LEN("[target_bound]") };
    return sp_add_for_loop_variable(thd, &name, value, expr_str);
  }
  bool sp_for_loop_intrange_declarations(THD *thd, Lex_for_loop_st *loop,
                                        const LEX_CSTRING *index,
                                        const Lex_for_loop_bounds_st &bounds);
  bool sp_for_loop_intrange_condition_test(THD *thd, const Lex_for_loop_st &loop);
  bool sp_for_loop_intrange_iterate(THD *thd, const Lex_for_loop_st &loop);

  /* Cursor FOR LOOP methods */
  bool sp_for_loop_cursor_declarations(THD *thd, Lex_for_loop_st *loop,
                                       const LEX_CSTRING *index,
                                       const Lex_for_loop_bounds_st &bounds);
  sp_variable *sp_add_for_loop_cursor_variable(THD *thd,
                                               const LEX_CSTRING *name,
                                               const class sp_pcursor *cur,
                                               uint coffset,
                                               sp_assignment_lex *param_lex,
                                               Item_args *parameters);
  bool sp_for_loop_implicit_cursor_statement(THD *thd,
                                             Lex_for_loop_bounds_st *bounds,
                                             sp_lex_cursor *cur);
  bool sp_for_loop_cursor_condition_test(THD *thd, const Lex_for_loop_st &loop);
  bool sp_for_loop_cursor_iterate(THD *thd, const Lex_for_loop_st &);

  /* Generic FOR LOOP methods*/

  /*
    Generate FOR loop declarations and
    initialize "loop" from "index" and "bounds".

    @param [IN]  thd    - current THD, for mem_root and error reporting
    @param [OUT] loop   - the loop generated SP variables are stored here,
                          together with additional loop characteristics.
    @param [IN]  index  - the loop index variable name
    @param [IN]  bounds - the loop bounds (in sp_assignment_lex format)
                          and additional loop characteristics,
                          as created by the sp_for_loop_bounds rule.
    @retval true        - on error
    @retval false       - on success

    This methods adds declarations:
    - An explicit integer or cursor%ROWTYPE "index" variable
    - An implicit integer upper bound variable, in case of integer range loops
    - A CURSOR, in case of an implicit CURSOR loops
    The generated variables are stored into "loop".
    Additional loop characteristics are copied from "bounds" to "loop".
  */
  bool sp_for_loop_declarations(THD *thd, Lex_for_loop_st *loop,
                                const LEX_CSTRING *index,
                                const Lex_for_loop_bounds_st &bounds)
  {
    return bounds.is_for_loop_cursor() ?
           sp_for_loop_cursor_declarations(thd, loop, index, bounds) :
           sp_for_loop_intrange_declarations(thd, loop, index, bounds);
  }

  /*
    Generate a conditional jump instruction to leave the loop,
    using a proper condition depending on the loop type:
    - Item_func_le            -- integer range loops
    - Item_func_ge            -- integer range reverse loops
    - Item_func_cursor_found  -- cursor loops
  */
  bool sp_for_loop_condition_test(THD *thd, const Lex_for_loop_st &loop)
  {
    return loop.is_for_loop_cursor() ?
           sp_for_loop_cursor_condition_test(thd, loop) :
           sp_for_loop_intrange_condition_test(thd, loop);
  }

  /*
    Generate "increment" instructions followed by a jump to the
    condition test in the beginnig of the loop.
    "Increment" depends on the loop type and can be:
    - index:= index + 1;       -- integer range loops
    - index:= index - 1;       -- integer range reverse loops
    - FETCH cursor INTO index; -- cursor loops
  */
  bool sp_for_loop_finalize(THD *thd, const Lex_for_loop_st &loop)
  {
    if (loop.is_for_loop_cursor() ?
        sp_for_loop_cursor_iterate(thd, loop) :
        sp_for_loop_intrange_iterate(thd, loop))
      return true;
    // Generate a jump to the beginning of the loop
    return sp_while_loop_finalize(thd);
  }
  bool sp_for_loop_outer_block_finalize(THD *thd, const Lex_for_loop_st &loop);

  /*
    Make an Item when an identifier is found in the FOR loop bounds:
      FOR rec IN cursor
      FOR rec IN var1 .. var2
      FOR rec IN row1.field1 .. xxx
  */
  Item *create_item_for_loop_bound(THD *thd,
                                   const LEX_CSTRING *a,
                                   const LEX_CSTRING *b,
                                   const LEX_CSTRING *c);
  /* End of FOR LOOP methods */

  bool add_signal_statement(THD *thd, const class sp_condition_value *value);
  bool add_resignal_statement(THD *thd, const class sp_condition_value *value);

  // Check if "KEY IF NOT EXISTS name" used outside of ALTER context
  bool check_add_key(DDL_options_st ddl)
  {
    if (ddl.if_not_exists() && sql_command != SQLCOM_ALTER_TABLE)
    {
      parse_error();
      return true;
    }
    return false;
  }
  // Add a key as a part of CREATE TABLE or ALTER TABLE
  bool add_key(Key::Keytype key_type, const LEX_CSTRING *key_name,
               ha_key_alg algorithm, DDL_options_st ddl)
  {
    if (check_add_key(ddl) ||
        !(last_key= new Key(key_type, key_name, algorithm, false, ddl)))
      return true;
    alter_info.key_list.push_back(last_key);
    return false;
  }
  // Add a key for a CREATE INDEX statement
  bool add_create_index(Key::Keytype key_type, const LEX_CSTRING *key_name,
                        ha_key_alg algorithm, DDL_options_st ddl)
  {
    if (check_create_options(ddl) ||
       !(last_key= new Key(key_type, key_name, algorithm, false, ddl)))
      return true;
    alter_info.key_list.push_back(last_key);
    return false;
  }
  bool add_create_index_prepare(Table_ident *table)
  {
    sql_command= SQLCOM_CREATE_INDEX;
    if (!current_select->add_table_to_list(thd, table, NULL,
                                           TL_OPTION_UPDATING,
                                           TL_READ_NO_INSERT,
                                           MDL_SHARED_UPGRADABLE))
      return true;
    alter_info.reset();
    alter_info.flags= ALTER_ADD_INDEX;
    option_list= NULL;
    return false;
  }
  /*
    Add an UNIQUE or PRIMARY key which is a part of a column definition:
      CREATE TABLE t1 (a INT PRIMARY KEY);
  */
  void add_key_to_list(LEX_CSTRING *field_name,
                       enum Key::Keytype type, bool check_exists);
  // Add a constraint as a part of CREATE TABLE or ALTER TABLE
  bool add_constraint(const LEX_CSTRING &name, Virtual_column_info *constr,
                      bool if_not_exists)
  {
    constr->name= name;
    constr->if_not_exists= if_not_exists;
    alter_info.check_constraint_list.push_back(constr);
    return false;
  }
  bool add_alter_list(LEX_CSTRING par_name, Virtual_column_info *expr,
                      bool par_exists);
  bool add_alter_list(LEX_CSTRING name, LEX_CSTRING new_name, bool exists);
  bool add_alter_list_item_convert_to_charset(Sql_used *used,
                                              const Charset_collation_map_st &map,
                                              CHARSET_INFO *cs)
  {
    if (create_info.add_table_option_convert_charset(used, map, cs))
      return true;
    alter_info.flags|= ALTER_CONVERT_TO;
    return false;
  }
  bool
  add_alter_list_item_convert_to_charset(Sql_used *used,
                                         const Charset_collation_map_st &map,
                                         CHARSET_INFO *cs,
                                         const Lex_extended_collation_st &cl)
  {
    if (create_info.add_table_option_convert_charset(used, map, cs) ||
        create_info.add_table_option_convert_collation(used, map, cl))
      return true;
    alter_info.flags|= ALTER_CONVERT_TO;
    return false;
  }
  void set_command(enum_sql_command command,
                   DDL_options_st options)
  {
    sql_command= command;
    create_info.set(options);
  }
  void set_command(enum_sql_command command,
                   uint scope,
                   DDL_options_st options)
  {
    set_command(command, options);
    create_info.options|= scope; // HA_LEX_CREATE_TMP_TABLE or 0
  }
  bool check_create_options(DDL_options_st options)
  {
    if (options.or_replace() && options.if_not_exists())
    {
      my_error(ER_WRONG_USAGE, MYF(0), "OR REPLACE", "IF NOT EXISTS");
      return true;
    }
    return false;
  }
  bool set_create_options_with_check(DDL_options_st options)
  {
    create_info.set(options);
    return check_create_options(create_info);
  }
  bool add_create_options_with_check(DDL_options_st options)
  {
    create_info.add(options);
    return check_create_options(create_info);
  }
  bool sp_add_cfetch(THD *thd, const LEX_CSTRING *name);
  bool sp_add_agg_cfetch();

  bool set_command_with_check(enum_sql_command command,
                              uint scope,
                              DDL_options_st options)
  {
    set_command(command, scope, options);
    return check_create_options(options);
  }
  bool set_command_with_check(enum_sql_command command, DDL_options_st options)
  {
    set_command(command, options);
    return check_create_options(options);
  }
  /*
    DROP shares lex->create_info to store TEMPORARY and IF EXISTS options
    to save on extra initialization in lex_start().
    Add some wrappers, to avoid direct use of lex->create_info in the
    caller code processing DROP statements (which might look confusing).
  */
  bool tmp_table() const { return create_info.tmp_table(); }
  bool if_exists() const { return create_info.if_exists(); }

  /*
    Run specified phases for derived tables/views in the given list

    @param table_list - list of derived tables/view to handle
    @param phase      - phases to process tables/views through

    @details
    This method runs phases specified by the 'phases' on derived
    tables/views found in the 'table_list' with help of the
    TABLE_LIST::handle_derived function.
    'this' is passed as an argument to the TABLE_LIST::handle_derived.

    @return false -  ok
    @return true  -  error
  */
  bool handle_list_of_derived(TABLE_LIST *table_list, uint phases)
  {
    for (TABLE_LIST *tl= table_list; tl; tl= tl->next_local)
    {
      if (tl->is_view_or_derived() && tl->handle_derived(this, phases))
        return true;
    }
    return false;
  }

  bool create_like() const
  {
    DBUG_ASSERT(!create_info.like() ||
                !first_select_lex()->item_list.elements);
    return create_info.like();
  }

  bool create_select() const
  {
    DBUG_ASSERT(!create_info.like() ||
                !first_select_lex()->item_list.elements);
    return first_select_lex()->item_list.elements;
  }

  bool create_simple() const
  {
    return !create_like() && !create_select();
  }

  SELECT_LEX *exclude_last_select();
  SELECT_LEX *exclude_not_first_select(SELECT_LEX *exclude);
  void check_automatic_up(enum sub_select_type type);
  bool create_or_alter_view_finalize(THD *thd, Table_ident *table_ident);
  bool add_alter_view(THD *thd, uint16 algorithm, enum_view_suid suid,
                      Table_ident *table_ident);
  bool add_create_view(THD *thd, DDL_options_st ddl,
                       uint16 algorithm, enum_view_suid suid,
                       Table_ident *table_ident);
  bool add_grant_command(THD *thd, const List<LEX_COLUMN> &columns);

  bool stmt_grant_table(THD *thd,
                        Grant_privilege *grant,
                        const Lex_grant_object_name &ident,
                        privilege_t grant_option);

  bool stmt_revoke_table(THD *thd,
                         Grant_privilege *grant,
                         const Lex_grant_object_name &ident);

  bool stmt_grant_sp(THD *thd,
                     Grant_privilege *grant,
                     const Lex_grant_object_name &ident,
                     const Sp_handler &sph,
                     privilege_t grant_option);

  bool stmt_revoke_sp(THD *thd,
                      Grant_privilege *grant,
                      const Lex_grant_object_name &ident,
                      const Sp_handler &sph);

  bool stmt_grant_proxy(THD *thd, LEX_USER *user, privilege_t grant_option);
  bool stmt_revoke_proxy(THD *thd, LEX_USER *user);

  Vers_parse_info &vers_get_info()
  {
    return create_info.vers_info;
  }

  /* The list of history-generating DML commands */
  bool vers_history_generating() const
  {
    switch (sql_command)
    {
      case SQLCOM_DELETE:
        return !vers_conditions.delete_history;
      case SQLCOM_UPDATE:
      case SQLCOM_UPDATE_MULTI:
      case SQLCOM_DELETE_MULTI:
      case SQLCOM_REPLACE:
      case SQLCOM_REPLACE_SELECT:
        return true;
      case SQLCOM_INSERT:
      case SQLCOM_INSERT_SELECT:
        return duplicates == DUP_UPDATE;
      case SQLCOM_LOAD:
        return duplicates == DUP_REPLACE;
      default:
        return false;
    }
  }

  int add_period(Lex_ident name, Lex_ident_sys_st start, Lex_ident_sys_st end)
  {
    if (check_period_name(name.str)) {
      my_error(ER_WRONG_COLUMN_NAME, MYF(0), name.str);
      return 1;
    }

    if (lex_string_cmp(system_charset_info, &start, &end) == 0)
    {
      my_error(ER_FIELD_SPECIFIED_TWICE, MYF(0), start.str);
      return 1;
    }

    Table_period_info &info= create_info.period_info;

    if (check_exists && info.name.streq(name))
      return 0;

    if (info.is_set())
    {
       my_error(ER_MORE_THAN_ONE_PERIOD, MYF(0));
       return 1;
    }
    info.set_period(start, end);
    info.name= name;

    info.constr= new Virtual_column_info();
    info.constr->expr= lt_creator.create(thd,
                                         create_item_ident_nosp(thd, &start),
                                         create_item_ident_nosp(thd, &end));
    add_constraint(null_clex_str, info.constr, false);
    return 0;
  }

  sp_package *get_sp_package() const;

  /**
    Check if the select is a simple select (not an union).
    @retval
      0 ok
    @retval
      1 error   ; In this case the error messege is sent to the client
  */
  bool check_simple_select(const LEX_CSTRING *option)
  {
    if (current_select != &builtin_select)
    {
      char command[80];
      strmake(command, option->str, MY_MIN(option->length, sizeof(command)-1));
      my_error(ER_CANT_USE_OPTION_HERE, MYF(0), command);
      return true;
    }
    return false;
  }

  SELECT_LEX_UNIT *alloc_unit();
  SELECT_LEX *alloc_select(bool is_select);
  SELECT_LEX_UNIT *create_unit(SELECT_LEX*);
  SELECT_LEX *wrap_unit_into_derived(SELECT_LEX_UNIT *unit);
  SELECT_LEX *wrap_select_chain_into_derived(SELECT_LEX *sel);
  void init_select()
  {
    current_select->init_select();
    wild= 0;
    exchange= 0;
  }
  bool main_select_push(bool service= false);
  bool insert_select_hack(SELECT_LEX *sel);
  SELECT_LEX *create_priority_nest(SELECT_LEX *first_in_nest);

  bool set_main_unit(st_select_lex_unit *u)
  {
    unit.options= u->options;
    unit.uncacheable= u->uncacheable;
    unit.register_select_chain(u->first_select());
    unit.first_select()->options|= builtin_select.options;
    unit.fake_select_lex= u->fake_select_lex;
    unit.union_distinct= u->union_distinct;
    unit.set_with_clause(u->with_clause);
    builtin_select.exclude_from_global();
    return false;
  }
  bool check_main_unit_semantics();

  SELECT_LEX_UNIT *parsed_select_expr_start(SELECT_LEX *s1, SELECT_LEX *s2,
                                            enum sub_select_type unit_type,
                                            bool distinct);
  SELECT_LEX_UNIT *parsed_select_expr_cont(SELECT_LEX_UNIT *unit,
                                           SELECT_LEX *s2,
                                           enum sub_select_type unit_type,
                                           bool distinct, bool oracle);
  bool parsed_multi_operand_query_expression_body(SELECT_LEX_UNIT *unit);
  SELECT_LEX_UNIT *add_tail_to_query_expression_body(SELECT_LEX_UNIT *unit,
						     Lex_order_limit_lock *l);
  SELECT_LEX_UNIT *
  add_tail_to_query_expression_body_ext_parens(SELECT_LEX_UNIT *unit,
					       Lex_order_limit_lock *l);
  SELECT_LEX_UNIT *parsed_body_ext_parens_primary(SELECT_LEX_UNIT *unit,
                                                  SELECT_LEX *primary,
                                              enum sub_select_type unit_type,
                                              bool distinct);
  SELECT_LEX_UNIT *
  add_primary_to_query_expression_body(SELECT_LEX_UNIT *unit,
                                       SELECT_LEX *sel,
                                       enum sub_select_type unit_type,
                                       bool distinct,
                                       bool oracle);
  SELECT_LEX_UNIT *
  add_primary_to_query_expression_body(SELECT_LEX_UNIT *unit,
                                       SELECT_LEX *sel,
                                       enum sub_select_type unit_type,
                                       bool distinct);
  SELECT_LEX_UNIT *
  add_primary_to_query_expression_body_ext_parens(
                                       SELECT_LEX_UNIT *unit,
                                       SELECT_LEX *sel,
                                       enum sub_select_type unit_type,
                                       bool distinct);
  SELECT_LEX *parsed_subselect(SELECT_LEX_UNIT *unit);
  bool parsed_insert_select(SELECT_LEX *firs_select);
  void save_values_list_state();
  void restore_values_list_state();
  bool parsed_TVC_start();
  SELECT_LEX *parsed_TVC_end();
  TABLE_LIST *parsed_derived_table(SELECT_LEX_UNIT *unit,
                                   int for_system_time,
                                   LEX_CSTRING *alias);
  bool parsed_create_view(SELECT_LEX_UNIT *unit, int check);
  bool select_finalize(st_select_lex_unit *expr);
  bool select_finalize(st_select_lex_unit *expr, Lex_select_lock l);
  void relink_hack(st_select_lex *select_lex);

  bool stmt_install_plugin(const DDL_options_st &opt,
                           const Lex_ident_sys_st &name,
                           const LEX_CSTRING &soname);
  void stmt_install_plugin(const LEX_CSTRING &soname);

  bool stmt_uninstall_plugin_by_name(const DDL_options_st &opt,
                                     const Lex_ident_sys_st &name);
  bool stmt_uninstall_plugin_by_soname(const DDL_options_st &opt,
                                       const LEX_CSTRING &soname);
  bool stmt_prepare_validate(const char *stmt_type);
  bool stmt_prepare(const Lex_ident_sys_st &ident, Item *code);
  bool stmt_execute(const Lex_ident_sys_st &ident, List<Item> *params);
  bool stmt_execute_immediate(Item *code, List<Item> *params);
  void stmt_deallocate_prepare(const Lex_ident_sys_st &ident);

  bool stmt_alter_table_exchange_partition(Table_ident *table);
  bool stmt_alter_table(Table_ident *table);

  void stmt_purge_to(const LEX_CSTRING &to);
  bool stmt_purge_before(Item *item);

  SELECT_LEX *returning()
  { return &builtin_select; }
  bool has_returning()
  { return !builtin_select.item_list.is_empty(); }

private:
  bool stmt_create_routine_start(const DDL_options_st &options)
  {
    create_info.set(options);
    return main_select_push() || check_create_options(options);
  }
public:
  bool stmt_create_function_start(const DDL_options_st &options)
  {
    sql_command= SQLCOM_CREATE_SPFUNCTION;
    return stmt_create_routine_start(options);
  }
  bool stmt_create_procedure_start(const DDL_options_st &options)
  {
    sql_command= SQLCOM_CREATE_PROCEDURE;
    return stmt_create_routine_start(options);
  }
  void stmt_create_routine_finalize()
  {
    pop_select(); // main select
  }

  bool stmt_create_stored_function_start(const DDL_options_st &options,
                                         enum_sp_aggregate_type,
                                         const sp_name *name);
  bool stmt_create_stored_function_finalize_standalone(const sp_name *end_name);

  bool stmt_create_udf_function(const DDL_options_st &options,
                                enum_sp_aggregate_type agg_type,
                                const Lex_ident_sys_st &name,
                                Item_result return_type,
                                const LEX_CSTRING &soname);

  bool stmt_drop_function(const DDL_options_st &options,
                          const Lex_ident_sys_st &db,
                          const Lex_ident_sys_st &name);

  bool stmt_drop_function(const DDL_options_st &options,
                          const Lex_ident_sys_st &name);

  bool stmt_drop_procedure(const DDL_options_st &options,
                           sp_name *name);

  bool stmt_alter_function_start(sp_name *name);
  bool stmt_alter_procedure_start(sp_name *name);

  sp_condition_value *stmt_signal_value(const Lex_ident_sys_st &ident);

  Spvar_definition *row_field_name(THD *thd, const Lex_ident_sys_st &name);

  bool set_field_type_udt(Lex_field_type_st *type,
                          const LEX_CSTRING &name,
                          const Lex_length_and_dec_st &attr);
  bool set_cast_type_udt(Lex_cast_type_st *type,
                         const LEX_CSTRING &name);

  bool map_data_type(const Lex_ident_sys_st &schema,
                     Lex_field_type_st *type) const;

  void mark_first_table_as_inserting();

  bool fields_are_impossible()
  {
    // no select or it is last select with no tables (service select)
    return !select_stack_head() ||
           (select_stack_top == 1 &&
            select_stack[0]->is_service_select);
  }

  bool add_table_foreign_key(const LEX_CSTRING *name,
                             const LEX_CSTRING *constraint_name,
                             Table_ident *table_name,
                             DDL_options ddl_options);
  bool add_column_foreign_key(const LEX_CSTRING *name,
                              const LEX_CSTRING *constraint_name,
                              Table_ident *ref_table_name,
                              DDL_options ddl_options);

  bool check_dependencies_in_with_clauses();
  bool check_cte_dependencies_and_resolve_references();
  bool resolve_references_to_cte(TABLE_LIST *tables,
                                 TABLE_LIST **tables_last);

  /**
    Turn on the SELECT_DESCRIBE flag for every SELECT_LEX involved into
    the statement being processed in case the statement is EXPLAIN UPDATE/DELETE.

    @param lex  current LEX
  */

  void promote_select_describe_flag_if_needed()
  {
    if (describe)
      builtin_select.options |= SELECT_DESCRIBE;
  }


  /**
    Check if the current statement uses meta-data (uses a table or a stored
    routine).
  */
  bool is_metadata_used() const
  {
    return query_tables != nullptr || sroutines.records > 0;
  }

  virtual sp_lex_cursor* get_lex_for_cursor()
  {
    return nullptr;
  }
};


/**
  Set_signal_information is a container used in the parsed tree to represent
  the collection of assignments to condition items in the SIGNAL and RESIGNAL
  statements.
*/
class Set_signal_information
{
public:
  /** Empty default constructor, use clear() */
 Set_signal_information() = default; 

  /** Copy constructor. */
  Set_signal_information(const Set_signal_information& set);

  /** Destructor. */
  ~Set_signal_information() = default;

  /** Clear all items. */
  void clear();

  /**
    For each condition item assignment, m_item[] contains the parsed tree
    that represents the expression assigned, if any.
    m_item[] is an array indexed by Diag_condition_item_name.
  */
  Item *m_item[LAST_DIAG_SET_PROPERTY+1];
};


/**
  The internal state of the syntax parser.
  This object is only available during parsing,
  and is private to the syntax parser implementation (sql_yacc.yy).
*/
class Yacc_state
{
public:
  Yacc_state() : yacc_yyss(NULL), yacc_yyvs(NULL) { reset(); }

  void reset()
  {
    if (yacc_yyss != NULL) {
      my_free(yacc_yyss);
      yacc_yyss = NULL;
    }
    if (yacc_yyvs != NULL) {
      my_free(yacc_yyvs);
      yacc_yyvs = NULL;
    }
    m_set_signal_info.clear();
    m_lock_type= TL_READ_DEFAULT;
    m_mdl_type= MDL_SHARED_READ;
  }

  ~Yacc_state();

  /**
    Reset part of the state which needs resetting before parsing
    substatement.
  */
  void reset_before_substatement()
  {
    m_lock_type= TL_READ_DEFAULT;
    m_mdl_type= MDL_SHARED_READ;
  }

  /**
    Bison internal state stack, yyss, when dynamically allocated using
    my_yyoverflow().
  */
  uchar *yacc_yyss;

  /**
    Bison internal semantic value stack, yyvs, when dynamically allocated using
    my_yyoverflow().
  */
  uchar *yacc_yyvs;

  /**
    Fragments of parsed tree,
    used during the parsing of SIGNAL and RESIGNAL.
  */
  Set_signal_information m_set_signal_info;

  /**
    Type of lock to be used for tables being added to the statement's
    table list in table_factor, table_alias_ref, single_multi and
    table_wild_one rules.
    Statements which use these rules but require lock type different
    from one specified by this member have to override it by using
    st_select_lex::set_lock_for_tables() method.

    The default value of this member is TL_READ_DEFAULT. The only two
    cases in which we change it are:
    - When parsing SELECT HIGH_PRIORITY.
    - Rule for DELETE. In which we use this member to pass information
      about type of lock from delete to single_multi part of rule.

    We should try to avoid introducing new use cases as we would like
    to get rid of this member eventually.
  */
  thr_lock_type m_lock_type;

  /**
    The type of requested metadata lock for tables added to
    the statement table list.
  */
  enum_mdl_type m_mdl_type;

  /*
    TODO: move more attributes from the LEX structure here.
  */
};

/**
  Internal state of the parser.
  The complete state consist of:
  - state data used during lexical parsing,
  - state data used during syntactic parsing.
*/
class Parser_state
{
public:
  Parser_state()
    : m_yacc()
  {}

  /**
     Object initializer. Must be called before usage.

     @retval FALSE OK
     @retval TRUE  Error
  */
  bool init(THD *thd, char *buff, size_t length)
  {
    return m_lip.init(thd, buff, length);
  }

  ~Parser_state() = default;

  Lex_input_stream m_lip;
  Yacc_state m_yacc;

  /**
    Current performance digest instrumentation. 
  */
  PSI_digest_locker* m_digest_psi;

  void reset(char *found_semicolon, unsigned int length)
  {
    m_lip.reset(found_semicolon, length);
    m_yacc.reset();
  }
};


extern sql_digest_state *
digest_add_token(sql_digest_state *state, uint token, LEX_YYSTYPE yylval);

extern sql_digest_state *
digest_reduce_token(sql_digest_state *state, uint token_left, uint token_right);

struct st_lex_local: public LEX, public Sql_alloc
{
};


/**
  An st_lex_local extension with automatic initialization for SP purposes.
  Used to parse sub-expressions and SP sub-statements.

  This class is reused for:
  1. sp_head::reset_lex() based constructs
    - SP variable assignments (e.g. SET x=10;)
    - FOR loop conditions and index variable increments
    - Cursor statements
    - SP statements
    - SP function RETURN statements
    - CASE statements
    - REPEAT..UNTIL expressions
    - WHILE expressions
    - EXIT..WHEN and CONTINUE..WHEN statements
  2. sp_assignment_lex based constructs:
    - CURSOR parameter assignments
*/
class sp_lex_local: public st_lex_local
{
public:
  sp_lex_local(THD *thd, const LEX *oldlex)
  {
    /* Reset most stuff. */
    start(thd);
    /* Keep the parent SP stuff */
    sphead= oldlex->sphead;
    spcont= oldlex->spcont;
    /* Keep the parent trigger stuff too */
    trg_chistics= oldlex->trg_chistics;
    sp_lex_in_use= false;
  }
};


class sp_lex_set_var: public sp_lex_local
{
public:
  sp_lex_set_var(THD *thd, const LEX *oldlex)
   :sp_lex_local(thd, oldlex)
  {
    // Set new LEX as if we at start of set rule
    init_select();
    sql_command= SQLCOM_SET_OPTION;
    var_list.empty();
    autocommit= 0;
    option_type= oldlex->option_type; // Inherit from the outer lex
  }
};


class sp_expr_lex: public sp_lex_local
{
  Item *m_item;       // The expression
  LEX_CSTRING m_expr_str;
public:
  sp_expr_lex(THD *thd, LEX *oldlex)
   :sp_lex_local(thd, oldlex),
    m_item(nullptr),
    m_expr_str(empty_clex_str)
  { }
  void set_item(Item *item)
  {
    m_item= item;
  }
  Item *get_item() const
  {
    return m_item;
  }
  bool sp_continue_when_statement(THD *thd);
  bool sp_continue_when_statement(THD *thd, const LEX_CSTRING *label_name);
  int case_stmt_action_expr();
  int case_stmt_action_when(bool simple);
  bool sp_while_loop_expression(THD *thd)
  {
    return LEX::sp_while_loop_expression(thd, get_item(), m_expr_str);
  }
  bool sp_repeat_loop_finalize(THD *thd);
  bool sp_if_expr(THD *thd);
  void set_expr_str(const LEX_CSTRING &expr_str)
  {
    m_expr_str= expr_str;
  }
  const LEX_CSTRING &get_expr_str() const
  {
    return m_expr_str;
  }
};


/**
  An assignment specific LEX, which additionally has an Item (an expression)
  and an associated with the Item free_list, which is usually freed
  after the expression is calculated.

  Note, consider changing some of sp_lex_local to sp_assignment_lex,
  as the latter allows to use a simpler grammar in sql_yacc.yy (IMO).

  If the expression is simple (e.g. does not have function calls),
  then m_item and m_free_list point to the same Item.

  If the expressions is complex (e.g. have function calls),
  then m_item points to the leftmost Item, while m_free_list points
  to the rightmost item.
  For example:
      f1(COALESCE(f2(10), f2(20)))
  - m_item points to Item_func_sp for f1 (the leftmost Item)
  - m_free_list points to Item_int for 20 (the rightmost Item)

  Note, we could avoid storing m_item at all, as we can always reach
  the leftmost item from the rightmost item by iterating through m_free_list.
  But with a separate m_item the code should be faster.
*/
class sp_assignment_lex: public sp_lex_local
{
  Item *m_item;       // The expression
  Item *m_free_list;  // The associated free_list (sub-expressions)
  LEX_CSTRING m_expr_str;
public:
  sp_assignment_lex(THD *thd, LEX *oldlex)
   :sp_lex_local(thd, oldlex),
    m_item(NULL),
    m_free_list(nullptr),
    m_expr_str(empty_clex_str)
  { }
  void set_item_and_free_list(Item *item, Item *free_list)
  {
    m_item= item;
    m_free_list= free_list;
  }
  Item *get_item() const
  {
    return m_item;
  }
  Item *get_free_list() const
  {
    return m_free_list;
  }
  void set_expr_str(const LEX_CSTRING &expr_str)
  {
    m_expr_str= expr_str;
  }
  const LEX_CSTRING &get_expr_str() const
  {
    return m_expr_str;
  }
};


extern void lex_init(void);
extern void lex_free(void);
extern void lex_start(THD *thd);
extern void lex_end(LEX *lex);
extern void lex_end_nops(LEX *lex);
extern void lex_unlock_plugins(LEX *lex);
void end_lex_with_single_table(THD *thd, TABLE *table, LEX *old_lex);
int init_lex_with_single_table(THD *thd, TABLE *table, LEX *lex);
extern int MYSQLlex(union YYSTYPE *yylval, THD *thd);
extern int ORAlex(union YYSTYPE *yylval, THD *thd);

inline void trim_whitespace(CHARSET_INFO *cs, LEX_CSTRING *str,
                            size_t * prefix_length = 0)
{
  *str= Lex_cstring(*str).trim_whitespace(cs, prefix_length);
}


extern bool is_lex_native_function(const LEX_CSTRING *name); 
extern bool is_native_function(THD *thd, const LEX_CSTRING *name);
extern bool is_native_function_with_warn(THD *thd, const LEX_CSTRING *name);

/**
  @} (End of group Semantic_Analysis)
*/

void my_missing_function_error(const LEX_CSTRING &token, const char *name);
bool is_keyword(const char *name, uint len);
int set_statement_var_if_exists(THD *thd, const char *var_name,
                                size_t var_name_length, ulonglong value);

Virtual_column_info *add_virtual_expression(THD *thd, Item *expr);
Item* handle_sql2003_note184_exception(THD *thd, Item* left, bool equal,
                                       Item *expr);

bool sp_create_assignment_lex(THD *thd, const char *pos);
bool sp_create_assignment_instr(THD *thd, bool no_lookahead,
                                bool need_set_keyword= true);

void mark_or_conds_to_avoid_pushdown(Item *cond);

#endif /* MYSQL_SERVER */
#endif /* SQL_LEX_INCLUDED */
