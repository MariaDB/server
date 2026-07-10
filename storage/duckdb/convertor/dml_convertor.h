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

#include "ddl_convertor.h"

void append_field_value_to_sql(String &target_str, Field *field);

class DMLConvertor : public BaseConvertor
{
public:
  DMLConvertor(TABLE *table) : m_table(table) {}

  bool check() override { return false; }

  std::string translate() override;

protected:
  virtual void generate_prefix(String &query)= 0;

  virtual void generate_fields_and_values(String &query) {}

  virtual void generate_where_clause(String &query);

  virtual void append_where_value(String &query, Field *field) {}

  TABLE *m_table;

private:
  void fill_index_fields_for_where(std::vector<Field *> &fields);
};

class InsertConvertor : public DMLConvertor
{
public:
  InsertConvertor(TABLE *table, bool flag)
      : DMLConvertor(table), idempotent_flag(flag)
  {
  }

protected:
  void generate_prefix(String &query) override;

  void generate_fields_and_values(String &query) override;

  void generate_where_clause(String &query) override {}

private:
  bool idempotent_flag;
};

class UpdateConvertor : public DMLConvertor
{
public:
  UpdateConvertor(TABLE *table, const uchar *old_row)
      : DMLConvertor(table), m_old_row(old_row)
  {
  }

protected:
  void generate_prefix(String &query) override;

  void generate_fields_and_values(String &query) override;

  void append_where_value(String &query, Field *field) override
  {
    uchar *saved_ptr= field->ptr;
    field->ptr=
        const_cast<uchar *>(m_old_row + field->offset(m_table->record[0]));
    append_field_value_to_sql(query, field);
    field->ptr= saved_ptr;
  }

private:
  const uchar *m_old_row;
};

class DeleteConvertor : public DMLConvertor
{
public:
  DeleteConvertor(TABLE *table, const uchar *old_row= nullptr)
      : DMLConvertor(table), m_old_row(old_row)
  {
  }

protected:
  void generate_prefix(String &query) override;

  void append_where_value(String &query, Field *field) override
  {
    if (!m_old_row)
    {
      append_field_value_to_sql(query, field);
    }
    else
    {
      uchar *saved_ptr= field->ptr;
      field->ptr=
          const_cast<uchar *>(m_old_row + field->offset(m_table->record[0]));
      append_field_value_to_sql(query, field);
      field->ptr= saved_ptr;
    }
  }

private:
  const uchar *m_old_row;
};
