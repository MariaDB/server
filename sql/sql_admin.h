/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.

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

#ifndef SQL_TABLE_MAINTENANCE_H
#define SQL_TABLE_MAINTENANCE_H

/* Must be able to hold ALTER TABLE t PARTITION BY ... KEY ALGORITHM = 1 ... */
#define SQL_ADMIN_MSG_TEXT_SIZE (128 * 1024)

bool mysql_assign_to_keycache(THD* thd, TABLE_LIST* table_list,
                              const LEX_CSTRING *key_cache_name);
bool mysql_preload_keys(THD* thd, TABLE_LIST* table_list);
int reassign_keycache_tables(THD* thd, KEY_CACHE *src_cache,
                             KEY_CACHE *dst_cache);
void fill_check_table_metadata_fields(THD *thd, List<Item>* fields);
/**
  Sql_cmd_analyze_table represents the ANALYZE TABLE statement.
*/
class Sql_cmd_analyze_table : public Sql_cmd
{
public:
  /**
    Constructor, used to represent a ANALYZE TABLE statement.
  */
  Sql_cmd_analyze_table() = default;

  ~Sql_cmd_analyze_table() = default;

  bool execute(THD *thd) override;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_ANALYZE;
  }
};



/**
  Sql_cmd_check_table represents the CHECK TABLE statement.
*/
class Sql_cmd_check_table : public Sql_cmd
{
public:
  /**
    Constructor, used to represent a CHECK TABLE statement.
  */
  Sql_cmd_check_table() = default;

  ~Sql_cmd_check_table() = default;

  bool execute(THD *thd) override;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_CHECK;
  }
};


/**
  Sql_cmd_optimize_table represents the OPTIMIZE TABLE statement.
*/
class Sql_cmd_optimize_table : public Sql_cmd
{
public:
  /**
    Constructor, used to represent a OPTIMIZE TABLE statement.
  */
  Sql_cmd_optimize_table() = default;

  ~Sql_cmd_optimize_table() = default;

  bool execute(THD *thd) override;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_OPTIMIZE;
  }
};



/**
  Sql_cmd_repair_table represents the REPAIR TABLE statement.
*/
class Sql_cmd_repair_table : public Sql_cmd
{
public:
  /**
    Constructor, used to represent a REPAIR TABLE statement.
  */
  Sql_cmd_repair_table() = default;

  ~Sql_cmd_repair_table() = default;

  bool execute(THD *thd) override;

  enum_sql_command sql_command_code() const override
  {
    return SQLCOM_REPAIR;
  }
};

/**
  Sql_cmd_clone implements CLONE ... statement.
*/
class Clone_handler;

class Sql_cmd_clone : public Sql_cmd {
 public:
  /** Construct clone command for clone server */
  explicit Sql_cmd_clone()
      : m_host(),
        m_port(),
        m_user(),
        m_passwd(),
        m_data_dir(),
        m_clone(),
        m_is_local(false) {}

  /** Construct clone command for clone client
  @param[in]  user_info user, password and remote host information
  @param[in]  port port for remote server
  @param[in]  data_dir data directory to clone */
  explicit Sql_cmd_clone(LEX_USER *user_info, ulong port, LEX_CSTRING data_dir);

  /** Construct clone command for local clone
  @param[in]  data_dir data directory to clone */
  explicit Sql_cmd_clone(LEX_CSTRING data_dir)
      : m_host(),
        m_port(),
        m_user(),
        m_passwd(),
        m_data_dir(data_dir),
        m_clone(),
        m_is_local(true) {}

  enum_sql_command sql_command_code() const override { return SQLCOM_CLONE; }

  bool execute(THD *thd) override;

  /** Execute clone server.
  @param[in] thd server session
  @return true, if error */
  bool execute_server(THD *thd);

  /** Load clone plugin for clone server.
  @param[in] thd server session
  @return true, if error */
  bool load(THD *thd);

  /** @return true, if it is local clone command */
  bool is_local() const { return (m_is_local); }

 private:
  /** Remote server IP */
  LEX_CSTRING m_host;

  /** Remote server port */
  const ulong m_port;

  /** User name for remote connection */
  LEX_CSTRING m_user;

  /** Password for remote connection */
  LEX_CSTRING m_passwd;

  /** Data directory for cloned data */
  LEX_CSTRING m_data_dir;

  /** Clone handle in server */
  Clone_handler *m_clone;

  /** Loaded clone plugin reference */
  plugin_ref m_plugin;

  /** If it is local clone operation */
  bool m_is_local;
};
#endif
