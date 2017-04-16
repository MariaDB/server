/* Copyright (c) 2002, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2014, SkySQL Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* variable declarations are in sys_vars.cc now !!! */

#include "sql_plugin.h"                         // Includes my_global.h
#include "sql_class.h"                   // set_var.h: session_var_ptr
#include "set_var.h"
#include "sql_priv.h"
#include "unireg.h"
#include "mysqld.h"                             // lc_messages_dir
#include "sys_vars_shared.h"
#include "transaction.h"
#include "sql_locale.h"                         // my_locale_by_number,
                                                // my_locale_by_name
#include "strfunc.h"      // find_set_from_flags, find_set
#include "sql_parse.h"    // check_global_access
#include "sql_table.h"  // reassign_keycache_tables
#include "sql_time.h"   // date_time_format_copy,
                        // date_time_format_make
#include "derror.h"
#include "tztime.h"     // my_tz_find, my_tz_SYSTEM, struct Time_zone
#include "sql_acl.h"    // SUPER_ACL
#include "sql_select.h" // free_underlaid_joins
#include "sql_show.h"
#include "sql_view.h"   // updatable_views_with_limit_typelib
#include "lock.h"                               // lock_global_read_lock,
                                                // make_global_read_lock_block_commit,
                                                // unlock_global_read_lock

static HASH system_variable_hash;
static PolyLock_mutex PLock_global_system_variables(&LOCK_global_system_variables);

/**
  Return variable name and length for hashing of variables.
*/

static uchar *get_sys_var_length(const sys_var *var, size_t *length,
                                 my_bool first)
{
  *length= var->name.length;
  return (uchar*) var->name.str;
}

sys_var_chain all_sys_vars = { NULL, NULL };

int sys_var_init()
{
  DBUG_ENTER("sys_var_init");

  /* Must be already initialized. */
  DBUG_ASSERT(system_charset_info != NULL);

  if (my_hash_init(&system_variable_hash, system_charset_info, 700, 0,
                   0, (my_hash_get_key) get_sys_var_length, 0, HASH_UNIQUE))
    goto error;

  if (mysql_add_sys_var_chain(all_sys_vars.first))
    goto error;

  DBUG_RETURN(0);

error:
  fprintf(stderr, "failed to initialize System variables");
  DBUG_RETURN(1);
}

uint sys_var_elements()
{
  return system_variable_hash.records;
}

int sys_var_add_options(DYNAMIC_ARRAY *long_options, int parse_flags)
{
  uint saved_elements= long_options->elements;

  DBUG_ENTER("sys_var_add_options");

  for (sys_var *var=all_sys_vars.first; var; var= var->next)
  {
    if (var->register_option(long_options, parse_flags))
      goto error;
  }

  DBUG_RETURN(0);

error:
  fprintf(stderr, "failed to initialize System variables");
  long_options->elements= saved_elements;
  DBUG_RETURN(1);
}

void sys_var_end()
{
  DBUG_ENTER("sys_var_end");

  my_hash_free(&system_variable_hash);

  for (sys_var *var=all_sys_vars.first; var; var= var->next)
    var->cleanup();

  DBUG_VOID_RETURN;
}


static bool static_test_load= TRUE;

/**
  sys_var constructor

  @param chain     variables are linked into chain for mysql_add_sys_var_chain()
  @param name_arg  the name of the variable. Must be 0-terminated and exist
                   for the liftime of the sys_var object. @sa my_option::name
  @param comment   shown in mysqld --help, @sa my_option::comment
  @param flags_arg or'ed flag_enum values
  @param off       offset of the global variable value from the
                   &global_system_variables.
  @param getopt_id -1 for no command-line option, otherwise @sa my_option::id
  @param getopt_arg_type @sa my_option::arg_type
  @param show_val_type_arg what value_ptr() returns for sql_show.cc
  @param def_val   default value, @sa my_option::def_value
  @param lock      mutex or rw_lock that protects the global variable
                   *in addition* to LOCK_global_system_variables.
  @param binlog_status_enum @sa binlog_status_enum
  @param on_check_func a function to be called at the end of sys_var::check,
                   put your additional checks here
  @param on_update_func a function to be called at the end of sys_var::update,
                   any post-update activity should happen here
  @param substitute If non-NULL, this variable is deprecated and the
  string describes what one should use instead. If an empty string,
  the variable is deprecated but no replacement is offered.
*/
sys_var::sys_var(sys_var_chain *chain, const char *name_arg,
                 const char *comment, int flags_arg, ptrdiff_t off,
                 int getopt_id, enum get_opt_arg_type getopt_arg_type,
                 SHOW_TYPE show_val_type_arg, longlong def_val,
                 PolyLock *lock, enum binlog_status_enum binlog_status_arg,
                 on_check_function on_check_func,
                 on_update_function on_update_func,
                 const char *substitute) :
  next(0), binlog_status(binlog_status_arg), value_origin(COMPILE_TIME),
  flags(flags_arg), show_val_type(show_val_type_arg),
  guard(lock), offset(off), on_check(on_check_func), on_update(on_update_func),
  deprecation_substitute(substitute),
  is_os_charset(FALSE)
{
  /*
    There is a limitation in handle_options() related to short options:
    - either all short options should be declared when parsing in multiple stages,
    - or none should be declared.
    Because a lot of short options are used in the normal parsing phase
    for mysqld, we enforce here that no short option is present
    in the first (PARSE_EARLY) stage.
    See handle_options() for details.
  */
  DBUG_ASSERT(!(flags & PARSE_EARLY) || getopt_id <= 0 || getopt_id >= 255);

  name.str= name_arg;     // ER_NO_DEFAULT relies on 0-termination of name_arg
  name.length= strlen(name_arg);                // and so does this.
  DBUG_ASSERT(name.length <= NAME_CHAR_LEN);

  bzero(&option, sizeof(option));
  option.name= name_arg;
  option.id= getopt_id;
  option.comment= comment;
  option.arg_type= getopt_arg_type;
  option.value= (uchar **)global_var_ptr();
  option.def_value= def_val;
  option.app_type= this;
  option.var_type= flags & AUTO_SET ? GET_AUTO : 0;

  if (chain->last)
    chain->last->next= this;
  else
    chain->first= this;
  chain->last= this;

  test_load= &static_test_load;
}

