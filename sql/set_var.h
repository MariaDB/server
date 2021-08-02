#ifndef SET_VAR_INCLUDED
#define SET_VAR_INCLUDED
/* Copyright (c) 2002, 2013, Oracle and/or its affiliates.
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
  "public" interface to sys_var - server configuration variables.
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface                       /* gcc class implementation */
#endif

#include <my_getopt.h>

class sys_var;
class set_var;
class sys_var_pluginvar;
class PolyLock;
class Item_func_set_user_var;

// This include needs to be here since item.h requires enum_var_type :-P
#include "item.h"                          /* Item */
#include "sql_class.h"                     /* THD  */

extern TYPELIB bool_typelib;

struct sys_var_chain
{
  sys_var *first;
  sys_var *last;
};

int mysql_add_sys_var_chain(sys_var *chain);
int mysql_del_sys_var_chain(sys_var *chain);


/**
  A class representing one system variable - that is something
  that can be accessed as @@global.variable_name or @@session.variable_name,
  visible in SHOW xxx VARIABLES and in INFORMATION_SCHEMA.xxx_VARIABLES,
  optionally it can be assigned to, optionally it can have a command-line
  counterpart with the same name.
*/
class sys_var: protected Value_source // for double_from_string_with_check
{
public:
  sys_var *next;
  LEX_CSTRING name;
  bool *test_load;
  enum flag_enum { GLOBAL, SESSION, ONLY_SESSION, SCOPE_MASK=1023,
                   READONLY=1024, ALLOCATED=2048, PARSE_EARLY=4096,
                   NO_SET_STATEMENT=8192, AUTO_SET=16384};
  enum { NO_GETOPT=-1, GETOPT_ONLY_HELP=-2 };
  enum where { CONFIG, COMMAND_LINE, AUTO, SQL, COMPILE_TIME, ENV };

  /**
    Enumeration type to indicate for a system variable whether
    it will be written to the binlog or not.
  */    
  enum binlog_status_enum { VARIABLE_NOT_IN_BINLOG,
                            SESSION_VARIABLE_IN_BINLOG } binlog_status;

  my_option option;     ///< min, max, default values are stored here
  enum where value_origin;
  const char *origin_filename;

protected:
  typedef bool (*on_check_function)(sys_var *self, THD *thd, set_var *var);
  typedef bool (*on_update_function)(sys_var *self, THD *thd, enum_var_type type);

  int flags;            ///< or'ed flag_enum values
  const SHOW_TYPE show_val_type; ///< what value_ptr() returns for sql_show.cc
  PolyLock *guard;      ///< *second* lock that protects the variable
  ptrdiff_t offset;     ///< offset to the value from global_system_variables
  on_check_function on_check;
  on_update_function on_update;
  const char *const deprecation_substitute;

public:
  sys_var(sys_var_chain *chain, const char *name_arg, const char *comment,
          int flag_args, ptrdiff_t off, int getopt_id,
          enum get_opt_arg_type getopt_arg_type, SHOW_TYPE show_val_type_arg,
          longlong def_val, PolyLock *lock, enum binlog_status_enum binlog_status_arg,
          on_check_function on_check_func, on_update_function on_update_func,
          const char *substitute);

  virtual ~sys_var() {}

  /**
    All the cleanup procedures should be performed here
  */
  virtual void cleanup() {}
  /**
    downcast for sys_var_pluginvar. Returns this if it's an instance
    of sys_var_pluginvar, and 0 otherwise.
  */
  virtual sys_var_pluginvar *cast_pluginvar() { return 0; }

  bool check(THD *thd, set_var *var);
  const uchar *value_ptr(THD *thd, enum_var_type type, const LEX_CSTRING *base) const;

  /**
     Update the system variable with the default value from either
     session or global scope.  The default value is stored in the
     'var' argument. Return false when successful.
  */
  bool set_default(THD *thd, set_var *var);
  bool update(THD *thd, set_var *var);

  String *val_str_nolock(String *str, THD *thd, const uchar *value);
  longlong val_int(bool *is_null, THD *thd, enum_var_type type, const LEX_CSTRING *base);
  String *val_str(String *str, THD *thd, enum_var_type type, const LEX_CSTRING *base);
  double val_real(bool *is_null, THD *thd, enum_var_type type, const LEX_CSTRING *base);

