#ifndef SQL_ACL_INCLUDED
#define SQL_ACL_INCLUDED

/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2017, 2020, MariaDB Corporation.

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

#include "violite.h"                            /* SSL_type */
#include "sql_class.h"                          /* LEX_COLUMN */
#include "grant.h"
#include "sql_cmd.h"                            /* Sql_cmd */


enum mysql_db_table_field
{
  MYSQL_DB_FIELD_HOST = 0,
  MYSQL_DB_FIELD_DB,
  MYSQL_DB_FIELD_USER,
  MYSQL_DB_FIELD_SELECT_PRIV,
  MYSQL_DB_FIELD_INSERT_PRIV,
  MYSQL_DB_FIELD_UPDATE_PRIV,
  MYSQL_DB_FIELD_DELETE_PRIV,
  MYSQL_DB_FIELD_CREATE_PRIV,
  MYSQL_DB_FIELD_DROP_PRIV,
  MYSQL_DB_FIELD_GRANT_PRIV,
  MYSQL_DB_FIELD_REFERENCES_PRIV,
  MYSQL_DB_FIELD_INDEX_PRIV,
  MYSQL_DB_FIELD_ALTER_PRIV,
  MYSQL_DB_FIELD_CREATE_TMP_TABLE_PRIV,
  MYSQL_DB_FIELD_LOCK_TABLES_PRIV,
  MYSQL_DB_FIELD_CREATE_VIEW_PRIV,
  MYSQL_DB_FIELD_SHOW_VIEW_PRIV,
  MYSQL_DB_FIELD_CREATE_ROUTINE_PRIV,
  MYSQL_DB_FIELD_ALTER_ROUTINE_PRIV,
  MYSQL_DB_FIELD_EXECUTE_PRIV,
  MYSQL_DB_FIELD_EVENT_PRIV,
  MYSQL_DB_FIELD_TRIGGER_PRIV,
  MYSQL_DB_FIELD_DELETE_VERSIONING_ROWS_PRIV,
  MYSQL_DB_FIELD_COUNT
};

extern const TABLE_FIELD_DEF mysql_db_table_def;
extern bool mysql_user_table_is_in_short_password_format;

extern LEX_CSTRING host_not_specified;
extern LEX_CSTRING current_user;
extern LEX_CSTRING current_role;
extern LEX_CSTRING current_user_and_current_role;
extern LEX_CSTRING none;
extern LEX_CSTRING public_name;


static inline int access_denied_error_code(int passwd_used)
{
#ifdef mysqld_error_find_printf_error_used
  return 0;
#else
  return passwd_used == 2 ? ER_ACCESS_DENIED_NO_PASSWORD_ERROR
                          : ER_ACCESS_DENIED_ERROR;
#endif
}

/* prototypes */

bool hostname_requires_resolving(const char *hostname);
bool  acl_init(bool dont_read_acl_tables);
bool acl_reload(THD *thd);
void acl_free(bool end=0);
privilege_t acl_get_all3(Security_context *sctx, const char *db,
                         bool db_is_patern);
bool acl_authenticate(THD *thd, uint com_change_user_pkt_len);
bool acl_getroot(Security_context *sctx,
                 const LEX_CSTRING &user, const LEX_CSTRING &host,
                 const LEX_CSTRING &ip, const LEX_CSTRING &db);
bool acl_check_host(const char *host, const char *ip);
bool check_change_password(THD *thd, LEX_USER *user);
bool change_password(THD *thd, LEX_USER *user);

bool mysql_grant_role(THD *thd, List<LEX_USER> &user_list, bool revoke);
bool grant_init();
void grant_free(void);
bool grant_reload(THD *thd);
bool check_grant(THD *thd, privilege_t want_access, TABLE_LIST *tables,
                 bool any_combination_will_do, uint number, bool no_errors);
bool check_grant_column(const Security_context *sctx,
                        GRANT_INFO *grant,
                        const Lex_ident_db &db_name,
                        const Lex_ident_table &table_name,
                        const Lex_ident_column &column_name);
bool check_column_grant_in_table_ref(THD *thd, TABLE_LIST * table_ref,
                                     const Lex_ident_column &name, Field *fld);
