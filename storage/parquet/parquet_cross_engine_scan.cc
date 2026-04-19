#define MYSQL_SERVER 1

#include <my_global.h>
#include "sql_class.h"
#include "field.h"
#include "handler.h"
#include "log.h"
#include "mysqld.h"

#undef UNKNOWN

#include "parquet_cross_engine_scan.h"

#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include <unordered_map>

namespace myparquet
{

static thread_local std::unordered_map<std::string, TABLE *> tls_external_tables;

void register_external_table(const std::string &name, TABLE *table)
{
  tls_external_tables[name] = table;
  sql_print_information("Parquet: cross-engine registry add key='%s'",
                        name.c_str());
}

void clear_external_tables()
{
  if (!tls_external_tables.empty()) {
    sql_print_information("Parquet: clearing cross-engine registry (%zu entries)",
                          tls_external_tables.size());
  }
  tls_external_tables.clear();
}

TABLE *find_external_table(const std::string &name)
{
  auto it = tls_external_tables.find(name);
  if (it != tls_external_tables.end())
    return it->second;
  return nullptr;
}

static duckdb::LogicalType field_to_logical_type(const Field *field)
{
  const bool is_unsigned = (field->flags & UNSIGNED_FLAG) != 0;

  switch (field->real_type()) {
    case MYSQL_TYPE_TINY:
      return is_unsigned ? duckdb::LogicalType::UTINYINT
                         : duckdb::LogicalType::TINYINT;
    case MYSQL_TYPE_SHORT:
      return is_unsigned ? duckdb::LogicalType::USMALLINT
                         : duckdb::LogicalType::SMALLINT;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      return is_unsigned ? duckdb::LogicalType::UINTEGER
                         : duckdb::LogicalType::INTEGER;
    case MYSQL_TYPE_LONGLONG:
      return is_unsigned ? duckdb::LogicalType::UBIGINT
                         : duckdb::LogicalType::BIGINT;
    case MYSQL_TYPE_FLOAT:
      return duckdb::LogicalType::FLOAT;
    case MYSQL_TYPE_DOUBLE:
      return duckdb::LogicalType::DOUBLE;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      auto *df = static_cast<const Field_new_decimal *>(field);
      uint prec = df->precision > 38 ? 38 : df->precision;
      return duckdb::LogicalType::DECIMAL(prec, df->dec);
    }
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      return duckdb::LogicalType::DATE;
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
      return duckdb::LogicalType::TIME;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
      return duckdb::LogicalType::TIMESTAMP;
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
      return duckdb::LogicalType::TIMESTAMP_TZ;
    case MYSQL_TYPE_YEAR:
      return duckdb::LogicalType::INTEGER;
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY:
      return duckdb::LogicalType::BLOB;
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      return field->has_charset() ? duckdb::LogicalType::VARCHAR
                                  : duckdb::LogicalType::BLOB;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_ENUM:
      return duckdb::LogicalType::VARCHAR;
    default:
      return duckdb::LogicalType::VARCHAR;
  }
}

static duckdb::Value field_to_duckdb_value(Field *field)
{
  if (field->is_null())
    return duckdb::Value();

  switch (field->real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
      if (field->is_unsigned())
        return duckdb::Value::UBIGINT(field->val_uint());
      return duckdb::Value::BIGINT(field->val_int());
    case MYSQL_TYPE_FLOAT:
      return duckdb::Value::FLOAT(static_cast<float>(field->val_real()));
    case MYSQL_TYPE_DOUBLE:
      return duckdb::Value::DOUBLE(field->val_real());
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
    default: {
      String buf;
      field->val_str(&buf);
      return duckdb::Value(std::string(buf.ptr(), buf.length()));
    }
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY: {
      String buf;
      field->val_str(&buf);
      return duckdb::Value::BLOB(std::string(buf.ptr(), buf.length()));
    }
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: {
      String buf;
      field->val_str(&buf);
      if (field->has_charset())
        return duckdb::Value(std::string(buf.ptr(), buf.length()));
      return duckdb::Value::BLOB(std::string(buf.ptr(), buf.length()));
    }
  }
}

struct MdbScanBindData : duckdb::FunctionData
{
  std::string table_key;
  TABLE *table = nullptr;

