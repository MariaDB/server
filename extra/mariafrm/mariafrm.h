/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2010, 2019, MariaDB

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

#include <vector>
#include <unordered_set>

#define TABLE_TYPE 510		//Magic number for table.frm files
#define VIEW_TYPE 22868		//Magic number for view.frm files

#define c_malloc(size)                                                        \
  ((char *) my_malloc(PSI_NOT_INSTRUMENTED, (size), MYF(MY_WME)))

struct label
{
  label(){};
  std::vector<LEX_CSTRING> names;
};

struct column
{
  column(){};
  LEX_CSTRING name;
  uint length;
  uint flags;   //field.h
  //enum utype unireg_check;
  uint unireg_check;
  enum enum_field_types type;
  LEX_CSTRING comment;
  uint charset_id;
  uint subtype;
  uint defaults_offset;
  uint null_byte;
  LEX_CSTRING default_value;
  int label_id;
  LEX_CSTRING extra_data_type_info;
};

struct key_part
{
  uint fieldnr;
  uint offset;
  uint key_part_flag;
  uint key_type;
  uint length;
};

struct key
{
  key(){};
  LEX_CSTRING name;
  LEX_CSTRING comment;
  uint flags;
  //uint fieldnr, length;
  uint key_info_length;
  key_part *key_parts;
  LEX_CSTRING column_name;
  bool is_unique;

  uint parts_count;
  enum ha_key_alg algorithm;
  uint key_block_size;
  LEX_CSTRING parser;
};

struct frm_file_data
{
  frm_file_data(){};
  uint mysql_version, keyinfo_offset, keyinfo_length;
  uint defaults_offset, defaults_length;
  uint extrainfo_offset, extrainfo_length;
  uint magic_number;
  uint names_length, forminfo_offset;
  uint screens_length;
  uint null_fields, column_count, labels_length, comments_length,
      metadata_offset, metadata_length;
  uint table_charset, min_rows, max_rows, avg_row_length, row_format;
  uint charset_primary_number;
  LEX_CSTRING table_cs_name;
  LEX_CSTRING table_coll_name;
  enum row_type  rtype;
  uint key_block_size, handler_option;
  LEX_CSTRING connect_string;
  LEX_CSTRING engine_name;
  uint legacy_db_type_1, legacy_db_type_2;
  LEX_CSTRING partition_info;

  char *connection;
  uint null_bit;
  column *columns;
  label *labels;

  uint key_count;
  key *keys;
  uint key_parts_count;
  uint key_extra_length;
  uint key_extra_info_offset;
  uint key_comment_offset;

  uint extra2_len;
  LEX_CUSTRING version;
  LEX_CUSTRING options;
  //Lex_ident engine;
  LEX_CUSTRING engine;
  LEX_CUSTRING gis;
  LEX_CUSTRING field_flags;
  LEX_CUSTRING system_period;
  LEX_CUSTRING application_period;
  LEX_CUSTRING field_data_type_info;
  LEX_CUSTRING without_overlaps;
  LEX_CUSTRING index_flags;

  LEX_CSTRING table_comment;
};

#define BYTES_PER_KEY 8
#define BYTES_PER_KEY_PART 9


#define FRM_HEADER_SIZE 64
#define FRM_FORMINFO_SIZE 288
#define FRM_MAX_SIZE (1024 * 1024)

static inline bool is_binary_frm_header(uchar *head)
{
  return head[0] == 254 && head[1] == 1 && head[2] >= FRM_VER &&
         head[2] <= FRM_VER_CURRENT;
}

#define FIELDFLAG_DECIMAL 1U
#define FIELDFLAG_ZEROFILL 4U
#define FIELDFLAG_NO_DEFAULT 16384U /* sql */
#define FIELDFLAG_MAYBE_NULL 32768U // sql
#define FIELDFLAG_DEC_SHIFT 8
#define FIELDFLAG_MAX_DEC 63U

#define f_is_dec(x) ((x) &FIELDFLAG_DECIMAL)
#define f_is_zerofill(x) ((x) &FIELDFLAG_ZEROFILL)
#define f_maybe_null(x) ((x) &FIELDFLAG_MAYBE_NULL)
#define f_no_default(x) ((x) &FIELDFLAG_NO_DEFAULT)
#define f_decimals(x)                                                         \
  ((uint8) (((x) >> FIELDFLAG_DEC_SHIFT) & FIELDFLAG_MAX_DEC))

inline bool is_temporal_type_with_date(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return true;
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP2:
    DBUG_ASSERT(0); // field->real_type() should not get to here.
    return false;
  default:
    return false;
  }
}
/*
enum row_type
{
  ROW_TYPE_NOT_USED= -1,
  ROW_TYPE_DEFAULT,
  ROW_TYPE_FIXED,
  ROW_TYPE_DYNAMIC,
  ROW_TYPE_COMPRESSED,
  ROW_TYPE_REDUNDANT,
  ROW_TYPE_COMPACT,
  ROW_TYPE_PAGE
};
*/
enum extra2_frm_value_type
{
  EXTRA2_TABLEDEF_VERSION= 0,
  EXTRA2_DEFAULT_PART_ENGINE= 1,
  EXTRA2_GIS= 2,
  EXTRA2_APPLICATION_TIME_PERIOD= 3,
  EXTRA2_PERIOD_FOR_SYSTEM_TIME= 4,
  EXTRA2_INDEX_FLAGS= 5,

#define EXTRA2_ENGINE_IMPORTANT 128

  EXTRA2_ENGINE_TABLEOPTS= 128,
  EXTRA2_FIELD_FLAGS= 129,
  EXTRA2_FIELD_DATA_TYPE_INFO= 130,
  EXTRA2_PERIOD_WITHOUT_OVERLAPS= 131,
};

enum geometry_types
{
  GEOM_GEOMETRY= 0,
  GEOM_POINT= 1,
  GEOM_LINESTRING= 2,
  GEOM_POLYGON= 3,
  GEOM_MULTIPOINT= 4,
  GEOM_MULTILINESTRING= 5,
  GEOM_MULTIPOLYGON= 6,
  GEOM_GEOMETRYCOLLECTION= 7
};

const char *legacy_db_types[29]= {"UNKNOWN",
                                  "DIAB_ISAM",
                                  "HASH",
                                  "MISAM",
                                  "PISAM",
                                  "RMS_ISAM",
                                  "HEAP",
                                  "ISAM",
                                  "MRG_ISAM",
                                  "MyISAM",
                                  "MRG_MYISAM",
                                  "BERKELEYDB",
                                  "InnoDB",
                                  "GEMINI",
                                  "NDBCLUSTER",
                                  "EXAMPLE_DB",
                                  "ARCHIVE_DB",
                                  "CSV",
                                  "FEDERATED",
                                  "BLACKHOLE",
                                  "PARTITION_DB",
                                  "BINLOG",
                                  "SOLID",
                                  "PBXT",
                                  "TABLE_FUNCTION",
                                  "MEMCACHE",
                                  "FALCON",
                                  "MARIA",
                                  "PERFORMANCE_SCHEMA"};
