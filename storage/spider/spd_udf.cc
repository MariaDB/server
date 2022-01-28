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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#define MYSQL_SERVER 1
#include <my_global.h>
#include "spd_environ.h"
#include "mysql.h"
#include "spd_udf.h"

extern "C" {
long long spider_direct_sql(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  return spider_direct_sql_body(initid, args, is_null, error, FALSE);
}

my_bool spider_direct_sql_init(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  return spider_direct_sql_init_body(initid, args, message, FALSE);
}

void spider_direct_sql_deinit(
  UDF_INIT *initid
) {
  spider_direct_sql_deinit_body(initid);
}

long long spider_bg_direct_sql(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  return spider_direct_sql_bg_end(initid);
}

my_bool spider_bg_direct_sql_init(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  return spider_direct_sql_init_body(initid, args, message, TRUE);
}

void spider_bg_direct_sql_deinit(
  UDF_INIT *initid
) {
  spider_direct_sql_deinit_body(initid);
}

void spider_bg_direct_sql_clear(
  UDF_INIT *initid,
  char *is_null,
  char *error
) {
  spider_direct_sql_bg_start(initid);
}

void spider_bg_direct_sql_add(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  spider_direct_sql_body(initid, args, is_null, error, TRUE);
}

long long spider_ping_table(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  return spider_ping_table_body(initid, args, is_null, error);
}

my_bool spider_ping_table_init(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  return spider_ping_table_init_body(initid, args, message);
}

void spider_ping_table_deinit(
  UDF_INIT *initid
) {
  spider_ping_table_deinit_body(initid);
}

long long spider_flush_table_mon_cache(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  return spider_flush_table_mon_cache_body();
}

my_bool spider_flush_table_mon_cache_init(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  return FALSE;
}

void spider_flush_table_mon_cache_deinit(
  UDF_INIT *initid
) {
}

long long spider_copy_tables(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *is_null,
  char *error
) {
  return spider_copy_tables_body(initid, args, is_null, error);
}

my_bool spider_copy_tables_init(
  UDF_INIT *initid,
  UDF_ARGS *args,
  char *message
) {
  return spider_copy_tables_init_body(initid, args, message);
}

void spider_copy_tables_deinit(
  UDF_INIT *initid
) {
  spider_copy_tables_deinit_body(initid);
}
}
