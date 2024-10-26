/* Copyright (C) 2008-2019 Kentoku Shiba
   Copyright (C) 2019-2022 MariaDB corp

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

/*
  Structure used to manage Spider parameter string parsing.  Types of
  parameters include:
    - connection strings
    - UDF parameters

  A parameter string consists of one or more parameter definitions using
  the following syntax:
    <parameter title> <parameter value>
  A comma is the separator character between multiple parameter definitions.
  Parameter titles must not be quoted.  Parameter values must be quoted with
  single or double quotes.
*/

typedef struct st_spider_param_string_parse
{
  char *start_title;   /* Pointer to the start of the current parameter
                         title */
  char *end_title;     /* Pointer to the end   of the current
                         parameter value */
  char *start_value;   /* Pointer to the start of the current parameter
                         value */
  char *end_value;     /* Pointer to the end   of the current parameter
                         value */
  char delim_value;    /* Current parameter value's delimiter
                         character, either a single or a double quote */
  int  error_num;      /* Error code of the error message to print when
                         an error is detected */

  int fail(bool restore_delim);
  bool locate_param_def(char*& start_param);
} SPIDER_PARAM_STRING_PARSE;

const uchar *spider_tbl_get_key(
  const void *share,
  size_t *length,
  my_bool
);

const uchar *spider_wide_share_get_key(
  const void *share,
  size_t *length,
  my_bool
);

const uchar *spider_link_get_key(
  const void *link_for_hash,
  size_t *length,
  my_bool
);

int spider_get_server(
  SPIDER_SHARE *share,
  int link_idx
);

int spider_free_share_alloc(
  SPIDER_SHARE *share
);

void spider_free_tmp_share_alloc(
  SPIDER_SHARE *share
);

int spider_create_string_list(
  char ***string_list,
  uint **string_length_list,
  uint *list_length,
  char *str,
  uint length
);

int spider_create_long_list(
  long **long_list,
  uint *list_length,
  char *str,
  uint length,
  long min_val,
  long max_val
);

int spider_create_longlong_list(
  longlong **longlong_list,
  uint *list_length,
  char *str,
  uint length,
  longlong min_val,
  longlong max_val
);

int spider_increase_string_list(
  char ***string_list,
  uint **string_length_list,
  uint *list_length,
  uint *list_charlen,
  uint link_count
);

int spider_increase_long_list(
  long **long_list,
  uint *list_length,
  uint link_count
);

int spider_increase_longlong_list(
  longlong **longlong_list,
  uint *list_length,
  uint link_count
);

int spider_parse_connect_info(
  SPIDER_SHARE *share,
  TABLE_SHARE *table_share,
  partition_info *part_info,
  uint create_table
);

int spider_set_connect_info_default(
  SPIDER_SHARE *share,
  partition_element *part_elem,
  partition_element *sub_elem,
  TABLE_SHARE *table_share
);

int spider_set_connect_info_default_db_table(
  SPIDER_SHARE *share,
  const char *db_name,
  uint db_name_length,
  const char *table_name,
  uint table_name_length
);

int spider_set_connect_info_default_dbtable(
  SPIDER_SHARE *share,
  const char *dbtable_name,
  int dbtable_name_length
);

#ifdef DBUG_TRACE
void spider_print_keys(
  const char *key,
  uint length
);
#endif

void spider_create_conn_key_add_one(int* counter, char** target, char* src);

int spider_create_conn_keys(
  SPIDER_SHARE *share
);

SPIDER_LGTM_TBLHND_SHARE *spider_get_lgtm_tblhnd_share(
  const char *table_name,
  uint table_name_length,
  my_hash_value_type hash_value,
  bool locked,
  bool need_to_create,
  int *error_num
);

void spider_free_lgtm_tblhnd_share_alloc(
  SPIDER_LGTM_TBLHND_SHARE *lgtm_tblhnd_share,
  bool locked
);

SPIDER_SHARE *spider_create_share(
  const char *table_name,
  TABLE_SHARE *table_share,
  partition_info *part_info,
  my_hash_value_type hash_value,
  int *error_num
);

SPIDER_SHARE *spider_get_share(
  const char *table_name,
  TABLE *table,
  THD *thd,
  ha_spider *spider,
  int *error_num
);

void spider_free_share_resource_only(
  SPIDER_SHARE *share
);

int spider_free_share(
  SPIDER_SHARE *share
);

void spider_update_link_status_for_share(
  const char *table_name,
  uint table_name_length,
  int link_idx,
  long link_status
);

SPIDER_WIDE_SHARE *spider_get_wide_share(
  SPIDER_SHARE *share,
  TABLE_SHARE *table_share,
  int *error_num
);

int spider_free_wide_share(
  SPIDER_WIDE_SHARE *wide_share
);

int spider_open_all_tables(
  SPIDER_TRX *trx,
  bool lock
);

bool spider_flush_logs(
  handlerton *hton
);

