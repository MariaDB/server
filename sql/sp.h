/* -*- C++ -*- */
/* Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2009, 2020, MariaDB Corporation.

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

#ifndef _SP_H_
#define _SP_H_

#include "my_global.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_string.h"                         // LEX_STRING
#include "sql_cmd.h"
#include "mdl.h"

class Field;
class Open_tables_backup;
class Open_tables_state;
class Query_arena;
class Query_tables_list;
class Sroutine_hash_entry;
class THD;
class sp_cache;
class sp_head;
class sp_package;
class sp_pcontext;
class sp_name;
class sp_expr_lex;
class Database_qualified_name;
struct st_sp_chistics;
class Stored_program_creation_ctx;
struct LEX;
struct TABLE;
struct TABLE_LIST;
typedef struct st_hash HASH;
template <typename T> class SQL_I_List;

/*
  Values for the type enum. This reflects the order of the enum declaration
  in the CREATE TABLE command.
  See also storage/perfschema/my_thread.h
*/
enum enum_sp_type
{
  SP_TYPE_FUNCTION=1,
  SP_TYPE_PROCEDURE=2,
  SP_TYPE_PACKAGE=3,
  SP_TYPE_PACKAGE_BODY=4,
  SP_TYPE_TRIGGER=5,
  SP_TYPE_EVENT=6,
};

class Sp_handler
{
  bool sp_resolve_package_routine_explicit(THD *thd,
                                           sp_head *caller,
                                           sp_name *name,
                                           const Sp_handler **pkg_routine_hndlr,
                                           Database_qualified_name *pkgname)
                                           const;
  bool sp_resolve_package_routine_implicit(THD *thd,
                                           sp_head *caller,
                                           sp_name *name,
                                           const Sp_handler **pkg_routine_hndlr,
                                           Database_qualified_name *pkgname)
                                           const;
protected:
  int db_find_routine_aux(THD *thd, const Database_qualified_name *name,
                          TABLE *table) const;
  int db_find_routine(THD *thd, const Database_qualified_name *name,
                      sp_head **sphp) const;
  int db_find_and_cache_routine(THD *thd,
                                const Database_qualified_name *name,
                                sp_head **sp) const;
  int db_load_routine(THD *thd, const Database_qualified_name *name,
                      sp_head **sphp,
                      sql_mode_t sql_mode,
                      const LEX_CSTRING &params,
                      const LEX_CSTRING &returns,
                      const LEX_CSTRING &body,
                      const st_sp_chistics &chistics,
                      const AUTHID &definer,
                      longlong created, longlong modified,
                      sp_package *parent,
                      Stored_program_creation_ctx *creation_ctx) const;
  int sp_drop_routine_internal(THD *thd,
                               const Database_qualified_name *name,
                               TABLE *table) const;

  sp_head *sp_clone_and_link_routine(THD *thd,
                                     const Database_qualified_name *name,
                                     sp_head *sp) const;
  int sp_cache_package_routine(THD *thd,
                               const LEX_CSTRING &pkgname_cstr,
                               const Database_qualified_name *name,
                               bool lookup_only, sp_head **sp) const;
  int sp_cache_package_routine(THD *thd,
                               const Database_qualified_name *name,
                               bool lookup_only, sp_head **sp) const;
  sp_head *sp_find_package_routine(THD *thd,
                                   const LEX_CSTRING pkgname_str,
                                   const Database_qualified_name *name,
                                   bool cache_only) const;
  sp_head *sp_find_package_routine(THD *thd,
                                   const Database_qualified_name *name,
                                   bool cache_only) const;
public: // TODO: make it private or protected
  virtual int sp_find_and_drop_routine(THD *thd, TABLE *table,
                                       const Database_qualified_name *name)
                                       const;

public:
  virtual ~Sp_handler() = default;
  static const Sp_handler *handler(enum enum_sql_command cmd);
  static const Sp_handler *handler(enum_sp_type type);
  static const Sp_handler *handler(MDL_key::enum_mdl_namespace ns);
  /*
    Return a handler only those SP objects that store
    definitions in the mysql.proc system table
  */
  static const Sp_handler *handler_mysql_proc(enum_sp_type type)
  {
    const Sp_handler *sph= handler(type);
    return sph ? sph->sp_handler_mysql_proc() : NULL;
  }