bool check_grant_all_columns(THD *thd, privilege_t want_access,
                             Field_iterator_table_ref *fields);
bool check_grant_routine(THD *thd, privilege_t want_access,
                         TABLE_LIST *procs, const Sp_handler *sph,
                         bool no_error);
bool check_grant_db(Security_context *sctx, const LEX_CSTRING &db);

bool check_global_access(THD *thd, const privilege_t want_access, bool no_errors= false);
bool check_access(THD *thd, privilege_t want_access,
                  const char *db, privilege_t *save_priv,
                  GRANT_INTERNAL_INFO *grant_internal_info,
                  bool dont_check_global_grants, bool no_errors);
bool check_fk_parent_table_access(THD *thd, HA_CREATE_INFO *create_info,
                                  Alter_info *alter_info,
                                  const LEX_CSTRING &create_db);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
bool check_one_table_access(THD *thd, privilege_t privilege,
                            TABLE_LIST *tables);
bool check_single_table_access(THD *thd, privilege_t privilege,
                               TABLE_LIST *tables, bool no_errors);
bool check_routine_access(THD *thd, privilege_t want_access,
                          const LEX_CSTRING *db, const LEX_CSTRING *name,
                          const Sp_handler *sph, bool no_errors);
bool check_some_access(THD *thd, privilege_t want_access, TABLE_LIST *table);
bool check_some_routine_access(THD *thd, const LEX_CSTRING &db,
                               const LEX_CSTRING &name, const Sp_handler &sph);
bool check_table_access(THD *thd, privilege_t requirements, TABLE_LIST *tables,
                        bool any_combination_of_privileges_will_do,
                        uint number, bool no_errors);
#else
inline bool check_one_table_access(THD *thd, privilege_t privilege,
                                   TABLE_LIST *tables)
{
  return false;
}
inline bool check_single_table_access(THD *thd, privilege_t privilege,
                                      TABLE_LIST *tables, bool no_errors)
{
  return false;
}
inline bool check_routine_access(THD *thd, privilege_t want_access,
                                 const LEX_CSTRING *db,
                                 const LEX_CSTRING *name,
                                 const Sp_handler *sph, bool no_errors)
{
  return false;
}
inline bool check_some_access(THD *thd, privilege_t want_access,
                              TABLE_LIST *table)
{
  table->grant.privilege= want_access;
  return false;
}
inline bool check_some_routine_access(THD *, const LEX_CSTRING &,
                                      const LEX_CSTRING &, const Sp_handler &)
{
  return false;
}
inline bool check_table_access(THD *thd, privilege_t requirements,
                               TABLE_LIST *tables,
                               bool any_combination_of_privileges_will_do,
                               uint number, bool no_errors)
{
  return false;
}
#endif /*NO_EMBEDDED_ACCESS_CHECKS*/

privilege_t get_table_grant(THD *thd, TABLE_LIST *table);
privilege_t get_column_grant(THD *thd, GRANT_INFO *grant,
                             const Lex_ident_db &db_name,
                             const Lex_ident_table &table_name,
                             const Lex_ident_column &field_name);
bool get_show_user(THD *thd, LEX_USER *lex_user, const char **username,
                   const char **hostname, const char **rolename);
void mysql_show_grants_get_fields(THD *thd, List<Item> *fields,
                                  const char *name, size_t length);
bool mysql_show_grants(THD *thd, LEX_USER *user);
bool mysql_show_create_user(THD *thd, LEX_USER *user);
int fill_schema_enabled_roles(THD *thd, TABLE_LIST *tables, COND *cond);
int fill_schema_applicable_roles(THD *thd, TABLE_LIST *tables, COND *cond);
void get_privilege_desc(char *to, size_t max_length, privilege_t access);
void get_mqh(const char *user, const char *host, USER_CONN *uc);
bool mysql_create_user(THD *thd, List <LEX_USER> &list, bool handle_as_role);
bool mysql_drop_user(THD *thd, List <LEX_USER> &list, bool handle_as_role);
bool mysql_rename_user(THD *thd, List <LEX_USER> &list);
int mysql_alter_user(THD *thd, List <LEX_USER> &list);
bool mysql_revoke_all(THD *thd, List <LEX_USER> &list);
void fill_effective_table_privileges(THD *thd, GRANT_INFO *grant,
                                     const char *db, const char *table);
