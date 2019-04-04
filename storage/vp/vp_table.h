/* Copyright (C) 2009-2019 Kentoku Shiba
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
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*
  Structure used to manage VP parameter string parsing.  Types of
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

typedef struct st_vp_param_string_parse
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

  void init(
    char *param_string,
    int error_code
  );

  /**
    Initialize the current parameter title.
  */

  void init_param_title();

  /**
    Save pointers to the start and end positions of the current parameter
    title in the parameter string.  Also save the parameter title's
    delimiter character.

    @param  start_value       Pointer to the start position of the current
                              parameter title.
    @param  end_value         Pointer to the end   position of the current
                              parameter title.
  */

  void set_param_title(
    char *start_title,
    char *end_title
  );

  /**
    Initialize the current parameter value.
  */

  void init_param_value();

  /**
    Save pointers to the start and end positions of the current parameter
    value in the parameter string.  Also save the parameter value's
    delimiter character.

    @param  start_value       Pointer to the start position of the current
                              parameter value.
    @param  end_value         Pointer to the end   position of the current
                              parameter value.
  */

  void set_param_value(
    char *start_value,
    char *end_value
  );

  /**
    Determine whether the current parameter in the parameter string has
    extra parameter values.

    @return   0               Current parameter value in the parameter string
                              does not have extra parameter values.
              <> 0            Error code indicating that the current parameter
                              value in the parameter string has extra
                              parameter values.
  */

  int has_extra_parameter_values();

  /**
    Restore the current parameter's input delimiter characters in the
    parameter string.  They were NULLed during parameter parsing.
  */

  void restore_delims();

  /**
    Print a parameter string error message.

    @return                   Error code.
  */

  int print_param_error();

} VP_PARAM_STRING_PARSE;

#define VP_PARAM_STR_LEN(name) name ## _length
#define VP_PARAM_STR(base_struct, title_name, param_name) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("vp " title_name " start")); \
    if (!base_struct->param_name) \
    { \
      if ((base_struct->param_name = vp_get_string_between_quote( \
        start_ptr, TRUE, &param_string_parse))) \
        base_struct->VP_PARAM_STR_LEN(param_name) = strlen(base_struct->param_name); \
      else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("vp " title_name "=%s", base_struct->param_name)); \
    } \
    break; \
  }
#define VP_PARAM_HINT_WITH_MAX(base_struct, title_name, param_name, check_length, max_size, min_val, max_val) \
  if (!strncasecmp(tmp_ptr, title_name, check_length)) \
  { \
    DBUG_PRINT("info",("vp " title_name " start")); \
    DBUG_PRINT("info",("vp max_size=%d", max_size)); \
    int hint_num = atoi(tmp_ptr + check_length) - 1; \
    DBUG_PRINT("info",("vp hint_num=%d", hint_num)); \
    DBUG_PRINT("info",("vp " base_struct "->param_name=%x", \
      base_struct->param_name)); \
    if (base_struct->param_name) \
    { \
      if (hint_num < 0 || hint_num >= max_size) \
      { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } else if (base_struct->param_name[hint_num] != -1) \
        break; \
      char *hint_str = vp_get_string_between_quote(start_ptr, FALSE); \
      if (hint_str) \
      { \
        base_struct->param_name[hint_num] = atoi(hint_str); \
        if (base_struct->param_name[hint_num] < min_val) \
          base_struct->param_name[hint_num] = min_val; \
        else if (base_struct->param_name[hint_num] > max_val) \
          base_struct->param_name[hint_num] = max_val; \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("vp " title_name "[%d]=%d", hint_num, \
        base_struct->param_name[hint_num])); \
    } else { \
      error_num = param_string_parse.print_param_error(); \
      goto error; \
    } \
    break; \
  }
#define VP_PARAM_INT_WITH_MAX(base_struct, title_name, param_name, min_val, max_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("vp " title_name " start")); \
    if (base_struct->param_name == -1) \
    { \
      if ((tmp_ptr2 = vp_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        base_struct->param_name = atoi(tmp_ptr2); \
        if (base_struct->param_name < min_val) \
          base_struct->param_name = min_val; \
        else if (base_struct->param_name > max_val) \
          base_struct->param_name = max_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                             tmp_ptr2 + \
                                               strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("vp " title_name "=%d", base_struct->param_name)); \
    } \
    break; \
  }
#define VP_PARAM_INT(base_struct, title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("vp " title_name " start")); \
    if (base_struct->param_name == -1) \
    { \
      if ((tmp_ptr2 = vp_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        base_struct->param_name = atoi(tmp_ptr2); \
        if (base_struct->param_name < min_val) \
          base_struct->param_name = min_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                             tmp_ptr2 + \
                                               strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("vp " title_name "=%d", base_struct->param_name)); \
    } \
    break; \
  }