bool sys_var::update(THD *thd, set_var *var)
{
  enum_var_type type= var->type;
  if (type == OPT_GLOBAL || scope() == GLOBAL)
  {
    /*
      Yes, both locks need to be taken before an update, just as
      both are taken to get a value. If we'll take only 'guard' here,
      then value_ptr() for strings won't be safe in SHOW VARIABLES anymore,
      to make it safe we'll need value_ptr_unlock().
    */
    AutoWLock lock1(&PLock_global_system_variables);
    AutoWLock lock2(guard);
    value_origin= SQL;
    return global_update(thd, var) ||
      (on_update && on_update(this, thd, OPT_GLOBAL));
  }
  else
  {
    bool ret= session_update(thd, var) ||
      (on_update && on_update(this, thd, OPT_SESSION));

    /*
      Make sure we don't session-track variables that are not actually
      part of the session. tx_isolation and and tx_read_only for example
      exist as GLOBAL, SESSION, and one-shot ("for next transaction only").
    */
    if ((var->type == OPT_SESSION) && (!ret))
    {
      SESSION_TRACKER_CHANGED(thd, SESSION_SYSVARS_TRACKER,
                              (LEX_CSTRING*)var->var);
      /*
        Here MySQL sends variable name to avoid reporting change of
        the tracker itself, but we decided that it is not needed
      */
      SESSION_TRACKER_CHANGED(thd, SESSION_STATE_CHANGE_TRACKER, NULL);
    }

    return ret;
  }
}

uchar *sys_var::session_value_ptr(THD *thd, const LEX_STRING *base)
{
  return session_var_ptr(thd);
}

uchar *sys_var::global_value_ptr(THD *thd, const LEX_STRING *base)
{
  return global_var_ptr();
}

bool sys_var::check(THD *thd, set_var *var)
{
  if ((var->value && do_check(thd, var))
      || (on_check && on_check(this, thd, var)))
  {
    if (!thd->is_error())
    {
      char buff[STRING_BUFFER_USUAL_SIZE];
      String str(buff, sizeof(buff), system_charset_info), *res;

      if (!var->value)
      {
        str.set(STRING_WITH_LEN("DEFAULT"), &my_charset_latin1);
        res= &str;
      }
      else if (!(res=var->value->val_str(&str)))
      {
        str.set(STRING_WITH_LEN("NULL"), &my_charset_latin1);
        res= &str;
      }
      ErrConvString err(res);
      my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), name.str, err.ptr());
    }
    return true;
  }
  return false;
}

uchar *sys_var::value_ptr(THD *thd, enum_var_type type, const LEX_STRING *base)
{
  DBUG_ASSERT(base);
  if (type == OPT_GLOBAL || scope() == GLOBAL)
  {
    mysql_mutex_assert_owner(&LOCK_global_system_variables);
    AutoRLock lock(guard);
    return global_value_ptr(thd, base);
  }
  else
    return session_value_ptr(thd, base);
}

bool sys_var::set_default(THD *thd, set_var* var)
{
  if (var->type == OPT_GLOBAL || scope() == GLOBAL)
    global_save_default(thd, var);
  else
    session_save_default(thd, var);

  return check(thd, var) || update(thd, var);
}


#define do_num_val(T,CMD)                           \
do {                                                \
  T val= *(T*) value;                               \
  CMD;                                              \
} while (0)

#define case_for_integers(CMD)                      \
    case SHOW_SINT:     do_num_val (int,CMD);       \
    case SHOW_SLONG:    do_num_val (long,CMD);      \
    case SHOW_SLONGLONG:do_num_val (longlong,CMD);  \
    case SHOW_UINT:     do_num_val (uint,CMD);      \
    case SHOW_ULONG:    do_num_val (ulong,CMD);     \
    case SHOW_ULONGLONG:do_num_val (ulonglong,CMD); \
    case SHOW_HA_ROWS:  do_num_val (ha_rows,CMD);