bool sp_revoke_privileges(THD *thd,
                          const Lex_ident_db &sp_db,
                          const Lex_ident_routine &sp_name,
                          const Sp_handler *sph);
bool sp_grant_privileges(THD *thd,
                         const Lex_ident_db &sp_db,
                         const Lex_ident_routine &sp_name,
                         const Sp_handler *sph);
bool check_routine_level_acl(THD *thd, privilege_t acl,
                             const char *db, const char *name,
                             const Sp_handler *sph);
bool is_acl_user(const LEX_CSTRING &host, const LEX_CSTRING &user);
int fill_users_schema_table(THD *thd, TABLE_LIST *tables, COND *cond);
int fill_schema_user_privileges(THD *thd, TABLE_LIST *tables, COND *cond);
int fill_schema_schema_privileges(THD *thd, TABLE_LIST *tables, COND *cond);
int fill_schema_table_privileges(THD *thd, TABLE_LIST *tables, COND *cond);
int fill_schema_column_privileges(THD *thd, TABLE_LIST *tables, COND *cond);
int wild_case_compare(CHARSET_INFO *cs, const char *str,const char *wildstr);

/**
  Result of an access check for an internal schema or table.
  Internal ACL checks are always performed *before* using
  the grant tables.
  This mechanism enforces that the server implementation has full
  control on its internal tables.
  Depending on the internal check result, the server implementation
  can choose to:
  - always allow access,
  - always deny access,
  - delegate the decision to the database administrator,
  by using the grant tables.
*/
enum ACL_internal_access_result
{
  /**
    Access granted for all the requested privileges,
    do not use the grant tables.
  */
  ACL_INTERNAL_ACCESS_GRANTED,
  /** Access denied, do not use the grant tables. */
  ACL_INTERNAL_ACCESS_DENIED,
  /** No decision yet, use the grant tables. */
  ACL_INTERNAL_ACCESS_CHECK_GRANT
};

/**
  Per internal table ACL access rules.
  This class is an interface.
  Per table(s) specific access rule should be implemented in a subclass.
  @sa ACL_internal_schema_access
*/
class ACL_internal_table_access
{
public:
  ACL_internal_table_access() = default;

  virtual ~ACL_internal_table_access() = default;

  /**
    Check access to an internal table.
    When a privilege is granted, this method add the requested privilege
    to save_priv.
    @param want_access the privileges requested
    @param [in, out] save_priv the privileges granted
    @return
      @retval ACL_INTERNAL_ACCESS_GRANTED All the requested privileges
      are granted, and saved in save_priv.
      @retval ACL_INTERNAL_ACCESS_DENIED At least one of the requested
      privileges was denied.
      @retval ACL_INTERNAL_ACCESS_CHECK_GRANT No requested privilege
      was denied, and grant should be checked for at least one
      privilege. Requested privileges that are granted, if any, are saved
      in save_priv.
  */
  virtual ACL_internal_access_result check(privilege_t want_access,
            privilege_t *save_priv, bool any_combination_will_do) const= 0;
};

/**
  Per internal schema ACL access rules.
  This class is an interface.
  Each per schema specific access rule should be implemented
  in a different subclass, and registered.
  Per schema access rules can control:
  - every schema privileges on schema.*
  - every table privileges on schema.table
  @sa ACL_internal_schema_registry
*/
class ACL_internal_schema_access
{
public:
  ACL_internal_schema_access() = default;

  virtual ~ACL_internal_schema_access() = default;

  /**
    Check access to an internal schema.
    @param want_access the privileges requested
    @param [in, out] save_priv the privileges granted
    @return
      @retval ACL_INTERNAL_ACCESS_GRANTED All the requested privileges
      are granted, and saved in save_priv.
      @retval ACL_INTERNAL_ACCESS_DENIED At least one of the requested
      privileges was denied.
      @retval ACL_INTERNAL_ACCESS_CHECK_GRANT No requested privilege
      was denied, and grant should be checked for at least one
      privilege. Requested privileges that are granted, if any, are saved
      in save_priv.
  */
  virtual ACL_internal_access_result check(privilege_t want_access,
                                           privilege_t *save_priv) const= 0;