  duckdb::unique_ptr<duckdb::FunctionData> Copy() const override
  {
    auto copy = duckdb::make_uniq<MdbScanBindData>();
    copy->table_key = table_key;
    copy->table = table;
    return copy;
  }

  bool Equals(const duckdb::FunctionData &other) const override
  {
    return table_key == other.Cast<MdbScanBindData>().table_key;
  }
};

struct MdbScanGlobalState : duckdb::GlobalTableFunctionState
{
  bool scan_started = false;
  bool finished = false;
  TABLE *table = nullptr;
  std::string table_key;
  duckdb::unique_ptr<duckdb::TableFilterSet> filters;
  duckdb::vector<duckdb::idx_t> column_ids;
  duckdb::idx_t rows_scanned = 0;
  duckdb::idx_t rows_emitted = 0;
  duckdb::idx_t rows_filtered = 0;
  bool completion_logged = false;

  idx_t MaxThreads() const override { return 1; }
};

static bool cast_value_for_filter(const duckdb::Value &source,
                                  const duckdb::LogicalType &target_type,
                                  duckdb::Value *cast_value)
{
  if (!cast_value)
    return false;

  if (source.IsNull()) {
    *cast_value = duckdb::Value(target_type);
    return true;
  }

  if (source.type() == target_type) {
    *cast_value = source;
    return true;
  }

  std::string error_message;
  return source.DefaultTryCastAs(target_type, *cast_value, &error_message);
}

static bool evaluate_filter_on_value(const duckdb::TableFilter &filter,
                                     const duckdb::Value &row_value);

static bool evaluate_dynamic_filter(const duckdb::DynamicFilter &filter,
                                    const duckdb::Value &row_value)
{
  if (!filter.filter_data)
    return true;

  duckdb::lock_guard<duckdb::mutex> guard(filter.filter_data->lock);
  if (!filter.filter_data->initialized || !filter.filter_data->filter)
    return true;

  return evaluate_filter_on_value(*filter.filter_data->filter, row_value);
}

static bool evaluate_in_filter(const duckdb::InFilter &filter,
                               const duckdb::Value &row_value)
{
  if (row_value.IsNull() || filter.values.empty())
    return false;

  duckdb::Value comparable_value;
  if (!cast_value_for_filter(row_value, filter.values[0].type(), &comparable_value))
    return false;

  for (const auto &candidate : filter.values) {
    if (duckdb::Value::NotDistinctFrom(comparable_value, candidate))
      return true;
  }
  return false;
}

static bool evaluate_filter_on_value(const duckdb::TableFilter &filter,
                                     const duckdb::Value &row_value)
{
  switch (filter.filter_type) {
    case duckdb::TableFilterType::CONSTANT_COMPARISON: {
      const auto &constant_filter = filter.Cast<duckdb::ConstantFilter>();
      if (row_value.IsNull())
        return false;

      duckdb::Value comparable_value;
      if (!cast_value_for_filter(row_value, constant_filter.constant.type(),
                                 &comparable_value)) {
        return false;
      }
      return constant_filter.Compare(comparable_value);
    }
    case duckdb::TableFilterType::IS_NULL:
      return row_value.IsNull();
    case duckdb::TableFilterType::IS_NOT_NULL:
      return !row_value.IsNull();
    case duckdb::TableFilterType::CONJUNCTION_AND: {
      const auto &and_filter = filter.Cast<duckdb::ConjunctionAndFilter>();
      for (const auto &child : and_filter.child_filters) {
        if (child && !evaluate_filter_on_value(*child, row_value))
          return false;
      }
      return true;
    }
    case duckdb::TableFilterType::CONJUNCTION_OR: {
      const auto &or_filter = filter.Cast<duckdb::ConjunctionOrFilter>();
      for (const auto &child : or_filter.child_filters) {
        if (child && evaluate_filter_on_value(*child, row_value))
          return true;
      }
      return false;
    }
    case duckdb::TableFilterType::OPTIONAL_FILTER: {
      const auto &optional_filter = filter.Cast<duckdb::OptionalFilter>();
      return !optional_filter.child_filter ||
             evaluate_filter_on_value(*optional_filter.child_filter, row_value);
    }
    case duckdb::TableFilterType::IN_FILTER:
      return evaluate_in_filter(filter.Cast<duckdb::InFilter>(), row_value);
    case duckdb::TableFilterType::DYNAMIC_FILTER:
      return evaluate_dynamic_filter(filter.Cast<duckdb::DynamicFilter>(), row_value);
    default:
      throw duckdb::NotImplementedException(
          "Parquet cross-engine filter pushdown does not support filter type %d",
          static_cast<int>(filter.filter_type));
  }
}

