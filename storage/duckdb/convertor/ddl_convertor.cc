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

#include "ddl_convertor.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include <my_global.h>
#undef UNKNOWN
#include "duckdb_charset_collation.h"
#include "duckdb_config.h"
#include "sql_class.h"
#include "sql_alter.h"
#include "sql_table.h" /* primary_key_name */

/* ----- Helpers ----- */

bool report_duckdb_table_struct_error(const char *not_supported,
                                      const char *try_instead,
                                      const char *column,
                                      ddl_error_context ctx)
{
  if (ctx == ddl_error_context::CREATE)
  {
    my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), "DuckDB", not_supported);
  }
  else
  {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s is not supported. Try %s '%s'",
             not_supported, try_instead, column);
    my_error(ER_GET_ERRMSG, MYF(0), HA_ERR_GENERIC, buf, "DuckDB");
  }
  return true;
}

/** Check if two types differ between old and new Create_field / Field */
static bool is_type_changed(const Create_field *new_field,
                            const Field *old_field)
{
  std::string old_type= FieldConvertor::convert_type(old_field);

  /*
    Build a temporary Field-like check via the new Create_field.
    For simplicity compare the DuckDB type strings.
  */
  if (new_field->field)
  {
    /* new_field->field points to the original old field for CHANGE COLUMN */
    /* The actual new type comes from the altered_table's field */
  }
  return false; /* Will be refined once we have the new table's field */
}

/** Check if nullable changed */
static bool is_nullable_change(const Create_field *new_field,
                               const Field *old_field)
{
  bool old_not_null= (old_field->flags & NOT_NULL_FLAG) != 0;
  bool new_not_null= (new_field->flags & NOT_NULL_FLAG) != 0;
  return old_not_null != new_not_null;
}

/** Check if name changed */
static bool is_name_changed(const Create_field *new_field,
                            const Field *old_field)
{
  if (new_field->change.str == nullptr)
    return false;
  return strcasecmp(new_field->field_name.str, new_field->change.str) != 0;
}

/** Find the associated field in the new table. */
static Field *find_field(const Create_field *new_field, const TABLE *new_table)
{
  Field **first_field= new_table->field;
  Field **ptr, *cur_field;
  for (ptr= first_field; (cur_field= *ptr); ptr++)
  {
    if (strcasecmp(cur_field->field_name.str, new_field->field_name.str) == 0)
      break;
  }
  return cur_field;
}

/** Check if the key is primary key. */
static bool is_primary_key(const KEY *key) __attribute__((unused));
static bool is_primary_key(const KEY *key)
{
  return ((key->flags & HA_NOSAME) != 0) &&
         (strcasecmp(key->name.str, primary_key_name.str) == 0);
}

/**
  Extract default expression string for a field from TABLE_SHARE::vcol_defs.

  The Item tree in field->default_value->expr may be corrupted at
  ha_duckdb::create() time (the mem_root it was allocated on gets reset
  between pack_vcols and ha_create_table). Instead, read the original
  expression text directly from the .frm binary blob (vcol_defs).

  vcol_defs format per entry (FRM_VER_EXPRESSSIONS):
    byte 0     : type (VCOL_DEFAULT=2)
    bytes 1-2  : field_nr
    bytes 3-4  : expr_length
    byte 5     : name_length
    bytes 6..  : name (name_length bytes)
    then       : expression text (expr_length bytes)
*/
static std::string get_default_expr_from_vcol_defs(const TABLE_SHARE *share,
                                                   uint target_field_nr)
{
  if (!share->vcol_defs.length || !share->vcol_defs.str)
    return "";

  const uchar *pos= share->vcol_defs.str;
  const uchar *end= pos + share->vcol_defs.length;

  while (pos < end)
  {
    uint type= pos[0];
    uint field_nr= uint2korr(pos + 1);
    uint expr_len= uint2korr(pos + 3);
    uint name_len= pos[5];
    pos+= 6 + name_len; /* FRM_VCOL_NEW_HEADER_SIZE + name */

    if (type == 2 /* VCOL_DEFAULT */ && field_nr == target_field_nr)
      return std::string((const char *) pos, expr_len);

    pos+= expr_len;
  }
  return "";
}

