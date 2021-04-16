/* Copyright (C) 2008-2019 Kentoku Shiba
   Copyright (C) 2019 MariaDB corp

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
  char *start_ptr;         /* Pointer to the start of the parameter string */
  char *end_ptr;           /* Pointer to the end   of the parameter string */
  char *start_title_ptr;   /* Pointer to the start of the current parameter
                              title */
  char *end_title_ptr;     /* Pointer to the end   of the current parameter
                              title */
  char *start_value_ptr;   /* Pointer to the start of the current parameter
                              value */
  char *end_value_ptr;     /* Pointer to the end   of the current parameter
                              value */
  int  error_num;          /* Error code of the error message to print when
                              an error is detected */
  uint delim_title_len;    /* Length of the paramater title's delimiter */
  uint delim_value_len;    /* Length of the paramater value's delimiter */
  char delim_title;        /* Current parameter title's delimiter character */
  char delim_value;        /* Current parameter value's delimiter character */

  /**
    Initialize the parameter string parse information.

    @param  param_string      Pointer to the parameter string being parsed.
    @param  error_code        Error code of the error message to print when
                              an error is detected.
  */

  inline void init(char *param_string, int error_code)
  {
    start_ptr = param_string;
    end_ptr = start_ptr + strlen(start_ptr);

    init_param_title();
    init_param_value();

    error_num = error_code;
  }

  /**
    Initialize the current parameter title.
  */

  inline void init_param_title()
  {
    start_title_ptr = end_title_ptr = NULL;
    delim_title_len = 0;
    delim_title = '\0';
  }

  /**
    Save pointers to the start and end positions of the current parameter
    title in the parameter string.  Also save the parameter title's
    delimiter character.

    @param  start_value       Pointer to the start position of the current
                              parameter title.
    @param  end_value         Pointer to the end   position of the current
                              parameter title.
  */

  inline void set_param_title(char *start_title, char *end_title)
  {
    start_title_ptr = start_title;
    end_title_ptr = end_title;

    if (*start_title == '"' ||
        *start_title == '\'')
    {
      delim_title = *start_title;

      if (start_title >= start_ptr && *--start_title == '\\')
        delim_title_len = 2;
      else
        delim_title_len = 1;
    }
  }

  /**
    Initialize the current parameter value.
  */

  inline void init_param_value()
  {
    start_value_ptr = end_value_ptr = NULL;
    delim_value_len = 0;
    delim_value = '\0';
  }

  /**
    Save pointers to the start and end positions of the current parameter
    value in the parameter string.  Also save the parameter value's
    delimiter character.

    @param  start_value       Pointer to the start position of the current
                              parameter value.
    @param  end_value         Pointer to the end   position of the current
                              parameter value.
  */

  inline void set_param_value(char *start_value, char *end_value)
  {
    start_value_ptr = start_value--;
    end_value_ptr = end_value;

    if (*start_value == '"' ||
        *start_value == '\'')
    {
      delim_value = *start_value;

      if (*--start_value == '\\')
        delim_value_len = 2;
      else
        delim_value_len = 1;
    }
  }

  /**
    Determine whether the current parameter in the parameter string has
    extra parameter values.

    @return   0               Current parameter value in the parameter string
                              does not have extra parameter values.
              <> 0            Error code indicating that the current parameter
                              value in the parameter string has extra
                              parameter values.
  */

  inline int has_extra_parameter_values()
  {
    int error_num = 0;
    DBUG_ENTER("has_extra_parameter_values");

    if (end_value_ptr)
    {
      /* There is a current parameter value */
      char *end_param_ptr =  end_value_ptr;

      while (end_param_ptr < end_ptr &&
        (*end_param_ptr == ' ' || *end_param_ptr == '\r' ||
         *end_param_ptr == '\n' || *end_param_ptr == '\t'))
        end_param_ptr++;

      if (end_param_ptr < end_ptr && *end_param_ptr != '\0')
      {
        /* Extra values in parameter definition */
        error_num = print_param_error();
      }
    }

    DBUG_RETURN(error_num);
  }

  inline int get_next_parameter_head(char *st, char **nx)
  {
    DBUG_ENTER("get_next_parameter_head");
    char *sq = strchr(st, '\'');
    char *dq = strchr(st, '"');
    if (!sq && !dq)
    {
      DBUG_RETURN(print_param_error());
    }
    else if (!sq || sq > dq)
    {
      while (1)
      {
        ++dq;
        if (*dq == '\\')
        {
          ++dq;
        }
        else if (*dq == '"')
        {
          break;
        }
        else if (*dq == '\0')
        {
          DBUG_RETURN(print_param_error());
        }
      }
      while (1)
      {
        ++dq;
        if (*dq == '\0')
        {
          *nx = dq;
          break;
        }
        else if (*dq == ',')
        {
          *dq = '\0';
          *nx = dq + 1;
          break;
        }
        else if (*dq != ' ' && *dq != '\r' && *dq != '\n' && *dq != '\t')
        {
          DBUG_RETURN(print_param_error());
        }
      }
    }
    else
    {
      while (1)
      {
        ++sq;
        if (*sq == '\\')
        {
          ++sq;
        }
        else if (*sq == '\'')
        {
          break;
        }
        else if (*sq == '\0')
        {
          DBUG_RETURN(print_param_error());
        }
      }
      while (1)
      {
        ++sq;
        if (*sq == '\0')
        {
          *nx = sq;
          break;
        }
        else if (*sq == ',')
        {
          *sq = '\0';
          *nx = sq + 1;
          break;
        }
        else if (*sq != ' ' && *sq != '\r' && *sq != '\n' && *sq != '\t')
        {
          DBUG_RETURN(print_param_error());
        }
      }
    }
    DBUG_RETURN(0);
  }

  /**
    Restore the current parameter's input delimiter characters in the
    parameter string.  They were NULLed during parameter parsing.
  */

  inline void restore_delims()
  {
    char *end = end_title_ptr - 1;

    switch (delim_title_len)
    {
    case 2:
      *end++ = '\\';
      /* Fall through */
    case 1:
      *end = delim_title;
    }

    end = end_value_ptr - 1;
    switch (delim_value_len)
    {
    case 2:
      *end++ = '\\';
      /* Fall through */
    case 1:
      *end = delim_value;
    }
  }

  /**
    Print a parameter string error message.

    @return                   Error code.
  */

  int print_param_error();
} SPIDER_PARAM_STRING_PARSE;

