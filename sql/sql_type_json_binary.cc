/*
   Copyright (c) 2019, Yubao Liu

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <algorithm>
#include <stack>
#include "sql_type_json_binary.h"
#include "sql_class.h"

// See https://github.com/mysql/mysql-server/blob/5.7/sql/json_binary.h
#define JSONB_TYPE_SMALL_OBJECT   0x0
#define JSONB_TYPE_LARGE_OBJECT   0x1
#define JSONB_TYPE_SMALL_ARRAY    0x2
#define JSONB_TYPE_LARGE_ARRAY    0x3
#define JSONB_TYPE_LITERAL        0x4
#define JSONB_TYPE_INT16          0x5
#define JSONB_TYPE_UINT16         0x6
#define JSONB_TYPE_INT32          0x7
#define JSONB_TYPE_UINT32         0x8
#define JSONB_TYPE_INT64          0x9
#define JSONB_TYPE_UINT64         0xA
#define JSONB_TYPE_DOUBLE         0xB
#define JSONB_TYPE_STRING         0xC
#define JSONB_TYPE_OPAQUE         0xF

#define JSONB_NULL_LITERAL        '\x00'
#define JSONB_TRUE_LITERAL        '\x01'
#define JSONB_FALSE_LITERAL       '\x02'


Type_handler_json_binary type_handler_json_binary;

const Name Type_handler_json_binary::m_name_json_binary(STRING_WITH_LEN("json"));


static const char* item_name(Item *a)
{
  return a->name.str ? a->name.str : a->full_name();
}

/**
  Read a variable length written by append_variable_length().

  @param[in] data  the buffer to read from
  @param[in] data_length  the maximum number of bytes to read from data
  @param[out] length  the length that was read
  @param[out] num  the number of bytes needed to represent the length
  @return  false on success, true on error
*/
static bool read_variable_length(const uchar *data, size_t data_length,
                                 size_t *length, size_t *num)
{
  /*
    It takes five bytes to represent UINT_MAX32, which is the largest
    supported length, so don't look any further.
  */
  const size_t max_bytes= std::min(data_length, static_cast<size_t>(5));

  size_t len= 0;
  for (size_t i= 0; i < max_bytes; i++)
  {
    // Get the next 7 bits of the length.
    len|= (data[i] & 0x7f) << (7 * i);
    if ((data[i] & 0x80) == 0)
    {
      // The length shouldn't exceed 32 bits.
      if (len > UINT_MAX32)
        return true;                          /* purecov: inspected */

      // This was the last byte. Return successfully.
      *num= i + 1;
      *length= len;
      return false;
    }
  }

  // No more available bytes. Return true to signal error.
  return true;                                /* purecov: inspected */
}

static void append_json_string(String *s, const char *buf, uint32 len)
{
  const char *end = buf + len;
  s->reserve(len + 1);
  while (buf != end) {
    switch (*buf) {
    case '"':
      s->append("\\\"", 2);
      break;
    case '\\':
      s->append("\\\\", 2);
      break;
    case '\b':
      s->append("\\b", 2);
      break;
    case '\f':
      s->append("\\f", 2);
      break;
    case '\n':
      s->append("\\n", 2);
      break;
    case '\r':
      s->append("\\r", 2);
      break;
    case '\t':
      s->append("\\t", 2);
      break;
    default:
      s->append(*buf);
      break;
    }
    ++buf;
  }
}