/**
  Read the literal default value of a field from the default record and
  return it as a string suitable for DuckDB SQL.

  BIT fields are converted to DuckDB blob literal format: '\xHH...'::BLOB.
  Other fields use standard quoted literal format: 'value'.

  @param field    Field whose default value to read (must not be at offset)
  @param offset   Offset from record[0] to default_values (s->default_values -
                  record[0])
  @return         Default value string, or "NULL" if field is null at default
                  record
*/
static std::string get_field_default_for_duckdb(Field *field,
                                                my_ptrdiff_t offset)
{
  field->move_field_offset(offset);

  std::string default_value;
  if (field->is_null())
  {
    default_value= "NULL";
  }
  else if (field->type() == MYSQL_TYPE_BIT)
  {
    /* BIT maps to blob in DuckDB: '\xHH\xHH...'::BLOB */
    char vbuf[MAX_FIELD_WIDTH];
    String vstr(vbuf, sizeof(vbuf), &my_charset_bin);
    String *val= field->val_str(&vstr);
    std::ostringstream ss;
    ss << "'";
    if (val)
    {
      for (uint i= 0; i < val->length(); i++)
      {
        char hx[8];
        snprintf(hx, sizeof(hx), "\\x%02X", (unsigned char) val->ptr()[i]);
        ss << hx;
      }
    }
    ss << "'::BLOB";
    default_value= ss.str();
  }
  else
  {
    char buf[MAX_FIELD_WIDTH];
    String str(buf, sizeof(buf), system_charset_info);
    String *val= field->val_str(&str);
    if (val && val->length() > 0)
      default_value= "'" + std::string(val->ptr(), val->length()) + "'";
    else
      default_value= "NULL";
  }

  field->move_field_offset(-offset);
  return default_value;
}

/* ----- String constants ----- */

static constexpr char CREATE_TABLE_STR[]= "CREATE TABLE ";
static constexpr char IF_NOT_EXISTS_STR[]= "IF NOT EXISTS ";
static constexpr char ALTER_TABLE_OP_STR[]= "ALTER TABLE ";
static constexpr char RENAME_TABLE_OP_STR[]= " RENAME TO ";
static constexpr char ALTER_COLUMN_OP_STR[]= " ALTER COLUMN ";
static constexpr char ADD_COLUMN_OP_STR[]= " ADD COLUMN ";
static constexpr char DROP_COLUMN_OP_STR[]= " DROP COLUMN ";
static constexpr char RENAME_COLUMN_OP_STR[]= " RENAME COLUMN ";
static constexpr char DEFINE_DEFAULT_STR[]= " DEFAULT ";
static constexpr char SET_DATA_TYPE_STR[]= " SET DATA TYPE ";
static constexpr char SET_DEFAULT_STR[]= " SET DEFAULT ";
static constexpr char DROP_DEFAULT_STR[]= " DROP DEFAULT";
static constexpr char SET_NOT_NULL_STR[]= " SET NOT NULL";
static constexpr char DROP_NOT_NULL_STR[]= " DROP NOT NULL";

/* ----- Statement builders ----- */

static void append_stmt_alter_table(std::ostringstream &output,
                                    const std::string &schema_name,
                                    const std::string &table_name)
{
  output << "USE \"" << schema_name << "\";";
  output << ALTER_TABLE_OP_STR << '"' << table_name << '"';
}

static void append_stmt_column_add(std::ostringstream &output,
                                   const std::string &schema_name,
                                   const std::string &table_name,
                                   const std::string &column_name,
                                   const std::string &column_type,
                                   bool has_default,
                                   const std::string &default_value)
{
  assert(!schema_name.empty() && !table_name.empty() && !column_name.empty() &&
         !column_type.empty());
  append_stmt_alter_table(output, schema_name, table_name);
  output << ADD_COLUMN_OP_STR << '"' << column_name << '"' << " "
         << column_type;
  if (has_default)
    output << DEFINE_DEFAULT_STR << default_value;
  output << ";";
}

static void append_stmt_column_drop(std::ostringstream &output,
                                    const std::string &schema_name,
                                    const std::string &table_name,
                                    const std::string &column_name)
{
  assert(!schema_name.empty() && !table_name.empty() && !column_name.empty());
  append_stmt_alter_table(output, schema_name, table_name);
  output << DROP_COLUMN_OP_STR << '"' << column_name << '"' << ";";
}

