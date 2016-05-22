/* Copyright (C) 2008-2014 Kentoku Shiba

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

uchar *spider_tbl_get_key(
  SPIDER_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

#ifdef WITH_PARTITION_STORAGE_ENGINE
uchar *spider_pt_share_get_key(
  SPIDER_PARTITION_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

uchar *spider_pt_handler_share_get_key(
  SPIDER_PARTITION_HANDLER_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);
#endif

uchar *spider_link_get_key(
  SPIDER_LINK_FOR_HASH *link_for_hash,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

uchar *spider_ha_get_key(
  ha_spider *spider,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
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

char *spider_get_string_between_quote(
  char *ptr,
  bool alloc
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
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info,
#endif
  uint create_table
);

int spider_set_connect_info_default(
  SPIDER_SHARE *share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_element *part_elem,
  partition_element *sub_elem,
#endif
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

#ifndef DBUG_OFF
void spider_print_keys(
  const char *key,
  uint length
);
#endif

int spider_create_conn_keys(
  SPIDER_SHARE *share
);

#ifdef SPIDER_HAS_HASH_VALUE_TYPE
SPIDER_LGTM_TBLHND_SHARE *spider_get_lgtm_tblhnd_share(
  const char *table_name,
  uint table_name_length,
  my_hash_value_type hash_value,
  bool locked,
  bool need_to_create,
  int *error_num
);
#else
SPIDER_LGTM_TBLHND_SHARE *spider_get_lgtm_tblhnd_share(
  const char *table_name,
  uint table_name_length,
  bool locked,
  bool need_to_create,
  int *error_num
);
#endif

void spider_free_lgtm_tblhnd_share_alloc(
  SPIDER_LGTM_TBLHND_SHARE *lgtm_tblhnd_share,
  bool locked
);

SPIDER_SHARE *spider_create_share(
  const char *table_name,
  TABLE_SHARE *table_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info,
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value,
#endif
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

#ifdef WITH_PARTITION_STORAGE_ENGINE
SPIDER_PARTITION_SHARE *spider_get_pt_share(
  SPIDER_SHARE *share,
  TABLE_SHARE *table_share,
  int *error_num
);

int spider_free_pt_share(
  SPIDER_PARTITION_SHARE *partition_share
);

void spider_copy_sts_to_pt_share(
  SPIDER_PARTITION_SHARE *partition_share,
  SPIDER_SHARE *share
);

void spider_copy_sts_to_share(
  SPIDER_SHARE *share,
  SPIDER_PARTITION_SHARE *partition_share
);

void spider_copy_crd_to_pt_share(
  SPIDER_PARTITION_SHARE *partition_share,
  SPIDER_SHARE *share,
  int fields
);

void spider_copy_crd_to_share(
  SPIDER_SHARE *share,
  SPIDER_PARTITION_SHARE *partition_share,
  int fields
);
#endif

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

#ifdef WITH_PARTITION_STORAGE_ENGINE
void spider_get_partition_info(
  const char *table_name,
  uint table_name_length,
  const TABLE_SHARE *table_share,
  partition_info *part_info,
  partition_element **part_elem,
  partition_element **sub_elem
);
#endif

int spider_get_sts(
  SPIDER_SHARE *share,
  int link_idx,
  time_t tmp_time,
  ha_spider *spider,
  double sts_interval,
  int sts_mode,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int sts_sync,
#endif
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
#ifdef WITH_PARTITION_STORAGE_ENGINE
  int crd_sync,
#endif
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

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
bool spider_check_hs_pk_update(
  ha_spider *spider,
  key_range *key
);
#endif
#endif

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

st_select_lex *spider_get_select_lex(
  ha_spider *spider
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

bool spider_check_index_merge(
  TABLE *table,
  st_select_lex *select_lex
);

int spider_compare_for_sort(
  SPIDER_SORT *a,
  SPIDER_SORT *b
);

ulong spider_calc_for_sort(
  uint count,
  ...
);

double spider_rand(
  uint32 rand_source
);

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
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
#endif
