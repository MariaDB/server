#define MYSQL_SERVER 1

#include "ha_parquet_pushdown.h"

#include "parquet_cross_engine_scan.h"
#include "parquet_metadata.h"
#include "parquet_shared.h"

#include "field.h"
#include "log.h"
#include "my_time.h"
#include "sql_select.h"
#include "sql_time.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>

namespace {

std::string quote_identifier(const std::string &identifier)
{
  std::string quoted = "\"";
  for (char ch : identifier) {
    if (ch == '"')
      quoted += "\"\"";
    else
      quoted += ch;
  }
  quoted += "\"";
  return quoted;
}

std::string mariadb_type_to_duckdb(Field *f)
{
  switch (f->type()) {
    case MYSQL_TYPE_TINY:
      return "TINYINT";
    case MYSQL_TYPE_SHORT:
      return "SMALLINT";
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      return "INTEGER";
    case MYSQL_TYPE_LONGLONG:
      return "BIGINT";
    case MYSQL_TYPE_FLOAT:
      return "FLOAT";
    case MYSQL_TYPE_DOUBLE:
      return "DOUBLE";
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      return "DECIMAL";
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      return "VARCHAR";
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      return (f->charset() == &my_charset_bin) ? "BLOB" : "VARCHAR";
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      return "DATE";
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
      return "TIME";
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
      return "TIMESTAMP";
    case MYSQL_TYPE_YEAR:
      return "SMALLINT";
    case MYSQL_TYPE_BIT:
      return "BOOLEAN";
    default:
      return "";
  }
}

std::string build_empty_view_query(TABLE_LIST *tbl)
{
  std::string query = "CREATE OR REPLACE TEMP VIEW " +
                      quote_identifier(tbl->table_name.str) + " AS SELECT ";
  bool first = true;

  for (Field **field = tbl->table->field; *field; ++field) {
    Field *f = *field;
    const std::string duck_type = mariadb_type_to_duckdb(f);
    if (duck_type.empty())
      return "";

    if (!first)
      query += ", ";
    first = false;
    query += "CAST(NULL AS " + duck_type + ") AS " +
             quote_identifier(f->field_name.str);
  }

  query += " WHERE FALSE";
  return query;
}

std::string build_read_view_query(TABLE_LIST *tbl,
                                  const std::vector<std::string> &s3_files)
{
  std::string parquet_file_list = "[";
  for (size_t i = 0; i < s3_files.size(); ++i) {
    if (i != 0)
      parquet_file_list += ", ";
    parquet_file_list += quote_string_literal(s3_files[i]);
  }
  parquet_file_list += "]";

  return "CREATE OR REPLACE TEMP VIEW " + quote_identifier(tbl->table_name.str) +
         " AS SELECT * FROM read_parquet(" + parquet_file_list + ")";
}

void store_field_temporal_value(Field *field, MYSQL_TIME *ltime)
{
  field->store_time(ltime);
}

void store_duckdb_field_in_mysql_format(Field *field, duckdb::Value &value,
                                        THD *thd)
{
  (void) thd;
  if (value.IsNull()) {
    field->set_default();
    if (field->real_maybe_null())
      field->set_null();
    return;
  }

  field->set_notnull();
  switch (field->type()) {
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_BIT: {
      auto str = value.GetValueUnsafe<duckdb::string>();
      field->store(str.c_str(), str.size(), &my_charset_bin);
      break;
    }
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING: {
      auto str = value.GetValue<duckdb::string>();
      field->store(str.c_str(), str.size(),
                   field->has_charset() ? field->charset() : &my_charset_bin);
      break;
    }
    case MYSQL_TYPE_NULL:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_NEWDECIMAL: {
      auto str = value.GetValue<duckdb::string>();
      field->store(str.c_str(), str.size(), system_charset_info);
      break;
    }
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG: {
      int64_t v = value.GetValue<int64_t>();
      field->store(v, field->is_unsigned());
      break;
    }
    case MYSQL_TYPE_LONGLONG: {
      if (field->is_unsigned())
        field->store(value.GetValue<uint64_t>(), true);
      else
        field->store(value.GetValue<int64_t>(), false);
      break;
    }
    case MYSQL_TYPE_FLOAT:
      field->store(value.GetValue<float>());
      break;
    case MYSQL_TYPE_DOUBLE:
      field->store(value.GetValue<double>());
      break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      auto str = value.GetValue<duckdb::string>();
      MYSQL_TIME tm;
      MYSQL_TIME_STATUS status;
      my_time_status_init(&status);
      str_to_datetime_or_date(str.c_str(), str.size(), &tm, 0, &status);
      store_field_temporal_value(field, &tm);
      break;
    }
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2: {
      auto str = value.GetValue<duckdb::string>();
      MYSQL_TIME tm;
      MYSQL_TIME_STATUS status;
      my_time_status_init(&status);
      str_to_DDhhmmssff(str.c_str(), str.size(), &tm, TIME_MAX_HOUR, &status);
      store_field_temporal_value(field, &tm);
      break;
    }
    default: {
      auto str = value.GetValue<duckdb::string>();
      field->store(str.c_str(), str.size(), system_charset_info);
      break;
    }
  }
}