static void append_stmt_column_change_type(std::ostringstream &output,
                                           const std::string &schema_name,
                                           const std::string &table_name,
                                           const std::string &column_name,
                                           const std::string &column_type)
{
  assert(!schema_name.empty() && !table_name.empty() && !column_name.empty() &&
         !column_type.empty());
  append_stmt_alter_table(output, schema_name, table_name);
  output << ALTER_COLUMN_OP_STR << '"' << column_name << '"'
         << SET_DATA_TYPE_STR << column_type << ";";
}

static void append_stmt_column_rename(std::ostringstream &output,
                                      const std::string &schema_name,
                                      const std::string &table_name,
                                      const std::string &old_column_name,
                                      const std::string &new_column_name)
{
  assert(!schema_name.empty() && !table_name.empty() &&
         !old_column_name.empty() && !new_column_name.empty());
  append_stmt_alter_table(output, schema_name, table_name);
  output << RENAME_COLUMN_OP_STR << '"' << old_column_name << '"' << " TO "
         << '"' << new_column_name << '"' << ";";
}

static void append_stmt_column_set_default(std::ostringstream &output,
                                           const std::string &schema_name,
                                           const std::string &table_name,
                                           const std::string &column_name,
                                           const std::string &default_value)
{
  assert(!schema_name.empty() && !table_name.empty() && !column_name.empty() &&
         !default_value.empty());
  append_stmt_alter_table(output, schema_name, table_name);
  output << ALTER_COLUMN_OP_STR << '"' << column_name << '"' << SET_DEFAULT_STR
         << default_value << ";";
}

static void append_stmt_column_drop_default(std::ostringstream &output,
                                            const std::string &schema_name,
                                            const std::string &table_name,
                                            const std::string &column_name)
{
  assert(!schema_name.empty() && !table_name.empty() && !column_name.empty());
  append_stmt_alter_table(output, schema_name, table_name);
  output << ALTER_COLUMN_OP_STR << '"' << column_name << '"'
         << DROP_DEFAULT_STR << ";";
}

static void append_stmt_column_set_not_null(std::ostringstream &output,
                                            const std::string &schema_name,
                                            const std::string &table_name,
                                            const std::string &column_name)
{
  assert(!schema_name.empty() && !table_name.empty() && !column_name.empty());
  append_stmt_alter_table(output, schema_name, table_name);
  output << ALTER_COLUMN_OP_STR << '"' << column_name << '"'
         << SET_NOT_NULL_STR << ";";
}

static void append_stmt_column_drop_not_null(std::ostringstream &output,
                                             const std::string &schema_name,
                                             const std::string &table_name,
                                             const std::string &column_name)
{
  assert(!schema_name.empty() && !table_name.empty() && !column_name.empty());
  append_stmt_alter_table(output, schema_name, table_name);
  output << ALTER_COLUMN_OP_STR << '"' << column_name << '"'
         << DROP_NOT_NULL_STR << ";";
}

static void append_stmt_table_rename(std::ostringstream &output,
                                     const std::string &old_schema_name,
                                     const std::string &old_table_name,
                                     const std::string &new_schema_name
                                     __attribute__((unused)),
                                     const std::string &new_table_name)
{
  assert(!old_schema_name.empty() && !old_table_name.empty() &&
         !new_schema_name.empty() && !new_table_name.empty());
  assert(old_schema_name == new_schema_name);
  append_stmt_alter_table(output, old_schema_name, old_table_name);
  output << RENAME_TABLE_OP_STR << '"' << new_table_name << '"' << ";";
}

/* ----- FieldConvertor ----- */

bool FieldConvertor::check()
{
  /* not support auto_increment */
  if (m_field->flags & AUTO_INCREMENT_FLAG)
    return report_duckdb_table_struct_error(
        "AUTO_INCREMENT", "removing AUTO_INCREMENT from column",
        m_field->field_name.str, m_ctx);

  /* No support for INVISIBLE columns. */
  if (m_field->invisible >= INVISIBLE_USER)
    return report_duckdb_table_struct_error("INVISIBLE column",
                                            "removing INVISIBLE from column",
                                            m_field->field_name.str, m_ctx);

  /* No support for non-utf8 charset. */
  if (m_field->has_charset())
  {
    const CHARSET_INFO *cs= m_field->charset();
    if (strcmp(cs->cs_name.str, "utf8") &&
        strcmp(cs->cs_name.str, "utf8mb3") &&
        strcmp(cs->cs_name.str, "utf8mb4") && strcmp(cs->cs_name.str, "ascii"))
    {
      return report_duckdb_table_struct_error(
          "non-utf8 charset", "using utf8mb4 charset for column",
          m_field->field_name.str, m_ctx);
    }
  }

  /* No support for generated column. */
  assert(!m_field->vcol_info);

  return false;
}

