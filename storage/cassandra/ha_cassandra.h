/*
   Copyright (c) 2012, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */
#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif


#include "my_global.h"                   /* ulonglong */
#include "thr_lock.h"                    /* THR_LOCK, THR_LOCK_DATA */
#include "handler.h"                     /* handler */
#include "my_base.h"                     /* ha_rows */

#include "cassandra_se.h"

/** @brief
  CASSANDRA_SHARE is a structure that will be shared among all open handlers.
  This example implements the minimum of what you will probably need.
*/
typedef struct st_cassandra_share {
  char *table_name;
  uint table_name_length,use_count;
  mysql_mutex_t mutex;
  THR_LOCK lock;
} CASSANDRA_SHARE;

class ColumnDataConverter;
struct st_dynamic_column_value;
typedef struct st_dynamic_column_value DYNAMIC_COLUMN_VALUE;

struct ha_table_option_struct;


struct st_dynamic_column_value;

typedef bool (* CAS2DYN_CONVERTER)(const char *cass_data,
                                   int cass_data_len,
                                   struct st_dynamic_column_value *value,
                                   MEM_ROOT *mem_root);
typedef bool (* DYN2CAS_CONVERTER)(struct st_dynamic_column_value *value,
                                   char **cass_data,
                                   int *cass_data_len,
                                   void *buf, void **freemem);
struct cassandra_type_def
{
  const char *name;
  CAS2DYN_CONVERTER cassandra_to_dynamic;
  DYN2CAS_CONVERTER dynamic_to_cassandra;
};

typedef struct cassandra_type_def CASSANDRA_TYPE_DEF;

enum cassandtra_type_enum {CT_BIGINT, CT_INT, CT_COUNTER, CT_FLOAT, CT_DOUBLE,
  CT_BLOB, CT_ASCII, CT_TEXT, CT_TIMESTAMP, CT_UUID, CT_BOOLEAN, CT_VARINT,
  CT_DECIMAL};

typedef enum cassandtra_type_enum CASSANDRA_TYPE;



/** @brief
  Class definition for the storage engine
*/
class ha_cassandra: public handler
{
  friend class Column_name_enumerator_impl;
  THR_LOCK_DATA lock;      ///< MySQL lock
  CASSANDRA_SHARE *share;    ///< Shared lock info

  Cassandra_se_interface *se;

  /* description of static part of the table definition */
  ColumnDataConverter **field_converters;
  uint n_field_converters;

  CASSANDRA_TYPE_DEF *default_type_def;
  /* description of dynamic columns part */
  CASSANDRA_TYPE_DEF *special_type_field_converters;
  LEX_STRING *special_type_field_names;
  uint n_special_type_fields;
  DYNAMIC_ARRAY dynamic_values, dynamic_names;
  DYNAMIC_STRING dynamic_rec;

  ColumnDataConverter *rowkey_converter;

  bool setup_field_converters(Field **field, uint n_fields);
  void free_field_converters();

  int read_cassandra_columns(bool unpack_pk);
  int check_table_options(struct ha_table_option_struct* options);

  bool doing_insert_batch;
  ha_rows insert_rows_batched;

  uint dyncol_field;
  bool dyncol_set;

  /* Used to produce 'wrong column %s at row %lu' warnings */
  ha_rows insert_lineno;
  void print_conversion_error(const char *field_name,
                              char *cass_value, int cass_value_len);
  int connect_and_check_options(TABLE *table_arg);
public:
  ha_cassandra(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_cassandra()
  {
    free_field_converters();
    delete se;
  }

  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const { return "CASSANDRA"; }

  /** @brief
    The name of the index type that will be used for display.
    Don't implement this method unless you really have indexes.
   */
  const char *index_type(uint inx) { return "HASH"; }

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const
  {
    return HA_BINLOG_STMT_CAPABLE |
           HA_REC_NOT_IN_SEQ |
           HA_NO_TRANSACTIONS |
           HA_REQUIRE_PRIMARY_KEY |
           HA_PRIMARY_KEY_IN_READ_INDEX |
           HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
           HA_NO_AUTO_INCREMENT |
           HA_TABLE_SCAN_ON_INDEX;
  }

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

      @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MySQL wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint inx, uint part, bool all_parts) const
  {
    return 0;
  }