  /**
    Search for per table ACL access rules by table name.
    @param name the table name
    @return per table access rules, or NULL
  */
  virtual const ACL_internal_table_access *lookup(const char *name) const= 0;
};

/**
  A registry for per internal schema ACL.
  An 'internal schema' is a database schema maintained by the
  server implementation, such as 'performance_schema' and 'INFORMATION_SCHEMA'.
*/
class ACL_internal_schema_registry
{
public:
  static void register_schema(const LEX_CSTRING *name,
                              const ACL_internal_schema_access *access);
  static const ACL_internal_schema_access *lookup(const char *name);
};

int acl_setauthorization(THD *thd, const LEX_USER *user);
int acl_setrole(THD *thd, const LEX_CSTRING &rolename, privilege_t access);
int acl_check_setrole(THD *thd,
                      const LEX_CSTRING &rolename,
                      privilege_t *access);
int acl_check_set_default_role(THD *thd,
                               const LEX_CSTRING &host,
                               const LEX_CSTRING &user,
                               const LEX_CSTRING &role);
int acl_set_default_role(THD *thd,
                         const LEX_CSTRING &host,
                         const LEX_CSTRING &user,
                         const LEX_CSTRING &rolename);

extern SHOW_VAR acl_statistics[];

/* Check if a role is granted to a user/role.

   If hostname == NULL, search for a role as the starting grantee.
*/
bool check_role_is_granted(const char *username,
                           const char *hostname,
                           const char *rolename);

#ifndef DBUG_OFF
extern ulong role_global_merges, role_db_merges, role_table_merges,
             role_column_merges, role_routine_merges;
#endif


class Sql_cmd_grant: public Sql_cmd
{
protected:
  enum_sql_command m_command;
  bool m_create_new_users;
  List<LEX_USER> m_resolved_users;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  void warn_hostname_requires_resolving(THD *thd, List<LEX_USER> &list);
  void user_list_reset_mqh();
  bool grant_stage0(THD *thd);
#endif
public:
  Sql_cmd_grant(enum_sql_command command)
   :m_command(command), m_create_new_users(false)
  { }
  bool is_revoke() const { return m_command == SQLCOM_REVOKE; }
  enum_sql_command sql_command_code() const override { return m_command; }
};


class Sql_cmd_grant_proxy: public Sql_cmd_grant
{
  privilege_t m_grant_option;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  bool check_access_proxy(THD *thd, List<LEX_USER> &list);
#endif
public:
  Sql_cmd_grant_proxy(enum_sql_command command, privilege_t grant_option)
   :Sql_cmd_grant(command), m_grant_option(grant_option)
  { }
  bool execute(THD *thd) override;
};


class Sql_cmd_grant_object: public Sql_cmd_grant
{
protected:
  Grant_privilege &m_gp;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  bool grant_stage0_exact_object(THD *thd, TABLE_LIST *table);
#endif
public:
  Sql_cmd_grant_object(enum_sql_command command, Grant_privilege &grant)
   :Sql_cmd_grant(command), m_gp(grant)
  { }
};


class Sql_cmd_grant_table: public Sql_cmd_grant_object
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  bool execute_grant_database_or_global(THD *thd);
  bool execute_grant_global(THD *thd);
  bool execute_grant_database(THD *thd);
  bool execute_grant_table(THD *thd, TABLE_LIST *table);
#endif
public:
  Sql_cmd_grant_table(enum_sql_command command, Grant_privilege &grant)
   :Sql_cmd_grant_object(command, grant)
  { }
  bool execute(THD *thd) override;
};



class Sql_cmd_grant_sp: public Sql_cmd_grant_object
{
  const Sp_handler &m_sph;
public:
  Sql_cmd_grant_sp(enum_sql_command command, Grant_privilege &grant,
                   const Sp_handler &sph)
   :Sql_cmd_grant_object(command, grant),
    m_sph(sph)
  { }
  bool execute(THD *thd) override;
};

#endif /* SQL_ACL_INCLUDED */