std::string FieldConvertor::translate()
{
  Field *field= m_field;

  /* Skip system-invisible columns (e.g. row versioning) */
  if (field->invisible >= INVISIBLE_SYSTEM)
    return "";

  std::ostringstream result;

  result << '"' << field->field_name.str << '"' << " ";
  result << convert_type(m_field);

  if (field->flags & NOT_NULL_FLAG)
    result << " NOT NULL";

  /*
    Get default value from Field directly (no dd::Column in MariaDB).
    In MariaDB, default expressions are stored in field->default_value
    (Virtual_column_info*), and simple defaults are stored in record[1].
  */
  if (!(field->flags & NO_DEFAULT_VALUE_FLAG))
  {
    if (field->default_value)
    {
      /*
        Expression default. The Item tree in field->default_value->expr
        is corrupted at ha_duckdb::create() time. Read the expression
        string directly from TABLE_SHARE::vcol_defs (.frm binary blob).
      */
      std::string expr_str=
          get_default_expr_from_vcol_defs(field->table->s, field->field_index);
      if (!expr_str.empty())
        result << " DEFAULT (" << expr_str << ")";
    }
    else if (field->table->s->default_values && field->table->record[0])
    {
      /*
        Simple literal default — extract from s->default_values.
        At ha_create() time these pointers may not be initialised yet
        (e.g. nullable column without explicit DEFAULT), so guard.
      */
      my_ptrdiff_t offset=
          field->table->s->default_values - field->table->record[0];
      std::string def= get_field_default_for_duckdb(field, offset);
      if (def != "NULL")
        result << " DEFAULT " << def;
    }
  }

  assert(!(field->flags & AUTO_INCREMENT_FLAG));

  return result.str();
}

std::string FieldConvertor::convert_type(const Field *field)
{
  std::string ret;

  if (is_uuid_field(field))
    return "UUID";

  enum_field_types field_type= field->real_type();
  bool is_unsigned= (field->flags & UNSIGNED_FLAG) != 0;
  bool has_charset= field->has_charset();

  switch (field_type)
  {
  case MYSQL_TYPE_TINY:
    ret= is_unsigned ? "utinyint" : "tinyint";
    break;
  case MYSQL_TYPE_SHORT:
    ret= is_unsigned ? "usmallint" : "smallint";
    break;
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
    ret= is_unsigned ? "uinteger" : "integer";
    break;
  case MYSQL_TYPE_LONGLONG:
    ret= is_unsigned ? "ubigint" : "bigint";
    break;
  case MYSQL_TYPE_FLOAT:
    ret= "float";
    break;
  case MYSQL_TYPE_DOUBLE:
    ret= "double";
    break;
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL: {
    const Field_new_decimal *decimal_field=
        static_cast<const Field_new_decimal *>(field);
    uint precision= decimal_field->precision;
    uint dec= decimal_field->dec;
    if (precision <= 38)
    {
      ret= "decimal(" + std::to_string(precision) + "," + std::to_string(dec) +
           ")";
    }
    else if (myduck::use_double_for_decimal)
    {
      ret= "double";
    }
    else
    {
      /* Clamp to max DuckDB precision */
      ret= "decimal(38," + std::to_string(std::min((uint) 38, dec)) + ")";
    }
    break;
  }
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
    ret= "timestamptz";
    break;
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_DATE:
    ret= "date";
    break;
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_TIME2:
    ret= "time";
    break;
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
    ret= "datetime";
    break;
  case MYSQL_TYPE_YEAR:
    ret= "integer";
    break;
  case MYSQL_TYPE_BIT:
    ret= "blob";
    break;
  case MYSQL_TYPE_GEOMETRY:
    ret= "blob";
    break;
  case MYSQL_TYPE_NULL:
    break;
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_ENUM:
    ret= "varchar";
    break;
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_VAR_STRING:
    ret= has_charset ? "varchar" : "blob";
    break;
  default:
    ret= "__unknown_type";
    break;
  }

  /* Append DuckDB collation for varchar columns */
  if (ret == "varchar" && field->has_charset())
  {
    std::string warn_msg;
    std::string co= myduck::get_duckdb_collation(field->charset(), warn_msg);
    ret.append(" COLLATE ").append(co);
  }

  /* MariaDB does not have MYSQL_TYPE_JSON; JSON is stored as LONG_BLOB with
     a special charset. This is handled by the blob/varchar path above. */

  std::transform(ret.begin(), ret.end(), ret.begin(), ::toupper);

  return ret;
}

