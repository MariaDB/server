/* Copyright (c) 2026, MariaDB plc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "sql_plugin.h"
#include "my_base.h"            /* HA_KEY_BLOB_LENGTH */
extern st_plugin_int *json_index_plugin;;


/* Size of the hlindex (high-level index) table's `value` column (see
   json_index_table_def) -- the JSON scalar's payload, not counting its
   type tag, which is the table's separate `typ` column. 16 bytes fits a
   JSON_VALUE_NUMBER double (with room to spare) and an 8-byte prefix plus
   an 8-byte XXH3_64bits() hash exactly; a JSON_VALUE_STRING longer than
   that is reduced to that prefix+hash pair (see json_index_encode_token()
   in json.cc) rather than stored raw. */
#define JSON_INDEX_VALUE_MAX_LEN 16
/*
  Size of a fully key_copy()-encoded lookup key for the (typ,value) keypart
  prefix of the hlindex table's PRIMARY KEY: `typ`'s own single byte (a
  fixed-length NOT NULL column needs neither a null byte nor a length
  prefix), plus a HA_KEY_BLOB_LENGTH-byte length prefix and up to
  JSON_INDEX_VALUE_MAX_LEN payload bytes for `value` (see key_copy()'s
  HA_VAR_LENGTH_PART/HA_BLOB_PART handling). This, not
  JSON_INDEX_VALUE_MAX_LEN, is the buffer size callers of
  json_index_make_key() (and the range optimizer's SEL_ARG/KEY_PART for the
  hlindex key) must use.
*/
#define JSON_INDEX_KEY_MAX_LEN (1 + JSON_INDEX_VALUE_MAX_LEN + HA_KEY_BLOB_LENGTH)