handler* spider_create_handler(
  handlerton *hton,
  TABLE_SHARE *table, 
  MEM_ROOT *mem_root
);

int spider_close_connection(
  handlerton* hton,
  THD* thd
);

void spider_drop_database(
  handlerton *hton,
  char* path
);

bool spider_show_status(
  handlerton *hton,
  THD *thd,
  stat_print_fn *stat_print,
  enum ha_stat_type stat_type
);

int spider_db_done(
  void *p
);

int spider_panic(
  handlerton *hton,
  ha_panic_function type
);

int spider_db_init(
  void *p
);

char *spider_create_table_name_string(
  const char *table_name,
  const char *part_name,
  const char *sub_name
);

void spider_get_partition_info(
  const char *table_name,
  uint table_name_length,
  const TABLE_SHARE *table_share,
  partition_info *part_info,
  partition_element **part_elem,
  partition_element **sub_elem
);

int spider_get_sts(
  SPIDER_SHARE *share,
  int link_idx,
  time_t tmp_time,
  ha_spider *spider,
  double sts_interval,
  int sts_mode,
  int sts_sync,
  int sts_sync_level,
  uint flag
);

int spider_get_crd(
  SPIDER_SHARE *share,
  int link_idx,
  time_t tmp_time,
  ha_spider *spider,
  TABLE *table,
  double crd_interval,
  int crd_mode,
  int crd_sync,
  int crd_sync_level
);

void spider_set_result_list_param(
  ha_spider *spider
);

SPIDER_INIT_ERROR_TABLE *spider_get_init_error_table(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  bool create
);

void spider_delete_init_error_table(
  const char *name
);

bool spider_check_pk_update(
  TABLE *table
);

void spider_set_tmp_share_pointer(
  SPIDER_SHARE *tmp_share,
  char **tmp_connect_info,
  uint *tmp_connect_info_length,
  long *tmp_long,
  longlong *tmp_longlong
);

int spider_create_tmp_dbton_share(
  SPIDER_SHARE *tmp_share
);

void spider_free_tmp_dbton_share(
  SPIDER_SHARE *tmp_share
);

int spider_create_tmp_dbton_handler(
  ha_spider *tmp_spider
);

void spider_free_tmp_dbton_handler(
  ha_spider *tmp_spider
);

TABLE_LIST *spider_get_parent_table_list(
  ha_spider *spider
);
List<Index_hint> *spider_get_index_hints(
  ha_spider *spider
  );

st_select_lex *spider_get_select_lex(
  ha_spider *spider
);

void spider_get_select_limit_from_select_lex(
  st_select_lex *select_lex,
  longlong *select_limit,
  longlong *offset_limit
);

void spider_get_select_limit(
  ha_spider *spider,
  st_select_lex **select_lex,
  longlong *select_limit,
  longlong *offset_limit
);

longlong spider_split_read_param(
  ha_spider *spider
);

longlong spider_bg_split_read_param(
  ha_spider *spider
);

void spider_first_split_read_param(
  ha_spider *spider
);

void spider_next_split_read_param(
  ha_spider *spider
);

bool spider_check_direct_order_limit(
  ha_spider *spider
);

bool spider_all_part_in_order(
  ORDER *order,
  TABLE *table
);

Field *spider_field_exchange(
  handler *handler,
  Field *field
);

int spider_set_direct_limit_offset(
  ha_spider *spider
);

bool spider_check_index_merge(
  TABLE *table,
  st_select_lex *select_lex
);

int spider_compare_for_sort(
  const void *a,
  const void *b
);

ulong spider_calc_for_sort(
  uint count,
  ...
);

double spider_rand(
  uint32 rand_source
);

int spider_discover_table_structure_internal(
  SPIDER_TRX *trx,
  SPIDER_SHARE *spider_share,
  spider_string *str
);

int spider_discover_table_structure(
  handlerton *hton,
  THD* thd,
  TABLE_SHARE *share,
  HA_CREATE_INFO *info
);

int spider_create_spider_object_for_share(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  ha_spider **spider
);

void spider_free_spider_object_for_share(
  ha_spider **spider
);

int spider_create_sts_threads(
  SPIDER_THREAD *spider_thread
);

void spider_free_sts_threads(
  SPIDER_THREAD *spider_thread
);

int spider_create_crd_threads(
  SPIDER_THREAD *spider_thread
);

void spider_free_crd_threads(
  SPIDER_THREAD *spider_thread
);

void *spider_table_bg_sts_action(
  void *arg
);

void *spider_table_bg_crd_action(
  void *arg
);

void spider_table_add_share_to_sts_thread(
  SPIDER_SHARE *share
);

void spider_table_add_share_to_crd_thread(
  SPIDER_SHARE *share
);

void spider_table_remove_share_from_sts_thread(
  SPIDER_SHARE *share
);

void spider_table_remove_share_from_crd_thread(
  SPIDER_SHARE *share
);
uchar *spider_duplicate_char(
  uchar *dst,
  uchar esc,
  uchar *src,
  uint src_lgt
);
