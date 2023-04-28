/* Copyright (C) 2010-2020 Kentoku Shiba
   Copyright (C) 2019-2020 MariaDB corp

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*
  This SQL script creates system tables for SPIDER
    or fixes incompatibilities if ones already exist.
*/

static LEX_STRING spider_init_queries[] = {
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_xa("
    "  format_id int not null default 0"
    ") engine=MyISAM default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "create table if not exists mysql.spider_xa_member("
    "  format_id int not null default 0"
    ") engine=Aria default charset=utf8 collate=utf8_bin"
  )},
  {C_STRING_WITH_LEN(
    "drop procedure if exists mysql.spider_fix_one_table"
  )}
};