static bool row_passes_filters(TABLE *tbl, const duckdb::TableFilterSet &filters)
{
  for (const auto &entry : filters.filters) {
    if (entry.first >= tbl->s->fields)
      return false;

    Field *field = tbl->field[entry.first];
    duckdb::Value row_value = field_to_duckdb_value(field);
    if (!evaluate_filter_on_value(*entry.second, row_value))
      return false;
  }
  return true;
}

static std::string describe_filter_set(TABLE *tbl,
                                       const duckdb::TableFilterSet &filters)
{
  std::string description;
  for (const auto &entry : filters.filters) {
    if (!description.empty())
      description += ", ";

    const char *field_name =
        (tbl && entry.first < tbl->s->fields && tbl->field[entry.first])
            ? tbl->field[entry.first]->field_name.str
            : "<unknown>";
    description += entry.second->ToString(field_name);
  }
  return description;
}

static duckdb::unique_ptr<duckdb::FunctionData>
mdb_scan_bind(duckdb::ClientContext &context,
              duckdb::TableFunctionBindInput &input,
              duckdb::vector<duckdb::LogicalType> &return_types,
              duckdb::vector<duckdb::string> &names)
{
  (void) context;
  auto key = input.inputs[0].GetValue<std::string>();

  TABLE *tbl = find_external_table(key);
  if (!tbl)
    throw duckdb::BinderException("_mdb_scan: table '%s' not found in external table registry",
                                  key.c_str());

  for (Field **f = tbl->field; *f; ++f) {
    names.push_back((*f)->field_name.str);
    return_types.push_back(field_to_logical_type(*f));
  }

  sql_print_information("Parquet: _mdb_scan bind table='%s' columns=%zu",
                        key.c_str(), names.size());

  auto data = duckdb::make_uniq<MdbScanBindData>();
  data->table_key = key;
  data->table = tbl;
  return data;
}

static duckdb::unique_ptr<duckdb::GlobalTableFunctionState>
mdb_scan_init_global(duckdb::ClientContext &context,
                     duckdb::TableFunctionInitInput &input)
{
  (void) context;
  auto &bind_data = input.bind_data->Cast<MdbScanBindData>();
  auto state = duckdb::make_uniq<MdbScanGlobalState>();
  state->table = bind_data.table;
  state->table_key = bind_data.table_key;
  if (input.filters)
    state->filters = input.filters->Copy();
  state->column_ids = input.column_ids;
  return state;
}