static uint32 json_stringify_scalar(String *s, char type, const uchar *buf, uint32 len)
{
  char numbuf[FLOATING_POINT_BUFFER + 1];

  switch (type) {
  case JSONB_TYPE_LITERAL:
    if (len == 0) return 0;
    switch (*buf) {
    case JSONB_NULL_LITERAL:
      s->append("null", strlen("null"));
      break;
    case JSONB_TRUE_LITERAL:
      s->append("true", strlen("true"));
      break;
    case JSONB_FALSE_LITERAL:
      s->append("false", strlen("false"));
      break;
    default:
      return 0;
    }
    return 1;

  case JSONB_TYPE_INT16:
    if (len < 2) return 0;
    int10_to_str(sint2korr(buf), numbuf, -10);
    s->append(numbuf);
    return 2;

  case JSONB_TYPE_UINT16:
    if (len < 2) return 0;
    int10_to_str(uint2korr(buf), numbuf, 10);
    s->append(numbuf);
    return 2;

  case JSONB_TYPE_INT32:
    if (len < 4) return 0;
    int10_to_str(sint4korr(buf), numbuf, -10);
    s->append(numbuf);
    return 4;

  case JSONB_TYPE_UINT32:
    if (len < 4) return 0;
    int10_to_str(uint4korr(buf), numbuf, 10);
    s->append(numbuf);
    return 4;

  case JSONB_TYPE_INT64:
    if (len < 8) return 0;
    longlong10_to_str(sint8korr(buf), numbuf, -10);
    s->append(numbuf);
    return 8;

  case JSONB_TYPE_UINT64:
    if (len < 8) return 0;
    longlong10_to_str(uint8korr(buf), numbuf, 10);
    s->append(numbuf);
    return 8;

  case JSONB_TYPE_DOUBLE:
    if (len < 8) return 0;
    {
      double d;
      size_t l;
      float8get(d, buf);
      l = my_gcvt(d, MY_GCVT_ARG_DOUBLE, FLOATING_POINT_BUFFER, numbuf, NULL);
      s->append(numbuf, l);
      return 8;
    }

  case JSONB_TYPE_STRING:
    if (len == 0) return 0;
    {
      size_t str_len;
      size_t n;
      if (read_variable_length(buf, len, &str_len, &n))
        return 0;
      if (len < n + str_len)
        return 0;
      s->append('"');
      append_json_string(s, (const char*)buf + n, str_len);
      s->append('"');
      return n + str_len;
    }

  case JSONB_TYPE_OPAQUE:
    if (len < 2) return 0;
    --len;
    type = *buf++;
    {
      enum_field_types field_type = static_cast<enum_field_types>(type);
      size_t blob_len;
      size_t n;
      if (read_variable_length(buf, len, &blob_len, &n))
        return 0;
      if (len < n + blob_len)
        return 0;
      // unsupported.
      return n + blob_len;
    }

  default:
    return 0;
  }
}

static void json_stringify_complex(String *s, char type, const uchar *buf, uint32 len)
{
  struct state_t
  {
    const uchar *buf;
    uint32 len;         // bytes of buf
    uint32 count;       // element count of object or array
    uint32 i;           // index to elemens in object or array
    uint32 offset_size; // 2 bytes for small, 4 bytes for large
    bool is_object;     // true: object, false: array
  };

  std::stack<state_t> states;

  uint32 count = 0;
  uint32 i = 0;
  uint32 offset_size = (type == JSONB_TYPE_SMALL_OBJECT || JSONB_TYPE_SMALL_ARRAY) ? 2 : 4;
  bool is_object = type < JSONB_TYPE_SMALL_ARRAY;

L_loop:
  if (len < 2 * offset_size) return;
  if (i == 0) {
    uint32 size;
    if (offset_size == 2) {
      count = uint2korr(buf);
      size = uint2korr(buf + 2);
    } else {
      count = uint4korr(buf);
      size = uint4korr(buf + 4);
    }
    if (len < size) return;
    len = size;
  }

  if (is_object) {
    if (count * (offset_size /* key-offset */ + 2 /* key length */ + 1 /* type */ + offset_size /* value */) > len) return;
  } else {
    if (count * (1 /* type */ + offset_size /* value */) > len) return;
  }

  if (i == 0) s->append(is_object ? '{' : '[');

  for (; i < count; ++i) {
    if (i > 0) s->append(", ", 2);

    const uchar *p;
    uint32 offset;
    if (is_object) {
      s->append('"');
      p = buf + 2 * offset_size + i * (offset_size + 2);
      offset = offset_size == 2 ? uint2korr(p) : uint4korr(p);
      uint16 l = uint2korr(p + offset_size);
      if (offset + l > len) return;
      append_json_string(s, (const char*)buf + offset, l);
      s->append("\": ", 3);

      p = buf + 2 * offset_size + count * (offset_size + 2) + i * (1 + offset_size);
    } else {
      p = buf + 2 * offset_size + i * (1 + offset_size);
    }

    switch (*p) {
    case JSONB_TYPE_SMALL_OBJECT:
    case JSONB_TYPE_LARGE_OBJECT:
    case JSONB_TYPE_SMALL_ARRAY:
    case JSONB_TYPE_LARGE_ARRAY:
      {
        state_t state;
        state.buf = buf;
        state.len = len;
        state.count = count;
        state.i = i + 1;
        state.offset_size = offset_size;
        state.is_object = is_object;
        states.push(state);
      }

      offset = offset_size == 2 ? uint2korr(p + 1) : uint4korr(p + 1);
      if (offset > len) return;
      buf += offset;
      len -= offset;
      count = 0;
      i = 0;
      offset_size = (*p == JSONB_TYPE_SMALL_OBJECT || *p == JSONB_TYPE_SMALL_ARRAY) ? 2 : 4;
      is_object = *p < JSONB_TYPE_SMALL_ARRAY;
      goto L_loop;
    case JSONB_TYPE_LITERAL:
    case JSONB_TYPE_INT16:
    case JSONB_TYPE_UINT16:
      json_stringify_scalar(s, *p, p + 1, 2);
      break;
    case JSONB_TYPE_INT32:
    case JSONB_TYPE_UINT32:
      if (offset_size == 4) {
        json_stringify_scalar(s, *p, p + 1, 4);
      } else {
        offset = uint2korr(p + 1);
        if (offset + 4 > len) return;
        json_stringify_scalar(s, *p, buf + offset, 4);
      }
      break;
    case JSONB_TYPE_INT64:
    case JSONB_TYPE_UINT64:
    case JSONB_TYPE_DOUBLE:
      offset = offset_size == 2 ? uint2korr(p + 1) : uint4korr(p + 1);
      if (offset + 8 > len) return;
      json_stringify_scalar(s, *p, buf + offset, 8);
      break;
    case JSONB_TYPE_STRING:
      offset = offset_size == 2 ? uint2korr(p + 1) : uint4korr(p + 1);
      if (0 == json_stringify_scalar(s, *p, buf + offset, len - offset))
        return;
      break;
    case JSONB_TYPE_OPAQUE:
      // unsupported
      break;
    default:
      break;
    }
  }

  s->append(is_object ? '}' : ']');

  if (! states.empty()) {
    const state_t& state = states.top();
    buf = state.buf;
    len = state.len;
    count = state.count;
    i = state.i;
    offset_size = state.offset_size;
    is_object = state.is_object;
    states.pop();
    goto L_loop;
  }
}