/* ----- CreateTableConvertor ----- */

bool CreateTableConvertor::check()
{
  Field **first_field= m_table->field;
  Field **ptr, *field;

  for (ptr= first_field; (field= *ptr); ptr++)
  {
    if (FieldConvertor(field, ddl_error_context::CREATE).check())
      return true;
  }

  /* Check PK. */
  TABLE_SHARE *share= m_table->s;

  bool has_pk= false;
  KEY *key_info= m_table->key_info;

  /*
    TABLE_SHARE::primary_key is supposed to be either MAX_KEY or an index into
    key_info[]. However during CREATE (especially via ALGORITHM=COPY shadow
    tables) it can be uninitialized/garbage, so validate the range and
    fall back to scanning keys.
  */
  if (share->primary_key != MAX_KEY && share->primary_key < share->keys)
    has_pk= is_primary_key(key_info + share->primary_key);

  if (!has_pk)
  {
    for (uint i= 0; i < share->keys; i++)
    {
      if (is_primary_key(key_info + i))
      {
        has_pk= true;
        break;
      }
    }
  }

  if (myduck::require_primary_key && !has_pk)
  {
    my_error(ER_REQUIRES_PRIMARY_KEY, MYF(0));
    return true;
  }

  return false;
}

std::string CreateTableConvertor::translate()
{
  std::ostringstream result;
  assert((m_create_info->options & HA_LEX_CREATE_TMP_TABLE) == 0);

  result << "CREATE SCHEMA IF NOT EXISTS " << '"' << m_schema_name << '"'
         << ";";

  result << "USE " << '"' << m_schema_name << '"' << ";";

  result << CREATE_TABLE_STR;
  /* MariaDB: IF NOT EXISTS is handled at the SQL layer, not in HA_CREATE_INFO.
     Always use IF NOT EXISTS for safety in DuckDB. */
  result << IF_NOT_EXISTS_STR;

  result << '"' << m_table_name << '"';
  result << " (";

  append_column_definition(result);
  result << ");";

  return result.str();
}

void CreateTableConvertor::append_column_definition(std::ostringstream &output)
{
  Field *field= nullptr;
  Field **first_field= m_table->field;
  for (Field **ptr= first_field; (field= *ptr); ptr++)
  {
    if (ptr != first_field)
      output << ",";
    output << FieldConvertor(field).translate();
  }
}

/* ----- RenameTableConvertor ----- */

bool RenameTableConvertor::check()
{
  if (m_new_schema_name != m_schema_name)
  {
    return report_duckdb_table_struct_error(
        "cross-schema rename", "renaming within the same schema, not to",
        m_new_schema_name.c_str());
  }
  return false;
}

std::string RenameTableConvertor::translate()
{
  std::ostringstream result;
  append_stmt_table_rename(result, m_schema_name, m_table_name,
                           m_new_schema_name, m_new_table_name);
  return result.str();
}

/* ----- AddColumnConvertor ----- */

void AddColumnConvertor::prepare_columns()
{
  List_iterator<Create_field> new_field_it(m_alter_info->create_list);
  Create_field *new_field;

  while ((new_field= new_field_it++))
  {
    if (new_field->field != nullptr)
      continue;

    Field *field= find_field(new_field, m_new_table);
    m_columns_to_add.emplace_back(new_field, field);

    if ((new_field->flags & NOT_NULL_FLAG) != 0)
      m_columns_to_set_not_null.emplace_back(new_field, field);
  }
}

bool AddColumnConvertor::check()
{
  for (auto &pair : m_columns_to_add)
  {
    if (FieldConvertor(pair.second).check())
      return true;
  }
  return false;
}