std::string describe_tables(const std::vector<TABLE_LIST *> &tables)
{
  std::string description;
  for (size_t i = 0; i < tables.size(); ++i) {
    if (i != 0)
      description += ", ";

    TABLE_LIST *tbl = tables[i];
    description += tbl && tbl->table_name.str ? tbl->table_name.str : "<unknown>";
    if (tbl && tbl->alias.str && std::strcmp(tbl->alias.str, tbl->table_name.str) != 0) {
      description += " AS ";
      description += tbl->alias.str;
    }
  }
  return description;
}

bool duckdb_configs_compatible(const parquet::ObjectStoreConfig &left,
                               const parquet::ObjectStoreConfig &right)
{
  return left.endpoint == right.endpoint && left.region == right.region &&
         left.url_style == right.url_style &&
         left.auth_mode == right.auth_mode &&
         left.credentials.access_key_id == right.credentials.access_key_id &&
         left.credentials.secret_access_key ==
             right.credentials.secret_access_key &&
         left.credentials.session_token == right.credentials.session_token;
}

bool can_pushdown_to_parquet(SELECT_LEX *sel_lex,
                             std::vector<TABLE_LIST *> &parquet_tables,
                             std::vector<TABLE_LIST *> &external_tables)
{
  std::unordered_set<std::string> seen_parquet_names;
  std::unordered_set<std::string> seen_external_names;
  bool has_parquet_table = false;

  for (TABLE_LIST *tbl = sel_lex->get_table_list(); tbl; tbl = tbl->next_global) {
    if (tbl->derived || !tbl->table || !tbl->table->file)
      return false;

    const std::string table_name(tbl->table_name.str);
    if (tbl->table->file->ht == parquet_hton) {
      has_parquet_table = true;
      if (seen_parquet_names.insert(table_name).second)
        parquet_tables.push_back(tbl);
    } else if (seen_external_names.insert(table_name).second) {
      external_tables.push_back(tbl);
    }
  }

  return has_parquet_table;
}

void register_external_table_names(TABLE_LIST *tbl)
{
  if (!tbl || !tbl->table)
    return;

  std::unordered_set<std::string> registered_names;
  auto register_name = [&](const char *name) {
    if (!name || !name[0])
      return;

    const std::string key(name);
    if (registered_names.insert(key).second)
      myparquet::register_external_table(key, tbl->table);
  };

  register_name(tbl->table_name.str);
  register_name(tbl->alias.str);

  if (tbl->db.str && tbl->db.str[0]) {
    const std::string qualified_name =
        std::string(tbl->db.str) + "." + tbl->table_name.str;
    if (registered_names.insert(qualified_name).second)
      myparquet::register_external_table(qualified_name, tbl->table);

    if (tbl->alias.str && std::strcmp(tbl->alias.str, tbl->table_name.str) != 0) {
      const std::string qualified_alias =
          std::string(tbl->db.str) + "." + tbl->alias.str;
      if (registered_names.insert(qualified_alias).second)
        myparquet::register_external_table(qualified_alias, tbl->table);
    }
  }
}

} // namespace

ha_parquet_select_handler::ha_parquet_select_handler(THD *thd_arg,
                                                     SELECT_LEX *sel_lex,
                                                     SELECT_LEX_UNIT *sel_unit)
    : select_handler(thd_arg, parquet_hton, sel_lex, sel_unit),
      current_row_index(0),
      has_cross_engine(false),
      query_string(thd_arg->charset())
{
  query_string.length(0);
  query_string.append(thd_arg->query(), thd_arg->query_length());
}