#define case_for_double(CMD)                        \
    case SHOW_DOUBLE:   do_num_val (double,CMD)

#define case_get_string_as_lex_string               \
    case SHOW_CHAR:                                 \
      sval.str= (char*) value;                      \
      sval.length= sval.str ? strlen(sval.str) : 0; \
      break;                                        \
    case SHOW_CHAR_PTR:                             \
      sval.str= *(char**) value;                    \
      sval.length= sval.str ? strlen(sval.str) : 0; \
      break;                                        \
    case SHOW_LEX_STRING:                           \
      sval= *(LEX_STRING *) value;                  \
      break

longlong sys_var::val_int(bool *is_null,
                          THD *thd, enum_var_type type, const LEX_STRING *base)
{
  LEX_STRING sval;
  AutoWLock lock(&PLock_global_system_variables);
  const uchar *value= value_ptr(thd, type, base);
  *is_null= false;

  switch (show_type())
  {
    case_get_string_as_lex_string;
    case_for_integers(return val);
    case_for_double(return (longlong) val);
    case SHOW_MY_BOOL:  return *(my_bool*)value;
    default:            
      my_error(ER_VAR_CANT_BE_READ, MYF(0), name.str); 
      return 0;
  }

  longlong ret= 0;
  if (!(*is_null= !sval.str))
    ret= longlong_from_string_with_check(charset(thd),
                                         sval.str, sval.str + sval.length);
  return ret;
}


String *sys_var::val_str_nolock(String *str, THD *thd, const uchar *value)
{
  static LEX_STRING bools[]=
  {
    { C_STRING_WITH_LEN("OFF") },
    { C_STRING_WITH_LEN("ON") }
  };

  LEX_STRING sval;
  switch (show_type())
  {
    case_get_string_as_lex_string;
    case_for_integers(return str->set(val, system_charset_info) ? 0 : str);
    case_for_double(return str->set_real(val, 6, system_charset_info) ? 0 : str);
    case SHOW_MY_BOOL:
      sval= bools[(int)*(my_bool*)value];
      break;
    default:
      my_error(ER_VAR_CANT_BE_READ, MYF(0), name.str);
      return 0;
  }

  if (!sval.str || str->copy(sval.str, sval.length, charset(thd)))
    str= NULL;
  return str;
}


String *sys_var::val_str(String *str,
                         THD *thd, enum_var_type type, const LEX_STRING *base)
{
  AutoWLock lock(&PLock_global_system_variables);
  const uchar *value= value_ptr(thd, type, base);
  return val_str_nolock(str, thd, value);
}


double sys_var::val_real(bool *is_null,
                         THD *thd, enum_var_type type, const LEX_STRING *base)
{
  LEX_STRING sval;
  AutoWLock lock(&PLock_global_system_variables);
  const uchar *value= value_ptr(thd, type, base);
  *is_null= false;

  switch (show_type())
  {
    case_get_string_as_lex_string;
    case_for_integers(return (double)val);
    case_for_double(return val);
    case SHOW_MY_BOOL:  return *(my_bool*)value;
    default:            
      my_error(ER_VAR_CANT_BE_READ, MYF(0), name.str); 
      return 0;
  }

  double ret= 0;
  if (!(*is_null= !sval.str))
    ret= double_from_string_with_check(charset(thd),
                                       sval.str, sval.str + sval.length);
  return ret;
}


void sys_var::do_deprecated_warning(THD *thd)
{
  if (deprecation_substitute != NULL)
  {
    char buf1[NAME_CHAR_LEN + 3];
    strxnmov(buf1, sizeof(buf1)-1, "@@", name.str, 0);

    /* 
       if deprecation_substitute is an empty string,
       there is no replacement for the syntax
    */
    uint errmsg= deprecation_substitute[0] == '\0'
      ? ER_WARN_DEPRECATED_SYNTAX_NO_REPLACEMENT
      : ER_WARN_DEPRECATED_SYNTAX;
    if (thd)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_DEPRECATED_SYNTAX, ER_THD(thd, errmsg),
                          buf1, deprecation_substitute);
    else
      sql_print_warning(ER_DEFAULT(errmsg), buf1, deprecation_substitute);
  }
}

/**
  Throw warning (error in STRICT mode) if value for variable needed bounding.
  Plug-in interface also uses this.

  @param thd         thread handle
  @param name        variable's name
  @param fixed       did we have to correct the value? (throw warn/err if so)
  @param is_unsigned is value's type unsigned?
  @param v           variable's value

  @retval         true on error, false otherwise (warning or ok)
 */