  static bool eq_routine_name(const LEX_CSTRING &name1,
                              const LEX_CSTRING &name2)
  {
    return system_charset_info->strnncoll(name1.str, name1.length,
                                          name2.str, name2.length) == 0;
  }
  const char *type_str() const { return type_lex_cstring().str; }
  virtual const char *show_create_routine_col1_caption() const
  {
    DBUG_ASSERT(0);
    return "";
  }
  virtual const char *show_create_routine_col3_caption() const
  {
    DBUG_ASSERT(0);
    return "";
  }
  virtual const Sp_handler *package_routine_handler() const
  {
    return this;
  }
  virtual enum_sp_type type() const= 0;
  virtual LEX_CSTRING type_lex_cstring() const= 0;
  virtual LEX_CSTRING empty_body_lex_cstring(sql_mode_t mode) const
  {
    static LEX_CSTRING m_empty_body= {STRING_WITH_LEN("???")};
    DBUG_ASSERT(0);
    return m_empty_body;
  }
  virtual MDL_key::enum_mdl_namespace get_mdl_type() const= 0;
  virtual const Sp_handler *sp_handler_mysql_proc() const { return this; }
  virtual sp_cache **get_cache(THD *) const { return NULL; }
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  virtual HASH *get_priv_hash() const { return NULL; }
#endif
  virtual ulong recursion_depth(THD *thd) const { return 0; }
  /**
    Return appropriate error about recursion limit reaching

    @param thd  Thread handle
    @param sp   SP routine

    @remark For functions and triggers we return error about
            prohibited recursion. For stored procedures we
            return about reaching recursion limit.
  */
  virtual void recursion_level_error(THD *thd, const sp_head *sp) const
  {
    my_error(ER_SP_NO_RECURSION, MYF(0));
  }
  virtual bool add_instr_freturn(THD *thd, sp_head *sp,
                                 sp_pcontext *spcont,
                                 Item *item, sp_expr_lex *lex) const;
  virtual bool add_instr_preturn(THD *thd, sp_head *sp,
                                 sp_pcontext *spcont) const;

  void add_used_routine(Query_tables_list *prelocking_ctx,
                        Query_arena *arena,
                        const Database_qualified_name *name) const;

  bool sp_resolve_package_routine(THD *thd,
                                  sp_head *caller,
                                  sp_name *name,
                                  const Sp_handler **pkg_routine_handler,
                                  Database_qualified_name *pkgname) const;
  virtual sp_head *sp_find_routine(THD *thd,
                                   const Database_qualified_name *name,
                                   bool cache_only) const;
  virtual int sp_cache_routine(THD *thd, const Database_qualified_name *name,
                               bool lookup_only, sp_head **sp) const;

  int sp_cache_routine_reentrant(THD *thd,
                                 const Database_qualified_name *nm,
                                 sp_head **sp) const;

  bool sp_exist_routines(THD *thd, TABLE_LIST *procs) const;
  bool sp_show_create_routine(THD *thd,
                              const Database_qualified_name *name) const;

  bool sp_create_routine(THD *thd, const sp_head *sp) const;

  int sp_update_routine(THD *thd, const Database_qualified_name *name,
                        const st_sp_chistics *chistics) const;

  int sp_drop_routine(THD *thd, const Database_qualified_name *name) const;

  sp_head *sp_load_for_information_schema(THD *thd, TABLE *proc_table,
                                          const LEX_CSTRING &db,
                                          const LEX_CSTRING &name,
                                          const LEX_CSTRING &params,
                                          const LEX_CSTRING &returns,
                                          sql_mode_t sql_mode,
                                          bool *free_sp_head) const;

