#ifndef SQL_AUDIT_INCLUDED
#define SQL_AUDIT_INCLUDED

/* Copyright (c) 2007, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2017, MariaDB Corporation.

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


#include <my_global.h>

#include <mysql/plugin_audit.h>
#include "sql_class.h"

extern unsigned long mysql_global_audit_mask[];


extern void mysql_audit_initialize();
extern void mysql_audit_finalize();


extern void mysql_audit_init_thd(THD *thd);
extern void mysql_audit_free_thd(THD *thd);
extern void mysql_audit_acquire_plugins(THD *thd, ulong *event_class_mask);


#ifndef EMBEDDED_LIBRARY
extern void mysql_audit_notify(THD *thd, uint event_class, const void *event);

static inline bool mysql_audit_general_enabled()
{
  return mysql_global_audit_mask[0] & MYSQL_AUDIT_GENERAL_CLASSMASK;
}

static inline bool mysql_audit_connection_enabled()
{
  return mysql_global_audit_mask[0] & MYSQL_AUDIT_CONNECTION_CLASSMASK;
}

static inline bool mysql_audit_table_enabled()
{
  return mysql_global_audit_mask[0] & MYSQL_AUDIT_TABLE_CLASSMASK;
}

#else
static inline void mysql_audit_notify(THD *thd, uint event_class,
                                      const void *event) {}
#define mysql_audit_general_enabled() 0
#define mysql_audit_connection_enabled() 0
#define mysql_audit_table_enabled() 0
#endif
extern my_bool mysql_audit_release_required(THD *thd);
extern void mysql_audit_release(THD *thd);

static inline unsigned int strlen_uint(const char *s)
{
  return (uint)strlen(s);
}

static inline unsigned int safe_strlen_uint(const char *s)
{
  return (uint)safe_strlen(s);
}

#define MAX_USER_HOST_SIZE 512
static inline uint make_user_name(THD *thd, char *buf)
{
  const Security_context *sctx= thd->security_ctx;
  char *end= strxnmov(buf, MAX_USER_HOST_SIZE,
                  sctx->priv_user[0] ? sctx->priv_user : "", "[",
                  sctx->user ? sctx->user : "", "] @ ",
                  sctx->host ? sctx->host : "", " [",
                  sctx->ip ? sctx->ip : "", "]", NullS);
  return (uint)(end-buf);
}

/**
  Call audit plugins of GENERAL audit class, MYSQL_AUDIT_GENERAL_LOG subtype.
  
  @param[in] thd
  @param[in] time             time that event occurred
  @param[in] user             User name
  @param[in] userlen          User name length
  @param[in] cmd              Command name
  @param[in] cmdlen           Command name length
  @param[in] query            Query string
  @param[in] querylen         Query string length
*/
 
static inline
void mysql_audit_general_log(THD *thd, time_t time,
                             const char *user, uint userlen,
                             const char *cmd, uint cmdlen,
                             const char *query, uint querylen)
{
  if (mysql_audit_general_enabled())
  {
    mysql_event_general event;

    event.event_subclass= MYSQL_AUDIT_GENERAL_LOG;
    event.general_error_code= 0;
    event.general_time= time;
    event.general_user= user;
    event.general_user_length= userlen;
    event.general_command= cmd;
    event.general_command_length= cmdlen;
    event.general_query= query;
    event.general_query_length= querylen;
    event.general_rows= 0;

    if (thd)
    {
      event.general_thread_id= (unsigned long)thd->thread_id;
      event.general_charset= thd->variables.character_set_client;
      event.database= thd->db;
      event.database_length= (unsigned int)thd->db_length;
      event.query_id= thd->query_id;
    }
    else
    {
      event.general_thread_id= 0;
      event.general_charset= global_system_variables.character_set_client;
      event.database= "";
      event.database_length= 0;
      event.query_id= 0;
    }

    mysql_audit_notify(thd, MYSQL_AUDIT_GENERAL_CLASS, &event);
  }
}

