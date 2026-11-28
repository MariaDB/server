/* Copyright (c) 2026, Shubham Chandrawanshi

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/**
  @file storage/perfschema/pfs_native_func.cc
  Function plugins PS_THREAD_ID() and PS_CURRENT_THREAD_ID()
  (MySQL 8.0.16 compatible).

  The functions are declared by the performance schema plugin library,
  so they read PFS_thread directly instead of going through the PSI
  interface, and they do not exist in a server built without the
  performance schema.
*/

#include "mariadb.h"
#include "sql_class.h"
#include <mysql/plugin_function.h>
#include "pfs_native_func.h"
#include "pfs_global.h"
#include "pfs_server.h"
#include "pfs_instr.h"
#include "pfs_buffer_container.h"


/**
  Raise an error unless the performance schema is running,
  like MySQL does for PS_THREAD_ID() and PS_CURRENT_THREAD_ID().
*/
static bool pfs_func_check_enabled(const char *func_name)
{
  if (pfs_param.m_enabled && pfs_initialized)
    return false;
  my_printf_error(ER_WRONG_PERFSCHEMA_USAGE,
                  "'%s': The Performance Schema is not enabled.", MYF(0),
                  func_name);
  return true;
}


/**
  performance_schema.threads.THREAD_ID of the thread with the given
  processlist id, or 0 if there is no such instrumented thread.
*/
static ulonglong pfs_thread_id_by_processlist_id(ulonglong processlist_id)
{
  PFS_thread_iterator it= global_thread_container.iterate();
  for (PFS_thread *pfs= it.scan_next(); pfs != NULL; pfs= it.scan_next())
  {
    pfs_optimistic_state lock;
    /* Protect this reader against thread termination */
    pfs->m_lock.begin_optimistic_lock(&lock);
    ulonglong pfs_processlist_id= pfs->m_processlist_id;
    ulonglong thread_id= pfs->m_thread_internal_id;
    if (pfs_processlist_id == processlist_id &&
        pfs->m_lock.end_optimistic_lock(&lock))
      return thread_id;
  }
  return 0;
}


/**
  PS_CURRENT_THREAD_ID(): THREAD_ID of the current connection.
*/
class Item_func_ps_current_thread_id :public Item_longlong_func
{
public:
  Item_func_ps_current_thread_id(THD *thd): Item_longlong_func(thd) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ps_current_thread_id") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= 21;
    unsigned_flag= 1;
    set_maybe_null();
    return false;
  }
  bool fix_fields(THD *thd, Item **ref) override
  {
    if (Item_longlong_func::fix_fields(thd, ref))
      return true;
    thd->used|= THD::THREAD_SPECIFIC_USED;
    return false;
  }
  longlong val_int() override
  {
    DBUG_ASSERT(fixed());
    null_value= true;
    if (pfs_func_check_enabled(func_name()))
      return 0;
    PFS_thread *pfs= PFS_thread::get_current_thread();
    if (pfs == NULL || pfs->m_thread_internal_id == 0)
      return 0;
    null_value= false;
    return (longlong) pfs->m_thread_internal_id;
  }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_SESSION_FUNC);
  }
protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_ps_current_thread_id>(thd, this); }
};


/**
  PS_THREAD_ID(connection_id): THREAD_ID of the connection, or NULL.
*/
class Item_func_ps_thread_id :public Item_longlong_func
{
public:
  Item_func_ps_thread_id(THD *thd, Item *a): Item_longlong_func(thd, a) {}
  LEX_CSTRING func_name_cstring() const override
  {
    static LEX_CSTRING name= {STRING_WITH_LEN("ps_thread_id") };
    return name;
  }
  bool fix_length_and_dec(THD *thd) override
  {
    max_length= 21;
    unsigned_flag= 1;
    set_maybe_null();
    return false;
  }
  longlong val_int() override
  {
    DBUG_ASSERT(fixed());
    null_value= true;
    if (pfs_func_check_enabled(func_name()))
      return 0;
    longlong processlist_id= args[0]->val_int();
    /* Like MySQL: NULL, non-integer, zero and negative arguments give NULL */
    if (args[0]->null_value || args[0]->cmp_type() != INT_RESULT ||
        processlist_id == 0 ||
        (!args[0]->unsigned_flag && processlist_id < 0))
      return 0;
    ulonglong thread_id=
      pfs_thread_id_by_processlist_id((ulonglong) processlist_id);
    if (thread_id == 0)
      return 0;
    null_value= false;
    return (longlong) thread_id;
  }
  /* The thread behind a connection id can end at any time */
  bool const_item() const override { return false; }
  table_map used_tables() const override
  { return Item_longlong_func::used_tables() | RAND_TABLE_BIT; }
  bool check_vcol_func_processor(void *arg) override
  {
    return mark_unsupported_function(func_name(), "()", arg,
                                     VCOL_NON_DETERMINISTIC);
  }
protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_ps_thread_id>(thd, this); }
};


class Create_func_ps_current_thread_id : public Create_func_arg0
{
public:
  Item *create_builder(THD *thd) override
  {
    thd->lex->safe_to_cache_query= 0;
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return new (thd->mem_root) Item_func_ps_current_thread_id(thd);
  }
  static Create_func_ps_current_thread_id s_singleton;
protected:
  Create_func_ps_current_thread_id() = default;
};


class Create_func_ps_thread_id : public Create_func_arg1
{
public:
  Item *create_1_arg(THD *thd, Item *arg1) override
  {
    thd->lex->safe_to_cache_query= 0;
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
    return new (thd->mem_root) Item_func_ps_thread_id(thd, arg1);
  }
  static Create_func_ps_thread_id s_singleton;
protected:
  Create_func_ps_thread_id() = default;
};


Create_func_ps_current_thread_id Create_func_ps_current_thread_id::s_singleton;
Create_func_ps_thread_id Create_func_ps_thread_id::s_singleton;

Plugin_function pfs_function_ps_current_thread_id(
  &Create_func_ps_current_thread_id::s_singleton);
Plugin_function
  pfs_function_ps_thread_id(&Create_func_ps_thread_id::s_singleton);
