/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */


/*
  The privileges are saved in the following tables:
  mysql/user	 ; super user who are allowed to do almost anything
  mysql/host	 ; host privileges. This is used if host is empty in mysql/db.
  mysql/db	 ; database privileges / user

  data in tables is sorted according to how many not-wild-cards there is
  in the relevant fields. Empty strings comes last.
*/

#include <my_global.h>                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "sql_acl.h"         // MYSQL_DB_FIELD_COUNT, ACL_ACCESS
#include "sql_base.h"                           // close_mysql_tables
#include "key.h"             // key_copy, key_cmp_if_same, key_restore
#include "sql_show.h"        // append_identifier
#include "sql_table.h"                         // build_table_filename
#include "hash_filo.h"
#include "sql_parse.h"                          // check_access
#include "sql_view.h"                           // VIEW_ANY_ACL
#include "records.h"              // READ_RECORD, read_record_info,
                                  // init_read_record, end_read_record
#include "rpl_filter.h"           // rpl_filter
#include "rpl_rli.h"
#include <m_ctype.h>
#include <stdarg.h>
#include "sp_head.h"
#include "sp.h"
#include "transaction.h"
#include "lock.h"                               // MYSQL_LOCK_IGNORE_TIMEOUT
#include <sql_common.h>
#include <mysql/plugin_auth.h>
#include <mysql/plugin_password_validation.h>
#include "sql_connect.h"
#include "hostname.h"
#include "sql_db.h"
#include "sql_array.h"
#include "sql_hset.h"
#include "password.h"

#include "sql_plugin_compat.h"

bool mysql_user_table_is_in_short_password_format= false;

static const
TABLE_FIELD_TYPE mysql_db_table_fields[MYSQL_DB_FIELD_COUNT] = {
  {
    { C_STRING_WITH_LEN("Host") },
    { C_STRING_WITH_LEN("char(60)") },
    {NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("Db") },
    { C_STRING_WITH_LEN("char(64)") },
    {NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("User") },
    { C_STRING_WITH_LEN("char(") },
    {NULL, 0}
  },
  {
    { C_STRING_WITH_LEN("Select_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Insert_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Update_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Delete_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Create_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Drop_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Grant_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("References_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Index_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Alter_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Create_tmp_table_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Lock_tables_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Create_view_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Show_view_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Create_routine_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Alter_routine_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Execute_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Event_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  },
  {
    { C_STRING_WITH_LEN("Trigger_priv") },
    { C_STRING_WITH_LEN("enum('N','Y')") },
    { C_STRING_WITH_LEN("utf8") }
  }
};

const TABLE_FIELD_DEF
mysql_db_table_def= {MYSQL_DB_FIELD_COUNT, mysql_db_table_fields, 0, (uint*) 0 };

static LEX_STRING native_password_plugin_name= {
  C_STRING_WITH_LEN("mysql_native_password")
};

static LEX_STRING old_password_plugin_name= {
  C_STRING_WITH_LEN("mysql_old_password")
};

/// @todo make it configurable
LEX_STRING *default_auth_plugin_name= &native_password_plugin_name;

/*
  Wildcard host, matches any hostname
*/
LEX_STRING host_not_specified= { C_STRING_WITH_LEN("%") };

/*
  Constants, used in the SHOW GRANTS command.
  Their actual string values are irrelevant, they're always compared
  as pointers to these string constants.
*/
LEX_STRING current_user= { C_STRING_WITH_LEN("*current_user") };
LEX_STRING current_role= { C_STRING_WITH_LEN("*current_role") };
LEX_STRING current_user_and_current_role= { C_STRING_WITH_LEN("*current_user_and_current_role") };


#ifndef NO_EMBEDDED_ACCESS_CHECKS
static plugin_ref old_password_plugin;
#endif
static plugin_ref native_password_plugin;

/* Classes */

struct acl_host_and_ip
{
  char *hostname;
  long ip, ip_mask;                      // Used with masked ip:s
};

#ifndef NO_EMBEDDED_ACCESS_CHECKS
static bool compare_hostname(const acl_host_and_ip *, const char *, const char *);
#else
#define compare_hostname(X,Y,Z) 0
#endif

class ACL_ACCESS {
public:
  ulong sort;
  ulong access;
};

/* ACL_HOST is used if no host is specified */

class ACL_HOST :public ACL_ACCESS
{
public:
  acl_host_and_ip host;
  char *db;
};

class ACL_USER_BASE :public ACL_ACCESS
{

public:
  static void *operator new(size_t size, MEM_ROOT *mem_root)
  { return (void*) alloc_root(mem_root, size); }

  uchar flags;           // field used to store various state information
  LEX_STRING user;
  /* list to hold references to granted roles (ACL_ROLE instances) */
  DYNAMIC_ARRAY role_grants;
};

class ACL_USER :public ACL_USER_BASE
{
public:
  acl_host_and_ip host;
  uint hostname_length;
  USER_RESOURCES user_resource;
  uint8 salt[SCRAMBLE_LENGTH + 1];       // scrambled password in binary form
  uint8 salt_len;        // 0 - no password, 4 - 3.20, 8 - 4.0,  20 - 4.1.1
  enum SSL_type ssl_type;
  const char *ssl_cipher, *x509_issuer, *x509_subject;
  LEX_STRING plugin;
  LEX_STRING auth_string;
  LEX_STRING default_rolename;

  ACL_USER *copy(MEM_ROOT *root)
  {
    ACL_USER *dst= (ACL_USER *) alloc_root(root, sizeof(ACL_USER));
    if (!dst)
      return 0;
    *dst= *this;
    dst->user.str= safe_strdup_root(root, user.str);
    dst->user.length= user.length;
    dst->ssl_cipher= safe_strdup_root(root, ssl_cipher);
    dst->x509_issuer= safe_strdup_root(root, x509_issuer);
    dst->x509_subject= safe_strdup_root(root, x509_subject);
    if (plugin.str == native_password_plugin_name.str ||
        plugin.str == old_password_plugin_name.str)
      dst->plugin= plugin;
    else
      dst->plugin.str= strmake_root(root, plugin.str, plugin.length);
    dst->auth_string.str= safe_strdup_root(root, auth_string.str);
    dst->host.hostname= safe_strdup_root(root, host.hostname);
    dst->default_rolename.str= safe_strdup_root(root, default_rolename.str);
    dst->default_rolename.length= default_rolename.length;
    bzero(&dst->role_grants, sizeof(role_grants));
    return dst;
  }

  int cmp(const char *user2, const char *host2)
  {
    CHARSET_INFO *cs= system_charset_info;
    int res;
    res= strcmp(safe_str(user.str), safe_str(user2));
    if (!res)
      res= my_strcasecmp(cs, host.hostname, host2);
    return res;
  }

  bool eq(const char *user2, const char *host2) { return !cmp(user2, host2); }

  bool wild_eq(const char *user2, const char *host2, const char *ip2)
  {
    if (strcmp(safe_str(user.str), safe_str(user2)))
      return false;

    return compare_hostname(&host, host2, ip2 ? ip2 : host2);
  }
};

class ACL_ROLE :public ACL_USER_BASE
{
public:
  /*
    In case of granting a role to a role, the access bits are merged together
    via a bit OR operation and placed in the ACL_USER::access field.

    When rebuilding role_grants via the rebuild_role_grant function,
    the ACL_USER::access field needs to be reset first. The field
    initial_role_access holds initial grants, as granted directly to the role
  */
  ulong initial_role_access;
  /*
    In subgraph traversal, when we need to traverse only a part of the graph
    (e.g. all direct and indirect grantees of a role X), the counter holds the
    number of affected neighbour nodes.
    See also propagate_role_grants()
  */
  uint  counter;
  DYNAMIC_ARRAY parent_grantee; // array of backlinks to elements granted

  ACL_ROLE(ACL_USER * user, MEM_ROOT *mem);
  ACL_ROLE(const char * rolename, ulong privileges, MEM_ROOT *mem);

};

class ACL_DB :public ACL_ACCESS
{
public:
  acl_host_and_ip host;
  char *user,*db;
  ulong initial_access; /* access bits present in the table */
};

#ifndef DBUG_OFF
/* status variables, only visible in SHOW STATUS after -#d,role_merge_stats */
ulong role_global_merges= 0, role_db_merges= 0, role_table_merges= 0,
      role_column_merges= 0, role_routine_merges= 0;
#endif

#ifndef NO_EMBEDDED_ACCESS_CHECKS
static bool fix_and_copy_user(LEX_USER *to, LEX_USER *from, THD *thd);
static void update_hostname(acl_host_and_ip *host, const char *hostname);
static ulong get_sort(uint count,...);
static bool show_proxy_grants (THD *, const char *, const char *,
                               char *, size_t);
static bool show_role_grants(THD *, const char *, const char *,
                             ACL_USER_BASE *, char *, size_t);
static bool show_global_privileges(THD *, ACL_USER_BASE *,
                                   bool, char *, size_t);
static bool show_database_privileges(THD *, const char *, const char *,
                                     char *, size_t);
static bool show_table_and_column_privileges(THD *, const char *, const char *,
                                             char *, size_t);
static int show_routine_grants(THD *, const char *, const char *, HASH *,
                               const char *, int, char *, int);

class ACL_PROXY_USER :public ACL_ACCESS
{
  acl_host_and_ip host;
  const char *user;
  acl_host_and_ip proxied_host;
  const char *proxied_user;
  bool with_grant;

  typedef enum {
    MYSQL_PROXIES_PRIV_HOST,
    MYSQL_PROXIES_PRIV_USER,
    MYSQL_PROXIES_PRIV_PROXIED_HOST,
    MYSQL_PROXIES_PRIV_PROXIED_USER,
    MYSQL_PROXIES_PRIV_WITH_GRANT,
    MYSQL_PROXIES_PRIV_GRANTOR,
    MYSQL_PROXIES_PRIV_TIMESTAMP } proxy_table_fields;
public:
  ACL_PROXY_USER () {};

  void init(const char *host_arg, const char *user_arg,
       const char *proxied_host_arg, const char *proxied_user_arg,
       bool with_grant_arg)
  {
    user= (user_arg && *user_arg) ? user_arg : NULL;
    update_hostname (&host, (host_arg && *host_arg) ? host_arg : NULL);
    proxied_user= (proxied_user_arg && *proxied_user_arg) ?
      proxied_user_arg : NULL;
    update_hostname (&proxied_host,
                     (proxied_host_arg && *proxied_host_arg) ?
                     proxied_host_arg : NULL);
    with_grant= with_grant_arg;
    sort= get_sort(4, host.hostname, user, proxied_host.hostname, proxied_user);
  }

  void init(MEM_ROOT *mem, const char *host_arg, const char *user_arg,
       const char *proxied_host_arg, const char *proxied_user_arg,
       bool with_grant_arg)
  {
    init ((host_arg && *host_arg) ? strdup_root (mem, host_arg) : NULL,
          (user_arg && *user_arg) ? strdup_root (mem, user_arg) : NULL,
          (proxied_host_arg && *proxied_host_arg) ?
            strdup_root (mem, proxied_host_arg) : NULL,
          (proxied_user_arg && *proxied_user_arg) ?
            strdup_root (mem, proxied_user_arg) : NULL,
          with_grant_arg);
  }

  void init(TABLE *table, MEM_ROOT *mem)
  {
    init (get_field(mem, table->field[MYSQL_PROXIES_PRIV_HOST]),
          get_field(mem, table->field[MYSQL_PROXIES_PRIV_USER]),
          get_field(mem, table->field[MYSQL_PROXIES_PRIV_PROXIED_HOST]),
          get_field(mem, table->field[MYSQL_PROXIES_PRIV_PROXIED_USER]),
          table->field[MYSQL_PROXIES_PRIV_WITH_GRANT]->val_int() != 0);
  }

  bool get_with_grant() { return with_grant; }
  const char *get_user() { return user; }
  const char *get_host() { return host.hostname; }
  const char *get_proxied_user() { return proxied_user; }
  const char *get_proxied_host() { return proxied_host.hostname; }
  void set_user(MEM_ROOT *mem, const char *user_arg)
  {
    user= user_arg && *user_arg ? strdup_root(mem, user_arg) : NULL;
  }
  void set_host(MEM_ROOT *mem, const char *host_arg)
  {
    update_hostname(&host, safe_strdup_root(mem, host_arg));
  }

  bool check_validity(bool check_no_resolve)
  {
    if (check_no_resolve &&
        (hostname_requires_resolving(host.hostname) ||
         hostname_requires_resolving(proxied_host.hostname)))
    {
      sql_print_warning("'proxies_priv' entry '%s@%s %s@%s' "
                        "ignored in --skip-name-resolve mode.",
                        safe_str(proxied_user),
                        safe_str(proxied_host.hostname),
                        safe_str(user),
                        safe_str(host.hostname));
      return TRUE;
    }
    return FALSE;
  }

  bool matches(const char *host_arg, const char *user_arg, const char *ip_arg,
                const char *proxied_user_arg)
  {
    DBUG_ENTER("ACL_PROXY_USER::matches");
    DBUG_PRINT("info", ("compare_hostname(%s,%s,%s) &&"
                        "compare_hostname(%s,%s,%s) &&"
                        "wild_compare (%s,%s) &&"
                        "wild_compare (%s,%s)",
                        host.hostname, host_arg, ip_arg, proxied_host.hostname,
                        host_arg, ip_arg, user_arg, user,
                        proxied_user_arg, proxied_user));
    DBUG_RETURN(compare_hostname(&host, host_arg, ip_arg) &&
                compare_hostname(&proxied_host, host_arg, ip_arg) &&
                (!user ||
                 (user_arg && !wild_compare(user_arg, user, TRUE))) &&
                (!proxied_user ||
                 (proxied_user && !wild_compare(proxied_user_arg,
                                                proxied_user, TRUE))));
  }


  inline static bool auth_element_equals(const char *a, const char *b)
  {
    return (a == b || (a != NULL && b != NULL && !strcmp(a,b)));
  }


  bool pk_equals(ACL_PROXY_USER *grant)
  {
    DBUG_ENTER("pk_equals");
    DBUG_PRINT("info", ("strcmp(%s,%s) &&"
                        "strcmp(%s,%s) &&"
                        "wild_compare (%s,%s) &&"
                        "wild_compare (%s,%s)",
                        user, grant->user, proxied_user, grant->proxied_user,
                        host.hostname, grant->host.hostname,
                        proxied_host.hostname, grant->proxied_host.hostname));

    bool res= auth_element_equals(user, grant->user) &&
              auth_element_equals(proxied_user, grant->proxied_user) &&
              auth_element_equals(host.hostname, grant->host.hostname) &&
              auth_element_equals(proxied_host.hostname,
                                  grant->proxied_host.hostname);
    DBUG_RETURN(res);
  }


  bool granted_on(const char *host_arg, const char *user_arg)
  {
    return (((!user && (!user_arg || !user_arg[0])) ||
             (user && user_arg && !strcmp(user, user_arg))) &&
            ((!host.hostname && (!host_arg || !host_arg[0])) ||
             (host.hostname && host_arg && !strcmp(host.hostname, host_arg))));
  }


  void print_grant(String *str)
  {
    str->append(STRING_WITH_LEN("GRANT PROXY ON '"));
    if (proxied_user)
      str->append(proxied_user, strlen(proxied_user));
    str->append(STRING_WITH_LEN("'@'"));
    if (proxied_host.hostname)
      str->append(proxied_host.hostname, strlen(proxied_host.hostname));
    str->append(STRING_WITH_LEN("' TO '"));
    if (user)
      str->append(user, strlen(user));
    str->append(STRING_WITH_LEN("'@'"));
    if (host.hostname)
      str->append(host.hostname, strlen(host.hostname));
    str->append(STRING_WITH_LEN("'"));
    if (with_grant)
      str->append(STRING_WITH_LEN(" WITH GRANT OPTION"));
  }

  void set_data(ACL_PROXY_USER *grant)
  {
    with_grant= grant->with_grant;
  }

  static int store_pk(TABLE *table,
                      const LEX_STRING *host,
                      const LEX_STRING *user,
                      const LEX_STRING *proxied_host,
                      const LEX_STRING *proxied_user)
  {
    DBUG_ENTER("ACL_PROXY_USER::store_pk");
    DBUG_PRINT("info", ("host=%s, user=%s, proxied_host=%s, proxied_user=%s",
                        host->str, user->str,
                        proxied_host->str, proxied_user->str));
    if (table->field[MYSQL_PROXIES_PRIV_HOST]->store(host->str,
                                                   host->length,
                                                   system_charset_info))
      DBUG_RETURN(TRUE);
    if (table->field[MYSQL_PROXIES_PRIV_USER]->store(user->str,
                                                   user->length,
                                                   system_charset_info))
      DBUG_RETURN(TRUE);
    if (table->field[MYSQL_PROXIES_PRIV_PROXIED_HOST]->store(proxied_host->str,
                                                           proxied_host->length,
                                                           system_charset_info))
      DBUG_RETURN(TRUE);
    if (table->field[MYSQL_PROXIES_PRIV_PROXIED_USER]->store(proxied_user->str,
                                                           proxied_user->length,
                                                           system_charset_info))
      DBUG_RETURN(TRUE);

    DBUG_RETURN(FALSE);
  }

  static int store_data_record(TABLE *table,
                               const LEX_STRING *host,
                               const LEX_STRING *user,
                               const LEX_STRING *proxied_host,
                               const LEX_STRING *proxied_user,
                               bool with_grant,
                               const char *grantor)
  {
    DBUG_ENTER("ACL_PROXY_USER::store_pk");
    if (store_pk(table,  host, user, proxied_host, proxied_user))
      DBUG_RETURN(TRUE);
    DBUG_PRINT("info", ("with_grant=%s", with_grant ? "TRUE" : "FALSE"));
    if (table->field[MYSQL_PROXIES_PRIV_WITH_GRANT]->store(with_grant ? 1 : 0,
                                                           TRUE))
      DBUG_RETURN(TRUE);
    if (table->field[MYSQL_PROXIES_PRIV_GRANTOR]->store(grantor,
                                                        strlen(grantor),
                                                        system_charset_info))
      DBUG_RETURN(TRUE);

    DBUG_RETURN(FALSE);
  }
};

#define FIRST_NON_YN_FIELD 26

class acl_entry :public hash_filo_element
{
public:
  ulong access;
  uint16 length;
  char key[1];					// Key will be stored here
};


static uchar* acl_entry_get_key(acl_entry *entry, size_t *length,
                                my_bool not_used __attribute__((unused)))
{
  *length=(uint) entry->length;
  return (uchar*) entry->key;
}

static uchar* acl_role_get_key(ACL_ROLE *entry, size_t *length,
                               my_bool not_used __attribute__((unused)))
{
  *length=(uint) entry->user.length;
  return (uchar*) entry->user.str;
}

struct ROLE_GRANT_PAIR : public Sql_alloc
{
  char *u_uname;
  char *u_hname;
  char *r_uname;
  LEX_STRING hashkey;
  bool with_admin;

  bool init(MEM_ROOT *mem, char *username, char *hostname, char *rolename,
            bool with_admin_option);
};

static uchar* acl_role_map_get_key(ROLE_GRANT_PAIR *entry, size_t *length,
                                  my_bool not_used __attribute__((unused)))
{
  *length=(uint) entry->hashkey.length;
  return (uchar*) entry->hashkey.str;
}

bool ROLE_GRANT_PAIR::init(MEM_ROOT *mem, char *username,
                           char *hostname, char *rolename,
                           bool with_admin_option)
{
  if (!this)
    return true;

  size_t uname_l = safe_strlen(username);
  size_t hname_l = safe_strlen(hostname);
  size_t rname_l = safe_strlen(rolename);
  /*
    Create a buffer that holds all 3 NULL terminated strings in succession
    To save memory space, the same buffer is used as the hashkey
  */
  size_t bufflen = uname_l + hname_l + rname_l + 3; //add the '\0' aswell
  char *buff= (char *)alloc_root(mem, bufflen);
  if (!buff)
    return true;

  /*
    Offsets in the buffer for all 3 strings
  */
  char *username_pos= buff;
  char *hostname_pos= buff + uname_l + 1;
  char *rolename_pos= buff + uname_l + hname_l + 2;

  if (username) //prevent undefined behaviour
    memcpy(username_pos, username, uname_l);
  username_pos[uname_l]= '\0';         //#1 string terminator
  u_uname= username_pos;

  if (hostname) //prevent undefined behaviour
    memcpy(hostname_pos, hostname, hname_l);
  hostname_pos[hname_l]= '\0';         //#2 string terminator
  u_hname= hostname_pos;

  if (rolename) //prevent undefined behaviour
    memcpy(rolename_pos, rolename, rname_l);
  rolename_pos[rname_l]= '\0';         //#3 string terminator
  r_uname= rolename_pos;

  hashkey.str = buff;
  hashkey.length = bufflen;

  with_admin= with_admin_option;

  return false;
}

#define IP_ADDR_STRLEN (3 + 1 + 3 + 1 + 3 + 1 + 3)
#define ACL_KEY_LENGTH (IP_ADDR_STRLEN + 1 + NAME_LEN + \
                        1 + USERNAME_LENGTH + 1)

#if defined(HAVE_OPENSSL)
/*
  Without SSL the handshake consists of one packet. This packet
  has both client capabilities and scrambled password.
  With SSL the handshake might consist of two packets. If the first
  packet (client capabilities) has CLIENT_SSL flag set, we have to
  switch to SSL and read the second packet. The scrambled password
  is in the second packet and client_capabilities field will be ignored.
  Maybe it is better to accept flags other than CLIENT_SSL from the
  second packet?
*/
#define SSL_HANDSHAKE_SIZE      2
#define MIN_HANDSHAKE_SIZE      2
#else
#define MIN_HANDSHAKE_SIZE      6
#endif /* HAVE_OPENSSL && !EMBEDDED_LIBRARY */
#define NORMAL_HANDSHAKE_SIZE   6

#define ROLE_ASSIGN_COLUMN_IDX  43
#define DEFAULT_ROLE_COLUMN_IDX 44
#define MAX_STATEMENT_TIME_COLUMN_IDX 45

/* various flags valid for ACL_USER */
#define IS_ROLE                 (1L << 0)
/* Flag to mark that a ROLE is on the recursive DEPTH_FIRST_SEARCH stack */
#define ROLE_ON_STACK            (1L << 1)
/*
  Flag to mark that a ROLE and all it's neighbours have
  been visited
*/
#define ROLE_EXPLORED           (1L << 2)
/* Flag to mark that on_node was already called for this role */
#define ROLE_OPENED             (1L << 3)

static DYNAMIC_ARRAY acl_hosts, acl_users, acl_dbs, acl_proxy_users;
static HASH acl_roles;
/*
  An hash containing mappings user <--> role

  A hash is used so as to make updates quickly
  The hashkey used represents all the entries combined
*/
static HASH acl_roles_mappings;
static MEM_ROOT acl_memroot, grant_memroot;
static bool initialized=0;
static bool allow_all_hosts=1;
static HASH acl_check_hosts, column_priv_hash, proc_priv_hash, func_priv_hash;
static DYNAMIC_ARRAY acl_wild_hosts;
static Hash_filo<acl_entry> *acl_cache;
static uint grant_version=0; /* Version of priv tables. incremented by acl_load */
static ulong get_access(TABLE *form,uint fieldnr, uint *next_field=0);
static bool check_is_role(TABLE *form);
static int acl_compare(ACL_ACCESS *a,ACL_ACCESS *b);
static ulong get_sort(uint count,...);
static void init_check_host(void);
static void rebuild_check_host(void);
static void rebuild_role_grants(void);
static ACL_USER *find_user_exact(const char *host, const char *user);
static ACL_USER *find_user_wild(const char *host, const char *user, const char *ip= 0);
static ACL_ROLE *find_acl_role(const char *user);
static ROLE_GRANT_PAIR *find_role_grant_pair(const LEX_STRING *u, const LEX_STRING *h, const LEX_STRING *r);
static ACL_USER_BASE *find_acl_user_base(const char *user, const char *host);
static bool update_user_table(THD *, TABLE *, const char *, const char *, const
                              char *, uint);
static bool acl_load(THD *thd, TABLE_LIST *tables);
static bool grant_load(THD *thd, TABLE_LIST *tables);
static inline void get_grantor(THD *thd, char* grantor);
static bool add_role_user_mapping(const char *uname, const char *hname, const char *rname);

#define ROLE_CYCLE_FOUND 2
static int traverse_role_graph_up(ACL_ROLE *, void *,
                                  int (*) (ACL_ROLE *, void *),
                                  int (*) (ACL_ROLE *, ACL_ROLE *, void *));

static int traverse_role_graph_down(ACL_USER_BASE *, void *,
                             int (*) (ACL_USER_BASE *, void *),
                             int (*) (ACL_USER_BASE *, ACL_ROLE *, void *));

/*
 Enumeration of ACL/GRANT tables in the mysql database
*/
enum enum_acl_tables
{
  USER_TABLE,
  DB_TABLE,
  TABLES_PRIV_TABLE,
  COLUMNS_PRIV_TABLE,
#define FIRST_OPTIONAL_TABLE HOST_TABLE
  HOST_TABLE,
  PROCS_PRIV_TABLE,
  PROXIES_PRIV_TABLE,
  ROLES_MAPPING_TABLE,
  TABLES_MAX // <== always the last
};
// bits for open_grant_tables
static const int Table_user= 1 << USER_TABLE;
static const int Table_db= 1 << DB_TABLE;
static const int Table_tables_priv= 1 << TABLES_PRIV_TABLE;
static const int Table_columns_priv= 1 << COLUMNS_PRIV_TABLE;
static const int Table_host= 1 << HOST_TABLE;
static const int Table_procs_priv= 1 << PROCS_PRIV_TABLE;
static const int Table_proxies_priv= 1 << PROXIES_PRIV_TABLE;
static const int Table_roles_mapping= 1 << ROLES_MAPPING_TABLE;

static int open_grant_tables(THD *thd, TABLE_LIST *tables,
                             enum thr_lock_type lock_type, int tables_to_open);

const LEX_STRING acl_table_names[]=     //  matches enum_acl_tables
{
  { C_STRING_WITH_LEN("user") },
  { C_STRING_WITH_LEN("db") },
  { C_STRING_WITH_LEN("tables_priv") },
  { C_STRING_WITH_LEN("columns_priv") },
  { C_STRING_WITH_LEN("host") },
  { C_STRING_WITH_LEN("procs_priv") },
  { C_STRING_WITH_LEN("proxies_priv") },
  { C_STRING_WITH_LEN("roles_mapping") }
};

/** check if the table was opened, issue an error otherwise */
static int no_such_table(TABLE_LIST *tl)
{
  if (tl->table)
    return 0;

  my_error(ER_NO_SUCH_TABLE, MYF(0), tl->db, tl->alias);
  return 1;
}

/*
 Enumeration of various ACL's and Hashes used in handle_grant_struct()
*/
enum enum_acl_lists
{
  USER_ACL= 0,
  ROLE_ACL,
  DB_ACL,
  COLUMN_PRIVILEGES_HASH,
  PROC_PRIVILEGES_HASH,
  FUNC_PRIVILEGES_HASH,
  PROXY_USERS_ACL,
  ROLES_MAPPINGS_HASH
};

ACL_ROLE::ACL_ROLE(ACL_USER *user, MEM_ROOT *root) : counter(0)
{

  access= user->access;
  /* set initial role access the same as the table row privileges */
  initial_role_access= user->access;
  this->user.str= safe_strdup_root(root, user->user.str);
  this->user.length= user->user.length;
  bzero(&role_grants, sizeof(role_grants));
  bzero(&parent_grantee, sizeof(parent_grantee));
  flags= IS_ROLE;
}

ACL_ROLE::ACL_ROLE(const char * rolename, ulong privileges, MEM_ROOT *root) :
  initial_role_access(privileges), counter(0)
{
  this->access= initial_role_access;
  this->user.str= safe_strdup_root(root, rolename);
  this->user.length= strlen(rolename);
  bzero(&role_grants, sizeof(role_grants));
  bzero(&parent_grantee, sizeof(parent_grantee));
  flags= IS_ROLE;
}


static bool is_invalid_role_name(const char *str)
{
  if (*str && strcasecmp(str, "PUBLIC") && strcasecmp(str, "NONE"))
    return false;

  my_error(ER_INVALID_ROLE, MYF(0), str);
  return true;
}


static void free_acl_user(ACL_USER *user)
{
  delete_dynamic(&(user->role_grants));
}

static void free_acl_role(ACL_ROLE *role)
{
  delete_dynamic(&(role->role_grants));
  delete_dynamic(&(role->parent_grantee));
}

static my_bool check_if_exists(THD *, plugin_ref, void *)
{
  return TRUE;
}

static bool has_validation_plugins()
{
  return plugin_foreach(NULL, check_if_exists,
                        MariaDB_PASSWORD_VALIDATION_PLUGIN, NULL);
}

struct validation_data { LEX_STRING *user, *password; };

static my_bool do_validate(THD *, plugin_ref plugin, void *arg)
{
  struct validation_data *data= (struct validation_data *)arg;
  struct st_mariadb_password_validation *handler=
    (st_mariadb_password_validation *)plugin_decl(plugin)->info;
  return handler->validate_password(data->user, data->password);
}


static bool validate_password(LEX_USER *user)
{
  if (user->pwtext.length || !user->pwhash.length)
  {
    struct validation_data data= { &user->user,
                                   user->pwtext.str ? &user->pwtext :
                                   const_cast<LEX_STRING *>(&empty_lex_str) };
    if (plugin_foreach(NULL, do_validate,
                       MariaDB_PASSWORD_VALIDATION_PLUGIN, &data))
    {
      my_error(ER_NOT_VALID_PASSWORD, MYF(0));
      return true;
    }
  }
  else
  {
    if (strict_password_validation && has_validation_plugins())
    {
      my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--strict-password-validation");
      return true;
    }
  }
  return false;
}

/**
  Convert scrambled password to binary form, according to scramble type,
  Binary form is stored in user.salt.
  
  @param acl_user The object where to store the salt
  @param password The password hash containing the salt
  @param password_len The length of the password hash
   
  Despite the name of the function it is used when loading ACLs from disk
  to store the password hash in the ACL_USER object.
*/

static void
set_user_salt(ACL_USER *acl_user, const char *password, uint password_len)
{
  if (password_len == SCRAMBLED_PASSWORD_CHAR_LENGTH)
  {
    get_salt_from_password(acl_user->salt, password);
    acl_user->salt_len= SCRAMBLE_LENGTH;
  }
  else if (password_len == SCRAMBLED_PASSWORD_CHAR_LENGTH_323)
  {
    get_salt_from_password_323((ulong *) acl_user->salt, password);
    acl_user->salt_len= SCRAMBLE_LENGTH_323;
  }
  else
    acl_user->salt_len= 0;
}

static char *fix_plugin_ptr(char *name)
{
  if (my_strcasecmp(system_charset_info, name,
                    native_password_plugin_name.str) == 0)
    return native_password_plugin_name.str;
  else
  if (my_strcasecmp(system_charset_info, name,
                    old_password_plugin_name.str) == 0)
    return old_password_plugin_name.str;
  else
    return name;
}

/**
  Fix ACL::plugin pointer to point to a hard-coded string, if appropriate

  Make sure that if ACL_USER's plugin is a built-in, then it points
  to a hard coded string, not to an allocated copy. Run-time, for
  authentication, we want to be able to detect built-ins by comparing
  pointers, not strings.

  Additionally - update the salt if the plugin is built-in.

  @retval 0 the pointers were fixed
  @retval 1 this ACL_USER uses a not built-in plugin
*/
static bool fix_user_plugin_ptr(ACL_USER *user)
{
  if (my_strcasecmp(system_charset_info, user->plugin.str,
                    native_password_plugin_name.str) == 0)
    user->plugin= native_password_plugin_name;
  else
  if (my_strcasecmp(system_charset_info, user->plugin.str,
                    old_password_plugin_name.str) == 0)
    user->plugin= old_password_plugin_name;
  else
    return true;

  if (user->auth_string.length)
    set_user_salt(user, user->auth_string.str, user->auth_string.length);
  return false;
}


/*
  Validates the password, calculates password hash, transforms
  equivalent LEX_USER representations.

  Upon entering this function:

  - if user->plugin is specified, user->auth is the plugin auth data.
  - if user->plugin is mysql_native_password or mysql_old_password,
    user->auth is the password hash, and LEX_USER is transformed
    to match the next case (that is, user->plugin is cleared).
  - if user->plugin is NOT specified, built-in auth is assumed, that is
    mysql_native_password or mysql_old_password. In that case,
    user->pwhash is the password hash. And user->pwtext is the original
    plain-text password. Either one can be set or both.

  Upon exiting this function:

  - user->pwtext is left untouched
  - user->pwhash is the password hash, as the mysql.user.password column
  - user->plugin is the plugin name, as the mysql.user.plugin column
  - user->auth is the plugin auth data, as the mysql.user.authentication_string column
*/
static bool fix_lex_user(THD *thd, LEX_USER *user)
{
  size_t check_length;

  DBUG_ASSERT(user->plugin.length || !user->auth.length);
  DBUG_ASSERT(!(user->plugin.length && (user->pwtext.length || user->pwhash.length)));

  if (my_strcasecmp(system_charset_info, user->plugin.str,
                    native_password_plugin_name.str) == 0)
    check_length= SCRAMBLED_PASSWORD_CHAR_LENGTH;
  else
  if (my_strcasecmp(system_charset_info, user->plugin.str,
                    old_password_plugin_name.str) == 0)
    check_length= SCRAMBLED_PASSWORD_CHAR_LENGTH_323;
  else
  if (user->plugin.length)
    return false; // nothing else to do
  else if (thd->variables.old_passwords == 1 ||
           user->pwhash.length == SCRAMBLED_PASSWORD_CHAR_LENGTH_323)
    check_length= SCRAMBLED_PASSWORD_CHAR_LENGTH_323;
  else
    check_length= SCRAMBLED_PASSWORD_CHAR_LENGTH;

  if (user->plugin.length)
  {
    user->pwhash= user->auth;
    user->plugin= empty_lex_str;
    user->auth= empty_lex_str;
  }

  if (user->pwhash.length && user->pwhash.length != check_length)
  {
    my_error(ER_PASSWD_LENGTH, MYF(0), check_length);
    return true;
  }

  if (user->pwtext.length && !user->pwhash.length)
  {
    size_t scramble_length;
    void (*make_scramble)(char *, const char *, size_t);

    if (thd->variables.old_passwords == 1)
    {
      scramble_length= SCRAMBLED_PASSWORD_CHAR_LENGTH_323;
      make_scramble= my_make_scrambled_password_323;
    }
    else
    {
      scramble_length= SCRAMBLED_PASSWORD_CHAR_LENGTH;
      make_scramble= my_make_scrambled_password;
    }

    char *buff= (char *) thd->alloc(scramble_length + 1);
    if (buff == NULL)
      return true;
    make_scramble(buff, user->pwtext.str, user->pwtext.length);
    user->pwhash.str= buff;
    user->pwhash.length= scramble_length;
  }

  return false;
}


static bool get_YN_as_bool(Field *field)
{
  char buff[2];
  String res(buff,sizeof(buff),&my_charset_latin1);
  field->val_str(&res);
  return res[0] == 'Y' || res[0] == 'y';
}


/*
  Initialize structures responsible for user/db-level privilege checking and
  load privilege information for them from tables in the 'mysql' database.

  SYNOPSIS
    acl_init()
      dont_read_acl_tables  TRUE if we want to skip loading data from
                            privilege tables and disable privilege checking.

  NOTES
    This function is mostly responsible for preparatory steps, main work
    on initialization and grants loading is done in acl_reload().

  RETURN VALUES
    0	ok
    1	Could not initialize grant's
*/

bool acl_init(bool dont_read_acl_tables)
{
  THD  *thd;
  bool return_val;
  DBUG_ENTER("acl_init");

  acl_cache= new Hash_filo<acl_entry>(ACL_CACHE_SIZE, 0, 0,
                           (my_hash_get_key) acl_entry_get_key,
                           (my_hash_free_key) free,
                           &my_charset_utf8_bin);

  /*
    cache built-in native authentication plugins,
    to avoid hash searches and a global mutex lock on every connect
  */
  native_password_plugin= my_plugin_lock_by_name(0,
           &native_password_plugin_name, MYSQL_AUTHENTICATION_PLUGIN);
  old_password_plugin= my_plugin_lock_by_name(0,
           &old_password_plugin_name, MYSQL_AUTHENTICATION_PLUGIN);

  if (!native_password_plugin || !old_password_plugin)
    DBUG_RETURN(1);

  if (dont_read_acl_tables)
  {
    DBUG_RETURN(0); /* purecov: tested */
  }

  /*
    To be able to run this from boot, we allocate a temporary THD
  */
  if (!(thd=new THD))
    DBUG_RETURN(1); /* purecov: inspected */
  thd->thread_stack= (char*) &thd;
  thd->store_globals();
  /*
    It is safe to call acl_reload() since acl_* arrays and hashes which
    will be freed there are global static objects and thus are initialized
    by zeros at startup.
  */
  return_val= acl_reload(thd);
  delete thd;
  DBUG_RETURN(return_val);
}

/**
  Choose from either native or old password plugins when assigning a password
*/

static bool set_user_plugin (ACL_USER *user, int password_len)
{
  switch (password_len)
  {
  case 0: /* no password */
  case SCRAMBLED_PASSWORD_CHAR_LENGTH:
    user->plugin= native_password_plugin_name;
    return FALSE;
  case SCRAMBLED_PASSWORD_CHAR_LENGTH_323:
    user->plugin= old_password_plugin_name;
    return FALSE;
  default:
    sql_print_warning("Found invalid password for user: '%s@%s'; "
                      "Ignoring user", safe_str(user->user.str),
                      safe_str(user->host.hostname));
    return TRUE;
  }
}


/*
  Initialize structures responsible for user/db-level privilege checking
  and load information about grants from open privilege tables.

  SYNOPSIS
    acl_load()
      thd     Current thread
      tables  List containing open "mysql.host", "mysql.user",
              "mysql.db", "mysql.proxies_priv" and "mysql.roles_mapping"
              tables.

  RETURN VALUES
    FALSE  Success
    TRUE   Error
*/

static bool acl_load(THD *thd, TABLE_LIST *tables)
{
  TABLE *table;
  READ_RECORD read_record_info;
  bool return_val= TRUE;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  char tmp_name[SAFE_NAME_LEN+1];
  int password_length;
  ulonglong old_sql_mode= thd->variables.sql_mode;
  DBUG_ENTER("acl_load");

  thd->variables.sql_mode&= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  grant_version++; /* Privileges updated */

  init_sql_alloc(&acl_memroot, ACL_ALLOC_BLOCK_SIZE, 0, MYF(0));
  if ((table= tables[HOST_TABLE].table)) // "host" table may not exist (e.g. in MySQL 5.6.7+)
  {
    if (init_read_record(&read_record_info, thd, table, NULL, 1, 1, FALSE))
      goto end;
    table->use_all_columns();
    while (!(read_record_info.read_record(&read_record_info)))
    {
      ACL_HOST host;
      update_hostname(&host.host,get_field(&acl_memroot, table->field[0]));
      host.db=	 get_field(&acl_memroot, table->field[1]);
      if (lower_case_table_names && host.db)
      {
        /*
          convert db to lower case and give a warning if the db wasn't
          already in lower case
        */
        char *end = strnmov(tmp_name, host.db, sizeof(tmp_name));
        if (end >= tmp_name + sizeof(tmp_name))
        {
          sql_print_warning(ER_THD(thd, ER_WRONG_DB_NAME), host.db);
          continue;
        }
        my_casedn_str(files_charset_info, host.db);
        if (strcmp(host.db, tmp_name) != 0)
          sql_print_warning("'host' entry '%s|%s' had database in mixed "
                            "case that has been forced to lowercase because "
                            "lower_case_table_names is set. It will not be "
                            "possible to remove this privilege using REVOKE.",
                            host.host.hostname, host.db);
      }
      host.access= get_access(table,2);
      host.access= fix_rights_for_db(host.access);
      host.sort=	 get_sort(2,host.host.hostname,host.db);
      if (check_no_resolve && hostname_requires_resolving(host.host.hostname))
      {
        sql_print_warning("'host' entry '%s|%s' "
                        "ignored in --skip-name-resolve mode.",
                         safe_str(host.host.hostname),
                         safe_str(host.db));
        continue;
      }
#ifndef TO_BE_REMOVED
      if (table->s->fields == 8)
      {						// Without grant
        if (host.access & CREATE_ACL)
          host.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL | CREATE_TMP_ACL;
      }
#endif
      (void) push_dynamic(&acl_hosts,(uchar*) &host);
    }
    my_qsort((uchar*) dynamic_element(&acl_hosts,0,ACL_HOST*),acl_hosts.elements,
             sizeof(ACL_HOST),(qsort_cmp) acl_compare);
    end_read_record(&read_record_info);
  }
  freeze_size(&acl_hosts);

  if (init_read_record(&read_record_info, thd, table=tables[USER_TABLE].table,
                       NULL, 1, 1, FALSE))
    goto end;
  table->use_all_columns();

  username_char_length= MY_MIN(table->field[1]->char_length(),
                               USERNAME_CHAR_LENGTH);
  password_length= table->field[2]->field_length /
    table->field[2]->charset()->mbmaxlen;
  if (password_length < SCRAMBLED_PASSWORD_CHAR_LENGTH_323)
  {
    sql_print_error("Fatal error: mysql.user table is damaged or in "
                    "unsupported 3.20 format.");
    goto end;
  }

  DBUG_PRINT("info",("user table fields: %d, password length: %d",
		     table->s->fields, password_length));

  mysql_mutex_lock(&LOCK_global_system_variables);
  if (password_length < SCRAMBLED_PASSWORD_CHAR_LENGTH)
  {
    if (opt_secure_auth)
    {
      mysql_mutex_unlock(&LOCK_global_system_variables);
      sql_print_error("Fatal error: mysql.user table is in old format, "
                      "but server started with --secure-auth option.");
      goto end;
    }
    mysql_user_table_is_in_short_password_format= true;
    if (global_system_variables.old_passwords)
      mysql_mutex_unlock(&LOCK_global_system_variables);
    else
    {
      extern sys_var *Sys_old_passwords_ptr;
      Sys_old_passwords_ptr->value_origin= sys_var::AUTO;
      global_system_variables.old_passwords= 1;
      mysql_mutex_unlock(&LOCK_global_system_variables);
      sql_print_warning("mysql.user table is not updated to new password format; "
                        "Disabling new password usage until "
                        "mysql_fix_privilege_tables is run");
    }
    thd->variables.old_passwords= 1;
  }
  else
  {
    mysql_user_table_is_in_short_password_format= false;
    mysql_mutex_unlock(&LOCK_global_system_variables);
  }

  allow_all_hosts=0;
  while (!(read_record_info.read_record(&read_record_info)))
  {
    ACL_USER user;
    bool is_role= FALSE;
    bzero(&user, sizeof(user));
    update_hostname(&user.host, get_field(&acl_memroot, table->field[0]));
    char *username= get_field(&acl_memroot, table->field[1]);
    user.user.str= username;
    user.user.length= safe_strlen(username);

    /*
       If the user entry is a role, skip password and hostname checks
       A user can not log in with a role so some checks are not necessary
    */
    is_role= check_is_role(table);

    if (is_role && is_invalid_role_name(username))
    {
      thd->clear_error(); // the warning is still issued
      continue;
    }

    if (!is_role && check_no_resolve &&
        hostname_requires_resolving(user.host.hostname))
    {
      sql_print_warning("'user' entry '%s@%s' "
                        "ignored in --skip-name-resolve mode.",
                        safe_str(user.user.str),
                        safe_str(user.host.hostname));
      continue;
    }

    char *password= get_field(&acl_memroot, table->field[2]);
    uint password_len= safe_strlen(password);
    user.auth_string.str= safe_str(password);
    user.auth_string.length= password_len;
    set_user_salt(&user, password, password_len);

    if (!is_role && set_user_plugin(&user, password_len))
      continue;
    
    {
      uint next_field;
      user.access= get_access(table,3,&next_field) & GLOBAL_ACLS;
      /*
        if it is pre 5.0.1 privilege table then map CREATE privilege on
        CREATE VIEW & SHOW VIEW privileges
      */
      if (table->s->fields <= 31 && (user.access & CREATE_ACL))
        user.access|= (CREATE_VIEW_ACL | SHOW_VIEW_ACL);

      /*
        if it is pre 5.0.2 privilege table then map CREATE/ALTER privilege on
        CREATE PROCEDURE & ALTER PROCEDURE privileges
      */
      if (table->s->fields <= 33 && (user.access & CREATE_ACL))
        user.access|= CREATE_PROC_ACL;
      if (table->s->fields <= 33 && (user.access & ALTER_ACL))
        user.access|= ALTER_PROC_ACL;

      /*
        pre 5.0.3 did not have CREATE_USER_ACL
      */
      if (table->s->fields <= 36 && (user.access & GRANT_ACL))
        user.access|= CREATE_USER_ACL;


      /*
        if it is pre 5.1.6 privilege table then map CREATE privilege on
        CREATE|ALTER|DROP|EXECUTE EVENT
      */
      if (table->s->fields <= 37 && (user.access & SUPER_ACL))
        user.access|= EVENT_ACL;

      /*
        if it is pre 5.1.6 privilege then map TRIGGER privilege on CREATE.
      */
      if (table->s->fields <= 38 && (user.access & SUPER_ACL))
        user.access|= TRIGGER_ACL;

      user.sort= get_sort(2, user.host.hostname, user.user.str);
      user.hostname_length= safe_strlen(user.host.hostname);
      user.user_resource.user_conn= 0;
      user.user_resource.max_statement_time= 0.0;

      /* Starting from 4.0.2 we have more fields */
      if (table->s->fields >= 31)
      {
        char *ssl_type=get_field(thd->mem_root, table->field[next_field++]);
        if (!ssl_type)
          user.ssl_type=SSL_TYPE_NONE;
        else if (!strcmp(ssl_type, "ANY"))
          user.ssl_type=SSL_TYPE_ANY;
        else if (!strcmp(ssl_type, "X509"))
          user.ssl_type=SSL_TYPE_X509;
        else  /* !strcmp(ssl_type, "SPECIFIED") */
          user.ssl_type=SSL_TYPE_SPECIFIED;

        user.ssl_cipher=   get_field(&acl_memroot, table->field[next_field++]);
        user.x509_issuer=  get_field(&acl_memroot, table->field[next_field++]);
        user.x509_subject= get_field(&acl_memroot, table->field[next_field++]);

        char *ptr = get_field(thd->mem_root, table->field[next_field++]);
        user.user_resource.questions=ptr ? atoi(ptr) : 0;
        ptr = get_field(thd->mem_root, table->field[next_field++]);
        user.user_resource.updates=ptr ? atoi(ptr) : 0;
        ptr = get_field(thd->mem_root, table->field[next_field++]);
        user.user_resource.conn_per_hour= ptr ? atoi(ptr) : 0;
        if (user.user_resource.questions || user.user_resource.updates ||
            user.user_resource.conn_per_hour)
          mqh_used=1;

        if (table->s->fields >= 36)
        {
          /* Starting from 5.0.3 we have max_user_connections field */
          ptr= get_field(thd->mem_root, table->field[next_field++]);
          user.user_resource.user_conn= ptr ? atoi(ptr) : 0;
        }

        if (!is_role && table->s->fields >= 41)
        {
          /* We may have plugin & auth_String fields */
          char *tmpstr= get_field(&acl_memroot, table->field[next_field++]);
          if (tmpstr)
          {
            user.plugin.str= tmpstr;
            user.plugin.length= strlen(user.plugin.str);
            user.auth_string.str=
              safe_str(get_field(&acl_memroot, table->field[next_field++]));
            user.auth_string.length= strlen(user.auth_string.str);

            if (user.auth_string.length && password_len)
            {
              sql_print_warning("'user' entry '%s@%s' has both a password "
                                "and an authentication plugin specified. The "
                                "password will be ignored.",
                                safe_str(user.user.str),
                                safe_str(user.host.hostname));
            }

            fix_user_plugin_ptr(&user);
          }
        }

        if (table->s->fields > MAX_STATEMENT_TIME_COLUMN_IDX)
        {
          /* Starting from 10.1.1 we can have max_statement_time */
          ptr= get_field(thd->mem_root,
                         table->field[MAX_STATEMENT_TIME_COLUMN_IDX]);
          user.user_resource.max_statement_time= ptr ? atof(ptr) : 0.0;
        }
      }
      else
      {
        user.ssl_type=SSL_TYPE_NONE;
#ifndef TO_BE_REMOVED
        if (table->s->fields <= 13)
        {						// Without grant
          if (user.access & CREATE_ACL)
            user.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL;
        }
        /* Convert old privileges */
        user.access|= LOCK_TABLES_ACL | CREATE_TMP_ACL | SHOW_DB_ACL;
        if (user.access & FILE_ACL)
          user.access|= REPL_CLIENT_ACL | REPL_SLAVE_ACL;
        if (user.access & PROCESS_ACL)
          user.access|= SUPER_ACL | EXECUTE_ACL;
#endif
      }

      (void) my_init_dynamic_array(&user.role_grants,sizeof(ACL_ROLE *),
                                   8, 8, MYF(0));

      /* check default role, if any */
      if (!is_role && table->s->fields > DEFAULT_ROLE_COLUMN_IDX)
      {
        user.default_rolename.str=
          get_field(&acl_memroot, table->field[DEFAULT_ROLE_COLUMN_IDX]);
        user.default_rolename.length= safe_strlen(user.default_rolename.str);
      }

      if (is_role)
      {
        DBUG_PRINT("info", ("Found role %s", user.user.str));
        ACL_ROLE *entry= new (&acl_memroot) ACL_ROLE(&user, &acl_memroot);
        entry->role_grants = user.role_grants;
        (void) my_init_dynamic_array(&entry->parent_grantee,
                                     sizeof(ACL_USER_BASE *), 8, 8, MYF(0));
        my_hash_insert(&acl_roles, (uchar *)entry);

        continue;
      }
      else
      {
        DBUG_PRINT("info", ("Found user %s", user.user.str));
        (void) push_dynamic(&acl_users,(uchar*) &user);
      }
      if (!user.host.hostname ||
	  (user.host.hostname[0] == wild_many && !user.host.hostname[1]))
        allow_all_hosts=1;			// Anyone can connect
    }
  }
  my_qsort((uchar*) dynamic_element(&acl_users,0,ACL_USER*),acl_users.elements,
	   sizeof(ACL_USER),(qsort_cmp) acl_compare);
  end_read_record(&read_record_info);
  freeze_size(&acl_users);

  if (init_read_record(&read_record_info, thd, table=tables[DB_TABLE].table,
                       NULL, 1, 1, FALSE))
    goto end;
  table->use_all_columns();
  while (!(read_record_info.read_record(&read_record_info)))
  {
    ACL_DB db;
    db.user=get_field(&acl_memroot, table->field[MYSQL_DB_FIELD_USER]);
    const char *hostname= get_field(&acl_memroot, table->field[MYSQL_DB_FIELD_HOST]);
    if (!hostname && find_acl_role(db.user))
      hostname= "";
    update_hostname(&db.host, hostname);
    db.db=get_field(&acl_memroot, table->field[MYSQL_DB_FIELD_DB]);
    if (!db.db)
    {
      sql_print_warning("Found an entry in the 'db' table with empty database name; Skipped");
      continue;
    }
    if (check_no_resolve && hostname_requires_resolving(db.host.hostname))
    {
      sql_print_warning("'db' entry '%s %s@%s' "
		        "ignored in --skip-name-resolve mode.",
		        db.db, safe_str(db.user), safe_str(db.host.hostname));
      continue;
    }
    db.access=get_access(table,3);
    db.access=fix_rights_for_db(db.access);
    db.initial_access= db.access;
    if (lower_case_table_names)
    {
      /*
        convert db to lower case and give a warning if the db wasn't
        already in lower case
      */
      char *end = strnmov(tmp_name, db.db, sizeof(tmp_name));
      if (end >= tmp_name + sizeof(tmp_name))
      {
        sql_print_warning(ER_THD(thd, ER_WRONG_DB_NAME), db.db);
        continue;
      }
      my_casedn_str(files_charset_info, db.db);
      if (strcmp(db.db, tmp_name) != 0)
      {
        sql_print_warning("'db' entry '%s %s@%s' had database in mixed "
                          "case that has been forced to lowercase because "
                          "lower_case_table_names is set. It will not be "
                          "possible to remove this privilege using REVOKE.",
		          db.db, safe_str(db.user), safe_str(db.host.hostname));
      }
    }
    db.sort=get_sort(3,db.host.hostname,db.db,db.user);
#ifndef TO_BE_REMOVED
    if (table->s->fields <=  9)
    {						// Without grant
      if (db.access & CREATE_ACL)
	db.access|=REFERENCES_ACL | INDEX_ACL | ALTER_ACL;
    }
#endif
    (void) push_dynamic(&acl_dbs,(uchar*) &db);
  }
  my_qsort((uchar*) dynamic_element(&acl_dbs,0,ACL_DB*),acl_dbs.elements,
	   sizeof(ACL_DB),(qsort_cmp) acl_compare);
  end_read_record(&read_record_info);
  freeze_size(&acl_dbs);

  if ((table= tables[PROXIES_PRIV_TABLE].table))
  {
    if (init_read_record(&read_record_info, thd, table,
                         NULL, 1, 1, FALSE))
      goto end;
    table->use_all_columns();
    while (!(read_record_info.read_record(&read_record_info)))
    {
      ACL_PROXY_USER proxy;
      proxy.init(table, &acl_memroot);
      if (proxy.check_validity(check_no_resolve))
        continue;
      if (push_dynamic(&acl_proxy_users, (uchar*) &proxy))
      {
        end_read_record(&read_record_info);
        goto end;
      }
    }
    my_qsort((uchar*) dynamic_element(&acl_proxy_users, 0, ACL_PROXY_USER*),
             acl_proxy_users.elements,
             sizeof(ACL_PROXY_USER), (qsort_cmp) acl_compare);
    end_read_record(&read_record_info);
  }
  else
  {
    sql_print_error("Missing system table mysql.proxies_priv; "
                    "please run mysql_upgrade to create it");
  }
  freeze_size(&acl_proxy_users);

  if ((table= tables[ROLES_MAPPING_TABLE].table))
  {
    if (init_read_record(&read_record_info, thd, table, NULL, 1, 1, FALSE))
      goto end;
    table->use_all_columns();

    MEM_ROOT temp_root;
    init_alloc_root(&temp_root, ACL_ALLOC_BLOCK_SIZE, 0, MYF(0));
    while (!(read_record_info.read_record(&read_record_info)))
    {
      char *hostname= safe_str(get_field(&temp_root, table->field[0]));
      char *username= safe_str(get_field(&temp_root, table->field[1]));
      char *rolename= safe_str(get_field(&temp_root, table->field[2]));
      bool with_grant_option= get_YN_as_bool(table->field[3]);

      if (add_role_user_mapping(username, hostname, rolename)) {
        sql_print_error("Invalid roles_mapping table entry user:'%s@%s', rolename:'%s'",
                        username, hostname, rolename);
        continue;
      }

      ROLE_GRANT_PAIR *mapping= new (&acl_memroot) ROLE_GRANT_PAIR;

      if (mapping->init(&acl_memroot, username, hostname, rolename, with_grant_option))
        continue;

      my_hash_insert(&acl_roles_mappings, (uchar*) mapping);
    }

    free_root(&temp_root, MYF(0));
    end_read_record(&read_record_info);
  }
  else
  {
    sql_print_error("Missing system table mysql.roles_mapping; "
                    "please run mysql_upgrade to create it");
  }

  init_check_host();

  initialized=1;
  return_val= FALSE;

end:
  end_read_record(&read_record_info);
  thd->variables.sql_mode= old_sql_mode;
  DBUG_RETURN(return_val);
}


void acl_free(bool end)
{
  my_hash_free(&acl_roles);
  free_root(&acl_memroot,MYF(0));
  delete_dynamic(&acl_hosts);
  delete_dynamic_with_callback(&acl_users, (FREE_FUNC) free_acl_user);
  delete_dynamic(&acl_dbs);
  delete_dynamic(&acl_wild_hosts);
  delete_dynamic(&acl_proxy_users);
  my_hash_free(&acl_check_hosts);
  my_hash_free(&acl_roles_mappings);
  if (!end)
    acl_cache->clear(1); /* purecov: inspected */
  else
  {
    plugin_unlock(0, native_password_plugin);
    plugin_unlock(0, old_password_plugin);
    delete acl_cache;
    acl_cache=0;
  }
}


/*
  Forget current user/db-level privileges and read new privileges
  from the privilege tables.

  SYNOPSIS
    acl_reload()
      thd  Current thread

  NOTE
    All tables of calling thread which were open and locked by LOCK TABLES
    statement will be unlocked and closed.
    This function is also used for initialization of structures responsible
    for user/db-level privilege checking.

  RETURN VALUE
    FALSE  Success
    TRUE   Failure
*/

bool acl_reload(THD *thd)
{
  TABLE_LIST tables[TABLES_MAX];
  DYNAMIC_ARRAY old_acl_hosts, old_acl_users, old_acl_dbs, old_acl_proxy_users;
  HASH old_acl_roles, old_acl_roles_mappings;
  MEM_ROOT old_mem;
  int result;
  DBUG_ENTER("acl_reload");

  /*
    To avoid deadlocks we should obtain table locks before
    obtaining acl_cache->lock mutex.
  */
  if ((result= open_grant_tables(thd, tables, TL_READ, Table_host |
            Table_user | Table_db | Table_proxies_priv | Table_roles_mapping)))
  {
    DBUG_ASSERT(result <= 0);
    /*
      Execution might have been interrupted; only print the error message
      if an error condition has been raised.
    */
    if (thd->get_stmt_da()->is_error())
      sql_print_error("Fatal error: Can't open and lock privilege tables: %s",
                      thd->get_stmt_da()->message());
    goto end;
  }

  acl_cache->clear(0);
  mysql_mutex_lock(&acl_cache->lock);

  old_acl_hosts= acl_hosts;
  old_acl_users= acl_users;
  old_acl_roles= acl_roles;
  old_acl_roles_mappings= acl_roles_mappings;
  old_acl_proxy_users= acl_proxy_users;
  old_acl_dbs= acl_dbs;
  my_init_dynamic_array(&acl_hosts, sizeof(ACL_HOST), 20, 50, MYF(0));
  my_init_dynamic_array(&acl_users, sizeof(ACL_USER), 50, 100, MYF(0));
  my_init_dynamic_array(&acl_dbs, sizeof(ACL_DB), 50, 100, MYF(0));
  my_init_dynamic_array(&acl_proxy_users, sizeof(ACL_PROXY_USER), 50, 100, MYF(0));
  my_hash_init2(&acl_roles,50, &my_charset_utf8_bin,
                0, 0, 0, (my_hash_get_key) acl_role_get_key, 0,
                (void (*)(void *))free_acl_role, 0);
  my_hash_init2(&acl_roles_mappings, 50, system_charset_info, 0, 0, 0,
                (my_hash_get_key) acl_role_map_get_key, 0, 0, 0);
  old_mem= acl_memroot;
  delete_dynamic(&acl_wild_hosts);
  my_hash_free(&acl_check_hosts);

  if ((result= acl_load(thd, tables)))
  {					// Error. Revert to old list
    DBUG_PRINT("error",("Reverting to old privileges"));
    acl_free();				/* purecov: inspected */
    acl_hosts= old_acl_hosts;
    acl_users= old_acl_users;
    acl_roles= old_acl_roles;
    acl_roles_mappings= old_acl_roles_mappings;
    acl_proxy_users= old_acl_proxy_users;
    acl_dbs= old_acl_dbs;
    acl_memroot= old_mem;
    init_check_host();
  }
  else
  {
    my_hash_free(&old_acl_roles);
    free_root(&old_mem,MYF(0));
    delete_dynamic(&old_acl_hosts);
    delete_dynamic_with_callback(&old_acl_users, (FREE_FUNC) free_acl_user);
    delete_dynamic(&old_acl_proxy_users);
    delete_dynamic(&old_acl_dbs);
    my_hash_free(&old_acl_roles_mappings);
  }
  mysql_mutex_unlock(&acl_cache->lock);
end:
  close_mysql_tables(thd);
  DBUG_RETURN(result);
}

/*
  Get all access bits from table after fieldnr

  IMPLEMENTATION
  We know that the access privileges ends when there is no more fields
  or the field is not an enum with two elements.

  SYNOPSIS
    get_access()
    form        an open table to read privileges from.
                The record should be already read in table->record[0]
    fieldnr     number of the first privilege (that is ENUM('N','Y') field
    next_field  on return - number of the field next to the last ENUM
                (unless next_field == 0)

  RETURN VALUE
    privilege mask
*/

static ulong get_access(TABLE *form, uint fieldnr, uint *next_field)
{
  ulong access_bits=0,bit;
  char buff[2];
  String res(buff,sizeof(buff),&my_charset_latin1);
  Field **pos;

  for (pos=form->field+fieldnr, bit=1;
       *pos && (*pos)->real_type() == MYSQL_TYPE_ENUM &&
	 ((Field_enum*) (*pos))->typelib->count == 2 ;
       pos++, fieldnr++, bit<<=1)
  {
    if (get_YN_as_bool(*pos))
      access_bits|= bit;
  }
  if (next_field)
    *next_field=fieldnr;
  return access_bits;
}

/*
  Check if a user entry in the user table is marked as being a role entry

  IMPLEMENTATION
  Access the coresponding column and check the coresponding ENUM of the form
  ENUM('N', 'Y')

  SYNOPSIS
    check_is_role()
    form      an open table to read the entry from.
              The record should be already read in table->record[0]

  RETURN VALUE
    TRUE      if the user is marked as a role
    FALSE     otherwise
*/

static bool check_is_role(TABLE *form)
{
  char buff[2];
  String res(buff, sizeof(buff), &my_charset_latin1);
  /* Table version does not support roles */
  if (form->s->fields <= ROLE_ASSIGN_COLUMN_IDX)
    return FALSE;

  return get_YN_as_bool(form->field[ROLE_ASSIGN_COLUMN_IDX]);
}


/*
  Return a number which, if sorted 'desc', puts strings in this order:
    no wildcards
    wildcards
    empty string
*/

static ulong get_sort(uint count,...)
{
  va_list args;
  va_start(args,count);
  ulong sort=0;

  /* Should not use this function with more than 4 arguments for compare. */
  DBUG_ASSERT(count <= 4);

  while (count--)
  {
    char *start, *str= va_arg(args,char*);
    uint chars= 0;
    uint wild_pos= 0;           /* first wildcard position */

    if ((start= str))
    {
      for (; *str ; str++)
      {
        if (*str == wild_prefix && str[1])
          str++;
        else if (*str == wild_many || *str == wild_one)
        {
          wild_pos= (uint) (str - start) + 1;
          break;
        }
        chars= 128;                             // Marker that chars existed
      }
    }
    sort= (sort << 8) + (wild_pos ? MY_MIN(wild_pos, 127U) : chars);
  }
  va_end(args);
  return sort;
}


static int acl_compare(ACL_ACCESS *a,ACL_ACCESS *b)
{
  if (a->sort > b->sort)
    return -1;
  if (a->sort < b->sort)
    return 1;
  return 0;
}


/*
  Gets user credentials without authentication and resource limit checks.

  SYNOPSIS
    acl_getroot()
      sctx               Context which should be initialized
      user               user name
      host               host name
      ip                 IP
      db                 current data base name

  RETURN
    FALSE  OK
    TRUE   Error
*/

bool acl_getroot(Security_context *sctx, char *user, char *host,
                 char *ip, char *db)
{
  int res= 1;
  uint i;
  ACL_USER *acl_user= 0;
  DBUG_ENTER("acl_getroot");

  DBUG_PRINT("enter", ("Host: '%s', Ip: '%s', User: '%s', db: '%s'",
                       host, ip, user, db));
  sctx->user= user;
  sctx->host= host;
  sctx->ip= ip;
  sctx->host_or_ip= host ? host : (safe_str(ip));

  if (!initialized)
  {
    /*
      here if mysqld's been started with --skip-grant-tables option.
    */
    sctx->skip_grants();
    DBUG_RETURN(FALSE);
  }

  mysql_mutex_lock(&acl_cache->lock);

  sctx->master_access= 0;
  sctx->db_access= 0;
  *sctx->priv_user= *sctx->priv_host= *sctx->priv_role= 0;

  if (host[0]) // User, not Role
  {
    acl_user= find_user_wild(host, user, ip);

    if (acl_user)
    {
      res= 0;
      for (i=0 ; i < acl_dbs.elements ; i++)
      {
        ACL_DB *acl_db= dynamic_element(&acl_dbs, i, ACL_DB*);
        if (!acl_db->user ||
            (user && user[0] && !strcmp(user, acl_db->user)))
        {
          if (compare_hostname(&acl_db->host, host, ip))
          {
            if (!acl_db->db || (db && !wild_compare(db, acl_db->db, 0)))
            {
              sctx->db_access= acl_db->access;
              break;
            }
          }
        }
      }
      sctx->master_access= acl_user->access;

      if (acl_user->user.str)
        strmake_buf(sctx->priv_user, user);

      if (acl_user->host.hostname)
        strmake_buf(sctx->priv_host, acl_user->host.hostname);
    }
  }
  else // Role, not User
  {
    ACL_ROLE *acl_role= find_acl_role(user);
    if (acl_role)
    {
      res= 0;
      for (i=0 ; i < acl_dbs.elements ; i++)
      {
        ACL_DB *acl_db= dynamic_element(&acl_dbs, i, ACL_DB*);
        if (!acl_db->user ||
            (user && user[0] && !strcmp(user, acl_db->user)))
        {
          if (compare_hostname(&acl_db->host, "", ""))
          {
            if (!acl_db->db || (db && !wild_compare(db, acl_db->db, 0)))
            {
              sctx->db_access= acl_db->access;
              break;
            }
          }
        }
      }
      sctx->master_access= acl_role->access;

      if (acl_role->user.str)
        strmake_buf(sctx->priv_user, user);
      sctx->priv_host[0]= 0;
    }
  }

  mysql_mutex_unlock(&acl_cache->lock);
  DBUG_RETURN(res);
}

static int check_user_can_set_role(const char *user, const char *host,
                      const char *ip, const char *rolename, ulonglong *access)
{
  ACL_ROLE *role;
  ACL_USER_BASE *acl_user_base;
  ACL_USER *UNINIT_VAR(acl_user);
  bool is_granted= FALSE;
  int result= 0;

  /* clear role privileges */
  mysql_mutex_lock(&acl_cache->lock);

  if (!strcasecmp(rolename, "NONE"))
  {
    /* have to clear the privileges */
    /* get the current user */
    acl_user= find_user_wild(host, user, ip);
    if (acl_user == NULL)
    {
      my_error(ER_INVALID_CURRENT_USER, MYF(0), rolename);
      result= -1;
    }
    else if (access)
      *access= acl_user->access;

    goto end;
  }

  role= find_acl_role(rolename);

  /* According to SQL standard, the same error message must be presented */
  if (role == NULL) {
    my_error(ER_INVALID_ROLE, MYF(0), rolename);
    result= -1;
    goto end;
  }

  for (uint i=0 ; i < role->parent_grantee.elements ; i++)
  {
    acl_user_base= *(dynamic_element(&role->parent_grantee, i, ACL_USER_BASE**));
    if (acl_user_base->flags & IS_ROLE)
      continue;

    acl_user= (ACL_USER *)acl_user_base;
    if (acl_user->wild_eq(user, host, ip))
    {
      is_granted= TRUE;
      break;
    }
  }

  /* According to SQL standard, the same error message must be presented */
  if (!is_granted)
  {
    my_error(ER_INVALID_ROLE, MYF(0), rolename);
    result= 1;
    goto end;
  }

  if (access)
  {
    *access = acl_user->access | role->access;
  }
end:
  mysql_mutex_unlock(&acl_cache->lock);
  return result;

}

int acl_check_setrole(THD *thd, char *rolename, ulonglong *access)
{
    /* Yes! priv_user@host. Don't ask why - that's what check_access() does. */
  return check_user_can_set_role(thd->security_ctx->priv_user,
        thd->security_ctx->host, thd->security_ctx->ip, rolename, access);
}


int acl_setrole(THD *thd, char *rolename, ulonglong access)
{
  /* merge the privileges */
  Security_context *sctx= thd->security_ctx;
  sctx->master_access= static_cast<ulong>(access);
  if (thd->db)
    sctx->db_access= acl_get(sctx->host, sctx->ip, sctx->user, thd->db, FALSE);

  if (!strcasecmp(rolename, "NONE"))
  {
    thd->security_ctx->priv_role[0]= 0;
  }
  else
  {
    if (thd->db)
      sctx->db_access|= acl_get("", "", rolename, thd->db, FALSE);
    /* mark the current role */
    strmake_buf(thd->security_ctx->priv_role, rolename);
  }
  return 0;
}

static uchar* check_get_key(ACL_USER *buff, size_t *length,
                            my_bool not_used __attribute__((unused)))
{
  *length=buff->hostname_length;
  return (uchar*) buff->host.hostname;
}


static void acl_update_role(const char *rolename, ulong privileges)
{
  ACL_ROLE *role= find_acl_role(rolename);
  if (role)
    role->initial_role_access= role->access= privileges;
}


static void acl_update_user(const char *user, const char *host,
			    const char *password, uint password_len,
			    enum SSL_type ssl_type,
			    const char *ssl_cipher,
			    const char *x509_issuer,
			    const char *x509_subject,
			    USER_RESOURCES  *mqh,
			    ulong privileges,
			    const LEX_STRING *plugin,
			    const LEX_STRING *auth)
{
  mysql_mutex_assert_owner(&acl_cache->lock);

  for (uint i=0 ; i < acl_users.elements ; i++)
  {
    ACL_USER *acl_user=dynamic_element(&acl_users,i,ACL_USER*);
    if (acl_user->eq(user, host))
    {
      if (plugin->str[0])
      {
        acl_user->plugin= *plugin;
        acl_user->auth_string.str= auth->str ?
          strmake_root(&acl_memroot, auth->str, auth->length) : const_cast<char*>("");
        acl_user->auth_string.length= auth->length;
        if (fix_user_plugin_ptr(acl_user))
          acl_user->plugin.str= strmake_root(&acl_memroot, plugin->str, plugin->length);
      }
      else
        if (password[0])
        {
          acl_user->auth_string.str= strmake_root(&acl_memroot, password, password_len);
          acl_user->auth_string.length= password_len;
          set_user_salt(acl_user, password, password_len);
          set_user_plugin(acl_user, password_len);
        }
      acl_user->access=privileges;
      if (mqh->specified_limits & USER_RESOURCES::QUERIES_PER_HOUR)
        acl_user->user_resource.questions=mqh->questions;
      if (mqh->specified_limits & USER_RESOURCES::UPDATES_PER_HOUR)
        acl_user->user_resource.updates=mqh->updates;
      if (mqh->specified_limits & USER_RESOURCES::CONNECTIONS_PER_HOUR)
        acl_user->user_resource.conn_per_hour= mqh->conn_per_hour;
      if (mqh->specified_limits & USER_RESOURCES::USER_CONNECTIONS)
        acl_user->user_resource.user_conn= mqh->user_conn;
      if (mqh->specified_limits & USER_RESOURCES::MAX_STATEMENT_TIME)
        acl_user->user_resource.max_statement_time= mqh->max_statement_time;
      if (ssl_type != SSL_TYPE_NOT_SPECIFIED)
      {
        acl_user->ssl_type= ssl_type;
        acl_user->ssl_cipher= (ssl_cipher ? strdup_root(&acl_memroot,ssl_cipher) :
                               0);
        acl_user->x509_issuer= (x509_issuer ? strdup_root(&acl_memroot,x509_issuer) :
                                0);
        acl_user->x509_subject= (x509_subject ?
                                 strdup_root(&acl_memroot,x509_subject) : 0);
      }
      /* search complete: */
      break;
    }
  }
}


static void acl_insert_role(const char *rolename, ulong privileges)
{
  ACL_ROLE *entry;

  mysql_mutex_assert_owner(&acl_cache->lock);
  entry= new (&acl_memroot) ACL_ROLE(rolename, privileges, &acl_memroot);
  (void) my_init_dynamic_array(&entry->parent_grantee,
                               sizeof(ACL_USER_BASE *), 8, 8, MYF(0));
  (void) my_init_dynamic_array(&entry->role_grants,sizeof(ACL_ROLE *),
                               8, 8, MYF(0));

  my_hash_insert(&acl_roles, (uchar *)entry);
}


static void acl_insert_user(const char *user, const char *host,
			    const char *password, uint password_len,
			    enum SSL_type ssl_type,
			    const char *ssl_cipher,
			    const char *x509_issuer,
			    const char *x509_subject,
			    USER_RESOURCES *mqh,
			    ulong privileges,
			    const LEX_STRING *plugin,
			    const LEX_STRING *auth)
{
  ACL_USER acl_user;

  mysql_mutex_assert_owner(&acl_cache->lock);

  bzero(&acl_user, sizeof(acl_user));
  acl_user.user.str=*user ? strdup_root(&acl_memroot,user) : 0;
  acl_user.user.length= strlen(user);
  update_hostname(&acl_user.host, safe_strdup_root(&acl_memroot, host));
  if (plugin->str[0])
  {
    acl_user.plugin= *plugin;
    acl_user.auth_string.str= auth->str ?
      strmake_root(&acl_memroot, auth->str, auth->length) : const_cast<char*>("");
    acl_user.auth_string.length= auth->length;
    if (fix_user_plugin_ptr(&acl_user))
      acl_user.plugin.str= strmake_root(&acl_memroot, plugin->str, plugin->length);
  }
  else
  {
    acl_user.auth_string.str= strmake_root(&acl_memroot, password, password_len);
    acl_user.auth_string.length= password_len;
    set_user_salt(&acl_user, password, password_len);
    set_user_plugin(&acl_user, password_len);
  }

  acl_user.flags= 0;
  acl_user.access=privileges;
  acl_user.user_resource = *mqh;
  acl_user.sort=get_sort(2, acl_user.host.hostname, acl_user.user.str);
  acl_user.hostname_length=(uint) strlen(host);
  acl_user.ssl_type= (ssl_type != SSL_TYPE_NOT_SPECIFIED ?
		      ssl_type : SSL_TYPE_NONE);
  acl_user.ssl_cipher=	ssl_cipher   ? strdup_root(&acl_memroot,ssl_cipher) : 0;
  acl_user.x509_issuer= x509_issuer  ? strdup_root(&acl_memroot,x509_issuer) : 0;
  acl_user.x509_subject=x509_subject ? strdup_root(&acl_memroot,x509_subject) : 0;
  (void) my_init_dynamic_array(&acl_user.role_grants, sizeof(ACL_USER *),
                               8, 8, MYF(0));

  (void) push_dynamic(&acl_users,(uchar*) &acl_user);
  if (!acl_user.host.hostname ||
      (acl_user.host.hostname[0] == wild_many && !acl_user.host.hostname[1]))
    allow_all_hosts=1;		// Anyone can connect /* purecov: tested */
  my_qsort((uchar*) dynamic_element(&acl_users,0,ACL_USER*),acl_users.elements,
	   sizeof(ACL_USER),(qsort_cmp) acl_compare);

  /* Rebuild 'acl_check_hosts' since 'acl_users' has been modified */
  rebuild_check_host();

  /*
    Rebuild every user's role_grants since 'acl_users' has been sorted
    and old pointers to ACL_USER elements are no longer valid
  */
  rebuild_role_grants();
}


static void acl_update_db(const char *user, const char *host, const char *db,
                          ulong privileges)
{
  mysql_mutex_assert_owner(&acl_cache->lock);

  for (uint i=0 ; i < acl_dbs.elements ; i++)
  {
    ACL_DB *acl_db=dynamic_element(&acl_dbs,i,ACL_DB*);
    if ((!acl_db->user && !user[0]) ||
	(acl_db->user &&
	!strcmp(user,acl_db->user)))
    {
      if ((!acl_db->host.hostname && !host[0]) ||
	  (acl_db->host.hostname &&
	   !strcmp(host, acl_db->host.hostname)))
      {
	if ((!acl_db->db && !db[0]) ||
	    (acl_db->db && !strcmp(db,acl_db->db)))

	{
	  if (privileges)
          {
            acl_db->access= privileges;
            acl_db->initial_access= acl_db->access;
          }
	  else
	    delete_dynamic_element(&acl_dbs,i);
	}
      }
    }
  }
}


/*
  Insert a user/db/host combination into the global acl_cache

  SYNOPSIS
    acl_insert_db()
    user		User name
    host		Host name
    db			Database name
    privileges		Bitmap of privileges

  NOTES
    acl_cache->lock must be locked when calling this
*/

static void acl_insert_db(const char *user, const char *host, const char *db,
                          ulong privileges)
{
  ACL_DB acl_db;
  mysql_mutex_assert_owner(&acl_cache->lock);
  acl_db.user=strdup_root(&acl_memroot,user);
  update_hostname(&acl_db.host, safe_strdup_root(&acl_memroot, host));
  acl_db.db=strdup_root(&acl_memroot,db);
  acl_db.initial_access= acl_db.access= privileges;
  acl_db.sort=get_sort(3,acl_db.host.hostname,acl_db.db,acl_db.user);
  (void) push_dynamic(&acl_dbs,(uchar*) &acl_db);
  my_qsort((uchar*) dynamic_element(&acl_dbs,0,ACL_DB*),acl_dbs.elements,
	   sizeof(ACL_DB),(qsort_cmp) acl_compare);
}


/*
  Get privilege for a host, user and db combination

  as db_is_pattern changes the semantics of comparison,
  acl_cache is not used if db_is_pattern is set.
*/

ulong acl_get(const char *host, const char *ip,
              const char *user, const char *db, my_bool db_is_pattern)
{
  ulong host_access= ~(ulong)0, db_access= 0;
  uint i;
  size_t key_length;
  char key[ACL_KEY_LENGTH],*tmp_db,*end;
  acl_entry *entry;
  DBUG_ENTER("acl_get");

  tmp_db= strmov(strmov(key, safe_str(ip)) + 1, user) + 1;
  end= strnmov(tmp_db, db, key + sizeof(key) - tmp_db);

  if (end >= key + sizeof(key)) // db name was truncated
    DBUG_RETURN(0);             // no privileges for an invalid db name

  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, tmp_db);
    db=tmp_db;
  }
  key_length= (size_t) (end-key);

  mysql_mutex_lock(&acl_cache->lock);
  if (!db_is_pattern && (entry=acl_cache->search((uchar*) key, key_length)))
  {
    db_access=entry->access;
    mysql_mutex_unlock(&acl_cache->lock);
    DBUG_PRINT("exit", ("access: 0x%lx", db_access));
    DBUG_RETURN(db_access);
  }

  /*
    Check if there are some access rights for database and user
  */
  for (i=0 ; i < acl_dbs.elements ; i++)
  {
    ACL_DB *acl_db=dynamic_element(&acl_dbs,i,ACL_DB*);
    if (!acl_db->user || !strcmp(user,acl_db->user))
    {
      if (compare_hostname(&acl_db->host,host,ip))
      {
        if (!acl_db->db || !wild_compare(db,acl_db->db,db_is_pattern))
        {
          db_access=acl_db->access;
          if (acl_db->host.hostname)
            goto exit;                          // Fully specified. Take it
          /* the host table is not used for roles */
          if ((!host || !host[0]) && !acl_db->host.hostname && find_acl_role(user))
            goto exit;
          break; /* purecov: tested */
	}
      }
    }
  }
  if (!db_access)
    goto exit;					// Can't be better

  /*
    No host specified for user. Get hostdata from host table
  */
  host_access=0;				// Host must be found
  for (i=0 ; i < acl_hosts.elements ; i++)
  {
    ACL_HOST *acl_host=dynamic_element(&acl_hosts,i,ACL_HOST*);
    if (compare_hostname(&acl_host->host,host,ip))
    {
      if (!acl_host->db || !wild_compare(db,acl_host->db,db_is_pattern))
      {
	host_access=acl_host->access;		// Fully specified. Take it
	break;
      }
    }
  }
exit:
  /* Save entry in cache for quick retrieval */
  if (!db_is_pattern &&
      (entry= (acl_entry*) malloc(sizeof(acl_entry)+key_length)))
  {
    entry->access=(db_access & host_access);
    entry->length=key_length;
    memcpy((uchar*) entry->key,key,key_length);
    acl_cache->add(entry);
  }
  mysql_mutex_unlock(&acl_cache->lock);
  DBUG_PRINT("exit", ("access: 0x%lx", db_access & host_access));
  DBUG_RETURN(db_access & host_access);
}

/*
  Check if there are any possible matching entries for this host

  NOTES
    All host names without wild cards are stored in a hash table,
    entries with wildcards are stored in a dynamic array
*/

static void init_check_host(void)
{
  DBUG_ENTER("init_check_host");
  (void) my_init_dynamic_array(&acl_wild_hosts,sizeof(struct acl_host_and_ip),
                               acl_users.elements, 1, MYF(0));
  (void) my_hash_init(&acl_check_hosts,system_charset_info,
                      acl_users.elements, 0, 0,
                      (my_hash_get_key) check_get_key, 0, 0);
  if (!allow_all_hosts)
  {
    for (uint i=0 ; i < acl_users.elements ; i++)
    {
      ACL_USER *acl_user=dynamic_element(&acl_users,i,ACL_USER*);
      if (strchr(acl_user->host.hostname,wild_many) ||
	  strchr(acl_user->host.hostname,wild_one) ||
	  acl_user->host.ip_mask)
      {						// Has wildcard
	uint j;
	for (j=0 ; j < acl_wild_hosts.elements ; j++)
	{					// Check if host already exists
	  acl_host_and_ip *acl=dynamic_element(&acl_wild_hosts,j,
					       acl_host_and_ip *);
	  if (!my_strcasecmp(system_charset_info,
                             acl_user->host.hostname, acl->hostname))
	    break;				// already stored
	}
	if (j == acl_wild_hosts.elements)	// If new
	  (void) push_dynamic(&acl_wild_hosts,(uchar*) &acl_user->host);
      }
      else if (!my_hash_search(&acl_check_hosts,(uchar*)
                               acl_user->host.hostname,
                               strlen(acl_user->host.hostname)))
      {
	if (my_hash_insert(&acl_check_hosts,(uchar*) acl_user))
	{					// End of memory
	  allow_all_hosts=1;			// Should never happen
	  DBUG_VOID_RETURN;
	}
      }
    }
  }
  freeze_size(&acl_wild_hosts);
  freeze_size(&acl_check_hosts.array);
  DBUG_VOID_RETURN;
}


/*
  Rebuild lists used for checking of allowed hosts

  We need to rebuild 'acl_check_hosts' and 'acl_wild_hosts' after adding,
  dropping or renaming user, since they contain pointers to elements of
  'acl_user' array, which are invalidated by drop operation, and use
  ACL_USER::host::hostname as a key, which is changed by rename.
*/
static void rebuild_check_host(void)
{
  delete_dynamic(&acl_wild_hosts);
  my_hash_free(&acl_check_hosts);
  init_check_host();
}

/*
  Reset a role role_grants dynamic array.
  Also, the role's access bits are reset to the ones present in the table.
*/
static my_bool acl_role_reset_role_arrays(void *ptr,
                                    void * not_used __attribute__((unused)))
{
  ACL_ROLE *role= (ACL_ROLE *)ptr;
  reset_dynamic(&role->role_grants);
  reset_dynamic(&role->parent_grantee);
  role->counter= 0;
  return 0;
}

/*
   Add a the coresponding pointers present in the mapping to the entries in
   acl_users and acl_roles
*/
static bool add_role_user_mapping(ACL_USER_BASE *grantee, ACL_ROLE *role)
{
  return push_dynamic(&grantee->role_grants, (uchar*) &role)
      || push_dynamic(&role->parent_grantee, (uchar*) &grantee);

}

/*
  Revert the last add_role_user_mapping() action
*/
static void undo_add_role_user_mapping(ACL_USER_BASE *grantee, ACL_ROLE *role)
{
  void *pop __attribute__((unused));

  pop= pop_dynamic(&grantee->role_grants);
  DBUG_ASSERT(role == *(ACL_ROLE**)pop);

  pop= pop_dynamic(&role->parent_grantee);
  DBUG_ASSERT(grantee == *(ACL_USER_BASE**)pop);
}

/*
  this helper is used when building role_grants and parent_grantee arrays
  from scratch.

  this happens either on initial loading of data from tables, in acl_load().
  or in rebuild_role_grants after acl_role_reset_role_arrays().
*/
static bool add_role_user_mapping(const char *uname, const char *hname,
                                  const char *rname)
{
  ACL_USER_BASE *grantee= find_acl_user_base(uname, hname);
  ACL_ROLE *role= find_acl_role(rname);

  if (grantee == NULL || role == NULL)
    return 1;

  /*
    because all arrays are rebuilt completely, and counters were also reset,
    we can increment them here, and after the rebuild all counters will
    have correct values (equal to the number of roles granted).
  */
  if (grantee->flags & IS_ROLE)
    ((ACL_ROLE*)grantee)->counter++;
  return add_role_user_mapping(grantee, role);
}

/*
  This helper function is used to removes roles and grantees
  from the corresponding cross-reference arrays. see remove_role_user_mapping().
  as such, it asserts that an element to delete is present in the array,
  and is present only once.
*/
static void remove_ptr_from_dynarray(DYNAMIC_ARRAY *array, void *ptr)
{
  bool found __attribute__((unused))= false;
  for (uint i= 0; i < array->elements; i++)
  {
    if (ptr == *dynamic_element(array, i, void**))
    {
      DBUG_ASSERT(!found);
      delete_dynamic_element(array, i);
      IF_DBUG(found= true, break);
    }
  }
  DBUG_ASSERT(found);
}

static void remove_role_user_mapping(ACL_USER_BASE *grantee, ACL_ROLE *role,
                                     int grantee_idx=-1, int role_idx=-1)
{
  remove_ptr_from_dynarray(&grantee->role_grants, role);
  remove_ptr_from_dynarray(&role->parent_grantee, grantee);
}


static my_bool add_role_user_mapping_action(void *ptr, void *unused __attribute__((unused)))
{
  ROLE_GRANT_PAIR *pair= (ROLE_GRANT_PAIR*)ptr;
  bool status __attribute__((unused));
  status= add_role_user_mapping(pair->u_uname, pair->u_hname, pair->r_uname);
  /*
     The invariant chosen is that acl_roles_mappings should _always_
     only contain valid entries, referencing correct user and role grants.
     If add_role_user_mapping detects an invalid entry, it will not add
     the mapping into the ACL_USER::role_grants array.
  */
  DBUG_ASSERT(status == 0);
  return 0;
}


/*
  Rebuild the role grants every time the acl_users is modified

  The role grants in the ACL_USER class need to be rebuilt, as they contain
  pointers to elements of the acl_users array.
*/

static void rebuild_role_grants(void)
{
  DBUG_ENTER("rebuild_role_grants");
  /*
    Reset every user's and role's role_grants array
  */
  for (uint i=0; i < acl_users.elements; i++) {
    ACL_USER *user= dynamic_element(&acl_users, i, ACL_USER *);
    reset_dynamic(&user->role_grants);
  }
  my_hash_iterate(&acl_roles, acl_role_reset_role_arrays, NULL);

  /* Rebuild the direct links between users and roles in ACL_USER::role_grants */
  my_hash_iterate(&acl_roles_mappings, add_role_user_mapping_action, NULL);

  DBUG_VOID_RETURN;
}


/* Return true if there is no users that can match the given host */
bool acl_check_host(const char *host, const char *ip)
{
  if (allow_all_hosts)
    return 0;
  mysql_mutex_lock(&acl_cache->lock);

  if ((host && my_hash_search(&acl_check_hosts,(uchar*) host,strlen(host))) ||
      (ip && my_hash_search(&acl_check_hosts,(uchar*) ip, strlen(ip))))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    return 0;					// Found host
  }
  for (uint i=0 ; i < acl_wild_hosts.elements ; i++)
  {
    acl_host_and_ip *acl=dynamic_element(&acl_wild_hosts,i,acl_host_and_ip*);
    if (compare_hostname(acl, host, ip))
    {
      mysql_mutex_unlock(&acl_cache->lock);
      return 0;					// Host ok
    }
  }
  mysql_mutex_unlock(&acl_cache->lock);
  if (ip != NULL)
  {
    /* Increment HOST_CACHE.COUNT_HOST_ACL_ERRORS. */
    Host_errors errors;
    errors.m_host_acl= 1;
    inc_host_errors(ip, &errors);
  }
  return 1;					// Host is not allowed
}

/**
  Check if the user is allowed to alter the mysql.user table

 @param thd              THD
 @param host             Hostname for the user
 @param user             User name

 @return Error status
   @retval 0 OK
   @retval 1 Error
*/

static int check_alter_user(THD *thd, const char *host, const char *user)
{
  int error = 1;
  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    goto end;
  }

  if (IF_WSREP((!WSREP(thd) || !thd->wsrep_applier), 1) &&
      !thd->slave_thread && !thd->security_ctx->priv_user[0] &&
      !in_bootstrap)
  {
    my_message(ER_PASSWORD_ANONYMOUS_USER,
               ER_THD(thd, ER_PASSWORD_ANONYMOUS_USER),
               MYF(0));
    goto end;
  }
  if (!host) // Role
  {
    my_error(ER_PASSWORD_NO_MATCH, MYF(0));
    goto end;
  }

  if (!thd->slave_thread &&
      IF_WSREP((!WSREP(thd) || !thd->wsrep_applier),1) &&
      (strcmp(thd->security_ctx->priv_user, user) ||
       my_strcasecmp(system_charset_info, host,
                     thd->security_ctx->priv_host)))
  {
    if (check_access(thd, UPDATE_ACL, "mysql", NULL, NULL, 1, 0))
      goto end;
  }

  error = 0;

end:
  return error;
}
/**
  Check if the user is allowed to change password

 @param thd              THD
 @param user             User, hostname, new password or password hash

 @return Error status
   @retval 0 OK
   @retval 1 ERROR; In this case the error is sent to the client.
*/

bool check_change_password(THD *thd, LEX_USER *user)
{
  LEX_USER *real_user= get_current_user(thd, user);

  if (fix_and_copy_user(real_user, user, thd) ||
      validate_password(real_user))
    return true;

  *user= *real_user;

  return check_alter_user(thd, user->host.str, user->user.str);
}


/**
  Change a password for a user.

  @param thd            THD
  @param user           User, hostname, new password hash
 
  @return Error code
   @retval 0 ok
   @retval 1 ERROR; In this case the error is sent to the client.
*/
bool change_password(THD *thd, LEX_USER *user)
{
  TABLE_LIST tables[TABLES_MAX];
  /* Buffer should be extended when password length is extended. */
  char buff[512];
  ulong query_length= 0;
  enum_binlog_format save_binlog_format;
  int result=0;
  const CSET_STRING query_save __attribute__((unused)) = thd->query_string;
  DBUG_ENTER("change_password");
  DBUG_PRINT("enter",("host: '%s'  user: '%s'  new_password: '%s'",
		      user->host.str, user->user.str, user->pwhash.str));
  DBUG_ASSERT(user->host.str != 0);                     // Ensured by parent

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
    This has to be handled here as it's called by set_var.cc, which is
    not automaticly handled by sql_parse.cc
  */
  save_binlog_format= thd->set_current_stmt_binlog_format_stmt();

  if (mysql_bin_log.is_open() ||
      (WSREP(thd) && !IF_WSREP(thd->wsrep_applier, 0)))
  {
    query_length= sprintf(buff, "SET PASSWORD FOR '%-.120s'@'%-.120s'='%-.120s'",
              safe_str(user->user.str), safe_str(user->host.str),
              safe_str(user->pwhash.str));
  }

  if (WSREP(thd) && !IF_WSREP(thd->wsrep_applier, 0))
  {
    thd->set_query_inner(buff, query_length, system_charset_info);
    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, (char*)"user", NULL);
  }

  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user)))
    DBUG_RETURN(result != 1);

  result= 1;

  mysql_mutex_lock(&acl_cache->lock);
  ACL_USER *acl_user;
  if (!(acl_user= find_user_exact(user->host.str, user->user.str)))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    my_message(ER_PASSWORD_NO_MATCH,
               ER_THD(thd, ER_PASSWORD_NO_MATCH), MYF(0));
    goto end;
  }

  /* update loaded acl entry: */
  if (acl_user->plugin.str == native_password_plugin_name.str ||
      acl_user->plugin.str == old_password_plugin_name.str)
  {
    acl_user->auth_string.str= strmake_root(&acl_memroot, user->pwhash.str, user->pwhash.length);
    acl_user->auth_string.length= user->pwhash.length;
    set_user_salt(acl_user, user->pwhash.str, user->pwhash.length);
    set_user_plugin(acl_user, user->pwhash.length);
  }
  else
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                 ER_SET_PASSWORD_AUTH_PLUGIN,
                 ER_THD(thd, ER_SET_PASSWORD_AUTH_PLUGIN));

  if (update_user_table(thd, tables[USER_TABLE].table,
                        safe_str(acl_user->host.hostname),
                        safe_str(acl_user->user.str),
                        user->pwhash.str, user->pwhash.length))
  {
    mysql_mutex_unlock(&acl_cache->lock); /* purecov: deadcode */
    goto end;
  }

  acl_cache->clear(1);				// Clear locked hostname cache
  mysql_mutex_unlock(&acl_cache->lock);
  result= 0;
  if (mysql_bin_log.is_open())
  {
    DBUG_ASSERT(query_length);
    thd->clear_error();
    result= thd->binlog_query(THD::STMT_QUERY_TYPE, buff, query_length,
                              FALSE, FALSE, FALSE, 0);
  }
end:
  close_mysql_tables(thd);

#ifdef WITH_WSREP
error: // this label is used in WSREP_TO_ISOLATION_BEGIN
  if (WSREP(thd) && !thd->wsrep_applier)
  {
    WSREP_TO_ISOLATION_END;

    thd->set_query_inner(query_save);
    thd->wsrep_exec_mode  = LOCAL_STATE;
  }
#endif /* WITH_WSREP */
  thd->restore_stmt_binlog_format(save_binlog_format);

  DBUG_RETURN(result);
}

int acl_check_set_default_role(THD *thd, const char *host, const char *user)
{
  return check_alter_user(thd, host, user);
}

int acl_set_default_role(THD *thd, const char *host, const char *user,
                         const char *rolename)
{
  TABLE_LIST tables[TABLES_MAX];
  TABLE *table;
  char user_key[MAX_KEY_LENGTH];
  int result= 1;
  int error;
  ulong query_length= 0;
  bool clear_role= FALSE;
  char buff[512];
  enum_binlog_format save_binlog_format;
  const CSET_STRING query_save __attribute__((unused)) = thd->query_string;

  DBUG_ENTER("acl_set_default_role");
  DBUG_PRINT("enter",("host: '%s'  user: '%s'  rolename: '%s'",
                      safe_str(user), safe_str(host), safe_str(rolename)));

  if (rolename == current_role.str) {
    if (!thd->security_ctx->priv_role[0])
      rolename= "NONE";
    else
      rolename= thd->security_ctx->priv_role;
  }

  if (check_user_can_set_role(user, host, host, rolename, NULL))
    DBUG_RETURN(result);

  if (!strcasecmp(rolename, "NONE"))
    clear_role= TRUE;

  if (mysql_bin_log.is_open() ||
      (WSREP(thd) && !IF_WSREP(thd->wsrep_applier, 0)))
  {
    query_length=
      sprintf(buff,"SET DEFAULT ROLE '%-.120s' FOR '%-.120s'@'%-.120s'",
              safe_str(rolename), safe_str(user), safe_str(host));
  }

  if (WSREP(thd) && !IF_WSREP(thd->wsrep_applier, 0))
  {
    thd->set_query_inner(buff, query_length, system_charset_info);
    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, (char*)"user", NULL);
  }

  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user)))
    DBUG_RETURN(result != 1);

  table= tables[USER_TABLE].table;
  result= 1;

  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
    This has to be handled here as it's called by set_var.cc, which is
    not automaticly handled by sql_parse.cc
  */
  save_binlog_format= thd->set_current_stmt_binlog_format_stmt();

  mysql_mutex_lock(&acl_cache->lock);
  ACL_USER *acl_user;
  if (!(acl_user= find_user_exact(host, user)))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    my_message(ER_PASSWORD_NO_MATCH, ER_THD(thd, ER_PASSWORD_NO_MATCH),
               MYF(0));
    goto end;
  }

  if (!clear_role) {
    /* set new default_rolename */
    acl_user->default_rolename.str= safe_strdup_root(&acl_memroot, rolename);
    acl_user->default_rolename.length= strlen(rolename);
  }
  else
  {
    /* clear the default_rolename */
    acl_user->default_rolename.str = NULL;
    acl_user->default_rolename.length = 0;
  }

  /* update the mysql.user table with the new default role */
  table->use_all_columns();
  if (table->s->fields <= DEFAULT_ROLE_COLUMN_IDX)
  {
    my_error(ER_COL_COUNT_DOESNT_MATCH_PLEASE_UPDATE, MYF(0),
             table->alias.c_ptr(), DEFAULT_ROLE_COLUMN_IDX + 1, table->s->fields,
             static_cast<int>(table->s->mysql_version), MYSQL_VERSION_ID);
    mysql_mutex_unlock(&acl_cache->lock);
    goto end;
  }
  table->field[0]->store(host,(uint) strlen(host), system_charset_info);
  table->field[1]->store(user,(uint) strlen(user), system_charset_info);
  key_copy((uchar *) user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  if (table->file->ha_index_read_idx_map(table->record[0], 0,
                                         (uchar *) user_key, HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    my_message(ER_PASSWORD_NO_MATCH, ER_THD(thd, ER_PASSWORD_NO_MATCH),
               MYF(0));
    goto end;
  }
  store_record(table, record[1]);
  table->field[DEFAULT_ROLE_COLUMN_IDX]->store(acl_user->default_rolename.str,
                                               acl_user->default_rolename.length,
                                               system_charset_info);
  if ((error=table->file->ha_update_row(table->record[1],table->record[0])) &&
      error != HA_ERR_RECORD_IS_THE_SAME)
  {
    mysql_mutex_unlock(&acl_cache->lock);
    table->file->print_error(error,MYF(0));	/* purecov: deadcode */
    goto end;
  }

  acl_cache->clear(1);
  mysql_mutex_unlock(&acl_cache->lock);
  result= 0;
  if (mysql_bin_log.is_open())
  {
    DBUG_ASSERT(query_length);
    thd->clear_error();
    result= thd->binlog_query(THD::STMT_QUERY_TYPE, buff, query_length,
                              FALSE, FALSE, FALSE, 0);
  }
end:
  close_mysql_tables(thd);

#ifdef WITH_WSREP
error: // this label is used in WSREP_TO_ISOLATION_END
  if (WSREP(thd) && !thd->wsrep_applier)
  {
    WSREP_TO_ISOLATION_END;

    thd->set_query_inner(query_save);
    thd->wsrep_exec_mode  = LOCAL_STATE;
  }
#endif /* WITH_WSREP */

  thd->restore_stmt_binlog_format(save_binlog_format);

  DBUG_RETURN(result);
}


/*
  Find user in ACL

  SYNOPSIS
    is_acl_user()
    host                 host name
    user                 user name

  RETURN
   FALSE  user not fond
   TRUE   there is such user
*/

bool is_acl_user(const char *host, const char *user)
{
  bool res;

  /* --skip-grants */
  if (!initialized)
    return TRUE;

  mysql_mutex_lock(&acl_cache->lock);

  if (*host) // User
    res= find_user_exact(host, user) != NULL;
  else // Role
    res= find_acl_role(user) != NULL;

  mysql_mutex_unlock(&acl_cache->lock);
  return res;
}


/*
  unlike find_user_exact and find_user_wild,
  this function finds anonymous users too, it's when a
  user is not empty, but priv_user (acl_user->user) is empty.
*/
static ACL_USER *find_user_or_anon(const char *host, const char *user, const char *ip)
{
  ACL_USER *result= NULL;
  mysql_mutex_assert_owner(&acl_cache->lock);
  for (uint i=0; i < acl_users.elements; i++)
  {
    ACL_USER *acl_user_tmp= dynamic_element(&acl_users, i, ACL_USER*);
    if ((!acl_user_tmp->user.str ||
         !strcmp(user, acl_user_tmp->user.str)) &&
         compare_hostname(&acl_user_tmp->host, host, ip))
    {
      result= acl_user_tmp;
      break;
    }
  }
  return result;
}


/*
  Find first entry that matches the specified user@host pair
*/
static ACL_USER * find_user_exact(const char *host, const char *user)
{
  mysql_mutex_assert_owner(&acl_cache->lock);

  for (uint i=0 ; i < acl_users.elements ; i++)
  {
    ACL_USER *acl_user=dynamic_element(&acl_users,i,ACL_USER*);
    if (acl_user->eq(user, host))
      return acl_user;
  }
  return 0;
}

/*
  Find first entry that matches the specified user@host pair
*/
static ACL_USER * find_user_wild(const char *host, const char *user, const char *ip)
{
  mysql_mutex_assert_owner(&acl_cache->lock);

  for (uint i=0 ; i < acl_users.elements ; i++)
  {
    ACL_USER *acl_user=dynamic_element(&acl_users,i,ACL_USER*);
    if (acl_user->wild_eq(user, host, ip))
      return acl_user;
  }
  return 0;
}

/*
  Find a role with the specified name
*/
static ACL_ROLE *find_acl_role(const char *role)
{
  DBUG_ENTER("find_acl_role");
  DBUG_PRINT("enter",("role: '%s'", role));
  DBUG_PRINT("info", ("Hash elements: %ld", acl_roles.records));

  mysql_mutex_assert_owner(&acl_cache->lock);

  ACL_ROLE *r= (ACL_ROLE *)my_hash_search(&acl_roles, (uchar *)role,
                                          safe_strlen(role));
  DBUG_RETURN(r);
}


static ACL_USER_BASE *find_acl_user_base(const char *user, const char *host)
{
  if (*host)
    return find_user_exact(host, user);

  return find_acl_role(user);
}


/*
  Comparing of hostnames

  NOTES
  A hostname may be of type:
  hostname   (May include wildcards);   monty.pp.sci.fi
  ip	   (May include wildcards);   192.168.0.0
  ip/netmask			      192.168.0.0/255.255.255.0

  A net mask of 0.0.0.0 is not allowed.
*/

static const char *calc_ip(const char *ip, long *val, char end)
{
  long ip_val,tmp;
  if (!(ip=str2int(ip,10,0,255,&ip_val)) || *ip != '.')
    return 0;
  ip_val<<=24;
  if (!(ip=str2int(ip+1,10,0,255,&tmp)) || *ip != '.')
    return 0;
  ip_val+=tmp<<16;
  if (!(ip=str2int(ip+1,10,0,255,&tmp)) || *ip != '.')
    return 0;
  ip_val+=tmp<<8;
  if (!(ip=str2int(ip+1,10,0,255,&tmp)) || *ip != end)
    return 0;
  *val=ip_val+tmp;
  return ip;
}


static void update_hostname(acl_host_and_ip *host, const char *hostname)
{
  // fix historical undocumented convention that empty host is the same as '%'
  hostname=const_cast<char*>(hostname ? hostname : host_not_specified.str);
  host->hostname=(char*) hostname;             // This will not be modified!
  if (!(hostname= calc_ip(hostname,&host->ip,'/')) ||
      !(hostname= calc_ip(hostname+1,&host->ip_mask,'\0')))
  {
    host->ip= host->ip_mask=0;			// Not a masked ip
  }
}


static bool compare_hostname(const acl_host_and_ip *host, const char *hostname,
			     const char *ip)
{
  long tmp;
  if (host->ip_mask && ip && calc_ip(ip,&tmp,'\0'))
  {
    return (tmp & host->ip_mask) == host->ip;
  }
  return (!host->hostname ||
	  (hostname && !wild_case_compare(system_charset_info,
                                          hostname, host->hostname)) ||
	  (ip && !wild_compare(ip, host->hostname, 0)));
}

/**
  Check if the given host name needs to be resolved or not.
  Host name has to be resolved if it actually contains *name*.

  For example:
    192.168.1.1               --> FALSE
    192.168.1.0/255.255.255.0 --> FALSE
    %                         --> FALSE
    192.168.1.%               --> FALSE
    AB%                       --> FALSE

    AAAAFFFF                  --> TRUE (Hostname)
    AAAA:FFFF:1234:5678       --> FALSE
    ::1                       --> FALSE

  This function does not check if the given string is a valid host name or
  not. It assumes that the argument is a valid host name.

  @param hostname   the string to check.

  @return a flag telling if the argument needs to be resolved or not.
  @retval TRUE the argument is a host name and needs to be resolved.
  @retval FALSE the argument is either an IP address, or a patter and
          should not be resolved.
*/

bool hostname_requires_resolving(const char *hostname)
{
  if (!hostname)
    return FALSE;

  /* Check if hostname is the localhost. */

  size_t hostname_len= strlen(hostname);
  size_t localhost_len= strlen(my_localhost);

  if (hostname == my_localhost ||
      (hostname_len == localhost_len &&
       !my_strnncoll(system_charset_info,
                     (const uchar *) hostname,  hostname_len,
                     (const uchar *) my_localhost, strlen(my_localhost))))
  {
    return FALSE;
  }

  /*
    If the string contains any of {':', '%', '_', '/'}, it is definitely
    not a host name:
      - ':' means that the string is an IPv6 address;
      - '%' or '_' means that the string is a pattern;
      - '/' means that the string is an IPv4 network address;
  */

  for (const char *p= hostname; *p; ++p)
  {
    switch (*p) {
      case ':':
      case '%':
      case '_':
      case '/':
        return FALSE;
    }
  }

  /*
    Now we have to tell a host name (ab.cd, 12.ab) from an IPv4 address
    (12.34.56.78). The assumption is that if the string contains only
    digits and dots, it is an IPv4 address. Otherwise -- a host name.
  */

  for (const char *p= hostname; *p; ++p)
  {
    if (*p != '.' && !my_isdigit(&my_charset_latin1, *p))
      return TRUE; /* a "letter" has been found. */
  }

  return FALSE; /* all characters are either dots or digits. */
}


/**
  Update record for user in mysql.user privilege table with new password.

  @param thd              THD
  @param table            Pointer to TABLE object for open mysql.user table
  @param host             Hostname
  @param user             Username
  @param new_password     New password hash
  @param new_password_len Length of new password hash

  @see change_password
*/

static bool update_user_table(THD *thd, TABLE *table,
                              const char *host, const char *user,
			      const char *new_password, uint new_password_len)
{
  char user_key[MAX_KEY_LENGTH];
  int error;
  DBUG_ENTER("update_user_table");
  DBUG_PRINT("enter",("user: %s  host: %s",user,host));

  table->use_all_columns();
  table->field[0]->store(host,(uint) strlen(host), system_charset_info);
  table->field[1]->store(user,(uint) strlen(user), system_charset_info);
  key_copy((uchar *) user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  if (table->file->ha_index_read_idx_map(table->record[0], 0,
                                         (uchar *) user_key, HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
  {
    my_message(ER_PASSWORD_NO_MATCH, ER_THD(thd, ER_PASSWORD_NO_MATCH),
               MYF(0));	/* purecov: deadcode */
    DBUG_RETURN(1);				/* purecov: deadcode */
  }
  store_record(table,record[1]);
  table->field[2]->store(new_password, new_password_len, system_charset_info);
  if ((error=table->file->ha_update_row(table->record[1],table->record[0])) &&
      error != HA_ERR_RECORD_IS_THE_SAME)
  {
    table->file->print_error(error,MYF(0));	/* purecov: deadcode */
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


/*
  Return 1 if we are allowed to create new users
  the logic here is: INSERT_ACL is sufficient.
  It's also a requirement in opt_safe_user_create,
  otherwise CREATE_USER_ACL is enough.
*/

static bool test_if_create_new_users(THD *thd)
{
  Security_context *sctx= thd->security_ctx;
  bool create_new_users= MY_TEST(sctx->master_access & INSERT_ACL) ||
                         (!opt_safe_user_create &&
                          MY_TEST(sctx->master_access & CREATE_USER_ACL));
  if (!create_new_users)
  {
    TABLE_LIST tl;
    ulong db_access;
    tl.init_one_table(C_STRING_WITH_LEN("mysql"),
                      C_STRING_WITH_LEN("user"), "user", TL_WRITE);
    create_new_users= 1;

    db_access=acl_get(sctx->host, sctx->ip,
		      sctx->priv_user, tl.db, 0);
    if (sctx->priv_role[0])
      db_access|= acl_get("", "", sctx->priv_role, tl.db, 0);
    if (!(db_access & INSERT_ACL))
    {
      if (check_grant(thd, INSERT_ACL, &tl, FALSE, UINT_MAX, TRUE))
	create_new_users=0;
    }
  }
  return create_new_users;
}


/****************************************************************************
  Handle GRANT commands
****************************************************************************/

static int replace_user_table(THD *thd, TABLE *table, LEX_USER &combo,
			      ulong rights, bool revoke_grant,
                              bool can_create_user, bool no_auto_create)
{
  int error = -1;
  bool old_row_exists=0;
  char what= (revoke_grant) ? 'N' : 'Y';
  uchar user_key[MAX_KEY_LENGTH];
  bool handle_as_role= combo.is_role();
  LEX *lex= thd->lex;
  DBUG_ENTER("replace_user_table");

  mysql_mutex_assert_owner(&acl_cache->lock);

  if (combo.pwhash.str && combo.pwhash.str[0])
  {
    if (combo.pwhash.length != SCRAMBLED_PASSWORD_CHAR_LENGTH &&
        combo.pwhash.length != SCRAMBLED_PASSWORD_CHAR_LENGTH_323)
    {
      DBUG_ASSERT(0);
      my_error(ER_PASSWD_LENGTH, MYF(0), SCRAMBLED_PASSWORD_CHAR_LENGTH);
      DBUG_RETURN(-1);
    }
  }
  else
    combo.pwhash= empty_lex_str;

  /* if the user table is not up to date, we can't handle role updates */
  if (table->s->fields <= ROLE_ASSIGN_COLUMN_IDX && handle_as_role)
  {
    my_error(ER_COL_COUNT_DOESNT_MATCH_PLEASE_UPDATE, MYF(0),
             table->alias.c_ptr(), ROLE_ASSIGN_COLUMN_IDX + 1, table->s->fields,
             static_cast<int>(table->s->mysql_version), MYSQL_VERSION_ID);
    DBUG_RETURN(-1);
  }

  table->use_all_columns();
  table->field[0]->store(combo.host.str,combo.host.length,
                         system_charset_info);
  table->field[1]->store(combo.user.str,combo.user.length,
                         system_charset_info);
  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  if (table->file->ha_index_read_idx_map(table->record[0], 0, user_key,
                                         HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
  {
    /* what == 'N' means revoke */
    if (what == 'N')
    {
      my_error(ER_NONEXISTING_GRANT, MYF(0), combo.user.str, combo.host.str);
      goto end;
    }
    /*
      There are four options which affect the process of creation of
      a new user (mysqld option --safe-create-user, 'insert' privilege
      on 'mysql.user' table, using 'GRANT' with 'IDENTIFIED BY' and
      SQL_MODE flag NO_AUTO_CREATE_USER). Below is the simplified rule
      how it should work.
      if (safe-user-create && ! INSERT_priv) => reject
      else if (identified_by) => create
      else if (no_auto_create_user) => reject
      else create

      see also test_if_create_new_users()
    */
    else if (!combo.pwhash.length && !combo.plugin.length && no_auto_create)
    {
      my_error(ER_PASSWORD_NO_MATCH, MYF(0));
      goto end;
    }
    else if (!can_create_user)
    {
      my_error(ER_CANT_CREATE_USER_WITH_GRANT, MYF(0));
      goto end;
    }
    else if (combo.plugin.str[0])
    {
      if (!plugin_is_ready(&combo.plugin, MYSQL_AUTHENTICATION_PLUGIN))
      {
        my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), combo.plugin.str);
        goto end;
      }
    }

    old_row_exists = 0;
    restore_record(table,s->default_values);
    table->field[0]->store(combo.host.str,combo.host.length,
                           system_charset_info);
    table->field[1]->store(combo.user.str,combo.user.length,
                           system_charset_info);
  }
  else
  {
    old_row_exists = 1;
    store_record(table,record[1]);			// Save copy for update
  }

  if (!old_row_exists || combo.pwtext.length || combo.pwhash.length)
    if (!handle_as_role && validate_password(&combo))
      goto end;

  /* Update table columns with new privileges */

  Field **tmp_field;
  ulong priv;
  uint next_field;
  for (tmp_field= table->field+3, priv = SELECT_ACL;
       *tmp_field && (*tmp_field)->real_type() == MYSQL_TYPE_ENUM &&
	 ((Field_enum*) (*tmp_field))->typelib->count == 2 ;
       tmp_field++, priv <<= 1)
  {
    if (priv & rights)				 // set requested privileges
      (*tmp_field)->store(&what, 1, &my_charset_latin1);
  }
  rights= get_access(table, 3, &next_field);
  DBUG_PRINT("info",("table fields: %d",table->s->fields));
  if (combo.pwhash.str[0])
    table->field[2]->store(combo.pwhash.str, combo.pwhash.length, system_charset_info);
  if (table->s->fields >= 31)		/* From 4.0.0 we have more fields */
  {
    /* We write down SSL related ACL stuff */
    switch (lex->ssl_type) {
    case SSL_TYPE_ANY:
      table->field[next_field]->store(STRING_WITH_LEN("ANY"),
                                      &my_charset_latin1);
      table->field[next_field+1]->store("", 0, &my_charset_latin1);
      table->field[next_field+2]->store("", 0, &my_charset_latin1);
      table->field[next_field+3]->store("", 0, &my_charset_latin1);
      break;
    case SSL_TYPE_X509:
      table->field[next_field]->store(STRING_WITH_LEN("X509"),
                                      &my_charset_latin1);
      table->field[next_field+1]->store("", 0, &my_charset_latin1);
      table->field[next_field+2]->store("", 0, &my_charset_latin1);
      table->field[next_field+3]->store("", 0, &my_charset_latin1);
      break;
    case SSL_TYPE_SPECIFIED:
      table->field[next_field]->store(STRING_WITH_LEN("SPECIFIED"),
                                      &my_charset_latin1);
      table->field[next_field+1]->store("", 0, &my_charset_latin1);
      table->field[next_field+2]->store("", 0, &my_charset_latin1);
      table->field[next_field+3]->store("", 0, &my_charset_latin1);
      if (lex->ssl_cipher)
        table->field[next_field+1]->store(lex->ssl_cipher,
                                strlen(lex->ssl_cipher), system_charset_info);
      if (lex->x509_issuer)
        table->field[next_field+2]->store(lex->x509_issuer,
                                strlen(lex->x509_issuer), system_charset_info);
      if (lex->x509_subject)
        table->field[next_field+3]->store(lex->x509_subject,
                                strlen(lex->x509_subject), system_charset_info);
      break;
    case SSL_TYPE_NOT_SPECIFIED:
      break;
    case SSL_TYPE_NONE:
      table->field[next_field]->store("", 0, &my_charset_latin1);
      table->field[next_field+1]->store("", 0, &my_charset_latin1);
      table->field[next_field+2]->store("", 0, &my_charset_latin1);
      table->field[next_field+3]->store("", 0, &my_charset_latin1);
      break;
    }
    next_field+=4;

    USER_RESOURCES mqh= lex->mqh;
    if (mqh.specified_limits & USER_RESOURCES::QUERIES_PER_HOUR)
      table->field[next_field]->store((longlong) mqh.questions, TRUE);
    if (mqh.specified_limits & USER_RESOURCES::UPDATES_PER_HOUR)
      table->field[next_field+1]->store((longlong) mqh.updates, TRUE);
    if (mqh.specified_limits & USER_RESOURCES::CONNECTIONS_PER_HOUR)
      table->field[next_field+2]->store((longlong) mqh.conn_per_hour, TRUE);
    if (table->s->fields >= 36 &&
        (mqh.specified_limits & USER_RESOURCES::USER_CONNECTIONS))
      table->field[next_field+3]->store((longlong) mqh.user_conn, FALSE);
    next_field+= 4;
    if (table->s->fields >= 41)
    {
      table->field[next_field]->set_notnull();
      table->field[next_field + 1]->set_notnull();
      if (combo.plugin.str[0])
      {
        DBUG_ASSERT(combo.pwhash.str[0] == 0);
        table->field[2]->reset();
        table->field[next_field]->store(combo.plugin.str, combo.plugin.length,
                                        system_charset_info);
        table->field[next_field + 1]->store(combo.auth.str, combo.auth.length,
                                            system_charset_info);
      }
      if (combo.pwhash.str[0])
      {
        DBUG_ASSERT(combo.plugin.str[0] == 0);
        table->field[next_field]->reset();
        table->field[next_field + 1]->reset();
      }

      if (table->s->fields > MAX_STATEMENT_TIME_COLUMN_IDX)
      {
        if (mqh.specified_limits & USER_RESOURCES::MAX_STATEMENT_TIME)
          table->field[MAX_STATEMENT_TIME_COLUMN_IDX]->
            store(mqh.max_statement_time);
      }
    }
    mqh_used= (mqh_used || mqh.questions || mqh.updates || mqh.conn_per_hour ||
               mqh.user_conn || mqh.max_statement_time != 0.0);

    /* table format checked earlier */
    if (handle_as_role)
    {
      if (old_row_exists && !check_is_role(table))
      {
        goto end;
      }
      table->field[ROLE_ASSIGN_COLUMN_IDX]->store("Y", 1, system_charset_info);
    }
  }

  if (old_row_exists)
  {
    /*
      We should NEVER delete from the user table, as a uses can still
      use mysqld even if he doesn't have any privileges in the user table!
    */
    if (cmp_record(table,record[1]))
    {
      if ((error=
           table->file->ha_update_row(table->record[1],table->record[0])) &&
          error != HA_ERR_RECORD_IS_THE_SAME)
      {                                         // This should never happen
        table->file->print_error(error,MYF(0)); /* purecov: deadcode */
        error= -1;                              /* purecov: deadcode */
        goto end;                               /* purecov: deadcode */
      }
      else
        error= 0;
    }
  }
  else if ((error=table->file->ha_write_row(table->record[0]))) // insert
  {						// This should never happen
    if (table->file->is_fatal_error(error, HA_CHECK_DUP))
    {
      table->file->print_error(error,MYF(0));	/* purecov: deadcode */
      error= -1;				/* purecov: deadcode */
      goto end;					/* purecov: deadcode */
    }
  }
  error=0;					// Privileges granted / revoked

end:
  if (!error)
  {
    acl_cache->clear(1);			// Clear privilege cache
    if (old_row_exists)
    {
      if (handle_as_role)
        acl_update_role(combo.user.str, rights);
      else
        acl_update_user(combo.user.str, combo.host.str,
                        combo.pwhash.str, combo.pwhash.length,
                        lex->ssl_type,
                        lex->ssl_cipher,
                        lex->x509_issuer,
                        lex->x509_subject,
                        &lex->mqh,
                        rights,
                        &combo.plugin,
                        &combo.auth);
    }
    else
    {
      if (handle_as_role)
        acl_insert_role(combo.user.str, rights);
      else
        acl_insert_user(combo.user.str, combo.host.str,
                        combo.pwhash.str, combo.pwhash.length,
                        lex->ssl_type,
                        lex->ssl_cipher,
                        lex->x509_issuer,
                        lex->x509_subject,
                        &lex->mqh,
                        rights,
                        &combo.plugin,
                        &combo.auth);
    }
  }
  DBUG_RETURN(error);
}


/*
  change grants in the mysql.db table
*/

static int replace_db_table(TABLE *table, const char *db,
			    const LEX_USER &combo,
			    ulong rights, bool revoke_grant)
{
  uint i;
  ulong priv,store_rights;
  bool old_row_exists=0;
  int error;
  char what= (revoke_grant) ? 'N' : 'Y';
  uchar user_key[MAX_KEY_LENGTH];
  DBUG_ENTER("replace_db_table");

  /* Check if there is such a user in user table in memory? */
  if (!find_user_wild(combo.host.str,combo.user.str))
  {
    /* The user could be a role, check if the user is registered as a role */
    if (!combo.host.length && !find_acl_role(combo.user.str))
    {
      my_message(ER_PASSWORD_NO_MATCH, ER_THD(table->in_use,
                                              ER_PASSWORD_NO_MATCH), MYF(0));
      DBUG_RETURN(-1);
    }
  }

  table->use_all_columns();
  table->field[0]->store(combo.host.str,combo.host.length,
                         system_charset_info);
  table->field[1]->store(db,(uint) strlen(db), system_charset_info);
  table->field[2]->store(combo.user.str,combo.user.length,
                         system_charset_info);
  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  if (table->file->ha_index_read_idx_map(table->record[0],0, user_key,
                                         HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
  {
    if (what == 'N')
    { // no row, no revoke
      my_error(ER_NONEXISTING_GRANT, MYF(0), combo.user.str, combo.host.str);
      goto abort;
    }
    old_row_exists = 0;
    restore_record(table, s->default_values);
    table->field[0]->store(combo.host.str,combo.host.length,
                           system_charset_info);
    table->field[1]->store(db,(uint) strlen(db), system_charset_info);
    table->field[2]->store(combo.user.str,combo.user.length,
                           system_charset_info);
  }
  else
  {
    old_row_exists = 1;
    store_record(table,record[1]);
  }

  store_rights=get_rights_for_db(rights);
  for (i= 3, priv= 1; i < table->s->fields; i++, priv <<= 1)
  {
    if (priv & store_rights)			// do it if priv is chosen
      table->field [i]->store(&what,1, &my_charset_latin1);// set requested privileges
  }
  rights=get_access(table,3);
  rights=fix_rights_for_db(rights);

  if (old_row_exists)
  {
    /* update old existing row */
    if (rights)
    {
      if ((error= table->file->ha_update_row(table->record[1],
                                             table->record[0])) &&
          error != HA_ERR_RECORD_IS_THE_SAME)
	goto table_error;			/* purecov: deadcode */
    }
    else	/* must have been a revoke of all privileges */
    {
      if ((error= table->file->ha_delete_row(table->record[1])))
	goto table_error;			/* purecov: deadcode */
    }
  }
  else if (rights && (error= table->file->ha_write_row(table->record[0])))
  {
    if (table->file->is_fatal_error(error, HA_CHECK_DUP_KEY))
      goto table_error; /* purecov: deadcode */
  }

  acl_cache->clear(1);				// Clear privilege cache
  if (old_row_exists)
    acl_update_db(combo.user.str,combo.host.str,db,rights);
  else
  if (rights)
    acl_insert_db(combo.user.str,combo.host.str,db,rights);
  DBUG_RETURN(0);

  /* This could only happen if the grant tables got corrupted */
table_error:
  table->file->print_error(error,MYF(0));	/* purecov: deadcode */

abort:
  DBUG_RETURN(-1);
}

/**
  Updates the mysql.roles_mapping table

  @param table          TABLE to update
  @param user           user name of the grantee
  @param host           host name of the grantee
  @param role           role name to grant
  @param with_admin     WITH ADMIN OPTION flag
  @param existing       the entry in the acl_roles_mappings hash or NULL.
                        it is never NULL if revoke_grant is true.
                        it is NULL when a new pair is added, it's not NULL
                        when an existing pair is updated.
  @param revoke_grant   true for REVOKE, false for GRANT
*/
static int
replace_roles_mapping_table(TABLE *table, LEX_STRING *user, LEX_STRING *host,
                            LEX_STRING *role, bool with_admin,
                            ROLE_GRANT_PAIR *existing, bool revoke_grant)
{
  DBUG_ENTER("replace_roles_mapping_table");

  uchar row_key[MAX_KEY_LENGTH];
  int error;
  table->use_all_columns();
  restore_record(table, s->default_values);
  table->field[0]->store(host->str, host->length, system_charset_info);
  table->field[1]->store(user->str, user->length, system_charset_info);
  table->field[2]->store(role->str, role->length, system_charset_info);

  DBUG_ASSERT(!revoke_grant || existing);

  if (existing) // delete or update
  {
    key_copy(row_key, table->record[0], table->key_info,
             table->key_info->key_length);
    if (table->file->ha_index_read_idx_map(table->record[1], 0, row_key,
                                           HA_WHOLE_KEY, HA_READ_KEY_EXACT))
    {
      /* No match */
      DBUG_RETURN(1);
    }
    if (revoke_grant && !with_admin) 
    {
      if ((error= table->file->ha_delete_row(table->record[1])))
      {
        DBUG_PRINT("info", ("error deleting row '%s' '%s' '%s'",
                            host->str, user->str, role->str));
        goto table_error;
      }
    }
    else if (with_admin)
    {
      table->field[3]->store(!revoke_grant + 1);

      if ((error= table->file->ha_update_row(table->record[1], table->record[0])))
      {
        DBUG_PRINT("info", ("error updating row '%s' '%s' '%s'",
                            host->str, user->str, role->str));
        goto table_error;
      }
    }
    DBUG_RETURN(0);
  }

  table->field[3]->store(with_admin + 1);

  if ((error= table->file->ha_write_row(table->record[0])))
  {
    DBUG_PRINT("info", ("error inserting row '%s' '%s' '%s'",
                        host->str, user->str, role->str));
    goto table_error;
  }

  /* all ok */
  DBUG_RETURN(0);

table_error:
  DBUG_PRINT("info", ("table error"));
  table->file->print_error(error, MYF(0));
  DBUG_RETURN(1);
}


/**
  Updates the acl_roles_mappings hash

  @param user           user name of the grantee
  @param host           host name of the grantee
  @param role           role name to grant
  @param with_admin     WITH ADMIN OPTION flag
  @param existing       the entry in the acl_roles_mappings hash or NULL.
                        it is never NULL if revoke_grant is true.
                        it is NULL when a new pair is added, it's not NULL
                        when an existing pair is updated.
  @param revoke_grant   true for REVOKE, false for GRANT
*/
static int
update_role_mapping(LEX_STRING *user, LEX_STRING *host, LEX_STRING *role,
                    bool with_admin, ROLE_GRANT_PAIR *existing, bool revoke_grant)
{
  if (revoke_grant)
  {
    if (with_admin)
    {
      existing->with_admin= false;
      return 0;
    }
    return my_hash_delete(&acl_roles_mappings, (uchar*)existing);
  }

  if (existing)
  {
    existing->with_admin|= with_admin;
    return 0;
  }

  /* allocate a new entry that will go in the hash */
  ROLE_GRANT_PAIR *hash_entry= new (&acl_memroot) ROLE_GRANT_PAIR;
  if (hash_entry->init(&acl_memroot, user->str, host->str,
                       role->str, with_admin))
    return 1;
  return my_hash_insert(&acl_roles_mappings, (uchar*) hash_entry);
}

static void
acl_update_proxy_user(ACL_PROXY_USER *new_value, bool is_revoke)
{
  mysql_mutex_assert_owner(&acl_cache->lock);

  DBUG_ENTER("acl_update_proxy_user");
  for (uint i= 0; i < acl_proxy_users.elements; i++)
  {
    ACL_PROXY_USER *acl_user=
      dynamic_element(&acl_proxy_users, i, ACL_PROXY_USER *);

    if (acl_user->pk_equals(new_value))
    {
      if (is_revoke)
      {
        DBUG_PRINT("info", ("delting ACL_PROXY_USER"));
        delete_dynamic_element(&acl_proxy_users, i);
      }
      else
      {
        DBUG_PRINT("info", ("updating ACL_PROXY_USER"));
        acl_user->set_data(new_value);
      }
      break;
    }
  }
  DBUG_VOID_RETURN;
}


static void
acl_insert_proxy_user(ACL_PROXY_USER *new_value)
{
  DBUG_ENTER("acl_insert_proxy_user");
  mysql_mutex_assert_owner(&acl_cache->lock);
  (void) push_dynamic(&acl_proxy_users, (uchar *) new_value);
  my_qsort((uchar*) dynamic_element(&acl_proxy_users, 0, ACL_PROXY_USER *),
           acl_proxy_users.elements,
           sizeof(ACL_PROXY_USER), (qsort_cmp) acl_compare);
  DBUG_VOID_RETURN;
}


static int
replace_proxies_priv_table(THD *thd, TABLE *table, const LEX_USER *user,
                         const LEX_USER *proxied_user, bool with_grant_arg,
                         bool revoke_grant)
{
  bool old_row_exists= 0;
  int error;
  uchar user_key[MAX_KEY_LENGTH];
  ACL_PROXY_USER new_grant;
  char grantor[USER_HOST_BUFF_SIZE];

  DBUG_ENTER("replace_proxies_priv_table");

  /* Check if there is such a user in user table in memory? */
  if (!find_user_wild(user->host.str,user->user.str))
  {
    my_message(ER_PASSWORD_NO_MATCH,
               ER_THD(thd, ER_PASSWORD_NO_MATCH), MYF(0));
    DBUG_RETURN(-1);
  }

  table->use_all_columns();
  ACL_PROXY_USER::store_pk (table, &user->host, &user->user,
                            &proxied_user->host, &proxied_user->user);

  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  get_grantor(thd, grantor);

  if ((error= table->file->ha_index_init(0, 1)))
  {
    table->file->print_error(error, MYF(0));
    DBUG_PRINT("info", ("ha_index_init error"));
    DBUG_RETURN(-1);
  }

  if (table->file->ha_index_read_map(table->record[0], user_key,
                                     HA_WHOLE_KEY,
                                     HA_READ_KEY_EXACT))
  {
    DBUG_PRINT ("info", ("Row not found"));
    if (revoke_grant)
    { // no row, no revoke
      my_error(ER_NONEXISTING_GRANT, MYF(0), user->user.str, user->host.str);
      goto abort;
    }
    old_row_exists= 0;
    restore_record(table, s->default_values);
    ACL_PROXY_USER::store_data_record(table, &user->host, &user->user,
                                      &proxied_user->host,
                                      &proxied_user->user,
                                      with_grant_arg,
                                      grantor);
  }
  else
  {
    DBUG_PRINT("info", ("Row found"));
    old_row_exists= 1;
    store_record(table, record[1]);
  }

  if (old_row_exists)
  {
    /* update old existing row */
    if (!revoke_grant)
    {
      if ((error= table->file->ha_update_row(table->record[1],
                                             table->record[0])) &&
          error != HA_ERR_RECORD_IS_THE_SAME)
	goto table_error;			/* purecov: inspected */
    }
    else
    {
      if ((error= table->file->ha_delete_row(table->record[1])))
	goto table_error;			/* purecov: inspected */
    }
  }
  else if ((error= table->file->ha_write_row(table->record[0])))
  {
    DBUG_PRINT("info", ("error inserting the row"));
    if (table->file->is_fatal_error(error, HA_CHECK_DUP_KEY))
      goto table_error; /* purecov: inspected */
  }

  acl_cache->clear(1);				// Clear privilege cache
  if (old_row_exists)
  {
    new_grant.init(user->host.str, user->user.str,
                   proxied_user->host.str, proxied_user->user.str,
                   with_grant_arg);
    acl_update_proxy_user(&new_grant, revoke_grant);
  }
  else
  {
    new_grant.init(&acl_memroot, user->host.str, user->user.str,
                   proxied_user->host.str, proxied_user->user.str,
                   with_grant_arg);
    acl_insert_proxy_user(&new_grant);
  }

  table->file->ha_index_end();
  DBUG_RETURN(0);

  /* This could only happen if the grant tables got corrupted */
table_error:
  DBUG_PRINT("info", ("table error"));
  table->file->print_error(error, MYF(0));	/* purecov: inspected */

abort:
  DBUG_PRINT("info", ("aborting replace_proxies_priv_table"));
  table->file->ha_index_end();
  DBUG_RETURN(-1);
}


class GRANT_COLUMN :public Sql_alloc
{
public:
  char *column;
  ulong rights;
  ulong init_rights;
  uint key_length;
  GRANT_COLUMN(String &c,  ulong y) :rights (y), init_rights(y)
  {
    column= (char*) memdup_root(&grant_memroot,c.ptr(), key_length=c.length());
  }

  /* this constructor assumes thas source->column is allocated in grant_memroot */
  GRANT_COLUMN(GRANT_COLUMN *source) : column(source->column),
    rights (source->rights), init_rights(0), key_length(source->key_length) { }
};


static uchar* get_key_column(GRANT_COLUMN *buff, size_t *length,
			    my_bool not_used __attribute__((unused)))
{
  *length=buff->key_length;
  return (uchar*) buff->column;
}

class GRANT_NAME :public Sql_alloc
{
public:
  acl_host_and_ip host;
  char *db, *user, *tname, *hash_key;
  ulong privs;
  ulong init_privs; /* privileges found in physical table */
  ulong sort;
  size_t key_length;
  GRANT_NAME(const char *h, const char *d,const char *u,
             const char *t, ulong p, bool is_routine);
  GRANT_NAME (TABLE *form, bool is_routine);
  virtual ~GRANT_NAME() {};
  virtual bool ok() { return privs != 0; }
  void set_user_details(const char *h, const char *d,
                        const char *u, const char *t,
                        bool is_routine);
};


class GRANT_TABLE :public GRANT_NAME
{
public:
  ulong cols;
  ulong init_cols; /* privileges found in physical table */
  HASH hash_columns;

  GRANT_TABLE(const char *h, const char *d,const char *u,
              const char *t, ulong p, ulong c);
  GRANT_TABLE (TABLE *form, TABLE *col_privs);
  ~GRANT_TABLE();
  bool ok() { return privs != 0 || cols != 0; }
  void init_hash()
  {
    my_hash_init2(&hash_columns, 4, system_charset_info, 0, 0, 0,
                  (my_hash_get_key) get_key_column, 0, 0, 0);
  }
};


void GRANT_NAME::set_user_details(const char *h, const char *d,
                                  const char *u, const char *t,
                                  bool is_routine)
{
  /* Host given by user */
  update_hostname(&host, strdup_root(&grant_memroot, h));
  if (db != d)
  {
    db= strdup_root(&grant_memroot, d);
    if (lower_case_table_names)
      my_casedn_str(files_charset_info, db);
  }
  user = strdup_root(&grant_memroot,u);
  sort=  get_sort(3,host.hostname,db,user);
  if (tname != t)
  {
    tname= strdup_root(&grant_memroot, t);
    if (lower_case_table_names || is_routine)
      my_casedn_str(files_charset_info, tname);
  }
  key_length= strlen(d) + strlen(u)+ strlen(t)+3;
  hash_key=   (char*) alloc_root(&grant_memroot,key_length);
  strmov(strmov(strmov(hash_key,user)+1,db)+1,tname);
}

GRANT_NAME::GRANT_NAME(const char *h, const char *d,const char *u,
                       const char *t, ulong p, bool is_routine)
  :db(0), tname(0), privs(p), init_privs(p)
{
  set_user_details(h, d, u, t, is_routine);
}

GRANT_TABLE::GRANT_TABLE(const char *h, const char *d,const char *u,
                	 const char *t, ulong p, ulong c)
  :GRANT_NAME(h,d,u,t,p, FALSE), cols(c)
{
  init_hash();
}

/*
  create a new GRANT_TABLE entry for role inheritance. init_* fields are set
  to 0
*/
GRANT_NAME::GRANT_NAME(TABLE *form, bool is_routine)
{
  user= safe_str(get_field(&grant_memroot,form->field[2]));

  const char *hostname= get_field(&grant_memroot, form->field[0]);
  mysql_mutex_lock(&acl_cache->lock);
  if (!hostname && find_acl_role(user))
    hostname= "";
  mysql_mutex_unlock(&acl_cache->lock);
  update_hostname(&host, hostname);

  db=    get_field(&grant_memroot,form->field[1]);
  sort=  get_sort(3, host.hostname, db, user);
  tname= get_field(&grant_memroot,form->field[3]);
  if (!db || !tname)
  {
    /* Wrong table row; Ignore it */
    privs= 0;
    return;					/* purecov: inspected */
  }
  if (lower_case_table_names)
  {
    my_casedn_str(files_charset_info, db);
  }
  if (lower_case_table_names || is_routine)
  {
    my_casedn_str(files_charset_info, tname);
  }
  key_length= (strlen(db) + strlen(user) + strlen(tname) + 3);
  hash_key=   (char*) alloc_root(&grant_memroot, key_length);
  strmov(strmov(strmov(hash_key,user)+1,db)+1,tname);
  privs = (ulong) form->field[6]->val_int();
  privs = fix_rights_for_table(privs);
  init_privs= privs;
}


GRANT_TABLE::GRANT_TABLE(TABLE *form, TABLE *col_privs)
  :GRANT_NAME(form, FALSE)
{
  uchar key[MAX_KEY_LENGTH];

  if (!db || !tname)
  {
    /* Wrong table row; Ignore it */
    my_hash_clear(&hash_columns);               /* allow for destruction */
    cols= 0;
    return;
  }
  cols= (ulong) form->field[7]->val_int();
  cols= fix_rights_for_column(cols);
  /*
    Initial columns privileges are the same as column privileges on creation.
    In case of roles, the cols privilege bits can get inherited and thus
    cause the cols field to change. The init_cols field is always the same
    as the physical table entry
  */
  init_cols= cols;

  init_hash();

  if (cols)
  {
    uint key_prefix_len;
    KEY_PART_INFO *key_part= col_privs->key_info->key_part;
    col_privs->field[0]->store(host.hostname,
                               (uint) safe_strlen(host.hostname),
                               system_charset_info);
    col_privs->field[1]->store(db,(uint) strlen(db), system_charset_info);
    col_privs->field[2]->store(user,(uint) strlen(user), system_charset_info);
    col_privs->field[3]->store(tname,(uint) strlen(tname), system_charset_info);

    key_prefix_len= (key_part[0].store_length +
                     key_part[1].store_length +
                     key_part[2].store_length +
                     key_part[3].store_length);
    key_copy(key, col_privs->record[0], col_privs->key_info, key_prefix_len);
    col_privs->field[4]->store("",0, &my_charset_latin1);

    if (col_privs->file->ha_index_init(0, 1))
    {
      cols= 0;
      init_cols= 0;
      return;
    }

    if (col_privs->file->ha_index_read_map(col_privs->record[0], (uchar*) key,
                                           (key_part_map)15,
                                           HA_READ_KEY_EXACT))
    {
      cols= 0; /* purecov: deadcode */
      init_cols= 0;
      col_privs->file->ha_index_end();
      return;
    }
    do
    {
      String *res,column_name;
      GRANT_COLUMN *mem_check;
      /* As column name is a string, we don't have to supply a buffer */
      res=col_privs->field[4]->val_str(&column_name);
      ulong priv= (ulong) col_privs->field[6]->val_int();
      if (!(mem_check = new GRANT_COLUMN(*res,
                                         fix_rights_for_column(priv))))
      {
        /* Don't use this entry */
        privs= cols= init_privs= init_cols=0;   /* purecov: deadcode */
        return;				/* purecov: deadcode */
      }
      if (my_hash_insert(&hash_columns, (uchar *) mem_check))
      {
        /* Invalidate this entry */
        privs= cols= init_privs= init_cols=0;
        return;
      }
    } while (!col_privs->file->ha_index_next(col_privs->record[0]) &&
             !key_cmp_if_same(col_privs,key,0,key_prefix_len));
    col_privs->file->ha_index_end();
  }
}


GRANT_TABLE::~GRANT_TABLE()
{
  my_hash_free(&hash_columns);
}


static uchar* get_grant_table(GRANT_NAME *buff, size_t *length,
			     my_bool not_used __attribute__((unused)))
{
  *length=buff->key_length;
  return (uchar*) buff->hash_key;
}


static void free_grant_table(GRANT_TABLE *grant_table)
{
  grant_table->~GRANT_TABLE();
}


/* Search after a matching grant. Prefer exact grants before not exact ones */

static GRANT_NAME *name_hash_search(HASH *name_hash,
                                    const char *host,const char* ip,
                                    const char *db,
                                    const char *user, const char *tname,
                                    bool exact, bool name_tolower)
{
  char helping[SAFE_NAME_LEN*2+USERNAME_LENGTH+3];
  char *hend = helping + sizeof(helping);
  uint len;
  GRANT_NAME *grant_name,*found=0;
  HASH_SEARCH_STATE state;

  char *db_ptr= strmov(helping, user) + 1;
  char *tname_ptr= strnmov(db_ptr, db, hend - db_ptr) + 1;
  if (tname_ptr > hend)
    return 0; // invalid name = not found
  char *end= strnmov(tname_ptr, tname, hend - tname_ptr) + 1;
  if (end > hend)
    return 0; // invalid name = not found

  len  = (uint) (end - helping);
  if (name_tolower)
    my_casedn_str(files_charset_info, tname_ptr);
  for (grant_name= (GRANT_NAME*) my_hash_first(name_hash, (uchar*) helping,
                                               len, &state);
       grant_name ;
       grant_name= (GRANT_NAME*) my_hash_next(name_hash,(uchar*) helping,
                                              len, &state))
  {
    if (exact)
    {
      if (!grant_name->host.hostname ||
          (host &&
	   !my_strcasecmp(system_charset_info, host,
                          grant_name->host.hostname)) ||
	  (ip && !strcmp(ip, grant_name->host.hostname)))
	return grant_name;
    }
    else
    {
      if (compare_hostname(&grant_name->host, host, ip) &&
          (!found || found->sort < grant_name->sort))
	found=grant_name;					// Host ok
    }
  }
  return found;
}


static GRANT_NAME *
routine_hash_search(const char *host, const char *ip, const char *db,
                 const char *user, const char *tname, bool proc, bool exact)
{
  return (GRANT_TABLE*)
    name_hash_search(proc ? &proc_priv_hash : &func_priv_hash,
		     host, ip, db, user, tname, exact, TRUE);
}


static GRANT_TABLE *
table_hash_search(const char *host, const char *ip, const char *db,
		  const char *user, const char *tname, bool exact)
{
  return (GRANT_TABLE*) name_hash_search(&column_priv_hash, host, ip, db,
					 user, tname, exact, FALSE);
}


static GRANT_COLUMN *
column_hash_search(GRANT_TABLE *t, const char *cname, uint length)
{
  return (GRANT_COLUMN*) my_hash_search(&t->hash_columns,
                                        (uchar*) cname, length);
}


static int replace_column_table(GRANT_TABLE *g_t,
				TABLE *table, const LEX_USER &combo,
				List <LEX_COLUMN> &columns,
				const char *db, const char *table_name,
				ulong rights, bool revoke_grant)
{
  int result=0;
  uchar key[MAX_KEY_LENGTH];
  uint key_prefix_length;
  KEY_PART_INFO *key_part= table->key_info->key_part;
  DBUG_ENTER("replace_column_table");

  table->use_all_columns();
  table->field[0]->store(combo.host.str,combo.host.length,
                         system_charset_info);
  table->field[1]->store(db,(uint) strlen(db),
                         system_charset_info);
  table->field[2]->store(combo.user.str,combo.user.length,
                         system_charset_info);
  table->field[3]->store(table_name,(uint) strlen(table_name),
                         system_charset_info);

  /* Get length of 4 first key parts */
  key_prefix_length= (key_part[0].store_length + key_part[1].store_length +
                      key_part[2].store_length + key_part[3].store_length);
  key_copy(key, table->record[0], table->key_info, key_prefix_length);

  rights&= COL_ACLS;				// Only ACL for columns

  /* first fix privileges for all columns in column list */

  List_iterator <LEX_COLUMN> iter(columns);
  class LEX_COLUMN *column;
  int error= table->file->ha_index_init(0, 1);
  if (error)
  {
    table->file->print_error(error, MYF(0));
    DBUG_RETURN(-1);
  }

  while ((column= iter++))
  {
    ulong privileges= column->rights;
    bool old_row_exists=0;
    uchar user_key[MAX_KEY_LENGTH];

    key_restore(table->record[0],key,table->key_info,
                key_prefix_length);
    table->field[4]->store(column->column.ptr(), column->column.length(),
                           system_charset_info);
    /* Get key for the first 4 columns */
    key_copy(user_key, table->record[0], table->key_info,
             table->key_info->key_length);

    if (table->file->ha_index_read_map(table->record[0], user_key,
                                       HA_WHOLE_KEY, HA_READ_KEY_EXACT))
    {
      if (revoke_grant)
      {
	my_error(ER_NONEXISTING_TABLE_GRANT, MYF(0),
                 combo.user.str, combo.host.str,
                 table_name);                   /* purecov: inspected */
	result= -1;                             /* purecov: inspected */
	continue;                               /* purecov: inspected */
      }
      old_row_exists = 0;
      restore_record(table, s->default_values);		// Get empty record
      key_restore(table->record[0],key,table->key_info,
                  key_prefix_length);
      table->field[4]->store(column->column.ptr(),column->column.length(),
                             system_charset_info);
    }
    else
    {
      ulong tmp= (ulong) table->field[6]->val_int();
      tmp=fix_rights_for_column(tmp);

      if (revoke_grant)
	privileges = tmp & ~(privileges | rights);
      else
	privileges |= tmp;
      old_row_exists = 1;
      store_record(table,record[1]);			// copy original row
    }

    table->field[6]->store((longlong) get_rights_for_column(privileges), TRUE);

    if (old_row_exists)
    {
      GRANT_COLUMN *grant_column;
      if (privileges)
	error=table->file->ha_update_row(table->record[1],table->record[0]);
      else
	error=table->file->ha_delete_row(table->record[1]);
      if (error && error != HA_ERR_RECORD_IS_THE_SAME)
      {
	table->file->print_error(error,MYF(0)); /* purecov: inspected */
	result= -1;				/* purecov: inspected */
	goto end;				/* purecov: inspected */
      }
      else
        error= 0;
      grant_column= column_hash_search(g_t, column->column.ptr(),
                                       column->column.length());
      if (grant_column)				// Should always be true
	grant_column->rights= privileges;	// Update hash
    }
    else					// new grant
    {
      GRANT_COLUMN *grant_column;
      if ((error=table->file->ha_write_row(table->record[0])))
      {
	table->file->print_error(error,MYF(0)); /* purecov: inspected */
	result= -1;				/* purecov: inspected */
	goto end;				/* purecov: inspected */
      }
      grant_column= new GRANT_COLUMN(column->column,privileges);
      if (my_hash_insert(&g_t->hash_columns,(uchar*) grant_column))
      {
        result= -1;
        goto end;
      }
    }
  }

  /*
    If revoke of privileges on the table level, remove all such privileges
    for all columns
  */

  if (revoke_grant)
  {
    uchar user_key[MAX_KEY_LENGTH];
    key_copy(user_key, table->record[0], table->key_info,
             key_prefix_length);

    if (table->file->ha_index_read_map(table->record[0], user_key,
                                       (key_part_map)15,
                                       HA_READ_KEY_EXACT))
      goto end;

    /* Scan through all rows with the same host,db,user and table */
    do
    {
      ulong privileges = (ulong) table->field[6]->val_int();
      privileges=fix_rights_for_column(privileges);
      store_record(table,record[1]);

      if (privileges & rights)	// is in this record the priv to be revoked ??
      {
	GRANT_COLUMN *grant_column = NULL;
	char  colum_name_buf[HOSTNAME_LENGTH+1];
	String column_name(colum_name_buf,sizeof(colum_name_buf),
                           system_charset_info);

	privileges&= ~rights;
	table->field[6]->store((longlong)
			       get_rights_for_column(privileges), TRUE);
	table->field[4]->val_str(&column_name);
	grant_column = column_hash_search(g_t,
					  column_name.ptr(),
					  column_name.length());
	if (privileges)
	{
	  int tmp_error;
	  if ((tmp_error=table->file->ha_update_row(table->record[1],
						    table->record[0])) &&
              tmp_error != HA_ERR_RECORD_IS_THE_SAME)
	  {					/* purecov: deadcode */
	    table->file->print_error(tmp_error,MYF(0)); /* purecov: deadcode */
	    result= -1;				/* purecov: deadcode */
	    goto end;				/* purecov: deadcode */
	  }
	  if (grant_column)
          {
            grant_column->rights  = privileges; // Update hash
            grant_column->init_rights = privileges;
          }
	}
	else
	{
	  int tmp_error;
	  if ((tmp_error = table->file->ha_delete_row(table->record[1])))
	  {					/* purecov: deadcode */
	    table->file->print_error(tmp_error,MYF(0)); /* purecov: deadcode */
	    result= -1;				/* purecov: deadcode */
	    goto end;				/* purecov: deadcode */
	  }
	  if (grant_column)
	    my_hash_delete(&g_t->hash_columns,(uchar*) grant_column);
	}
      }
    } while (!table->file->ha_index_next(table->record[0]) &&
	     !key_cmp_if_same(table, key, 0, key_prefix_length));
  }

end:
  table->file->ha_index_end();
  DBUG_RETURN(result);
}

static inline void get_grantor(THD *thd, char *grantor)
{
  const char *user= thd->security_ctx->user;
  const char *host= thd->security_ctx->host_or_ip;

#if defined(HAVE_REPLICATION)
  if (thd->slave_thread && thd->has_invoker())
  {
    user= thd->get_invoker_user().str;
    host= thd->get_invoker_host().str;
  }
#endif
  strxmov(grantor, user, "@", host, NullS);
}

static int replace_table_table(THD *thd, GRANT_TABLE *grant_table,
			       TABLE *table, const LEX_USER &combo,
			       const char *db, const char *table_name,
			       ulong rights, ulong col_rights,
			       bool revoke_grant)
{
  char grantor[USER_HOST_BUFF_SIZE];
  int old_row_exists = 1;
  int error=0;
  ulong store_table_rights, store_col_rights;
  uchar user_key[MAX_KEY_LENGTH];
  DBUG_ENTER("replace_table_table");

  get_grantor(thd, grantor);
  /*
    The following should always succeed as new users are created before
    this function is called!
  */
  if (!find_user_wild(combo.host.str,combo.user.str))
  {
    if (!combo.host.length && !find_acl_role(combo.user.str))
    {
      my_message(ER_PASSWORD_NO_MATCH, ER_THD(thd, ER_PASSWORD_NO_MATCH),
                 MYF(0)); /* purecov: deadcode */
      DBUG_RETURN(-1);                            /* purecov: deadcode */
    }
  }

  table->use_all_columns();
  restore_record(table, s->default_values);     // Get empty record
  table->field[0]->store(combo.host.str,combo.host.length,
                         system_charset_info);
  table->field[1]->store(db,(uint) strlen(db), system_charset_info);
  table->field[2]->store(combo.user.str,combo.user.length,
                         system_charset_info);
  table->field[3]->store(table_name,(uint) strlen(table_name),
                         system_charset_info);
  store_record(table,record[1]);			// store at pos 1
  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);

  if (table->file->ha_index_read_idx_map(table->record[0], 0, user_key,
                                         HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
  {
    /*
      The following should never happen as we first check the in memory
      grant tables for the user.  There is however always a small change that
      the user has modified the grant tables directly.
    */
    if (revoke_grant)
    { // no row, no revoke
      my_error(ER_NONEXISTING_TABLE_GRANT, MYF(0),
               combo.user.str, combo.host.str,
               table_name);		        /* purecov: deadcode */
      DBUG_RETURN(-1);				/* purecov: deadcode */
    }
    old_row_exists = 0;
    restore_record(table,record[1]);			// Get saved record
  }

  store_table_rights= get_rights_for_table(rights);
  store_col_rights=   get_rights_for_column(col_rights);
  if (old_row_exists)
  {
    ulong j,k;
    store_record(table,record[1]);
    j = (ulong) table->field[6]->val_int();
    k = (ulong) table->field[7]->val_int();

    if (revoke_grant)
    {
      /* column rights are already fixed in mysql_table_grant */
      store_table_rights=j & ~store_table_rights;
    }
    else
    {
      store_table_rights|= j;
      store_col_rights|=   k;
    }
  }

  table->field[4]->store(grantor,(uint) strlen(grantor), system_charset_info);
  table->field[6]->store((longlong) store_table_rights, TRUE);
  table->field[7]->store((longlong) store_col_rights, TRUE);
  rights=fix_rights_for_table(store_table_rights);
  col_rights=fix_rights_for_column(store_col_rights);

  if (old_row_exists)
  {
    if (store_table_rights || store_col_rights)
    {
      if ((error=table->file->ha_update_row(table->record[1],
                                            table->record[0])) &&
          error != HA_ERR_RECORD_IS_THE_SAME)
	goto table_error;			/* purecov: deadcode */
    }
    else if ((error = table->file->ha_delete_row(table->record[1])))
      goto table_error;				/* purecov: deadcode */
  }
  else
  {
    error=table->file->ha_write_row(table->record[0]);
    if (table->file->is_fatal_error(error, HA_CHECK_DUP_KEY))
      goto table_error;				/* purecov: deadcode */
  }

  if (rights | col_rights)
  {
    grant_table->init_privs= rights;
    grant_table->init_cols=  col_rights;

    grant_table->privs= rights;
    grant_table->cols=	col_rights;
  }
  else
  {
    my_hash_delete(&column_priv_hash,(uchar*) grant_table);
  }
  DBUG_RETURN(0);

  /* This should never happen */
table_error:
  table->file->print_error(error,MYF(0)); /* purecov: deadcode */
  DBUG_RETURN(-1); /* purecov: deadcode */
}


/**
  @retval       0  success
  @retval      -1  error
*/
static int replace_routine_table(THD *thd, GRANT_NAME *grant_name,
			      TABLE *table, const LEX_USER &combo,
			      const char *db, const char *routine_name,
			      bool is_proc, ulong rights, bool revoke_grant)
{
  char grantor[USER_HOST_BUFF_SIZE];
  int old_row_exists= 1;
  int error=0;
  ulong store_proc_rights;
  HASH *hash= is_proc ? &proc_priv_hash : &func_priv_hash;
  DBUG_ENTER("replace_routine_table");

  if (revoke_grant && !grant_name->init_privs) // only inherited role privs
  {
    my_hash_delete(hash, (uchar*) grant_name);
    DBUG_RETURN(0);
  }

  get_grantor(thd, grantor);
  /*
    New users are created before this function is called.

    There may be some cases where a routine's definer is removed but the
    routine remains.
  */

  table->use_all_columns();
  restore_record(table, s->default_values);		// Get empty record
  table->field[0]->store(combo.host.str,combo.host.length, &my_charset_latin1);
  table->field[1]->store(db,(uint) strlen(db), &my_charset_latin1);
  table->field[2]->store(combo.user.str,combo.user.length, &my_charset_latin1);
  table->field[3]->store(routine_name,(uint) strlen(routine_name),
                         &my_charset_latin1);
  table->field[4]->store((longlong)(is_proc ?
                                    TYPE_ENUM_PROCEDURE : TYPE_ENUM_FUNCTION),
                         TRUE);
  store_record(table,record[1]);			// store at pos 1

  if (table->file->ha_index_read_idx_map(table->record[0], 0,
                                         (uchar*) table->field[0]->ptr,
                                         HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
  {
    /*
      The following should never happen as we first check the in memory
      grant tables for the user.  There is however always a small change that
      the user has modified the grant tables directly.

      Also, there is also a second posibility that this routine entry
      is created for a role by being inherited from a granted role.
    */
    if (revoke_grant)
    { // no row, no revoke
      my_error(ER_NONEXISTING_PROC_GRANT, MYF(0),
               combo.user.str, combo.host.str, routine_name);
      DBUG_RETURN(-1);
    }
    old_row_exists= 0;
    restore_record(table,record[1]);			// Get saved record
  }

  store_proc_rights= get_rights_for_procedure(rights);
  if (old_row_exists)
  {
    ulong j;
    store_record(table,record[1]);
    j= (ulong) table->field[6]->val_int();

    if (revoke_grant)
    {
      /* column rights are already fixed in mysql_table_grant */
      store_proc_rights=j & ~store_proc_rights;
    }
    else
    {
      store_proc_rights|= j;
    }
  }

  table->field[5]->store(grantor,(uint) strlen(grantor), &my_charset_latin1);
  table->field[6]->store((longlong) store_proc_rights, TRUE);
  rights=fix_rights_for_procedure(store_proc_rights);

  if (old_row_exists)
  {
    if (store_proc_rights)
    {
      if ((error=table->file->ha_update_row(table->record[1],
                                            table->record[0])) &&
          error != HA_ERR_RECORD_IS_THE_SAME)
	goto table_error;
    }
    else if ((error= table->file->ha_delete_row(table->record[1])))
      goto table_error;
  }
  else
  {
    error=table->file->ha_write_row(table->record[0]);
    if (table->file->is_fatal_error(error, HA_CHECK_DUP_KEY))
      goto table_error;
  }

  if (rights)
  {
    grant_name->init_privs= rights;
    grant_name->privs= rights;
  }
  else
  {
    my_hash_delete(hash, (uchar*) grant_name);
  }
  DBUG_RETURN(0);

  /* This should never happen */
table_error:
  table->file->print_error(error,MYF(0));
  DBUG_RETURN(-1);
}


/*****************************************************************
  Role privilege propagation and graph traversal functionality

  According to the SQL standard, a role can be granted to a role,
  thus role grants can create an arbitrarily complex directed acyclic
  graph (a standard explicitly specifies that cycles are not allowed).

  When a privilege is granted to a role, it becomes available to all grantees.
  The code below recursively traverses a DAG of role grants, propagating
  privilege changes.

  The traversal function can work both ways, from roles to grantees or
  from grantees to roles. The first is used for privilege propagation,
  the second - for SHOW GRANTS and I_S.APPLICABLE_ROLES

  The role propagation code is smart enough to propagate only privilege
  changes to one specific database, table, or routine, if only they
  were changed (like in GRANT ... ON ... TO ...) or it can propagate
  everything (on startup or after FLUSH PRIVILEGES).

  It traverses only a subgraph that's accessible from the modified role,
  only visiting roles that can be possibly affected by the GRANT statement.

  Additionally, it stops traversal early, if this particular GRANT statement
  didn't result in any changes of privileges (e.g. both role1 and role2
  are granted to the role3, both role1 and role2 have SELECT privilege.
  if SELECT is revoked from role1 it won't change role3 privileges,
  so we won't traverse from role3 to its grantees).
******************************************************************/
struct PRIVS_TO_MERGE
{
  enum what { ALL, GLOBAL, DB, TABLE_COLUMN, PROC, FUNC } what;
  const char *db, *name;
};

static int init_role_for_merging(ACL_ROLE *role, void *context)
{
  role->counter= 0;
  return 0;
}

static int count_subgraph_nodes(ACL_ROLE *role, ACL_ROLE *grantee, void *context)
{
  grantee->counter++;
  return 0;
}

static int merge_role_privileges(ACL_ROLE *, ACL_ROLE *, void *);

/**
  rebuild privileges of all affected roles

  entry point into role privilege propagation. after privileges of the
  'role' were changed, this function rebuilds privileges of all affected roles
  as necessary.
*/
static void propagate_role_grants(ACL_ROLE *role,
                                  enum PRIVS_TO_MERGE::what what,
                                  const char *db= 0, const char *name= 0)
{

  mysql_mutex_assert_owner(&acl_cache->lock);
  PRIVS_TO_MERGE data= { what, db, name };

  /*
     Changing privileges of a role causes all other roles that had
     this role granted to them to have their rights invalidated.

     We need to rebuild all roles' related access bits.

     This cannot be a simple depth-first search, instead we have to merge
     privieges for all roles granted to a specific grantee, *before*
     merging privileges for this grantee. In other words, we must visit all
     parent nodes of a specific node, before descencing into this node.

     For example, if role1 is granted to role2 and role3, and role3 is
     granted to role2, after "GRANT ... role1", we cannot merge privileges
     for role2, until role3 is merged.  The counter will be 0 for role1, 2
     for role2, 1 for role3. Traversal will start from role1, go to role2,
     decrement the counter, backtrack, go to role3, merge it, go to role2
     again, merge it.

     And the counter is not just "all parent nodes", but only parent nodes
     that are part of the subgraph we're interested in. For example, if
     both roleA and roleB are granted to roleC, then roleC has two parent
     nodes. But when granting a privilege to roleA, we're only looking at a
     subgraph that includes roleA and roleC (roleB cannot be possibly
     affected by that grant statement). In this subgraph roleC has only one
     parent.

     (on the other hand, in acl_load we want to update all roles, and
     the counter is exactly equal to the number of all parent nodes)

     Thus, we do two graph traversals here. First we only count parents
     that are part of the subgraph. On the second traversal we decrement
     the counter and actually merge privileges for a node when a counter
     drops to zero.
  */
  traverse_role_graph_up(role, &data, init_role_for_merging, count_subgraph_nodes);
  traverse_role_graph_up(role, &data, NULL, merge_role_privileges);
}


// State of a node during a Depth First Search exploration
struct NODE_STATE
{
  ACL_USER_BASE *node_data; /* pointer to the node data */
  uint neigh_idx;           /* the neighbour that needs to be evaluated next */
};

/**
  Traverse the role grant graph and invoke callbacks at the specified points. 
  
  @param user           user or role to start traversal from
  @param context        opaque parameter to pass to callbacks
  @param offset         offset to ACL_ROLE::parent_grantee or to
                        ACL_USER_BASE::role_grants. Depending on this value,
                        traversal will go from roles to grantees or from
                        grantees to roles.
  @param on_node        called when a node is visited for the first time.
                        Returning a value <0 will abort the traversal.
  @param on_edge        called for every edge in the graph, when traversal
                        goes from a node to a neighbour node.
                        Returning <0 will abort the traversal. Returning >0
                        will make the traversal not to follow this edge.

  @note
  The traverse method is a DEPTH FIRST SEARCH, but callbacks can influence
  that (on_edge returning >0 value).

  @note
  This function should not be called directly, use
  traverse_role_graph_up() and traverse_role_graph_down() instead.

  @retval 0                 traversal finished successfully
  @retval ROLE_CYCLE_FOUND  traversal aborted, cycle detected
  @retval <0                traversal was aborted, because a callback returned
                            this error code
*/
static int traverse_role_graph_impl(ACL_USER_BASE *user, void *context,
       off_t offset,
       int (*on_node) (ACL_USER_BASE *role, void *context),
       int (*on_edge) (ACL_USER_BASE *current, ACL_ROLE *neighbour, void *context))
{
  DBUG_ENTER("traverse_role_graph_impl");
  DBUG_ASSERT(user);
  DBUG_PRINT("enter",("role: '%s'", user->user.str));
  /*
     The search operation should always leave the ROLE_ON_STACK and
     ROLE_EXPLORED flags clean for all nodes involved in the search
  */
  DBUG_ASSERT(!(user->flags & ROLE_ON_STACK));
  DBUG_ASSERT(!(user->flags & ROLE_EXPLORED));
  mysql_mutex_assert_owner(&acl_cache->lock);

  /*
     Stack used to simulate the recursive calls of DFS.
     It uses a Dynamic_array to reduce the number of
     malloc calls to a minimum
  */
  Dynamic_array<NODE_STATE> stack(20,50);
  Dynamic_array<ACL_USER_BASE *> to_clear(20,50);
  NODE_STATE state;     /* variable used to insert elements in the stack */
  int result= 0;

  state.neigh_idx= 0;
  state.node_data= user;
  user->flags|= ROLE_ON_STACK;

  stack.push(state);
  to_clear.push(user);

  user->flags|= ROLE_OPENED;
  if (on_node && ((result= on_node(user, context)) < 0))
    goto end;

  while (stack.elements())
  {
    NODE_STATE *curr_state= stack.back();

    DBUG_ASSERT(curr_state->node_data->flags & ROLE_ON_STACK);

    ACL_USER_BASE *current= curr_state->node_data;
    ACL_USER_BASE *neighbour= NULL;
    DBUG_PRINT("info", ("Examining role %s", current->user.str));
    /*
      Iterate through the neighbours until a first valid jump-to
      neighbour is found
    */
    bool found= FALSE;
    uint i;
    DYNAMIC_ARRAY *array= (DYNAMIC_ARRAY *)(((char*)current) + offset);

    DBUG_ASSERT(array == &current->role_grants || current->flags & IS_ROLE);
    for (i= curr_state->neigh_idx; i < array->elements; i++)
    {
      neighbour= *(dynamic_element(array, i, ACL_ROLE**));
      if (!(neighbour->flags & IS_ROLE))
        continue;

      DBUG_PRINT("info", ("Examining neighbour role %s", neighbour->user.str));

      /* check if it forms a cycle */
      if (neighbour->flags & ROLE_ON_STACK)
      {
        DBUG_PRINT("info", ("Found cycle"));
        result= ROLE_CYCLE_FOUND;
        goto end;
      }

      if (!(neighbour->flags & ROLE_OPENED))
      {
        neighbour->flags|= ROLE_OPENED;
        to_clear.push(neighbour);
        if (on_node && ((result= on_node(neighbour, context)) < 0))
          goto end;
      }

      if (on_edge)
      {
        result= on_edge(current, (ACL_ROLE*)neighbour, context);
        if (result < 0)
          goto end;
        if (result > 0)
          continue;
      }

      /* Check if it was already explored, in that case, move on */
      if (neighbour->flags & ROLE_EXPLORED)
        continue;

      found= TRUE;
      break;
    }

    /* found states that we have found a node to jump next into */
    if (found)
    {
      curr_state->neigh_idx= i + 1;

      /* some sanity checks */
      DBUG_ASSERT(!(neighbour->flags & ROLE_ON_STACK));

      /* add the neighbour on the stack */
      neighbour->flags|= ROLE_ON_STACK;
      state.neigh_idx= 0;
      state.node_data= neighbour;
      stack.push(state);
    }
    else
    {
      /* Make sure we got a correct node */
      DBUG_ASSERT(curr_state->node_data->flags & ROLE_ON_STACK);
      /* Finished with exploring the current node, pop it off the stack */
      curr_state= &stack.pop();
      curr_state->node_data->flags&= ~ROLE_ON_STACK; /* clear the on-stack bit */
      curr_state->node_data->flags|= ROLE_EXPLORED;
    }
  }

end:
  /* Cleanup */
  for (uint i= 0; i < to_clear.elements(); i++)
  {
    ACL_USER_BASE *current= to_clear.at(i);
    DBUG_ASSERT(current->flags & (ROLE_EXPLORED | ROLE_ON_STACK | ROLE_OPENED));
    current->flags&= ~(ROLE_EXPLORED | ROLE_ON_STACK | ROLE_OPENED);
  }
  DBUG_RETURN(result);
}

/**
  Traverse the role grant graph, going from a role to its grantees.

  This is used to propagate changes in privileges, for example,
  when GRANT or REVOKE is issued for a role.
*/

static int traverse_role_graph_up(ACL_ROLE *role, void *context,
       int (*on_node) (ACL_ROLE *role, void *context),
       int (*on_edge) (ACL_ROLE *current, ACL_ROLE *neighbour, void *context))
{
  return traverse_role_graph_impl(role, context,
                    my_offsetof(ACL_ROLE, parent_grantee),
                    (int (*)(ACL_USER_BASE *, void *))on_node,
                    (int (*)(ACL_USER_BASE *, ACL_ROLE *, void *))on_edge);
}

/**
  Traverse the role grant graph, going from a user or a role to granted roles.

  This is used, for example, to print all grants available to a user or a role
  (as in SHOW GRANTS).
*/

static int traverse_role_graph_down(ACL_USER_BASE *user, void *context,
       int (*on_node) (ACL_USER_BASE *role, void *context),
       int (*on_edge) (ACL_USER_BASE *current, ACL_ROLE *neighbour, void *context))
{
  return traverse_role_graph_impl(user, context,
                             my_offsetof(ACL_USER_BASE, role_grants),
                             on_node, on_edge);
}

/*
  To find all db/table/routine privilege for a specific role
  we need to scan the array of privileges. It can be big.
  But the set of privileges granted to a role in question (or
  to roles directly granted to the role in question) is supposedly
  much smaller.

  We put a role and all roles directly granted to it in a hash, and iterate
  the (suposedly long) array of privileges, filtering out "interesting"
  entries using the role hash. We put all these "interesting"
  entries in a (suposedly small) dynamic array and them use it for merging.
*/
static uchar* role_key(const ACL_ROLE *role, size_t *klen, my_bool)
{
  *klen= role->user.length;
  return (uchar*) role->user.str;
}
typedef Hash_set<ACL_ROLE> role_hash_t;

static bool merge_role_global_privileges(ACL_ROLE *grantee)
{
  ulong old= grantee->access;
  grantee->access= grantee->initial_role_access;

  DBUG_EXECUTE_IF("role_merge_stats", role_global_merges++;);

  for (uint i= 0; i < grantee->role_grants.elements; i++)
  {
    ACL_ROLE *r= *dynamic_element(&grantee->role_grants, i, ACL_ROLE**);
    grantee->access|= r->access;
  }
  return old != grantee->access;
}

static int db_name_sort(ACL_DB * const *db1, ACL_DB * const *db2)
{
  return strcmp((*db1)->db, (*db2)->db);
}

/**
  update ACL_DB for given database and a given role with merged privileges

  @param merged ACL_DB of the role in question (or NULL if it wasn't found)
  @param first  first ACL_DB in an array for the database in question
  @param access new privileges for the given role on the gived database
  @param role   the name of the given role

  @return a bitmap of
          1 - privileges were changed
          2 - ACL_DB was added
          4 - ACL_DB was deleted
*/
static int update_role_db(ACL_DB *merged, ACL_DB **first, ulong access, char *role)
{
  if (!first)
    return 0;

  DBUG_EXECUTE_IF("role_merge_stats", role_db_merges++;);

  if (merged == NULL)
  {
    /*
      there's no ACL_DB for this role (all db grants come from granted roles)
      we need to create it

      Note that we cannot use acl_insert_db() now:
      1. it'll sort elements in the acl_dbs, so the pointers will become invalid
      2. we may need many of them, no need to sort every time
    */
    DBUG_ASSERT(access);
    ACL_DB acl_db;
    acl_db.user= role;
    acl_db.host.hostname= const_cast<char*>("");
    acl_db.host.ip= acl_db.host.ip_mask= 0;
    acl_db.db= first[0]->db;
    acl_db.access= access;
    acl_db.initial_access= 0;
    acl_db.sort=get_sort(3, "", acl_db.db, role);
    push_dynamic(&acl_dbs,(uchar*) &acl_db);
    return 2;
  }
  else if (access == 0)
  {
    /*
      there is ACL_DB but the role has no db privileges granted
      (all privileges were coming from granted roles, and now those roles
      were dropped or had their privileges revoked).
      we need to remove this ACL_DB entry

      Note, that we cannot delete now:
      1. it'll shift elements in the acl_dbs, so the pointers will become invalid
      2. it's O(N) operation, and we may need many of them
      so we only mark elements deleted and will delete later.
    */
    merged->sort= 0; // lower than any valid ACL_DB sort value, will be sorted last
    return 4;
  }
  else if (merged->access != access)
  {
    /* this is easy */
    merged->access= access;
    return 1;
  }
  return 0;
}

/**
  merges db privileges from roles granted to the role 'grantee'.

  @return true if database privileges of the 'grantee' were changed

*/
static bool merge_role_db_privileges(ACL_ROLE *grantee, const char *dbname,
                                     role_hash_t *rhash)
{
  Dynamic_array<ACL_DB *> dbs; 

  /*
    Supposedly acl_dbs can be huge, but only a handful of db grants
    apply to grantee or roles directly granted to grantee.

    Collect these applicable db grants.
  */
  for (uint i=0 ; i < acl_dbs.elements ; i++)
  {
    ACL_DB *db= dynamic_element(&acl_dbs,i,ACL_DB*);
    if (db->host.hostname[0])
      continue;
    if (dbname && strcmp(db->db, dbname))
      continue;
    ACL_ROLE *r= rhash->find(db->user, strlen(db->user));
    if (!r)
      continue;
    dbs.append(db);
  }
  dbs.sort(db_name_sort);

  /*
    Because dbs array is sorted by the db name, all grants for the same db
    (that should be merged) are sorted together. The grantee's ACL_DB element
    is not necessarily the first and may be not present at all.
  */
  ACL_DB **first= NULL, *UNINIT_VAR(merged);
  ulong UNINIT_VAR(access), update_flags= 0;
  for (ACL_DB **cur= dbs.front(); cur <= dbs.back(); cur++)
  {
    if (!first || (!dbname && strcmp(cur[0]->db, cur[-1]->db)))
    { // new db name series
      update_flags|= update_role_db(merged, first, access, grantee->user.str);
      merged= NULL;
      access= 0;
      first= cur;
    }
    if (strcmp(cur[0]->user, grantee->user.str) == 0)
      access|= (merged= cur[0])->initial_access;
    else
      access|= cur[0]->access;
  }
  update_flags|= update_role_db(merged, first, access, grantee->user.str);

  /*
    to make this code a bit simpler, we sort on deletes, to move
    deleted elements to the end of the array. strictly speaking it's
    unnecessary, it'd be faster to remove them in one O(N) array scan.
    
    on the other hand, qsort on almost sorted array is pretty fast anyway...
  */
  if (update_flags & (2|4))
  { // inserted or deleted, need to sort
    my_qsort((uchar*) dynamic_element(&acl_dbs,0,ACL_DB*),acl_dbs.elements,
             sizeof(ACL_DB),(qsort_cmp) acl_compare);
  }
  if (update_flags & 4)
  { // deleted, trim the end
    while (acl_dbs.elements &&
           dynamic_element(&acl_dbs, acl_dbs.elements-1, ACL_DB*)->sort == 0)
      acl_dbs.elements--;
  }
  return update_flags;
}

static int table_name_sort(GRANT_TABLE * const *tbl1, GRANT_TABLE * const *tbl2)
{
  int res = strcmp((*tbl1)->db, (*tbl2)->db);
  if (res) return res;
  return strcmp((*tbl1)->tname, (*tbl2)->tname);
}

/**
  merges column privileges for the entry 'merged'

  @param merged GRANT_TABLE to merge the privileges into
  @param cur    first entry in the array of GRANT_TABLE's for a given table
  @param last   last entry in the array of GRANT_TABLE's for a given table,
                all entries between cur and last correspond to the *same* table

  @return 1 if the _set of columns_ in 'merged' was changed
          (not if the _set of privileges_ was changed).
*/
static int update_role_columns(GRANT_TABLE *merged,
                               GRANT_TABLE **cur, GRANT_TABLE **last)

{
  ulong rights __attribute__((unused))= 0;
  int changed= 0;
  if (!merged->cols)
  {
    changed= merged->hash_columns.records > 0;
    my_hash_reset(&merged->hash_columns);
    return changed;
  }

  DBUG_EXECUTE_IF("role_merge_stats", role_column_merges++;);

  HASH *mh= &merged->hash_columns;
  for (uint i=0 ; i < mh->records ; i++)
  {
    GRANT_COLUMN *col = (GRANT_COLUMN *)my_hash_element(mh, i);
    col->rights= col->init_rights;
  }

  for (; cur < last; cur++)
  {
    if (*cur == merged)
      continue;
    HASH *ch= &cur[0]->hash_columns;
    for (uint i=0 ; i < ch->records ; i++)
    {
      GRANT_COLUMN *ccol = (GRANT_COLUMN *)my_hash_element(ch, i);
      GRANT_COLUMN *mcol = (GRANT_COLUMN *)my_hash_search(mh,
                                  (uchar *)ccol->column, ccol->key_length);
      if (mcol)
        mcol->rights|= ccol->rights;
      else
      {
        changed= 1;
        my_hash_insert(mh, (uchar*)new (&grant_memroot) GRANT_COLUMN(ccol));
      }
    }
  }

  for (uint i=0 ; i < mh->records ; i++)
  {
    GRANT_COLUMN *col = (GRANT_COLUMN *)my_hash_element(mh, i);
    rights|= col->rights;
    if (!col->rights)
    {
      changed= 1;
      my_hash_delete(mh, (uchar*)col);
    }
  }
  DBUG_ASSERT(rights == merged->cols);
  return changed;
}

/**
  update GRANT_TABLE for a given table and a given role with merged privileges

  @param merged GRANT_TABLE of the role in question (or NULL if it wasn't found)
  @param first  first GRANT_TABLE in an array for the table in question
  @param last   last entry in the array of GRANT_TABLE's for a given table,
                all entries between first and last correspond to the *same* table
  @param privs  new table-level privileges for 'merged'
  @param cols   new OR-ed column-level privileges for 'merged'
  @param role   the name of the given role

  @return a bitmap of
          1 - privileges were changed
          2 - GRANT_TABLE was added
          4 - GRANT_TABLE was deleted
*/
static int update_role_table_columns(GRANT_TABLE *merged,
                                      GRANT_TABLE **first, GRANT_TABLE **last,
                                      ulong privs, ulong cols, char *role)
{
  if (!first)
    return 0;

  DBUG_EXECUTE_IF("role_merge_stats", role_table_merges++;);

  if (merged == NULL)
  {
    /*
      there's no GRANT_TABLE for this role (all table grants come from granted
      roles) we need to create it
    */
    DBUG_ASSERT(privs | cols);
    merged= new (&grant_memroot) GRANT_TABLE("", first[0]->db, role, first[0]->tname,
                                     privs, cols);
    merged->init_privs= merged->init_cols= 0;
    update_role_columns(merged, first, last);
    my_hash_insert(&column_priv_hash,(uchar*) merged);
    return 2;
  }
  else if ((privs | cols) == 0)
  {
    /*
      there is GRANT_TABLE object but the role has no table or column
      privileges granted (all privileges were coming from granted roles, and
      now those roles were dropped or had their privileges revoked).
      we need to remove this GRANT_TABLE
    */
    DBUG_EXECUTE_IF("role_merge_stats",
                    role_column_merges+= MY_TEST(merged->cols););
    my_hash_delete(&column_priv_hash,(uchar*) merged);
    return 4;
  }
  else
  {
    bool changed= merged->cols != cols || merged->privs != privs;
    merged->cols= cols;
    merged->privs= privs;
    if (update_role_columns(merged, first, last))
      changed= true;
    return changed;
  }
}

/**
  merges table privileges from roles granted to the role 'grantee'.

  @return true if table privileges of the 'grantee' were changed

*/
static bool merge_role_table_and_column_privileges(ACL_ROLE *grantee,
                        const char *db, const char *tname, role_hash_t *rhash)
{
  Dynamic_array<GRANT_TABLE *> grants;
  DBUG_ASSERT(MY_TEST(db) == MY_TEST(tname)); // both must be set, or neither

  /*
    first, collect table/column privileges granted to
    roles in question.
  */
  for (uint i=0 ; i < column_priv_hash.records ; i++)
  {
    GRANT_TABLE *grant= (GRANT_TABLE *) my_hash_element(&column_priv_hash, i);
    if (grant->host.hostname[0])
      continue;
    if (tname && (strcmp(grant->db, db) || strcmp(grant->tname, tname)))
      continue;
    ACL_ROLE *r= rhash->find(grant->user, strlen(grant->user));
    if (!r)
      continue;
    grants.append(grant);
  }
  grants.sort(table_name_sort);

  GRANT_TABLE **first= NULL, *UNINIT_VAR(merged), **cur;
  ulong UNINIT_VAR(privs), UNINIT_VAR(cols), update_flags= 0;
  for (cur= grants.front(); cur <= grants.back(); cur++)
  {
    if (!first ||
        (!tname && (strcmp(cur[0]->db, cur[-1]->db) ||
                   strcmp(cur[0]->tname, cur[-1]->tname))))
    { // new db.tname series
      update_flags|= update_role_table_columns(merged, first, cur,
                                               privs, cols, grantee->user.str);
      merged= NULL;
      privs= cols= 0;
      first= cur;
    }
    if (strcmp(cur[0]->user, grantee->user.str) == 0)
    {
      merged= cur[0];
      cols|= cur[0]->init_cols;
      privs|= cur[0]->init_privs;
    }
    else
    {
      cols|= cur[0]->cols;
      privs|= cur[0]->privs;
    }
  }
  update_flags|= update_role_table_columns(merged, first, cur,
                                           privs, cols, grantee->user.str);

  return update_flags;
}

static int routine_name_sort(GRANT_NAME * const *r1, GRANT_NAME * const *r2)
{
  int res= strcmp((*r1)->db, (*r2)->db);
  if (res) return res;
  return strcmp((*r1)->tname, (*r2)->tname);
}

/**
  update GRANT_NAME for a given routine and a given role with merged privileges

  @param merged GRANT_NAME of the role in question (or NULL if it wasn't found)
  @param first  first GRANT_NAME in an array for the routine in question
  @param privs  new routine-level privileges for 'merged'
  @param role   the name of the given role
  @param hash   proc_priv_hash or func_priv_hash

  @return a bitmap of
          1 - privileges were changed
          2 - GRANT_NAME was added
          4 - GRANT_NAME was deleted
*/
static int update_role_routines(GRANT_NAME *merged, GRANT_NAME **first,
                                ulong privs, char *role, HASH *hash)
{
  if (!first)
    return 0;

  DBUG_EXECUTE_IF("role_merge_stats", role_routine_merges++;);

  if (merged == NULL)
  {
    /*
      there's no GRANT_NAME for this role (all routine grants come from granted
      roles) we need to create it
    */
    DBUG_ASSERT(privs);
    merged= new (&grant_memroot) GRANT_NAME("", first[0]->db, role, first[0]->tname,
                                    privs, true);
    merged->init_privs= 0; // all privs are inherited
    my_hash_insert(hash, (uchar *)merged);
    return 2;
  }
  else if (privs == 0)
  {
    /*
      there is GRANT_NAME but the role has no privileges granted
      (all privileges were coming from granted roles, and now those roles
      were dropped or had their privileges revoked).
      we need to remove this entry
    */
    my_hash_delete(hash, (uchar*)merged);
    return 4;
  }
  else if (merged->privs != privs)
  {
    /* this is easy */
    merged->privs= privs;
    return 1;
  }
  return 0;
}

/**
  merges routine privileges from roles granted to the role 'grantee'.

  @return true if routine privileges of the 'grantee' were changed

*/
static bool merge_role_routine_grant_privileges(ACL_ROLE *grantee,
            const char *db, const char *tname, role_hash_t *rhash, HASH *hash)
{
  ulong update_flags= 0;

  DBUG_ASSERT(MY_TEST(db) == MY_TEST(tname)); // both must be set, or neither

  Dynamic_array<GRANT_NAME *> grants; 

  /* first, collect routine privileges granted to roles in question */
  for (uint i=0 ; i < hash->records ; i++)
  {
    GRANT_NAME *grant= (GRANT_NAME *) my_hash_element(hash, i);
    if (grant->host.hostname[0])
      continue;
    if (tname && (strcmp(grant->db, db) || strcmp(grant->tname, tname)))
      continue;
    ACL_ROLE *r= rhash->find(grant->user, strlen(grant->user));
    if (!r)
      continue;
    grants.append(grant);
  }
  grants.sort(routine_name_sort);

  GRANT_NAME **first= NULL, *UNINIT_VAR(merged);
  ulong UNINIT_VAR(privs);
  for (GRANT_NAME **cur= grants.front(); cur <= grants.back(); cur++)
  {
    if (!first ||
        (!tname && (strcmp(cur[0]->db, cur[-1]->db) ||
                    strcmp(cur[0]->tname, cur[-1]->tname))))
    { // new db.tname series
      update_flags|= update_role_routines(merged, first, privs,
                                          grantee->user.str, hash);
      merged= NULL;
      privs= 0;
      first= cur;
    }
    if (strcmp(cur[0]->user, grantee->user.str) == 0)
    {
      merged= cur[0];
      privs|= cur[0]->init_privs;
    }
    else
    {
      privs|= cur[0]->privs;
    }
  }
  update_flags|= update_role_routines(merged, first, privs,
                                      grantee->user.str, hash);
  return update_flags;
}

/**
  update privileges of the 'grantee' from all roles, granted to it
*/
static int merge_role_privileges(ACL_ROLE *role __attribute__((unused)),
                                 ACL_ROLE *grantee, void *context)
{
  PRIVS_TO_MERGE *data= (PRIVS_TO_MERGE *)context;

  if (--grantee->counter)
    return 1; // don't recurse into grantee just yet

  /* if we'll do db/table/routine privileges, create a hash of role names */
  role_hash_t role_hash(role_key);
  if (data->what != PRIVS_TO_MERGE::GLOBAL)
  {
    role_hash.insert(grantee);
    for (uint i= 0; i < grantee->role_grants.elements; i++)
      role_hash.insert(*dynamic_element(&grantee->role_grants, i, ACL_ROLE**));
  }

  bool all= data->what == PRIVS_TO_MERGE::ALL;
  bool changed= false;
  if (all || data->what == PRIVS_TO_MERGE::GLOBAL)
    changed|= merge_role_global_privileges(grantee);
  if (all || data->what == PRIVS_TO_MERGE::DB)
    changed|= merge_role_db_privileges(grantee, data->db, &role_hash);
  if (all || data->what == PRIVS_TO_MERGE::TABLE_COLUMN)
    changed|= merge_role_table_and_column_privileges(grantee,
                                             data->db, data->name, &role_hash);
  if (all || data->what == PRIVS_TO_MERGE::PROC)
    changed|= merge_role_routine_grant_privileges(grantee,
                            data->db, data->name, &role_hash, &proc_priv_hash);
  if (all || data->what == PRIVS_TO_MERGE::FUNC)
    changed|= merge_role_routine_grant_privileges(grantee,
                            data->db, data->name, &role_hash, &func_priv_hash);

  return !changed; // don't recurse into the subgraph if privs didn't change
}

static bool merge_one_role_privileges(ACL_ROLE *grantee)
{
  PRIVS_TO_MERGE data= { PRIVS_TO_MERGE::ALL, 0, 0 };
  grantee->counter= 1;
  return merge_role_privileges(0, grantee, &data);
}

/*****************************************************************
  End of the role privilege propagation and graph traversal code
******************************************************************/

static bool has_auth(LEX_USER *user, LEX *lex)
{
  return user->pwtext.length || user->pwhash.length || user->plugin.length || user->auth.length ||
         lex->ssl_type != SSL_TYPE_NOT_SPECIFIED || lex->ssl_cipher ||
         lex->x509_issuer || lex->x509_subject ||
         lex->mqh.specified_limits;
}

static bool fix_and_copy_user(LEX_USER *to, LEX_USER *from, THD *thd)
{
  if (to != from)
  {
    /* preserve authentication information, if LEX_USER was  reallocated */
    to->pwtext= from->pwtext;
    to->pwhash= from->pwhash;
    to->plugin= from->plugin;
    to->auth= from->auth;
  }

  if (fix_lex_user(thd, to))
    return true;

  return false;
}

static bool copy_and_check_auth(LEX_USER *to, LEX_USER *from, THD *thd)
{
  if (fix_and_copy_user(to, from, thd))
    return true;

  // if changing auth for an existing user
  if (has_auth(to, thd->lex) && find_user_exact(to->host.str, to->user.str))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    bool res= check_alter_user(thd, to->host.str, to->user.str);
    mysql_mutex_lock(&acl_cache->lock);
    return res;
  }

  return false;
}


/*
  Store table level and column level grants in the privilege tables

  SYNOPSIS
    mysql_table_grant()
    thd			Thread handle
    table_list		List of tables to give grant
    user_list		List of users to give grant
    columns		List of columns to give grant
    rights		Table level grant
    revoke_grant	Set to 1 if this is a REVOKE command

  RETURN
    FALSE ok
    TRUE  error
*/

int mysql_table_grant(THD *thd, TABLE_LIST *table_list,
		      List <LEX_USER> &user_list,
		      List <LEX_COLUMN> &columns, ulong rights,
		      bool revoke_grant)
{
  ulong column_priv= 0;
  int result;
  List_iterator <LEX_USER> str_list (user_list);
  LEX_USER *Str, *tmp_Str;
  TABLE_LIST tables[TABLES_MAX];
  bool create_new_users=0;
  char *db_name, *table_name;
  DBUG_ENTER("mysql_table_grant");

  if (rights & ~TABLE_ACLS)
  {
    my_message(ER_ILLEGAL_GRANT_FOR_TABLE,
               ER_THD(thd, ER_ILLEGAL_GRANT_FOR_TABLE),
               MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (!revoke_grant)
  {
    if (columns.elements)
    {
      class LEX_COLUMN *column;
      List_iterator <LEX_COLUMN> column_iter(columns);

      if (open_normal_and_derived_tables(thd, table_list, 0, DT_PREPARE))
        DBUG_RETURN(TRUE);

      while ((column = column_iter++))
      {
        uint unused_field_idx= NO_CACHED_FIELD_INDEX;
        TABLE_LIST *dummy;
        Field *f=find_field_in_table_ref(thd, table_list, column->column.ptr(),
                                         column->column.length(),
                                         column->column.ptr(), NULL, NULL,
                                         NULL, TRUE, FALSE,
                                         &unused_field_idx, FALSE, &dummy);
        if (f == (Field*)0)
        {
          my_error(ER_BAD_FIELD_ERROR, MYF(0),
                   column->column.c_ptr(), table_list->alias);
          DBUG_RETURN(TRUE);
        }
        if (f == (Field *)-1)
          DBUG_RETURN(TRUE);
        column_priv|= column->rights;
      }
      close_mysql_tables(thd);
    }
    else
    {
      if (!(rights & CREATE_ACL))
      {
        if (!ha_table_exists(thd, table_list->db, table_list->table_name, 0))
        {
          my_error(ER_NO_SUCH_TABLE, MYF(0), table_list->db, table_list->alias);
          DBUG_RETURN(TRUE);
        }
      }
      if (table_list->grant.want_privilege)
      {
        char command[128];
        get_privilege_desc(command, sizeof(command),
                           table_list->grant.want_privilege);
        my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                 command, thd->security_ctx->priv_user,
                 thd->security_ctx->host_or_ip, table_list->alias);
        DBUG_RETURN(-1);
      }
    }
  }

  /*
    Open the mysql.user and mysql.tables_priv tables.
    Don't open column table if we don't need it !
  */
  int maybe_columns_priv= 0;
  if (column_priv ||
      (revoke_grant && ((rights & COL_ACLS) || columns.elements)))
    maybe_columns_priv= Table_columns_priv;

  /*
    The lock api is depending on the thd->lex variable which needs to be
    re-initialized.
  */
  Query_tables_list backup;
  thd->lex->reset_n_backup_query_tables_list(&backup);
  /*
    Restore Query_tables_list::sql_command value, which was reset
    above, as the code writing query to the binary log assumes that
    this value corresponds to the statement being executed.
  */
  thd->lex->sql_command= backup.sql_command;

  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user |
                                 Table_tables_priv | maybe_columns_priv)))
  {
    thd->lex->restore_backup_query_tables_list(&backup);
    DBUG_RETURN(result != 1);
  }

  if (!revoke_grant)
    create_new_users= test_if_create_new_users(thd);
  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);
  MEM_ROOT *old_root= thd->mem_root;
  thd->mem_root= &grant_memroot;
  grant_version++;

  while ((tmp_Str = str_list++))
  {
    int error;
    GRANT_TABLE *grant_table;
    if (!(Str= get_current_user(thd, tmp_Str, false)))
    {
      result= TRUE;
      continue;
    }
    /* Create user if needed */
    error= copy_and_check_auth(Str, tmp_Str, thd) ||
           replace_user_table(thd, tables[USER_TABLE].table, *Str,
                               0, revoke_grant, create_new_users,
                               MY_TEST(thd->variables.sql_mode &
                                       MODE_NO_AUTO_CREATE_USER));
    if (error)
    {
      result= TRUE;				// Remember error
      continue;					// Add next user
    }

    db_name= table_list->get_db_name();
    table_name= table_list->get_table_name();

    /* Find/create cached table grant */
    grant_table= table_hash_search(Str->host.str, NullS, db_name,
				   Str->user.str, table_name, 1);
    if (!grant_table)
    {
      if (revoke_grant)
      {
	my_error(ER_NONEXISTING_TABLE_GRANT, MYF(0),
                 Str->user.str, Str->host.str, table_list->table_name);
	result= TRUE;
	continue;
      }
      grant_table = new GRANT_TABLE (Str->host.str, db_name,
				     Str->user.str, table_name,
				     rights,
				     column_priv);
      if (!grant_table ||
        my_hash_insert(&column_priv_hash,(uchar*) grant_table))
      {
	result= TRUE;				/* purecov: deadcode */
	continue;				/* purecov: deadcode */
      }
    }

    /* If revoke_grant, calculate the new column privilege for tables_priv */
    if (revoke_grant)
    {
      class LEX_COLUMN *column;
      List_iterator <LEX_COLUMN> column_iter(columns);
      GRANT_COLUMN *grant_column;

      /* Fix old grants */
      while ((column = column_iter++))
      {
	grant_column = column_hash_search(grant_table,
					  column->column.ptr(),
					  column->column.length());
	if (grant_column)
	  grant_column->rights&= ~(column->rights | rights);
      }
      /* scan trough all columns to get new column grant */
      column_priv= 0;
      for (uint idx=0 ; idx < grant_table->hash_columns.records ; idx++)
      {
        grant_column= (GRANT_COLUMN*)
          my_hash_element(&grant_table->hash_columns, idx);
	grant_column->rights&= ~rights;		// Fix other columns
	column_priv|= grant_column->rights;
      }
    }
    else
    {
      column_priv|= grant_table->cols;
    }


    /* update table and columns */

    if (replace_table_table(thd, grant_table, tables[TABLES_PRIV_TABLE].table,
                            *Str, db_name, table_name,
			    rights, column_priv, revoke_grant))
    {
      /* Should only happen if table is crashed */
      result= TRUE;			       /* purecov: deadcode */
    }
    else if (tables[COLUMNS_PRIV_TABLE].table)
    {
      if (replace_column_table(grant_table, tables[COLUMNS_PRIV_TABLE].table,
                               *Str, columns, db_name, table_name, rights,
                               revoke_grant))
      {
	result= TRUE;
      }
    }
    if (Str->is_role())
      propagate_role_grants(find_acl_role(Str->user.str),
                            PRIVS_TO_MERGE::TABLE_COLUMN, db_name, table_name);
  }

  thd->mem_root= old_root;
  mysql_mutex_unlock(&acl_cache->lock);

  if (!result) /* success */
  {
    result= write_bin_log(thd, TRUE, thd->query(), thd->query_length());
  }

  mysql_rwlock_unlock(&LOCK_grant);

  if (!result) /* success */
    my_ok(thd);

  thd->lex->restore_backup_query_tables_list(&backup);
  DBUG_RETURN(result);
}


/**
  Store routine level grants in the privilege tables

  @param thd Thread handle
  @param table_list List of routines to give grant
  @param is_proc Is this a list of procedures?
  @param user_list List of users to give grant
  @param rights Table level grant
  @param revoke_grant Is this is a REVOKE command?

  @return
    @retval FALSE Success.
    @retval TRUE An error occurred.
*/

bool mysql_routine_grant(THD *thd, TABLE_LIST *table_list, bool is_proc,
			 List <LEX_USER> &user_list, ulong rights,
			 bool revoke_grant, bool write_to_binlog)
{
  List_iterator <LEX_USER> str_list (user_list);
  LEX_USER *Str, *tmp_Str;
  TABLE_LIST tables[TABLES_MAX];
  bool create_new_users= 0, result;
  char *db_name, *table_name;
  DBUG_ENTER("mysql_routine_grant");

  if (rights & ~PROC_ACLS)
  {
    my_message(ER_ILLEGAL_GRANT_FOR_TABLE,
               ER_THD(thd, ER_ILLEGAL_GRANT_FOR_TABLE),
               MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (!revoke_grant)
  {
    if (sp_exist_routines(thd, table_list, is_proc))
      DBUG_RETURN(TRUE);
  }

  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user |
                                 Table_procs_priv)))
    DBUG_RETURN(result != 1);

  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());

  if (!revoke_grant)
    create_new_users= test_if_create_new_users(thd);
  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);
  MEM_ROOT *old_root= thd->mem_root;
  thd->mem_root= &grant_memroot;

  DBUG_PRINT("info",("now time to iterate and add users"));

  while ((tmp_Str= str_list++))
  {
    GRANT_NAME *grant_name;
    if (!(Str= get_current_user(thd, tmp_Str, false)))
    {
      result= TRUE;
      continue;
    }
    /* Create user if needed */
    if (copy_and_check_auth(Str, tmp_Str, thd) ||
        replace_user_table(thd, tables[USER_TABLE].table, *Str,
			   0, revoke_grant, create_new_users,
                           MY_TEST(thd->variables.sql_mode &
                                     MODE_NO_AUTO_CREATE_USER)))
    {
      result= TRUE;
      continue;
    }

    db_name= table_list->db;
    table_name= table_list->table_name;
    grant_name= routine_hash_search(Str->host.str, NullS, db_name,
                                    Str->user.str, table_name, is_proc, 1);
    if (!grant_name || !grant_name->init_privs)
    {
      if (revoke_grant)
      {
        my_error(ER_NONEXISTING_PROC_GRANT, MYF(0),
	         Str->user.str, Str->host.str, table_name);
	result= TRUE;
	continue;
      }
      grant_name= new GRANT_NAME(Str->host.str, db_name,
				 Str->user.str, table_name,
				 rights, TRUE);
      if (!grant_name ||
        my_hash_insert(is_proc ?
                       &proc_priv_hash : &func_priv_hash,(uchar*) grant_name))
      {
        result= TRUE;
	continue;
      }
    }

    if (no_such_table(tables + PROCS_PRIV_TABLE) ||
        replace_routine_table(thd, grant_name, tables[PROCS_PRIV_TABLE].table,
                              *Str, db_name, table_name, is_proc, rights,
                              revoke_grant) != 0)
    {
      result= TRUE;
      continue;
    }
    if (Str->is_role())
      propagate_role_grants(find_acl_role(Str->user.str),
                            is_proc ? PRIVS_TO_MERGE::PROC : PRIVS_TO_MERGE::FUNC,
                            db_name, table_name);
  }
  thd->mem_root= old_root;
  mysql_mutex_unlock(&acl_cache->lock);

  if (write_to_binlog)
  {
    if (write_bin_log(thd, FALSE, thd->query(), thd->query_length()))
      result= TRUE;
  }

  mysql_rwlock_unlock(&LOCK_grant);

  /* Tables are automatically closed */
  DBUG_RETURN(result);
}

/**
  append a user or role name to a buffer that will be later used as an error message
*/
static void append_user(THD *thd, String *str,
                        const LEX_STRING *u, const LEX_STRING *h)
{
  if (str->length())
    str->append(',');
  append_query_string(system_charset_info, str, u->str, u->length,
                      thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES);
  /* hostname part is not relevant for roles, it is always empty */
  if (u->length == 0 || h->length != 0)
  {
    str->append('@');
    append_query_string(system_charset_info, str, h->str, h->length,
                        thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES);
  }
}

static void append_user(THD *thd, String *str, LEX_USER *user)
{
  append_user(thd, str, & user->user, & user->host);
}

/**
  append a string to a buffer that will be later used as an error message

  @note
  a string can be either CURRENT_USER or CURRENT_ROLE or NONE, it should be
  neither quoted nor escaped.
*/
static void append_str(String *str, const char *s, size_t l)
{
  if (str->length())
    str->append(',');
  str->append(s, l);
}

static int can_grant_role_callback(ACL_USER_BASE *grantee,
                                   ACL_ROLE *role, void *data)
{
  ROLE_GRANT_PAIR *pair;

  if (role != (ACL_ROLE*)data)
    return 0; // keep searching

  if (grantee->flags & IS_ROLE)
    pair= find_role_grant_pair(&grantee->user, &empty_lex_str, &role->user);
  else
  {
    ACL_USER *user= (ACL_USER *)grantee;
    LEX_STRING host= { user->host.hostname, user->hostname_length };
    pair= find_role_grant_pair(&user->user, &host, &role->user);
  }
  if (!pair->with_admin)
    return 0; // keep searching

  return -1; // abort the traversal
}


/*
  One can only grant a role if SELECT * FROM I_S.APPLICABLE_ROLES shows this
  role as grantable.
  
  What this really means - we need to traverse role graph for the current user
  looking for our role being granted with the admin option.
*/
static bool can_grant_role(THD *thd, ACL_ROLE *role)
{
  Security_context *sctx= thd->security_ctx;

  if (!sctx->user) // replication
    return true;

  ACL_USER *grantee= find_user_exact(sctx->priv_host, sctx->priv_user);
  if (!grantee)
    return false;

  return traverse_role_graph_down(grantee, role, NULL,
                                  can_grant_role_callback) == -1;
}


bool mysql_grant_role(THD *thd, List <LEX_USER> &list, bool revoke)
{
  DBUG_ENTER("mysql_grant_role");
  /*
     The first entry in the list is the granted role. Need at least two
     entries for the command to be valid
   */
  DBUG_ASSERT(list.elements >= 2);
  int result;
  bool create_new_user, no_auto_create_user;
  String wrong_users;
  LEX_USER *user, *granted_role;
  LEX_STRING rolename;
  LEX_STRING username;
  LEX_STRING hostname;
  ACL_ROLE *role, *role_as_user;

  List_iterator <LEX_USER> user_list(list);
  granted_role= user_list++;
  if (!(granted_role= get_current_user(thd, granted_role)))
    DBUG_RETURN(TRUE);

  DBUG_ASSERT(granted_role->is_role());
  rolename= granted_role->user;

  create_new_user= test_if_create_new_users(thd);
  no_auto_create_user= MY_TEST(thd->variables.sql_mode &
                               MODE_NO_AUTO_CREATE_USER);

  TABLE_LIST tables[TABLES_MAX];
  if ((result= open_grant_tables(thd, tables, TL_WRITE,
                                 Table_user | Table_roles_mapping)))
    DBUG_RETURN(result != 1);

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);
  if (!(role= find_acl_role(rolename.str)))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    mysql_rwlock_unlock(&LOCK_grant);
    my_error(ER_INVALID_ROLE, MYF(0), rolename.str);
    DBUG_RETURN(TRUE);
  }

  if (!can_grant_role(thd, role))
  {
    mysql_mutex_unlock(&acl_cache->lock);
    mysql_rwlock_unlock(&LOCK_grant);
    my_error(ER_ACCESS_DENIED_NO_PASSWORD_ERROR, MYF(0),
             thd->security_ctx->priv_user, thd->security_ctx->priv_host);
    DBUG_RETURN(TRUE);
  }

  while ((user= user_list++))
  {
    role_as_user= NULL;
    /* current_role is treated slightly different */
    if (user->user.str == current_role.str)
    {
      /* current_role is NONE */
      if (!thd->security_ctx->priv_role[0])
      {
        my_error(ER_INVALID_ROLE, MYF(0), "NONE");
        append_str(&wrong_users, STRING_WITH_LEN("NONE"));
        result= 1;
        continue;
      }
      if (!(role_as_user= find_acl_role(thd->security_ctx->priv_role)))
      {
        LEX_STRING ls= { thd->security_ctx->priv_role,
                         strlen(thd->security_ctx->priv_role) };
        append_user(thd, &wrong_users, &ls, &empty_lex_str);
        result= 1;
        continue;
      }

      /* can not grant current_role to current_role */
      if (granted_role->user.str == current_role.str)
      {
        append_user(thd, &wrong_users, &role_as_user->user, &empty_lex_str);
        result= 1;
        continue;
      }
      username.str= thd->security_ctx->priv_role;
      username.length= strlen(username.str);
      hostname= empty_lex_str;
    }
    else if (user->user.str == current_user.str)
    {
      username.str= thd->security_ctx->priv_user;
      username.length= strlen(username.str);
      hostname.str= thd->security_ctx->priv_host;
      hostname.length= strlen(hostname.str);
    }
    else
    {
      username= user->user;
      if (user->host.str)
        hostname= user->host;
      else
      if ((role_as_user= find_acl_role(user->user.str)))
        hostname= empty_lex_str;
      else
      {
        if (is_invalid_role_name(username.str))
        {
          append_user(thd, &wrong_users, &username, &empty_lex_str);
          result= 1;
          continue;
        }
        hostname= host_not_specified;
      }
    }

    ROLE_GRANT_PAIR *hash_entry= find_role_grant_pair(&username, &hostname,
                                                      &rolename);
    ACL_USER_BASE *grantee= role_as_user;

    if (has_auth(user, thd->lex))
      DBUG_ASSERT(!grantee);
    else if (!grantee)
      grantee= find_user_exact(hostname.str, username.str);

    if (!grantee && !revoke)
    {
      LEX_USER user_combo = *user;
      user_combo.host = hostname;
      user_combo.user = username;

      if (copy_and_check_auth(&user_combo, &user_combo, thd) ||
          replace_user_table(thd, tables[USER_TABLE].table, user_combo, 0,
                             false, create_new_user,
                             no_auto_create_user))
      {
        append_user(thd, &wrong_users, &username, &hostname);
        result= 1;
        continue;
      }
      grantee= find_user_exact(hostname.str, username.str);

      /* either replace_user_table failed, or we've added the user */
      DBUG_ASSERT(grantee);
    }

    if (!grantee)
    {
      append_user(thd, &wrong_users, &username, &hostname);
      result= 1;
      continue;
    }

    if (!revoke)
    {
      if (hash_entry)
      {
        // perhaps, updating an existing grant, adding WITH ADMIN OPTION
      }
      else
      {
        add_role_user_mapping(grantee, role);

        /*
          Check if this grant would cause a cycle. It only needs to be run
          if we're granting a role to a role
        */
        if (role_as_user &&
            traverse_role_graph_down(role, 0, 0, 0) == ROLE_CYCLE_FOUND)
        {
          append_user(thd, &wrong_users, &username, &empty_lex_str);
          result= 1;
          undo_add_role_user_mapping(grantee, role);
          continue;
        }
      }
    }
    else
    {
      /* grant was already removed or never existed */
      if (!hash_entry)
      {
        append_user(thd, &wrong_users, &username, &hostname);
        result= 1;
        continue;
      }
      if (thd->lex->with_admin_option)
      {
        // only revoking an admin option, not the complete grant
      }
      else
      {
        /* revoke a role grant */
        remove_role_user_mapping(grantee, role);
      }
    }

    /* write into the roles_mapping table */
    if (replace_roles_mapping_table(tables[ROLES_MAPPING_TABLE].table,
                                    &username, &hostname, &rolename,
                                    thd->lex->with_admin_option,
                                    hash_entry, revoke))
    {
      append_user(thd, &wrong_users, &username, &empty_lex_str);
      result= 1;
      if (!revoke)
      {
        /* need to remove the mapping added previously */
        undo_add_role_user_mapping(grantee, role);
      }
      else
      {
        /* need to restore the mapping deleted previously */
        add_role_user_mapping(grantee, role);
      }
      continue;
    }
    update_role_mapping(&username, &hostname, &rolename,
                        thd->lex->with_admin_option, hash_entry, revoke);

    /*
       Only need to propagate grants when granting/revoking a role to/from
       a role
    */
    if (role_as_user && merge_one_role_privileges(role_as_user) == 0)
      propagate_role_grants(role_as_user, PRIVS_TO_MERGE::ALL);
  }

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
    my_error(revoke ? ER_CANNOT_REVOKE_ROLE : ER_CANNOT_GRANT_ROLE, MYF(0),
             rolename.str, wrong_users.c_ptr_safe());
  else
    result= write_bin_log(thd, TRUE, thd->query(), thd->query_length());

  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_RETURN(result);
}


bool mysql_grant(THD *thd, const char *db, List <LEX_USER> &list,
                 ulong rights, bool revoke_grant, bool is_proxy)
{
  List_iterator <LEX_USER> str_list (list);
  LEX_USER *Str, *tmp_Str, *proxied_user= NULL;
  char tmp_db[SAFE_NAME_LEN+1];
  bool create_new_users=0, result;
  TABLE_LIST tables[TABLES_MAX];
  DBUG_ENTER("mysql_grant");

  if (lower_case_table_names && db)
  {
    char *end= strnmov(tmp_db,db, sizeof(tmp_db));
    if (end >= tmp_db + sizeof(tmp_db))
    {
      my_error(ER_WRONG_DB_NAME ,MYF(0), db);
      DBUG_RETURN(TRUE);
    }
    my_casedn_str(files_charset_info, tmp_db);
    db=tmp_db;
  }

  if (is_proxy)
  {
    DBUG_ASSERT(!db);
    proxied_user= str_list++;
  }

  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user |
                                 (is_proxy ? Table_proxies_priv : Table_db))))
    DBUG_RETURN(result != 1);

  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());

  if (!revoke_grant)
    create_new_users= test_if_create_new_users(thd);

  /* go through users in user_list */
  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);
  grant_version++;

  if (proxied_user)
  {
    if (!(proxied_user= get_current_user(thd, proxied_user, false)))
      DBUG_RETURN(TRUE);
    DBUG_ASSERT(proxied_user->host.length); // not a Role
  }

  while ((tmp_Str = str_list++))
  {
    if (!(Str= get_current_user(thd, tmp_Str, false)))
    {
      result= true;
      continue;
    }

    if (copy_and_check_auth(Str, tmp_Str, thd) ||
        replace_user_table(thd, tables[USER_TABLE].table, *Str,
                           (!db ? rights : 0), revoke_grant, create_new_users,
                           MY_TEST(thd->variables.sql_mode &
                                   MODE_NO_AUTO_CREATE_USER)))
      result= true;
    else if (db)
    {
      ulong db_rights= rights & DB_ACLS;
      if (db_rights  == rights)
      {
	if (replace_db_table(tables[DB_TABLE].table, db, *Str, db_rights,
			     revoke_grant))
	  result= true;
      }
      else
      {
	my_error(ER_WRONG_USAGE, MYF(0), "DB GRANT", "GLOBAL PRIVILEGES");
	result= true;
      }
    }
    else if (is_proxy)
    {
      if (no_such_table(tables + PROXIES_PRIV_TABLE) ||
          replace_proxies_priv_table (thd, tables[PROXIES_PRIV_TABLE].table,
                                      Str, proxied_user,
                                      rights & GRANT_ACL ? TRUE : FALSE,
                                      revoke_grant))
        result= true;
    }
    if (Str->is_role())
      propagate_role_grants(find_acl_role(Str->user.str),
                            db ? PRIVS_TO_MERGE::DB : PRIVS_TO_MERGE::GLOBAL,
                            db);
  }
  mysql_mutex_unlock(&acl_cache->lock);

  if (!result)
  {
    result= write_bin_log(thd, TRUE, thd->query(), thd->query_length());
  }

  mysql_rwlock_unlock(&LOCK_grant);

  if (!result)
    my_ok(thd);

  DBUG_RETURN(result);
}


/* Free grant array if possible */

void  grant_free(void)
{
  DBUG_ENTER("grant_free");
  my_hash_free(&column_priv_hash);
  my_hash_free(&proc_priv_hash);
  my_hash_free(&func_priv_hash);
  free_root(&grant_memroot,MYF(0));
  DBUG_VOID_RETURN;
}


/**
  @brief Initialize structures responsible for table/column-level privilege
   checking and load information for them from tables in the 'mysql' database.

  @return Error status
    @retval 0 OK
    @retval 1 Could not initialize grant subsystem.
*/

bool grant_init()
{
  THD  *thd;
  bool return_val;
  DBUG_ENTER("grant_init");

  if (!(thd= new THD))
    DBUG_RETURN(1);				/* purecov: deadcode */
  thd->thread_stack= (char*) &thd;
  thd->store_globals();
  return_val=  grant_reload(thd);
  delete thd;
  DBUG_RETURN(return_val);
}


/**
  @brief Initialize structures responsible for table/column-level privilege
    checking and load information about grants from open privilege tables.

  @param thd Current thread
  @param tables List containing open "mysql.tables_priv" and
    "mysql.columns_priv" tables.

  @see grant_reload

  @return Error state
    @retval FALSE Success
    @retval TRUE Error
*/

static bool grant_load(THD *thd, TABLE_LIST *tables)
{
  MEM_ROOT *memex_ptr;
  bool return_val= 1;
  TABLE *t_table, *c_table, *p_table;
  bool check_no_resolve= specialflag & SPECIAL_NO_RESOLVE;
  MEM_ROOT **save_mem_root_ptr= my_pthread_getspecific_ptr(MEM_ROOT**,
                                                           THR_MALLOC);
  ulonglong old_sql_mode= thd->variables.sql_mode;
  DBUG_ENTER("grant_load");

  thd->variables.sql_mode&= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  (void) my_hash_init(&column_priv_hash, &my_charset_utf8_bin,
                      0,0,0, (my_hash_get_key) get_grant_table,
                      (my_hash_free_key) free_grant_table,0);
  (void) my_hash_init(&proc_priv_hash, &my_charset_utf8_bin,
                      0,0,0, (my_hash_get_key) get_grant_table, 0,0);
  (void) my_hash_init(&func_priv_hash, &my_charset_utf8_bin,
                      0,0,0, (my_hash_get_key) get_grant_table, 0,0);
  init_sql_alloc(&grant_memroot, ACL_ALLOC_BLOCK_SIZE, 0, MYF(0));

  t_table= tables[TABLES_PRIV_TABLE].table;
  c_table= tables[COLUMNS_PRIV_TABLE].table;
  p_table= tables[PROCS_PRIV_TABLE].table; // this can be NULL

  if (t_table->file->ha_index_init(0, 1))
    goto end_index_init;

  t_table->use_all_columns();
  c_table->use_all_columns();

  memex_ptr= &grant_memroot;
  my_pthread_setspecific_ptr(THR_MALLOC, &memex_ptr);

  if (!t_table->file->ha_index_first(t_table->record[0]))
  {
    do
    {
      GRANT_TABLE *mem_check;
      if (!(mem_check=new (memex_ptr) GRANT_TABLE(t_table,c_table)))
      {
	/* This could only happen if we are out memory */
	goto end_unlock;
      }

      if (check_no_resolve)
      {
	if (hostname_requires_resolving(mem_check->host.hostname))
	{
          sql_print_warning("'tables_priv' entry '%s %s@%s' "
                            "ignored in --skip-name-resolve mode.",
                            mem_check->tname,
                            safe_str(mem_check->user),
                            safe_str(mem_check->host.hostname));
	  continue;
	}
      }

      if (! mem_check->ok())
	delete mem_check;
      else if (my_hash_insert(&column_priv_hash,(uchar*) mem_check))
      {
	delete mem_check;
	goto end_unlock;
      }
    }
    while (!t_table->file->ha_index_next(t_table->record[0]));
  }

  return_val= 0;

  if (p_table)
  {
    if (p_table->file->ha_index_init(0, 1))
      goto end_unlock;

    p_table->use_all_columns();

    if (!p_table->file->ha_index_first(p_table->record[0]))
    {
      do
      {
        GRANT_NAME *mem_check;
        HASH *hash;
        if (!(mem_check=new (memex_ptr) GRANT_NAME(p_table, TRUE)))
        {
          /* This could only happen if we are out memory */
          goto end_unlock_p;
        }

        if (check_no_resolve)
        {
          if (hostname_requires_resolving(mem_check->host.hostname))
          {
            sql_print_warning("'procs_priv' entry '%s %s@%s' "
                              "ignored in --skip-name-resolve mode.",
                              mem_check->tname, mem_check->user,
                              safe_str(mem_check->host.hostname));
            continue;
          }
        }
        if (p_table->field[4]->val_int() == TYPE_ENUM_PROCEDURE)
        {
          hash= &proc_priv_hash;
        }
        else
        if (p_table->field[4]->val_int() == TYPE_ENUM_FUNCTION)
        {
          hash= &func_priv_hash;
        }
        else
        {
          sql_print_warning("'procs_priv' entry '%s' "
                            "ignored, bad routine type",
                            mem_check->tname);
          continue;
        }

        mem_check->privs= fix_rights_for_procedure(mem_check->privs);
        mem_check->init_privs= mem_check->privs;
        if (! mem_check->ok())
          delete mem_check;
        else if (my_hash_insert(hash, (uchar*) mem_check))
        {
          delete mem_check;
          goto end_unlock_p;
        }
      }
      while (!p_table->file->ha_index_next(p_table->record[0]));
    }
  }

end_unlock_p:
  if (p_table)
    p_table->file->ha_index_end();
end_unlock:
  t_table->file->ha_index_end();
  my_pthread_setspecific_ptr(THR_MALLOC, save_mem_root_ptr);
end_index_init:
  thd->variables.sql_mode= old_sql_mode;
  DBUG_RETURN(return_val);
}


static my_bool role_propagate_grants_action(void *ptr,
                                            void *unused __attribute__((unused)))
{
  ACL_ROLE *role= (ACL_ROLE *)ptr;
  if (role->counter)
    return 0;

  mysql_mutex_assert_owner(&acl_cache->lock);
  PRIVS_TO_MERGE data= { PRIVS_TO_MERGE::ALL, 0, 0 };
  traverse_role_graph_up(role, &data, NULL, merge_role_privileges);
  return 0;
}


/**
  @brief Reload information about table and column level privileges if possible

  @param thd Current thread

  Locked tables are checked by acl_reload() and doesn't have to be checked
  in this call.
  This function is also used for initialization of structures responsible
  for table/column-level privilege checking.

  @return Error state
    @retval FALSE Success
    @retval TRUE  Error
*/

bool grant_reload(THD *thd)
{
  TABLE_LIST tables[TABLES_MAX];
  HASH old_column_priv_hash, old_proc_priv_hash, old_func_priv_hash;
  MEM_ROOT old_mem;
  int result;
  DBUG_ENTER("grant_reload");

  /*
    To avoid deadlocks we should obtain table locks before
    obtaining LOCK_grant rwlock.
  */

  if ((result= open_grant_tables(thd, tables, TL_READ, Table_tables_priv |
                                 Table_columns_priv | Table_procs_priv)))
    DBUG_RETURN(result != 1);

  mysql_rwlock_wrlock(&LOCK_grant);
  grant_version++;
  old_column_priv_hash= column_priv_hash;
  old_proc_priv_hash= proc_priv_hash;
  old_func_priv_hash= func_priv_hash;

  /*
    Create a new memory pool but save the current memory pool to make an undo
    opertion possible in case of failure.
  */
  old_mem= grant_memroot;

  if ((result= grant_load(thd, tables)))
  {						// Error. Revert to old hash
    DBUG_PRINT("error",("Reverting to old privileges"));
    grant_free();				/* purecov: deadcode */
    column_priv_hash= old_column_priv_hash;	/* purecov: deadcode */
    proc_priv_hash= old_proc_priv_hash;
    func_priv_hash= old_func_priv_hash;
    grant_memroot= old_mem;                     /* purecov: deadcode */
  }
  else
  {
    my_hash_free(&old_column_priv_hash);
    my_hash_free(&old_proc_priv_hash);
    my_hash_free(&old_func_priv_hash);
    free_root(&old_mem,MYF(0));
  }

  mysql_mutex_lock(&acl_cache->lock);
  my_hash_iterate(&acl_roles, role_propagate_grants_action, NULL);
  mysql_mutex_unlock(&acl_cache->lock);

  mysql_rwlock_unlock(&LOCK_grant);

  close_mysql_tables(thd);

  DBUG_RETURN(result);
}


/**
  @brief Check table level grants

  @param thd          Thread handler
  @param want_access  Bits of privileges user needs to have.
  @param tables       List of tables to check. The user should have
                      'want_access' to all tables in list.
  @param any_combination_will_do TRUE if it's enough to have any privilege for
    any combination of the table columns.
  @param number       Check at most this number of tables.
  @param no_errors    TRUE if no error should be sent directly to the client.

  If table->grant.want_privilege != 0 then the requested privileges where
  in the set of COL_ACLS but access was not granted on the table level. As
  a consequence an extra check of column privileges is required.

  Specifically if this function returns FALSE the user has some kind of
  privilege on a combination of columns in each table.

  This function is usually preceeded by check_access which establish the
  User-, Db- and Host access rights.

  @see check_access
  @see check_table_access

  @note
     This functions assumes that either number of tables to be inspected
     by it is limited explicitly (i.e. is is not UINT_MAX) or table list
     used and thd->lex->query_tables_own_last value correspond to each
     other (the latter should be either 0 or point to next_global member
     of one of elements of this table list).

     We delay locking of LOCK_grant until we really need it as we assume that
     most privileges be resolved with user or db level accesses.

   @return Access status
     @retval FALSE Access granted; But column privileges might need to be
      checked.
     @retval TRUE The user did not have the requested privileges on any of the
      tables.

*/

bool check_grant(THD *thd, ulong want_access, TABLE_LIST *tables,
                 bool any_combination_will_do, uint number, bool no_errors)
{
  TABLE_LIST *tl;
  TABLE_LIST *first_not_own_table= thd->lex->first_not_own_table();
  Security_context *sctx= thd->security_ctx;
  uint i;
  ulong orig_want_access= want_access;
  bool locked= 0;
  GRANT_TABLE *grant_table;
  GRANT_TABLE *grant_table_role= NULL;
  DBUG_ENTER("check_grant");
  DBUG_ASSERT(number > 0);

  /*
    Walk through the list of tables that belong to the query and save the
    requested access (orig_want_privilege) to be able to use it when
    checking access rights to the underlying tables of a view. Our grant
    system gradually eliminates checked bits from want_privilege and thus
    after all checks are done we can no longer use it.
    The check that first_not_own_table is not reached is for the case when
    the given table list refers to the list for prelocking (contains tables
    of other queries). For simple queries first_not_own_table is 0.
  */
  for (i= 0, tl= tables;
       i < number  && tl != first_not_own_table;
       tl= tl->next_global, i++)
  {
    /*
      Save a copy of the privileges without the SHOW_VIEW_ACL attribute.
      It will be checked during making view.
    */
    tl->grant.orig_want_privilege= (want_access & ~SHOW_VIEW_ACL);
  }
  number= i;

  for (tl= tables; number-- ; tl= tl->next_global)
  {
    TABLE_LIST *const t_ref=
      tl->correspondent_table ? tl->correspondent_table : tl;
    sctx= t_ref->security_ctx ? t_ref->security_ctx : thd->security_ctx;

    const ACL_internal_table_access *access=
      get_cached_table_access(&t_ref->grant.m_internal,
                              t_ref->get_db_name(),
                              t_ref->get_table_name());

    if (access)
    {
      switch(access->check(orig_want_access, &t_ref->grant.privilege))
      {
      case ACL_INTERNAL_ACCESS_GRANTED:
        /*
          Currently,
          -  the information_schema does not subclass ACL_internal_table_access,
          there are no per table privilege checks for I_S,
          - the performance schema does use per tables checks, but at most
          returns 'CHECK_GRANT', and never 'ACCESS_GRANTED'.
          so this branch is not used.
        */
        DBUG_ASSERT(0);
      case ACL_INTERNAL_ACCESS_DENIED:
        goto err;
      case ACL_INTERNAL_ACCESS_CHECK_GRANT:
        break;
      }
    }

    want_access= orig_want_access;
    want_access&= ~sctx->master_access;
    if (!want_access)
      continue;                                 // ok

    if (!(~t_ref->grant.privilege & want_access) ||
        t_ref->is_anonymous_derived_table() || t_ref->schema_table)
    {
      /*
        It is subquery in the FROM clause. VIEW set t_ref->derived after
        table opening, but this function always called before table opening.
      */
      if (!t_ref->referencing_view)
      {
        /*
          If it's a temporary table created for a subquery in the FROM
          clause, or an INFORMATION_SCHEMA table, drop the request for
          a privilege.
        */
        t_ref->grant.want_privilege= 0;
      }
      continue;
    }

    if (is_temporary_table(t_ref))
    {
      /*
        If this table list element corresponds to a pre-opened temporary
        table skip checking of all relevant table-level privileges for it.
        Note that during creation of temporary table we still need to check
        if user has CREATE_TMP_ACL.
      */
      t_ref->grant.privilege|= TMP_TABLE_ACLS;
      t_ref->grant.want_privilege= 0;
      continue;
    }

    if (!locked)
    {
      locked= 1;
      mysql_rwlock_rdlock(&LOCK_grant);
    }

    grant_table= table_hash_search(sctx->host, sctx->ip,
                                   t_ref->get_db_name(),
                                   sctx->priv_user,
                                   t_ref->get_table_name(),
                                   FALSE);
    if (sctx->priv_role[0])
      grant_table_role= table_hash_search("", NULL, t_ref->get_db_name(),
                                          sctx->priv_role,
                                          t_ref->get_table_name(),
                                          TRUE);

    if (!grant_table && !grant_table_role)
    {
      want_access&= ~t_ref->grant.privilege;
      goto err;					// No grants
    }

    /*
      For SHOW COLUMNS, SHOW INDEX it is enough to have some
      privileges on any column combination on the table.
    */
    if (any_combination_will_do)
      continue;

    t_ref->grant.grant_table_user= grant_table; // Remember for column test
    t_ref->grant.grant_table_role= grant_table_role;
    t_ref->grant.version= grant_version;
    t_ref->grant.privilege|= grant_table ? grant_table->privs : 0;
    t_ref->grant.privilege|= grant_table_role ? grant_table_role->privs : 0;
    t_ref->grant.want_privilege= ((want_access & COL_ACLS) & ~t_ref->grant.privilege);

    if (!(~t_ref->grant.privilege & want_access))
      continue;

    if ((want_access&= ~((grant_table ? grant_table->cols : 0) |
                        (grant_table_role ? grant_table_role->cols : 0) |
                        t_ref->grant.privilege)))
    {
      goto err;                                 // impossible
    }
  }
  if (locked)
    mysql_rwlock_unlock(&LOCK_grant);
  DBUG_RETURN(FALSE);

err:
  if (locked)
    mysql_rwlock_unlock(&LOCK_grant);
  if (!no_errors)				// Not a silent skip of table
  {
    char command[128];
    get_privilege_desc(command, sizeof(command), want_access);
    status_var_increment(thd->status_var.access_denied_errors);

    my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
             command,
             sctx->priv_user,
             sctx->host_or_ip,
             tl ? tl->get_table_name() : "unknown");
  }
  DBUG_RETURN(TRUE);
}


/*
  Check column rights in given security context

  SYNOPSIS
    check_grant_column()
    thd                  thread handler
    grant                grant information structure
    db_name              db name
    table_name           table  name
    name                 column name
    length               column name length
    sctx                 security context

  RETURN
    FALSE OK
    TRUE  access denied
*/

bool check_grant_column(THD *thd, GRANT_INFO *grant,
			const char *db_name, const char *table_name,
			const char *name, uint length,  Security_context *sctx)
{
  GRANT_TABLE *grant_table;
  GRANT_TABLE *grant_table_role;
  GRANT_COLUMN *grant_column;
  ulong want_access= grant->want_privilege & ~grant->privilege;
  DBUG_ENTER("check_grant_column");
  DBUG_PRINT("enter", ("table: %s  want_access: %lu", table_name, want_access));

  if (!want_access)
    DBUG_RETURN(0);				// Already checked

  mysql_rwlock_rdlock(&LOCK_grant);

  /* reload table if someone has modified any grants */

  if (grant->version != grant_version)
  {
    grant->grant_table_user=
      table_hash_search(sctx->host, sctx->ip, db_name,
			sctx->priv_user,
			table_name, 0);         /* purecov: inspected */
    grant->grant_table_role=
      sctx->priv_role[0] ? table_hash_search("", NULL, db_name,
                                             sctx->priv_role,
                                             table_name, TRUE) : NULL;
    grant->version= grant_version;		/* purecov: inspected */
  }

  grant_table= grant->grant_table_user;
  grant_table_role= grant->grant_table_role;

  if (!grant_table && !grant_table_role)
    goto err;

  if (grant_table)
  {
    grant_column= column_hash_search(grant_table, name, length);
    if (grant_column)
    {
      want_access&= ~grant_column->rights;
    }
  }
  if (grant_table_role)
  {
    grant_column= column_hash_search(grant_table_role, name, length);
    if (grant_column)
    {
      want_access&= ~grant_column->rights;
    }
  }
  if (!want_access)
  {
    mysql_rwlock_unlock(&LOCK_grant);
    DBUG_RETURN(0);
  }

err:
  mysql_rwlock_unlock(&LOCK_grant);
  char command[128];
  get_privilege_desc(command, sizeof(command), want_access);
  /* TODO perhaps error should print current rolename aswell */
  my_error(ER_COLUMNACCESS_DENIED_ERROR, MYF(0),
           command,
           sctx->priv_user,
           sctx->host_or_ip,
           name,
           table_name);
  DBUG_RETURN(1);
}


/*
  Check the access right to a column depending on the type of table.

  SYNOPSIS
    check_column_grant_in_table_ref()
    thd              thread handler
    table_ref        table reference where to check the field
    name             name of field to check
    length           length of name

  DESCRIPTION
    Check the access rights to a column depending on the type of table
    reference where the column is checked. The function provides a
    generic interface to check column access rights that hides the
    heterogeneity of the column representation - whether it is a view
    or a stored table colum.

  RETURN
    FALSE OK
    TRUE  access denied
*/

bool check_column_grant_in_table_ref(THD *thd, TABLE_LIST * table_ref,
                                     const char *name, uint length)
{
  GRANT_INFO *grant;
  const char *db_name;
  const char *table_name;
  Security_context *sctx= MY_TEST(table_ref->security_ctx) ?
                          table_ref->security_ctx : thd->security_ctx;

  if (table_ref->view || table_ref->field_translation)
  {
    /* View or derived information schema table. */
    ulong view_privs;
    grant= &(table_ref->grant);
    db_name= table_ref->view_db.str;
    table_name= table_ref->view_name.str;
    if (table_ref->belong_to_view &&
        thd->lex->sql_command == SQLCOM_SHOW_FIELDS)
    {
      view_privs= get_column_grant(thd, grant, db_name, table_name, name);
      if (view_privs & VIEW_ANY_ACL)
      {
        table_ref->belong_to_view->allowed_show= TRUE;
        return FALSE;
      }
      table_ref->belong_to_view->allowed_show= FALSE;
      my_message(ER_VIEW_NO_EXPLAIN, ER_THD(thd, ER_VIEW_NO_EXPLAIN), MYF(0));
      return TRUE;
    }
  }
  else
  {
    /* Normal or temporary table. */
    TABLE *table= table_ref->table;
    grant= &(table->grant);
    db_name= table->s->db.str;
    table_name= table->s->table_name.str;
  }

  if (grant->want_privilege)
    return check_grant_column(thd, grant, db_name, table_name, name,
                              length, sctx);
  else
    return FALSE;

}


/**
  @brief check if a query can access a set of columns

  @param  thd  the current thread
  @param  want_access_arg  the privileges requested
  @param  fields an iterator over the fields of a table reference.
  @return Operation status
    @retval 0 Success
    @retval 1 Falure
  @details This function walks over the columns of a table reference
   The columns may originate from different tables, depending on the kind of
   table reference, e.g. join, view.
   For each table it will retrieve the grant information and will use it
   to check the required access privileges for the fields requested from it.
*/
bool check_grant_all_columns(THD *thd, ulong want_access_arg,
                             Field_iterator_table_ref *fields)
{
  Security_context *sctx= thd->security_ctx;
  ulong UNINIT_VAR(want_access);
  const char *table_name= NULL;
  const char* db_name;
  GRANT_INFO *grant;
  GRANT_TABLE *UNINIT_VAR(grant_table);
  GRANT_TABLE *UNINIT_VAR(grant_table_role);
  /*
     Flag that gets set if privilege checking has to be performed on column
     level.
  */
  bool using_column_privileges= FALSE;

  mysql_rwlock_rdlock(&LOCK_grant);

  for (; !fields->end_of_fields(); fields->next())
  {
    const char *field_name= fields->name();

    if (table_name != fields->get_table_name())
    {
      table_name= fields->get_table_name();
      db_name= fields->get_db_name();
      grant= fields->grant();
      /* get a fresh one for each table */
      want_access= want_access_arg & ~grant->privilege;
      if (want_access)
      {
        /* reload table if someone has modified any grants */
        if (grant->version != grant_version)
        {
          grant->grant_table_user=
            table_hash_search(sctx->host, sctx->ip, db_name,
                              sctx->priv_user,
                              table_name, 0);	/* purecov: inspected */
          grant->grant_table_role=
            sctx->priv_role[0] ? table_hash_search("", NULL, db_name,
                                                   sctx->priv_role,
                                                   table_name, TRUE) : NULL;
          grant->version= grant_version;	/* purecov: inspected */
        }

        grant_table= grant->grant_table_user;
        grant_table_role= grant->grant_table_role;
        DBUG_ASSERT (grant_table || grant_table_role);
      }
    }

    if (want_access)
    {
      ulong have_access= 0;
      if (grant_table)
      {
        GRANT_COLUMN *grant_column=
          column_hash_search(grant_table, field_name,
                             (uint) strlen(field_name));
        if (grant_column)
          have_access= grant_column->rights;
      }
      if (grant_table_role)
      {
        GRANT_COLUMN *grant_column=
          column_hash_search(grant_table_role, field_name,
                             (uint) strlen(field_name));
        if (grant_column)
          have_access|= grant_column->rights;
      }

      if (have_access)
        using_column_privileges= TRUE;
      if (want_access & ~have_access)
        goto err;
    }
  }
  mysql_rwlock_unlock(&LOCK_grant);
  return 0;

err:
  mysql_rwlock_unlock(&LOCK_grant);

  char command[128];
  get_privilege_desc(command, sizeof(command), want_access);
  /*
    Do not give an error message listing a column name unless the user has
    privilege to see all columns.
  */
  if (using_column_privileges)
    my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
             command, sctx->priv_user,
             sctx->host_or_ip, table_name);
  else
    my_error(ER_COLUMNACCESS_DENIED_ERROR, MYF(0),
             command,
             sctx->priv_user,
             sctx->host_or_ip,
             fields->name(),
             table_name);
  return 1;
}


static bool check_grant_db_routine(THD *thd, const char *db, HASH *hash)
{
  Security_context *sctx= thd->security_ctx;

  for (uint idx= 0; idx < hash->records; ++idx)
  {
    GRANT_NAME *item= (GRANT_NAME*) my_hash_element(hash, idx);

    if (strcmp(item->user, sctx->priv_user) == 0 &&
        strcmp(item->db, db) == 0 &&
        compare_hostname(&item->host, sctx->host, sctx->ip))
    {
      return FALSE;
    }
    if (sctx->priv_role[0] && strcmp(item->user, sctx->priv_role) == 0 &&
        strcmp(item->db, db) == 0 &&
        (!item->host.hostname || !item->host.hostname[0]))
    {
      return FALSE; /* Found current role match */
    }
  }

  return TRUE;
}


/*
  Check if a user has the right to access a database
  Access is accepted if he has a grant for any table/routine in the database
  Return 1 if access is denied
*/

bool check_grant_db(THD *thd, const char *db)
{
  Security_context *sctx= thd->security_ctx;
  char helping [SAFE_NAME_LEN + USERNAME_LENGTH+2], *end;
  char helping2 [SAFE_NAME_LEN + USERNAME_LENGTH+2], *tmp_db;
  uint len, UNINIT_VAR(len2);
  bool error= TRUE;

  tmp_db= strmov(helping, sctx->priv_user) + 1;
  end= strnmov(tmp_db, db, helping + sizeof(helping) - tmp_db);

  if (end >= helping + sizeof(helping)) // db name was truncated
    return 1;                           // no privileges for an invalid db name

  if (lower_case_table_names)
  {
    end = tmp_db + my_casedn_str(files_charset_info, tmp_db);
    db=tmp_db;
  }

  len= (uint) (end - helping) + 1;

  /*
     If a role is set, we need to check for privileges
     here aswell
  */
  if (sctx->priv_role[0])
  {
    end= strmov(helping2, sctx->priv_role) + 1;
    end= strnmov(end, db, helping2 + sizeof(helping2) - end);
    len2= (uint) (end - helping2) + 1;
  }


  mysql_rwlock_rdlock(&LOCK_grant);

  for (uint idx=0 ; idx < column_priv_hash.records ; idx++)
  {
    GRANT_TABLE *grant_table= (GRANT_TABLE*)
      my_hash_element(&column_priv_hash,
                      idx);
    if (len < grant_table->key_length &&
	!memcmp(grant_table->hash_key,helping,len) &&
        compare_hostname(&grant_table->host, sctx->host, sctx->ip))
    {
      error= FALSE; /* Found match. */
      break;
    }
    if (sctx->priv_role[0] &&
        len2 < grant_table->key_length &&
        !memcmp(grant_table->hash_key,helping2,len) &&
        (!grant_table->host.hostname || !grant_table->host.hostname[0]))
    {
      error= FALSE; /* Found role match */
      break;
    }
  }

  if (error)
    error= check_grant_db_routine(thd, db, &proc_priv_hash) &&
           check_grant_db_routine(thd, db, &func_priv_hash);

  mysql_rwlock_unlock(&LOCK_grant);

  return error;
}


/****************************************************************************
  Check routine level grants

  SYNPOSIS
   bool check_grant_routine()
   thd		Thread handler
   want_access  Bits of privileges user needs to have
   procs	List of routines to check. The user should have 'want_access'
   is_proc	True if the list is all procedures, else functions
   no_errors	If 0 then we write an error. The error is sent directly to
		the client

   RETURN
     0  ok
     1  Error: User did not have the requested privielges
****************************************************************************/

bool check_grant_routine(THD *thd, ulong want_access,
			 TABLE_LIST *procs, bool is_proc, bool no_errors)
{
  TABLE_LIST *table;
  Security_context *sctx= thd->security_ctx;
  char *user= sctx->priv_user;
  char *host= sctx->priv_host;
  char *role= sctx->priv_role;
  DBUG_ENTER("check_grant_routine");

  want_access&= ~sctx->master_access;
  if (!want_access)
    DBUG_RETURN(0);                             // ok

  mysql_rwlock_rdlock(&LOCK_grant);
  for (table= procs; table; table= table->next_global)
  {
    GRANT_NAME *grant_proc;
    if ((grant_proc= routine_hash_search(host, sctx->ip, table->db, user,
					 table->table_name, is_proc, 0)))
      table->grant.privilege|= grant_proc->privs;
    if (role[0]) /* current role set check */
    {
      if ((grant_proc= routine_hash_search("", NULL, table->db, role,
                                           table->table_name, is_proc, 0)))
      table->grant.privilege|= grant_proc->privs;
    }

    if (want_access & ~table->grant.privilege)
    {
      want_access &= ~table->grant.privilege;
      goto err;
    }
  }
  mysql_rwlock_unlock(&LOCK_grant);
  DBUG_RETURN(0);
err:
  mysql_rwlock_unlock(&LOCK_grant);
  if (!no_errors)
  {
    char buff[1024];
    const char *command="";
    if (table)
      strxmov(buff, table->db, ".", table->table_name, NullS);
    if (want_access & EXECUTE_ACL)
      command= "execute";
    else if (want_access & ALTER_PROC_ACL)
      command= "alter routine";
    else if (want_access & GRANT_ACL)
      command= "grant";
    my_error(ER_PROCACCESS_DENIED_ERROR, MYF(0),
             command, user, host, table ? buff : "unknown");
  }
  DBUG_RETURN(1);
}


/*
  Check if routine has any of the
  routine level grants

  SYNPOSIS
   bool    check_routine_level_acl()
   thd	        Thread handler
   db           Database name
   name         Routine name

  RETURN
   0            Ok
   1            error
*/

bool check_routine_level_acl(THD *thd, const char *db, const char *name,
                             bool is_proc)
{
  bool no_routine_acl= 1;
  GRANT_NAME *grant_proc;
  Security_context *sctx= thd->security_ctx;
  mysql_rwlock_rdlock(&LOCK_grant);
  if ((grant_proc= routine_hash_search(sctx->priv_host,
                                       sctx->ip, db,
                                       sctx->priv_user,
                                       name, is_proc, 0)))
    no_routine_acl= !(grant_proc->privs & SHOW_PROC_ACLS);

  if (no_routine_acl && sctx->priv_role[0]) /* current set role check */
  {
    if ((grant_proc= routine_hash_search("",
                                         NULL, db,
                                         sctx->priv_role,
                                         name, is_proc, 0)))
      no_routine_acl= !(grant_proc->privs & SHOW_PROC_ACLS);
  }
  mysql_rwlock_unlock(&LOCK_grant);
  return no_routine_acl;
}


/*****************************************************************************
  Functions to retrieve the grant for a table/column  (for SHOW functions)
*****************************************************************************/

ulong get_table_grant(THD *thd, TABLE_LIST *table)
{
  ulong privilege;
  Security_context *sctx= thd->security_ctx;
  const char *db = table->db ? table->db : thd->db;
  GRANT_TABLE *grant_table;
  GRANT_TABLE *grant_table_role= NULL;

  mysql_rwlock_rdlock(&LOCK_grant);
#ifdef EMBEDDED_LIBRARY
  grant_table= NULL;
  grant_table_role= NULL;
#else
  grant_table= table_hash_search(sctx->host, sctx->ip, db, sctx->priv_user,
				 table->table_name, 0);
  if (sctx->priv_role[0])
    grant_table_role= table_hash_search("", "", db, sctx->priv_role,
                                        table->table_name, 0);
#endif
  table->grant.grant_table_user= grant_table; // Remember for column test
  table->grant.grant_table_role= grant_table_role;
  table->grant.version=grant_version;
  if (grant_table)
    table->grant.privilege|= grant_table->privs;
  if (grant_table_role)
    table->grant.privilege|= grant_table_role->privs;
  privilege= table->grant.privilege;
  mysql_rwlock_unlock(&LOCK_grant);
  return privilege;
}


/*
  Determine the access priviliges for a field.

  SYNOPSIS
    get_column_grant()
    thd         thread handler
    grant       grants table descriptor
    db_name     name of database that the field belongs to
    table_name  name of table that the field belongs to
    field_name  name of field

  DESCRIPTION
    The procedure may also modify: grant->grant_table and grant->version.

  RETURN
    The access priviliges for the field db_name.table_name.field_name
*/

ulong get_column_grant(THD *thd, GRANT_INFO *grant,
                       const char *db_name, const char *table_name,
                       const char *field_name)
{
  GRANT_TABLE *grant_table;
  GRANT_TABLE *grant_table_role;
  GRANT_COLUMN *grant_column;
  ulong priv= 0;

  mysql_rwlock_rdlock(&LOCK_grant);
  /* reload table if someone has modified any grants */
  if (grant->version != grant_version)
  {
    Security_context *sctx= thd->security_ctx;
    grant->grant_table_user=
      table_hash_search(sctx->host, sctx->ip,
                        db_name, sctx->priv_user,
                        table_name, 0);         /* purecov: inspected */
    grant->grant_table_role=
      sctx->priv_role[0] ? table_hash_search("", "", db_name,
                                             sctx->priv_role,
                                             table_name, TRUE) : NULL;
    grant->version= grant_version;              /* purecov: inspected */
  }

  grant_table= grant->grant_table_user;
  grant_table_role= grant->grant_table_role;

  if (!grant_table && !grant_table_role)
    priv= grant->privilege;
  else
  {
    if (grant_table)
    {
      grant_column= column_hash_search(grant_table, field_name,
                                       (uint) strlen(field_name));
      if (!grant_column)
        priv= (grant->privilege | grant_table->privs);
      else
        priv= (grant->privilege | grant_table->privs | grant_column->rights);
    }

    if (grant_table_role)
    {
      grant_column= column_hash_search(grant_table_role, field_name,
                                       (uint) strlen(field_name));
      if (!grant_column)
        priv|= (grant->privilege | grant_table_role->privs);
      else
        priv|= (grant->privilege | grant_table_role->privs |
                grant_column->rights);
    }
  }
  mysql_rwlock_unlock(&LOCK_grant);
  return priv;
}


/* Help function for mysql_show_grants */

static void add_user_option(String *grant, long value, const char *name,
                            bool is_signed)
{
  if (value)
  {
    char buff[22], *p; // just as in int2str
    grant->append(' ');
    grant->append(name, strlen(name));
    grant->append(' ');
    p=int10_to_str(value, buff, is_signed ? -10 : 10);
    grant->append(buff,p-buff);
  }
}


static void add_user_option(String *grant, double value, const char *name)
{
  if (value != 0.0 )
  {
    char buff[FLOATING_POINT_BUFFER];
    size_t len;
    grant->append(' ');
    grant->append(name, strlen(name));
    grant->append(' ');
    len= my_fcvt(value, 6, buff, NULL);
    grant->append(buff, len);
  }
}

static const char *command_array[]=
{
  "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "RELOAD",
  "SHUTDOWN", "PROCESS","FILE", "GRANT", "REFERENCES", "INDEX",
  "ALTER", "SHOW DATABASES", "SUPER", "CREATE TEMPORARY TABLES",
  "LOCK TABLES", "EXECUTE", "REPLICATION SLAVE", "REPLICATION CLIENT",
  "CREATE VIEW", "SHOW VIEW", "CREATE ROUTINE", "ALTER ROUTINE",
  "CREATE USER", "EVENT", "TRIGGER", "CREATE TABLESPACE"
};

static uint command_lengths[]=
{
  6, 6, 6, 6, 6, 4, 6, 8, 7, 4, 5, 10, 5, 5, 14, 5, 23, 11, 7, 17, 18, 11, 9,
  14, 13, 11, 5, 7, 17
};


static bool print_grants_for_role(THD *thd, ACL_ROLE * role)
{
  char buff[1024];

  if (show_role_grants(thd, role->user.str, "", role, buff, sizeof(buff)))
    return TRUE;

  if (show_global_privileges(thd, role, TRUE, buff, sizeof(buff)))
    return TRUE;

  if (show_database_privileges(thd, role->user.str, "", buff, sizeof(buff)))
    return TRUE;

  if (show_table_and_column_privileges(thd, role->user.str, "", buff, sizeof(buff)))
    return TRUE;

  if (show_routine_grants(thd, role->user.str, "", &proc_priv_hash,
                          STRING_WITH_LEN("PROCEDURE"), buff, sizeof(buff)))
    return TRUE;

  if (show_routine_grants(thd, role->user.str, "", &func_priv_hash,
                          STRING_WITH_LEN("FUNCTION"), buff, sizeof(buff)))
    return TRUE;

  return FALSE;

}


static int show_grants_callback(ACL_USER_BASE *role, void *data)
{
  THD *thd= (THD *)data;
  DBUG_ASSERT(role->flags & IS_ROLE);
  if (print_grants_for_role(thd, (ACL_ROLE *)role))
    return -1;
  return 0;
}


void mysql_show_grants_get_fields(THD *thd, List<Item> *fields,
                                  const char *name)
{
  Item_string *field=new (thd->mem_root) Item_string_ascii(thd, "", 0);
  field->name= (char *) name;
  field->max_length=1024;
  fields->push_back(field, thd->mem_root);
}


/*
  SHOW GRANTS;  Send grants for a user to the client

  IMPLEMENTATION
   Send to client grant-like strings depicting user@host privileges
*/

bool mysql_show_grants(THD *thd, LEX_USER *lex_user)
{
  int  error = -1;
  ACL_USER *UNINIT_VAR(acl_user);
  ACL_ROLE *acl_role= NULL;
  char buff[1024];
  Protocol *protocol= thd->protocol;
  char *username= NULL;
  char *hostname= NULL;
  char *rolename= NULL;
  DBUG_ENTER("mysql_show_grants");

  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(TRUE);
  }

  if (lex_user->user.str == current_user.str)
  {
    username= thd->security_ctx->priv_user;
    hostname= thd->security_ctx->priv_host;
  }
  else if (lex_user->user.str == current_role.str)
  {
    rolename= thd->security_ctx->priv_role;
  }
  else if (lex_user->user.str == current_user_and_current_role.str)
  {
    username= thd->security_ctx->priv_user;
    hostname= thd->security_ctx->priv_host;
    rolename= thd->security_ctx->priv_role;
  }
  else
  {
    Security_context *sctx= thd->security_ctx;
    bool do_check_access;

    lex_user= get_current_user(thd, lex_user);
    if (!lex_user)
      DBUG_RETURN(TRUE);

    if (lex_user->is_role())
    {
      rolename= lex_user->user.str;
      do_check_access= strcmp(rolename, sctx->priv_role);
    }
    else
    {
      username= lex_user->user.str;
      hostname= lex_user->host.str;
      do_check_access= strcmp(username, sctx->priv_user) ||
                       strcmp(hostname, sctx->priv_host);
    }

    if (do_check_access && check_access(thd, SELECT_ACL, "mysql", 0, 0, 1, 0))
      DBUG_RETURN(TRUE);
  }
  DBUG_ASSERT(rolename || username);

  List<Item> field_list;
  if (!username)
    strxmov(buff,"Grants for ",rolename, NullS);
  else
    strxmov(buff,"Grants for ",username,"@",hostname, NullS);

  mysql_show_grants_get_fields(thd, &field_list, buff);

  if (protocol->send_result_set_metadata(&field_list,
                               Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  mysql_rwlock_rdlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  if (username)
  {
    acl_user= find_user_exact(hostname, username);
    if (!acl_user)
    {
      mysql_mutex_unlock(&acl_cache->lock);
      mysql_rwlock_unlock(&LOCK_grant);

      my_error(ER_NONEXISTING_GRANT, MYF(0),
               username, hostname);
      DBUG_RETURN(TRUE);
    }

    /* Show granted roles to acl_user */
    if (show_role_grants(thd, username, hostname, acl_user, buff, sizeof(buff)))
      goto end;

    /* Add first global access grants */
    if (show_global_privileges(thd, acl_user, FALSE, buff, sizeof(buff)))
      goto end;

    /* Add database access */
    if (show_database_privileges(thd, username, hostname, buff, sizeof(buff)))
      goto end;

    /* Add table & column access */
    if (show_table_and_column_privileges(thd, username, hostname, buff, sizeof(buff)))
      goto end;

    if (show_routine_grants(thd, username, hostname, &proc_priv_hash,
                            STRING_WITH_LEN("PROCEDURE"), buff, sizeof(buff)))
      goto end;

    if (show_routine_grants(thd, username, hostname, &func_priv_hash,
                            STRING_WITH_LEN("FUNCTION"), buff, sizeof(buff)))
      goto end;

    if (show_proxy_grants(thd, username, hostname, buff, sizeof(buff)))
      goto end;
  }

  if (rolename)
  {
    acl_role= find_acl_role(rolename);
    if (acl_role)
    {
      /* get a list of all inherited roles */
      traverse_role_graph_down(acl_role, thd, show_grants_callback, NULL);
    }
    else
    {
      if (lex_user->user.str == current_role.str)
      {
        mysql_mutex_unlock(&acl_cache->lock);
        mysql_rwlock_unlock(&LOCK_grant);
        my_error(ER_NONEXISTING_GRANT, MYF(0),
                 thd->security_ctx->priv_user,
                 thd->security_ctx->priv_host);
        DBUG_RETURN(TRUE);
      }
    }
  }

  error= 0;
end:
  mysql_mutex_unlock(&acl_cache->lock);
  mysql_rwlock_unlock(&LOCK_grant);

  my_eof(thd);
  DBUG_RETURN(error);
}

static ROLE_GRANT_PAIR *find_role_grant_pair(const LEX_STRING *u,
                                             const LEX_STRING *h,
                                             const LEX_STRING *r)
{
  char buf[1024];
  String pair_key(buf, sizeof(buf), &my_charset_bin);

  size_t key_length= u->length + h->length + r->length + 3;
  pair_key.alloc(key_length);

  strmov(strmov(strmov(const_cast<char*>(pair_key.ptr()),
                       safe_str(u->str)) + 1, h->str) + 1, r->str);

  return (ROLE_GRANT_PAIR *)
    my_hash_search(&acl_roles_mappings, (uchar*)pair_key.ptr(), key_length);
}

static bool show_role_grants(THD *thd, const char *username,
                             const char *hostname, ACL_USER_BASE *acl_entry,
                             char *buff, size_t buffsize)
{
  uint counter;
  Protocol *protocol= thd->protocol;
  LEX_STRING host= {const_cast<char*>(hostname), strlen(hostname)};

  String grant(buff,sizeof(buff),system_charset_info);
  for (counter= 0; counter < acl_entry->role_grants.elements; counter++)
  {
    grant.length(0);
    grant.append(STRING_WITH_LEN("GRANT "));
    ACL_ROLE *acl_role= *(dynamic_element(&acl_entry->role_grants, counter,
                                          ACL_ROLE**));
    grant.append(acl_role->user.str, acl_role->user.length,
                  system_charset_info);
    grant.append(STRING_WITH_LEN(" TO '"));
    grant.append(acl_entry->user.str, acl_entry->user.length,
                  system_charset_info);
    if (!(acl_entry->flags & IS_ROLE))
    {
      grant.append(STRING_WITH_LEN("'@'"));
      grant.append(&host);
    }
    grant.append('\'');

    ROLE_GRANT_PAIR *pair=
      find_role_grant_pair(&acl_entry->user, &host, &acl_role->user);
    DBUG_ASSERT(pair);

    if (pair->with_admin)
      grant.append(STRING_WITH_LEN(" WITH ADMIN OPTION"));

    protocol->prepare_for_resend();
    protocol->store(grant.ptr(),grant.length(),grant.charset());
    if (protocol->write())
    {
      return TRUE;
    }
  }
  return FALSE;
}

static bool show_global_privileges(THD *thd, ACL_USER_BASE *acl_entry,
                                   bool handle_as_role,
                                   char *buff, size_t buffsize)
{
  uint counter;
  ulong want_access;
  Protocol *protocol= thd->protocol;

  String global(buff,sizeof(buff),system_charset_info);
  global.length(0);
  global.append(STRING_WITH_LEN("GRANT "));

  if (handle_as_role)
    want_access= ((ACL_ROLE *)acl_entry)->initial_role_access;
  else
    want_access= acl_entry->access;
  if (test_all_bits(want_access, (GLOBAL_ACLS & ~ GRANT_ACL)))
    global.append(STRING_WITH_LEN("ALL PRIVILEGES"));
  else if (!(want_access & ~GRANT_ACL))
    global.append(STRING_WITH_LEN("USAGE"));
  else
  {
    bool found=0;
    ulong j,test_access= want_access & ~GRANT_ACL;
    for (counter=0, j = SELECT_ACL;j <= GLOBAL_ACLS;counter++,j <<= 1)
    {
      if (test_access & j)
      {
        if (found)
          global.append(STRING_WITH_LEN(", "));
        found=1;
        global.append(command_array[counter],command_lengths[counter]);
      }
    }
  }
  global.append (STRING_WITH_LEN(" ON *.* TO '"));
  global.append(acl_entry->user.str, acl_entry->user.length,
                system_charset_info);
  global.append('\'');

  if (!handle_as_role)
  {
    ACL_USER *acl_user= (ACL_USER *)acl_entry;

    global.append (STRING_WITH_LEN("@'"));
    global.append(acl_user->host.hostname, acl_user->hostname_length,
                  system_charset_info);
    global.append ('\'');

    if (acl_user->plugin.str == native_password_plugin_name.str ||
        acl_user->plugin.str == old_password_plugin_name.str)
    {
      if (acl_user->auth_string.length)
      {
        DBUG_ASSERT(acl_user->salt_len);
        global.append(STRING_WITH_LEN(" IDENTIFIED BY PASSWORD '"));
        global.append(acl_user->auth_string.str, acl_user->auth_string.length);
        global.append('\'');
      }
    }
    else
    {
      global.append(STRING_WITH_LEN(" IDENTIFIED VIA "));
      global.append(acl_user->plugin.str, acl_user->plugin.length);
      if (acl_user->auth_string.length)
      {
        global.append(STRING_WITH_LEN(" USING '"));
        global.append(acl_user->auth_string.str, acl_user->auth_string.length);
        global.append('\'');
      }
    }
    /* "show grants" SSL related stuff */
    if (acl_user->ssl_type == SSL_TYPE_ANY)
      global.append(STRING_WITH_LEN(" REQUIRE SSL"));
    else if (acl_user->ssl_type == SSL_TYPE_X509)
      global.append(STRING_WITH_LEN(" REQUIRE X509"));
    else if (acl_user->ssl_type == SSL_TYPE_SPECIFIED)
    {
      int ssl_options = 0;
      global.append(STRING_WITH_LEN(" REQUIRE "));
      if (acl_user->x509_issuer)
      {
        ssl_options++;
        global.append(STRING_WITH_LEN("ISSUER \'"));
        global.append(acl_user->x509_issuer,strlen(acl_user->x509_issuer));
        global.append('\'');
      }
      if (acl_user->x509_subject)
      {
        if (ssl_options++)
          global.append(' ');
        global.append(STRING_WITH_LEN("SUBJECT \'"));
        global.append(acl_user->x509_subject,strlen(acl_user->x509_subject),
                      system_charset_info);
        global.append('\'');
      }
      if (acl_user->ssl_cipher)
      {
        if (ssl_options++)
          global.append(' ');
        global.append(STRING_WITH_LEN("CIPHER '"));
        global.append(acl_user->ssl_cipher,strlen(acl_user->ssl_cipher),
                      system_charset_info);
        global.append('\'');
      }
    }
    if ((want_access & GRANT_ACL) ||
        (acl_user->user_resource.questions ||
         acl_user->user_resource.updates ||
         acl_user->user_resource.conn_per_hour ||
         acl_user->user_resource.user_conn ||
         acl_user->user_resource.max_statement_time != 0.0))
    {
      global.append(STRING_WITH_LEN(" WITH"));
      if (want_access & GRANT_ACL)
        global.append(STRING_WITH_LEN(" GRANT OPTION"));
      add_user_option(&global, acl_user->user_resource.questions,
                      "MAX_QUERIES_PER_HOUR", false);
      add_user_option(&global, acl_user->user_resource.updates,
                      "MAX_UPDATES_PER_HOUR", false);
      add_user_option(&global, acl_user->user_resource.conn_per_hour,
                      "MAX_CONNECTIONS_PER_HOUR", false);
      add_user_option(&global, acl_user->user_resource.user_conn,
                      "MAX_USER_CONNECTIONS", true);
      add_user_option(&global, acl_user->user_resource.max_statement_time,
                      "MAX_STATEMENT_TIME");
    }
  }

  protocol->prepare_for_resend();
  protocol->store(global.ptr(),global.length(),global.charset());
  if (protocol->write())
    return TRUE;

  return FALSE;

}

static bool show_database_privileges(THD *thd, const char *username,
                                     const char *hostname,
                                     char *buff, size_t buffsize)
{
  ACL_DB *acl_db;
  ulong want_access;
  uint counter;
  Protocol *protocol= thd->protocol;

  for (counter=0 ; counter < acl_dbs.elements ; counter++)
  {
    const char *user, *host;

    acl_db=dynamic_element(&acl_dbs,counter,ACL_DB*);
    user= safe_str(acl_db->user);
    host=acl_db->host.hostname;

    /*
      We do not make SHOW GRANTS case-sensitive here (like REVOKE),
      but make it case-insensitive because that's the way they are
      actually applied, and showing fewer privileges than are applied
      would be wrong from a security point of view.
    */

    if (!strcmp(username, user) &&
        !my_strcasecmp(system_charset_info, hostname, host))
    {
      /*
        do not print inherited access bits for roles,
        the role bits present in the table are what matters
      */
      if (*hostname) // User
        want_access=acl_db->access;
      else // Role
        want_access=acl_db->initial_access;
      if (want_access)
      {
        String db(buff,sizeof(buff),system_charset_info);
        db.length(0);
        db.append(STRING_WITH_LEN("GRANT "));

        if (test_all_bits(want_access,(DB_ACLS & ~GRANT_ACL)))
          db.append(STRING_WITH_LEN("ALL PRIVILEGES"));
        else if (!(want_access & ~GRANT_ACL))
          db.append(STRING_WITH_LEN("USAGE"));
        else
        {
          int found=0, cnt;
          ulong j,test_access= want_access & ~GRANT_ACL;
          for (cnt=0, j = SELECT_ACL; j <= DB_ACLS; cnt++,j <<= 1)
          {
            if (test_access & j)
            {
              if (found)
                db.append(STRING_WITH_LEN(", "));
              found = 1;
              db.append(command_array[cnt],command_lengths[cnt]);
            }
          }
        }
        db.append (STRING_WITH_LEN(" ON "));
        append_identifier(thd, &db, acl_db->db, strlen(acl_db->db));
        db.append (STRING_WITH_LEN(".* TO '"));
        db.append(username, strlen(username),
                  system_charset_info);
        if (*hostname)
        {
          db.append (STRING_WITH_LEN("'@'"));
          // host and lex_user->host are equal except for case
          db.append(host, strlen(host), system_charset_info);
        }
        db.append ('\'');
        if (want_access & GRANT_ACL)
          db.append(STRING_WITH_LEN(" WITH GRANT OPTION"));
        protocol->prepare_for_resend();
        protocol->store(db.ptr(),db.length(),db.charset());
        if (protocol->write())
        {
          return TRUE;
        }
      }
    }
  }
  return FALSE;

}

static bool show_table_and_column_privileges(THD *thd, const char *username,
                                             const char *hostname,
                                             char *buff, size_t buffsize)
{
  uint counter, index;
  Protocol *protocol= thd->protocol;

  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user, *host;
    GRANT_TABLE *grant_table= (GRANT_TABLE*)
      my_hash_element(&column_priv_hash, index);

    user= safe_str(grant_table->user);
    host= grant_table->host.hostname;

    /*
      We do not make SHOW GRANTS case-sensitive here (like REVOKE),
      but make it case-insensitive because that's the way they are
      actually applied, and showing fewer privileges than are applied
      would be wrong from a security point of view.
    */

    if (!strcmp(username,user) &&
        !my_strcasecmp(system_charset_info, hostname, host))
    {
      ulong table_access;
      ulong cols_access;
      if (*hostname) // User
      {
        table_access= grant_table->privs;
        cols_access= grant_table->cols;
      }
      else // Role
      {
        table_access= grant_table->init_privs;
        cols_access= grant_table->init_cols;
      }

      if ((table_access | cols_access) != 0)
      {
        String global(buff, sizeof(buff), system_charset_info);
        ulong test_access= (table_access | cols_access) & ~GRANT_ACL;

        global.length(0);
        global.append(STRING_WITH_LEN("GRANT "));

        if (test_all_bits(table_access, (TABLE_ACLS & ~GRANT_ACL)))
          global.append(STRING_WITH_LEN("ALL PRIVILEGES"));
        else if (!test_access)
          global.append(STRING_WITH_LEN("USAGE"));
        else
        {
          /* Add specific column access */
          int found= 0;
          ulong j;

          for (counter= 0, j= SELECT_ACL; j <= TABLE_ACLS; counter++, j<<= 1)
          {
            if (test_access & j)
            {
              if (found)
                global.append(STRING_WITH_LEN(", "));
              found= 1;
              global.append(command_array[counter],command_lengths[counter]);

              if (grant_table->cols)
              {
                uint found_col= 0;
                HASH *hash_columns;
                hash_columns= &grant_table->hash_columns;

                for (uint col_index=0 ;
                     col_index < hash_columns->records ;
                     col_index++)
                {
                  GRANT_COLUMN *grant_column = (GRANT_COLUMN*)
                    my_hash_element(hash_columns,col_index);
                  if (j & (*hostname ? grant_column->rights         // User
                                     : grant_column->init_rights))  // Role
                  {
                    if (!found_col)
                    {
                      found_col= 1;
                      /*
                        If we have a duplicated table level privilege, we
                        must write the access privilege name again.
                      */
                      if (table_access & j)
                      {
                        global.append(STRING_WITH_LEN(", "));
                        global.append(command_array[counter],
                                      command_lengths[counter]);
                      }
                      global.append(STRING_WITH_LEN(" ("));
                    }
                    else
                      global.append(STRING_WITH_LEN(", "));
                    global.append(grant_column->column,
                                  grant_column->key_length,
                                  system_charset_info);
                  }
                }
                if (found_col)
                  global.append(')');
              }
            }
          }
        }
        global.append(STRING_WITH_LEN(" ON "));
        append_identifier(thd, &global, grant_table->db,
                          strlen(grant_table->db));
        global.append('.');
        append_identifier(thd, &global, grant_table->tname,
                          strlen(grant_table->tname));
        global.append(STRING_WITH_LEN(" TO '"));
        global.append(username, strlen(username),
                      system_charset_info);
        if (*hostname)
        {
          global.append(STRING_WITH_LEN("'@'"));
          // host and lex_user->host are equal except for case
          global.append(host, strlen(host), system_charset_info);
        }
        global.append('\'');
        if (table_access & GRANT_ACL)
          global.append(STRING_WITH_LEN(" WITH GRANT OPTION"));
        protocol->prepare_for_resend();
        protocol->store(global.ptr(),global.length(),global.charset());
        if (protocol->write())
        {
          return TRUE;
        }
      }
    }
  }
  return FALSE;

}

static int show_routine_grants(THD* thd,
                               const char *username, const char *hostname,
                               HASH *hash, const char *type, int typelen,
                               char *buff, int buffsize)
{
  uint counter, index;
  int error= 0;
  Protocol *protocol= thd->protocol;
  /* Add routine access */
  for (index=0 ; index < hash->records ; index++)
  {
    const char *user, *host;
    GRANT_NAME *grant_proc= (GRANT_NAME*) my_hash_element(hash, index);

    user= safe_str(grant_proc->user);
    host= grant_proc->host.hostname;

    /*
      We do not make SHOW GRANTS case-sensitive here (like REVOKE),
      but make it case-insensitive because that's the way they are
      actually applied, and showing fewer privileges than are applied
      would be wrong from a security point of view.
    */

    if (!strcmp(username, user) &&
        !my_strcasecmp(system_charset_info, hostname, host))
    {
      ulong proc_access;
      if (*hostname) // User
        proc_access= grant_proc->privs;
      else // Role
        proc_access= grant_proc->init_privs;

      if (proc_access != 0)
      {
	String global(buff, buffsize, system_charset_info);
	ulong test_access= proc_access & ~GRANT_ACL;

	global.length(0);
	global.append(STRING_WITH_LEN("GRANT "));

	if (!test_access)
 	  global.append(STRING_WITH_LEN("USAGE"));
	else
	{
          /* Add specific procedure access */
	  int found= 0;
	  ulong j;

	  for (counter= 0, j= SELECT_ACL; j <= PROC_ACLS; counter++, j<<= 1)
	  {
	    if (test_access & j)
	    {
	      if (found)
		global.append(STRING_WITH_LEN(", "));
	      found= 1;
	      global.append(command_array[counter],command_lengths[counter]);
	    }
	  }
	}
	global.append(STRING_WITH_LEN(" ON "));
        global.append(type,typelen);
        global.append(' ');
	append_identifier(thd, &global, grant_proc->db,
			  strlen(grant_proc->db));
	global.append('.');
	append_identifier(thd, &global, grant_proc->tname,
			  strlen(grant_proc->tname));
	global.append(STRING_WITH_LEN(" TO '"));
        global.append(username, strlen(username),
		      system_charset_info);
        if (*hostname)
        {
          global.append(STRING_WITH_LEN("'@'"));
          // host and lex_user->host are equal except for case
          global.append(host, strlen(host), system_charset_info);
        }
	global.append('\'');
	if (proc_access & GRANT_ACL)
	  global.append(STRING_WITH_LEN(" WITH GRANT OPTION"));
	protocol->prepare_for_resend();
	protocol->store(global.ptr(),global.length(),global.charset());
	if (protocol->write())
	{
	  error= -1;
	  break;
	}
      }
    }
  }
  return error;
}


/*
  Make a clear-text version of the requested privilege.
*/

void get_privilege_desc(char *to, uint max_length, ulong access)
{
  uint pos;
  char *start=to;
  DBUG_ASSERT(max_length >= 30);                // For end ', ' removal

  if (access)
  {
    max_length--;				// Reserve place for end-zero
    for (pos=0 ; access ; pos++, access>>=1)
    {
      if ((access & 1) &&
	  command_lengths[pos] + (uint) (to-start) < max_length)
      {
	to= strmov(to, command_array[pos]);
        *to++= ',';
        *to++= ' ';
      }
    }
    to--;                                       // Remove end ' '
    to--;					// Remove end ','
  }
  *to=0;
}


void get_mqh(const char *user, const char *host, USER_CONN *uc)
{
  ACL_USER *acl_user;

  mysql_mutex_lock(&acl_cache->lock);

  if (initialized && (acl_user= find_user_wild(host,user)))
    uc->user_resources= acl_user->user_resource;
  else
    bzero((char*) &uc->user_resources, sizeof(uc->user_resources));

  mysql_mutex_unlock(&acl_cache->lock);
}

/*
  Initialize a TABLE_LIST array and open grant tables

  All tables will be opened with the same lock type, either read or write.

  @retval  1 replication filters matched. Abort the operation, but return OK (!)
  @retval  0 tables were opened successfully
  @retval -1 error, tables could not be opened
*/

static int open_grant_tables(THD *thd, TABLE_LIST *tables,
                             enum thr_lock_type lock_type, int tables_to_open)
{
  DBUG_ENTER("open_grant_tables");

  /*
    We can read privilege tables even when !initialized.
    This can be acl_load() - server startup or FLUSH PRIVILEGES
  */
  if (lock_type >= TL_WRITE_ALLOW_WRITE && !initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(-1);
  }

  int prev= -1;
  bzero(tables, sizeof(TABLE_LIST) * TABLES_MAX);
  for (int cur=TABLES_MAX-1, mask= 1 << cur; mask; cur--, mask >>= 1)
  {
    if ((tables_to_open & mask) == 0)
      continue;
    tables[cur].init_one_table(C_STRING_WITH_LEN("mysql"),
                               acl_table_names[cur].str,
                               acl_table_names[cur].length,
                               acl_table_names[cur].str, lock_type);
    tables[cur].open_type= OT_BASE_ONLY;
    if (lock_type >= TL_WRITE_ALLOW_WRITE)
      tables[cur].updating= 1;
    if (cur >= FIRST_OPTIONAL_TABLE)
      tables[cur].open_strategy= TABLE_LIST::OPEN_IF_EXISTS;
    if (prev != -1)
      tables[cur].next_local= tables[cur].next_global= & tables[prev];
    prev= cur;
  }

#ifdef HAVE_REPLICATION
  if (lock_type >= TL_WRITE_ALLOW_WRITE && thd->slave_thread && !thd->spcont)
  {
    /*
      GRANT and REVOKE are applied the slave in/exclusion rules as they are
      some kind of updates to the mysql.% tables.
    */
    Rpl_filter *rpl_filter= thd->system_thread_info.rpl_sql_info->rpl_filter;
    if (rpl_filter->is_on() && !rpl_filter->tables_ok(0, tables))
      DBUG_RETURN(1);
  }
#endif

  if (open_and_lock_tables(thd, tables + prev, FALSE,
                           MYSQL_LOCK_IGNORE_TIMEOUT))
    DBUG_RETURN(-1);

  DBUG_RETURN(0);
}

/*
  Modify a privilege table.

  SYNOPSIS
    modify_grant_table()
    table                       The table to modify.
    host_field                  The host name field.
    user_field                  The user name field.
    user_to                     The new name for the user if to be renamed,
                                NULL otherwise.

  DESCRIPTION
  Update user/host in the current record if user_to is not NULL.
  Delete the current record if user_to is NULL.

  RETURN
    0           OK.
    != 0        Error.
*/

static int modify_grant_table(TABLE *table, Field *host_field,
                              Field *user_field, LEX_USER *user_to)
{
  int error;
  DBUG_ENTER("modify_grant_table");

  if (user_to)
  {
    /* rename */
    store_record(table, record[1]);
    host_field->store(user_to->host.str, user_to->host.length,
                      system_charset_info);
    user_field->store(user_to->user.str, user_to->user.length,
                      system_charset_info);
    if ((error= table->file->ha_update_row(table->record[1],
                                           table->record[0])) &&
        error != HA_ERR_RECORD_IS_THE_SAME)
      table->file->print_error(error, MYF(0));
    else
      error= 0;
  }
  else
  {
    /* delete */
    if ((error=table->file->ha_delete_row(table->record[0])))
      table->file->print_error(error, MYF(0));
  }

  DBUG_RETURN(error);
}

/*
  Handle the roles_mapping privilege table
*/
static int handle_roles_mappings_table(TABLE *table, bool drop,
                                       LEX_USER *user_from, LEX_USER *user_to)
{
  /*
    All entries (Host, User) that match user_from will be renamed,
    as well as all Role entries that match if user_from.host.str == ""

    Otherwise, only matching (Host, User) will be renamed.
  */
  DBUG_ENTER("handle_roles_mappings_table");

  int error;
  int result= 0;
  THD *thd= table->in_use;
  const char *host, *user, *role;
  Field *host_field= table->field[0];
  Field *user_field= table->field[1];
  Field *role_field= table->field[2];

  DBUG_PRINT("info", ("Rewriting entry in roles_mapping table: %s@%s",
                      user_from->user.str, user_from->host.str));
  table->use_all_columns();
  if ((error= table->file->ha_rnd_init(1)))
  {
    table->file->print_error(error, MYF(0));
    result= -1;
  }
  else
  {
    while((error= table->file->ha_rnd_next(table->record[0])) !=
          HA_ERR_END_OF_FILE)
    {
      if (error)
      {
        DBUG_PRINT("info", ("scan error: %d", error));
        continue;
      }

      host= safe_str(get_field(thd->mem_root, host_field));
      user= safe_str(get_field(thd->mem_root, user_field));

      if (!(strcmp(user_from->user.str, user) ||
            my_strcasecmp(system_charset_info, user_from->host.str, host)))
        result= ((drop || user_to) &&
                 modify_grant_table(table, host_field, user_field, user_to)) ?
          -1 : result ? result : 1; /* Error or keep result or found. */
      else
      {
        role= safe_str(get_field(thd->mem_root, role_field));

        if (!user_from->is_role() || strcmp(user_from->user.str, role))
          continue;

        error= 0;

        if (drop) /* drop if requested */
        {
          if ((error= table->file->ha_delete_row(table->record[0])))
            table->file->print_error(error, MYF(0));
        }
        else if (user_to)
        {
          store_record(table, record[1]);
          role_field->store(user_to->user.str, user_to->user.length,
                            system_charset_info);
          if ((error= table->file->ha_update_row(table->record[1],
                                                 table->record[0])) &&
              error != HA_ERR_RECORD_IS_THE_SAME)
            table->file->print_error(error, MYF(0));
        }

        /* Error or keep result or found. */
        result= error ? -1 : result ? result : 1;
      }
    }
    table->file->ha_rnd_end();
  }
  DBUG_RETURN(result);
}

/*
  Handle a privilege table.

  SYNOPSIS
    handle_grant_table()
    tables                      The array with the four open tables.
    table_no                    The number of the table to handle (0..4).
    drop                        If user_from is to be dropped.
    user_from                   The the user to be searched/dropped/renamed.
    user_to                     The new name for the user if to be renamed,
                                NULL otherwise.

  DESCRIPTION
    Scan through all records in a grant table and apply the requested
    operation. For the "user" table, a single index access is sufficient,
    since there is an unique index on (host, user).
    Delete from grant table if drop is true.
    Update in grant table if drop is false and user_to is not NULL.
    Search in grant table if drop is false and user_to is NULL.

  RETURN
    > 0         At least one record matched.
    0           OK, but no record matched.
    < 0         Error.
*/

static int handle_grant_table(THD *thd, TABLE_LIST *tables,
                              enum enum_acl_tables table_no, bool drop,
                              LEX_USER *user_from, LEX_USER *user_to)
{
  int result= 0;
  int error;
  TABLE *table= tables[table_no].table;
  Field *host_field= table->field[0];
  Field *user_field= table->field[table_no == USER_TABLE ||
                                  table_no == PROXIES_PRIV_TABLE ? 1 : 2];
  const char *host_str= user_from->host.str;
  const char *user_str= user_from->user.str;
  const char *host;
  const char *user;
  uchar user_key[MAX_KEY_LENGTH];
  uint key_prefix_length;
  DBUG_ENTER("handle_grant_table");

  if (table_no == ROLES_MAPPING_TABLE)
  {
    result= handle_roles_mappings_table(table, drop, user_from, user_to);
    DBUG_RETURN(result);
  }

  table->use_all_columns();
  if (table_no == USER_TABLE) // mysql.user table
  {
    /*
      The 'user' table has an unique index on (host, user).
      Thus, we can handle everything with a single index access.
      The host- and user fields are consecutive in the user table records.
      So we set host- and user fields of table->record[0] and use the
      pointer to the host field as key.
      index_read_idx() will replace table->record[0] (its first argument)
      by the searched record, if it exists.
    */
    DBUG_PRINT("info",("read table: '%s'  search: '%s'@'%s'",
                       table->s->table_name.str, user_str, host_str));
    host_field->store(host_str, user_from->host.length, system_charset_info);
    user_field->store(user_str, user_from->user.length, system_charset_info);

    key_prefix_length= (table->key_info->key_part[0].store_length +
                        table->key_info->key_part[1].store_length);
    key_copy(user_key, table->record[0], table->key_info, key_prefix_length);

    error= table->file->ha_index_read_idx_map(table->record[0], 0,
                                              user_key, (key_part_map)3,
                                              HA_READ_KEY_EXACT);
    if (!error && !*host_str)
    { // verify that we got a role or a user, as needed
      if (check_is_role(table) != user_from->is_role())
        error= HA_ERR_KEY_NOT_FOUND;
    }
    if (error)
    {
      if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      {
        table->file->print_error(error, MYF(0));
        result= -1;
      }
    }
    else
    {
      /* If requested, delete or update the record. */
      result= ((drop || user_to) &&
               modify_grant_table(table, host_field, user_field, user_to)) ?
        -1 : 1; /* Error or found. */
    }
    DBUG_PRINT("info",("read result: %d", result));
  }
  else
  {
    /*
      The non-'user' table do not have indexes on (host, user).
      And their host- and user fields are not consecutive.
      Thus, we need to do a table scan to find all matching records.
    */
    if ((error= table->file->ha_rnd_init(1)))
    {
      table->file->print_error(error, MYF(0));
      result= -1;
    }
    else
    {
#ifdef EXTRA_DEBUG
      DBUG_PRINT("info",("scan table: '%s'  search: '%s'@'%s'",
                         table->s->table_name.str, user_str, host_str));
#endif
      while ((error= table->file->ha_rnd_next(table->record[0])) !=
             HA_ERR_END_OF_FILE)
      {
        if (error)
        {
          /* Most probable 'deleted record'. */
          DBUG_PRINT("info",("scan error: %d", error));
          continue;
        }
        host= safe_str(get_field(thd->mem_root, host_field));
        user= safe_str(get_field(thd->mem_root, user_field));

#ifdef EXTRA_DEBUG
        if (table_no != PROXIES_PRIV_TABLE)
        {
          DBUG_PRINT("loop",("scan fields: '%s'@'%s' '%s' '%s' '%s'",
                             user, host,
                             get_field(thd->mem_root, table->field[1]) /*db*/,
                             get_field(thd->mem_root, table->field[3]) /*table*/,
                             get_field(thd->mem_root,
                                       table->field[4]) /*column*/));
        }
#endif
        if (strcmp(user_str, user) ||
            my_strcasecmp(system_charset_info, host_str, host))
          continue;

        /* If requested, delete or update the record. */
        result= ((drop || user_to) &&
                 modify_grant_table(table, host_field, user_field, user_to)) ?
          -1 : result ? result : 1; /* Error or keep result or found. */
        /* If search is requested, we do not need to search further. */
        if (! drop && ! user_to)
          break ;
      }
      (void) table->file->ha_rnd_end();
      DBUG_PRINT("info",("scan result: %d", result));
    }
  }

  DBUG_RETURN(result);
}


/**
  Handle an in-memory privilege structure.

  @param struct_no  The number of the structure to handle (0..6).
  @param drop       If user_from is to be dropped.
  @param user_from  The the user to be searched/dropped/renamed.
  @param user_to    The new name for the user if to be renamed, NULL otherwise.

  @note
    Scan through all elements in an in-memory grant structure and apply
    the requested operation.
    Delete from grant structure if drop is true.
    Update in grant structure if drop is false and user_to is not NULL.
    Search in grant structure if drop is false and user_to is NULL.

  @retval > 0  At least one element matched.
  @retval 0    OK, but no element matched.
*/

static int handle_grant_struct(enum enum_acl_lists struct_no, bool drop,
                               LEX_USER *user_from, LEX_USER *user_to)
{
  int result= 0;
  int idx;
  int elements;
  const char *UNINIT_VAR(user);
  const char *UNINIT_VAR(host);
  ACL_USER *acl_user= NULL;
  ACL_ROLE *acl_role= NULL;
  ACL_DB *acl_db= NULL;
  ACL_PROXY_USER *acl_proxy_user= NULL;
  GRANT_NAME *grant_name= NULL;
  ROLE_GRANT_PAIR *UNINIT_VAR(role_grant_pair);
  HASH *grant_name_hash= NULL;
  HASH *roles_mappings_hash= NULL;
  DBUG_ENTER("handle_grant_struct");
  DBUG_PRINT("info",("scan struct: %u  search: '%s'@'%s'",
                     struct_no, user_from->user.str, user_from->host.str));

  mysql_mutex_assert_owner(&acl_cache->lock);

  /* No point in querying ROLE ACL if user_from is not a role */
  if (struct_no == ROLE_ACL && user_from->host.length)
    DBUG_RETURN(0);

  /* same. no roles in PROXY_USERS_ACL */
  if (struct_no == PROXY_USERS_ACL && user_from->is_role())
    DBUG_RETURN(0);

  if (struct_no == ROLE_ACL) //no need to scan the structures in this case
  {
    acl_role= find_acl_role(user_from->user.str);
    if (!acl_role)
      DBUG_RETURN(0);

    if (!drop && !user_to) //role was found
      DBUG_RETURN(1);

    /* this calls for a role update */
    char *old_key= acl_role->user.str;
    size_t old_key_length= acl_role->user.length;
    if (drop)
    {
      /* all grants must be revoked from this role by now. propagate this */
      propagate_role_grants(acl_role, PRIVS_TO_MERGE::ALL);

      // delete the role from cross-reference arrays
      for (uint i=0; i < acl_role->role_grants.elements; i++)
      {
        ACL_ROLE *grant= *dynamic_element(&acl_role->role_grants,
                                          i, ACL_ROLE**);
        remove_ptr_from_dynarray(&grant->parent_grantee, acl_role);
      }

      for (uint i=0; i < acl_role->parent_grantee.elements; i++)
      {
        ACL_USER_BASE *grantee= *dynamic_element(&acl_role->parent_grantee,
                                                 i, ACL_USER_BASE**);
        remove_ptr_from_dynarray(&grantee->role_grants, acl_role);
      }

      my_hash_delete(&acl_roles, (uchar*) acl_role);
      DBUG_RETURN(1);
    }
    acl_role->user.str= strdup_root(&acl_memroot, user_to->user.str);
    acl_role->user.length= user_to->user.length;

    my_hash_update(&acl_roles, (uchar*) acl_role, (uchar*) old_key,
                   old_key_length);
    DBUG_RETURN(1);

  }

  /* Get the number of elements in the in-memory structure. */
  switch (struct_no) {
  case USER_ACL:
    elements= acl_users.elements;
    break;
  case DB_ACL:
    elements= acl_dbs.elements;
    break;
  case COLUMN_PRIVILEGES_HASH:
    grant_name_hash= &column_priv_hash;
    elements= grant_name_hash->records;
    break;
  case PROC_PRIVILEGES_HASH:
    grant_name_hash= &proc_priv_hash;
    elements= grant_name_hash->records;
    break;
  case FUNC_PRIVILEGES_HASH:
    grant_name_hash= &func_priv_hash;
    elements= grant_name_hash->records;
    break;
  case PROXY_USERS_ACL:
    elements= acl_proxy_users.elements;
    break;
  case ROLES_MAPPINGS_HASH:
    roles_mappings_hash= &acl_roles_mappings;
    elements= roles_mappings_hash->records;
    break;
  default:
    DBUG_ASSERT(0);
    DBUG_RETURN(-1);
  }

#ifdef EXTRA_DEBUG
    DBUG_PRINT("loop",("scan struct: %u  search    user: '%s'  host: '%s'",
                       struct_no, user_from->user.str, user_from->host.str));
#endif
  /* Loop over all elements *backwards* (see the comment below). */
  for (idx= elements - 1; idx >= 0; idx--)
  {
    /*
      Get a pointer to the element.
    */
    switch (struct_no) {
    case USER_ACL:
      acl_user= dynamic_element(&acl_users, idx, ACL_USER*);
      user= acl_user->user.str;
      host= acl_user->host.hostname;
    break;

    case DB_ACL:
      acl_db= dynamic_element(&acl_dbs, idx, ACL_DB*);
      user= acl_db->user;
      host= acl_db->host.hostname;
      break;

    case COLUMN_PRIVILEGES_HASH:
    case PROC_PRIVILEGES_HASH:
    case FUNC_PRIVILEGES_HASH:
      grant_name= (GRANT_NAME*) my_hash_element(grant_name_hash, idx);
      user= grant_name->user;
      host= grant_name->host.hostname;
      break;

    case PROXY_USERS_ACL:
      acl_proxy_user= dynamic_element(&acl_proxy_users, idx, ACL_PROXY_USER*);
      user= acl_proxy_user->get_user();
      host= acl_proxy_user->get_host();
      break;

    case ROLES_MAPPINGS_HASH:
      role_grant_pair= (ROLE_GRANT_PAIR *) my_hash_element(roles_mappings_hash, idx);
      user= role_grant_pair->u_uname;
      host= role_grant_pair->u_hname;
      break;

    default:
      DBUG_ASSERT(0);
    }
    if (! user)
      user= "";
    if (! host)
      host= "";

#ifdef EXTRA_DEBUG
    DBUG_PRINT("loop",("scan struct: %u  index: %u  user: '%s'  host: '%s'",
                       struct_no, idx, user, host));
#endif

    if (struct_no == ROLES_MAPPINGS_HASH)
    {
      const char* role= role_grant_pair->r_uname? role_grant_pair->r_uname: "";
      if (user_from->is_role())
      {
        /* When searching for roles within the ROLES_MAPPINGS_HASH, we have
           to check both the user field as well as the role field for a match.

           It is possible to have a role granted to a role. If we are going
           to modify the mapping entry, it needs to be done on either on the
           "user" end (here represented by a role) or the "role" end. At least
           one part must match.

           If the "user" end has a not-empty host string, it can never match
           as we are searching for a role here. A role always has an empty host
           string.
        */
        if ((*host || strcmp(user_from->user.str, user)) &&
            strcmp(user_from->user.str, role))
          continue;
      }
      else
      {
        if (strcmp(user_from->user.str, user) ||
            my_strcasecmp(system_charset_info, user_from->host.str, host))
          continue;
      }
    }
    else
    {
      if (strcmp(user_from->user.str, user) ||
          my_strcasecmp(system_charset_info, user_from->host.str, host))
        continue;
    }

    result= 1; /* At least one element found. */
    if ( drop )
    {
      elements--;
      switch ( struct_no ) {
      case USER_ACL:
        free_acl_user(dynamic_element(&acl_users, idx, ACL_USER*));
        delete_dynamic_element(&acl_users, idx);
        break;

      case DB_ACL:
        delete_dynamic_element(&acl_dbs, idx);
        break;

      case COLUMN_PRIVILEGES_HASH:
      case PROC_PRIVILEGES_HASH:
      case FUNC_PRIVILEGES_HASH:
        my_hash_delete(grant_name_hash, (uchar*) grant_name);
        /*
          In our HASH implementation on deletion one elements
          is moved into a place where a deleted element was,
          and the last element is moved into the empty space.
          Thus we need to re-examine the current element, but
          we don't have to restart the search from the beginning.
        */
        if (idx != elements)
          idx++;
	break;

      case PROXY_USERS_ACL:
        delete_dynamic_element(&acl_proxy_users, idx);
        break;

      case ROLES_MAPPINGS_HASH:
        my_hash_delete(roles_mappings_hash, (uchar*) role_grant_pair);
        if (idx != elements)
          idx++;
        break;

      default:
        DBUG_ASSERT(0);
        break;
      }
    }
    else if ( user_to )
    {
      switch ( struct_no ) {
      case USER_ACL:
        acl_user->user.str= strdup_root(&acl_memroot, user_to->user.str);
        acl_user->user.length= user_to->user.length;
        acl_user->host.hostname= strdup_root(&acl_memroot, user_to->host.str);
        acl_user->hostname_length= user_to->host.length;
        break;

      case DB_ACL:
        acl_db->user= strdup_root(&acl_memroot, user_to->user.str);
        acl_db->host.hostname= strdup_root(&acl_memroot, user_to->host.str);
        break;

      case COLUMN_PRIVILEGES_HASH:
      case PROC_PRIVILEGES_HASH:
      case FUNC_PRIVILEGES_HASH:
        {
          /*
            Save old hash key and its length to be able to properly update
            element position in hash.
          */
          char *old_key= grant_name->hash_key;
          size_t old_key_length= grant_name->key_length;

          /*
            Update the grant structure with the new user name and host name.
          */
          grant_name->set_user_details(user_to->host.str, grant_name->db,
                                       user_to->user.str, grant_name->tname,
                                       TRUE);

          /*
            Since username is part of the hash key, when the user name
            is renamed, the hash key is changed. Update the hash to
            ensure that the position matches the new hash key value
          */
          my_hash_update(grant_name_hash, (uchar*) grant_name, (uchar*) old_key,
                         old_key_length);
          /*
            hash_update() operation could have moved element from the tail or
            the head of the hash to the current position.  But it can never
            move an element from the head to the tail or from the tail to the
            head over the current element.
            So we need to examine the current element once again, but
            we don't need to restart the search from the beginning.
          */
          idx++;
          break;
        }

      case PROXY_USERS_ACL:
        acl_proxy_user->set_user (&acl_memroot, user_to->user.str);
        acl_proxy_user->set_host (&acl_memroot, user_to->host.str);
        break;

      case ROLES_MAPPINGS_HASH:
        {
          /*
            Save old hash key and its length to be able to properly update
            element position in hash.
          */
          char *old_key= role_grant_pair->hashkey.str;
          size_t old_key_length= role_grant_pair->hashkey.length;
          bool oom;

          if (user_to->is_role())
            oom= role_grant_pair->init(&acl_memroot, role_grant_pair->u_uname,
                                       role_grant_pair->u_hname,
                                       user_to->user.str, false);
          else
            oom= role_grant_pair->init(&acl_memroot, user_to->user.str,
                                       user_to->host.str,
                                       role_grant_pair->r_uname, false);
          if (oom)
            DBUG_RETURN(-1);

          my_hash_update(roles_mappings_hash, (uchar*) role_grant_pair,
                         (uchar*) old_key, old_key_length);
          idx++; // see the comment above
          break;
        }

      default:
        DBUG_ASSERT(0);
        break;
      }

    }
    else
    {
      /* If search is requested, we do not need to search further. */
      break;
    }
  }
#ifdef EXTRA_DEBUG
  DBUG_PRINT("loop",("scan struct: %u  result %d", struct_no, result));
#endif

  DBUG_RETURN(result);
}


/*
  Handle all privilege tables and in-memory privilege structures.

  SYNOPSIS
    handle_grant_data()
    tables                      The array with the four open tables.
    drop                        If user_from is to be dropped.
    user_from                   The the user to be searched/dropped/renamed.
    user_to                     The new name for the user if to be renamed,
                                NULL otherwise.

  DESCRIPTION
    Go through all grant tables and in-memory grant structures and apply
    the requested operation.
    Delete from grant data if drop is true.
    Update in grant data if drop is false and user_to is not NULL.
    Search in grant data if drop is false and user_to is NULL.

  RETURN
    > 0         At least one element matched.
    0           OK, but no element matched.
    < 0         Error.
*/

static int handle_grant_data(THD *thd, TABLE_LIST *tables, bool drop,
                             LEX_USER *user_from, LEX_USER *user_to)
{
  int result= 0;
  int found;
  bool handle_as_role= user_from->is_role();
  bool search_only= !drop && !user_to;
  DBUG_ENTER("handle_grant_data");

  if (user_to)
    DBUG_ASSERT(handle_as_role == user_to->is_role());

  if (search_only)
  {
    /* quickly search in-memory structures first */
    if (handle_as_role && find_acl_role(user_from->user.str))
      DBUG_RETURN(1); // found

    if (!handle_as_role && find_user_exact(user_from->host.str, user_from->user.str))
      DBUG_RETURN(1); // found
  }

  /* Handle db table. */
  if ((found= handle_grant_table(thd, tables, DB_TABLE, drop, user_from,
                                 user_to)) < 0)
  {
    /* Handle of table failed, don't touch the in-memory array. */
    result= -1;
  }
  else
  {
    /* Handle db array. */
    if ((handle_grant_struct(DB_ACL, drop, user_from, user_to) || found)
        && ! result)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (search_only)
        goto end;
      acl_cache->clear(1);
    }
  }

  /* Handle stored routines table. */
  if ((found= handle_grant_table(thd, tables, PROCS_PRIV_TABLE, drop,
                                 user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch in-memory array. */
    result= -1;
  }
  else
  {
    /* Handle procs array. */
    if ((handle_grant_struct(PROC_PRIVILEGES_HASH, drop, user_from, user_to) || found)
        && ! result)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (search_only)
        goto end;
    }
    /* Handle funcs array. */
    if ((handle_grant_struct(FUNC_PRIVILEGES_HASH, drop, user_from, user_to) || found)
        && ! result)
    {
      result= 1; /* At least one record/element found. */
      /* If search is requested, we do not need to search further. */
      if (search_only)
        goto end;
    }
  }

  /* Handle tables table. */
  if ((found= handle_grant_table(thd, tables, TABLES_PRIV_TABLE, drop,
                                 user_from, user_to)) < 0)
  {
    /* Handle of table failed, don't touch columns and in-memory array. */
    result= -1;
  }
  else
  {
    if (found && ! result)
    {
      result= 1; /* At least one record found. */
      /* If search is requested, we do not need to search further. */
      if (search_only)
        goto end;
    }

    /* Handle columns table. */
    if ((found= handle_grant_table(thd, tables, COLUMNS_PRIV_TABLE, drop,
                                   user_from, user_to)) < 0)
    {
      /* Handle of table failed, don't touch the in-memory array. */
      result= -1;
    }
    else
    {
      /* Handle columns hash. */
      if ((handle_grant_struct(COLUMN_PRIVILEGES_HASH, drop, user_from, user_to) || found)
          && ! result)
        result= 1; /* At least one record/element found. */
      if (search_only)
        goto end;
    }
  }

  /* Handle proxies_priv table. */
  if (tables[PROXIES_PRIV_TABLE].table)
  {
    if ((found= handle_grant_table(thd, tables, PROXIES_PRIV_TABLE, drop,
                                   user_from, user_to)) < 0)
    {
      /* Handle of table failed, don't touch the in-memory array. */
      result= -1;
    }
    else
    {
      /* Handle proxies_priv array. */
      if ((handle_grant_struct(PROXY_USERS_ACL, drop, user_from, user_to) || found)
          && ! result)
        result= 1; /* At least one record/element found. */
      if (search_only)
        goto end;
    }
  }

  /* Handle roles_mapping table. */
  if (tables[ROLES_MAPPING_TABLE].table)
  {
    if ((found= handle_grant_table(thd, tables, ROLES_MAPPING_TABLE, drop,
                                   user_from, user_to)) < 0)
    {
      /* Handle of table failed, don't touch the in-memory array. */
      result= -1;
    }
    else
    {
      /* Handle acl_roles_mappings array */
      if ((handle_grant_struct(ROLES_MAPPINGS_HASH, drop, user_from, user_to) || found)
          && ! result)
        result= 1; /* At least one record/element found */
      if (search_only)
        goto end;
    }
  }

  /* Handle user table. */
  if ((found= handle_grant_table(thd, tables, USER_TABLE, drop, user_from,
                                 user_to)) < 0)
  {
    /* Handle of table failed, don't touch the in-memory array. */
    result= -1;
  }
  else
  {
    enum enum_acl_lists what= handle_as_role ? ROLE_ACL : USER_ACL;
    if (((handle_grant_struct(what, drop, user_from, user_to)) || found) && !result)
    {
      result= 1; /* At least one record/element found. */
      DBUG_ASSERT(! search_only);
    }
  }

end:
  DBUG_RETURN(result);
}

/*
  Create a list of users.

  SYNOPSIS
    mysql_create_user()
    thd                         The current thread.
    list                        The users to create.
    handle_as_role              Handle the user list as roles if true

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_create_user(THD *thd, List <LEX_USER> &list, bool handle_as_role)
{
  int result;
  String wrong_users;
  LEX_USER *user_name;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[TABLES_MAX];
  bool binlog= false;
  DBUG_ENTER("mysql_create_user");
  DBUG_PRINT("entry", ("Handle as %s", handle_as_role ? "role" : "user"));

  if (handle_as_role && sp_process_definer(thd))
    DBUG_RETURN(TRUE);

  /* CREATE USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user | Table_db |
                       Table_tables_priv | Table_columns_priv |
                       Table_procs_priv | Table_proxies_priv |
                       Table_roles_mapping)))
    DBUG_RETURN(result != 1);

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  while ((user_name= user_list++))
  {
    if (user_name->user.str == current_user.str)
    {
      append_str(&wrong_users, STRING_WITH_LEN("CURRENT_USER"));
      result= TRUE;
      continue;
    }

    if (user_name->user.str == current_role.str)
    {
      append_str(&wrong_users, STRING_WITH_LEN("CURRENT_ROLE"));
      result= TRUE;
      continue;
    }

    if (handle_as_role && is_invalid_role_name(user_name->user.str))
    {
      append_user(thd, &wrong_users, user_name);
      result= TRUE;
      continue;
    }

    if (!user_name->host.str)
      user_name->host= host_not_specified;

    if (fix_lex_user(thd, user_name))
    {
      append_user(thd, &wrong_users, user_name);
      result= TRUE;
      continue;
    }

    /*
      Search all in-memory structures and grant tables
      for a mention of the new user/role name.
    */
    if (handle_grant_data(thd, tables, 0, user_name, NULL))
    {
      if (thd->lex->create_info.or_replace())
      {
        // Drop the existing user
        if (handle_grant_data(thd, tables, 1, user_name, NULL) <= 0)
        {
          // DROP failed
          append_user(thd, &wrong_users, user_name);
          result= true;
          continue;
        }
        // Proceed with the creation
      }
      else if (thd->lex->create_info.if_not_exists())
      {
        binlog= true;
        if (handle_as_role)
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                              ER_ROLE_CREATE_EXISTS,
                              ER_THD(thd, ER_ROLE_CREATE_EXISTS),
                              user_name->user.str);
        else
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                              ER_USER_CREATE_EXISTS,
                              ER_THD(thd, ER_USER_CREATE_EXISTS),
                              user_name->user.str, user_name->host.str);
        continue;
      }
      else
      {
        // "CREATE USER user1" for an existing user
        append_user(thd, &wrong_users, user_name);
        result= true;
        continue;
      }
    }

    binlog= true;
    if (replace_user_table(thd, tables[USER_TABLE].table, *user_name, 0, 0, 1, 0))
    {
      append_user(thd, &wrong_users, user_name);
      result= TRUE;
      continue;
    }

    // every created role is automatically granted to its creator-admin
    if (handle_as_role)
    {
      ACL_USER_BASE *grantee= find_acl_user_base(thd->lex->definer->user.str,
                                                 thd->lex->definer->host.str);
      ACL_ROLE *role= find_acl_role(user_name->user.str);

      /*
        just like with routines, views, triggers, and events we allow
        non-existant definers here with a warning (see sp_process_definer())
      */
      if (grantee)
        add_role_user_mapping(grantee, role);

      if (replace_roles_mapping_table(tables[ROLES_MAPPING_TABLE].table,
                                      &thd->lex->definer->user,
                                      &thd->lex->definer->host,
                                      &user_name->user, true,
                                      NULL, false))
      {
        append_user(thd, &wrong_users, user_name);
        if (grantee)
          undo_add_role_user_mapping(grantee, role);
        result= TRUE;
      }
      else if (grantee)
             update_role_mapping(&thd->lex->definer->user,
                                 &thd->lex->definer->host,
                                 &user_name->user, true, NULL, false);
    }
  }

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
    my_error(ER_CANNOT_USER, MYF(0),
             (handle_as_role) ? "CREATE ROLE" : "CREATE USER",
             wrong_users.c_ptr_safe());

  if (binlog)
    result |= write_bin_log(thd, FALSE, thd->query(), thd->query_length());

  mysql_rwlock_unlock(&LOCK_grant);
  DBUG_RETURN(result);
}

/*
  Drop a list of users and all their privileges.

  SYNOPSIS
    mysql_drop_user()
    thd                         The current thread.
    list                        The users to drop.

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_drop_user(THD *thd, List <LEX_USER> &list, bool handle_as_role)
{
  int result;
  String wrong_users;
  LEX_USER *user_name, *tmp_user_name;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[TABLES_MAX];
  bool binlog= false;
  ulonglong old_sql_mode= thd->variables.sql_mode;
  DBUG_ENTER("mysql_drop_user");
  DBUG_PRINT("entry", ("Handle as %s", handle_as_role ? "role" : "user"));

  /* DROP USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user | Table_db |
                       Table_tables_priv | Table_columns_priv |
                       Table_procs_priv | Table_proxies_priv |
                       Table_roles_mapping)))
    DBUG_RETURN(result != 1);

  thd->variables.sql_mode&= ~MODE_PAD_CHAR_TO_FULL_LENGTH;

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  while ((tmp_user_name= user_list++))
  {
    int rc;
    user_name= get_current_user(thd, tmp_user_name, false);
    if (!user_name)
    {
      thd->clear_error();
      append_str(&wrong_users, STRING_WITH_LEN("CURRENT_ROLE"));
      result= TRUE;
      continue;
    }

    if (handle_as_role != user_name->is_role())
    {
      append_user(thd, &wrong_users, user_name);
      result= TRUE;
      continue;
    }

    if ((rc= handle_grant_data(thd, tables, 1, user_name, NULL)) > 0)
    {
      // The user or role was successfully deleted
      binlog= true;
      continue;
    }

    if (rc == 0 && thd->lex->if_exists())
    {
      // "DROP USER IF EXISTS user1" for a non-existing user or role
      if (handle_as_role)
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_ROLE_DROP_EXISTS,
                            ER_THD(thd, ER_ROLE_DROP_EXISTS),
                            user_name->user.str);
      else
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_USER_DROP_EXISTS,
                            ER_THD(thd, ER_USER_DROP_EXISTS),
                            user_name->user.str, user_name->host.str);
      binlog= true;
      continue;
    }
    // Internal error, or "DROP USER user1" for a non-existing user
    append_user(thd, &wrong_users, user_name);
    result= TRUE;
  }

  if (!handle_as_role)
  {
    /* Rebuild 'acl_check_hosts' since 'acl_users' has been modified */
    rebuild_check_host();

    /*
      Rebuild every user's role_grants since 'acl_users' has been sorted
      and old pointers to ACL_USER elements are no longer valid
    */
    rebuild_role_grants();
  }

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
    my_error(ER_CANNOT_USER, MYF(0),
             (handle_as_role) ? "DROP ROLE" : "DROP USER",
             wrong_users.c_ptr_safe());

  if (binlog)
    result |= write_bin_log(thd, FALSE, thd->query(), thd->query_length());

  mysql_rwlock_unlock(&LOCK_grant);
  thd->variables.sql_mode= old_sql_mode;
  DBUG_RETURN(result);
}

/*
  Rename a user.

  SYNOPSIS
    mysql_rename_user()
    thd                         The current thread.
    list                        The user name pairs: (from, to).

  RETURN
    FALSE       OK.
    TRUE        Error.
*/

bool mysql_rename_user(THD *thd, List <LEX_USER> &list)
{
  int result;
  String wrong_users;
  LEX_USER *user_from, *tmp_user_from;
  LEX_USER *user_to, *tmp_user_to;
  List_iterator <LEX_USER> user_list(list);
  TABLE_LIST tables[TABLES_MAX];
  bool some_users_renamed= FALSE;
  DBUG_ENTER("mysql_rename_user");

  /* RENAME USER may be skipped on replication client. */
  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user | Table_db |
                       Table_tables_priv | Table_columns_priv |
                       Table_procs_priv | Table_proxies_priv |
                       Table_roles_mapping)))
    DBUG_RETURN(result != 1);

  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  while ((tmp_user_from= user_list++))
  {
    tmp_user_to= user_list++;
    if (!(user_from= get_current_user(thd, tmp_user_from, false)))
    {
      append_user(thd, &wrong_users, user_from);
      result= TRUE;
      continue;
    }
    if (!(user_to= get_current_user(thd, tmp_user_to, false)))
    {
      append_user(thd, &wrong_users, user_to);
      result= TRUE;
      continue;
    }
    DBUG_ASSERT(!user_from->is_role());
    DBUG_ASSERT(!user_to->is_role());

    /*
      Search all in-memory structures and grant tables
      for a mention of the new user name.
    */
    if (handle_grant_data(thd, tables, 0, user_to, NULL) ||
        handle_grant_data(thd, tables, 0, user_from, user_to) <= 0)
    {
      /* NOTE TODO renaming roles is not yet implemented */
      append_user(thd, &wrong_users, user_from);
      result= TRUE;
      continue;
    }
    some_users_renamed= TRUE;
  }

  /* Rebuild 'acl_check_hosts' since 'acl_users' has been modified */
  rebuild_check_host();

  /*
    Rebuild every user's role_grants since 'acl_users' has been sorted
    and old pointers to ACL_USER elements are no longer valid
  */
  rebuild_role_grants();

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
    my_error(ER_CANNOT_USER, MYF(0), "RENAME USER", wrong_users.c_ptr_safe());

  if (some_users_renamed && mysql_bin_log.is_open())
    result |= write_bin_log(thd, FALSE, thd->query(), thd->query_length());

  mysql_rwlock_unlock(&LOCK_grant);
  DBUG_RETURN(result);
}


/*
  Revoke all privileges from a list of users.

  SYNOPSIS
    mysql_revoke_all()
    thd                         The current thread.
    list                        The users to revoke all privileges from.

  RETURN
    > 0         Error. Error message already sent.
    0           OK.
    < 0         Error. Error message not yet sent.
*/

bool mysql_revoke_all(THD *thd,  List <LEX_USER> &list)
{
  uint counter, revoked, is_proc;
  int result;
  ACL_DB *acl_db;
  TABLE_LIST tables[TABLES_MAX];
  DBUG_ENTER("mysql_revoke_all");

  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user | Table_db |
                       Table_tables_priv | Table_columns_priv |
                       Table_procs_priv | Table_proxies_priv |
                       Table_roles_mapping)))
    DBUG_RETURN(result != 1);

  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  LEX_USER *lex_user, *tmp_lex_user;
  List_iterator <LEX_USER> user_list(list);
  while ((tmp_lex_user= user_list++))
  {
    if (!(lex_user= get_current_user(thd, tmp_lex_user, false)))
    {
      result= -1;
      continue;
    }

    /* This is not a role and the user could not be found */
    if (!lex_user->is_role() &&
        !find_user_exact(lex_user->host.str, lex_user->user.str))
    {
      result= -1;
      continue;
    }

    if (replace_user_table(thd, tables[USER_TABLE].table, *lex_user,
                           ~(ulong)0, 1, 0, 0))
    {
      result= -1;
      continue;
    }

    /* Remove db access privileges */
    /*
      Because acl_dbs and column_priv_hash shrink and may re-order
      as privileges are removed, removal occurs in a repeated loop
      until no more privileges are revoked.
     */
    do
    {
      for (counter= 0, revoked= 0 ; counter < acl_dbs.elements ; )
      {
	const char *user,*host;

	acl_db=dynamic_element(&acl_dbs,counter,ACL_DB*);

        user= safe_str(acl_db->user);
        host= safe_str(acl_db->host.hostname);

	if (!strcmp(lex_user->user.str, user) &&
            !strcmp(lex_user->host.str, host))
	{
	  if (!replace_db_table(tables[DB_TABLE].table, acl_db->db, *lex_user,
                                ~(ulong)0, 1))
	  {
	    /*
	      Don't increment counter as replace_db_table deleted the
	      current element in acl_dbs.
	     */
	    revoked= 1;
	    continue;
	  }
	  result= -1; // Something went wrong
	}
	counter++;
      }
    } while (revoked);

    /* Remove column access */
    do
    {
      for (counter= 0, revoked= 0 ; counter < column_priv_hash.records ; )
      {
	const char *user,*host;
        GRANT_TABLE *grant_table=
          (GRANT_TABLE*) my_hash_element(&column_priv_hash, counter);
        user= safe_str(grant_table->user);
        host= safe_str(grant_table->host.hostname);

	if (!strcmp(lex_user->user.str,user) &&
            !strcmp(lex_user->host.str, host))
	{
	  if (replace_table_table(thd, grant_table,
                                  tables[TABLES_PRIV_TABLE].table,
                                  *lex_user, grant_table->db,
				  grant_table->tname, ~(ulong)0, 0, 1))
	  {
	    result= -1;
	  }
	  else
	  {
	    if (!grant_table->cols)
	    {
	      revoked= 1;
	      continue;
	    }
	    List<LEX_COLUMN> columns;
	    if (!replace_column_table(grant_table,
                                      tables[COLUMNS_PRIV_TABLE].table,
                                      *lex_user, columns, grant_table->db,
				      grant_table->tname, ~(ulong)0, 1))
	    {
	      revoked= 1;
	      continue;
	    }
	    result= -1;
	  }
	}
	counter++;
      }
    } while (revoked);

    /* Remove procedure access */
    for (is_proc=0; is_proc<2; is_proc++) do {
      HASH *hash= is_proc ? &proc_priv_hash : &func_priv_hash;
      for (counter= 0, revoked= 0 ; counter < hash->records ; )
      {
	const char *user,*host;
        GRANT_NAME *grant_proc= (GRANT_NAME*) my_hash_element(hash, counter);
        user= safe_str(grant_proc->user);
        host= safe_str(grant_proc->host.hostname);

	if (!strcmp(lex_user->user.str,user) &&
            !strcmp(lex_user->host.str, host))
	{
	  if (replace_routine_table(thd, grant_proc,
                                    tables[PROCS_PRIV_TABLE].table, *lex_user,
                                    grant_proc->db, grant_proc->tname,
                                    is_proc, ~(ulong)0, 1) == 0)
	  {
	    revoked= 1;
	    continue;
	  }
	  result= -1;	// Something went wrong
	}
	counter++;
      }
    } while (revoked);

    ACL_USER_BASE *user_or_role;
    /* remove role grants */
    if (lex_user->is_role())
    {
      /* this can not fail due to get_current_user already having searched for it */
      user_or_role= find_acl_role(lex_user->user.str);
    }
    else
    {
      user_or_role= find_user_exact(lex_user->host.str, lex_user->user.str);
    }
    /*
      Find every role grant pair matching the role_grants array and remove it,
      both from the acl_roles_mappings and the roles_mapping table
    */
    for (counter= 0; counter < user_or_role->role_grants.elements; counter++)
    {
      ACL_ROLE *role_grant= *dynamic_element(&user_or_role->role_grants,
                                             counter, ACL_ROLE**);
      ROLE_GRANT_PAIR *pair = find_role_grant_pair(&lex_user->user,
                                                   &lex_user->host,
                                                   &role_grant->user);
      if (replace_roles_mapping_table(tables[ROLES_MAPPING_TABLE].table,
                                      &lex_user->user, &lex_user->host,
                                      &role_grant->user, false, pair, true))
      {
        result= -1; //Something went wrong
      }
      update_role_mapping(&lex_user->user, &lex_user->host,
                          &role_grant->user, false, pair, true);
      /*
        Delete from the parent_grantee array of the roles granted,
        the entry pointing to this user_or_role
      */
      remove_ptr_from_dynarray(&role_grant->parent_grantee, user_or_role);
    }
    /* TODO
       How to handle an error in the replace_roles_mapping_table, in
       regards to the privileges held in memory
    */

    /* Finally, clear the role_grants array */
    if (counter == user_or_role->role_grants.elements)
    {
      reset_dynamic(&user_or_role->role_grants);
    }
    /*
      If we are revoking from a role, we need to update all the parent grantees
    */
    if (lex_user->is_role())
    {
      propagate_role_grants((ACL_ROLE *)user_or_role, PRIVS_TO_MERGE::ALL);
    }
  }

  mysql_mutex_unlock(&acl_cache->lock);

  if (result)
    my_message(ER_REVOKE_GRANTS, ER_THD(thd, ER_REVOKE_GRANTS), MYF(0));
  
  result= result |
    write_bin_log(thd, FALSE, thd->query(), thd->query_length());

  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_RETURN(result);
}




/**
  If the defining user for a routine does not exist, then the ACL lookup
  code should raise two errors which we should intercept.  We convert the more
  descriptive error into a warning, and consume the other.

  If any other errors are raised, then we set a flag that should indicate
  that there was some failure we should complain at a higher level.
*/
class Silence_routine_definer_errors : public Internal_error_handler
{
public:
  Silence_routine_definer_errors()
    : is_grave(FALSE)
  {}

  virtual ~Silence_routine_definer_errors()
  {}

  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_warning_level level,
                                const char* msg,
                                Sql_condition ** cond_hdl);

  bool has_errors() { return is_grave; }

private:
  bool is_grave;
};

bool
Silence_routine_definer_errors::handle_condition(
  THD *thd,
  uint sql_errno,
  const char*,
  Sql_condition::enum_warning_level level,
  const char* msg,
  Sql_condition ** cond_hdl)
{
  *cond_hdl= NULL;
  if (level == Sql_condition::WARN_LEVEL_ERROR)
  {
    switch (sql_errno)
    {
      case ER_NONEXISTING_PROC_GRANT:
        /* Convert the error into a warning. */
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                     sql_errno, msg);
        return TRUE;
      default:
        is_grave= TRUE;
    }
  }

  return FALSE;
}


/**
  Revoke privileges for all users on a stored procedure.  Use an error handler
  that converts errors about missing grants into warnings.

  @param
    thd                         The current thread.
  @param
    db				DB of the stored procedure
  @param
    name			Name of the stored procedure

  @retval
    0           OK.
  @retval
    < 0         Error. Error message not yet sent.
*/

bool sp_revoke_privileges(THD *thd, const char *sp_db, const char *sp_name,
                          bool is_proc)
{
  uint counter, revoked;
  int result;
  TABLE_LIST tables[TABLES_MAX];
  HASH *hash= is_proc ? &proc_priv_hash : &func_priv_hash;
  Silence_routine_definer_errors error_handler;
  DBUG_ENTER("sp_revoke_privileges");

  if ((result= open_grant_tables(thd, tables, TL_WRITE, Table_user | Table_db |
                       Table_tables_priv | Table_columns_priv |
                       Table_procs_priv | Table_proxies_priv |
                       Table_roles_mapping)))
    DBUG_RETURN(result != 1);

  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());

  /* Be sure to pop this before exiting this scope! */
  thd->push_internal_handler(&error_handler);

  mysql_rwlock_wrlock(&LOCK_grant);
  mysql_mutex_lock(&acl_cache->lock);

  /* Remove procedure access */
  do
  {
    for (counter= 0, revoked= 0 ; counter < hash->records ; )
    {
      GRANT_NAME *grant_proc= (GRANT_NAME*) my_hash_element(hash, counter);
      if (!my_strcasecmp(&my_charset_utf8_bin, grant_proc->db, sp_db) &&
	  !my_strcasecmp(system_charset_info, grant_proc->tname, sp_name))
      {
        LEX_USER lex_user;
	lex_user.user.str= grant_proc->user;
	lex_user.user.length= strlen(grant_proc->user);
        lex_user.host.str= safe_str(grant_proc->host.hostname);
        lex_user.host.length= strlen(lex_user.host.str);
	if (replace_routine_table(thd, grant_proc,
                                  tables[PROCS_PRIV_TABLE].table, lex_user,
				  grant_proc->db, grant_proc->tname,
                                  is_proc, ~(ulong)0, 1) == 0)
	{
	  revoked= 1;
	  continue;
	}
      }
      counter++;
    }
  } while (revoked);

  mysql_mutex_unlock(&acl_cache->lock);
  mysql_rwlock_unlock(&LOCK_grant);

  thd->pop_internal_handler();

  DBUG_RETURN(error_handler.has_errors());
}


/**
  Grant EXECUTE,ALTER privilege for a stored procedure

  @param thd The current thread.
  @param sp_db
  @param sp_name
  @param is_proc

  @return
    @retval FALSE Success
    @retval TRUE An error occurred. Error message not yet sent.
*/

bool sp_grant_privileges(THD *thd, const char *sp_db, const char *sp_name,
                         bool is_proc)
{
  Security_context *sctx= thd->security_ctx;
  LEX_USER *combo;
  TABLE_LIST tables[1];
  List<LEX_USER> user_list;
  bool result;
  ACL_USER *au;
  Dummy_error_handler error_handler;
  DBUG_ENTER("sp_grant_privileges");

  if (!(combo=(LEX_USER*) thd->alloc(sizeof(st_lex_user))))
    DBUG_RETURN(TRUE);

  combo->user.str= sctx->user;

  mysql_mutex_lock(&acl_cache->lock);

  if ((au= find_user_wild(combo->host.str=(char*)sctx->host_or_ip, combo->user.str)))
    goto found_acl;
  if ((au= find_user_wild(combo->host.str=(char*)sctx->host, combo->user.str)))
    goto found_acl;
  if ((au= find_user_wild(combo->host.str=(char*)sctx->ip, combo->user.str)))
    goto found_acl;
  if ((au= find_user_wild(combo->host.str=(char*)"%", combo->user.str)))
    goto found_acl;

  mysql_mutex_unlock(&acl_cache->lock);
  DBUG_RETURN(TRUE);

 found_acl:
  mysql_mutex_unlock(&acl_cache->lock);

  bzero((char*)tables, sizeof(TABLE_LIST));
  user_list.empty();

  tables->db= (char*)sp_db;
  tables->table_name= tables->alias= (char*)sp_name;

  thd->make_lex_string(&combo->user, combo->user.str, strlen(combo->user.str));
  thd->make_lex_string(&combo->host, combo->host.str, strlen(combo->host.str));

  combo->reset_auth();

  if(au)
  {
    combo->plugin= au->plugin;
    combo->auth= au->auth_string;
  }

  if (user_list.push_back(combo, thd->mem_root))
    DBUG_RETURN(TRUE);

  thd->lex->ssl_type= SSL_TYPE_NOT_SPECIFIED;
  thd->lex->ssl_cipher= thd->lex->x509_subject= thd->lex->x509_issuer= 0;
  bzero((char*) &thd->lex->mqh, sizeof(thd->lex->mqh));

  /*
    Only care about whether the operation failed or succeeded
    as all errors will be handled later.
  */
  thd->push_internal_handler(&error_handler);
  result= mysql_routine_grant(thd, tables, is_proc, user_list,
                              DEFAULT_CREATE_PROC_ACLS, FALSE, FALSE);
  thd->pop_internal_handler();
  DBUG_RETURN(result);
}


/**
  Validate if a user can proxy as another user

  @thd                     current thread
  @param user              the logged in user (proxy user)
  @param authenticated_as  the effective user a plugin is trying to
                           impersonate as (proxied user)
  @return                  proxy user definition
    @retval NULL           proxy user definition not found or not applicable
    @retval non-null       the proxy user data
*/

static ACL_PROXY_USER *
acl_find_proxy_user(const char *user, const char *host, const char *ip,
                    const char *authenticated_as, bool *proxy_used)
{
  uint i;
  /* if the proxied and proxy user are the same return OK */
  DBUG_ENTER("acl_find_proxy_user");
  DBUG_PRINT("info", ("user=%s host=%s ip=%s authenticated_as=%s",
                      user, host, ip, authenticated_as));

  if (!strcmp(authenticated_as, user))
  {
    DBUG_PRINT ("info", ("user is the same as authenticated_as"));
    DBUG_RETURN (NULL);
  }

  *proxy_used= TRUE;
  for (i=0; i < acl_proxy_users.elements; i++)
  {
    ACL_PROXY_USER *proxy= dynamic_element(&acl_proxy_users, i,
                                           ACL_PROXY_USER *);
    if (proxy->matches(host, user, ip, authenticated_as))
      DBUG_RETURN(proxy);
  }

  DBUG_RETURN(NULL);
}


bool
acl_check_proxy_grant_access(THD *thd, const char *host, const char *user,
                             bool with_grant)
{
  DBUG_ENTER("acl_check_proxy_grant_access");
  DBUG_PRINT("info", ("user=%s host=%s with_grant=%d", user, host,
                      (int) with_grant));
  if (!initialized)
  {
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--skip-grant-tables");
    DBUG_RETURN(1);
  }

  /* replication slave thread can do anything */
  if (thd->slave_thread)
  {
    DBUG_PRINT("info", ("replication slave"));
    DBUG_RETURN(FALSE);
  }

  /*
    one can grant proxy for self to others.
    Security context in THD contains two pairs of (user,host):
    1. (user,host) pair referring to inbound connection.
    2. (priv_user,priv_host) pair obtained from mysql.user table after doing
        authnetication of incoming connection.
    Privileges should be checked wrt (priv_user, priv_host) tuple, because
    (user,host) pair obtained from inbound connection may have different
    values than what is actually stored in mysql.user table and while granting
    or revoking proxy privilege, user is expected to provide entries mentioned
    in mysql.user table.
  */
  if (!strcmp(thd->security_ctx->priv_user, user) &&
      !my_strcasecmp(system_charset_info, host,
                     thd->security_ctx->priv_host))
  {
    DBUG_PRINT("info", ("strcmp (%s, %s) my_casestrcmp (%s, %s) equal",
                        thd->security_ctx->priv_user, user,
                        host, thd->security_ctx->priv_host));
    DBUG_RETURN(FALSE);
  }

  mysql_mutex_lock(&acl_cache->lock);

  /* check for matching WITH PROXY rights */
  for (uint i=0; i < acl_proxy_users.elements; i++)
  {
    ACL_PROXY_USER *proxy= dynamic_element(&acl_proxy_users, i,
                                           ACL_PROXY_USER *);
    if (proxy->matches(thd->security_ctx->host,
                       thd->security_ctx->user,
                       thd->security_ctx->ip,
                       user) &&
        proxy->get_with_grant())
    {
      DBUG_PRINT("info", ("found"));
      mysql_mutex_unlock(&acl_cache->lock);
      DBUG_RETURN(FALSE);
    }
  }

  mysql_mutex_unlock(&acl_cache->lock);
  my_error(ER_ACCESS_DENIED_NO_PASSWORD_ERROR, MYF(0),
           thd->security_ctx->user,
           thd->security_ctx->host_or_ip);
  DBUG_RETURN(TRUE);
}


static bool
show_proxy_grants(THD *thd, const char *username, const char *hostname,
                  char *buff, size_t buffsize)
{
  Protocol *protocol= thd->protocol;
  int error= 0;

  for (uint i=0; i < acl_proxy_users.elements; i++)
  {
    ACL_PROXY_USER *proxy= dynamic_element(&acl_proxy_users, i,
                                           ACL_PROXY_USER *);
    if (proxy->granted_on(hostname, username))
    {
      String global(buff, buffsize, system_charset_info);
      global.length(0);
      proxy->print_grant(&global);
      protocol->prepare_for_resend();
      protocol->store(global.ptr(), global.length(), global.charset());
      if (protocol->write())
      {
        error= -1;
        break;
      }
    }
  }
  return error;
}

static int enabled_roles_insert(ACL_USER_BASE *role, void *context_data)
{
  TABLE *table= (TABLE*) context_data;
  DBUG_ASSERT(role->flags & IS_ROLE);

  restore_record(table, s->default_values);
  table->field[0]->set_notnull();
  table->field[0]->store(role->user.str, role->user.length,
                         system_charset_info);
  if (schema_table_store_record(table->in_use, table))
    return -1;
  return 0;
}

struct APPLICABLE_ROLES_DATA
{
  TABLE *table;
  const LEX_STRING host;
  const LEX_STRING user_and_host;
  ACL_USER *user;
};

static int
applicable_roles_insert(ACL_USER_BASE *grantee, ACL_ROLE *role, void *ptr)
{
  APPLICABLE_ROLES_DATA *data= (APPLICABLE_ROLES_DATA *)ptr;
  CHARSET_INFO *cs= system_charset_info;
  TABLE *table= data->table;
  bool is_role= grantee != data->user;
  const LEX_STRING *user_and_host= is_role ? &grantee->user
                                           : &data->user_and_host;
  const LEX_STRING *host= is_role ? &empty_lex_str : &data->host;

  restore_record(table, s->default_values);
  table->field[0]->store(user_and_host->str, user_and_host->length, cs);
  table->field[1]->store(role->user.str, role->user.length, cs);

  ROLE_GRANT_PAIR *pair=
    find_role_grant_pair(&grantee->user, host, &role->user);
  DBUG_ASSERT(pair);

  if (pair->with_admin)
    table->field[2]->store(STRING_WITH_LEN("YES"), cs);
  else
    table->field[2]->store(STRING_WITH_LEN("NO"), cs);

  /* Default role is only valid when looking at a role granted to a user. */
  if (!is_role)
  {
    if (data->user->default_rolename.length &&
        !strcmp(data->user->default_rolename.str, role->user.str))
      table->field[3]->store(STRING_WITH_LEN("YES"), cs);
    else
      table->field[3]->store(STRING_WITH_LEN("NO"), cs);
    table->field[3]->set_notnull();
  }

  if (schema_table_store_record(table->in_use, table))
    return -1;
  return 0;
}

/**
  Hash iterate function to count the number of total column privileges granted.
*/
static my_bool count_column_grants(void *grant_table,
                                       void *current_count)
{
  HASH hash_columns = ((GRANT_TABLE *)grant_table)->hash_columns;
  *(ulong *)current_count+= hash_columns.records;
  return 0;
}

/**
  SHOW function that computes the number of column grants.

  This must be performed under the mutex in order to make sure the
  iteration does not fail.
*/
static int show_column_grants(THD *thd, SHOW_VAR *var, char *buff,
                              enum enum_var_type scope)
{
  var->type= SHOW_ULONG;
  var->value= buff;
  *(ulong *)buff= 0;
  if (initialized)
  {
    mysql_rwlock_rdlock(&LOCK_grant);
    mysql_mutex_lock(&acl_cache->lock);
    my_hash_iterate(&column_priv_hash, count_column_grants, buff);
    mysql_mutex_unlock(&acl_cache->lock);
    mysql_rwlock_unlock(&LOCK_grant);
  }
  return 0;
}


#else
bool check_grant(THD *, ulong, TABLE_LIST *, bool, uint, bool)
{
  return 0;
}
#endif /*NO_EMBEDDED_ACCESS_CHECKS */


SHOW_VAR acl_statistics[] = {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  {"column_grants",    (char*)show_column_grants,          SHOW_SIMPLE_FUNC},
  {"database_grants",  (char*)&acl_dbs.elements,           SHOW_UINT},
  {"function_grants",  (char*)&func_priv_hash.records,     SHOW_ULONG},
  {"procedure_grants", (char*)&proc_priv_hash.records,     SHOW_ULONG},
  {"proxy_users",      (char*)&acl_proxy_users.elements,   SHOW_UINT},
  {"role_grants",      (char*)&acl_roles_mappings.records, SHOW_ULONG},
  {"roles",            (char*)&acl_roles.records,          SHOW_ULONG},
  {"table_grants",     (char*)&column_priv_hash.records,   SHOW_ULONG},
  {"users",            (char*)&acl_users.elements,         SHOW_UINT},
#endif
  {NullS, NullS, SHOW_LONG},
};


int fill_schema_enabled_roles(THD *thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *table= tables->table;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (thd->security_ctx->priv_role[0])
  {
    mysql_rwlock_rdlock(&LOCK_grant);
    mysql_mutex_lock(&acl_cache->lock);
    ACL_ROLE *acl_role= find_acl_role(thd->security_ctx->priv_role);
    if (acl_role)
      traverse_role_graph_down(acl_role, table, enabled_roles_insert, NULL);
    mysql_mutex_unlock(&acl_cache->lock);
    mysql_rwlock_unlock(&LOCK_grant);
    if (acl_role)
      return 0;
  }
#endif

  restore_record(table, s->default_values);
  table->field[0]->set_null();
  return schema_table_store_record(table->in_use, table);
}


/*
  This shows all roles granted to current user
  and recursively all roles granted to those roles
*/
int fill_schema_applicable_roles(THD *thd, TABLE_LIST *tables, COND *cond)
{
  int res= 0;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (initialized)
  {
    TABLE *table= tables->table;
    Security_context *sctx= thd->security_ctx;
    mysql_rwlock_rdlock(&LOCK_grant);
    mysql_mutex_lock(&acl_cache->lock);
    ACL_USER *user= find_user_exact(sctx->priv_host, sctx->priv_user);
    if (user)
    {
      char buff[USER_HOST_BUFF_SIZE+10];
      DBUG_ASSERT(user->user.length + user->hostname_length +2 < sizeof(buff));
      char *end= strxmov(buff, user->user.str, "@", user->host.hostname, NULL);
      APPLICABLE_ROLES_DATA data= { table,
        { user->host.hostname, user->hostname_length },
        { buff, (size_t)(end - buff) }, user
      };

      res= traverse_role_graph_down(user, &data, 0, applicable_roles_insert);
    }

    mysql_mutex_unlock(&acl_cache->lock);
    mysql_rwlock_unlock(&LOCK_grant);
  }
#endif

  return res;
}


int wild_case_compare(CHARSET_INFO *cs, const char *str,const char *wildstr)
{
  reg3 int flag;
  DBUG_ENTER("wild_case_compare");
  DBUG_PRINT("enter",("str: '%s'  wildstr: '%s'",str,wildstr));
  while (*wildstr)
  {
    while (*wildstr && *wildstr != wild_many && *wildstr != wild_one)
    {
      if (*wildstr == wild_prefix && wildstr[1])
	wildstr++;
      if (my_toupper(cs, *wildstr++) !=
          my_toupper(cs, *str++)) DBUG_RETURN(1);
    }
    if (! *wildstr ) DBUG_RETURN (*str != 0);
    if (*wildstr++ == wild_one)
    {
      if (! *str++) DBUG_RETURN (1);	/* One char; skip */
    }
    else
    {						/* Found '*' */
      if (!*wildstr) DBUG_RETURN(0);		/* '*' as last char: OK */
      flag=(*wildstr != wild_many && *wildstr != wild_one);
      do
      {
	if (flag)
	{
	  char cmp;
	  if ((cmp= *wildstr) == wild_prefix && wildstr[1])
	    cmp=wildstr[1];
	  cmp=my_toupper(cs, cmp);
	  while (*str && my_toupper(cs, *str) != cmp)
	    str++;
	  if (!*str) DBUG_RETURN (1);
	}
	if (wild_case_compare(cs, str,wildstr) == 0) DBUG_RETURN (0);
      } while (*str++);
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN (*str != '\0');
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS
static bool update_schema_privilege(THD *thd, TABLE *table, char *buff,
                                    const char* db, const char* t_name,
                                    const char* column, uint col_length,
                                    const char *priv, uint priv_length,
                                    const char* is_grantable)
{
  int i= 2;
  CHARSET_INFO *cs= system_charset_info;
  restore_record(table, s->default_values);
  table->field[0]->store(buff, (uint) strlen(buff), cs);
  table->field[1]->store(STRING_WITH_LEN("def"), cs);
  if (db)
    table->field[i++]->store(db, (uint) strlen(db), cs);
  if (t_name)
    table->field[i++]->store(t_name, (uint) strlen(t_name), cs);
  if (column)
    table->field[i++]->store(column, col_length, cs);
  table->field[i++]->store(priv, priv_length, cs);
  table->field[i]->store(is_grantable, strlen(is_grantable), cs);
  return schema_table_store_record(thd, table);
}
#endif


int fill_schema_user_privileges(THD *thd, TABLE_LIST *tables, COND *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int error= 0;
  uint counter;
  ACL_USER *acl_user;
  ulong want_access;
  char buff[100];
  TABLE *table= tables->table;
  bool no_global_access= check_access(thd, SELECT_ACL, "mysql",
                                      NULL, NULL, 1, 1);
  char *curr_host= thd->security_ctx->priv_host_name();
  DBUG_ENTER("fill_schema_user_privileges");

  if (!initialized)
    DBUG_RETURN(0);
  mysql_mutex_lock(&acl_cache->lock);

  for (counter=0 ; counter < acl_users.elements ; counter++)
  {
    const char *user,*host, *is_grantable="YES";
    acl_user=dynamic_element(&acl_users,counter,ACL_USER*);
    user= safe_str(acl_user->user.str);
    host= safe_str(acl_user->host.hostname);

    if (no_global_access &&
        (strcmp(thd->security_ctx->priv_user, user) ||
         my_strcasecmp(system_charset_info, curr_host, host)))
      continue;

    want_access= acl_user->access;
    if (!(want_access & GRANT_ACL))
      is_grantable= "NO";

    strxmov(buff,"'",user,"'@'",host,"'",NullS);
    if (!(want_access & ~GRANT_ACL))
    {
      if (update_schema_privilege(thd, table, buff, 0, 0, 0, 0,
                                  STRING_WITH_LEN("USAGE"), is_grantable))
      {
        error= 1;
        goto err;
      }
    }
    else
    {
      uint priv_id;
      ulong j,test_access= want_access & ~GRANT_ACL;
      for (priv_id=0, j = SELECT_ACL;j <= GLOBAL_ACLS; priv_id++,j <<= 1)
      {
	if (test_access & j)
        {
          if (update_schema_privilege(thd, table, buff, 0, 0, 0, 0,
                                      command_array[priv_id],
                                      command_lengths[priv_id], is_grantable))
          {
            error= 1;
            goto err;
          }
        }
      }
    }
  }
err:
  mysql_mutex_unlock(&acl_cache->lock);

  DBUG_RETURN(error);
#else
  return(0);
#endif
}


int fill_schema_schema_privileges(THD *thd, TABLE_LIST *tables, COND *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int error= 0;
  uint counter;
  ACL_DB *acl_db;
  ulong want_access;
  char buff[100];
  TABLE *table= tables->table;
  bool no_global_access= check_access(thd, SELECT_ACL, "mysql",
                                      NULL, NULL, 1, 1);
  char *curr_host= thd->security_ctx->priv_host_name();
  DBUG_ENTER("fill_schema_schema_privileges");

  if (!initialized)
    DBUG_RETURN(0);
  mysql_mutex_lock(&acl_cache->lock);

  for (counter=0 ; counter < acl_dbs.elements ; counter++)
  {
    const char *user, *host, *is_grantable="YES";

    acl_db=dynamic_element(&acl_dbs,counter,ACL_DB*);
    user= safe_str(acl_db->user);
    host= safe_str(acl_db->host.hostname);

    if (no_global_access &&
        (strcmp(thd->security_ctx->priv_user, user) ||
         my_strcasecmp(system_charset_info, curr_host, host)))
      continue;

    want_access=acl_db->access;
    if (want_access)
    {
      if (!(want_access & GRANT_ACL))
      {
        is_grantable= "NO";
      }
      strxmov(buff,"'",user,"'@'",host,"'",NullS);
      if (!(want_access & ~GRANT_ACL))
      {
        if (update_schema_privilege(thd, table, buff, acl_db->db, 0, 0,
                                    0, STRING_WITH_LEN("USAGE"), is_grantable))
        {
          error= 1;
          goto err;
        }
      }
      else
      {
        int cnt;
        ulong j,test_access= want_access & ~GRANT_ACL;
        for (cnt=0, j = SELECT_ACL; j <= DB_ACLS; cnt++,j <<= 1)
          if (test_access & j)
          {
            if (update_schema_privilege(thd, table, buff, acl_db->db, 0, 0, 0,
                                        command_array[cnt], command_lengths[cnt],
                                        is_grantable))
            {
              error= 1;
              goto err;
            }
          }
      }
    }
  }
err:
  mysql_mutex_unlock(&acl_cache->lock);

  DBUG_RETURN(error);
#else
  return (0);
#endif
}


int fill_schema_table_privileges(THD *thd, TABLE_LIST *tables, COND *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int error= 0;
  uint index;
  char buff[100];
  TABLE *table= tables->table;
  bool no_global_access= check_access(thd, SELECT_ACL, "mysql",
                                      NULL, NULL, 1, 1);
  char *curr_host= thd->security_ctx->priv_host_name();
  DBUG_ENTER("fill_schema_table_privileges");

  mysql_rwlock_rdlock(&LOCK_grant);

  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user, *host, *is_grantable= "YES";
    GRANT_TABLE *grant_table= (GRANT_TABLE*) my_hash_element(&column_priv_hash,
                                                             index);
    user= safe_str(grant_table->user);
    host= safe_str(grant_table->host.hostname);

    if (no_global_access &&
        (strcmp(thd->security_ctx->priv_user, user) ||
         my_strcasecmp(system_charset_info, curr_host, host)))
      continue;

    ulong table_access= grant_table->privs;
    if (table_access)
    {
      ulong test_access= table_access & ~GRANT_ACL;
      /*
        We should skip 'usage' privilege on table if
        we have any privileges on column(s) of this table
      */
      if (!test_access && grant_table->cols)
        continue;
      if (!(table_access & GRANT_ACL))
        is_grantable= "NO";

      strxmov(buff, "'", user, "'@'", host, "'", NullS);
      if (!test_access)
      {
        if (update_schema_privilege(thd, table, buff, grant_table->db,
                                    grant_table->tname, 0, 0,
                                    STRING_WITH_LEN("USAGE"), is_grantable))
        {
          error= 1;
          goto err;
        }
      }
      else
      {
        ulong j;
        int cnt;
        for (cnt= 0, j= SELECT_ACL; j <= TABLE_ACLS; cnt++, j<<= 1)
        {
          if (test_access & j)
          {
            if (update_schema_privilege(thd, table, buff, grant_table->db,
                                        grant_table->tname, 0, 0,
                                        command_array[cnt],
                                        command_lengths[cnt], is_grantable))
            {
              error= 1;
              goto err;
            }
          }
        }
      }
    }
  }
err:
  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_RETURN(error);
#else
  return (0);
#endif
}


int fill_schema_column_privileges(THD *thd, TABLE_LIST *tables, COND *cond)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  int error= 0;
  uint index;
  char buff[100];
  TABLE *table= tables->table;
  bool no_global_access= check_access(thd, SELECT_ACL, "mysql",
                                      NULL, NULL, 1, 1);
  char *curr_host= thd->security_ctx->priv_host_name();
  DBUG_ENTER("fill_schema_table_privileges");

  mysql_rwlock_rdlock(&LOCK_grant);

  for (index=0 ; index < column_priv_hash.records ; index++)
  {
    const char *user, *host, *is_grantable= "YES";
    GRANT_TABLE *grant_table= (GRANT_TABLE*) my_hash_element(&column_priv_hash,
                                                          index);
    user= safe_str(grant_table->user);
    host= safe_str(grant_table->host.hostname);

    if (no_global_access &&
        (strcmp(thd->security_ctx->priv_user, user) ||
         my_strcasecmp(system_charset_info, curr_host, host)))
      continue;

    ulong table_access= grant_table->cols;
    if (table_access != 0)
    {
      if (!(grant_table->privs & GRANT_ACL))
        is_grantable= "NO";

      ulong test_access= table_access & ~GRANT_ACL;
      strxmov(buff, "'", user, "'@'", host, "'", NullS);
      if (!test_access)
        continue;
      else
      {
        ulong j;
        int cnt;
        for (cnt= 0, j= SELECT_ACL; j <= TABLE_ACLS; cnt++, j<<= 1)
        {
          if (test_access & j)
          {
            for (uint col_index=0 ;
                 col_index < grant_table->hash_columns.records ;
                 col_index++)
            {
              GRANT_COLUMN *grant_column = (GRANT_COLUMN*)
                my_hash_element(&grant_table->hash_columns,col_index);
              if ((grant_column->rights & j) && (table_access & j))
              {
                if (update_schema_privilege(thd, table, buff, grant_table->db,
                                            grant_table->tname,
                                            grant_column->column,
                                            grant_column->key_length,
                                            command_array[cnt],
                                            command_lengths[cnt], is_grantable))
                {
                  error= 1;
                  goto err;
                }
              }
            }
          }
        }
      }
    }
  }
err:
  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_RETURN(error);
#else
  return (0);
#endif
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS
/*
  fill effective privileges for table

  SYNOPSIS
    fill_effective_table_privileges()
    thd     thread handler
    grant   grants table descriptor
    db      db name
    table   table name
*/

void fill_effective_table_privileges(THD *thd, GRANT_INFO *grant,
                                     const char *db, const char *table)
{
  Security_context *sctx= thd->security_ctx;
  DBUG_ENTER("fill_effective_table_privileges");
  DBUG_PRINT("enter", ("Host: '%s', Ip: '%s', User: '%s', table: `%s`.`%s`",
                       sctx->priv_host, sctx->ip, sctx->priv_user, db, table));
  /* --skip-grants */
  if (!initialized)
  {
    DBUG_PRINT("info", ("skip grants"));
    grant->privilege= ~NO_ACCESS;             // everything is allowed
    DBUG_PRINT("info", ("privilege 0x%lx", grant->privilege));
    DBUG_VOID_RETURN;
  }

  /* global privileges */
  grant->privilege= sctx->master_access;

  if (!sctx->priv_user)
  {
    DBUG_PRINT("info", ("privilege 0x%lx", grant->privilege));
    DBUG_VOID_RETURN;                         // it is slave
  }

  if (!thd->db || strcmp(db, thd->db))
  {
    /* db privileges */
    grant->privilege|= acl_get(sctx->host, sctx->ip, sctx->priv_user, db, 0);
    /* db privileges for role */
    if (sctx->priv_role[0])
      grant->privilege|= acl_get("", "", sctx->priv_role, db, 0);
  }
  else
  {
    grant->privilege|= sctx->db_access;
  }

  /* table privileges */
  mysql_rwlock_rdlock(&LOCK_grant);
  if (grant->version != grant_version)
  {
    grant->grant_table_user=
      table_hash_search(sctx->host, sctx->ip, db,
                        sctx->priv_user,
                        table, 0);              /* purecov: inspected */
    grant->grant_table_role=
      sctx->priv_role[0] ? table_hash_search("", "", db,
                                             sctx->priv_role,
                                             table, TRUE) : NULL;
    grant->version= grant_version;              /* purecov: inspected */
  }
  if (grant->grant_table_user != 0)
  {
    grant->privilege|= grant->grant_table_user->privs;
  }
  if (grant->grant_table_role != 0)
  {
    grant->privilege|= grant->grant_table_role->privs;
  }
  mysql_rwlock_unlock(&LOCK_grant);

  DBUG_PRINT("info", ("privilege 0x%lx", grant->privilege));
  DBUG_VOID_RETURN;
}

#else /* NO_EMBEDDED_ACCESS_CHECKS */

/****************************************************************************
 Dummy wrappers when we don't have any access checks
****************************************************************************/

bool check_routine_level_acl(THD *thd, const char *db, const char *name,
                             bool is_proc)
{
  return FALSE;
}

#endif

/**
  Return information about user or current user.

  @param[in] thd          thread handler
  @param[in] user         user
  @param[in] lock         whether &acl_cache->lock mutex needs to be locked

  @return
    - On success, return a valid pointer to initialized
    LEX_USER, which contains user information.
    - On error, return 0.
*/

LEX_USER *get_current_user(THD *thd, LEX_USER *user, bool lock)
{
  if (user->user.str == current_user.str)  // current_user
    return create_default_definer(thd, false);

  if (user->user.str == current_role.str)  // current_role
    return create_default_definer(thd, true);

  if (user->host.str == NULL) // Possibly a role
  {
    // to be reexecution friendly we have to make a copy
    LEX_USER *dup= (LEX_USER*) thd->memdup(user, sizeof(*user));
    if (!dup)
      return 0;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
    if (has_auth(user, thd->lex))
    {
      dup->host= host_not_specified;
      return dup;
    }

    if (is_invalid_role_name(user->user.str))
      return 0;

    if (lock)
      mysql_mutex_lock(&acl_cache->lock);
    if (find_acl_role(dup->user.str))
      dup->host= empty_lex_str;
    else
      dup->host= host_not_specified;
    if (lock)
      mysql_mutex_unlock(&acl_cache->lock);
#endif

    return dup;
  }

  return user;
}

struct ACL_internal_schema_registry_entry
{
  const LEX_STRING *m_name;
  const ACL_internal_schema_access *m_access;
};

/**
  Internal schema registered.
  Currently, this is only:
  - performance_schema
  - information_schema,
  This can be reused later for:
  - mysql
*/
static ACL_internal_schema_registry_entry registry_array[2];
static uint m_registry_array_size= 0;

/**
  Add an internal schema to the registry.
  @param name the schema name
  @param access the schema ACL specific rules
*/
void ACL_internal_schema_registry::register_schema
  (const LEX_STRING *name, const ACL_internal_schema_access *access)
{
  DBUG_ASSERT(m_registry_array_size < array_elements(registry_array));

  /* Not thread safe, and does not need to be. */
  registry_array[m_registry_array_size].m_name= name;
  registry_array[m_registry_array_size].m_access= access;
  m_registry_array_size++;
}

/**
  Search per internal schema ACL by name.
  @param name a schema name
  @return per schema rules, or NULL
*/
const ACL_internal_schema_access *
ACL_internal_schema_registry::lookup(const char *name)
{
  DBUG_ASSERT(name != NULL);

  uint i;

  for (i= 0; i<m_registry_array_size; i++)
  {
    if (my_strcasecmp(system_charset_info, registry_array[i].m_name->str,
                      name) == 0)
      return registry_array[i].m_access;
  }
  return NULL;
}

/**
  Get a cached internal schema access.
  @param grant_internal_info the cache
  @param schema_name the name of the internal schema
*/
const ACL_internal_schema_access *
get_cached_schema_access(GRANT_INTERNAL_INFO *grant_internal_info,
                         const char *schema_name)
{
  if (grant_internal_info)
  {
    if (! grant_internal_info->m_schema_lookup_done)
    {
      grant_internal_info->m_schema_access=
        ACL_internal_schema_registry::lookup(schema_name);
      grant_internal_info->m_schema_lookup_done= TRUE;
    }
    return grant_internal_info->m_schema_access;
  }
  return ACL_internal_schema_registry::lookup(schema_name);
}

/**
  Get a cached internal table access.
  @param grant_internal_info the cache
  @param schema_name the name of the internal schema
  @param table_name the name of the internal table
*/
const ACL_internal_table_access *
get_cached_table_access(GRANT_INTERNAL_INFO *grant_internal_info,
                        const char *schema_name,
                        const char *table_name)
{
  DBUG_ASSERT(grant_internal_info);
  if (! grant_internal_info->m_table_lookup_done)
  {
    const ACL_internal_schema_access *schema_access;
    schema_access= get_cached_schema_access(grant_internal_info, schema_name);
    if (schema_access)
      grant_internal_info->m_table_access= schema_access->lookup(table_name);
    grant_internal_info->m_table_lookup_done= TRUE;
  }
  return grant_internal_info->m_table_access;
}


/****************************************************************************
   AUTHENTICATION CODE
   including initial connect handshake, invoking appropriate plugins,
   client-server plugin negotiation, COM_CHANGE_USER, and native
   MySQL authentication plugins.
****************************************************************************/

/* few defines to have less ifdef's in the code below */
#ifdef EMBEDDED_LIBRARY
#undef HAVE_OPENSSL
#ifdef NO_EMBEDDED_ACCESS_CHECKS
#define initialized 0
#define check_for_max_user_connections(X,Y)   0
#define get_or_create_user_conn(A,B,C,D) 0
#endif
#endif
#ifndef HAVE_OPENSSL
#define ssl_acceptor_fd 0
#define sslaccept(A,B,C,D) 1
#endif

/**
  The internal version of what plugins know as MYSQL_PLUGIN_VIO,
  basically the context of the authentication session
*/
struct MPVIO_EXT :public MYSQL_PLUGIN_VIO
{
  MYSQL_SERVER_AUTH_INFO auth_info;
  THD *thd;
  ACL_USER *acl_user;       ///< a copy, independent from acl_users array
  plugin_ref plugin;        ///< what plugin we're under
  LEX_STRING db;            ///< db name from the handshake packet
  /** when restarting a plugin this caches the last client reply */
  struct {
    char *plugin, *pkt;     ///< pointers into NET::buff
    uint pkt_len;
  } cached_client_reply;
  /** this caches the first plugin packet for restart request on the client */
  struct {
    char *pkt;
    uint pkt_len;
  } cached_server_packet;
  int packets_read, packets_written; ///< counters for send/received packets
  bool make_it_fail;
  /** when plugin returns a failure this tells us what really happened */
  enum { SUCCESS, FAILURE, RESTART } status;
};

/**
  a helper function to report an access denied error in all the proper places
*/
static void login_failed_error(THD *thd)
{
  my_error(access_denied_error_code(thd->password), MYF(0),
           thd->main_security_ctx.user,
           thd->main_security_ctx.host_or_ip,
           thd->password ? ER_THD(thd, ER_YES) : ER_THD(thd, ER_NO));
  general_log_print(thd, COM_CONNECT,
                    ER_THD(thd, access_denied_error_code(thd->password)),
                    thd->main_security_ctx.user,
                    thd->main_security_ctx.host_or_ip,
                    thd->password ? ER_THD(thd, ER_YES) : ER_THD(thd, ER_NO));
  status_var_increment(thd->status_var.access_denied_errors);
  /*
    Log access denied messages to the error log when log-warnings = 2
    so that the overhead of the general query log is not required to track
    failed connections.
  */
  if (global_system_variables.log_warnings > 1)
  {
    sql_print_warning(ER_THD(thd, access_denied_error_code(thd->password)),
                      thd->main_security_ctx.user,
                      thd->main_security_ctx.host_or_ip,
                      thd->password ? ER_THD(thd, ER_YES) : ER_THD(thd, ER_NO));
  }
}

/**
  sends a server handshake initialization packet, the very first packet
  after the connection was established

  Packet format:

    Bytes       Content
    -----       ----
    1           protocol version (always 10)
    n           server version string, \0-terminated
    4           thread id
    8           first 8 bytes of the plugin provided data (scramble)
    1           \0 byte, terminating the first part of a scramble
    2           server capabilities (two lower bytes)
    1           server character set
    2           server status
    2           server capabilities (two upper bytes)
    1           length of the scramble
    10          reserved, always 0
    n           rest of the plugin provided data (at least 12 bytes)
    1           \0 byte, terminating the second part of a scramble

  @retval 0 ok
  @retval 1 error
*/
static bool send_server_handshake_packet(MPVIO_EXT *mpvio,
                                         const char *data, uint data_len)
{
  DBUG_ASSERT(mpvio->status == MPVIO_EXT::FAILURE);
  DBUG_ASSERT(data_len <= 255);

  THD *thd= mpvio->thd;
  char *buff= (char *) my_alloca(1 + SERVER_VERSION_LENGTH + 1 + data_len + 64);
  char scramble_buf[SCRAMBLE_LENGTH];
  char *end= buff;
  DBUG_ENTER("send_server_handshake_packet");

  *end++= protocol_version;

  thd->client_capabilities= CLIENT_BASIC_FLAGS;

  if (opt_using_transactions)
    thd->client_capabilities|= CLIENT_TRANSACTIONS;

  thd->client_capabilities|= CAN_CLIENT_COMPRESS;

  if (ssl_acceptor_fd)
  {
    thd->client_capabilities |= CLIENT_SSL;
    thd->client_capabilities |= CLIENT_SSL_VERIFY_SERVER_CERT;
  }

  if (data_len)
  {
    mpvio->cached_server_packet.pkt= (char*)thd->memdup(data, data_len);
    mpvio->cached_server_packet.pkt_len= data_len;
  }

  if (data_len < SCRAMBLE_LENGTH)
  {
    if (data_len)
    {
      /*
        the first packet *must* have at least 20 bytes of a scramble.
        if a plugin provided less, we pad it to 20 with zeros
      */
      memcpy(scramble_buf, data, data_len);
      bzero(scramble_buf + data_len, SCRAMBLE_LENGTH - data_len);
      data= scramble_buf;
    }
    else
    {
      /*
        if the default plugin does not provide the data for the scramble at
        all, we generate a scramble internally anyway, just in case the
        user account (that will be known only later) uses a
        native_password_plugin (which needs a scramble). If we don't send a
        scramble now - wasting 20 bytes in the packet -
        native_password_plugin will have to send it in a separate packet,
        adding one more round trip.
      */
      create_random_string(thd->scramble, SCRAMBLE_LENGTH, &thd->rand);
      data= thd->scramble;
    }
    data_len= SCRAMBLE_LENGTH;
  }

  end= strxnmov(end, SERVER_VERSION_LENGTH, RPL_VERSION_HACK, server_version, NullS) + 1;
  int4store((uchar*) end, mpvio->thd->thread_id);
  end+= 4;

  /*
    Old clients does not understand long scrambles, but can ignore packet
    tail: that's why first part of the scramble is placed here, and second
    part at the end of packet.
  */
  end= (char*) memcpy(end, data, SCRAMBLE_LENGTH_323);
  end+= SCRAMBLE_LENGTH_323;
  *end++= 0;

  int2store(end, thd->client_capabilities);
  /* write server characteristics: up to 16 bytes allowed */
  end[2]= (char) default_charset_info->number;
  int2store(end+3, mpvio->thd->server_status);
  int2store(end+5, thd->client_capabilities >> 16);
  end[7]= data_len;
  DBUG_EXECUTE_IF("poison_srv_handshake_scramble_len", end[7]= -100;);
  bzero(end + 8, 10);
  end+= 18;
  /* write scramble tail */
  end= (char*) memcpy(end, data + SCRAMBLE_LENGTH_323,
                      data_len - SCRAMBLE_LENGTH_323);
  end+= data_len - SCRAMBLE_LENGTH_323;
  end= strmake(end, plugin_name(mpvio->plugin)->str,
                    plugin_name(mpvio->plugin)->length);

  int res= my_net_write(&mpvio->thd->net, (uchar*) buff,
                        (size_t) (end - buff + 1)) ||
           net_flush(&mpvio->thd->net);
  my_afree(buff);
  DBUG_RETURN (res);
}

static bool secure_auth(THD *thd)
{
  if (!opt_secure_auth)
    return 0;

  /*
    If the server is running in secure auth mode, short scrambles are
    forbidden. Extra juggling to report the same error as the old code.
  */
  if (thd->client_capabilities & CLIENT_PROTOCOL_41)
  {
    my_error(ER_SERVER_IS_IN_SECURE_AUTH_MODE, MYF(0),
             thd->security_ctx->user,
             thd->security_ctx->host_or_ip);
    general_log_print(thd, COM_CONNECT,
                      ER_THD(thd, ER_SERVER_IS_IN_SECURE_AUTH_MODE),
                      thd->security_ctx->user,
                      thd->security_ctx->host_or_ip);
  }
  else
  {
    my_error(ER_NOT_SUPPORTED_AUTH_MODE, MYF(0));
    general_log_print(thd, COM_CONNECT,
                      ER_THD(thd, ER_NOT_SUPPORTED_AUTH_MODE));
  }
  return 1;
}

/**
  sends a "change plugin" packet, requesting a client to restart authentication
  using a different authentication plugin

  Packet format:

    Bytes       Content
    -----       ----
    1           byte with the value 254
    n           client plugin to use, \0-terminated
    n           plugin provided data

  In a special case of switching from native_password_plugin to
  old_password_plugin, the packet contains only one - the first - byte,
  plugin name is omitted, plugin data aren't needed as the scramble was
  already sent. This one-byte packet is identical to the "use the short
  scramble" packet in the protocol before plugins were introduced.

  @retval 0 ok
  @retval 1 error
*/
static bool send_plugin_request_packet(MPVIO_EXT *mpvio,
                                       const uchar *data, uint data_len)
{
  DBUG_ASSERT(mpvio->packets_written == 1);
  DBUG_ASSERT(mpvio->packets_read == 1);
  NET *net= &mpvio->thd->net;
  static uchar switch_plugin_request_buf[]= { 254 };
  DBUG_ENTER("send_plugin_request_packet");

  mpvio->status= MPVIO_EXT::FAILURE; // the status is no longer RESTART

  const char *client_auth_plugin=
    ((st_mysql_auth *) (plugin_decl(mpvio->plugin)->info))->client_auth_plugin;

  DBUG_ASSERT(client_auth_plugin);

  /*
    we send an old "short 4.0 scramble request", if we need to request a
    client to use 4.0 auth plugin (short scramble) and the scramble was
    already sent to the client

    below, cached_client_reply.plugin is the plugin name that client has used,
    client_auth_plugin is derived from mysql.user table, for the given
    user account, it's the plugin that the client need to use to login.
  */
  bool switch_from_long_to_short_scramble=
    native_password_plugin_name.str == mpvio->cached_client_reply.plugin &&
    client_auth_plugin == old_password_plugin_name.str;

  if (switch_from_long_to_short_scramble)
    DBUG_RETURN (secure_auth(mpvio->thd) ||
                 my_net_write(net, switch_plugin_request_buf, 1) ||
                 net_flush(net));

  /*
    We never request a client to switch from a short to long scramble.
    Plugin-aware clients can do that, but traditionally it meant to
    ask an old 4.0 client to use the new 4.1 authentication protocol.
  */
  bool switch_from_short_to_long_scramble=
    old_password_plugin_name.str == mpvio->cached_client_reply.plugin &&
    client_auth_plugin == native_password_plugin_name.str;

  if (switch_from_short_to_long_scramble)
  {
    my_error(ER_NOT_SUPPORTED_AUTH_MODE, MYF(0));
    general_log_print(mpvio->thd, COM_CONNECT,
                      ER_THD(mpvio->thd, ER_NOT_SUPPORTED_AUTH_MODE));
    DBUG_RETURN (1);
  }

  DBUG_PRINT("info", ("requesting client to use the %s plugin",
                      client_auth_plugin));
  DBUG_RETURN(net_write_command(net, switch_plugin_request_buf[0],
                                (uchar*) client_auth_plugin,
                                strlen(client_auth_plugin) + 1,
                                (uchar*) data, data_len));
}

#ifndef NO_EMBEDDED_ACCESS_CHECKS
/**
   Finds acl entry in user database for authentication purposes.

   Finds a user and copies it into mpvio. Creates a fake user
   if no matching user account is found.

   @retval 0    found
   @retval 1    error
*/
static bool find_mpvio_user(MPVIO_EXT *mpvio)
{
  Security_context *sctx= mpvio->thd->security_ctx;
  DBUG_ENTER("find_mpvio_user");
  DBUG_ASSERT(mpvio->acl_user == 0);

  mysql_mutex_lock(&acl_cache->lock);

  ACL_USER *user= find_user_or_anon(sctx->host, sctx->user, sctx->ip);
  if (user)
    mpvio->acl_user= user->copy(mpvio->thd->mem_root);

  mysql_mutex_unlock(&acl_cache->lock);

  if (!mpvio->acl_user)
  {
    /*
      A matching user was not found. Fake it. Take any user, make the
      authentication fail later.
      This way we get a realistically looking failure, with occasional
      "change auth plugin" requests even for nonexistent users. The ratio
      of "change auth plugin" request will be the same for real and
      nonexistent users.
      Note, that we cannot pick any user at random, it must always be
      the same user account for the incoming sctx->user name.
    */
    ulong nr1=1, nr2=4;
    CHARSET_INFO *cs= &my_charset_latin1;
    cs->coll->hash_sort(cs, (uchar*) sctx->user, strlen(sctx->user), &nr1, &nr2);

    mysql_mutex_lock(&acl_cache->lock);
    if (!acl_users.elements)
    {
      mysql_mutex_unlock(&acl_cache->lock);
      login_failed_error(mpvio->thd);
      DBUG_RETURN(1);
    }
    uint i= nr1 % acl_users.elements;
    ACL_USER *acl_user_tmp= dynamic_element(&acl_users, i, ACL_USER*);
    mpvio->acl_user= acl_user_tmp->copy(mpvio->thd->mem_root);
    mysql_mutex_unlock(&acl_cache->lock);

    mpvio->make_it_fail= true;
  }

  /* user account requires non-default plugin and the client is too old */
  if (mpvio->acl_user->plugin.str != native_password_plugin_name.str &&
      mpvio->acl_user->plugin.str != old_password_plugin_name.str &&
      !(mpvio->thd->client_capabilities & CLIENT_PLUGIN_AUTH))
  {
    DBUG_ASSERT(my_strcasecmp(system_charset_info, mpvio->acl_user->plugin.str,
                              native_password_plugin_name.str));
    DBUG_ASSERT(my_strcasecmp(system_charset_info, mpvio->acl_user->plugin.str,
                              old_password_plugin_name.str));
    my_error(ER_NOT_SUPPORTED_AUTH_MODE, MYF(0));
    general_log_print(mpvio->thd, COM_CONNECT,
                      ER_THD(mpvio->thd, ER_NOT_SUPPORTED_AUTH_MODE));
    DBUG_RETURN (1);
  }

  mpvio->auth_info.user_name= sctx->user;
  mpvio->auth_info.user_name_length= strlen(sctx->user);
  mpvio->auth_info.auth_string= mpvio->acl_user->auth_string.str;
  mpvio->auth_info.auth_string_length= (unsigned long) mpvio->acl_user->auth_string.length;
  strmake_buf(mpvio->auth_info.authenticated_as, safe_str(mpvio->acl_user->user.str));

  DBUG_PRINT("info", ("exit: user=%s, auth_string=%s, authenticated as=%s"
                      "plugin=%s",
                      mpvio->auth_info.user_name,
                      mpvio->auth_info.auth_string,
                      mpvio->auth_info.authenticated_as,
                      mpvio->acl_user->plugin.str));
  DBUG_RETURN(0);
}

static bool
read_client_connect_attrs(char **ptr, char *end, CHARSET_INFO *from_cs)
{
  size_t length;
  char *ptr_save= *ptr;

  /* not enough bytes to hold the length */
  if (ptr_save >= end)
    return true;

  length= safe_net_field_length_ll((uchar **) ptr, end - ptr_save);

  /* cannot even read the length */
  if (*ptr == NULL)
    return true;

  /* length says there're more data than can fit into the packet */
  if (*ptr + length > end)
    return true;

  /* impose an artificial length limit of 64k */
  if (length > 65535)
    return true;

#ifdef HAVE_PSI_THREAD_INTERFACE
  if (PSI_THREAD_CALL(set_thread_connect_attrs)(*ptr, length, from_cs) &&
      current_thd->variables.log_warnings)
    sql_print_warning("Connection attributes of length %lu were truncated",
                      (unsigned long) length);
#endif
  return false;
}

#endif

/* the packet format is described in send_change_user_packet() */
static bool parse_com_change_user_packet(MPVIO_EXT *mpvio, uint packet_length)
{
  THD *thd= mpvio->thd;
  NET *net= &thd->net;
  Security_context *sctx= thd->security_ctx;

  char *user= (char*) net->read_pos;
  char *end= user + packet_length;
  /* Safe because there is always a trailing \0 at the end of the packet */
  char *passwd= strend(user) + 1;
  uint user_len= passwd - user - 1;
  char *db= passwd;
  char db_buff[SAFE_NAME_LEN + 1];            // buffer to store db in utf8
  char user_buff[USERNAME_LENGTH + 1];	      // buffer to store user in utf8
  uint dummy_errors;
  DBUG_ENTER ("parse_com_change_user_packet");

  if (passwd >= end)
  {
    my_message(ER_UNKNOWN_COM_ERROR, ER_THD(thd, ER_UNKNOWN_COM_ERROR),
               MYF(0));
    DBUG_RETURN (1);
  }

  /*
    Old clients send null-terminated string as password; new clients send
    the size (1 byte) + string (not null-terminated). Hence in case of empty
    password both send '\0'.

    This strlen() can't be easily deleted without changing protocol.

    Cast *passwd to an unsigned char, so that it doesn't extend the sign for
    *passwd > 127 and become 2**32-127+ after casting to uint.
  */
  uint passwd_len= (thd->client_capabilities & CLIENT_SECURE_CONNECTION ?
                    (uchar) (*passwd++) : strlen(passwd));

  db+= passwd_len + 1;
  /*
    Database name is always NUL-terminated, so in case of empty database
    the packet must contain at least the trailing '\0'.
  */
  if (db >= end)
  {
    my_message(ER_UNKNOWN_COM_ERROR, ER_THD(thd, ER_UNKNOWN_COM_ERROR),
               MYF(0));
    DBUG_RETURN (1);
  }

  uint db_len= strlen(db);

  char *next_field= db + db_len + 1;

  if (next_field + 1 < end)
  {
    if (thd_init_client_charset(thd, uint2korr(next_field)))
      DBUG_RETURN(1);
    thd->update_charset();
    next_field+= 2;
  }

  /* Convert database and user names to utf8 */
  db_len= copy_and_convert(db_buff, sizeof(db_buff) - 1, system_charset_info,
                           db, db_len, thd->charset(), &dummy_errors);

  user_len= copy_and_convert(user_buff, sizeof(user_buff) - 1,
                             system_charset_info, user, user_len,
                             thd->charset(), &dummy_errors);

  if (!(sctx->user= my_strndup(user_buff, user_len, MYF(MY_WME))))
    DBUG_RETURN(1);

  /* Clear variables that are allocated */
  thd->user_connect= 0;
  strmake_buf(sctx->priv_user, sctx->user);

  if (thd->make_lex_string(&mpvio->db, db_buff, db_len) == 0)
    DBUG_RETURN(1); /* The error is set by make_lex_string(). */

  /*
    Clear thd->db as it points to something, that will be freed when
    connection is closed. We don't want to accidentally free a wrong
    pointer if connect failed.
  */
  thd->reset_db(NULL, 0);

  if (!initialized)
  {
    // if mysqld's been started with --skip-grant-tables option
    mpvio->status= MPVIO_EXT::SUCCESS;
    DBUG_RETURN(0);
  }

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  thd->password= passwd_len > 0;
  if (find_mpvio_user(mpvio))
    DBUG_RETURN(1);

  char *client_plugin;
  if (thd->client_capabilities & CLIENT_PLUGIN_AUTH)
  {
    if (next_field >= end)
    {
      my_message(ER_UNKNOWN_COM_ERROR, ER_THD(thd, ER_UNKNOWN_COM_ERROR),
                 MYF(0));
      DBUG_RETURN(1);
    }
    client_plugin= fix_plugin_ptr(next_field);
    next_field+= strlen(next_field) + 1;
  }
  else
  {
    if (thd->client_capabilities & CLIENT_SECURE_CONNECTION)
      client_plugin= native_password_plugin_name.str;
    else
    {
      client_plugin=  old_password_plugin_name.str;
      /*
        For a passwordless accounts we use native_password_plugin.
        But when an old 4.0 client connects to it, we change it to
        old_password_plugin, otherwise MySQL will think that server
        and client plugins don't match.
      */
      if (mpvio->acl_user->auth_string.length == 0)
        mpvio->acl_user->plugin= old_password_plugin_name;
    }
  }

  if ((thd->client_capabilities & CLIENT_CONNECT_ATTRS) &&
      read_client_connect_attrs(&next_field, end,
                                thd->charset()))
  {
    my_message(ER_UNKNOWN_COM_ERROR, ER_THD(thd, ER_UNKNOWN_COM_ERROR),
               MYF(0));
    DBUG_RETURN(packet_error);
  }

  DBUG_PRINT("info", ("client_plugin=%s, restart", client_plugin));
  /*
    Remember the data part of the packet, to present it to plugin in
    read_packet()
  */
  mpvio->cached_client_reply.pkt= passwd;
  mpvio->cached_client_reply.pkt_len= passwd_len;
  mpvio->cached_client_reply.plugin= client_plugin;
  mpvio->status= MPVIO_EXT::RESTART;
#endif

  DBUG_RETURN (0);
}


/* the packet format is described in send_client_reply_packet() */
static ulong parse_client_handshake_packet(MPVIO_EXT *mpvio,
                                           uchar **buff, ulong pkt_len)
{
#ifndef EMBEDDED_LIBRARY
  THD *thd= mpvio->thd;
  NET *net= &thd->net;
  char *end;
  DBUG_ASSERT(mpvio->status == MPVIO_EXT::FAILURE);

  if (pkt_len < MIN_HANDSHAKE_SIZE)
    return packet_error;

  /*
    Protocol buffer is guaranteed to always end with \0. (see my_net_read())
    As the code below depends on this, lets check that.
  */
  DBUG_ASSERT(net->read_pos[pkt_len] == 0);

  ulong client_capabilities= uint2korr(net->read_pos);
  if (client_capabilities & CLIENT_PROTOCOL_41)
  {
    if (pkt_len < 4)
      return packet_error;
    client_capabilities|= ((ulong) uint2korr(net->read_pos+2)) << 16;
  }

  /* Disable those bits which are not supported by the client. */
  thd->client_capabilities&= client_capabilities;

  DBUG_PRINT("info", ("client capabilities: %lu", thd->client_capabilities));
  if (thd->client_capabilities & CLIENT_SSL)
  {
    unsigned long errptr __attribute__((unused));

    /* Do the SSL layering. */
    if (!ssl_acceptor_fd)
      return packet_error;

    DBUG_PRINT("info", ("IO layer change in progress..."));
    if (sslaccept(ssl_acceptor_fd, net->vio, net->read_timeout, &errptr))
    {
      DBUG_PRINT("error", ("Failed to accept new SSL connection"));
      return packet_error;
    }

    DBUG_PRINT("info", ("Reading user information over SSL layer"));
    pkt_len= my_net_read(net);
    if (pkt_len == packet_error || pkt_len < NORMAL_HANDSHAKE_SIZE)
    {
      DBUG_PRINT("error", ("Failed to read user information (pkt_len= %lu)",
			   pkt_len));
      return packet_error;
    }
  }

  if (client_capabilities & CLIENT_PROTOCOL_41)
  {
    if (pkt_len < 32)
      return packet_error;
    thd->max_client_packet_length= uint4korr(net->read_pos+4);
    DBUG_PRINT("info", ("client_character_set: %d", (uint) net->read_pos[8]));
    if (thd_init_client_charset(thd, (uint) net->read_pos[8]))
      return packet_error;
    thd->update_charset();
    end= (char*) net->read_pos+32;
  }
  else
  {
    if (pkt_len < 5)
      return packet_error;
    thd->max_client_packet_length= uint3korr(net->read_pos+2);
    end= (char*) net->read_pos+5;
  }

  if (end >= (char*) net->read_pos+ pkt_len +2)
    return packet_error;

  if (thd->client_capabilities & CLIENT_IGNORE_SPACE)
    thd->variables.sql_mode|= MODE_IGNORE_SPACE;
  if (thd->client_capabilities & CLIENT_INTERACTIVE)
    thd->variables.net_wait_timeout= thd->variables.net_interactive_timeout;

  if (end >= (char*) net->read_pos+ pkt_len +2)
    return packet_error;

  if ((thd->client_capabilities & CLIENT_TRANSACTIONS) &&
      opt_using_transactions)
    net->return_status= &thd->server_status;

  char *user= end;
  char *passwd= strend(user)+1;
  uint user_len= passwd - user - 1, db_len;
  char *db= passwd;
  char user_buff[USERNAME_LENGTH + 1];	// buffer to store user in utf8
  uint dummy_errors;

  /*
    Old clients send null-terminated string as password; new clients send
    the size (1 byte) + string (not null-terminated). Hence in case of empty
    password both send '\0'.

    This strlen() can't be easily deleted without changing protocol.

    Cast *passwd to an unsigned char, so that it doesn't extend the sign for
    *passwd > 127 and become 2**32-127+ after casting to uint.
  */
  uint passwd_len;
  if (!(thd->client_capabilities & CLIENT_SECURE_CONNECTION))
    passwd_len= strlen(passwd);
  else if (!(thd->client_capabilities & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA))
    passwd_len= (uchar)(*passwd++);
  else
    passwd_len= safe_net_field_length_ll((uchar**)&passwd,
                                      net->read_pos + pkt_len - (uchar*)passwd);
  
  db= thd->client_capabilities & CLIENT_CONNECT_WITH_DB ?
    db + passwd_len + 1 : 0;

  if (passwd == NULL ||
      passwd + passwd_len + MY_TEST(db) > (char*) net->read_pos + pkt_len)
    return packet_error;

  /* strlen() can't be easily deleted without changing protocol */
  db_len= safe_strlen(db);

  char *next_field;
  char *client_plugin= next_field= passwd + passwd_len + (db ? db_len + 1 : 0);

  /* Since 4.1 all database names are stored in utf8 */
  if (thd->copy_with_error(system_charset_info, &mpvio->db,
                           thd->charset(), db, db_len))
    return packet_error;

  user_len= copy_and_convert(user_buff, sizeof(user_buff) - 1,
                             system_charset_info, user, user_len,
                             thd->charset(), &dummy_errors);
  user= user_buff;

  /* If username starts and ends in "'", chop them off */
  if (user_len > 1 && user[0] == '\'' && user[user_len - 1] == '\'')
  {
    user++;
    user_len-= 2;
  }

  /*
    Clip username to allowed length in characters (not bytes).  This is
    mostly for backward compatibility (to truncate long usernames, as
    old 5.1 did)
  */
  {
    CHARSET_INFO *cs= system_charset_info;
    int           err;

    user_len= (uint) cs->cset->well_formed_len(cs, user, user + user_len,
                                               username_char_length, &err);
    user[user_len]= '\0';
  }

  Security_context *sctx= thd->security_ctx;

  my_free(sctx->user);
  if (!(sctx->user= my_strndup(user, user_len, MYF(MY_WME))))
    return packet_error; /* The error is set by my_strdup(). */


  /*
    Clear thd->db as it points to something, that will be freed when
    connection is closed. We don't want to accidentally free a wrong
    pointer if connect failed.
  */
  thd->reset_db(NULL, 0);

  if (!initialized)
  {
    // if mysqld's been started with --skip-grant-tables option
    mpvio->status= MPVIO_EXT::SUCCESS;
    return packet_error;
  }

  thd->password= passwd_len > 0;
  if (find_mpvio_user(mpvio))
    return packet_error;

  if ((thd->client_capabilities & CLIENT_PLUGIN_AUTH) &&
      (client_plugin < (char *)net->read_pos + pkt_len))
  {
    client_plugin= fix_plugin_ptr(client_plugin);
    next_field+= strlen(next_field) + 1;
  }
  else
  {
    /* Some clients lie. Sad, but true */
    thd->client_capabilities &= ~CLIENT_PLUGIN_AUTH;

    if (thd->client_capabilities & CLIENT_SECURE_CONNECTION)
      client_plugin= native_password_plugin_name.str;
    else
    {
      client_plugin=  old_password_plugin_name.str;
      /*
        For a passwordless accounts we use native_password_plugin.
        But when an old 4.0 client connects to it, we change it to
        old_password_plugin, otherwise MySQL will think that server
        and client plugins don't match.
      */
      if (mpvio->acl_user->auth_string.length == 0)
        mpvio->acl_user->plugin= old_password_plugin_name;
    }
  }

  if ((thd->client_capabilities & CLIENT_CONNECT_ATTRS) &&
      read_client_connect_attrs(&next_field, ((char *)net->read_pos) + pkt_len,
                                mpvio->thd->charset()))
    return packet_error;

  /*
    if the acl_user needs a different plugin to authenticate
    (specified in GRANT ... AUTHENTICATED VIA plugin_name ..)
    we need to restart the authentication in the server.
    But perhaps the client has already used the correct plugin -
    in that case the authentication on the client may not need to be
    restarted and a server auth plugin will read the data that the client
    has just send. Cache them to return in the next server_mpvio_read_packet().
  */
  if (my_strcasecmp(system_charset_info, mpvio->acl_user->plugin.str,
                    plugin_name(mpvio->plugin)->str) != 0)
  {
    mpvio->cached_client_reply.pkt= passwd;
    mpvio->cached_client_reply.pkt_len= passwd_len;
    mpvio->cached_client_reply.plugin= client_plugin;
    mpvio->status= MPVIO_EXT::RESTART;
    return packet_error;
  }

  /*
    ok, we don't need to restart the authentication on the server.
    but if the client used the wrong plugin, we need to restart
    the authentication on the client. Do it here, the server plugin
    doesn't need to know.
  */
  const char *client_auth_plugin=
    ((st_mysql_auth *) (plugin_decl(mpvio->plugin)->info))->client_auth_plugin;

  if (client_auth_plugin &&
      my_strcasecmp(system_charset_info, client_plugin, client_auth_plugin))
  {
    mpvio->cached_client_reply.plugin= client_plugin;
    if (send_plugin_request_packet(mpvio,
                                   (uchar*) mpvio->cached_server_packet.pkt,
                                   mpvio->cached_server_packet.pkt_len))
      return packet_error;

    passwd_len= my_net_read(&thd->net);
    passwd= (char*)thd->net.read_pos;
  }

  *buff= (uchar*) passwd;
  return passwd_len;
#else
  return 0;
#endif
}


/**
  vio->write_packet() callback method for server authentication plugins

  This function is called by a server authentication plugin, when it wants
  to send data to the client.

  It transparently wraps the data into a handshake packet,
  and handles plugin negotiation with the client. If necessary,
  it escapes the plugin data, if it starts with a mysql protocol packet byte.
*/
static int server_mpvio_write_packet(MYSQL_PLUGIN_VIO *param,
                                   const uchar *packet, int packet_len)
{
  MPVIO_EXT *mpvio= (MPVIO_EXT *) param;
  int res;
  DBUG_ENTER("server_mpvio_write_packet");

  /* reset cached_client_reply */
  mpvio->cached_client_reply.pkt= 0;

  /* for the 1st packet we wrap plugin data into the handshake packet */
  if (mpvio->packets_written == 0)
    res= send_server_handshake_packet(mpvio, (char*) packet, packet_len);
  else if (mpvio->status == MPVIO_EXT::RESTART)
    res= send_plugin_request_packet(mpvio, packet, packet_len);
  else if (packet_len > 0 && (*packet == 1 || *packet == 255 || *packet == 254))
  {
    /*
      we cannot allow plugin data packet to start from 255 or 254 -
      as the client will treat it as an error or "change plugin" packet.
      We'll escape these bytes with \1. Consequently, we
      have to escape \1 byte too.
    */
    res= net_write_command(&mpvio->thd->net, 1, (uchar*)"", 0,
                           packet, packet_len);
  }
  else
  {
    res= my_net_write(&mpvio->thd->net, packet, packet_len) ||
         net_flush(&mpvio->thd->net);
  }
  mpvio->packets_written++;
  DBUG_RETURN(res);
}

/**
  vio->read_packet() callback method for server authentication plugins

  This function is called by a server authentication plugin, when it wants
  to read data from the client.

  It transparently extracts the client plugin data, if embedded into
  a client authentication handshake packet, and handles plugin negotiation
  with the client, if necessary.
*/
static int server_mpvio_read_packet(MYSQL_PLUGIN_VIO *param, uchar **buf)
{
  MPVIO_EXT *mpvio= (MPVIO_EXT *) param;
  ulong pkt_len;
  DBUG_ENTER("server_mpvio_read_packet");
  if (mpvio->packets_written == 0)
  {
    /*
      plugin wants to read the data without sending anything first.
      send an empty packet to force a server handshake packet to be sent
    */
    if (server_mpvio_write_packet(mpvio, 0, 0))
      pkt_len= packet_error;
    else
      pkt_len= my_net_read(&mpvio->thd->net);
  }
  else if (mpvio->cached_client_reply.pkt)
  {
    DBUG_ASSERT(mpvio->status == MPVIO_EXT::RESTART);
    DBUG_ASSERT(mpvio->packets_read > 0);
    /*
      if the have the data cached from the last server_mpvio_read_packet
      (which can be the case if it's a restarted authentication)
      and a client has used the correct plugin, then we can return the
      cached data straight away and avoid one round trip.
    */
    const char *client_auth_plugin=
      ((st_mysql_auth *) (plugin_decl(mpvio->plugin)->info))->client_auth_plugin;
    if (client_auth_plugin == 0 ||
        my_strcasecmp(system_charset_info, mpvio->cached_client_reply.plugin,
                      client_auth_plugin) == 0)
    {
      mpvio->status= MPVIO_EXT::FAILURE;
      *buf= (uchar*) mpvio->cached_client_reply.pkt;
      mpvio->cached_client_reply.pkt= 0;
      mpvio->packets_read++;

      DBUG_RETURN ((int) mpvio->cached_client_reply.pkt_len);
    }

    /*
      But if the client has used the wrong plugin, the cached data are
      useless. Furthermore, we have to send a "change plugin" request
      to the client.
    */
    if (server_mpvio_write_packet(mpvio, 0, 0))
      pkt_len= packet_error;
    else
      pkt_len= my_net_read(&mpvio->thd->net);
  }
  else
    pkt_len= my_net_read(&mpvio->thd->net);

  if (pkt_len == packet_error)
    goto err;

  mpvio->packets_read++;

  /*
    the 1st packet has the plugin data wrapped into the client authentication
    handshake packet
  */
  if (mpvio->packets_read == 1)
  {
    pkt_len= parse_client_handshake_packet(mpvio, buf, pkt_len);
    if (pkt_len == packet_error)
      goto err;
  }
  else
    *buf= mpvio->thd->net.read_pos;

  DBUG_RETURN((int)pkt_len);

err:
  if (mpvio->status == MPVIO_EXT::FAILURE)
  {
    if (!mpvio->thd->is_error())
      my_error(ER_HANDSHAKE_ERROR, MYF(0));
  }
  DBUG_RETURN(-1);
}

/**
  fills MYSQL_PLUGIN_VIO_INFO structure with the information about the
  connection
*/
static void server_mpvio_info(MYSQL_PLUGIN_VIO *vio,
                              MYSQL_PLUGIN_VIO_INFO *info)
{
  MPVIO_EXT *mpvio= (MPVIO_EXT *) vio;
  mpvio_info(mpvio->thd->net.vio, info);
}

static bool acl_check_ssl(THD *thd, const ACL_USER *acl_user)
{
#ifdef HAVE_OPENSSL
  Vio *vio= thd->net.vio;
  SSL *ssl= (SSL *) vio->ssl_arg;
  X509 *cert;
#endif

  /*
    At this point we know that user is allowed to connect
    from given host by given username/password pair. Now
    we check if SSL is required, if user is using SSL and
    if X509 certificate attributes are OK
  */
  switch (acl_user->ssl_type) {
  case SSL_TYPE_NOT_SPECIFIED:                  // Impossible
  case SSL_TYPE_NONE:                           // SSL is not required
    return 0;
#ifdef HAVE_OPENSSL
  case SSL_TYPE_ANY:                            // Any kind of SSL is ok
    return vio_type(vio) != VIO_TYPE_SSL;
  case SSL_TYPE_X509: /* Client should have any valid certificate. */
    /*
      Connections with non-valid certificates are dropped already
      in sslaccept() anyway, so we do not check validity here.

      We need to check for absence of SSL because without SSL
      we should reject connection.
    */
    if (vio_type(vio) == VIO_TYPE_SSL &&
        SSL_get_verify_result(ssl) == X509_V_OK &&
        (cert= SSL_get_peer_certificate(ssl)))
    {
      X509_free(cert);
      return 0;
    }
    return 1;
  case SSL_TYPE_SPECIFIED: /* Client should have specified attrib */
    /* If a cipher name is specified, we compare it to actual cipher in use. */
    if (vio_type(vio) != VIO_TYPE_SSL ||
        SSL_get_verify_result(ssl) != X509_V_OK)
      return 1;
    if (acl_user->ssl_cipher)
    {
      DBUG_PRINT("info", ("comparing ciphers: '%s' and '%s'",
                         acl_user->ssl_cipher, SSL_get_cipher(ssl)));
      if (strcmp(acl_user->ssl_cipher, SSL_get_cipher(ssl)))
      {
        if (global_system_variables.log_warnings)
          sql_print_information("X509 ciphers mismatch: should be '%s' but is '%s'",
                            acl_user->ssl_cipher, SSL_get_cipher(ssl));
        return 1;
      }
    }
    /* Prepare certificate (if exists) */
    if (!(cert= SSL_get_peer_certificate(ssl)))
      return 1;
    /* If X509 issuer is specified, we check it... */
    if (acl_user->x509_issuer)
    {
      char *ptr= X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
      DBUG_PRINT("info", ("comparing issuers: '%s' and '%s'",
                         acl_user->x509_issuer, ptr));
      if (strcmp(acl_user->x509_issuer, ptr))
      {
        if (global_system_variables.log_warnings)
          sql_print_information("X509 issuer mismatch: should be '%s' "
                            "but is '%s'", acl_user->x509_issuer, ptr);
        free(ptr);
        X509_free(cert);
        return 1;
      }
      free(ptr);
    }
    /* X509 subject is specified, we check it .. */
    if (acl_user->x509_subject)
    {
      char *ptr= X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
      DBUG_PRINT("info", ("comparing subjects: '%s' and '%s'",
                         acl_user->x509_subject, ptr));
      if (strcmp(acl_user->x509_subject, ptr))
      {
        if (global_system_variables.log_warnings)
          sql_print_information("X509 subject mismatch: should be '%s' but is '%s'",
                          acl_user->x509_subject, ptr);
        free(ptr);
        X509_free(cert);
        return 1;
      }
      free(ptr);
    }
    X509_free(cert);
    return 0;
#else  /* HAVE_OPENSSL */
  default:
    /*
      If we don't have SSL but SSL is required for this user the
      authentication should fail.
    */
    return 1;
#endif /* HAVE_OPENSSL */
  }
  return 1;
}


static int do_auth_once(THD *thd, const LEX_STRING *auth_plugin_name,
                        MPVIO_EXT *mpvio)
{
  int res= CR_OK, old_status= MPVIO_EXT::FAILURE;
  bool unlock_plugin= false;
  plugin_ref plugin= NULL;

  if (auth_plugin_name->str == native_password_plugin_name.str)
    plugin= native_password_plugin;
#ifndef EMBEDDED_LIBRARY
  else if (auth_plugin_name->str == old_password_plugin_name.str)
    plugin= old_password_plugin;
  else if ((plugin= my_plugin_lock_by_name(thd, auth_plugin_name,
                                           MYSQL_AUTHENTICATION_PLUGIN)))
    unlock_plugin= true;
#endif

  mpvio->plugin= plugin;
  old_status= mpvio->status;

  if (plugin)
  {
    st_mysql_auth *auth= (st_mysql_auth *) plugin_decl(plugin)->info;
    switch (auth->interface_version) {
    case 0x0200:
      res= auth->authenticate_user(mpvio, &mpvio->auth_info);
      break;
    case 0x0100:
      {
        MYSQL_SERVER_AUTH_INFO_0x0100 compat;
        compat.downgrade(&mpvio->auth_info);
        res= auth->authenticate_user(mpvio, (MYSQL_SERVER_AUTH_INFO *)&compat);
        compat.upgrade(&mpvio->auth_info);
      }
      break;
    default: DBUG_ASSERT(0);
    }

    if (unlock_plugin)
      plugin_unlock(thd, plugin);
  }
  else
  {
    /* Server cannot load the required plugin. */
    Host_errors errors;
    errors.m_no_auth_plugin= 1;
    inc_host_errors(mpvio->thd->security_ctx->ip, &errors);
    my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), auth_plugin_name->str);
    res= CR_ERROR;
  }

  /*
    If the status was MPVIO_EXT::RESTART before the authenticate_user() call
    it can never be MPVIO_EXT::RESTART after the call, because any call
    to write_packet() or read_packet() will reset the status.

    But (!) if a plugin never called a read_packet() or write_packet(), the
    status will stay unchanged. We'll fix it, by resetting the status here.
  */
  if (old_status == MPVIO_EXT::RESTART && mpvio->status == MPVIO_EXT::RESTART)
    mpvio->status= MPVIO_EXT::FAILURE; // reset to the default

  return res;
}


/**
  Perform the handshake, authorize the client and update thd sctx variables.

  @param thd                     thread handle
  @param com_change_user_pkt_len size of the COM_CHANGE_USER packet
                                 (without the first, command, byte) or 0
                                 if it's not a COM_CHANGE_USER (that is, if
                                 it's a new connection)

  @retval 0  success, thd is updated.
  @retval 1  error
*/
bool acl_authenticate(THD *thd, uint com_change_user_pkt_len)
{
  int res= CR_OK;
  MPVIO_EXT mpvio;
  const LEX_STRING *auth_plugin_name= default_auth_plugin_name;
  enum  enum_server_command command= com_change_user_pkt_len ? COM_CHANGE_USER
                                                             : COM_CONNECT;
  DBUG_ENTER("acl_authenticate");

  bzero(&mpvio, sizeof(mpvio));
  mpvio.read_packet= server_mpvio_read_packet;
  mpvio.write_packet= server_mpvio_write_packet;
  mpvio.info= server_mpvio_info;
  mpvio.thd= thd;
  mpvio.status= MPVIO_EXT::FAILURE;
  mpvio.make_it_fail= false;
  mpvio.auth_info.host_or_ip= thd->security_ctx->host_or_ip;
  mpvio.auth_info.host_or_ip_length=
    (unsigned int) strlen(thd->security_ctx->host_or_ip);

  DBUG_PRINT("info", ("com_change_user_pkt_len=%u", com_change_user_pkt_len));

  if (command == COM_CHANGE_USER)
  {
    mpvio.packets_written++; // pretend that a server handshake packet was sent
    mpvio.packets_read++;    // take COM_CHANGE_USER packet into account

    if (parse_com_change_user_packet(&mpvio, com_change_user_pkt_len))
      DBUG_RETURN(1);

    DBUG_ASSERT(mpvio.status == MPVIO_EXT::RESTART ||
                mpvio.status == MPVIO_EXT::SUCCESS);
  }
  else
  {
    /* mark the thd as having no scramble yet */
    thd->scramble[SCRAMBLE_LENGTH]= 1;

    /*
      perform the first authentication attempt, with the default plugin.
      This sends the server handshake packet, reads the client reply
      with a user name, and performs the authentication if everyone has used
      the correct plugin.
    */

    res= do_auth_once(thd, auth_plugin_name, &mpvio);
  }

  /*
    retry the authentication, if - after receiving the user name -
    we found that we need to switch to a non-default plugin
  */
  if (mpvio.status == MPVIO_EXT::RESTART)
  {
    DBUG_ASSERT(mpvio.acl_user);
    DBUG_ASSERT(command == COM_CHANGE_USER ||
                my_strcasecmp(system_charset_info, auth_plugin_name->str,
                              mpvio.acl_user->plugin.str));
    auth_plugin_name= &mpvio.acl_user->plugin;
    res= do_auth_once(thd, auth_plugin_name, &mpvio);
  }
  if (mpvio.make_it_fail && res == CR_OK)
  {
    mpvio.status= MPVIO_EXT::FAILURE;
    res= CR_ERROR;
  }
 
  Security_context *sctx= thd->security_ctx;
  const ACL_USER *acl_user= mpvio.acl_user;

  thd->password= mpvio.auth_info.password_used;  // remember for error messages

  /*
    Log the command here so that the user can check the log
    for the tried logins and also to detect break-in attempts.

    if sctx->user is unset it's protocol failure, bad packet.
  */
  if (sctx->user)
  {
    if (strcmp(sctx->priv_user, sctx->user))
    {
      general_log_print(thd, command, "%s@%s as %s on %s",
                        sctx->user, sctx->host_or_ip,
                        sctx->priv_user[0] ? sctx->priv_user : "anonymous",
                        safe_str(mpvio.db.str));
    }
    else
      general_log_print(thd, command, (char*) "%s@%s on %s",
                        sctx->user, sctx->host_or_ip,
                        safe_str(mpvio.db.str));
  }

  if (res > CR_OK && mpvio.status != MPVIO_EXT::SUCCESS)
  {
    Host_errors errors;
    DBUG_ASSERT(mpvio.status == MPVIO_EXT::FAILURE);
    switch (res)
    {
    case CR_AUTH_PLUGIN_ERROR:
      errors.m_auth_plugin= 1;
      break;
    case CR_AUTH_HANDSHAKE:
      errors.m_handshake= 1;
      break;
    case CR_AUTH_USER_CREDENTIALS:
      errors.m_authentication= 1;
      break;
    case CR_ERROR:
    default:
      /* Unknown of unspecified auth plugin error. */
      errors.m_auth_plugin= 1;
      break;
    }
    inc_host_errors(mpvio.thd->security_ctx->ip, &errors);
    if (!thd->is_error())
      login_failed_error(thd);
    DBUG_RETURN(1);
  }

  sctx->proxy_user[0]= 0;

  if (initialized) // if not --skip-grant-tables
  {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    bool is_proxy_user= FALSE;
    const char *auth_user = safe_str(acl_user->user.str);
    ACL_PROXY_USER *proxy_user;
    /* check if the user is allowed to proxy as another user */
    proxy_user= acl_find_proxy_user(auth_user, sctx->host, sctx->ip,
                                    mpvio.auth_info.authenticated_as,
                                          &is_proxy_user);
    if (is_proxy_user)
    {
      ACL_USER *acl_proxy_user;

      /* we need to find the proxy user, but there was none */
      if (!proxy_user)
      {
        Host_errors errors;
        errors.m_proxy_user= 1;
        inc_host_errors(mpvio.thd->security_ctx->ip, &errors);
        if (!thd->is_error())
          login_failed_error(thd);
        DBUG_RETURN(1);
      }

      my_snprintf(sctx->proxy_user, sizeof(sctx->proxy_user) - 1,
                  "'%s'@'%s'", auth_user,
                  safe_str(acl_user->host.hostname));

      /* we're proxying : find the proxy user definition */
      mysql_mutex_lock(&acl_cache->lock);
      acl_proxy_user= find_user_exact(safe_str(proxy_user->get_proxied_host()),
                                     mpvio.auth_info.authenticated_as);
      if (!acl_proxy_user)
      {
        mysql_mutex_unlock(&acl_cache->lock);

        Host_errors errors;
        errors.m_proxy_user_acl= 1;
        inc_host_errors(mpvio.thd->security_ctx->ip, &errors);
        if (!thd->is_error())
          login_failed_error(thd);
        DBUG_RETURN(1);
      }
      acl_user= acl_proxy_user->copy(thd->mem_root);
      mysql_mutex_unlock(&acl_cache->lock);
    }
#endif

    sctx->master_access= acl_user->access;
    if (acl_user->user.str)
      strmake_buf(sctx->priv_user, acl_user->user.str);
    else
      *sctx->priv_user= 0;

    if (acl_user->host.hostname)
      strmake_buf(sctx->priv_host, acl_user->host.hostname);
    else
      *sctx->priv_host= 0;

    /*
      OK. Let's check the SSL. Historically it was checked after the password,
      as an additional layer, not instead of the password
      (in which case it would've been a plugin too).
    */
    if (acl_check_ssl(thd, acl_user))
    {
      Host_errors errors;
      errors.m_ssl= 1;
      inc_host_errors(mpvio.thd->security_ctx->ip, &errors);
      login_failed_error(thd);
      DBUG_RETURN(1);
    }

    /*
      Don't allow the user to connect if he has done too many queries.
      As we are testing max_user_connections == 0 here, it means that we
      can't let the user change max_user_connections from 0 in the server
      without a restart as it would lead to wrong connect counting.
    */
    if ((acl_user->user_resource.questions ||
         acl_user->user_resource.updates ||
         acl_user->user_resource.conn_per_hour ||
         acl_user->user_resource.user_conn ||
         acl_user->user_resource.max_statement_time != 0.0 ||
         max_user_connections_checking) &&
         get_or_create_user_conn(thd,
           (opt_old_style_user_limits ? sctx->user : sctx->priv_user),
           (opt_old_style_user_limits ? sctx->host_or_ip : sctx->priv_host),
           &acl_user->user_resource))
      DBUG_RETURN(1); // The error is set by get_or_create_user_conn()

    if (acl_user->user_resource.max_statement_time != 0.0)
    {
      thd->variables.max_statement_time_double=
        acl_user->user_resource.max_statement_time;
      thd->variables.max_statement_time=
        (ulonglong) (thd->variables.max_statement_time_double * 1e6 + 0.1);
    }
  }
  else
    sctx->skip_grants();

  if (thd->user_connect &&
      (thd->user_connect->user_resources.conn_per_hour ||
       thd->user_connect->user_resources.user_conn ||
       max_user_connections_checking) &&
       check_for_max_user_connections(thd, thd->user_connect))
  {
    /* Ensure we don't decrement thd->user_connections->connections twice */
    thd->user_connect= 0;
    status_var_increment(denied_connections);
    DBUG_RETURN(1); // The error is set in check_for_max_user_connections()
  }

  DBUG_PRINT("info",
             ("Capabilities: %lu  packet_length: %ld  Host: '%s'  "
              "Login user: '%s' Priv_user: '%s'  Using password: %s "
              "Access: %lu  db: '%s'",
              thd->client_capabilities, thd->max_client_packet_length,
              sctx->host_or_ip, sctx->user, sctx->priv_user,
              thd->password ? "yes": "no",
              sctx->master_access, mpvio.db.str));

  if (command == COM_CONNECT &&
      !(thd->main_security_ctx.master_access & SUPER_ACL))
  {
    mysql_mutex_lock(&LOCK_connection_count);
    bool count_ok= (*thd->scheduler->connection_count <=
                    *thd->scheduler->max_connections);
    mysql_mutex_unlock(&LOCK_connection_count);
    if (!count_ok)
    {                                         // too many connections
      my_error(ER_CON_COUNT_ERROR, MYF(0));
      DBUG_RETURN(1);
    }
  }

  /*
    This is the default access rights for the current database.  It's
    set to 0 here because we don't have an active database yet (and we
    may not have an active database to set.
  */
  sctx->db_access=0;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /*
    In case the user has a default role set, attempt to set that role
  */
  if (initialized && acl_user->default_rolename.length) {
    ulonglong access= 0;
    int result;
    result= acl_check_setrole(thd, acl_user->default_rolename.str, &access);
    if (!result)
      result= acl_setrole(thd, acl_user->default_rolename.str, access);
    if (result)
      thd->clear_error(); // even if the default role was not granted, do not
                          // close the connection
  }
#endif

  /* Change a database if necessary */
  if (mpvio.db.length)
  {
    if (mysql_change_db(thd, &mpvio.db, FALSE))
    {
      /* mysql_change_db() has pushed the error message. */
      status_var_increment(thd->status_var.access_denied_errors);
      DBUG_RETURN(1);
    }
  }

  thd->net.net_skip_rest_factor= 2;  // skip at most 2*max_packet_size

  if (mpvio.auth_info.external_user[0])
    sctx->external_user= my_strdup(mpvio.auth_info.external_user, MYF(0));

  if (res == CR_OK_HANDSHAKE_COMPLETE)
    thd->get_stmt_da()->disable_status();
  else
    my_ok(thd);

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(set_thread_user_host)
    (thd->main_security_ctx.user, strlen(thd->main_security_ctx.user),
    thd->main_security_ctx.host_or_ip, strlen(thd->main_security_ctx.host_or_ip));
#endif

  /* Ready to handle queries */
  DBUG_RETURN(0);
}

/**
  MySQL Server Password Authentication Plugin

  In the MySQL authentication protocol:
  1. the server sends the random scramble to the client
  2. client sends the encrypted password back to the server
  3. the server checks the password.
*/
static int native_password_authenticate(MYSQL_PLUGIN_VIO *vio,
                                        MYSQL_SERVER_AUTH_INFO *info)
{
  uchar *pkt;
  int pkt_len;
  MPVIO_EXT *mpvio= (MPVIO_EXT *) vio;
  THD *thd=mpvio->thd;
  DBUG_ENTER("native_password_authenticate");

  /* generate the scramble, or reuse the old one */
  if (thd->scramble[SCRAMBLE_LENGTH])
  {
    create_random_string(thd->scramble, SCRAMBLE_LENGTH, &thd->rand);
    /* and send it to the client */
    if (mpvio->write_packet(mpvio, (uchar*)thd->scramble, SCRAMBLE_LENGTH + 1))
      DBUG_RETURN(CR_AUTH_HANDSHAKE);
  }

  /* reply and authenticate */

  /*
    <digression>
      This is more complex than it looks.

      The plugin (we) may be called right after the client was connected -
      and will need to send a scramble, read reply, authenticate.

      Or the plugin may be called after another plugin has sent a scramble,
      and read the reply. If the client has used the correct client-plugin,
      we won't need to read anything here from the client, the client
      has already sent a reply with everything we need for authentication.

      Or the plugin may be called after another plugin has sent a scramble,
      and read the reply, but the client has used the wrong client-plugin.
      We'll need to sent a "switch to another plugin" packet to the
      client and read the reply. "Use the short scramble" packet is a special
      case of "switch to another plugin" packet.

      Or, perhaps, the plugin may be called after another plugin has
      done the handshake but did not send a useful scramble. We'll need
      to send a scramble (and perhaps a "switch to another plugin" packet)
      and read the reply.

      Besides, a client may be an old one, that doesn't understand plugins.
      Or doesn't even understand 4.0 scramble.

      And we want to keep the same protocol on the wire  unless non-native
      plugins are involved.

      Anyway, it still looks simple from a plugin point of view:
      "send the scramble, read the reply and authenticate".
      All the magic is transparently handled by the server.
    </digression>
  */

  /* read the reply with the encrypted password */
  if ((pkt_len= mpvio->read_packet(mpvio, &pkt)) < 0)
    DBUG_RETURN(CR_AUTH_HANDSHAKE);
  DBUG_PRINT("info", ("reply read : pkt_len=%d", pkt_len));

#ifdef NO_EMBEDDED_ACCESS_CHECKS
  DBUG_RETURN(CR_OK);
#endif

  DBUG_EXECUTE_IF("native_password_bad_reply", { pkt_len= 12; });

  if (pkt_len == 0) /* no password */
    DBUG_RETURN(mpvio->acl_user->salt_len != 0 ? CR_AUTH_USER_CREDENTIALS : CR_OK);

  info->password_used= PASSWORD_USED_YES;
  if (pkt_len == SCRAMBLE_LENGTH)
  {
    if (!mpvio->acl_user->salt_len)
      DBUG_RETURN(CR_AUTH_USER_CREDENTIALS);

    if (check_scramble(pkt, thd->scramble, mpvio->acl_user->salt))
      DBUG_RETURN(CR_AUTH_USER_CREDENTIALS);
    else
      DBUG_RETURN(CR_OK);
  }

  my_error(ER_HANDSHAKE_ERROR, MYF(0));
  DBUG_RETURN(CR_AUTH_HANDSHAKE);
}

static int old_password_authenticate(MYSQL_PLUGIN_VIO *vio,
                                     MYSQL_SERVER_AUTH_INFO *info)
{
  uchar *pkt;
  int pkt_len;
  MPVIO_EXT *mpvio= (MPVIO_EXT *) vio;
  THD *thd=mpvio->thd;

  /* generate the scramble, or reuse the old one */
  if (thd->scramble[SCRAMBLE_LENGTH])
  {
    create_random_string(thd->scramble, SCRAMBLE_LENGTH, &thd->rand);
    /* and send it to the client */
    if (mpvio->write_packet(mpvio, (uchar*)thd->scramble, SCRAMBLE_LENGTH + 1))
      return CR_AUTH_HANDSHAKE;
  }

  /* read the reply and authenticate */
  if ((pkt_len= mpvio->read_packet(mpvio, &pkt)) < 0)
    return CR_AUTH_HANDSHAKE;

#ifdef NO_EMBEDDED_ACCESS_CHECKS
  return CR_OK;
#endif

  /*
    legacy: if switch_from_long_to_short_scramble,
    the password is sent \0-terminated, the pkt_len is always 9 bytes.
    We need to figure out the correct scramble length here.
  */
  if (pkt_len == SCRAMBLE_LENGTH_323 + 1)
    pkt_len= strnlen((char*)pkt, pkt_len);

  if (pkt_len == 0) /* no password */
    return info->auth_string[0] ? CR_AUTH_USER_CREDENTIALS : CR_OK;

  if (secure_auth(thd))
    return CR_AUTH_HANDSHAKE;

  info->password_used= PASSWORD_USED_YES;

  if (pkt_len == SCRAMBLE_LENGTH_323)
  {
    if (!mpvio->acl_user->salt_len)
      return CR_AUTH_USER_CREDENTIALS;

    return check_scramble_323(pkt, thd->scramble,
                             (ulong *) mpvio->acl_user->salt) ? 
                             CR_AUTH_USER_CREDENTIALS : CR_OK;
  }

  my_error(ER_HANDSHAKE_ERROR, MYF(0));
  return CR_AUTH_HANDSHAKE;
}

static struct st_mysql_auth native_password_handler=
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  native_password_plugin_name.str,
  native_password_authenticate
};

static struct st_mysql_auth old_password_handler=
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  old_password_plugin_name.str,
  old_password_authenticate
};

maria_declare_plugin(mysql_password)
{
  MYSQL_AUTHENTICATION_PLUGIN,                  /* type constant    */
  &native_password_handler,                     /* type descriptor  */
  native_password_plugin_name.str,              /* Name             */
  "R.J.Silk, Sergei Golubchik",                 /* Author           */
  "Native MySQL authentication",                /* Description      */
  PLUGIN_LICENSE_GPL,                           /* License          */
  NULL,                                         /* Init function    */
  NULL,                                         /* Deinit function  */
  0x0100,                                       /* Version (1.0)    */
  NULL,                                         /* status variables */
  NULL,                                         /* system variables */
  "1.0",                                        /* String version   */
  MariaDB_PLUGIN_MATURITY_STABLE                /* Maturity         */
},
{
  MYSQL_AUTHENTICATION_PLUGIN,                  /* type constant    */
  &old_password_handler,                        /* type descriptor  */
  old_password_plugin_name.str,                 /* Name             */
  "R.J.Silk, Sergei Golubchik",                 /* Author           */
  "Old MySQL-4.0 authentication",               /* Description      */
  PLUGIN_LICENSE_GPL,                           /* License          */
  NULL,                                         /* Init function    */
  NULL,                                         /* Deinit function  */
  0x0100,                                       /* Version (1.0)    */
  NULL,                                         /* status variables */
  NULL,                                         /* system variables */
  "1.0",                                        /* String version   */
  MariaDB_PLUGIN_MATURITY_STABLE                /* Maturity         */
}
maria_declare_plugin_end;

