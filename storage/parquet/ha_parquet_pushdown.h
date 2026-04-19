#ifndef HA_PARQUET_PUSHDOWN_INCLUDED
#define HA_PARQUET_PUSHDOWN_INCLUDED

#define MYSQL_SERVER 1

#include "my_global.h"
#include "sql_class.h"
#include "select_handler.h"

#undef UNKNOWN

#include "duckdb.hpp"

#include <memory>
#include <string>
#include <vector>

extern handlerton *parquet_hton;

class ha_parquet_select_handler : public select_handler
{
public:
  ha_parquet_select_handler(THD *thd_arg, SELECT_LEX *sel_lex,
                            SELECT_LEX_UNIT *sel_unit);
  ~ha_parquet_select_handler() override;

  void set_parquet_tables(std::vector<TABLE_LIST *> &&tables);
  void set_cross_engine(std::vector<TABLE_LIST *> &&tables);

protected:
  int init_scan() override;
  int next_row() override;
  int end_scan() override;

private:
  std::unique_ptr<duckdb::DuckDB> duckdb_db;
  std::unique_ptr<duckdb::Connection> duckdb_con;
  std::unique_ptr<duckdb::QueryResult> query_result;
  std::unique_ptr<duckdb::DataChunk> current_chunk;
  size_t current_row_index;
  bool has_cross_engine;

  StringBuffer<4096> query_string;
  std::vector<TABLE_LIST *> parquet_tables;
  std::vector<TABLE_LIST *> external_tables;
};

select_handler *create_duckdb_select_handler(THD *thd,
                                             SELECT_LEX *sel_lex,
                                             SELECT_LEX_UNIT *sel_unit);

#endif