/**
  Call audit plugins of GENERAL audit class.
  event_subtype should be set to one of:
    MYSQL_AUDIT_GENERAL_ERROR
    MYSQL_AUDIT_GENERAL_RESULT
    MYSQL_AUDIT_GENERAL_STATUS
  
  @param[in] thd
  @param[in] event_subtype    Type of general audit event.
  @param[in] error_code       Error code
  @param[in] msg              Message
*/
static inline
void mysql_audit_general(THD *thd, uint event_subtype,
                         int error_code, const char *msg)
{
  if (mysql_audit_general_enabled())
  {
    char user_buff[MAX_USER_HOST_SIZE];
    mysql_event_general event;

    event.event_subclass= event_subtype;
    event.general_error_code= error_code;
    event.general_time= my_time(0);
    event.general_command= msg;
    event.general_command_length= safe_strlen_uint(msg);

    if (thd)
    {
      event.general_user= user_buff;
      event.general_user_length= make_user_name(thd, user_buff);
      event.general_thread_id= (unsigned long)thd->thread_id;
      event.general_query= thd->query_string.str();
      event.general_query_length= (unsigned) thd->query_string.length();
      event.general_charset= thd->query_string.charset();
      event.general_rows= thd->get_stmt_da()->current_row_for_warning();
      event.database= thd->db;
      event.database_length= (uint)thd->db_length;
      event.query_id= thd->query_id;
    }
    else
    {
      event.general_user= NULL;
      event.general_user_length= 0;
      event.general_thread_id= 0;
      event.general_query= NULL;
      event.general_query_length= 0;
      event.general_charset= &my_charset_bin;
      event.general_rows= 0;
      event.database= "";
      event.database_length= 0;
      event.query_id= 0;
    }

    mysql_audit_notify(thd, MYSQL_AUDIT_GENERAL_CLASS, &event);
  }
}

static inline
void mysql_audit_notify_connection_connect(THD *thd)
{
  if (mysql_audit_connection_enabled())
  {
    const Security_context *sctx= thd->security_ctx;
    mysql_event_connection event;

    event.event_subclass= MYSQL_AUDIT_CONNECTION_CONNECT;
    event.status= thd->get_stmt_da()->is_error() ?
                  thd->get_stmt_da()->sql_errno() : 0;
    event.thread_id= (unsigned long)thd->thread_id;
    event.user= sctx->user;
    event.user_length= safe_strlen_uint(sctx->user);
    event.priv_user= sctx->priv_user;
    event.priv_user_length= strlen_uint(sctx->priv_user);
    event.external_user= sctx->external_user;
    event.external_user_length= safe_strlen_uint(sctx->external_user);
    event.proxy_user= sctx->proxy_user;
    event.proxy_user_length= strlen_uint(sctx->proxy_user);
    event.host= sctx->host;
    event.host_length= safe_strlen_uint(sctx->host);
    event.ip= sctx->ip;
    event.ip_length= safe_strlen_uint(sctx->ip);
    event.database= thd->db;
    event.database_length= safe_strlen_uint(thd->db);

    mysql_audit_notify(thd, MYSQL_AUDIT_CONNECTION_CLASS, &event);
  }
}

static inline
void mysql_audit_notify_connection_disconnect(THD *thd, int errcode)
{
  if (mysql_audit_connection_enabled())
  {
    const Security_context *sctx= thd->security_ctx;
    mysql_event_connection event;

    event.event_subclass= MYSQL_AUDIT_CONNECTION_DISCONNECT;
    event.status= errcode;
    event.thread_id= (unsigned long)thd->thread_id;
    event.user= sctx->user;
    event.user_length= safe_strlen_uint(sctx->user);
    event.priv_user= sctx->priv_user;
    event.priv_user_length= strlen_uint(sctx->priv_user);
    event.external_user= sctx->external_user;
    event.external_user_length= safe_strlen_uint(sctx->external_user);
    event.proxy_user= sctx->proxy_user;
    event.proxy_user_length= strlen_uint(sctx->proxy_user);
    event.host= sctx->host;
    event.host_length= safe_strlen_uint(sctx->host);
    event.ip= sctx->ip;
    event.ip_length= safe_strlen_uint(sctx->ip) ;
    event.database= thd->db;
    event.database_length= safe_strlen_uint(thd->db);

    mysql_audit_notify(thd, MYSQL_AUDIT_CONNECTION_CLASS, &event);
  }
}