std::string AddColumnConvertor::translate()
{
  std::ostringstream result;

  for (auto &pair : m_columns_to_add)
  {
    Create_field *new_field= pair.first;
    Field *field= pair.second;
    assert(field != nullptr);

    std::string type= FieldConvertor::convert_type(field);

    bool has_default= false;
    std::string default_value= "NULL";

    if (new_field->on_update != nullptr)
    {
      has_default= true;
      default_value= "CURRENT_TIMESTAMP";
    }
    else if (!(field->flags & NO_DEFAULT_VALUE_FLAG))
    {
      my_ptrdiff_t offset=
          field->table->s->default_values - field->table->record[0];
      has_default= true;
      default_value= get_field_default_for_duckdb(field, offset);
    }

    append_stmt_column_add(result, m_schema_name, m_table_name,
                           new_field->field_name.str, type, has_default,
                           default_value);
  }

  for (auto &pair : m_columns_to_set_not_null)
  {
    Create_field *new_field= pair.first;
    assert((new_field->flags & NOT_NULL_FLAG) != 0);
    append_stmt_column_set_not_null(result, m_schema_name, m_table_name,
                                    new_field->field_name.str);
  }

  return result.str();
}

/* ----- DropColumnConvertor ----- */

void DropColumnConvertor::prepare_columns()
{
  /*
    Prefer Alter_info::drop_list as authoritative (if still populated).
    Fallback to old/new table diff when drop_list is unavailable at commit
    time.

    Important: exclude renamed/changed columns from diff-based drop detection.
    In MariaDB, rename/change is represented in Alter_info::create_list via
    Create_field::change (old name).
  */
  if (m_alter_info && m_alter_info->drop_list.elements)
  {
    List_iterator<Alter_drop> drop_it(m_alter_info->drop_list);
    Alter_drop *drop;
    while ((drop= drop_it++))
    {
      if (drop->type != Alter_drop::COLUMN)
        continue;

      for (Field **old_ptr= m_old_table->field; *old_ptr; old_ptr++)
      {
        Field *old_field= *old_ptr;
#if MYSQL_VERSION_ID >= 110501
        if (strcasecmp(old_field->field_name.str, drop->name.str) == 0)
#else
        if (strcasecmp(old_field->field_name.str, drop->name) == 0)
#endif
        {
          m_columns_to_drop.emplace_back(nullptr, old_field);
          break;
        }
      }
    }
    return;
  }

  if (!m_new_table)
    return;

  /* Build a set of old names that are being renamed/changed. */
  std::unordered_set<std::string> renamed_old_names;
  if (m_alter_info)
  {
    List_iterator<Create_field> def_it(m_alter_info->create_list);
    Create_field *def;
    while ((def= def_it++))
    {
      if (def->change.str)
        renamed_old_names.emplace(def->change.str);
    }
  }

  for (Field **old_ptr= m_old_table->field; *old_ptr; old_ptr++)
  {
    Field *old_field= *old_ptr;

    if (renamed_old_names.find(old_field->field_name.str) !=
        renamed_old_names.end())
      continue;

    bool found_in_new= false;
    for (Field **new_ptr= m_new_table->field; *new_ptr; new_ptr++)
    {
      if (strcasecmp((*new_ptr)->field_name.str, old_field->field_name.str) ==
          0)
      {
        found_in_new= true;
        break;
      }
    }

    if (!found_in_new)
      m_columns_to_drop.emplace_back(nullptr, old_field);
  }
}

bool DropColumnConvertor::check()
{
  if (!myduck::require_primary_key)
    return false;

  for (auto &pair : m_columns_to_drop)
  {
    Field *field= pair.second;
    if (field && (field->flags & PRI_KEY_FLAG))
    {
      my_error(ER_REQUIRES_PRIMARY_KEY, MYF(0));
      return true;
    }
  }

  return false;
}

std::string DropColumnConvertor::translate()
{
  std::ostringstream result;

  for (auto &pair : m_columns_to_drop)
  {
    Field *field= pair.second;
    append_stmt_column_drop(result, m_schema_name, m_table_name,
                            field->field_name.str);
  }

  return result.str();
}

/* ----- ChangeColumnDefaultConvertor ----- */