bool throw_bounds_warning(THD *thd, const char *name,
                          bool fixed, bool is_unsigned, longlong v)
{
  if (fixed)
  {
    char buf[22];

    if (is_unsigned)
      ullstr((ulonglong) v, buf);
    else
      llstr(v, buf);

    if (thd->variables.sql_mode & MODE_STRICT_ALL_TABLES)
    {
      my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), name, buf);
      return true;
    }
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_TRUNCATED_WRONG_VALUE,
                        ER_THD(thd, ER_TRUNCATED_WRONG_VALUE), name, buf);
  }
  return false;
}

bool throw_bounds_warning(THD *thd, const char *name, bool fixed, double v)
{
  if (fixed)
  {
    char buf[64];

    my_gcvt(v, MY_GCVT_ARG_DOUBLE, sizeof(buf) - 1, buf, NULL);

    if (thd->variables.sql_mode & MODE_STRICT_ALL_TABLES)
    {
      my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), name, buf);
      return true;
    }
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_TRUNCATED_WRONG_VALUE,
                        ER_THD(thd, ER_TRUNCATED_WRONG_VALUE), name, buf);
  }
  return false;
}

CHARSET_INFO *sys_var::charset(THD *thd)
{
  return is_os_charset ? thd->variables.character_set_filesystem :
    system_charset_info;
}


typedef struct old_names_map_st
{
  const char *old_name;
  const char *new_name;
} my_old_conv;

static my_old_conv old_conv[]=
{
  {     "cp1251_koi8"           ,       "cp1251"        },
  {     "cp1250_latin2"         ,       "cp1250"        },
  {     "kam_latin2"            ,       "keybcs2"       },
  {     "mac_latin2"            ,       "MacRoman"      },
  {     "macce_latin2"          ,       "MacCE"         },
  {     "pc2_latin2"            ,       "pclatin2"      },
  {     "vga_latin2"            ,       "pclatin1"      },
  {     "koi8_cp1251"           ,       "koi8r"         },
  {     "win1251ukr_koi8_ukr"   ,       "win1251ukr"    },
  {     "koi8_ukr_win1251ukr"   ,       "koi8u"         },
  {     NULL                    ,       NULL            }
};

CHARSET_INFO *get_old_charset_by_name(const char *name)
{
  my_old_conv *conv;

  for (conv= old_conv; conv->old_name; conv++)
  {
    if (!my_strcasecmp(&my_charset_latin1, name, conv->old_name))
      return get_charset_by_csname(conv->new_name, MY_CS_PRIMARY, MYF(0));
  }
  return NULL;
}

/****************************************************************************
  Main handling of variables:
  - Initialisation
  - Searching during parsing
  - Update loop
****************************************************************************/

/**
  Add variables to the dynamic hash of system variables

  @param first       Pointer to first system variable to add

  @retval
    0           SUCCESS
  @retval
    otherwise   FAILURE
*/


int mysql_add_sys_var_chain(sys_var *first)
{
  sys_var *var;

  /* A write lock should be held on LOCK_system_variables_hash */

  for (var= first; var; var= var->next)
  {
    /* this fails if there is a conflicting variable name. see HASH_UNIQUE */
    if (my_hash_insert(&system_variable_hash, (uchar*) var))
    {
      fprintf(stderr, "*** duplicate variable name '%s' ?\n", var->name.str);
      goto error;
    }
  }
  return 0;

error:
  for (; first != var; first= first->next)
    my_hash_delete(&system_variable_hash, (uchar*) first);
  return 1;
}


/*
  Remove variables to the dynamic hash of system variables

  SYNOPSIS
    mysql_del_sys_var_chain()
    first       Pointer to first system variable to remove

  RETURN VALUES
    0           SUCCESS
    otherwise   FAILURE
*/

int mysql_del_sys_var_chain(sys_var *first)
{
  int result= 0;

  mysql_rwlock_wrlock(&LOCK_system_variables_hash);
  for (sys_var *var= first; var; var= var->next)
    result|= my_hash_delete(&system_variable_hash, (uchar*) var);
  mysql_rwlock_unlock(&LOCK_system_variables_hash);

  return result;
}


static int show_cmp(SHOW_VAR *a, SHOW_VAR *b)
{
  return strcmp(a->name, b->name);
}


/**
  Constructs an array of system variables for display to the user.

  @param thd       current thread
  @param sorted    If TRUE, the system variables should be sorted
  @param scope     OPT_GLOBAL or OPT_SESSION for SHOW GLOBAL|SESSION VARIABLES

  @retval
    pointer     Array of SHOW_VAR elements for display
  @retval
    NULL        FAILURE
*/