  SHOW_TYPE show_type() const { return show_val_type; }
  int scope() const { return flags & SCOPE_MASK; }
  virtual CHARSET_INFO *charset(THD *thd) const
  {
    return system_charset_info;
  }
  bool is_readonly() const { return flags & READONLY; }
  /**
    the following is only true for keycache variables,
    that support the syntax @@keycache_name.variable_name
  */
  bool is_struct() { return option.var_type & GET_ASK_ADDR; }
  bool is_set_stmt_ok() const { return !(flags & NO_SET_STATEMENT); }
  bool is_written_to_binlog(enum_var_type type)
  { return type != OPT_GLOBAL && binlog_status == SESSION_VARIABLE_IN_BINLOG; }
  bool check_update_type(const Item *item)
  {
    Item_result type= item->result_type();
    switch (option.var_type & GET_TYPE_MASK) {
    case GET_INT:
    case GET_UINT:
    case GET_LONG:
    case GET_ULONG:
    case GET_LL:
    case GET_ULL:
      return type != INT_RESULT &&
             (type != DECIMAL_RESULT || item->decimals != 0);
    case GET_STR:
    case GET_STR_ALLOC:
      return type != STRING_RESULT;
    case GET_ENUM:
    case GET_BOOL:
    case GET_SET:
    case GET_FLAGSET:
    case GET_BIT:
      return type != STRING_RESULT && type != INT_RESULT;
    case GET_DOUBLE:
      return type != INT_RESULT && type != REAL_RESULT && type != DECIMAL_RESULT;
    default:
      return true;
    }
  }

  bool check_type(enum_var_type type)
  {
    switch (scope())
    {
    case GLOBAL:       return type != OPT_GLOBAL;
    case SESSION:      return false; // always ok
    case ONLY_SESSION: return type == OPT_GLOBAL;
    }
    return true; // keep gcc happy
  }
  bool register_option(DYNAMIC_ARRAY *array, int parse_flags)
  {
    DBUG_ASSERT(parse_flags == GETOPT_ONLY_HELP ||
                parse_flags == PARSE_EARLY || parse_flags == 0);
    if (option.id == NO_GETOPT)
      return 0;
    if (parse_flags == GETOPT_ONLY_HELP)
    {
      if (option.id != GETOPT_ONLY_HELP)
        return 0;
    }
    else
    {
      if (option.id == GETOPT_ONLY_HELP)
        return 0;
      if ((flags & PARSE_EARLY) != parse_flags)
        return 0;
    }
    return insert_dynamic(array, (uchar*)&option);
  }
  void do_deprecated_warning(THD *thd);
  /**
    whether session value of a sysvar is a default one.

    in this simple implementation we don't distinguish between default
    and non-default values. for most variables it's ok, they don't treat
    default values specially. this method is overwritten in descendant
    classes as necessary.
  */
  virtual bool session_is_default(THD *thd) { return false; }

  virtual const uchar *default_value_ptr(THD *thd) const
  { return (uchar*)&option.def_value; }

  virtual bool on_check_access_global(THD *thd) const;
  virtual bool on_check_access_session(THD *thd) const
  {
    return false;
  }

private:
  virtual bool do_check(THD *thd, set_var *var) = 0;
  /**
    save the session default value of the variable in var
  */
  virtual void session_save_default(THD *thd, set_var *var) = 0;
  /**
    save the global default value of the variable in var
  */
  virtual void global_save_default(THD *thd, set_var *var) = 0;
  virtual bool session_update(THD *thd, set_var *var) = 0;
  virtual bool global_update(THD *thd, set_var *var) = 0;

protected:
  /**
    A pointer to a value of the variable for SHOW.
    It must be of show_val_type type (my_bool for SHOW_MY_BOOL,
    int for SHOW_INT, longlong for SHOW_LONGLONG, etc).
  */
  virtual const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const;
  virtual const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const;

  /**
    A pointer to a storage area of the variable, to the raw data.
    Typically it's the same as session_value_ptr(), but it's different,
    for example, for ENUM, that is printed as a string, but stored as a number.
  */
  uchar *session_var_ptr(THD *thd) const
  { return ((uchar*)&(thd->variables)) + offset; }

