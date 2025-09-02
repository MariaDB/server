#ifndef DATADICT_INCLUDED
#define DATADICT_INCLUDED
/* Copyright (c) 2010, Oracle and/or its affiliates.
   Copyright (c) 2017 MariaDB corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "handler.h"

/*
  Data dictionary API.
*/

enum Table_type
{
  TABLE_TYPE_UNKNOWN,
  TABLE_TYPE_NORMAL,                            /* Normal table */
  TABLE_TYPE_SEQUENCE,
  TABLE_TYPE_VIEW
};


#define INVISIBLE_MAX_BITS              3

/*
  Types of values in the MariaDB extra2 frm segment.
  Each value is written as
    type:       1 byte
    length:     1 byte  (1..255) or \0 and 2 bytes.
    binary value of the 'length' bytes.

  Older MariaDB servers can ignore values of unknown types if
  the type code is less than 128 (EXTRA2_ENGINE_IMPORTANT).
  Otherwise older (but newer than 10.0.1) servers are required
  to report an error.
*/
enum extra2_frm_value_type
{
  EXTRA2_TABLEDEF_VERSION=0,
  EXTRA2_DEFAULT_PART_ENGINE=1,
  EXTRA2_GIS=2,
  EXTRA2_APPLICATION_TIME_PERIOD=3,
  EXTRA2_PERIOD_FOR_SYSTEM_TIME=4,
  EXTRA2_INDEX_FLAGS=5,

#define EXTRA2_ENGINE_IMPORTANT 128

  EXTRA2_ENGINE_TABLEOPTS=128,
  EXTRA2_FIELD_FLAGS=129,
  EXTRA2_FIELD_DATA_TYPE_INFO=130,
  EXTRA2_PERIOD_WITHOUT_OVERLAPS=131,
};

enum extra2_field_flags
{
  VERS_OPTIMIZED_UPDATE= 1 << INVISIBLE_MAX_BITS,
};

enum extra2_index_flags
{
  EXTRA2_DEFAULT_INDEX_FLAGS,
  EXTRA2_IGNORED_KEY
};

#define FRM_HEADER_SIZE 64
#define FRM_FORMINFO_SIZE 288
#define FRM_MAX_SIZE (1024*1024)


inline size_t extra2_read_len(const uchar **pos, const uchar *end)
{
  size_t length= *(*pos)++;
  if (length)
    return length;

  if ((*pos) + 2 >= end)
    return 0;
  length= uint2korr(*pos);
  (*pos)+= 2;
  if (length < 256 || *pos + length > end)
    return 0;
  return length;
}

/*
  write the length as
  if (  0 < length <= 255)      one byte
  if (256 < length < ~65535)    zero byte, then two bytes, low-endian
*/
inline uchar *
extra2_write_len(uchar *pos, size_t len)
{
  DBUG_ASSERT(len);
  if (len <= 255)
    *pos++= (uchar)len;
  else
  {
    /*
      At the moment we support options_len up to 64K.
      We can easily extend it in the future, if the need arises.

      See build_frm_image():

        int2store(frm_header + 6, frm.length);

      frm.length includes FRM_HEADER_SIZE + extra2_size + 4
      and it must be 2 bytes, therefore extra2_size cannot be more than
      0xFFFF - FRM_HEADER_SIZE - 4.
    */
    DBUG_ASSERT(len <= 0xffff - FRM_HEADER_SIZE - 4);
    *pos++= 0;
    int2store(pos, len);
    pos+= 2;
  }
  return pos;
}

inline
uchar* extra2_write_str(uchar *pos, const LEX_CSTRING str)
{
  pos= extra2_write_len(pos, str.length);
  memcpy(pos, str.str, str.length);
  return pos + str.length;
}

inline uchar *
extra2_write(uchar *pos, enum extra2_frm_value_type type,
             const LEX_CSTRING &str)
{
  *pos++ = type;
  return extra2_write_str(pos, str);
}

inline uchar *
extra2_write(uchar *pos, enum extra2_frm_value_type type,
             const LEX_CUSTRING &str)
{
  return extra2_write(pos, type, *reinterpret_cast<const LEX_CSTRING*>(&str));
}

uchar *
extra2_write_field_properties(uchar *pos, List<Create_field> &create_fields);


struct Extra2_info
{
  LEX_CUSTRING version;
  LEX_CUSTRING options;
  Lex_ident_engine engine;
  LEX_CUSTRING gis;
  LEX_CUSTRING field_flags;
  LEX_CUSTRING system_period;
  LEX_CUSTRING application_period;
  LEX_CUSTRING field_data_type_info;
  LEX_CUSTRING without_overlaps;
  LEX_CUSTRING index_flags;

  uint read_size;
  size_t write_size;

  Extra2_info()
  {
    bzero((void*)this, sizeof(*this));
  }

  template <class S>
  size_t store_size(const S &f) const
  {
    if (f.length == 0)
      return 0;
    DBUG_ASSERT(f.length <= 65535);
    // 1 byte is for type; 1 or 3 bytes for length
    return f.length + (f.length <= 255 ? 2 : 4);
  }
  size_t store_size() const
  {
    return
      store_size(version) +
      store_size(options) +
      store_size(engine) +
      store_size(gis) +
      store_size(field_flags) +
      store_size(system_period) +
      store_size(application_period) +
      store_size(field_data_type_info) +
      store_size(without_overlaps) +
      store_size(index_flags);
  }

  bool read_once(LEX_CUSTRING *section, const uchar *pos, size_t len)
  {
    if (section->str)
      return true;

    *section= { pos, len };
    return false;
  }

  bool read(const uchar* frm_image, size_t frm_size);
  uchar * write(uchar* frm_image, size_t frm_size);
};


/*
  Take extra care when using dd_frm_type() - it only checks the .frm file,
  and it won't work for any engine that supports discovery.

  Prefer to use ha_table_exists() instead.
  To check whether it's an frm of a view, use dd_frm_is_view().
*/

enum Table_type dd_frm_type(THD *thd, char *path, LEX_CSTRING *engine_name,
                            LEX_CUSTRING *table_version);

static inline bool dd_frm_is_view(THD *thd, char *path)
{
  return dd_frm_type(thd, path, NULL, NULL) == TABLE_TYPE_VIEW;
}

bool dd_recreate_table(THD *thd, const char *db, const char *table_name);

#endif // DATADICT_INCLUDED