#define VP_PARAM_LONGLONG(base_struct, title_name, param_name, min_val) \
  if (!strncasecmp(tmp_ptr, title_name, title_length)) \
  { \
    DBUG_PRINT("info",("vp " title_name " start")); \
    if (base_struct->param_name == -1) \
    { \
      if ((tmp_ptr2 = vp_get_string_between_quote( \
        start_ptr, FALSE))) \
      { \
        base_struct->param_name = \
          my_strtoll10(tmp_ptr2, (char**) NULL, &error_num); \
        if (base_struct->param_name < min_val) \
          base_struct->param_name = min_val; \
        param_string_parse.set_param_value(tmp_ptr2, \
                                           tmp_ptr2 + \
                                             strlen(tmp_ptr2) + 1); \
      } else { \
        error_num = param_string_parse.print_param_error(); \
        goto error; \
      } \
      DBUG_PRINT("info",("vp " title_name "=%lld", \
        base_struct->param_name)); \
    } \
    break; \
  }

uchar *vp_tbl_get_key(
  VP_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

#ifdef WITH_PARTITION_STORAGE_ENGINE
uchar *vp_pt_share_get_key(
  VP_PARTITION_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);

uchar *vp_pt_handler_share_get_key(
  VP_PARTITION_HANDLER_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
);
#endif

int vp_free_share_alloc(
  VP_SHARE *share
);

char *vp_get_string_between_quote(
  char *ptr,
  bool alloc,
  VP_PARAM_STRING_PARSE *param_string_parse = NULL
);

int vp_parse_table_info(
  VP_SHARE *share,
  TABLE *table,
  uint create_table
);

int vp_set_table_info_default(
  VP_SHARE *share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_element *part_elem,
  partition_element *sub_elem,
#endif
  TABLE *table
);

VP_SHARE *vp_get_share(
  const char *table_name,
  TABLE *table,
  const THD *thd,
  ha_vp *vp,
  int *error_num
);

int vp_free_share(
  VP_SHARE *vp
);

#ifdef WITH_PARTITION_STORAGE_ENGINE
VP_PARTITION_SHARE *vp_get_pt_share(
  TABLE *table,
  VP_SHARE *share,
  int *error_num
);

int vp_free_pt_share(
  VP_PARTITION_SHARE *partition_share
);
#endif

bool vp_flush_logs(
  handlerton *hton
);

handler* vp_create_handler(
  handlerton *hton,
  TABLE_SHARE *table, 
  MEM_ROOT *mem_root
);

int vp_close_connection(
  handlerton* hton,
  THD* thd
);

void vp_drop_database(
  handlerton *hton,
  char* path
);

bool vp_show_status(
  handlerton *hton,
  THD *thd,
  stat_print_fn *stat_print,
  enum ha_stat_type stat_type
);

int vp_start_consistent_snapshot(
  handlerton *hton,
  THD* thd
);

int vp_commit(
  handlerton *hton,
  THD *thd,
  bool all
);

int vp_rollback(
  handlerton *hton,
  THD *thd,
  bool all
);

int vp_xa_prepare(
  handlerton *hton,
  THD* thd,
  bool all
);

int vp_xa_recover(
  handlerton *hton,
  XID* xid_list,
  uint len
);

int vp_xa_commit_by_xid(
  handlerton *hton,
  XID* xid
);

int vp_xa_rollback_by_xid(
  handlerton *hton,
  XID* xid
);

int vp_db_done(
  void *p
);

int vp_panic(
  handlerton *hton,
  ha_panic_function type
);

int vp_db_init(
  void *p
);

char *vp_create_string(
  const char *str,
  uint length
);

char *vp_create_table_name_string(
  const char *table_name,
  const char *part_name,
  const char *sub_name
);

#ifdef WITH_PARTITION_STORAGE_ENGINE
void vp_get_partition_info(
  const char *table_name,
  uint table_name_length,
  const TABLE_SHARE *table_share,
  partition_info *part_info,
  partition_element **part_elem,
  partition_element **sub_elem
);
#endif

int vp_create_table_list(
  VP_SHARE *share
);

int vp_correspond_columns(
  ha_vp *vp,
  TABLE *table,
  VP_SHARE *share,
  TABLE_SHARE *table_share,
  TABLE_LIST *part_tables,
  bool reinit
);

uchar vp_bit_count(
  uchar bitmap
);

void *vp_bg_action(
  void *arg
);

int vp_table_num_list_to_bitmap(
  VP_SHARE *share,
  char *table_num_list,
  uchar *bitmap
);