ha_parquet_select_handler::~ha_parquet_select_handler() = default;

void ha_parquet_select_handler::set_parquet_tables(std::vector<TABLE_LIST *> &&tables)
{
  parquet_tables = std::move(tables);
}

void ha_parquet_select_handler::set_cross_engine(std::vector<TABLE_LIST *> &&tables)
{
  has_cross_engine = !tables.empty();
  external_tables = std::move(tables);
}

int ha_parquet_select_handler::init_scan()
{
  DBUG_ENTER("ha_parquet_select_handler::init_scan");

  duckdb_db = std::make_unique<duckdb::DuckDB>(nullptr);
  myparquet::register_cross_engine_scan(*duckdb_db->instance);
  duckdb_con = std::make_unique<duckdb::Connection>(*duckdb_db);
  parquet_log_info("DuckDB select handler initialized");
  query_result.reset();
  current_chunk.reset();
  current_row_index = 0;

  auto cleanup_external_registry = [&]() {
    if (has_cross_engine)
      myparquet::clear_external_tables();
  };

  parquet_log_info("DuckDB query [select/install-parquet] INSTALL parquet;");
  duckdb_con->Query("INSTALL parquet;");
  parquet_log_info("DuckDB query [select/load-parquet] LOAD parquet;");
  duckdb_con->Query("LOAD parquet;");
  parquet_log_info("DuckDB query [select/install-httpfs] INSTALL httpfs;");
  duckdb_con->Query("INSTALL httpfs;");
  parquet_log_info("DuckDB query [select/load-httpfs] LOAD httpfs;");
  duckdb_con->Query("LOAD httpfs;");

  parquet::ObjectStoreConfig scan_object_store_config;
  bool have_scan_object_store_config = false;

  for (TABLE_LIST *tbl : parquet_tables) {
    parquet::TableMetadata metadata;
    std::string error_message;
    if (!parquet::ResolveRuntimeTableMetadata(
            tbl->table->s->normalized_path.str, &metadata, &error_message) ||
        !parquet::ValidateCatalogConfig(metadata, &error_message) ||
        !parquet::ValidateObjectStoreConfig(metadata, true, &error_message)) {
      cleanup_external_registry();
      my_error(ER_UNKNOWN_ERROR, MYF(0), error_message.c_str());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    if (!have_scan_object_store_config) {
      scan_object_store_config = metadata.object_store_config;
      have_scan_object_store_config = true;
    } else if (!duckdb_configs_compatible(scan_object_store_config,
                                          metadata.object_store_config)) {
      cleanup_external_registry();
      my_error(ER_UNKNOWN_ERROR, MYF(0),
               "Parquet pushdown currently requires matching object-store "
               "credentials and endpoint settings across Parquet tables");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    if (!configure_duckdb_s3(duckdb_con.get(), metadata.object_store_config,
                             &error_message)) {
      cleanup_external_registry();
      my_error(ER_UNKNOWN_ERROR, MYF(0), error_message.c_str());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    std::vector<std::string> s3_files;
    long http_code = 0;
    if (!resolve_parquet_data_files(metadata, &s3_files, &http_code)) {
      cleanup_external_registry();
      my_error(ER_UNKNOWN_ERROR, MYF(0),
               "Failed to fetch LakeKeeper metadata for Parquet pushdown");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    if (http_code != 200) {
      cleanup_external_registry();
      my_error(ER_UNKNOWN_ERROR, MYF(0),
               "LakeKeeper returned a non-200 response for Parquet pushdown");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    const std::string create_view_sql = s3_files.empty()
        ? build_empty_view_query(tbl)
        : build_read_view_query(tbl, s3_files);
    if (create_view_sql.empty()) {
      cleanup_external_registry();
      my_error(ER_UNKNOWN_ERROR, MYF(0),
               "Parquet pushdown could not map the table schema to DuckDB");
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }

    parquet_log_info("DuckDB query [select/create-parquet-view] " +
                     parquet_log_preview(create_view_sql));
    auto create_view_result = duckdb_con->Query(create_view_sql);
    if (!create_view_result || create_view_result->HasError()) {
      cleanup_external_registry();
      const std::string error_message =
          create_view_result ? create_view_result->GetError()
                             : "DuckDB returned a null result while creating a Parquet view";
      my_error(ER_UNKNOWN_ERROR, MYF(0), error_message.c_str());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
  }

  if (have_scan_object_store_config) {
    std::string error_message;
    if (!configure_duckdb_s3(duckdb_con.get(), scan_object_store_config,
                             &error_message)) {
      cleanup_external_registry();
      my_error(ER_UNKNOWN_ERROR, MYF(0), error_message.c_str());
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
  }

  if (has_cross_engine) {
    for (TABLE_LIST *tbl : external_tables)
      register_external_table_names(tbl);

    const std::string parquet_desc = describe_tables(parquet_tables);
    const std::string external_desc = describe_tables(external_tables);
    sql_print_information(
        "Parquet: cross-engine pushdown init parquet_tables=[%s] external_tables=[%s]",
        parquet_desc.c_str(), external_desc.c_str());
  }

  std::string sql(query_string.ptr(), query_string.length());
  parquet_log_info("DuckDB query [select/execute] " +
                   parquet_log_preview(sql));
  query_result = duckdb_con->Query(sql);

  if (!query_result || query_result->HasError()) {
    cleanup_external_registry();
    const std::string error_message =
        query_result ? query_result->GetError()
                     : "DuckDB returned a null result for Parquet pushdown";
    my_error(ER_UNKNOWN_ERROR, MYF(0), error_message.c_str());
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  DBUG_RETURN(0);
}

int ha_parquet_select_handler::next_row()
{
  DBUG_ENTER("ha_parquet_select_handler::next_row");

  if (!query_result)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  if (!current_chunk || current_row_index >= current_chunk->size()) {
    current_chunk.reset();
    current_chunk = query_result->Fetch();

    if (!current_chunk || current_chunk->size() == 0)
      DBUG_RETURN(HA_ERR_END_OF_FILE);

    current_row_index = 0;
  }

  size_t col_count = current_chunk->ColumnCount();
  size_t field_count = 0;
  for (Field **f = table->field; *f; ++f)
    field_count++;

  size_t ncols = (col_count < field_count) ? col_count : field_count;
  for (size_t col_idx = 0; col_idx < ncols; ++col_idx) {
    duckdb::Value value = current_chunk->GetValue(col_idx, current_row_index);
    Field *field = table->field[col_idx];
    store_duckdb_field_in_mysql_format(field, value, thd);
  }

  current_row_index++;
  DBUG_RETURN(0);
}

int ha_parquet_select_handler::end_scan()
{
  DBUG_ENTER("ha_parquet_select_handler::end_scan");

  if (has_cross_engine)
    myparquet::clear_external_tables();

  current_chunk.reset();
  query_result.reset();
  duckdb_con.reset();
  duckdb_db.reset();
  parquet_log_info("DuckDB select handler deinitialized");
  current_row_index = 0;
  has_cross_engine = false;
  external_tables.clear();

  if (table) {
    free_tmp_table(thd, table);
    table = 0;
  }

  DBUG_RETURN(0);
}

select_handler *create_duckdb_select_handler(THD *thd,
                                             SELECT_LEX *sel_lex,
                                             SELECT_LEX_UNIT *sel_unit)
{
  if (!thd || !sel_lex)
    return nullptr;

  if (thd->lex->sql_command != SQLCOM_SELECT)
    return nullptr;

  if (thd->stmt_arena && thd->stmt_arena->is_stmt_prepare())
    return nullptr;

  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  if (sel_lex->master_unit() && sel_lex->master_unit()->is_unit_op())
    return nullptr;

  std::vector<TABLE_LIST *> parquet_tables;
  std::vector<TABLE_LIST *> external_tables;
  if (!can_pushdown_to_parquet(sel_lex, parquet_tables, external_tables))
    return nullptr;

  auto *handler = new ha_parquet_select_handler(thd, sel_lex, sel_unit);
  handler->set_parquet_tables(std::move(parquet_tables));
  if (!external_tables.empty()) {
    handler->set_cross_engine(std::move(external_tables));
    sql_print_information("Parquet: selected cross-engine pushdown handler");
  }
  return handler;
}