  /*
    Make a SHOW CREATE statement.
      @retval   true on error
      @retval   false on success
  */
  virtual bool show_create_sp(THD *thd, String *buf,
                              const LEX_CSTRING &db,
                              const LEX_CSTRING &name,
                              const LEX_CSTRING &params,
                              const LEX_CSTRING &returns,
                              const LEX_CSTRING &body,
                              const st_sp_chistics &chistics,
                              const AUTHID &definer,
                              const DDL_options_st ddl_options,
                              sql_mode_t sql_mode) const;

};


class Sp_handler_procedure: public Sp_handler
{
public:
  enum_sp_type type() const { return SP_TYPE_PROCEDURE; }
  LEX_CSTRING type_lex_cstring() const
  {
    static LEX_CSTRING m_type_str= { STRING_WITH_LEN("PROCEDURE")};
    return m_type_str;
  }
  LEX_CSTRING empty_body_lex_cstring(sql_mode_t mode) const;
  const char *show_create_routine_col1_caption() const
  {
    return "Procedure";
  }
  const char *show_create_routine_col3_caption() const
  {
    return "Create Procedure";
  }
  MDL_key::enum_mdl_namespace get_mdl_type() const
  {
    return MDL_key::PROCEDURE;
  }
  const Sp_handler *package_routine_handler() const;
  sp_cache **get_cache(THD *) const;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  HASH *get_priv_hash() const;
#endif
  ulong recursion_depth(THD *thd) const;
  void recursion_level_error(THD *thd, const sp_head *sp) const;
  bool add_instr_preturn(THD *thd, sp_head *sp, sp_pcontext *spcont) const;
};


class Sp_handler_package_procedure: public Sp_handler_procedure
{
public:
  int sp_cache_routine(THD *thd, const Database_qualified_name *name,
                       bool lookup_only, sp_head **sp) const
  {
    return sp_cache_package_routine(thd, name, lookup_only, sp);
  }
  sp_head *sp_find_routine(THD *thd,
                           const Database_qualified_name *name,
                           bool cache_only) const
  {
    return sp_find_package_routine(thd, name, cache_only);
  }
};


class Sp_handler_function: public Sp_handler
{
public:
  enum_sp_type type() const { return SP_TYPE_FUNCTION; }
  LEX_CSTRING type_lex_cstring() const
  {
    static LEX_CSTRING m_type_str= { STRING_WITH_LEN("FUNCTION")};
    return m_type_str;
  }
  LEX_CSTRING empty_body_lex_cstring(sql_mode_t mode) const;
  const char *show_create_routine_col1_caption() const
  {
    return "Function";
  }
  const char *show_create_routine_col3_caption() const
  {
    return "Create Function";
  }
  MDL_key::enum_mdl_namespace get_mdl_type() const
  {
    return MDL_key::FUNCTION;
  }
  const Sp_handler *package_routine_handler() const;
  sp_cache **get_cache(THD *) const;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  HASH *get_priv_hash() const;
#endif
  bool add_instr_freturn(THD *thd, sp_head *sp, sp_pcontext *spcont,
                         Item *item, sp_expr_lex *lex) const;
};


class Sp_handler_package_function: public Sp_handler_function
{
public:
  int sp_cache_routine(THD *thd, const Database_qualified_name *name,
                       bool lookup_only, sp_head **sp) const
  {
    return sp_cache_package_routine(thd, name, lookup_only, sp);
  }
  sp_head *sp_find_routine(THD *thd,
                           const Database_qualified_name *name,
                           bool cache_only) const
  {
    return sp_find_package_routine(thd, name, cache_only);
  }
};


class Sp_handler_package: public Sp_handler
{
public:
  bool show_create_sp(THD *thd, String *buf,
                      const LEX_CSTRING &db,
                      const LEX_CSTRING &name,
                      const LEX_CSTRING &params,
                      const LEX_CSTRING &returns,
                      const LEX_CSTRING &body,
                      const st_sp_chistics &chistics,
                      const AUTHID &definer,
                      const DDL_options_st ddl_options,
                      sql_mode_t sql_mode) const;
};