  /** @brief
    unireg.cc will call max_supported_record_length(), max_supported_keys(),
    max_supported_key_parts(), uint max_supported_key_length()
    to make sure that the storage engine can handle the data it is about to
    send. Return *real* limits of your storage engine here; MySQL will do
    min(your_limits, MySQL_limits) automatically.
   */
  uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }

  /* Support only one Primary Key, for now */
  uint max_supported_keys()          const { return 1; }
  uint max_supported_key_parts()     const { return 1; }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_key_length()    const { return 16*1024; /* just to return something*/ }

  int index_init(uint idx, bool sorted);

  int index_read_map(uchar * buf, const uchar * key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag);

  /** @brief
    Called in test_quick_select to determine if indexes should be used.
  */
  virtual double scan_time() { return (double) (stats.records+stats.deleted) / 20.0+10; }

  /** @brief
    This method will never be called if you do not implement indexes.
  */
  virtual double read_time(uint, uint, ha_rows rows)
  { return (double) rows /  20.0+1; }

  virtual void start_bulk_insert(ha_rows rows, uint flags);
  virtual int end_bulk_insert();

  virtual int reset();


  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode, HANDLER_BUFFER *buf);
  int multi_range_read_next(range_id_t *range_info);
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param,
                                      uint n_ranges, uint *bufsz,
                                      uint *flags, Cost_estimate *cost);
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint key_parts, uint *bufsz,
                                uint *flags, Cost_estimate *cost);
  int multi_range_read_explain_info(uint mrr_mode, char *str, size_t size);

private:
  bool source_exhausted;
  bool mrr_start_read();
  int check_field_options(Field **fields);
  int read_dyncol(uint *count,
                  DYNAMIC_COLUMN_VALUE **vals, LEX_STRING **names,
                  String *valcol);
  int write_dynamic_row(uint count,
                        DYNAMIC_COLUMN_VALUE *vals,
                        LEX_STRING *names);
  void static free_dynamic_row(DYNAMIC_COLUMN_VALUE **vals,
                               LEX_STRING **names);
  CASSANDRA_TYPE_DEF * get_cassandra_field_def(char *cass_name,
                                               int cass_name_length);
public:
  int open(const char *name, int mode, uint test_if_locked);
  int close(void);

  int write_row(uchar *buf);
  int update_row(const uchar *old_data, uchar *new_data);
  int delete_row(const uchar *buf);

  /** @brief
    Unlike index_init(), rnd_init() can be called two consecutive times
    without rnd_end() in between (it only makes sense if scan=1). In this
    case, the second call should prepare for the new table scan (e.g if
    rnd_init() allocates the cursor, the second call should position the
    cursor to the start of the table; no need to deallocate and allocate
    it again. This is a required method.
  */
  int rnd_init(bool scan);                                      //required
  int rnd_end();
  int rnd_next(uchar *buf);                                     ///< required
  int rnd_pos(uchar *buf, uchar *pos);                          ///< required
  void position(const uchar *record);                           ///< required
  int info(uint);                                               ///< required
  int delete_all_rows(void);
  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key);
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info);                      ///< required
  bool check_if_incompatible_data(HA_CREATE_INFO *info,
                                  uint table_changes);

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);     ///< required

  my_bool register_query_cache_table(THD *thd, char *table_key,
                                     uint key_length,
                                     qc_engine_callback
                                     *engine_callback,
                                     ulonglong *engine_data)
  {
    /* 
      Do not put data from Cassandra tables into query cache (because there 
      is no way to tell whether the data in cassandra cluster has changed or 
      not)
    */
    return FALSE;
  }
};