  uchar *global_var_ptr() const
  { return ((uchar*)&global_system_variables) + offset; }

  void *max_var_ptr()
  {
    return scope() == SESSION ? (((uchar*)&max_system_variables) + offset) :
                                0;
  }

  friend class Session_sysvars_tracker;
  friend class Session_tracker;
};

#include "sql_plugin.h"                    /* SHOW_HA_ROWS, SHOW_MY_BOOL */


/****************************************************************************
  Classes for parsing of the SET command
****************************************************************************/

/**
  A base class for everything that can be set with SET command.
  It's similar to Items, an instance of this is created by the parser
  for every assigmnent in SET (or elsewhere, e.g. in SELECT).
*/
class set_var_base :public Sql_alloc
{
public:
  set_var_base() {}
  virtual ~set_var_base() {}
  virtual int check(THD *thd)=0;           /* To check privileges etc. */
  virtual int update(THD *thd)=0;                  /* To set the value */
  virtual int light_check(THD *thd) { return check(thd); }   /* for PS */
  virtual bool is_system() { return FALSE; }
  /**
    @returns whether this variable is @@@@optimizer_trace.
  */
  virtual bool is_var_optimizer_trace() const { return false; }
};


/**
  Structure for holding unix timestamp and high precision second part.
 */
typedef struct my_time_t_hires
{
  my_time_t unix_time;
  ulong second_part;
} my_time_t_hires;


/**
  set_var_base descendant for assignments to the system variables.
*/
class set_var :public set_var_base
{
public:
  sys_var *var; ///< system variable to be updated
  Item *value;  ///< the expression that provides the new value of the variable
  enum_var_type type;
  union ///< temp storage to hold a value between sys_var::check and ::update
  {
    ulonglong ulonglong_value;          ///< for unsigned integer, set, enum sysvars
    longlong longlong_value;            ///< for signed integer
    double double_value;                ///< for Sys_var_double
    plugin_ref plugin;                  ///< for Sys_var_plugin
    plugin_ref *plugins;                ///< for Sys_var_pluginlist
    Time_zone *time_zone;               ///< for Sys_var_tz
    LEX_STRING string_value;            ///< for Sys_var_charptr and others
    my_time_t_hires timestamp;          ///< for Sys_var_vers_asof
    const void *ptr;                    ///< for Sys_var_struct
  } save_result;
  LEX_CSTRING base; /**< for structured variables, like keycache_name.variable_name */

  set_var(THD *thd, enum_var_type type_arg, sys_var *var_arg,
          const LEX_CSTRING *base_name_arg, Item *value_arg);
  virtual bool is_system() { return 1; }
  int check(THD *thd);
  int update(THD *thd);
  int light_check(THD *thd);
  virtual bool is_var_optimizer_trace() const
  {
    extern sys_var *Sys_optimizer_trace_ptr;
    return var == Sys_optimizer_trace_ptr;
  }
};


/* User variables like @my_own_variable */
class set_var_user: public set_var_base
{
  Item_func_set_user_var *user_var_item;
public:
  set_var_user(Item_func_set_user_var *item)
    :user_var_item(item)
  {}
  int check(THD *thd);
  int update(THD *thd);
  int light_check(THD *thd);
};

/* For SET PASSWORD */

class set_var_password: public set_var_base
{
  LEX_USER *user;
public:
  set_var_password(LEX_USER *user_arg) :user(user_arg)
  {}
  int check(THD *thd);
  int update(THD *thd);
};

/* For SET ROLE */

class set_var_role: public set_var_base
{
  LEX_CSTRING role;
  privilege_t access;
public:
  set_var_role(LEX_CSTRING role_arg) : role(role_arg), access(NO_ACL) {}
  int check(THD *thd);
  int update(THD *thd);
};

/* For SET DEFAULT ROLE */

class set_var_default_role: public set_var_base
{
  LEX_USER *user, *real_user;
  LEX_CSTRING role;
  const char *real_role;
public:
  set_var_default_role(LEX_USER *user_arg, LEX_CSTRING role_arg) :
    user(user_arg), role(role_arg) {}
  int check(THD *thd);
  int update(THD *thd);
};