static inline
void mysql_audit_notify_connection_change_user(THD *thd)
{
  if (mysql_audit_connection_enabled())
  {
    const Security_context *sctx= thd->security_ctx;
    mysql_event_connection event;

    event.event_subclass= MYSQL_AUDIT_CONNECTION_CHANGE_USER;
    event.status= thd->get_stmt_da()->is_error() ?
                  thd->get_stmt_da()->sql_errno() : 0;
    event.thread_id= (unsigned long)thd->thread_id;
    event.user= sctx->user;
    event.user_length= safe_strlen_uint(sctx->user);
    event.priv_user= sctx->priv_user;
    event.priv_user_length= strlen_uint(sctx->priv_user);
    event.external_user= sctx->external_user;
    event.external_user_length= safe_strlen_uint(sctx->external_user);
    event.proxy_user= sctx->proxy_user;
    event.proxy_user_length= strlen_uint(sctx->proxy_user);
    event.host= sctx->host;
    event.host_length= safe_strlen_uint(sctx->host);
    event.ip= sctx->ip;
    event.ip_length= safe_strlen_uint(sctx->ip);
    event.database= thd->db;
    event.database_length= safe_strlen_uint(thd->db);

    mysql_audit_notify(thd, MYSQL_AUDIT_CONNECTION_CLASS, &event);
  }
}

static inline
void mysql_audit_external_lock_ex(THD *thd, my_thread_id thread_id,
    const char *user, const char *host, const char *ip, query_id_t query_id,
    TABLE_SHARE *share, int lock)
{
  if (lock != F_UNLCK && mysql_audit_table_enabled())
  {
    const Security_context *sctx= thd->security_ctx;
    mysql_event_table event;

    event.event_subclass= MYSQL_AUDIT_TABLE_LOCK;
    event.read_only= lock == F_RDLCK;
    event.thread_id= (unsigned long)thread_id;
    event.user= user;
    event.priv_user= sctx->priv_user;
    event.priv_host= sctx->priv_host;
    event.external_user= sctx->external_user;
    event.proxy_user= sctx->proxy_user;
    event.host= host;
    event.ip= ip;
    event.database= share->db.str;
    event.database_length= (unsigned int)share->db.length;
    event.table= share->table_name.str;
    event.table_length= (unsigned int)share->table_name.length;
    event.new_database= 0;
    event.new_database_length= 0;
    event.new_table= 0;
    event.new_table_length= 0;
    event.query_id= query_id;

    mysql_audit_notify(thd, MYSQL_AUDIT_TABLE_CLASS, &event);
  }
}

static inline
void mysql_audit_external_lock(THD *thd, TABLE_SHARE *share, int lock)
{
  mysql_audit_external_lock_ex(thd, thd->thread_id, thd->security_ctx->user,
      thd->security_ctx->host, thd->security_ctx->ip, thd->query_id,
      share, lock);
}

static inline
void mysql_audit_create_table(TABLE *table)
{
  if (mysql_audit_table_enabled())
  {
    THD *thd= table->in_use;
    const TABLE_SHARE *share= table->s;
    const Security_context *sctx= thd->security_ctx;
    mysql_event_table event;

    event.event_subclass= MYSQL_AUDIT_TABLE_CREATE;
    event.read_only= 0;
    event.thread_id= (unsigned long)thd->thread_id;
    event.user= sctx->user;
    event.priv_user= sctx->priv_user;
    event.priv_host= sctx->priv_host;
    event.external_user= sctx->external_user;
    event.proxy_user= sctx->proxy_user;
    event.host= sctx->host;
    event.ip= sctx->ip;
    event.database= share->db.str;
    event.database_length= (unsigned int)share->db.length;
    event.table= share->table_name.str;
    event.table_length= (unsigned int)share->table_name.length;
    event.new_database= 0;
    event.new_database_length= 0;
    event.new_table= 0;
    event.new_table_length= 0;
    event.query_id= thd->query_id;

    mysql_audit_notify(thd, MYSQL_AUDIT_TABLE_CLASS, &event);
  }
}