void ChangeColumnDefaultConvertor::prepare_columns()
{
  /*
    Compare old and new table fields to detect default value changes.
    We cannot use alter_info->alter_list because it is consumed
    (emptied) by mysql_prepare_alter_table() before commit.
  */
  for (Field **new_ptr= m_new_table->field; *new_ptr; new_ptr++)
  {
    Field *new_field= *new_ptr;

    /* Find the same column in the old table by name */
    Field *old_field= nullptr;
    for (Field **old_ptr= m_old_table->field; *old_ptr; old_ptr++)
    {
      if (strcasecmp((*old_ptr)->field_name.str, new_field->field_name.str) ==
          0)
      {
        old_field= *old_ptr;
        break;
      }
    }

    if (!old_field)
      continue;

    bool old_has_default= !(old_field->flags & NO_DEFAULT_VALUE_FLAG);
    bool new_has_default= !(new_field->flags & NO_DEFAULT_VALUE_FLAG);

    if (old_has_default && !new_has_default)
    {
      /* Default was dropped */
      m_columns_to_drop_default.emplace_back(nullptr, new_field);
    }
    else if (new_has_default && !old_has_default)
    {
      /* Default was added */
      m_columns_to_set_default.emplace_back(nullptr, new_field);
    }
    else if (old_has_default && new_has_default)
    {
      /* Both have default — check if value changed */
      my_ptrdiff_t old_off=
          old_field->table->s->default_values - old_field->table->record[0];
      my_ptrdiff_t new_off=
          new_field->table->s->default_values - new_field->table->record[0];

      old_field->move_field_offset(old_off);
      new_field->move_field_offset(new_off);

      bool changed= false;
      bool new_is_null= new_field->is_null();
      if (old_field->is_null() != new_is_null)
        changed= true;
      else if (!old_field->is_null())
      {
        char buf1[MAX_FIELD_WIDTH], buf2[MAX_FIELD_WIDTH];
        String s1(buf1, sizeof(buf1), system_charset_info);
        String s2(buf2, sizeof(buf2), system_charset_info);
        String *v1= old_field->val_str(&s1);
        String *v2= new_field->val_str(&s2);
        if (v1 && v2)
          changed= sortcmp(v1, v2, system_charset_info) != 0;
        else
          changed= (v1 != v2);
      }

      old_field->move_field_offset(-old_off);
      new_field->move_field_offset(-new_off);

      if (changed)
      {
        /*
          If the new default is NULL in the default record, treat it
          as DROP DEFAULT for DuckDB. MariaDB does not set
          NO_DEFAULT_VALUE_FLAG for nullable columns on DROP DEFAULT,
          but the semantic is "no explicit default".
        */
        if (new_is_null)
          m_columns_to_drop_default.emplace_back(nullptr, new_field);
        else
          m_columns_to_set_default.emplace_back(nullptr, new_field);
      }
    }
  }
}

std::string ChangeColumnDefaultConvertor::translate()
{
  std::ostringstream result;

  /* Drop default value. */
  for (auto &pair : m_columns_to_drop_default)
  {
    Field *field= pair.second;
    append_stmt_column_drop_default(result, m_schema_name, m_table_name,
                                    field->field_name.str);
  }

  /* Set default value. */
  for (auto &pair : m_columns_to_set_default)
  {
    Field *field= pair.second;

    my_ptrdiff_t offset=
        field->table->s->default_values - field->table->record[0];
    std::string default_value= get_field_default_for_duckdb(field, offset);

    append_stmt_column_set_default(result, m_schema_name, m_table_name,
                                   field->field_name.str, default_value);
  }

  return result.str();
}

/* ----- ChangeColumnConvertor ----- */

void ChangeColumnConvertor::prepare_columns()
{
  List_iterator<Create_field> new_field_it(m_alter_info->create_list);
  Create_field *new_field;
  Field *field;

  while ((new_field= new_field_it++))
  {
    if (new_field->change.str == nullptr)
      continue;

    field= new_field->field;
    Field *cur_field= find_field(new_field, m_new_table);

    bool type_changed= is_type_changed(new_field, field);
    bool nullable_changed= is_nullable_change(new_field, field);
    bool name_changed= is_name_changed(new_field, field);

    /* Change type. */
    if (type_changed)
      m_columns_to_change_type.emplace_back(new_field, cur_field);

    /* Change nullable. */
    if (nullable_changed)
    {
      if ((new_field->flags & NOT_NULL_FLAG) != 0)
        m_columns_to_set_not_null.emplace_back(new_field, cur_field);
      else
        m_columns_to_drop_not_null.emplace_back(new_field, cur_field);
    }

    /* Change name. */
    if (name_changed)
    {
      assert(field->flags & FIELD_IS_RENAMED);
      m_columns_to_rename.emplace_back(new_field, cur_field);
    }

    /* All columns will be saved here. */
    m_columns.emplace_back(new_field, cur_field);
  }
}

