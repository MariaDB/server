/* Copyright (C) 2009-2014 Kentoku Shiba

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

typedef struct st_vp_copy_tables
{
  THD                        *thd;
  char                       *vp_db_name;
  uint                       vp_db_name_length;
  char                       *vp_table_name;
  uint                       vp_table_name_length;
  TABLE_LIST                 vp_table_list;

  int                        table_count[2];
  char                       **db_names[2];
  uint                       *db_names_length[2];
  char                       **table_names[2];
  uint                       *table_names_length[2];
  int                        *table_idx[2];

  int                        bulk_insert_interval;
  longlong                   bulk_insert_rows;
  int                        suppress_autoinc;

  char                       *default_database;
  char                       *table_name_prefix;
  char                       *table_name_suffix;

  uint                       default_database_length;
  uint                       table_name_prefix_length;
  uint                       table_name_suffix_length;
} VP_COPY_TABLES;

int vp_udf_copy_tables_create_table_list(
  VP_COPY_TABLES *copy_tables,
  char *vp_table_name,
  uint vp_table_name_length,
  char *src_table_name_list,
  uint src_table_name_list_length,
  char *dst_table_name_list,
  uint dst_table_name_list_length
);

int vp_udf_parse_copy_tables_param(
  VP_COPY_TABLES *copy_tables,
  char *param,
  int param_length
);

int vp_udf_set_copy_tables_param_default(
  VP_COPY_TABLES *copy_tables
);

void vp_udf_free_copy_tables_alloc(
  VP_COPY_TABLES *copy_tables
);