static void mdb_scan_function(duckdb::ClientContext &context,
                              duckdb::TableFunctionInput &input,
                              duckdb::DataChunk &output)
{
  (void) context;
  auto &state = input.global_state->Cast<MdbScanGlobalState>();

  if (state.finished) {
    if (!state.completion_logged) {
      sql_print_information("Parquet: _mdb_scan complete table='%s' scanned=%llu emitted=%llu filtered=%llu",
                            state.table_key.c_str(),
                            static_cast<unsigned long long>(state.rows_scanned),
                            static_cast<unsigned long long>(state.rows_emitted),
                            static_cast<unsigned long long>(state.rows_filtered));
      state.completion_logged = true;
    }
    output.SetCardinality(0);
    return;
  }

  TABLE *tbl = state.table;
  if (!tbl) {
    output.SetCardinality(0);
    state.finished = true;
    if (!state.completion_logged) {
      sql_print_information("Parquet: _mdb_scan complete table='%s' scanned=%llu emitted=%llu filtered=%llu",
                            state.table_key.c_str(),
                            static_cast<unsigned long long>(state.rows_scanned),
                            static_cast<unsigned long long>(state.rows_emitted),
                            static_cast<unsigned long long>(state.rows_filtered));
      state.completion_logged = true;
    }
    return;
  }

  THD *prev_thd = _current_thd();
  if (tbl->in_use && tbl->in_use != prev_thd)
    set_current_thd(tbl->in_use);

  if (!state.scan_started) {
    sql_print_information("Parquet: _mdb_scan start table='%s' projected_columns=%zu",
                          state.table_key.c_str(), state.column_ids.size());
    if (state.filters && !state.filters->filters.empty()) {
      const std::string filter_desc = describe_filter_set(tbl, *state.filters);
      sql_print_information("Parquet: _mdb_scan pushed_filters table='%s' filters=[%s]",
                            state.table_key.c_str(), filter_desc.c_str());
    }
    bitmap_clear_all(tbl->read_set);
    for (auto col_idx : state.column_ids)
      bitmap_set_bit(tbl->read_set, static_cast<uint>(col_idx));

    if (tbl->file->ha_rnd_init(true)) {
      sql_print_warning("Parquet: _mdb_scan failed to initialize table='%s'",
                        state.table_key.c_str());
      state.finished = true;
      output.SetCardinality(0);
      if (!state.completion_logged) {
        sql_print_information("Parquet: _mdb_scan complete table='%s' scanned=%llu emitted=%llu filtered=%llu",
                              state.table_key.c_str(),
                              static_cast<unsigned long long>(state.rows_scanned),
                              static_cast<unsigned long long>(state.rows_emitted),
                              static_cast<unsigned long long>(state.rows_filtered));
        state.completion_logged = true;
      }
      if (_current_thd() != prev_thd)
        set_current_thd(prev_thd);
      return;
    }
    state.scan_started = true;
  }

  duckdb::idx_t count = 0;
  duckdb::idx_t ncols = state.column_ids.size();

  while (count < STANDARD_VECTOR_SIZE) {
    int err = tbl->file->ha_rnd_next(tbl->record[0]);
    if (err) {
      tbl->file->ha_rnd_end();
      state.finished = true;
      break;
    }

    state.rows_scanned++;

    if (state.filters && !state.filters->filters.empty() &&
        !row_passes_filters(tbl, *state.filters)) {
      state.rows_filtered++;
      continue;
    }

    for (duckdb::idx_t i = 0; i < ncols; ++i) {
      Field *field = tbl->field[state.column_ids[i]];
      duckdb::Value val = field_to_duckdb_value(field);
      output.data[i].SetValue(count, val);
    }
    count++;
  }

  state.rows_emitted += count;
  output.SetCardinality(count);

  if (state.finished && !state.completion_logged) {
    sql_print_information("Parquet: _mdb_scan complete table='%s' scanned=%llu emitted=%llu filtered=%llu",
                          state.table_key.c_str(),
                          static_cast<unsigned long long>(state.rows_scanned),
                          static_cast<unsigned long long>(state.rows_emitted),
                          static_cast<unsigned long long>(state.rows_filtered));
    state.completion_logged = true;
  }

  if (_current_thd() != prev_thd)
    set_current_thd(prev_thd);
}

duckdb::unique_ptr<duckdb::TableRef> mariadb_replacement_scan(
    duckdb::ClientContext &context, duckdb::ReplacementScanInput &input,
    duckdb::optional_ptr<duckdb::ReplacementScanData> data)
{
  (void) context;
  (void) data;
  sql_print_information("Parquet: replacement scan lookup schema='%s' table='%s'",
                        input.schema_name.c_str(), input.table_name.c_str());
  TABLE *tbl = find_external_table(input.table_name);
  if (!tbl)
    return nullptr;

  auto ref = duckdb::make_uniq<duckdb::TableFunctionRef>();

  duckdb::vector<duckdb::unique_ptr<duckdb::ParsedExpression>> children;
  children.push_back(duckdb::make_uniq<duckdb::ConstantExpression>(
      duckdb::Value(input.table_name)));

  ref->function = duckdb::make_uniq<duckdb::FunctionExpression>(
      "_mdb_scan", std::move(children));
  ref->alias = input.table_name;
  return ref;
}

void register_cross_engine_scan(duckdb::DatabaseInstance &db)
{
  duckdb::TableFunction mdb_scan("_mdb_scan", {duckdb::LogicalType::VARCHAR},
                                 mdb_scan_function, mdb_scan_bind,
                                 mdb_scan_init_global);
  mdb_scan.projection_pushdown = true;
  mdb_scan.filter_pushdown = true;

  duckdb::ExtensionUtil::RegisterFunction(db, std::move(mdb_scan));

  auto &config = duckdb::DBConfig::GetConfig(db);
  config.replacement_scans.emplace_back(mariadb_replacement_scan);
}

} // namespace myparquet