static void json_stringify(String *s, const uchar *buf, uint32 len)
{
  if (len == 0) return;

  if (*buf < JSONB_TYPE_LITERAL) {
    json_stringify_complex(s, *buf, buf + 1, len - 1);
  } else {
    json_stringify_scalar(s, *buf, buf + 1, len - 1);
  }
}

String *Field_json_binary::val_str(String *val_buffer,
                                   String *val_ptr)
{
  DBUG_ASSERT(marked_for_read());
  const uchar *blob = *(uchar**)(ptr + packlength);
  val_buffer->set_charset(charset());
  val_buffer->length(0);
  if (val_ptr != val_buffer) {
    val_ptr->set_charset(charset());
    val_ptr->length(0);
  }
  if (blob) {
    uint32 len = get_length(ptr);
    if (len == 0) {
      return val_buffer;
    } else if (*blob <= JSONB_TYPE_OPAQUE) {
      json_stringify(val_buffer, blob, len);
      return val_buffer;
    } else {
      // A hack to allow MariaDB updates JSON binary with JSON text in place,
      // the modified table can't be read by Oracle MySQL anymore.
      val_ptr->set((const char*)blob, len, charset());
      return val_ptr;
    }
  }
  return val_buffer;
}


Item *Type_handler_json_binary::
        create_typecast_item(THD *thd, Item *item,
                             const Type_cast_attributes &attr) const
{
  int len= -1;
  CHARSET_INFO *real_cs= attr.charset() ?
                         attr.charset() :
                         thd->variables.collation_connection;
  if (attr.length_specified())
  {
    if (attr.length() > MAX_FIELD_BLOBLENGTH)
    {
      my_error(ER_TOO_BIG_DISPLAYWIDTH, MYF(0), item_name(item),
               MAX_FIELD_BLOBLENGTH);
      return NULL;
    }
    len= (int) attr.length();
  }
  return new (thd->mem_root) Item_char_typecast(thd, item, len, real_cs);
}


Field *Type_handler_json_binary::make_conversion_table_field(TABLE *table,
                                                             uint metadata,
                                                             const Field *target)
                                                             const
{
  return new(table->in_use->mem_root)
         Field_json_binary(NULL, (uchar *) "", 1, Field::NONE, &empty_clex_str,
                           table->s, 4, target->charset());
}


Field *Type_handler_json_binary::
  make_table_field_from_def(TABLE_SHARE *share, MEM_ROOT *mem_root,
                            const LEX_CSTRING *name,
                            const Record_addr &rec, const Bit_addr &bit,
                            const Column_definition_attributes *attr,
                            uint32 flags) const
{
  return new (mem_root)
    Field_json_binary(rec.ptr(), rec.null_ptr(), rec.null_bit(),
                      attr->unireg_check, name, share,
                      attr->pack_flag_to_pack_length(), attr->charset);
}

Field *Type_handler_json_binary::make_table_field(const LEX_CSTRING *name,
                                                  const Record_addr &addr,
                                                  const Type_all_attributes &attr,
                                                  TABLE *table) const
{
  return new (table->in_use->mem_root)
         Field_json_binary(addr.ptr(), addr.null_ptr(), addr.null_bit(),
                           Field::NONE, name, table->s,
                           4, attr.collation);
}