SHOW_VAR* enumerate_sys_vars(THD *thd, bool sorted, enum enum_var_type scope)
{
  int count= system_variable_hash.records, i;
  int size= sizeof(SHOW_VAR) * (count + 1);
  SHOW_VAR *result= (SHOW_VAR*) thd->alloc(size);

  if (result)
  {
    SHOW_VAR *show= result;

    for (i= 0; i < count; i++)
    {
      sys_var *var= (sys_var*) my_hash_element(&system_variable_hash, i);

      // don't show session-only variables in SHOW GLOBAL VARIABLES
      if (scope == OPT_GLOBAL && var->check_type(scope))
        continue;

      show->name= var->name.str;
      show->value= (char*) var;
      show->type= SHOW_SYS;
      show++;
    }

    /* sort into order */
    if (sorted)
      my_qsort(result, show-result, sizeof(SHOW_VAR),
               (qsort_cmp) show_cmp);

    /* make last element empty */
    bzero(show, sizeof(SHOW_VAR));
  }
  return result;
}

/**
  Find a user set-table variable.

  @param str       Name of system variable to find
  @param length    Length of variable.  zero means that we should use strlen()
                   on the variable

  @retval
    pointer     pointer to variable definitions
  @retval
    0           Unknown variable (error message is given)
*/

sys_var *intern_find_sys_var(const char *str, uint length)
{
  sys_var *var;

  /*
    This function is only called from the sql_plugin.cc.
    A lock on LOCK_system_variable_hash should be held
  */
  var= (sys_var*) my_hash_search(&system_variable_hash,
                              (uchar*) str, length ? length : strlen(str));

  return var;
}


bool find_sys_var_null_base(THD *thd, struct sys_var_with_base *tmp)
{
  tmp->var= find_sys_var(thd, tmp->base_name.str, tmp->base_name.length);

  if (tmp->var != NULL)
    tmp->base_name= null_lex_str;

  return thd->is_error();
}


/**
  Execute update of all variables.

  First run a check of all variables that all updates will go ok.
  If yes, then execute all updates, returning an error if any one failed.

  This should ensure that in all normal cases none all or variables are
  updated.

  @param THD            Thread id
  @param var_list       List of variables to update

  @retval
    0   ok
  @retval
    1   ERROR, message sent (normally no variables was updated)
  @retval
    -1  ERROR, message not sent
*/

int sql_set_variables(THD *thd, List<set_var_base> *var_list, bool free)
{
  int error= 0;
  bool was_error= thd->is_error();
  List_iterator_fast<set_var_base> it(*var_list);
  DBUG_ENTER("sql_set_variables");

  set_var_base *var;
  while ((var=it++))
  {
    if ((error= var->check(thd)))
      goto err;
  }
  if (was_error || !(error= MY_TEST(thd->is_error())))
  {
    it.rewind();
    while ((var= it++))
      error|= var->update(thd);         // Returns 0, -1 or 1
  }

err:
  if (free)
    free_underlaid_joins(thd, &thd->lex->select_lex);
  DBUG_RETURN(error);
}

/*****************************************************************************
  Functions to handle SET mysql_internal_variable=const_expr
*****************************************************************************/

/**
  Verify that the supplied value is correct.

  @param thd Thread handler

  @return status code
   @retval -1 Failure
   @retval 0 Success
 */

int set_var::check(THD *thd)
{
  var->do_deprecated_warning(thd);
  if (var->is_readonly())
  {
    my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0), var->name.str, "read only");
    return -1;
  }
  if (var->check_type(type))
  {
    int err= type == OPT_GLOBAL ? ER_LOCAL_VARIABLE : ER_GLOBAL_VARIABLE;
    my_error(err, MYF(0), var->name.str);
    return -1;
  }
  if ((type == OPT_GLOBAL && check_global_access(thd, SUPER_ACL)))
    return 1;
  /* value is a NULL pointer if we are using SET ... = DEFAULT */
  if (!value)
    return 0;

  if ((!value->fixed &&
       value->fix_fields(thd, &value)) || value->check_cols(1))
    return -1;
  if (var->check_update_type(value))
  {
    my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0), var->name.str);
    return -1;
  }
  return var->check(thd, this) ? -1 : 0;
}


/**
  Check variable, but without assigning value (used by PS).

  @param thd            thread handler

  @retval
    0   ok
  @retval
    1   ERROR, message sent (normally no variables was updated)
  @retval
    -1   ERROR, message not sent
*/
int set_var::light_check(THD *thd)
{
  if (var->check_type(type))
  {
    int err= type == OPT_GLOBAL ? ER_LOCAL_VARIABLE : ER_GLOBAL_VARIABLE;
    my_error(err, MYF(0), var->name);
    return -1;
  }
  if (type == OPT_GLOBAL && check_global_access(thd, SUPER_ACL))
    return 1;

  if (value && ((!value->fixed && value->fix_fields(thd, &value)) ||
                value->check_cols(1)))
    return -1;
  return 0;
}

/**
  Update variable

  @param   thd    thread handler
  @returns 0|1    ok or ERROR

  @note ERROR can be only due to abnormal operations involving
  the server's execution evironment such as
  out of memory, hard disk failure or the computer blows up.
  Consider set_var::check() method if there is a need to return
  an error due to logics.
*/

int set_var::update(THD *thd)
{
  return value ? var->update(thd, this) : var->set_default(thd, this);
}


