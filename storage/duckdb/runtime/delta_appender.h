/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#pragma once

#include <map>
#include <string>
#include "duckdb_manager.h"
#include "field.h"

class DeltaAppender
{
public:
  int append_row_insert(TABLE *table, ulonglong trx_no,
                        const MY_BITMAP *blob_type_map);

  int append_row_update(TABLE *table, ulonglong trx_no, const uchar *old_row);

  int append_row_delete(TABLE *table, ulonglong trx_no,
                        const uchar *old_row= nullptr);

  static std::string buf_table_name(std::string db, std::string tb)
  {
    return db + "_rds_buf_" + tb;
  }

  DeltaAppender(std::shared_ptr<duckdb::Connection> con, std::string db,
                std::string tb, bool use_tmp_table)
      : m_use_tmp_table(use_tmp_table), m_schema_name(db), m_table_name(tb),
        m_con(con)
  {
  }

  bool Initialize(TABLE *table);

  int append_mysql_field(const Field *field,
                         const MY_BITMAP *blob_type_map= nullptr);

  DeltaAppender()= default;

  ~DeltaAppender() { cleanup(); }

  bool flush(bool idempotent_flag);

  void cleanup();

private:
  void generateQuery(std::stringstream &ss, bool delete_flag);

  bool m_use_tmp_table;

  std::string m_schema_name;
  std::string m_table_name;
  std::string m_tmp_table_name;

  MY_BITMAP m_pk_bitmap;
  std::string m_pk_list{""};
  std::string m_col_list{""};

  uint64_t m_row_count{0};
  bool m_has_insert{false};
  bool m_has_update{false};
  bool m_has_delete{false};

  std::shared_ptr<duckdb::Connection> m_con;

  std::unique_ptr<duckdb::Appender> m_appender;
};

class DeltaAppenders
{
public:
  DeltaAppenders(std::shared_ptr<duckdb::Connection> con)
      : m_con(con), m_append_infos()
  {
  }

  ~DeltaAppenders()= default;

  void delete_appender(std::string &db, std::string &tb);

  bool flush_all(bool idempotent_flag, std::string &error_msg);

  bool is_empty() { return m_append_infos.empty(); }

  DeltaAppender *get_appender(std::string &db, std::string &tb,
                              bool insert_only, TABLE *table);

private:
  std::shared_ptr<duckdb::Connection> m_con;

  std::map<std::pair<std::string, std::string>, std::unique_ptr<DeltaAppender>>
      m_append_infos;
};