static inline
void mysql_audit_drop_table(THD *thd, TABLE_LIST *table)
{
  if (mysql_audit_table_enabled())
  {
    const Security_context *sctx= thd->security_ctx;
    mysql_event_table event;

    event.event_subclass= MYSQL_AUDIT_TABLE_DROP;
    event.read_only= 0;
    event.thread_id= (unsigned long)thd->thread_id;
    event.user= sctx->user;
    event.priv_user= sctx->priv_user;
    event.priv_host= sctx->priv_host;
    event.external_user= sctx->external_user;
    event.proxy_user= sctx->proxy_user;
    event.host= sctx->host;
    event.ip= sctx->ip;
    event.database= table->db;
    event.database_length= (unsigned int)table->db_length;
    event.table= table->table_name;
    event.table_length= (unsigned int)table->table_name_length;
    event.new_database= 0;
    event.new_database_length= 0;
    event.new_table= 0;
    event.new_table_length= 0;
    event.query_id= thd->query_id;

    mysql_audit_notify(thd, MYSQL_AUDIT_TABLE_CLASS, &event);
  }
}

static inline
void mysql_audit_rename_table(THD *thd, const char *old_db, const char *old_tb,
                              const char *new_db, const char *new_tb)
{
  if (mysql_audit_table_enabled())
  {
    const Security_context *sctx= thd->security_ctx;
    mysql_event_table event;

    event.event_subclass= MYSQL_AUDIT_TABLE_RENAME;
    event.read_only= 0;
    event.thread_id= (unsigned long)thd->thread_id;
    event.user= sctx->user;
    event.priv_user= sctx->priv_user;
    event.priv_host= sctx->priv_host;
    event.external_user= sctx->external_user;
    event.proxy_user= sctx->proxy_user;
    event.host= sctx->host;
    event.ip= sctx->ip;
    event.database= old_db;
    event.database_length= strlen_uint(old_db);
    event.table= old_tb;
    event.table_length= strlen_uint(old_tb);
    event.new_database= new_db;
    event.new_database_length= strlen_uint(new_db);
    event.new_table= new_tb;
    event.new_table_length= strlen_uint(new_tb);
    event.query_id= thd->query_id;

    mysql_audit_notify(thd, MYSQL_AUDIT_TABLE_CLASS, &event);
  }
}

static inline
void mysql_audit_alter_table(THD *thd, TABLE_LIST *table)
{
  if (mysql_audit_table_enabled())
  {
    const Security_context *sctx= thd->security_ctx;
    mysql_event_table event;

    event.event_subclass= MYSQL_AUDIT_TABLE_ALTER;
    event.read_only= 0;
    event.thread_id= (unsigned long)thd->thread_id;
    event.user= sctx->user;
    event.priv_user= sctx->priv_user;
    event.priv_host= sctx->priv_host;
    event.external_user= sctx->external_user;
    event.proxy_user= sctx->proxy_user;
    event.host= sctx->host;
    event.ip= sctx->ip;
    event.database= table->db;
    event.database_length= (unsigned int)table->db_length;
    event.table= table->table_name;
    event.table_length= (unsigned int)table->table_name_length;
    event.new_database= 0;
    event.new_database_length= 0;
    event.new_table= 0;
    event.new_table_length= 0;
    event.query_id= thd->query_id;

    mysql_audit_notify(thd, MYSQL_AUDIT_TABLE_CLASS, &event);
  }
}

#endif /* SQL_AUDIT_INCLUDED */