set_var::set_var(THD *thd, enum_var_type type_arg, sys_var *var_arg,
                 const LEX_STRING *base_name_arg, Item *value_arg)
  :var(var_arg), type(type_arg), base(*base_name_arg)
{
  /*
    If the set value is a field, change it to a string to allow things like
    SET table_type=MYISAM;
  */
  if (value_arg && value_arg->type() == Item::FIELD_ITEM)
  {
    Item_field *item= (Item_field*) value_arg;
    // names are utf8
    if (!(value= new (thd->mem_root) Item_string_sys(thd, item->field_name)))
      value=value_arg;                        /* Give error message later */
  }
  else
    value=value_arg;
}


/*****************************************************************************
  Functions to handle SET @user_variable=const_expr
*****************************************************************************/

int set_var_user::check(THD *thd)
{
  /*
    Item_func_set_user_var can't substitute something else on its place =>
    0 can be passed as last argument (reference on item)
  */
  return (user_var_item->fix_fields(thd, (Item**) 0) ||
          user_var_item->check(0)) ? -1 : 0;
}


/**
  Check variable, but without assigning value (used by PS).

  @param thd            thread handler

  @retval
    0   ok
  @retval
    1   ERROR, message sent (normally no variables was updated)
  @retval
    -1   ERROR, message not sent
*/
int set_var_user::light_check(THD *thd)
{
  /*
    Item_func_set_user_var can't substitute something else on its place =>
    0 can be passed as last argument (reference on item)
  */
  return (user_var_item->fix_fields(thd, (Item**) 0));
}


int set_var_user::update(THD *thd)
{
  if (user_var_item->update())
  {
    /* Give an error if it's not given already */
    my_message(ER_SET_CONSTANTS_ONLY, ER_THD(thd, ER_SET_CONSTANTS_ONLY),
               MYF(0));
    return -1;
  }

  SESSION_TRACKER_CHANGED(thd, SESSION_STATE_CHANGE_TRACKER, NULL);
  return 0;
}


/*****************************************************************************
  Functions to handle SET PASSWORD
*****************************************************************************/

int set_var_password::check(THD *thd)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  return check_change_password(thd, user);
#else
  return 0;
#endif
}

int set_var_password::update(THD *thd)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Reprepare_observer *save_reprepare_observer= thd->m_reprepare_observer;
  thd->m_reprepare_observer= 0;
  int res= change_password(thd, user);
  thd->m_reprepare_observer= save_reprepare_observer;
  return res;
#else
  return 0;
#endif
}

/*****************************************************************************
  Functions to handle SET ROLE
*****************************************************************************/

int set_var_role::check(THD *thd)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int status= acl_check_setrole(thd, role.str, &access);
  return status;
#else
  return 0;
#endif
}

int set_var_role::update(THD *thd)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int res= acl_setrole(thd, role.str, access);
  if (!res)
    thd->session_tracker.mark_as_changed(thd, SESSION_STATE_CHANGE_TRACKER,
                                         NULL);
  return res;
#else
  return 0;
#endif
}

/*****************************************************************************
  Functions to handle SET DEFAULT ROLE
*****************************************************************************/

int set_var_default_role::check(THD *thd)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  real_user= get_current_user(thd, user);
  int status= acl_check_set_default_role(thd, real_user->host.str, real_user->user.str);
  return status;
#else
  return 0;
#endif
}

int set_var_default_role::update(THD *thd)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Reprepare_observer *save_reprepare_observer= thd->m_reprepare_observer;
  thd->m_reprepare_observer= 0;
  int res= acl_set_default_role(thd, real_user->host.str, real_user->user.str, role.str);
  thd->m_reprepare_observer= save_reprepare_observer;
  return res;
#else
  return 0;
#endif
}

/*****************************************************************************
  Functions to handle SET NAMES and SET CHARACTER SET
*****************************************************************************/

int set_var_collation_client::check(THD *thd)
{
  /* Currently, UCS-2 cannot be used as a client character set */
  if (!is_supported_parser_charset(character_set_client))
  {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "character_set_client",
             character_set_client->csname);
    return 1;
  }
  return 0;
}

int set_var_collation_client::update(THD *thd)
{
  thd->update_charset(character_set_client, collation_connection,
                      character_set_results);

  /* Mark client collation variables as changed */
#ifndef EMBEDDED_LIBRARY
  if (thd->session_tracker.get_tracker(SESSION_SYSVARS_TRACKER)->is_enabled())
  {
    sys_var *svar;
    mysql_mutex_lock(&LOCK_plugin);
    if ((svar= find_sys_var_ex(thd, "character_set_client",
                               sizeof("character_set_client") - 1,
                               false, true)))
      thd->session_tracker.get_tracker(SESSION_SYSVARS_TRACKER)->
        mark_as_changed(thd, (LEX_CSTRING*)svar);
    if ((svar= find_sys_var_ex(thd, "character_set_results",
                             sizeof("character_set_results") - 1,
                               false, true)))
      thd->session_tracker.get_tracker(SESSION_SYSVARS_TRACKER)->
        mark_as_changed(thd, (LEX_CSTRING*)svar);
    if ((svar= find_sys_var_ex(thd, "character_set_connection",
                                sizeof("character_set_connection") - 1,
                               false, true)))
      thd->session_tracker.get_tracker(SESSION_SYSVARS_TRACKER)->
        mark_as_changed(thd, (LEX_CSTRING*)svar);
    mysql_mutex_unlock(&LOCK_plugin);
  }
  thd->session_tracker.mark_as_changed(thd, SESSION_STATE_CHANGE_TRACKER, NULL);
#endif //EMBEDDED_LIBRARY

  thd->protocol_text.init(thd);
  thd->protocol_binary.init(thd);
  return 0;
}