bool ChangeColumnConvertor::check()
{
  for (auto &pair : m_columns)
  {
    Field *new_field= pair.second;
    if (FieldConvertor(new_field).check())
      return true;
  }
  return false;
}

std::string ChangeColumnConvertor::translate()
{
  std::ostringstream result;

  /* Rename column. */
  for (auto &pair : m_columns_to_rename)
  {
    Create_field *new_field= pair.first;
    Field *old_field= new_field->field;
    assert(old_field->flags & FIELD_IS_RENAMED);
    append_stmt_column_rename(result, m_schema_name, m_table_name,
                              old_field->field_name.str,
                              new_field->field_name.str);
  }

  /* Change type. */
  for (auto &pair : m_columns_to_change_type)
  {
    Field *field= pair.second;
    std::string new_type= FieldConvertor::convert_type(field);
    append_stmt_column_change_type(result, m_schema_name, m_table_name,
                                   field->field_name.str, new_type);
  }

  /* Change default value. All columns should be processed. */
  for (auto &pair : m_columns)
  {
    Create_field *new_field= pair.first;
    Field *field= pair.second;
    bool drop_default= ((new_field->flags & NO_DEFAULT_VALUE_FLAG) != 0);

    /* Drop default value. */
    if (drop_default)
    {
      append_stmt_column_drop_default(result, m_schema_name, m_table_name,
                                      new_field->field_name.str);
      continue;
    }

    if (!field || (field->flags & NO_DEFAULT_VALUE_FLAG))
      continue;

    my_ptrdiff_t offset=
        field->table->s->default_values - field->table->record[0];
    std::string default_value= get_field_default_for_duckdb(field, offset);

    append_stmt_column_set_default(result, m_schema_name, m_table_name,
                                   new_field->field_name.str, default_value);
  }

  for (auto &pair : m_columns_to_drop_not_null)
  {
    Create_field *new_field= pair.first;
    append_stmt_column_drop_not_null(result, m_schema_name, m_table_name,
                                     new_field->field_name.str);
  }

  for (auto &pair : m_columns_to_set_not_null)
  {
    Create_field *new_field= pair.first;
    assert((new_field->flags & NOT_NULL_FLAG) != 0);
    append_stmt_column_set_not_null(result, m_schema_name, m_table_name,
                                    new_field->field_name.str);
  }

  return result.str();
}

/* ----- ChangeColumnForPrimaryKeyConvertor ----- */

std::string ChangeColumnForPrimaryKeyConvertor::translate()
{
  std::ostringstream result;
  for (auto field : m_columns_to_set_not_null)
  {
    assert(field->flags & PRI_KEY_FLAG);
    assert(field->flags & NOT_NULL_FLAG);
    append_stmt_column_set_not_null(result, m_schema_name, m_table_name,
                                    field->field_name.str);
  }
  return result.str();
}

void ChangeColumnForPrimaryKeyConvertor::prepare_columns()
{
  Field **first_field= m_new_table->field;
  Field **ptr, *cur_field;
  for (ptr= first_field; (cur_field= *ptr); ptr++)
  {
    if (!(cur_field->flags & PRI_KEY_FLAG))
      continue;
    if (!(cur_field->flags & NOT_NULL_FLAG))
      continue;
    m_columns_to_set_not_null.push_back(cur_field);
  }
}

/* ----- Utility ----- */

std::string toHex(const char *data, size_t length)
{
  std::stringstream ss;
  ss << "'";
  for (size_t i= 0; i < length; ++i)
  {
    ss << "\\x" << std::hex << std::uppercase << std::setw(2)
       << std::setfill('0')
       << static_cast<int>(static_cast<unsigned char>(data[i]));
  }
  ss << "'::BLOB";
  return ss.str();
}