/* For SET NAMES and SET CHARACTER SET */

class set_var_collation_client: public set_var_base
{
  CHARSET_INFO *character_set_client;
  CHARSET_INFO *character_set_results;
  CHARSET_INFO *collation_connection;
public:
  set_var_collation_client(CHARSET_INFO *client_coll_arg,
                           CHARSET_INFO *connection_coll_arg,
                           CHARSET_INFO *result_coll_arg)
    :character_set_client(client_coll_arg),
     character_set_results(result_coll_arg),
     collation_connection(connection_coll_arg)
  {}
  int check(THD *thd);
  int update(THD *thd);
};


/* optional things, have_* variables */
extern SHOW_COMP_OPTION have_csv, have_innodb;
extern SHOW_COMP_OPTION have_ndbcluster, have_partitioning;
extern SHOW_COMP_OPTION have_profiling;

extern SHOW_COMP_OPTION have_ssl, have_symlink, have_dlopen;
extern SHOW_COMP_OPTION have_query_cache;
extern SHOW_COMP_OPTION have_geometry, have_rtree_keys;
extern SHOW_COMP_OPTION have_crypt;
extern SHOW_COMP_OPTION have_compress;
extern SHOW_COMP_OPTION have_openssl;

/*
  Prototypes for helper functions
*/
ulong get_system_variable_hash_records(void);
ulonglong get_system_variable_hash_version(void);

SHOW_VAR* enumerate_sys_vars(THD *thd, bool sorted, enum enum_var_type type);
int fill_sysvars(THD *thd, TABLE_LIST *tables, COND *cond);

sys_var *find_sys_var(THD *thd, const char *str, size_t length= 0,
                      bool throw_error= false);
int sql_set_variables(THD *thd, List<set_var_base> *var_list, bool free);

#define SYSVAR_AUTOSIZE(VAR,VAL)                        \
  do {                                                  \
    VAR= (VAL);                                         \
    set_sys_var_value_origin(&VAR, sys_var::AUTO);      \
  } while(0)

#define SYSVAR_AUTOSIZE_IF_CHANGED(VAR,VAL,TYPE)        \
  do {                                                  \
    TYPE tmp= (VAL);                                    \
    if (VAR != tmp)                                     \
    {                                                   \
      VAR= (VAL);                                       \
      set_sys_var_value_origin(&VAR, sys_var::AUTO);    \
    }                                                   \
  } while(0)

void set_sys_var_value_origin(void *ptr, enum sys_var::where here);

enum sys_var::where get_sys_var_value_origin(void *ptr);
inline bool IS_SYSVAR_AUTOSIZE(void *ptr)
{
  enum sys_var::where res= get_sys_var_value_origin(ptr);
  return (res == sys_var::AUTO || res == sys_var::COMPILE_TIME);
}

bool fix_delay_key_write(sys_var *self, THD *thd, enum_var_type type);

sql_mode_t expand_sql_mode(sql_mode_t sql_mode);
const char *sql_mode_string_representation(uint bit_number);
bool sql_mode_string_representation(THD *thd, sql_mode_t sql_mode,
                                    LEX_CSTRING *ls);
int default_regex_flags_pcre(THD *thd);

extern sys_var *Sys_autocommit_ptr, *Sys_last_gtid_ptr,
  *Sys_character_set_client_ptr, *Sys_character_set_connection_ptr,
  *Sys_character_set_results_ptr;

CHARSET_INFO *get_old_charset_by_name(const char *old_name);

int sys_var_init();
uint sys_var_elements();
int sys_var_add_options(DYNAMIC_ARRAY *long_options, int parse_flags);
void sys_var_end(void);
bool check_has_super(sys_var *self, THD *thd, set_var *var);
plugin_ref *resolve_engine_list(THD *thd, const char *str_arg, size_t str_arg_len,
                                bool error_on_unknown_engine, bool temp_copy);
void free_engine_list(plugin_ref *list);
plugin_ref *copy_engine_list(plugin_ref *list);
plugin_ref *temp_copy_engine_list(THD *thd, plugin_ref *list);
char *pretty_print_engine_list(THD *thd, plugin_ref *list);

#endif