/*****************************************************************************
 INFORMATION_SCHEMA.SYSTEM_VARIABLES
*****************************************************************************/
static void store_value_ptr(Field *field, sys_var *var, String *str,
                            uchar *value_ptr)
{
  field->set_notnull();
  str= var->val_str_nolock(str, field->table->in_use, value_ptr);
  if (str)
    field->store(str->ptr(), str->length(), str->charset());
}

static void store_var(Field *field, sys_var *var, enum_var_type scope,
                      String *str)
{
  if (var->check_type(scope))
    return;

  store_value_ptr(field, var, str,
                  var->value_ptr(field->table->in_use, scope, &null_lex_str));
}

int fill_sysvars(THD *thd, TABLE_LIST *tables, COND *cond)
{
  char name_buffer[NAME_CHAR_LEN];
  enum_check_fields save_count_cuted_fields= thd->count_cuted_fields;
  bool res= 1;
  CHARSET_INFO *scs= system_charset_info;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> strbuf(scs);
  const char *wild= thd->lex->wild ? thd->lex->wild->ptr() : 0;
  Field **fields=tables->table->field;

  DBUG_ASSERT(tables->table->in_use == thd);

  cond= make_cond_for_info_schema(thd, cond, tables);
  thd->count_cuted_fields= CHECK_FIELD_WARN;
  mysql_rwlock_rdlock(&LOCK_system_variables_hash);

  for (uint i= 0; i < system_variable_hash.records; i++)
  {
    sys_var *var= (sys_var*) my_hash_element(&system_variable_hash, i);

    strmake_buf(name_buffer, var->name.str);
    my_caseup_str(system_charset_info, name_buffer);

    /* this must be done before evaluating cond */
    restore_record(tables->table, s->default_values);
    fields[0]->store(name_buffer, strlen(name_buffer), scs);

    if ((wild && wild_case_compare(system_charset_info, name_buffer, wild))
        || (cond && !cond->val_int()))
      continue;

    mysql_mutex_lock(&LOCK_global_system_variables);

    // SESSION_VALUE
    store_var(fields[1], var, OPT_SESSION, &strbuf);

    // GLOBAL_VALUE
    store_var(fields[2], var, OPT_GLOBAL, &strbuf);

    // GLOBAL_VALUE_ORIGIN
    static const LEX_CSTRING origins[]=
    {
      { STRING_WITH_LEN("CONFIG") },
      { STRING_WITH_LEN("AUTO") },
      { STRING_WITH_LEN("SQL") },
      { STRING_WITH_LEN("COMPILE-TIME") },
      { STRING_WITH_LEN("ENVIRONMENT") }
    };
    const LEX_CSTRING *origin= origins + var->value_origin;
    fields[3]->store(origin->str, origin->length, scs);

    // DEFAULT_VALUE
    uchar *def= var->is_readonly() && var->option.id < 0
                ? 0 : var->default_value_ptr(thd);
    if (def)
      store_value_ptr(fields[4], var, &strbuf, def);

    mysql_mutex_unlock(&LOCK_global_system_variables);

    // VARIABLE_SCOPE
    static const LEX_CSTRING scopes[]=
    {
      { STRING_WITH_LEN("GLOBAL") },
      { STRING_WITH_LEN("SESSION") },
      { STRING_WITH_LEN("SESSION ONLY") }
    };
    const LEX_CSTRING *scope= scopes + var->scope();
    fields[5]->store(scope->str, scope->length, scs);

    // VARIABLE_TYPE
#if SIZEOF_LONG == SIZEOF_INT
#define LONG_TYPE "INT"
#else
#define LONG_TYPE "BIGINT"
#endif

    static const LEX_CSTRING types[]=
    {
      { 0, 0 },                                      // unused         0
      { 0, 0 },                                      // GET_NO_ARG     1
      { STRING_WITH_LEN("BOOLEAN") },                // GET_BOOL       2
      { STRING_WITH_LEN("INT") },                    // GET_INT        3
      { STRING_WITH_LEN("INT UNSIGNED") },           // GET_UINT       4
      { STRING_WITH_LEN(LONG_TYPE) },                // GET_LONG       5
      { STRING_WITH_LEN(LONG_TYPE " UNSIGNED") },    // GET_ULONG      6
      { STRING_WITH_LEN("BIGINT") },                 // GET_LL         7
      { STRING_WITH_LEN("BIGINT UNSIGNED") },        // GET_ULL        8
      { STRING_WITH_LEN("VARCHAR") },                // GET_STR        9
      { STRING_WITH_LEN("VARCHAR") },                // GET_STR_ALLOC 10
      { 0, 0 },                                      // GET_DISABLED  11
      { STRING_WITH_LEN("ENUM") },                   // GET_ENUM      12
      { STRING_WITH_LEN("SET") },                    // GET_SET       13
      { STRING_WITH_LEN("DOUBLE") },                 // GET_DOUBLE    14
      { STRING_WITH_LEN("FLAGSET") },                // GET_FLAGSET   15
    };
    const ulong vartype= (var->option.var_type & GET_TYPE_MASK);
    const LEX_CSTRING *type= types + vartype;
    fields[6]->store(type->str, type->length, scs);

    // VARIABLE_COMMENT
    fields[7]->store(var->option.comment, strlen(var->option.comment),
                           scs);

    // NUMERIC_MIN_VALUE
    // NUMERIC_MAX_VALUE
    // NUMERIC_BLOCK_SIZE
    bool is_unsigned= true;
    switch (vartype)
    {
    case GET_INT:
    case GET_LONG:
    case GET_LL:
      is_unsigned= false;
      /* fall through */
    case GET_UINT:
    case GET_ULONG:
    case GET_ULL:
      fields[8]->set_notnull();
      fields[9]->set_notnull();
      fields[10]->set_notnull();
      fields[8]->store(var->option.min_value, is_unsigned);
      fields[9]->store(var->option.max_value, is_unsigned);
      fields[10]->store(var->option.block_size, is_unsigned);
      break;
    case GET_DOUBLE:
      fields[8]->set_notnull();
      fields[9]->set_notnull();
      fields[8]->store(getopt_ulonglong2double(var->option.min_value));
      fields[9]->store(getopt_ulonglong2double(var->option.max_value));
    }

    // ENUM_VALUE_LIST
    TYPELIB *tl= var->option.typelib;
    if (tl)
    {
      uint i;
      strbuf.length(0);
      for (i=0; i + 1 < tl->count; i++)
      {
        strbuf.append(tl->type_names[i]);
        strbuf.append(',');
      }
      strbuf.append(tl->type_names[i]);
      fields[11]->set_notnull();
      fields[11]->store(strbuf.ptr(), strbuf.length(), scs);
    }

    // READ_ONLY
    static const LEX_CSTRING yesno[]=
    {
      { STRING_WITH_LEN("NO") },
      { STRING_WITH_LEN("YES") }
    };
    const LEX_CSTRING *yn = yesno + var->is_readonly();
    fields[12]->store(yn->str, yn->length, scs);

    // COMMAND_LINE_ARGUMENT
    if (var->option.id >= 0)
    {
      static const LEX_CSTRING args[]=
      {
        { STRING_WITH_LEN("NONE") },          // NO_ARG
        { STRING_WITH_LEN("OPTIONAL") },      // OPT_ARG
        { STRING_WITH_LEN("REQUIRED") }       // REQUIRED_ARG
      };
      const LEX_CSTRING *arg= args + var->option.arg_type;
      fields[13]->set_notnull();
      fields[13]->store(arg->str, arg->length, scs);
    }

    if (schema_table_store_record(thd, tables->table))
      goto end;
    thd->get_stmt_da()->inc_current_row_for_warning();
  }
  res= 0;
end:
  mysql_rwlock_unlock(&LOCK_system_variables_hash);
  thd->count_cuted_fields= save_count_cuted_fields;
  return res;
}

