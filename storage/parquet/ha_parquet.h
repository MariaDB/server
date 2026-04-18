#ifndef HA_PARQUET_INCLUDED
#define HA_PARQUET_INCLUDED


#define MYSQL_SERVER 1


#include "handler.h"
#include "thr_lock.h"
#include "my_base.h"
#include "duckdb.hpp"


#include <vector>
#include <string>




struct parquet_trx_data {
 std::vector<std::string> s3_file_paths;
 std::vector<int64_t>     row_counts;
 std::vector<int64_t>     file_sizes;
 std::string table_name;
};


class ha_parquet final : public handler
{
public:
 ha_parquet(handlerton *hton, TABLE_SHARE *table_arg);


 ~ha_parquet() override = default;


 ulonglong table_flags() const override;
 ulong index_flags(uint idx, uint part, bool all_parts) const override;


  int open(const char *name, int mode, uint test_if_locked) override;
 int close(void) override;
 int create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info) override;




 int write_row(const uchar *buf) override;
 int update_row(const uchar *old_data, const uchar *new_data) override;
 int delete_row(const uchar *buf) override;
  int rnd_init(bool scan) override;
 int rnd_next(uchar *buf) override;
 int rnd_pos(uchar *buf, uchar *pos) override;
 void position(const uchar *record) override;
 int info(uint flag) override;


 enum_alter_inplace_result check_if_supported_inplace_alter(TABLE *altered_table, Alter_inplace_info *ha_alter_info) override;


 int external_lock(THD *thd, int lock_type) override;


 THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                            enum thr_lock_type lock_type) override;
 const Item *cond_push(const Item *cond) override;
 void cond_pop() override;


private:
 THR_LOCK_DATA lock;
 std::string helper_db_path;
 std::string parquet_file_path;


 uint64_t row_count;
 uint64_t flush_threshold;
 bool duckdb_initialized;
 bool buffer_table_created = false;
  duckdb::DuckDB *db = nullptr;
 duckdb::Connection *con = nullptr;


 void flush_remaining_rows_to_s3(parquet_trx_data *trx);


 std::string pushed_cond_sql;
 bool has_pushed_cond = false;


 std::unique_ptr<duckdb::MaterializedQueryResult> scan_result;
 size_t current_row = 0;




};
#endif