uchar *spider_tbl_get_key(
  SPIDER_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

uchar *spider_wide_share_get_key(
  SPIDER_WIDE_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

#ifdef WITH_PARTITION_STORAGE_ENGINE
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
  bool alloc,
  SPIDER_PARAM_STRING_PARSE *param_string_parse = NULL
);

int spider_create_string_list(
  char ***string_list,
  uint **string_length_list,
  uint *list_length,
  char *str,
  uint length,
  SPIDER_PARAM_STRING_PARSE *param_string_parse
);

int spider_create_long_list(
  long **long_list,
  uint *list_length,
  char *str,
  uint length,
  long min_val,
  long max_val,
  SPIDER_PARAM_STRING_PARSE *param_string_parse
);

int spider_create_longlong_list(
  longlong **longlong_list,
  uint *list_length,
  char *str,
  uint length,
  longlong min_val,
  longlong max_val,
  SPIDER_PARAM_STRING_PARSE *param_string_parse
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

SPIDER_WIDE_SHARE *spider_get_wide_share(
  SPIDER_SHARE *share,
  TABLE_SHARE *table_share,
  int *error_num
);

int spider_free_wide_share(
  SPIDER_WIDE_SHARE *wide_share
);

void spider_copy_sts_to_wide_share(
  SPIDER_WIDE_SHARE *wide_share,
  SPIDER_SHARE *share
);

void spider_copy_sts_to_share(
  SPIDER_SHARE *share,
  SPIDER_WIDE_SHARE *wide_share
);

void spider_copy_crd_to_wide_share(
  SPIDER_WIDE_SHARE *wide_share,
  SPIDER_SHARE *share,
  int fields
);

void spider_copy_crd_to_share(
  SPIDER_SHARE *share,
  SPIDER_WIDE_SHARE *wide_share,
  int fields
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

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
bool spider_all_part_in_order(
  ORDER *order,
  TABLE *table
);

Field *spider_field_exchange(
  handler *handler,
  Field *field
);
#endif

int spider_set_direct_limit_offset(
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

#ifndef WITHOUT_SPIDER_BG_SEARCH
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
#endif
uchar *spider_duplicate_char(
  uchar *dst,
  uchar esc,
  uchar *src,
  uint src_lgt
);