class Sp_handler_package_spec: public Sp_handler_package
{
public: // TODO: make it private or protected
  int sp_find_and_drop_routine(THD *thd, TABLE *table,
                               const Database_qualified_name *name)
                               const;
public:
  enum_sp_type type() const { return SP_TYPE_PACKAGE; }
  LEX_CSTRING type_lex_cstring() const
  {
    static LEX_CSTRING m_type_str= {STRING_WITH_LEN("PACKAGE")};
    return m_type_str;
  }
  LEX_CSTRING empty_body_lex_cstring(sql_mode_t mode) const
  {
    static LEX_CSTRING m_empty_body= {STRING_WITH_LEN("BEGIN END")};
    return m_empty_body;
  }
  const char *show_create_routine_col1_caption() const
  {
    return "Package";
  }
  const char *show_create_routine_col3_caption() const
  {
    return "Create Package";
  }
  MDL_key::enum_mdl_namespace get_mdl_type() const
  {
    return MDL_key::PACKAGE_BODY;
  }
  sp_cache **get_cache(THD *) const;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  HASH *get_priv_hash() const;
#endif
};


class Sp_handler_package_body: public Sp_handler_package
{
public:
  enum_sp_type type() const { return SP_TYPE_PACKAGE_BODY; }
  LEX_CSTRING type_lex_cstring() const
  {
    static LEX_CSTRING m_type_str= {STRING_WITH_LEN("PACKAGE BODY")};
    return m_type_str;
  }
  LEX_CSTRING empty_body_lex_cstring(sql_mode_t mode) const
  {
    static LEX_CSTRING m_empty_body= {STRING_WITH_LEN("BEGIN END")};
    return m_empty_body;
  }
  const char *show_create_routine_col1_caption() const
  {
    return "Package body";
  }
  const char *show_create_routine_col3_caption() const
  {
    return "Create Package Body";
  }
  MDL_key::enum_mdl_namespace get_mdl_type() const
  {
    return MDL_key::PACKAGE_BODY;
  }
  sp_cache **get_cache(THD *) const;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  HASH *get_priv_hash() const;
#endif
};


class Sp_handler_trigger: public Sp_handler
{
public:
  enum_sp_type type() const { return SP_TYPE_TRIGGER; }
  LEX_CSTRING type_lex_cstring() const
  {
    static LEX_CSTRING m_type_str= { STRING_WITH_LEN("TRIGGER")};
    return m_type_str;
  }
  MDL_key::enum_mdl_namespace get_mdl_type() const
  {
    DBUG_ASSERT(0);
    return MDL_key::TRIGGER;
  }
  const Sp_handler *sp_handler_mysql_proc() const { return NULL; }
};


extern MYSQL_PLUGIN_IMPORT Sp_handler_function sp_handler_function;
extern MYSQL_PLUGIN_IMPORT Sp_handler_procedure sp_handler_procedure;
extern MYSQL_PLUGIN_IMPORT Sp_handler_package_spec sp_handler_package_spec;
extern MYSQL_PLUGIN_IMPORT Sp_handler_package_body sp_handler_package_body;
extern MYSQL_PLUGIN_IMPORT Sp_handler_package_function sp_handler_package_function;
extern MYSQL_PLUGIN_IMPORT Sp_handler_package_procedure sp_handler_package_procedure;
extern MYSQL_PLUGIN_IMPORT Sp_handler_trigger sp_handler_trigger;


