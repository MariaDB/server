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

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "vp_environ.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_base.h"
#include "sql_select.h"
#endif
#ifdef WITH_PARTITION_STORAGE_ENGINE
#include "ha_partition.h"
#endif
#include "vp_err.h"
#include "vp_param.h"
#include "vp_include.h"
#include "ha_vp.h"
#include "vp_table.h"

#ifndef VP_HAS_NEXT_THREAD_ID
ulong *vp_db_att_thread_id;
#endif

handlerton *vp_hton_ptr;
handlerton *vp_partition_hton_ptr;

#ifdef HAVE_PSI_INTERFACE
PSI_mutex_key vp_key_mutex_tbl;
#ifdef WITH_PARTITION_STORAGE_ENGINE
PSI_mutex_key vp_key_mutex_pt_share;
#endif
PSI_mutex_key vp_key_mutex_bg_sync;
PSI_mutex_key vp_key_mutex_bg;
PSI_mutex_key vp_key_mutex_share;
PSI_mutex_key vp_key_mutex_share_init;
#ifdef WITH_PARTITION_STORAGE_ENGINE
PSI_mutex_key vp_key_mutex_pt_handler;
#endif

static PSI_mutex_info all_vp_mutexes[]=
{
  { &vp_key_mutex_tbl, "tbl", PSI_FLAG_GLOBAL},
#ifdef WITH_PARTITION_STORAGE_ENGINE
  { &vp_key_mutex_pt_share, "pt_share", PSI_FLAG_GLOBAL},
#endif
#ifndef WITHOUT_VP_BG_ACCESS
  { &vp_key_mutex_bg_sync, "bg_sync", 0},
  { &vp_key_mutex_bg, "bg", 0},
#endif
  { &vp_key_mutex_share, "share", 0},
  { &vp_key_mutex_share_init, "share_init", 0},
#ifdef WITH_PARTITION_STORAGE_ENGINE
  { &vp_key_mutex_pt_handler, "pt_handler", 0},
#endif
};

#ifndef WITHOUT_VP_BG_ACCESS
PSI_cond_key vp_key_cond_bg_sync;
PSI_cond_key vp_key_cond_bg;
#endif

static PSI_cond_info all_vp_conds[] = {
#ifndef WITHOUT_VP_BG_ACCESS
  {&vp_key_cond_bg_sync, "bg_sync", 0},
  {&vp_key_cond_bg, "bg", 0},
#endif
};

#ifndef WITHOUT_VP_BG_ACCESS
PSI_thread_key vp_key_thd_bg;
#endif

static PSI_thread_info all_vp_threads[] = {
#ifndef WITHOUT_VP_BG_ACCESS
  {&vp_key_thd_bg, "bg", 0},
#endif
};
#endif

HASH vp_open_tables;
pthread_mutex_t vp_tbl_mutex;

#ifdef WITH_PARTITION_STORAGE_ENGINE
HASH vp_open_pt_share;
pthread_mutex_t vp_pt_share_mutex;
#endif

#ifndef WITHOUT_VP_BG_ACCESS
pthread_attr_t vp_pt_attr;
#endif

// for vp_open_tables
uchar *vp_tbl_get_key(
  VP_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("vp_tbl_get_key");
  *length = share->table_name_length;
  DBUG_RETURN((uchar*) share->table_name);
}

#ifdef WITH_PARTITION_STORAGE_ENGINE
uchar *vp_pt_share_get_key(
  VP_PARTITION_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("vp_pt_share_get_key");
  *length = share->table_name_length;
  DBUG_RETURN((uchar*) share->table_name);
}

uchar *vp_pt_handler_share_get_key(
  VP_PARTITION_HANDLER_SHARE *share,
  size_t *length,
  my_bool not_used __attribute__ ((unused))
) {
  DBUG_ENTER("vp_pt_handler_share_get_key");
  *length = sizeof(TABLE *);
  DBUG_RETURN((uchar*) &share->table);
}
#endif

#ifdef HAVE_PSI_INTERFACE
static void init_vp_psi_keys()
{
  DBUG_ENTER("init_vp_psi_keys");
  if (PSI_server == NULL)
    DBUG_VOID_RETURN;

  PSI_server->register_mutex("vp", all_vp_mutexes,
    array_elements(all_vp_mutexes));
  PSI_server->register_cond("vp", all_vp_conds,
    array_elements(all_vp_conds));
  PSI_server->register_thread("vp", all_vp_threads,
    array_elements(all_vp_threads));
  DBUG_VOID_RETURN;
}
#endif

int vp_free_share_alloc(
  VP_SHARE *share
) {
  DBUG_ENTER("vp_free_share_alloc");
  if (share->tgt_default_db_name)
    vp_my_free(share->tgt_default_db_name, MYF(0));
  if (share->tgt_table_name_list)
    vp_my_free(share->tgt_table_name_list, MYF(0));
  if (share->tgt_table_name_prefix)
    vp_my_free(share->tgt_table_name_prefix, MYF(0));
  if (share->tgt_table_name_suffix)
    vp_my_free(share->tgt_table_name_suffix, MYF(0));
  if (share->choose_ignore_table_list)
    vp_my_free(share->choose_ignore_table_list, MYF(0));
  if (share->choose_ignore_table_list_for_lock)
    vp_my_free(share->choose_ignore_table_list_for_lock, MYF(0));
  if (share->tgt_db_name)
    vp_my_free(share->tgt_db_name, MYF(0));
  if (share->correspond_columns_p)
    vp_my_free(share->correspond_columns_p, MYF(0));
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (share->partition_share)
    vp_free_pt_share(share->partition_share);
#endif
  DBUG_RETURN(0);
}

/**
  Initialize the parameter string parse information.

  @param  param_string      Pointer to the parameter string being parsed.
  @param  error_code        Error code of the error message to print when
                            an error is detected.
*/

inline void st_vp_param_string_parse::init(
  char *param_string,
  int error_code
)
{
  DBUG_ENTER("st_vp_param_string_parse::init");
  start_ptr = param_string;
  end_ptr = start_ptr + strlen(start_ptr);

  init_param_title();
  init_param_value();

  error_num = error_code;
  DBUG_VOID_RETURN;
}

/**
  Initialize the current parameter title.
*/