/*
  This is a simple and inefficient helper that sets sys_var::value_origin
  for a specific sysvar.
  It should *only* be used on server startup, if you need to do this later,
  get yourself a pointer to your sysvar (see e.g. Sys_autocommit_ptr)
  and update it directly.
*/

void set_sys_var_value_origin(void *ptr, enum sys_var::where here)
{
  bool found __attribute__((unused))= false;
  DBUG_ASSERT(!mysqld_server_started); // only to be used during startup

  for (uint i= 0; i < system_variable_hash.records; i++)
  {
    sys_var *var= (sys_var*) my_hash_element(&system_variable_hash, i);
    if (var->option.value == ptr)
    {
      found= true;
      var->value_origin= here;
      /* don't break early, search for all matches */
    }
  }

  DBUG_ASSERT(found); // variable must have been found
}

enum sys_var::where get_sys_var_value_origin(void *ptr)
{
  DBUG_ASSERT(!mysqld_server_started); // only to be used during startup

  for (uint i= 0; i < system_variable_hash.records; i++)
  {
    sys_var *var= (sys_var*) my_hash_element(&system_variable_hash, i);
    if (var->option.value == ptr)
    {
      return var->value_origin; //first match
    }
  }

  DBUG_ASSERT(0); // variable must have been found
  return sys_var::CONFIG;
}