inline const Sp_handler *Sp_handler::handler(enum_sql_command cmd)
{
  switch (cmd) {
  case SQLCOM_CREATE_PROCEDURE:
  case SQLCOM_ALTER_PROCEDURE:
  case SQLCOM_DROP_PROCEDURE:
  case SQLCOM_SHOW_PROC_CODE:
  case SQLCOM_SHOW_CREATE_PROC:
  case SQLCOM_SHOW_STATUS_PROC:
    return &sp_handler_procedure;
  case SQLCOM_CREATE_SPFUNCTION:
  case SQLCOM_ALTER_FUNCTION:
  case SQLCOM_DROP_FUNCTION:
  case SQLCOM_SHOW_FUNC_CODE:
  case SQLCOM_SHOW_CREATE_FUNC:
  case SQLCOM_SHOW_STATUS_FUNC:
    return &sp_handler_function;
  case SQLCOM_CREATE_PACKAGE:
  case SQLCOM_DROP_PACKAGE:
  case SQLCOM_SHOW_CREATE_PACKAGE:
  case SQLCOM_SHOW_STATUS_PACKAGE:
    return &sp_handler_package_spec;
  case SQLCOM_CREATE_PACKAGE_BODY:
  case SQLCOM_DROP_PACKAGE_BODY:
  case SQLCOM_SHOW_CREATE_PACKAGE_BODY:
  case SQLCOM_SHOW_STATUS_PACKAGE_BODY:
  case SQLCOM_SHOW_PACKAGE_BODY_CODE:
    return &sp_handler_package_body;
  default:
    break;
  }
  return NULL;
}


inline const Sp_handler *Sp_handler::handler(enum_sp_type type)
{
  switch (type) {
  case SP_TYPE_PROCEDURE:
    return &sp_handler_procedure;
  case SP_TYPE_FUNCTION:
    return &sp_handler_function;
  case SP_TYPE_PACKAGE:
    return &sp_handler_package_spec;
  case SP_TYPE_PACKAGE_BODY:
    return &sp_handler_package_body;
  case SP_TYPE_TRIGGER:
    return &sp_handler_trigger;
  case SP_TYPE_EVENT:
    break;
  }
  return NULL;
}


inline const Sp_handler *Sp_handler::handler(MDL_key::enum_mdl_namespace type)
{
  switch (type) {
  case MDL_key::FUNCTION:
    return &sp_handler_function;
  case MDL_key::PROCEDURE:
    return &sp_handler_procedure;
  case MDL_key::PACKAGE_BODY:
    return &sp_handler_package_body;
  case MDL_key::BACKUP:
  case MDL_key::SCHEMA:
  case MDL_key::TABLE:
  case MDL_key::TRIGGER:
  case MDL_key::EVENT:
  case MDL_key::USER_LOCK:
  case MDL_key::NAMESPACE_END:
    break;
  }
  return NULL;
}


/* Tells what SP_DEFAULT_ACCESS should be mapped to */
#define SP_DEFAULT_ACCESS_MAPPING SP_CONTAINS_SQL

// Return codes from sp_create_*, sp_drop_*, and sp_show_*:
#define SP_OK                 0
#define SP_KEY_NOT_FOUND     -1
#define SP_OPEN_TABLE_FAILED -2
#define SP_WRITE_ROW_FAILED  -3
#define SP_DELETE_ROW_FAILED -4
#define SP_GET_FIELD_FAILED  -5
#define SP_PARSE_ERROR       -6
#define SP_INTERNAL_ERROR    -7
#define SP_NO_DB_ERROR       -8
#define SP_BAD_IDENTIFIER    -9
#define SP_BODY_TOO_LONG    -10
#define SP_FLD_STORE_FAILED -11