inline void st_vp_param_string_parse::init_param_title()
{
  DBUG_ENTER("st_vp_param_string_parse::init_param_title");
  start_title_ptr = end_title_ptr = NULL;
  delim_title_len = 0;
  delim_title = '\0';
  DBUG_VOID_RETURN;
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

inline void st_vp_param_string_parse::set_param_title(
  char *start_title,
  char *end_title
)
{
  DBUG_ENTER("st_vp_param_string_parse::set_param_title");
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
  DBUG_VOID_RETURN;
}

/**
  Initialize the current parameter value.
*/

inline void st_vp_param_string_parse::init_param_value()
{
  DBUG_ENTER("st_vp_param_string_parse::init_param_value");
  start_value_ptr = end_value_ptr = NULL;
  delim_value_len = 0;
  delim_value = '\0';
  DBUG_VOID_RETURN;
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

inline void st_vp_param_string_parse::set_param_value(
  char *start_value,
  char *end_value
)
{
  DBUG_ENTER("st_vp_param_string_parse::set_param_value");
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
  DBUG_VOID_RETURN;
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

inline int st_vp_param_string_parse::has_extra_parameter_values()
{
  int error_num = 0;
  DBUG_ENTER("st_vp_param_string_parse::has_extra_parameter_values");

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

/**
  Restore the current parameter's input delimiter characters in the
  parameter string.  They were NULLed during parameter parsing.
*/

inline void st_vp_param_string_parse::restore_delims()
{
  char *end = end_title_ptr - 1;
  DBUG_ENTER("st_vp_param_string_parse::restore_delims");

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
  DBUG_VOID_RETURN;
}

/**
  Print a parameter string error message.

  @return                   Error code.
*/

int st_vp_param_string_parse::print_param_error()
{
  DBUG_ENTER("st_vp_param_string_parse::print_param_error");
  if (start_title_ptr)
  {
    /* Restore the input delimiter characters */
    restore_delims();

    /* Print the error message */
    switch (error_num)
    {
    case ER_VP_INVALID_UDF_PARAM_NUM:
      my_printf_error(error_num, ER_VP_INVALID_UDF_PARAM_STR,
                      MYF(0), start_title_ptr);
      break;
    case ER_VP_INVALID_TABLE_INFO_NUM:
    default:
      my_printf_error(error_num, ER_VP_INVALID_TABLE_INFO_STR,
                      MYF(0), start_title_ptr);
    }

    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

char *vp_get_string_between_quote(
  char *ptr,
  bool alloc,
  VP_PARAM_STRING_PARSE *param_string_parse
) {
  char *start_ptr, *end_ptr, *tmp_ptr, *esc_ptr;
  bool find_flg = FALSE, esc_flg = FALSE;
  DBUG_ENTER("vp_get_string_between_quote");

  start_ptr = strchr(ptr, '\'');
  end_ptr = strchr(ptr, '"');
  if (start_ptr && (!end_ptr || start_ptr < end_ptr))
  {
    tmp_ptr = ++start_ptr;
    while (!find_flg)
    {
      if (!(end_ptr = strchr(tmp_ptr, '\'')))
        DBUG_RETURN(NULL);
      esc_ptr = tmp_ptr;
      while (!find_flg)
      {
        esc_ptr = strchr(esc_ptr, '\\');
        if (!esc_ptr || esc_ptr > end_ptr)
          find_flg = TRUE;
        else if (esc_ptr == end_ptr - 1)
        {
          esc_flg = TRUE;
          tmp_ptr = end_ptr + 1;
          break;
        } else {
          esc_flg = TRUE;
          esc_ptr += 2;
        }
      }
    }
  } else if (end_ptr)
  {
    start_ptr = end_ptr;
    tmp_ptr = ++start_ptr;
    while (!find_flg)
    {
      if (!(end_ptr = strchr(tmp_ptr, '"')))
        DBUG_RETURN(NULL);
      esc_ptr = tmp_ptr;
      while (!find_flg)
      {
        esc_ptr = strchr(esc_ptr, '\\');
        if (!esc_ptr || esc_ptr > end_ptr)
          find_flg = TRUE;
        else if (esc_ptr == end_ptr - 1)
        {
          esc_flg = TRUE;
          tmp_ptr = end_ptr + 1;
          break;
        } else {
          esc_flg = TRUE;
          esc_ptr += 2;
        }
      }
    }
  } else
    DBUG_RETURN(NULL);

  *end_ptr = '\0';
  if (esc_flg)
  {
    esc_ptr = start_ptr;
    while (TRUE)
    {
      esc_ptr = strchr(esc_ptr, '\\');
      if (!esc_ptr)
        break;
      switch(*(esc_ptr + 1))
      {
        case 'b':
          *esc_ptr = '\b';
          break;
        case 'n':
          *esc_ptr = '\n';
          break;
        case 'r':
          *esc_ptr = '\r';
          break;
        case 't':
          *esc_ptr = '\t';
          break;
        default:
          *esc_ptr = *(esc_ptr + 1);
          break;
      }
      esc_ptr++;
      strcpy(esc_ptr, esc_ptr + 1);
    }
  }

  if (param_string_parse)
    param_string_parse->set_param_value(start_ptr, start_ptr + strlen(start_ptr) + 1);

  if (alloc)
  {
    DBUG_RETURN(
      vp_create_string(
      start_ptr,
      strlen(start_ptr))
    );
  } else {
    DBUG_RETURN(start_ptr);
  }
}

int vp_parse_table_info(
  VP_SHARE *share,
  TABLE *table,
  uint create_table
) {
  int error_num = 0;
  char *comment_string = NULL;
  char *sprit_ptr[2];
  char *tmp_ptr, *tmp_ptr2, *start_ptr;
  int roop_count;
  int title_length;
  VP_PARAM_STRING_PARSE param_string_parse;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_element *part_elem;
  partition_element *sub_elem;
#endif
  DBUG_ENTER("vp_parse_table_info");
#if MYSQL_VERSION_ID < 50500
  DBUG_PRINT("info",("vp partition_info=%s", table->s->partition_info));
#else
  DBUG_PRINT("info",("vp partition_info=%s", table->s->partition_info_str));
#endif
  DBUG_PRINT("info",("vp part_info=%p", table->part_info));
  DBUG_PRINT("info",("vp s->db=%s", table->s->db.str));
  DBUG_PRINT("info",("vp s->table_name=%s", table->s->table_name.str));
  DBUG_PRINT("info",("vp s->path=%s", table->s->path.str));
  DBUG_PRINT("info",
    ("vp s->normalized_path=%s", table->s->normalized_path.str));
#ifdef WITH_PARTITION_STORAGE_ENGINE
  vp_get_partition_info(share->table_name, share->table_name_length, table->s,
    table->part_info, &part_elem, &sub_elem);
#endif
  share->choose_table_mode = -1;
  share->choose_table_mode_for_lock = -1;
  share->multi_range_mode = -1;
  share->pk_correspond_mode = -1;
  share->info_src_table = -1;
  share->auto_increment_table = -1;
  share->table_count_mode = -1;
  share->support_table_cache = -1;
  share->child_binlog = -1;
#ifndef WITHOUT_VP_BG_ACCESS
  share->bgs_mode = -1;
  share->bgi_mode = -1;
  share->bgu_mode = -1;
#endif
  share->zero_record_update_mode = -1;
  share->allow_bulk_autoinc = -1;
  share->allow_different_column_type = -1;

#ifdef WITH_PARTITION_STORAGE_ENGINE
#ifdef VP_PARTITION_HAS_CONNECTION_STRING
  for (roop_count = 6; roop_count > 0; roop_count--)
#else
  for (roop_count = 4; roop_count > 0; roop_count--)
#endif
#else
  for (roop_count = 2; roop_count > 0; roop_count--)
#endif
  {
    if (comment_string)
    {
      vp_my_free(comment_string, MYF(0));
      comment_string = NULL;
    }
    switch (roop_count)
    {
#ifdef WITH_PARTITION_STORAGE_ENGINE
#ifdef VP_PARTITION_HAS_CONNECTION_STRING
      case 6:
        if (!sub_elem || sub_elem->connect_string.length == 0)
          continue;
        DBUG_PRINT("info",("vp create sub connect string"));
        if (
          !(comment_string = vp_create_string(
            sub_elem->connect_string.str,
            sub_elem->connect_string.length))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_comment_string;
        }
        DBUG_PRINT("info",("vp connect_string=%s", comment_string));
        break;
      case 5:
#else
      case 4:
#endif
        if (!sub_elem || !sub_elem->part_comment)
          continue;
        DBUG_PRINT("info",("vp create sub comment string"));
        if (
          !(comment_string = vp_create_string(
            sub_elem->part_comment,
            strlen(sub_elem->part_comment)))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_comment_string;
        }
        DBUG_PRINT("info",("vp sub comment string=%s", comment_string));
        break;
#ifdef VP_PARTITION_HAS_CONNECTION_STRING
      case 4:
        if (!part_elem || part_elem->connect_string.length == 0)
          continue;
        DBUG_PRINT("info",("vp create part connect string"));
        if (
          !(comment_string = vp_create_string(
            part_elem->connect_string.str,
            part_elem->connect_string.length))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_comment_string;
        }
        DBUG_PRINT("info",("vp connect_string=%s", comment_string));
        break;
#endif
      case 3:
        if (!part_elem || !part_elem->part_comment)
          continue;
        DBUG_PRINT("info",("vp create part comment string"));
        if (
          !(comment_string = vp_create_string(
            part_elem->part_comment,
            strlen(part_elem->part_comment)))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_comment_string;
        }
        DBUG_PRINT("info",("vp part comment string=%s", comment_string));
        break;
#endif
      case 2:
        if (table->s->comment.length == 0)
          continue;
        DBUG_PRINT("info",("vp create comment string"));
        if (
          !(comment_string = vp_create_string(
            table->s->comment.str,
            table->s->comment.length))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_comment_string;
        }
        DBUG_PRINT("info",("vp comment string=%s", comment_string));
        break;
      default:
        if (table->s->connect_string.length == 0)
          continue;
        DBUG_PRINT("info",("vp create connect_string string"));
        if (
          !(comment_string = vp_create_string(
            table->s->connect_string.str,
            table->s->connect_string.length))
        ) {
          error_num = HA_ERR_OUT_OF_MEM;
          goto error_alloc_comment_string;
        }
        DBUG_PRINT("info",("vp comment_string=%s", comment_string));
        break;
    }

    sprit_ptr[0] = comment_string;
    param_string_parse.init(comment_string,
      ER_VP_INVALID_TABLE_INFO_NUM);
    while (sprit_ptr[0])
    {
      if ((sprit_ptr[1] = strchr(sprit_ptr[0], ',')))
      {
        *sprit_ptr[1] = '\0';
        sprit_ptr[1]++;
      }
      tmp_ptr = sprit_ptr[0];
      sprit_ptr[0] = sprit_ptr[1];
      while (*tmp_ptr == ' ' || *tmp_ptr == '\r' ||
        *tmp_ptr == '\n' || *tmp_ptr == '\t')
        tmp_ptr++;

      if (*tmp_ptr == '\0')
        continue;

      title_length = 0;
      start_ptr = tmp_ptr;
      while (*start_ptr != ' ' && *start_ptr != '\'' &&
        *start_ptr != '"' && *start_ptr != '\0' &&
        *start_ptr != '\r' && *start_ptr != '\n' &&
        *start_ptr != '\t')
      {
        title_length++;
        start_ptr++;
      }
      param_string_parse.set_param_title(tmp_ptr, tmp_ptr + title_length);

      switch (title_length)
      {
        case 0:
          error_num = param_string_parse.print_param_error();
          if (error_num)
            goto error;
          continue;
        case 3:
          VP_PARAM_INT_WITH_MAX(share, "aba", allow_bulk_autoinc, 0, 1);
          VP_PARAM_INT_WITH_MAX(share, "adc", allow_different_column_type,
            0, 1);
          VP_PARAM_INT(share, "ait", auto_increment_table, 1);
#ifndef WITHOUT_VP_BG_ACCESS
          VP_PARAM_INT_WITH_MAX(share, "bgs", bgs_mode, 0, 1);
          VP_PARAM_INT_WITH_MAX(share, "bgi", bgi_mode, 0, 1);
          VP_PARAM_INT_WITH_MAX(share, "bgu", bgu_mode, 0, 1);
#endif
          VP_PARAM_INT_WITH_MAX(share, "cbl", child_binlog, 0, 1);
          VP_PARAM_STR(share, "cil", choose_ignore_table_list_for_lock);
          VP_PARAM_STR(share, "cit", choose_ignore_table_list);
          VP_PARAM_INT_WITH_MAX(share, "cml", choose_table_mode_for_lock,
            0, 1);
          VP_PARAM_INT_WITH_MAX(share, "ctm", choose_table_mode, 0, 1);
          VP_PARAM_STR(share, "ddb", tgt_default_db_name);
          VP_PARAM_INT_WITH_MAX(share, "tcm", table_count_mode, 0, 1);
          VP_PARAM_INT(share, "ist", info_src_table, 0);
          VP_PARAM_INT_WITH_MAX(share, "mrm", multi_range_mode, 0, 1);
          VP_PARAM_INT_WITH_MAX(share, "pcm", pk_correspond_mode, 0, 1);
          VP_PARAM_INT_WITH_MAX(share, "stc", support_table_cache, 0, 2);
          VP_PARAM_STR(share, "tnl", tgt_table_name_list);
          VP_PARAM_STR(share, "tnp", tgt_table_name_prefix);
          VP_PARAM_STR(share, "tns", tgt_table_name_suffix);
          VP_PARAM_INT_WITH_MAX(share, "zru", zero_record_update_mode, 0, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
#ifndef WITHOUT_VP_BG_ACCESS
        case 8:
          VP_PARAM_INT_WITH_MAX(share, "bgs_mode", bgs_mode, 0, 1);
          VP_PARAM_INT_WITH_MAX(share, "bgi_mode", bgi_mode, 0, 1);
          VP_PARAM_INT_WITH_MAX(share, "bgu_mode", bgu_mode, 0, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
#endif
        case 12:
          VP_PARAM_INT_WITH_MAX(share, "child_binlog", child_binlog, 0, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 15:
          VP_PARAM_STR(share, "table_name_list", tgt_table_name_list);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 16:
          VP_PARAM_INT_WITH_MAX(share, "multi_range_mode", multi_range_mode,
            0, 1);
          VP_PARAM_STR(share, "default_database", tgt_default_db_name);
          VP_PARAM_INT_WITH_MAX(share, "table_count_mode", table_count_mode,
            0, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 17:
          VP_PARAM_INT_WITH_MAX(share, "choose_table_mode", choose_table_mode,
            0, 1);
          VP_PARAM_STR(share, "table_name_prefix", tgt_table_name_prefix);
          VP_PARAM_STR(share, "table_name_suffix", tgt_table_name_suffix);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 18:
          VP_PARAM_INT_WITH_MAX(share, "pk_correspond_mode",
            pk_correspond_mode, 0, 1);
          VP_PARAM_INT_WITH_MAX(share, "allow_bulk_autoinc",
            allow_bulk_autoinc, 0, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 19:
          VP_PARAM_INT_WITH_MAX(share, "support_table_cache",
            support_table_cache, 0, 2);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 20:
          VP_PARAM_INT(share, "auto_increment_table", auto_increment_table, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 23:
          VP_PARAM_INT(share, "infomation_source_table", info_src_table, 0);
          VP_PARAM_INT_WITH_MAX(share, "zero_record_update_mode",
            zero_record_update_mode, 0, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 24:
          VP_PARAM_STR(share, "choose_ignore_table_list",
            choose_ignore_table_list);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 26:
          VP_PARAM_INT_WITH_MAX(share, "choose_table_mode_for_lock",
            choose_table_mode_for_lock, 0, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 27:
          VP_PARAM_INT_WITH_MAX(share, "allow_different_column_type",
            allow_different_column_type, 0, 1);
          error_num = param_string_parse.print_param_error();
          goto error;
        case 33:
          VP_PARAM_STR(share, "choose_ignore_table_list_for_lock",
            choose_ignore_table_list_for_lock);
          error_num = param_string_parse.print_param_error();
          goto error;
        default:
          error_num = param_string_parse.print_param_error();
          goto error;
      }

      /* Verify that the remainder of the parameter value is whitespace */
      if ((error_num = param_string_parse.has_extra_parameter_values()))
          goto error;
    }
  }

  if ((error_num = vp_set_table_info_default(
    share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
    part_elem,
    sub_elem,
#endif
    table
  )))
    goto error;

  if (create_table)
  {
    DBUG_PRINT("info",
      ("vp tgt_default_db_name_length = %u",
      share->tgt_default_db_name_length));
    if (share->tgt_default_db_name_length > VP_TABLE_INFO_MAX_LEN)
    {
      error_num = ER_VP_INVALID_TABLE_INFO_TOO_LONG_NUM;
      my_printf_error(error_num, ER_VP_INVALID_TABLE_INFO_TOO_LONG_STR,
        MYF(0), share->tgt_default_db_name, "default_database");
      goto error;
    }

/*
    DBUG_PRINT("info",
      ("vp tgt_table_name_list_length = %ld",
      share->tgt_table_name_list_length));
    if (share->tgt_table_name_list_length > VP_TABLE_INFO_MAX_LEN)
    {
      error_num = ER_VP_INVALID_TABLE_INFO_TOO_LONG_NUM;
      my_printf_error(error_num, ER_VP_INVALID_TABLE_INFO_TOO_LONG_STR,
        MYF(0), share->tgt_table_name_list, "table_name_list");
      goto error;
    }
*/

    DBUG_PRINT("info",
      ("vp tgt_table_name_prefix_length = %u",
      share->tgt_table_name_prefix_length));
    if (share->tgt_table_name_prefix_length > VP_TABLE_INFO_MAX_LEN)
    {
      error_num = ER_VP_INVALID_TABLE_INFO_TOO_LONG_NUM;
      my_printf_error(error_num, ER_VP_INVALID_TABLE_INFO_TOO_LONG_STR,
        MYF(0), share->tgt_table_name_prefix, "table_name_prefix");
      goto error;
    }

    DBUG_PRINT("info",
      ("vp tgt_table_name_suffix_length = %u",
      share->tgt_table_name_suffix_length));
    if (share->tgt_table_name_suffix_length > VP_TABLE_INFO_MAX_LEN)
    {
      error_num = ER_VP_INVALID_TABLE_INFO_TOO_LONG_NUM;
      my_printf_error(error_num, ER_VP_INVALID_TABLE_INFO_TOO_LONG_STR,
        MYF(0), share->tgt_table_name_suffix, "table_name_suffix");
      goto error;
    }
  }

  if (comment_string)
    vp_my_free(comment_string, MYF(0));
  DBUG_RETURN(0);

error:
  if (comment_string)
    vp_my_free(comment_string, MYF(0));
error_alloc_comment_string:
  DBUG_RETURN(error_num);
}

int vp_set_table_info_default(
  VP_SHARE *share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_element *part_elem,
  partition_element *sub_elem,
#endif
  TABLE *table
) {
  DBUG_ENTER("vp_set_table_info_default");
  if (!share->tgt_default_db_name)
  {
    DBUG_PRINT("info",("vp create default tgt_default_db_name"));
    share->tgt_default_db_name_length = table->s->db.length;
    if (
      !(share->tgt_default_db_name = vp_create_string(
        table->s->db.str,
        share->tgt_default_db_name_length))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (!share->tgt_table_name_prefix)
  {
    DBUG_PRINT("info",("vp create default tgt_table_name_prefix"));
    share->tgt_table_name_prefix_length = 0;
    if (
      !(share->tgt_table_name_prefix = vp_create_string(
        "",
        share->tgt_table_name_prefix_length))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (!share->tgt_table_name_suffix)
  {
    DBUG_PRINT("info",("vp create default tgt_table_name_suffix"));
    share->tgt_table_name_suffix_length = 0;
    if (
      !(share->tgt_table_name_suffix = vp_create_string(
        "",
        share->tgt_table_name_suffix_length))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (!share->tgt_table_name_list && table)
  {
    DBUG_PRINT("info",("vp create default tgt_table_name_list"));
    share->tgt_table_name_list_length = share->table_name_length;
    if (
      !(share->tgt_table_name_list = vp_create_table_name_string(
        table->s->table_name.str,
#ifdef WITH_PARTITION_STORAGE_ENGINE
        (part_elem ? part_elem->partition_name : NULL),
        (sub_elem ? sub_elem->partition_name : NULL)
#else
        NULL,
        NULL
#endif
      ))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (!share->choose_ignore_table_list)
  {
    DBUG_PRINT("info",("vp create default choose_ignore_table_list"));
    share->choose_ignore_table_list_length = 0;
    if (
      !(share->choose_ignore_table_list = vp_create_string(
        "",
        share->choose_ignore_table_list_length))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (!share->choose_ignore_table_list_for_lock)
  {
    DBUG_PRINT("info",("vp create default choose_ignore_table_list_for_lock"));
    share->choose_ignore_table_list_for_lock_length = 0;
    if (
      !(share->choose_ignore_table_list_for_lock = vp_create_string(
        "",
        share->choose_ignore_table_list_for_lock_length))
    ) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  if (share->choose_table_mode == -1)
    share->choose_table_mode = 0;
  if (share->choose_table_mode_for_lock == -1)
    share->choose_table_mode_for_lock = 1;
  if (share->multi_range_mode == -1)
    share->multi_range_mode = 1;
  if (share->pk_correspond_mode == -1)
    share->pk_correspond_mode = 0;
  if (share->info_src_table == -1)
    share->info_src_table = 0;
  if (share->table_count_mode == -1)
    share->table_count_mode = 0;
  if (share->support_table_cache == -1)
    share->support_table_cache = 1;
  if (share->auto_increment_table == -1)
    share->auto_increment_table = 1;
  if (share->child_binlog == -1)
    share->child_binlog = 0;
#ifndef WITHOUT_VP_BG_ACCESS
  if (share->bgs_mode == -1)
    share->bgs_mode = 0;
  if (share->bgi_mode == -1)
    share->bgi_mode = 0;
  if (share->bgu_mode == -1)
    share->bgu_mode = 0;
#endif
  if (share->zero_record_update_mode == -1)
    share->zero_record_update_mode = 0;
  if (share->allow_bulk_autoinc == -1)
    share->allow_bulk_autoinc = 0;
  if (share->allow_different_column_type == -1)
    share->allow_different_column_type = 0;
  DBUG_RETURN(0);
}

VP_SHARE *vp_get_share(
  const char *table_name,
  TABLE *table,
  const THD *thd,
  ha_vp *vp,
  int *error_num
) {
  VP_SHARE *share;
  uint length;
  char *tmp_name;
  DBUG_ENTER("vp_get_share");

  length = (uint) strlen(table_name);
  pthread_mutex_lock(&vp_tbl_mutex);
  if (!(share = (VP_SHARE*) my_hash_search(&vp_open_tables,
    (uchar*) table_name, length)))
  {
    DBUG_PRINT("info",("vp create new share"));
    if (!(share = (VP_SHARE *)
      my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
        &share, sizeof(*share),
        &tmp_name, length + 1,
        NullS))
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc_share;
    }

    share->use_count = 0;
    share->table_name_length = length;
    share->table_name = tmp_name;
    strmov(share->table_name, table_name);

    if ((*error_num = vp_parse_table_info(share, table, 0)))
      goto error_parse_comment_string;

    if (
      (*error_num = vp_create_table_list(share)) ||
      (*error_num = vp_table_num_list_to_bitmap(share,
        share->choose_ignore_table_list, share->select_ignore)) ||
      (*error_num = vp_table_num_list_to_bitmap(share,
        share->choose_ignore_table_list_for_lock,
        share->select_ignore_with_lock))
    )
      goto error_create_table_list;

    if (share->table_count_mode)
      share->additional_table_flags |= HA_STATS_RECORDS_IS_EXACT;

    if (share->info_src_table > share->table_count)
      share->info_src_table = share->table_count;
    if (share->auto_increment_table > share->table_count)
      share->auto_increment_table = share->table_count;
    share->auto_increment_table--;

#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&share->mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(vp_key_mutex_share,
      &share->mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_mutex;
    }

#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&share->init_mutex, MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(vp_key_mutex_share_init,
      &share->init_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_init_mutex;
    }

    thr_lock_init(&share->lock);

#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (!(share->partition_share =
      vp_get_pt_share(table, share, error_num)))
      goto error_get_pt_share;
#endif

    if (my_hash_insert(&vp_open_tables, (uchar*) share))
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_hash_insert;
    }
  }
  share->use_count++;
  pthread_mutex_unlock(&vp_tbl_mutex);

  vp->share = share;

  DBUG_PRINT("info",("vp share=%p", share));
  DBUG_RETURN(share);

error_hash_insert:
#ifdef WITH_PARTITION_STORAGE_ENGINE
error_get_pt_share:
#endif
  thr_lock_delete(&share->lock);
  pthread_mutex_destroy(&share->init_mutex);
error_init_init_mutex:
  pthread_mutex_destroy(&share->mutex);
error_init_mutex:
error_create_table_list:
error_parse_comment_string:
  vp_free_share_alloc(share);
  vp_my_free(share, MYF(0));
error_alloc_share:
  pthread_mutex_unlock(&vp_tbl_mutex);
  DBUG_RETURN(NULL);
}

int vp_free_share(
  VP_SHARE *share
) {
  DBUG_ENTER("vp_free_share");
  pthread_mutex_lock(&vp_tbl_mutex);
  if (!--share->use_count)
  {
    vp_free_share_alloc(share);
    my_hash_delete(&vp_open_tables, (uchar*) share);
    thr_lock_delete(&share->lock);
    pthread_mutex_destroy(&share->init_mutex);
    pthread_mutex_destroy(&share->mutex);
    vp_my_free(share, MYF(0));
  }
  pthread_mutex_unlock(&vp_tbl_mutex);
  DBUG_RETURN(0);
}

#ifdef WITH_PARTITION_STORAGE_ENGINE
VP_PARTITION_SHARE *vp_get_pt_share(
  TABLE *table,
  VP_SHARE *share,
  int *error_num
) {
  VP_PARTITION_SHARE *partition_share;
  char *tmp_name;
  DBUG_ENTER("vp_get_pt_share");

  pthread_mutex_lock(&vp_pt_share_mutex);
  if (!(partition_share = (VP_PARTITION_SHARE*) my_hash_search(
    &vp_open_pt_share,
    (uchar*) table->s->path.str, table->s->path.length)))
  {
    DBUG_PRINT("info",("vp create new pt share"));
    if (!(partition_share = (VP_PARTITION_SHARE *)
      my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
        &partition_share, sizeof(*partition_share),
        &tmp_name, table->s->path.length + 1,
        NullS))
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc_share;
    }

    partition_share->use_count = 0;
    partition_share->table_name_length = table->s->path.length;
    partition_share->table_name = tmp_name;
    memcpy(partition_share->table_name, table->s->path.str,
      partition_share->table_name_length);

#if MYSQL_VERSION_ID < 50500
    if (pthread_mutex_init(&partition_share->pt_handler_mutex,
      MY_MUTEX_INIT_FAST))
#else
    if (mysql_mutex_init(vp_key_mutex_pt_handler,
      &partition_share->pt_handler_mutex, MY_MUTEX_INIT_FAST))
#endif
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_pt_handler_mutex;
    }

    if(
      my_hash_init(&partition_share->pt_handler_hash, system_charset_info,
        32, 0, 0, (my_hash_get_key) vp_pt_handler_share_get_key, 0, 0)
    ) {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_init_pt_handler_hash;
    }

    if (my_hash_insert(&vp_open_pt_share, (uchar*) partition_share))
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      goto error_hash_insert;
    }
  }
  partition_share->use_count++;
  pthread_mutex_unlock(&vp_pt_share_mutex);

  DBUG_PRINT("info",("vp partition_share=%p", partition_share));
  DBUG_RETURN(partition_share);

error_hash_insert:
  my_hash_free(&partition_share->pt_handler_hash);
error_init_pt_handler_hash:
  pthread_mutex_destroy(&partition_share->pt_handler_mutex);
error_init_pt_handler_mutex:
  vp_my_free(partition_share, MYF(0));
error_alloc_share:
  pthread_mutex_unlock(&vp_pt_share_mutex);
  DBUG_RETURN(NULL);
}

int vp_free_pt_share(
  VP_PARTITION_SHARE *partition_share
) {
  DBUG_ENTER("vp_free_pt_share");
  pthread_mutex_lock(&vp_pt_share_mutex);
  if (!--partition_share->use_count)
  {
    my_hash_delete(&vp_open_pt_share, (uchar*) partition_share);
    my_hash_free(&partition_share->pt_handler_hash);
    pthread_mutex_destroy(&partition_share->pt_handler_mutex);
    vp_my_free(partition_share, MYF(0));
  }
  pthread_mutex_unlock(&vp_pt_share_mutex);
  DBUG_RETURN(0);
}
#endif

bool vp_flush_logs(
  handlerton *hton
) {
  DBUG_ENTER("vp_flush_logs");
  DBUG_RETURN(FALSE);
}

handler* vp_create_handler(
  handlerton *hton,
  TABLE_SHARE *table, 
  MEM_ROOT *mem_root
) {
  DBUG_ENTER("vp_create_handler");
  DBUG_RETURN(new (mem_root) ha_vp(hton, table));
}

int vp_close_connection(
  handlerton*	hton,
  THD* thd
) {
  DBUG_ENTER("vp_close_connection");
  DBUG_RETURN(0);
}

void vp_drop_database(
  handlerton *hton,
  char* path
) {
  DBUG_ENTER("vp_drop_database");
  DBUG_VOID_RETURN;
}

bool vp_show_status(
  handlerton *hton,
  THD *thd, 
  stat_print_fn *stat_print,
  enum ha_stat_type stat_type
) {
  DBUG_ENTER("vp_show_status");
  switch (stat_type) {
    case HA_ENGINE_STATUS:
    default:
      DBUG_RETURN(FALSE);
	}
}

int vp_start_consistent_snapshot(
  handlerton *hton,
  THD* thd
) {
  DBUG_ENTER("vp_start_consistent_snapshot");
  DBUG_RETURN(0);
}

int vp_commit(
  handlerton *hton,
  THD *thd,
  bool all
) {
  DBUG_ENTER("vp_commit");
  DBUG_RETURN(0);
}

int vp_rollback(
  handlerton *hton,
  THD *thd,
  bool all
) {
  DBUG_ENTER("vp_rollback");
  DBUG_RETURN(0);
}

int vp_xa_prepare(
  handlerton *hton,
  THD* thd,
  bool all
) {
  DBUG_ENTER("vp_xa_prepare");
  DBUG_RETURN(0);
}

int vp_xa_recover(
  handlerton *hton,
  XID* xid_list,
  uint len
) {
  DBUG_ENTER("vp_xa_recover");
  DBUG_RETURN(0);
}

int vp_xa_commit_by_xid(
  handlerton *hton,
  XID* xid
) {
  DBUG_ENTER("vp_xa_commit_by_xid");
  DBUG_RETURN(0);
}

int vp_xa_rollback_by_xid(
  handlerton *hton,
  XID* xid
) {
  DBUG_ENTER("vp_xa_rollback_by_xid");
  DBUG_RETURN(0);
}

int vp_db_done(
  void *p
) {
  DBUG_ENTER("vp_db_done");
#ifdef WITH_PARTITION_STORAGE_ENGINE
  my_hash_free(&vp_open_pt_share);
#endif
  my_hash_free(&vp_open_tables);
#ifdef WITH_PARTITION_STORAGE_ENGINE
  pthread_mutex_destroy(&vp_pt_share_mutex);
#endif
  pthread_mutex_destroy(&vp_tbl_mutex);
#ifndef WITHOUT_VP_BG_ACCESS
  pthread_attr_destroy(&vp_pt_attr);
#endif
  DBUG_RETURN(0);
}

int vp_panic(
  handlerton *hton,
  ha_panic_function type
) {
  DBUG_ENTER("vp_panic");
  DBUG_RETURN(0);
}

int vp_db_init(
  void *p
) {
  int error_num;
  handlerton *vp_hton = (handlerton *)p;
  DBUG_ENTER("vp_db_init");
  vp_hton_ptr = vp_hton;

  vp_hton->state = SHOW_OPTION_YES;
#ifdef PARTITION_HAS_EXTRA_ATTACH_CHILDREN
  vp_hton->flags = HTON_NO_FLAGS;
#else
  vp_hton->flags = HTON_NO_PARTITION;
#endif
#ifdef HTON_CAN_MERGE
  vp_hton->flags |= HTON_CAN_MERGE;
#endif
#ifdef HTON_CAN_MULTISTEP_MERGE
  vp_hton->flags |= HTON_CAN_MULTISTEP_MERGE;
#endif
#ifdef HTON_CAN_READ_CONNECT_STRING_IN_PARTITION
  vp_hton->flags |= HTON_CAN_READ_CONNECT_STRING_IN_PARTITION;
#endif
  /* vp_hton->db_type = DB_TYPE_VP; */
  /*
  vp_hton->savepoint_offset;
  vp_hton->savepoint_set = vp_savepoint_set;
  vp_hton->savepoint_rollback = vp_savepoint_rollback;
  vp_hton->savepoint_release = vp_savepoint_release;
  vp_hton->create_cursor_read_view = vp_create_cursor_read_view;
  vp_hton->set_cursor_read_view = vp_set_cursor_read_view;
  vp_hton->close_cursor_read_view = vp_close_cursor_read_view;
  */
  vp_hton->panic = vp_panic;
  vp_hton->close_connection = vp_close_connection;
  vp_hton->start_consistent_snapshot = vp_start_consistent_snapshot;
  vp_hton->flush_logs = vp_flush_logs;
  vp_hton->commit = vp_commit;
  vp_hton->rollback = vp_rollback;
  if (vp_param_support_xa())
  {
    vp_hton->prepare = vp_xa_prepare;
    vp_hton->recover = vp_xa_recover;
    vp_hton->commit_by_xid = vp_xa_commit_by_xid;
    vp_hton->rollback_by_xid = vp_xa_rollback_by_xid;
  }
  vp_hton->create = vp_create_handler;
  vp_hton->drop_database = vp_drop_database;
  vp_hton->show_status = vp_show_status;

#ifdef _WIN32
  HMODULE current_module = GetModuleHandle(NULL);
  vp_partition_hton_ptr = (handlerton *)
    GetProcAddress(current_module, "?partition_hton@@3PAUhandlerton@@A");
#ifndef VP_HAS_NEXT_THREAD_ID
  vp_db_att_thread_id = (ulong *)
    GetProcAddress(current_module, "?thread_id@@3KA");
#endif
#else
  vp_partition_hton_ptr = partition_hton;
#ifndef VP_HAS_NEXT_THREAD_ID
  vp_db_att_thread_id = &thread_id;
#endif
#endif

#ifdef HAVE_PSI_INTERFACE
  init_vp_psi_keys();
#endif

#ifndef WITHOUT_VP_BG_ACCESS
  if (pthread_attr_init(&vp_pt_attr))
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_pt_attr_init;
  }
  if (pthread_attr_setdetachstate(&vp_pt_attr, PTHREAD_CREATE_DETACHED))
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_pt_attr_setstate;
  }
#endif

#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&vp_tbl_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(vp_key_mutex_tbl,
    &vp_tbl_mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_tbl_mutex_init;
  }
#ifdef WITH_PARTITION_STORAGE_ENGINE
#if MYSQL_VERSION_ID < 50500
  if (pthread_mutex_init(&vp_pt_share_mutex, MY_MUTEX_INIT_FAST))
#else
  if (mysql_mutex_init(vp_key_mutex_pt_share,
    &vp_pt_share_mutex, MY_MUTEX_INIT_FAST))
#endif
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_pt_share_mutex_init;
  }
#endif

  if(
    my_hash_init(&vp_open_tables, system_charset_info, 32, 0, 0,
                   (my_hash_get_key) vp_tbl_get_key, 0, 0)
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_open_tables_hash_init;
  }
#ifdef WITH_PARTITION_STORAGE_ENGINE
  if(
    my_hash_init(&vp_open_pt_share, system_charset_info, 32, 0, 0,
                   (my_hash_get_key) vp_pt_share_get_key, 0, 0)
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error_open_pt_share_hash_init;
  }
#endif

  DBUG_RETURN(0);

#ifdef WITH_PARTITION_STORAGE_ENGINE
error_open_pt_share_hash_init:
#endif
  my_hash_free(&vp_open_tables);
error_open_tables_hash_init:
#ifdef WITH_PARTITION_STORAGE_ENGINE
  pthread_mutex_destroy(&vp_pt_share_mutex);
error_pt_share_mutex_init:
#endif
  pthread_mutex_destroy(&vp_tbl_mutex);
error_tbl_mutex_init:
#ifndef WITHOUT_VP_BG_ACCESS
error_pt_attr_setstate:
  pthread_attr_destroy(&vp_pt_attr);
error_pt_attr_init:
#endif
  DBUG_RETURN(error_num);
}

char *vp_create_string(
  const char *str,
  uint length
) {
  char *res;
  DBUG_ENTER("vp_create_string");
  if (!(res = (char*) my_malloc(length + 1, MYF(MY_WME))))
    DBUG_RETURN(NULL);
  memcpy(res, str, length);
  res[length] = '\0';
  DBUG_RETURN(res);
}

char *vp_create_table_name_string(
  const char *table_name,
  const char *part_name,
  const char *sub_name
) {
  char *res, *tmp;
  uint length = strlen(table_name);
  DBUG_ENTER("vp_create_table_name_string");
  if (part_name)
  {
    length += sizeof("#P#") - 1 + strlen(part_name);
    if (sub_name)
      length += sizeof("#SP#") - 1 + strlen(sub_name);
  }
  if (!(res = (char*) my_malloc(length + 1, MYF(MY_WME))))
    DBUG_RETURN(NULL);
  tmp = strmov(res, table_name);
  if (part_name)
  {
    tmp = strmov(tmp, "#P#");
    tmp = strmov(tmp, part_name);
    if (sub_name)
    {
      tmp = strmov(tmp, "#SP#");
      tmp = strmov(tmp, sub_name);
    }
  }
  DBUG_RETURN(res);
}

#ifdef WITH_PARTITION_STORAGE_ENGINE
void vp_get_partition_info(
  const char *table_name,
  uint table_name_length,
  const TABLE_SHARE *table_share,
  partition_info *part_info,
  partition_element **part_elem,
  partition_element **sub_elem
) {
  char tmp_name[FN_REFLEN + 1];
  partition_element *tmp_part_elem = NULL, *tmp_sub_elem = NULL;
  bool tmp_flg = FALSE, tmp_find_flg = FALSE;
  DBUG_ENTER("vp_get_partition_info");
  *part_elem = NULL;
  *sub_elem = NULL;
  if (!part_info)
    DBUG_VOID_RETURN;

  if (!memcmp(table_name + table_name_length - 5, "#TMP#", 5))
    tmp_flg = TRUE;

  DBUG_PRINT("info",("vp table_name=%s", table_name));
  List_iterator<partition_element> part_it(part_info->partitions);
  while ((*part_elem = part_it++))
  {
    if ((*part_elem)->subpartitions.elements)
    {
      List_iterator<partition_element> sub_it((*part_elem)->subpartitions);
      while ((*sub_elem = sub_it++))
      {
        if (VP_create_subpartition_name(
          tmp_name, FN_REFLEN + 1, table_share->path.str,
          (*part_elem)->partition_name, (*sub_elem)->partition_name,
          NORMAL_PART_NAME))
        {
          DBUG_VOID_RETURN;
        }
        DBUG_PRINT("info",("vp tmp_name=%s", tmp_name));
        if (!memcmp(table_name, tmp_name, table_name_length + 1))
          DBUG_VOID_RETURN;
        if (
          tmp_flg &&
          *(tmp_name + table_name_length - 5) == '\0' &&
          !memcmp(table_name, tmp_name, table_name_length - 5)
        ) {
          tmp_part_elem = *part_elem;
          tmp_sub_elem = *sub_elem;
          tmp_flg = FALSE;
          tmp_find_flg = TRUE;
        }
      }
    } else {
      if (VP_create_partition_name(
        tmp_name, FN_REFLEN + 1, table_share->path.str,
        (*part_elem)->partition_name, NORMAL_PART_NAME, TRUE))
      {
        DBUG_VOID_RETURN;
      }
      DBUG_PRINT("info",("vp tmp_name=%s", tmp_name));
      if (!memcmp(table_name, tmp_name, table_name_length + 1))
        DBUG_VOID_RETURN;
      if (
        tmp_flg &&
        *(tmp_name + table_name_length - 5) == '\0' &&
        !memcmp(table_name, tmp_name, table_name_length - 5)
      ) {
        tmp_part_elem = *part_elem;
        tmp_flg = FALSE;
        tmp_find_flg = TRUE;
      }
    }
  }
  if (tmp_find_flg)
  {
    *part_elem = tmp_part_elem;
    *sub_elem = tmp_sub_elem;
    DBUG_PRINT("info",("vp tmp find"));
    DBUG_VOID_RETURN;
  }
  *part_elem = NULL;
  *sub_elem = NULL;
  DBUG_PRINT("info",("vp no hit"));
  DBUG_VOID_RETURN;
}
#endif

int vp_create_table_list(
  VP_SHARE *share
) {
  int table_count, roop_count, length;
  char *tmp_ptr, *tmp_ptr2, *tmp_ptr3, *tmp_name_ptr, *tmp_path_ptr;
  DBUG_ENTER("vp_create_table_list");
  DBUG_PRINT("info",("vp tgt_table_name_list=%s",
    share->tgt_table_name_list));
  table_count = 1;
  tmp_ptr = share->tgt_table_name_list;
  while (*tmp_ptr == ' ')
    tmp_ptr++;
  while (TRUE)
  {
    if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
    {
      table_count++;
      tmp_ptr = tmp_ptr2 + 1;
      while (*tmp_ptr == ' ')
        tmp_ptr++;
    } else
      break;
  }
  share->use_tables_size = (table_count + 7) / 8;
  if (!(share->tgt_db_name = (char**)
    my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
      &share->tgt_db_name, sizeof(char*) * table_count,
      &share->tgt_table_name, sizeof(char*) * table_count,
      &share->part_tables, sizeof(TABLE_LIST) * table_count,
      &tmp_name_ptr, sizeof(char) * (
        share->tgt_table_name_list_length +
        share->tgt_default_db_name_length * table_count +
        share->tgt_table_name_prefix_length * table_count +
        share->tgt_table_name_suffix_length * table_count +
        2 * table_count
      ),
      &tmp_path_ptr, sizeof(char) * (
        share->tgt_table_name_list_length +
        share->tgt_default_db_name_length * table_count +
        share->tgt_table_name_prefix_length * table_count +
        share->tgt_table_name_suffix_length * table_count +
        4 * table_count
      ),
      &share->select_ignore,
      sizeof(uchar) * share->use_tables_size,
      &share->select_ignore_with_lock,
      sizeof(uchar) * share->use_tables_size,
      NullS))
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  tmp_ptr = share->tgt_table_name_list;
  while (*tmp_ptr == ' ')
    tmp_ptr++;
  roop_count = 0;
  while (TRUE)
  {
    if ((tmp_ptr2 = strchr(tmp_ptr, ' ')))
      *tmp_ptr2 = '\0';

    VP_TABLE_LIST_db_str(&share->part_tables[roop_count]) =
      share->tgt_db_name[roop_count] = tmp_name_ptr;

    if ((tmp_ptr3 = strchr(tmp_ptr, '.')))
    {
      /* exist database name */
      *tmp_ptr3 = '\0';
      length = strlen(tmp_ptr);
      memcpy(tmp_name_ptr, tmp_ptr, length + 1);
      tmp_name_ptr += length + 1;
      tmp_ptr = tmp_ptr3 + 1;
      VP_TABLE_LIST_db_length(&share->part_tables[roop_count]) = length;
    } else {
      memcpy(tmp_name_ptr, share->tgt_default_db_name,
        share->tgt_default_db_name_length + 1);
      tmp_name_ptr += share->tgt_default_db_name_length + 1;
      VP_TABLE_LIST_db_length(&share->part_tables[roop_count]) =
        share->tgt_default_db_name_length;
    }

    VP_TABLE_LIST_alias_str(&share->part_tables[roop_count]) =
      VP_TABLE_LIST_table_name_str(&share->part_tables[roop_count]) =
      share->tgt_table_name[roop_count] =
      tmp_name_ptr;
    memcpy(tmp_name_ptr, share->tgt_table_name_prefix,
      share->tgt_table_name_prefix_length);
    tmp_name_ptr += share->tgt_table_name_prefix_length;

    length = strlen(tmp_ptr);
    memcpy(tmp_name_ptr, tmp_ptr, length);
    tmp_name_ptr += length;

    memcpy(tmp_name_ptr, share->tgt_table_name_suffix,
      share->tgt_table_name_suffix_length + 1);
    tmp_name_ptr += share->tgt_table_name_suffix_length + 1;

#ifdef VP_TABLE_LIST_ALIAS_HAS_LENGTH
    VP_TABLE_LIST_alias_length(&share->part_tables[roop_count]) =
#endif
    VP_TABLE_LIST_table_name_length(&share->part_tables[roop_count]) =
      share->tgt_table_name_prefix_length + length +
      share->tgt_table_name_suffix_length;

    DBUG_PRINT("info",("vp db=%s",
      VP_TABLE_LIST_db_str(&share->part_tables[roop_count])));
    DBUG_PRINT("info",("vp table_name=%s",
      VP_TABLE_LIST_table_name_str(&share->part_tables[roop_count])));

    if (!tmp_ptr2)
      break;
    tmp_ptr = tmp_ptr2 + 1;
    while (*tmp_ptr == ' ')
      tmp_ptr++;
    roop_count++;
  }
  share->table_count = table_count;
  DBUG_RETURN(0);
}

int vp_correspond_columns(
  ha_vp *vp,
  TABLE *table,
  VP_SHARE *share,
  TABLE_SHARE *table_share,
  TABLE_LIST *part_tables,
  bool reinit
) {
  int error_num, roop_count, roop_count2, roop_count3, field_count, field_idx,
    key_count, bitmap_size;
  uint max_count, min_count;
  int *correspond_columns_p, *tmp_correspond_columns_p;
  int **correspond_columns_c_ptr, *correspond_columns_c,
    *tmp_correspond_columns_c;
  int *correspond_pt_columns_p, *tmp_correspond_pt_columns_p;
  int **uncorrespond_pt_columns_c_ptr, *uncorrespond_pt_columns_c,
    *tmp_uncorrespond_pt_columns_c;
  uchar *correspond_columns_bit, *tmp_correspond_columns_bit;
  VP_KEY *keys, *largest_key, *tmp_keys, *tmp_keys2, *tmp_keys3;
  uchar *keys_bit, *tmp_keys_bit;
  VP_CORRESPOND_KEY **correspond_pk, *correspond_keys_p,
    *tmp_correspond_keys_p, *tmp_correspond_keys_p2,
    *tmp_correspond_keys_p3, *tmp_correspond_keys_p4, *tmp_correspond_keys_p5,
    **correspond_keys_p_ptr;
  uchar *correspond_keys_bit, *tmp_correspond_keys_bit;
  bool correspond_flag, tmp_correspond_flag, different_column,
    different_column_pos, same_all_columns;
  uchar *need_converting, *same_columns, *need_searching,
    *need_full_col_for_update, *pk_in_read_index, *cpy_clm_bitmap;
  TABLE *part_table;
  TABLE_SHARE *part_table_share = NULL;
  Field *field, *field2, **fields;
  KEY *key_info, *key_info2;
  KEY_PART_INFO *key_part, *key_part2, *key_part3 = NULL;
  handlerton *hton;
  ulong *def_versions;
  uint key_parts2;
  DBUG_ENTER("vp_correspond_columns");
  if (share->init && reinit)
  {
    correspond_flag = TRUE;
    def_versions = share->def_versions;
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (def_versions[roop_count] !=
        part_tables[roop_count].table->s->get_table_def_version())
      {
        correspond_flag = FALSE;
        break;
      }
    }
    if (correspond_flag)
      DBUG_RETURN(0);
  }

  pthread_mutex_lock(&share->init_mutex);
  share->reinit = reinit;
  if (share->init && share->reinit)
  {
    correspond_flag = TRUE;
    def_versions = share->def_versions;
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      if (def_versions[roop_count] !=
        part_tables[roop_count].table->s->get_table_def_version())
      {
        correspond_flag = FALSE;
        break;
      }
    }
    if (correspond_flag)
      share->reinit = FALSE;
    else
      vp_my_free(share->correspond_columns_p, MYF(0));
  }

  if (!share->init || share->reinit)
  {
    field_count = 0;
    key_count = 0;
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      part_table_share = part_tables[roop_count].table->s;
      field_count += part_table_share->fields;
      key_count += part_table_share->keys;
    }

    bitmap_size = (table_share->fields + 7) / 8;
    if (!(correspond_columns_p = (int *)
      my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
        &correspond_columns_p,
        sizeof(int) * table_share->fields * share->table_count,
        &correspond_pt_columns_p,
        sizeof(int) * table_share->fields * share->table_count,
        &correspond_columns_c_ptr,
        sizeof(int *) * share->table_count,
        &uncorrespond_pt_columns_c_ptr,
        sizeof(int *) * share->table_count,
        &correspond_columns_c,
        sizeof(int) * field_count,
        &uncorrespond_pt_columns_c,
        sizeof(int) * field_count,
        &correspond_columns_bit,
        sizeof(uchar) * bitmap_size * (share->table_count + 1),
        &keys,
        sizeof(VP_KEY) * table_share->keys,
        &keys_bit,
        sizeof(uchar) * bitmap_size * table_share->keys,
        &correspond_pk,
        sizeof(VP_CORRESPOND_KEY *) * share->table_count,
        &correspond_keys_p_ptr,
        sizeof(VP_CORRESPOND_KEY *) * share->table_count,
        &correspond_keys_p,
        sizeof(VP_CORRESPOND_KEY) * key_count,
        &correspond_keys_bit,
        sizeof(uchar) * bitmap_size * key_count,
        &cpy_clm_bitmap, sizeof(uchar) * bitmap_size,
        &need_converting,
        sizeof(uchar) * share->use_tables_size,
        &same_columns,
        sizeof(uchar) * share->use_tables_size,
        &need_searching,
        sizeof(uchar) * share->use_tables_size,
        &need_full_col_for_update,
        sizeof(uchar) * share->use_tables_size,
        &pk_in_read_index,
        sizeof(uchar) * share->use_tables_size,
        &def_versions,
        sizeof(ulong) * share->table_count,
        NullS))
    ) {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error_alloc;
    }

    tmp_correspond_columns_p = correspond_columns_p;
    for (roop_count = 0; roop_count < share->table_count;
      roop_count++)
    {
      for (roop_count2 = 0; roop_count2 < (int) table_share->fields;
        roop_count2++, tmp_correspond_columns_p++)
        *tmp_correspond_columns_p = MAX_FIELDS;
    }
    same_all_columns = TRUE;
    tmp_correspond_columns_p = correspond_columns_p;
    tmp_correspond_pt_columns_p = correspond_pt_columns_p;
    tmp_correspond_columns_c = correspond_columns_c;
    tmp_uncorrespond_pt_columns_c = uncorrespond_pt_columns_c;
    tmp_correspond_columns_bit =
      correspond_columns_bit + bitmap_size;
    tmp_correspond_keys_p = correspond_keys_p;
    tmp_correspond_keys_p2 = NULL;
    tmp_correspond_keys_bit = correspond_keys_bit;
    for (roop_count = 0; roop_count < share->table_count; roop_count++)
    {
      correspond_columns_c_ptr[roop_count] = tmp_correspond_columns_c;
      uncorrespond_pt_columns_c_ptr[roop_count] = tmp_uncorrespond_pt_columns_c;
      correspond_keys_p_ptr[roop_count] = tmp_correspond_keys_p;
      part_table = part_tables[roop_count].table;
      part_table_share = part_table->s;
      hton = part_table_share->db_type();
      if (hton->db_type == DB_TYPE_PARTITION_DB)
      {
        hton = vp_get_default_part_db_type_from_partition(part_table_share);
      }
      DBUG_PRINT("info",("vp part_table_share->primary_key=%u",
        part_table_share->primary_key));
      if (
        hton->db_type == DB_TYPE_HEAP ||
        hton->db_type == DB_TYPE_MYISAM ||
        hton->db_type == DB_TYPE_MRG_MYISAM ||
        part_table_share->primary_key == MAX_KEY
      )
        vp_set_bit(need_full_col_for_update, roop_count);

      if (
        part_table->file->ha_table_flags() & HA_PRIMARY_KEY_IN_READ_INDEX &&
        part_table_share->primary_key != MAX_KEY
      )
        vp_set_bit(pk_in_read_index, roop_count);

      correspond_flag = FALSE;
      different_column = FALSE;
      different_column_pos = FALSE;
      DBUG_PRINT("info",("vp correspond table %s",
        part_table_share->table_name.str));
      for (roop_count2 = 0; roop_count2 < (int) part_table_share->fields;
        roop_count2++)
      {
        if ((field = find_field_in_table_sef(table,
          part_table_share->fieldnames.type_names[roop_count2])))
        {
          DBUG_PRINT("info",("vp correspond column %s",
            part_table_share->fieldnames.type_names[roop_count2]));

          field2 = part_table->field[roop_count2];

          DBUG_PRINT("info",("vp field->field_index=%u",
            field->field_index));
          DBUG_PRINT("info",("vp field2->field_index=%u",
            field2->field_index));
          if (field->field_index != field2->field_index)
          {
            different_column_pos = TRUE;
          }

#ifdef VP_CREATE_FIELD_FIELDPTR_FIELDPTR_REQUIRES_THDPTR
          Create_field create_field(current_thd, field2, NULL);
#else
          Create_field create_field(field2, NULL);
#endif
          if (!field->is_equal(&create_field))
          {
            DBUG_PRINT("info",("vp different column"));
            different_column = TRUE;
          }
          if (field->type() != field2->type())
          {
            if (share->allow_different_column_type)
            {
              DBUG_PRINT("info",("vp different column type"));
              different_column = TRUE;
            } else {
              error_num = ER_VP_DIFFERENT_COLUMN_TYPE_NUM;
              my_printf_error(error_num, ER_VP_DIFFERENT_COLUMN_TYPE_STR,
                MYF(0),
                part_table_share->table_name.str,
                part_table_share->fieldnames.type_names[roop_count2]);
              goto error_table_correspond;
            }
          }
          if (
            field->charset() != field2->charset() ||
            ((field->null_bit == 0) != (field2->null_bit == 0)) ||
            field->pack_length() != field2->pack_length()
          ) {
            DBUG_PRINT("info",("vp need converting"));
            DBUG_PRINT("info",("vp charset %p %p",
              field->charset(), field2->charset()));
            DBUG_PRINT("info",("vp null_bit %d %d",
              field->null_bit, field2->null_bit));
            DBUG_PRINT("info",("vp pack_length %u %u",
              field->pack_length(), field2->pack_length()));
            vp_set_bit(need_converting, roop_count);
          }

          field_idx = field->field_index;
          /* tmp_correspond_columns_p[parent field idx] = child field idx */
          tmp_correspond_columns_p[field_idx] = roop_count2;
          /* tmp_correspond_columns_c[child field idx] = parent field idx */
          tmp_correspond_columns_c[roop_count2] = field_idx;
          vp_set_bit(tmp_correspond_columns_bit, field_idx);
          vp_set_bit(correspond_columns_bit, field_idx);
          correspond_flag = TRUE;
        } else {
          DBUG_PRINT("info",("vp uncorrespond column %s",
            part_table_share->fieldnames.type_names[roop_count2]));
          tmp_correspond_columns_c[roop_count2] = MAX_FIELDS;
          DBUG_PRINT("info",("vp different column"));
          different_column = TRUE;
        }
      }
      if (!correspond_flag)
      {
        error_num = ER_VP_CANT_CORRESPOND_TABLE_NUM;
        my_printf_error(error_num, ER_VP_CANT_CORRESPOND_TABLE_STR, MYF(0),
          part_table_share->table_name.str);
        goto error_table_correspond;
      }
      if (
        !different_column &&
        part_table_share->fields == table_share->fields &&
        !different_column_pos &&
        !vp_bit_is_set(need_converting, roop_count)
      ) {
        DBUG_PRINT("info",("vp same_columns %d", roop_count));
        vp_set_bit(same_columns, roop_count);
      } else
        same_all_columns = FALSE;

      roop_count2 = 0;
      roop_count3 = 0;
      fields = part_table->file->get_full_part_fields();
      if (fields)
      {
        for (; *fields; ++fields)
        {
          field_idx = (*fields)->field_index;
          if (tmp_correspond_columns_c[field_idx] == MAX_FIELDS)
          {
            tmp_uncorrespond_pt_columns_c[roop_count2] = field_idx;
            ++roop_count2;
          } else {
            tmp_correspond_pt_columns_p[roop_count3] =
              tmp_correspond_columns_c[field_idx];
            ++roop_count3;
          }
        }
      }
      tmp_uncorrespond_pt_columns_c[roop_count2] = MAX_FIELDS;
      tmp_correspond_pt_columns_p[roop_count3] = MAX_FIELDS;

      part_table_share = part_tables[roop_count].table->s;
      key_info = part_table_share->key_info;
      for (roop_count2 = 0; roop_count2 < (int) part_table_share->keys;
        roop_count2++)
      {
        tmp_correspond_keys_p->table_idx = roop_count;
        tmp_correspond_keys_p->key_idx = roop_count2;
        tmp_correspond_keys_p->columns_bit = tmp_correspond_keys_bit;
        tmp_correspond_keys_p->next = tmp_correspond_keys_p2;
        tmp_correspond_keys_p2 = tmp_correspond_keys_p;
        for (
          roop_count3 = 0, key_part = key_info[roop_count2].key_part;
          roop_count3 <
            (int) vp_user_defined_key_parts(&key_info[roop_count2]);
          roop_count3++, key_part++
        ) {
          field_idx = tmp_correspond_columns_c[key_part->field->field_index];
          if (field_idx < MAX_FIELDS)
            vp_set_bit(tmp_correspond_keys_bit, field_idx);
        }
        if (vp_bit_is_set(pk_in_read_index, roop_count))
        {
          for (
            roop_count3 = 0,
            key_part = key_info[part_table_share->primary_key].key_part;
            roop_count3 <
              (int) vp_user_defined_key_parts(
                &key_info[part_table_share->primary_key]);
            roop_count3++, key_part++
          ) {
            field_idx = tmp_correspond_columns_c[key_part->field->field_index];
            if (field_idx < MAX_FIELDS)
              vp_set_bit(tmp_correspond_keys_bit, field_idx);
          }
        }
        tmp_correspond_keys_p++;
        tmp_correspond_keys_bit += bitmap_size;
      }

      tmp_correspond_columns_p += table_share->fields;
      tmp_correspond_pt_columns_p += table_share->fields;
      tmp_correspond_columns_c += part_table_share->fields;
      tmp_uncorrespond_pt_columns_c += part_table_share->fields;
      tmp_correspond_columns_bit += bitmap_size;
    }
    tmp_correspond_keys_p4 = tmp_correspond_keys_p2;

    for (roop_count = 0; roop_count < (int) table_share->fields; roop_count++)
    {
      if (!(vp_bit_is_set(correspond_columns_bit, roop_count)))
      {
        error_num = ER_VP_CANT_CORRESPOND_COLUMN_NUM;
        my_printf_error(error_num, ER_VP_CANT_CORRESPOND_COLUMN_STR, MYF(0),
          table_share->fieldnames.type_names[roop_count]);
        goto error_column_correspond;
      }
    }

    tmp_keys_bit = keys_bit;
    key_info = table_share->key_info;
    for (roop_count = 0; roop_count < (int) table_share->keys;
      roop_count++)
    {
      keys[roop_count].key_idx = roop_count;
      keys[roop_count].columns_bit = tmp_keys_bit;
      if (roop_count < (int) table_share->keys - 1)
      {
        keys[roop_count].key_length_next = &keys[roop_count + 1];
      }
      if (roop_count > 0)
      {
        keys[roop_count].key_length_prev = &keys[roop_count - 1];
      }
      for (
        roop_count2 = 0, key_part = key_info[roop_count].key_part;
        roop_count2 < (int) vp_user_defined_key_parts(&key_info[roop_count]);
        roop_count2++, key_part++
      ) {
        field_idx = key_part->field->field_index;
        vp_set_bit(tmp_keys_bit, field_idx);
      }
      tmp_keys_bit += bitmap_size;
    }

    tmp_keys = largest_key = &keys[0];
    while (tmp_keys)
    {
      max_count = vp_user_defined_key_parts(&key_info[tmp_keys->key_idx]);
      tmp_keys2 = tmp_keys->key_length_next;
      tmp_keys3 = tmp_keys;
      while (tmp_keys2)
      {
        if (max_count < vp_user_defined_key_parts(
          &key_info[tmp_keys2->key_idx]))
        {
          max_count = vp_user_defined_key_parts(&key_info[tmp_keys2->key_idx]);
          tmp_keys = tmp_keys2;
        }
        tmp_keys2 = tmp_keys2->key_length_next;
      }
      if (tmp_keys != tmp_keys3)
      {
        if (tmp_keys->key_length_next)
          tmp_keys->key_length_next->key_length_prev =
            tmp_keys->key_length_prev;
        tmp_keys->key_length_prev->key_length_next =
          tmp_keys->key_length_next;
        tmp_keys->key_length_next = tmp_keys3;
        tmp_keys->key_length_prev = tmp_keys3->key_length_prev;
        if (tmp_keys3->key_length_prev)
          tmp_keys3->key_length_prev->key_length_next = tmp_keys;
        else
          largest_key = tmp_keys;
        tmp_keys3->key_length_prev = tmp_keys;
      }
      DBUG_PRINT("info",("vp key_length=%d key_id=%d",
        vp_user_defined_key_parts(&key_info[tmp_keys->key_idx]),
        tmp_keys->key_idx));
      tmp_keys = tmp_keys->key_length_next;
    }

    /* correspond keys */
    tmp_keys = largest_key;
    tmp_correspond_keys_p = tmp_correspond_keys_p4;
    while (tmp_keys)
    {
      correspond_flag = FALSE;
      min_count = MAX_FIELDS;
      tmp_correspond_keys_p2 = tmp_correspond_keys_p;
      tmp_correspond_keys_p3 = NULL;
      while (tmp_correspond_keys_p2)
      {
        tmp_correspond_flag = TRUE;
        for (roop_count = 0; roop_count < bitmap_size; roop_count++)
        {
          if ((tmp_keys->columns_bit[roop_count] &
            tmp_correspond_keys_p2->columns_bit[roop_count]) !=
            tmp_keys->columns_bit[roop_count])
          {
            DBUG_PRINT("info",("vp uncorrespond key p1 %d and child %d-%d",
              tmp_keys->key_idx, tmp_correspond_keys_p2->table_idx,
              tmp_correspond_keys_p2->key_idx));
            tmp_correspond_flag = FALSE;
            break;
          }
        }
        if (tmp_correspond_flag)
        {
          part_table_share =
            part_tables[tmp_correspond_keys_p2->table_idx].table->s;
          key_info2 = part_table_share->key_info;
          if (
            (key_info[tmp_keys->key_idx].flags & HA_FULLTEXT) !=
            (key_info2[tmp_correspond_keys_p2->key_idx].flags & HA_FULLTEXT) ||
            (key_info[tmp_keys->key_idx].flags & HA_SPATIAL) !=
            (key_info2[tmp_correspond_keys_p2->key_idx].flags & HA_SPATIAL) ||
            key_info[tmp_keys->key_idx].algorithm !=
              key_info2[tmp_correspond_keys_p2->key_idx].algorithm
          ) {
            DBUG_PRINT("info",("vp uncorrespond key p2 %d and child %d-%d",
              tmp_keys->key_idx, tmp_correspond_keys_p2->table_idx,
              tmp_correspond_keys_p2->key_idx));
            DBUG_PRINT("info",("vp flags=%lu %lu",
              key_info[tmp_keys->key_idx].flags,
              key_info2[tmp_correspond_keys_p2->key_idx].flags));
            DBUG_PRINT("info",("vp algorithm=%u %u",
              key_info[tmp_keys->key_idx].algorithm,
              key_info2[tmp_correspond_keys_p2->key_idx].algorithm));
            tmp_correspond_flag = FALSE;
          } else if (
            (key_info[tmp_keys->key_idx].flags & HA_FULLTEXT) &&
            vp_user_defined_key_parts(&key_info[tmp_keys->key_idx]) !=
              vp_user_defined_key_parts(
                &key_info2[tmp_correspond_keys_p2->key_idx])
          ) {
            DBUG_PRINT("info",("vp uncorrespond key p2 %d and child %d-%d",
              tmp_keys->key_idx, tmp_correspond_keys_p2->table_idx,
              tmp_correspond_keys_p2->key_idx));
            DBUG_PRINT("info",("vp key_parts=%u %u",
              vp_user_defined_key_parts(&key_info[tmp_keys->key_idx]),
              vp_user_defined_key_parts(
                &key_info2[tmp_correspond_keys_p2->key_idx])));
            tmp_correspond_flag = FALSE;
          }
        }
        if (tmp_correspond_flag)
        {
          key_part = key_info[tmp_keys->key_idx].key_part;
          key_part2 = key_info2[tmp_correspond_keys_p2->key_idx].key_part;
          key_parts2 = vp_user_defined_key_parts(
            &key_info2[tmp_correspond_keys_p2->key_idx]);
          if (vp_bit_is_set(pk_in_read_index,
            tmp_correspond_keys_p2->table_idx))
            key_part3 = key_info2[part_table_share->primary_key].key_part;
          for (roop_count = 0;
            roop_count < (int) vp_user_defined_key_parts(
              &key_info[tmp_keys->key_idx]);
            roop_count++)
          {
            field = key_part[roop_count].field;
            if (roop_count < (int) key_parts2)
              field2 = key_part2[roop_count].field;
            else
              field2 = key_part3[roop_count - key_parts2].field;
            if (
              correspond_columns_p[table_share->fields *
              tmp_correspond_keys_p2->table_idx + field->field_index] !=
              field2->field_index
            ) {
              DBUG_PRINT("info",("vp uncorrespond key p3 %d and child %d-%d",
                tmp_keys->key_idx, tmp_correspond_keys_p2->table_idx,
                tmp_correspond_keys_p2->key_idx));
              tmp_correspond_flag = FALSE;
              break;
            }
          }
        }
        if (tmp_correspond_flag)
        {
          DBUG_PRINT("info",("vp correspond key %d and child %d-%d",
            tmp_keys->key_idx, tmp_correspond_keys_p2->table_idx,
            tmp_correspond_keys_p2->key_idx));
          if (tmp_correspond_keys_p2 == tmp_correspond_keys_p)
            tmp_correspond_keys_p = tmp_correspond_keys_p->next;
          else
            tmp_correspond_keys_p3->next = tmp_correspond_keys_p2->next;
          tmp_correspond_keys_p4 = tmp_correspond_keys_p2->next;
          tmp_correspond_keys_p2->next = tmp_keys->correspond_key;
          tmp_keys->correspond_key = tmp_correspond_keys_p2;
          correspond_flag = TRUE;
          tmp_correspond_keys_p2->key_parts =
            vp_user_defined_key_parts(
              &key_info2[tmp_correspond_keys_p2->key_idx]);
          if (min_count > tmp_correspond_keys_p2->key_parts)
          {
            min_count = tmp_correspond_keys_p2->key_parts;
            tmp_correspond_keys_p2->next_shortest =
              tmp_keys->shortest_correspond_key;
            tmp_keys->shortest_correspond_key = tmp_correspond_keys_p2;
          } else {
            tmp_correspond_keys_p5 = tmp_keys->shortest_correspond_key;
            while (TRUE)
            {
              if (!tmp_correspond_keys_p5->next_shortest)
              {
                tmp_correspond_keys_p5->next_shortest = tmp_correspond_keys_p2;
                break;
              }
              if (tmp_correspond_keys_p5->next_shortest->key_parts >
                tmp_correspond_keys_p2->key_parts)
              {
                tmp_correspond_keys_p2->next_shortest =
                  tmp_correspond_keys_p5->next_shortest;
                tmp_correspond_keys_p5->next_shortest = tmp_correspond_keys_p2;
                break;
              }
              tmp_correspond_keys_p5 = tmp_correspond_keys_p5->next_shortest;
            }
          }
          tmp_correspond_keys_p2 = tmp_correspond_keys_p4;
        } else {
          tmp_correspond_keys_p3 = tmp_correspond_keys_p2;
          tmp_correspond_keys_p2 = tmp_correspond_keys_p2->next;
        }
      }

      if (!correspond_flag && (tmp_keys2 = tmp_keys->key_length_prev))
      {
        DBUG_PRINT("info",("vp check key with same columns"));
        while (vp_user_defined_key_parts(&key_info[tmp_keys->key_idx]) ==
          vp_user_defined_key_parts(&key_info[tmp_keys2->key_idx]))
        {
          tmp_correspond_flag = TRUE;
          for (roop_count = 0; roop_count < bitmap_size; roop_count++)
          {
            if (tmp_keys->columns_bit[roop_count] !=
              tmp_keys2->columns_bit[roop_count])
            {
              DBUG_PRINT("info",("vp uncorrespond key(same size) %d and %d",
                tmp_keys->key_idx, tmp_keys2->key_idx));
              tmp_correspond_flag = FALSE;
              break;
            }
          }
          if (tmp_correspond_flag)
          {
            DBUG_PRINT("info",("vp correspond key(same size) %d and %d",
              tmp_keys->key_idx, tmp_keys2->key_idx));
            tmp_keys->correspond_key = tmp_keys2->correspond_key;
            tmp_keys->shortest_correspond_key =
              tmp_keys2->shortest_correspond_key;
            correspond_flag = TRUE;
            break;
          }
          if (!(tmp_keys2 = tmp_keys2->key_length_prev))
            break;
        }
      }

      if (!correspond_flag)
      {
        error_num = ER_VP_CANT_CORRESPOND_KEY_NUM;
        my_printf_error(error_num, ER_VP_CANT_CORRESPOND_KEY_STR, MYF(0),
          tmp_keys->key_idx);
        goto error_key_correspond;
      }
      tmp_keys = tmp_keys->key_length_next;
    }

    /* PK check */
    DBUG_PRINT("info",("vp PK check start"));
    if (share->pk_correspond_mode == 0)
    {
      tmp_correspond_keys_p = correspond_keys_p;
      key_info = &table_share->key_info[table_share->primary_key];
      key_part = key_info->key_part;
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        DBUG_PRINT("info",("vp roop_count=%d", roop_count));
        part_table_share = part_tables[roop_count].table->s;
        DBUG_PRINT("info",("vp part_table_share->primary_key=%d",
          part_table_share->primary_key));
        DBUG_PRINT("info",("vp parent key_parts=%d",
          vp_user_defined_key_parts(key_info)));
        DBUG_PRINT("info",("vp child key_parts=%d",
          vp_user_defined_key_parts(
            &part_table_share->key_info[part_table_share->primary_key])));
        if (
          part_table_share->primary_key == MAX_KEY ||
          vp_user_defined_key_parts(key_info) !=
            vp_user_defined_key_parts(
              &part_table_share->key_info[part_table_share->primary_key])
        ) {
          error_num = ER_VP_CANT_CORRESPOND_PK_NUM;
          my_printf_error(error_num, ER_VP_CANT_CORRESPOND_PK_STR, MYF(0),
            part_table_share->table_name.str);
          goto error_key_correspond;
        }
        key_info2 = &part_table_share->key_info[part_table_share->primary_key];
        key_part2 = key_info2->key_part;
        for (roop_count2 = 0;
          roop_count2 < (int) vp_user_defined_key_parts(key_info);
          roop_count2++)
        {
          DBUG_PRINT("info",("vp roop_count2=%d", roop_count2));
          field = key_part[roop_count2].field;
          field2 = key_part2[roop_count2].field;
          DBUG_PRINT("info",("vp correspond field index=%d",
            correspond_columns_p[table_share->fields *
            roop_count + field->field_index]));
          DBUG_PRINT("info",("vp current field index=%d",
            field2->field_index));
          if (
            correspond_columns_p[table_share->fields *
            roop_count + field->field_index] !=
            field2->field_index
          ) {
            error_num = ER_VP_CANT_CORRESPOND_PK_NUM;
            my_printf_error(error_num, ER_VP_CANT_CORRESPOND_PK_STR, MYF(0),
              part_table_share->table_name.str);
            goto error_key_correspond;
          }
        }
        correspond_pk[roop_count] =
          &tmp_correspond_keys_p[part_table_share->primary_key];
        tmp_correspond_keys_p += part_table_share->keys;
      }
    } else {
      tmp_correspond_keys_p = keys[table_share->primary_key].correspond_key;
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (
          !tmp_correspond_keys_p ||
          tmp_correspond_keys_p->table_idx != roop_count
        ) {
          part_table_share = part_tables[roop_count].table->s;
          error_num = ER_VP_CANT_CORRESPOND_PK_NUM;
          my_printf_error(error_num, ER_VP_CANT_CORRESPOND_PK_STR, MYF(0),
            part_table_share->table_name.str);
          goto error_key_correspond;
        }

        key_info2 = part_tables[roop_count].table->s->key_info;
        min_count = vp_user_defined_key_parts(
          &key_info2[tmp_correspond_keys_p->key_idx]);
        correspond_pk[roop_count] = tmp_correspond_keys_p;
        tmp_correspond_keys_p = tmp_correspond_keys_p->next;
        while (
          tmp_correspond_keys_p &&
          tmp_correspond_keys_p->table_idx == roop_count
        ) {
          if (min_count > vp_user_defined_key_parts(
            &key_info2[tmp_correspond_keys_p->key_idx]))
          {
            min_count = vp_user_defined_key_parts(
              &key_info2[tmp_correspond_keys_p->key_idx]);
            correspond_pk[roop_count] = tmp_correspond_keys_p;
          }
          tmp_correspond_keys_p = tmp_correspond_keys_p->next;
        }
      }
    }

    for (roop_count = 0; roop_count < share->table_count; roop_count++)
      def_versions[roop_count] =
        part_tables[roop_count].table->s->get_table_def_version();

    if (share->zero_record_update_mode)
    {
      for (roop_count = 0; roop_count < share->table_count; roop_count++)
      {
        if (vp_bit_is_set(share->select_ignore_with_lock, roop_count))
        {
          for (roop_count2 = 0; roop_count2 < bitmap_size; roop_count2++)
          {
            cpy_clm_bitmap[roop_count2] |= correspond_columns_bit[
              (roop_count * bitmap_size) + roop_count2];
          }
        }
      }
    }

    /* auto_increment column check */
    if (table->found_next_number_field)
    {
      part_table = part_tables[share->auto_increment_table].table;
      if (
        !part_table->found_next_number_field ||
        part_table->found_next_number_field->field_index !=
          correspond_columns_p[table_share->fields *
          share->auto_increment_table +
          table->found_next_number_field->field_index]
      ) {
        error_num = ER_VP_CANT_CORRESPOND_AUTO_INC_NUM;
        my_printf_error(error_num, ER_VP_CANT_CORRESPOND_AUTO_INC_STR, MYF(0),
          part_table_share->table_name.str);
        goto error_auto_inc_correspond;
      }
    }

    share->bitmap_size = bitmap_size;
    share->correspond_columns_p = correspond_columns_p;
    share->correspond_pt_columns_p = correspond_pt_columns_p;
    share->correspond_columns_c_ptr = correspond_columns_c_ptr;
    share->uncorrespond_pt_columns_c_ptr = uncorrespond_pt_columns_c_ptr;
    share->correspond_columns_bit = correspond_columns_bit + bitmap_size;
    share->all_columns_bit = correspond_columns_bit;
    share->keys = keys;
    share->largest_key = largest_key;
    share->correspond_pk = correspond_pk;
    share->correspond_keys_p_ptr = correspond_keys_p_ptr;
    share->cpy_clm_bitmap = cpy_clm_bitmap;
    share->need_converting = need_converting;
    share->same_all_columns = same_all_columns;
    share->same_columns = same_columns;
    share->need_searching = need_searching;
    share->need_full_col_for_update = need_full_col_for_update;
    share->pk_in_read_index = pk_in_read_index;
    share->def_versions = def_versions;
    share->init = TRUE;
    share->reinit = FALSE;

    vp->overwrite_index_bits();
  }
  pthread_mutex_unlock(&share->init_mutex);
  DBUG_RETURN(0);

error_auto_inc_correspond:
error_key_correspond:
error_column_correspond:
error_table_correspond:
  vp_my_free(correspond_columns_p, MYF(0));
error_alloc:
  pthread_mutex_unlock(&share->init_mutex);
  DBUG_RETURN(error_num);
}

uchar vp_bit_count(
  uchar bitmap
) {
  DBUG_ENTER("vp_bit_count");
  DBUG_PRINT("info",("vp bitmap=%d", bitmap));
  bitmap = ((bitmap & 0xaa) >> 1) + (bitmap & 0x55);
  bitmap = ((bitmap & 0xcc) >> 2) + (bitmap & 0x33);
  bitmap = ((bitmap & 0xf0) >> 4) + (bitmap & 0x0f);
  DBUG_PRINT("info",("vp bitcount=%d", bitmap));
  DBUG_RETURN(bitmap);
}

void *vp_bg_action(
  void *arg
) {
  VP_BG_BASE *base = (VP_BG_BASE *) arg;
  TABLE *table;
  THD *thd;
  my_thread_init();
  DBUG_ENTER("vp_bg_action");
  /* init start */
  if (!(thd = VP_new_THD(next_thread_id())))
  {
    pthread_mutex_lock(&base->bg_sync_mutex);
    pthread_cond_signal(&base->bg_sync_cond);
    pthread_mutex_unlock(&base->bg_sync_mutex);
    my_thread_end();
    DBUG_RETURN(NULL);
  }
  VP_set_next_thread_id(thd);
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_set_psi_id(thd->thread_id);
#endif
  thd->thread_stack = (char*) &thd;
  thd->store_globals();
  base->bg_thd = thd;
  pthread_mutex_lock(&base->bg_mutex);
  pthread_mutex_lock(&base->bg_sync_mutex);
  pthread_cond_signal(&base->bg_sync_cond);
  base->bg_init = TRUE;
  pthread_mutex_unlock(&base->bg_sync_mutex);
  /* init end */

  while (TRUE)
  {
    pthread_cond_wait(&base->bg_cond, &base->bg_mutex);
    DBUG_PRINT("info",("vp bg loop start"));
    if (base->bg_caller_sync_wait)
    {
      pthread_mutex_lock(&base->bg_sync_mutex);
      pthread_cond_signal(&base->bg_sync_cond);
      pthread_mutex_unlock(&base->bg_sync_mutex);
    }
    switch (base->bg_command)
    {
      case VP_BG_COMMAND_KILL:
        DBUG_PRINT("info",("vp bg kill start"));
        pthread_mutex_lock(&base->bg_sync_mutex);
        pthread_cond_signal(&base->bg_sync_cond);
        pthread_mutex_unlock(&base->bg_mutex);
        pthread_mutex_unlock(&base->bg_sync_mutex);
        delete thd;
#if !defined(MYSQL_DYNAMIC_PLUGIN) || !defined(_WIN32)
        my_pthread_setspecific_ptr(THR_THD, NULL);
#endif
        my_thread_end();
        DBUG_RETURN(NULL);
      case VP_BG_COMMAND_SELECT:
        DBUG_PRINT("info",("vp bg select start"));
        table = base->part_table->table;
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
        base->bg_error = table->file->ha_index_read_map(
          table->record[0], (const uchar *) base->table_key,
          base->tgt_key_part_map, HA_READ_KEY_EXACT);
#else
        base->bg_error = table->file->index_read_map(
          table->record[0], (const uchar *) base->table_key,
          base->tgt_key_part_map, HA_READ_KEY_EXACT);
#endif
        break;
      case VP_BG_COMMAND_INSERT:
        DBUG_PRINT("info",("vp bg insert start"));
        table = base->part_table->table;
/*
        table->next_number_field = table->found_next_number_field;
*/
        base->bg_error = table->file->ha_write_row(table->record[0]);
        table->next_number_field = NULL;
        table->auto_increment_field_not_null = FALSE;
        break;
      case VP_BG_COMMAND_UPDATE:
        table = base->part_table->table;
        base->bg_error = table->file->ha_update_row(
          table->record[1], table->record[0]);
        break;
      case VP_BG_COMMAND_DELETE:
        table = base->part_table->table;
        base->bg_error = table->file->ha_delete_row(table->record[0]);
        break;
      case VP_BG_COMMAND_UPDATE_SELECT:
        DBUG_PRINT("info",("vp bg update select start"));
        table = base->part_table->table;
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 50200
        base->bg_error = table->file->ha_index_read_idx_map(
          table->record[0], base->key_idx, (const uchar *) base->table_key,
          base->tgt_key_part_map, HA_READ_KEY_EXACT);
#else
        base->bg_error = table->file->index_read_idx_map(
          table->record[0], base->key_idx, (const uchar *) base->table_key,
          base->tgt_key_part_map, HA_READ_KEY_EXACT);
#endif
        if (!base->bg_error)
        {
          if (base->record_idx)
          {
            store_record(table, record[1]);
          } else {
            base->bg_error = table->file->ha_delete_row(table->record[0]);
          }
        }
        break;
    }
    continue;
  }
}

int vp_table_num_list_to_bitmap(
  VP_SHARE *share,
  char *table_num_list,
  uchar *bitmap
) {
  int table_idx;
  DBUG_ENTER("vp_table_num_list_to_bitmap");
  while(*table_num_list)
  {
    while (*table_num_list == ' ')
      table_num_list++;
    if (*table_num_list)
    {
      table_idx = atoi(table_num_list);
      if (table_idx > share->table_count || table_idx < 1)
      {
        my_printf_error(ER_VP_TBL_NUM_OUT_OF_RANGE_NUM,
          ER_VP_TBL_NUM_OUT_OF_RANGE_STR, MYF(0), table_idx);
        DBUG_RETURN(ER_VP_TBL_NUM_OUT_OF_RANGE_NUM);
      }
      vp_set_bit(bitmap, table_idx - 1);
    }
    if (!(table_num_list = strchr(table_num_list, ' ')))
      break;
  }
  DBUG_RETURN(0);
}