/* DB storage of Stored PROCEDUREs and FUNCTIONs */
enum
{
  MYSQL_PROC_FIELD_DB = 0,
  MYSQL_PROC_FIELD_NAME,
  MYSQL_PROC_MYSQL_TYPE,
  MYSQL_PROC_FIELD_SPECIFIC_NAME,
  MYSQL_PROC_FIELD_LANGUAGE,
  MYSQL_PROC_FIELD_ACCESS,
  MYSQL_PROC_FIELD_DETERMINISTIC,
  MYSQL_PROC_FIELD_SECURITY_TYPE,
  MYSQL_PROC_FIELD_PARAM_LIST,
  MYSQL_PROC_FIELD_RETURNS,
  MYSQL_PROC_FIELD_BODY,
  MYSQL_PROC_FIELD_DEFINER,
  MYSQL_PROC_FIELD_CREATED,
  MYSQL_PROC_FIELD_MODIFIED,
  MYSQL_PROC_FIELD_SQL_MODE,
  MYSQL_PROC_FIELD_COMMENT,
  MYSQL_PROC_FIELD_CHARACTER_SET_CLIENT,
  MYSQL_PROC_FIELD_COLLATION_CONNECTION,
  MYSQL_PROC_FIELD_DB_COLLATION,
  MYSQL_PROC_FIELD_BODY_UTF8,
  MYSQL_PROC_FIELD_AGGREGATE,
  MYSQL_PROC_FIELD_COUNT
};

/* Drop all routines in database 'db' */
int
sp_drop_db_routines(THD *thd, const char *db);

/**
   Acquires exclusive metadata lock on all stored routines in the
   given database.

   @param  thd  Thread handler
   @param  db   Database name

   @retval  false  Success
   @retval  true   Failure
 */
bool lock_db_routines(THD *thd, const char *db);

/**
  Structure that represents element in the set of stored routines
  used by statement or routine.
*/

class Sroutine_hash_entry
{
public:
  /**
    Metadata lock request for routine.
    MDL_key in this request is also used as a key for set.
  */
  MDL_request mdl_request;
  /**
    Next element in list linking all routines in set. See also comments
    for LEX::sroutine/sroutine_list and sp_head::m_sroutines.
  */
  Sroutine_hash_entry *next;
  /**
    Uppermost view which directly or indirectly uses this routine.
    0 if routine is not used in view. Note that it also can be 0 if
    statement uses routine both via view and directly.
  */
  TABLE_LIST *belong_to_view;
  /**
    This is for prepared statement validation purposes.
    A statement looks up and pre-loads all its stored functions
    at prepare. Later on, if a function is gone from the cache,
    execute may fail.
    Remember the version of sp_head at prepare to be able to
    invalidate the prepared statement at execute if it
    changes.
  */
  ulong m_sp_cache_version;

  const Sp_handler *m_handler;

  int sp_cache_routine(THD *thd, bool lookup_only, sp_head **sp) const;
};


bool sp_add_used_routine(Query_tables_list *prelocking_ctx, Query_arena *arena,
                         const MDL_key *key,
                         const Sp_handler *handler,
                         TABLE_LIST *belong_to_view);
void sp_remove_not_own_routines(Query_tables_list *prelocking_ctx);
bool sp_update_sp_used_routines(HASH *dst, HASH *src);
void sp_update_stmt_used_routines(THD *thd, Query_tables_list *prelocking_ctx,
                                  HASH *src, TABLE_LIST *belong_to_view);
void sp_update_stmt_used_routines(THD *thd, Query_tables_list *prelocking_ctx,
                                  SQL_I_List<Sroutine_hash_entry> *src,
                                  TABLE_LIST *belong_to_view);

extern "C" uchar* sp_sroutine_key(const uchar *ptr, size_t *plen,
                                  my_bool first);

/*
  Routines which allow open/lock and close mysql.proc table even when
  we already have some tables open and locked.
*/
TABLE *open_proc_table_for_read(THD *thd);

bool load_charset(THD *thd,
                  MEM_ROOT *mem_root,
                  Field *field,
                  CHARSET_INFO *dflt_cs,
                  CHARSET_INFO **cs);

bool load_collation(THD *thd,MEM_ROOT *mem_root,
                    Field *field,
                    CHARSET_INFO *dflt_cl,
                    CHARSET_INFO **cl);

void sp_returns_type(THD *thd,
                     String &result,
                     const sp_head *sp);

#endif /* _SP_H_ */
