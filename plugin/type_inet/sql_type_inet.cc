/* Copyright (c) 2011, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2014 MariaDB Foundation
   Copyright (c) 2019 MariaDB Corporation

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

#define MYSQL_SERVER
#include "mariadb.h"
#include "my_net.h"
#include "sql_class.h" // THD, SORT_FIELD_ATTR
#include "opt_range.h" // SEL_ARG
#include "sql_type_inet.h"

///////////////////////////////////////////////////////////////////////////

static const char HEX_DIGITS[]= "0123456789abcdef";


///////////////////////////////////////////////////////////////////////////

/**
  Tries to convert given string to binary IPv4-address representation.
  This is a portable alternative to inet_pton(AF_INET).

  @param      str          String to convert.
  @param      str_length   String length.

  @return Completion status.
  @retval true  - error, the given string does not represent an IPv4-address.
  @retval false - ok, the string has been converted sucessfully.

  @note The problem with inet_pton() is that it treats leading zeros in
  IPv4-part differently on different platforms.
*/

bool Inet4::ascii_to_ipv4(const char *str, size_t str_length)
{
  if (str_length < 7)
  {
    DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): "
                         "invalid IPv4 address: too short.",
                         (int) str_length, str));
    return true;
  }

  if (str_length > IN_ADDR_MAX_CHAR_LENGTH)
  {
    DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): "
                         "invalid IPv4 address: too long.",
                         (int) str_length, str));
    return true;
  }

  unsigned char *ipv4_bytes= (unsigned char *) &m_buffer;
  const char *str_end= str + str_length;
  const char *p= str;
  int byte_value= 0;
  int chars_in_group= 0;
  int dot_count= 0;
  char c= 0;

  while (p < str_end && *p)
  {
    c= *p++;

    if (my_isdigit(&my_charset_latin1, c))
    {
      ++chars_in_group;

      if (chars_in_group > 3)
      {
        DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                             "too many characters in a group.",
                             (int) str_length, str));
        return true;
      }

      byte_value= byte_value * 10 + (c - '0');

      if (byte_value > 255)
      {
        DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                             "invalid byte value.",
                             (int) str_length, str));
        return true;
      }
    }
    else if (c == '.')
    {
      if (chars_in_group == 0)
      {
        DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                             "too few characters in a group.",
                             (int) str_length, str));
        return true;
      }

      ipv4_bytes[dot_count]= (unsigned char) byte_value;

      ++dot_count;
      byte_value= 0;
      chars_in_group= 0;

      if (dot_count > 3)
      {
        DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                             "too many dots.", (int) str_length, str));
        return true;
      }
    }
    else
    {
      DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                           "invalid character at pos %d.",
                           (int) str_length, str, (int) (p - str)));
      return true;
    }
  }

  if (c == '.')
  {
    DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                         "ending at '.'.", (int) str_length, str));
    return true;
  }

  if (dot_count != 3)
  {
    DBUG_PRINT("error", ("ascii_to_ipv4(%.*s): invalid IPv4 address: "
                         "too few groups.",
                         (int) str_length, str));
    return true;
  }

  ipv4_bytes[3]= (unsigned char) byte_value;

  DBUG_PRINT("info", ("ascii_to_ipv4(%.*s): valid IPv4 address: %d.%d.%d.%d",
                      (int) str_length, str,
                      ipv4_bytes[0], ipv4_bytes[1],
                      ipv4_bytes[2], ipv4_bytes[3]));
  return false;
}


/**
  Tries to convert given string to binary IPv6-address representation.
  This is a portable alternative to inet_pton(AF_INET6).

  @param      str          String to convert.
  @param      str_length   String length.

  @return Completion status.
  @retval true  - error, the given string does not represent an IPv6-address.
  @retval false - ok, the string has been converted sucessfully.

  @note The problem with inet_pton() is that it treats leading zeros in
  IPv4-part differently on different platforms.
*/

bool Inet6::ascii_to_ipv6(const char *str, size_t str_length)
{
  if (str_length < 2)
  {
    DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: too short.",
                         (int) str_length, str));
    return true;
  }

  if (str_length > IN6_ADDR_MAX_CHAR_LENGTH)
  {
    DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: too long.",
                         (int) str_length, str));
    return true;
  }

  memset(m_buffer, 0, sizeof(m_buffer));

  const char *p= str;

  if (*p == ':')
  {
    ++p;

    if (*p != ':')
    {
      DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                           "can not start with ':x'.", (int) str_length, str));
      return true;
    }
  }

  const char *str_end= str + str_length;
  char *ipv6_bytes_end= m_buffer + sizeof(m_buffer);
  char *dst= m_buffer;
  char *gap_ptr= NULL;
  const char *group_start_ptr= p;
  int chars_in_group= 0;
  int group_value= 0;

  while (p < str_end && *p)
  {
    char c= *p++;

    if (c == ':')
    {
      group_start_ptr= p;

      if (!chars_in_group)
      {
        if (gap_ptr)
        {
          DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                               "too many gaps(::).", (int) str_length, str));
          return true;
        }

        gap_ptr= dst;
        continue;
      }

      if (!*p || p >= str_end)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "ending at ':'.", (int) str_length, str));
        return true;
      }

      if (dst + 2 > ipv6_bytes_end)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "too many groups (1).", (int) str_length, str));
        return true;
      }

      dst[0]= (unsigned char) (group_value >> 8) & 0xff;
      dst[1]= (unsigned char) group_value & 0xff;
      dst += 2;

      chars_in_group= 0;
      group_value= 0;
    }
    else if (c == '.')
    {
      if (dst + IN_ADDR_SIZE > ipv6_bytes_end)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "unexpected IPv4-part.", (int) str_length, str));
        return true;
      }

      Inet4_null tmp(group_start_ptr, (size_t) (str_end - group_start_ptr),
                     &my_charset_latin1);
      if (tmp.is_null())
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "invalid IPv4-part.", (int) str_length, str));
        return true;
      }

      tmp.to_binary(dst, IN_ADDR_SIZE);
      dst += IN_ADDR_SIZE;
      chars_in_group= 0;

      break;
    }
    else
    {
      const char *hdp= strchr(HEX_DIGITS, my_tolower(&my_charset_latin1, c));

      if (!hdp)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "invalid character at pos %d.",
                             (int) str_length, str, (int) (p - str)));
        return true;
      }

      if (chars_in_group >= 4)
      {
        DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                             "too many digits in group.",
                             (int) str_length, str));
        return true;
      }

      group_value <<= 4;
      group_value |= hdp - HEX_DIGITS;

      DBUG_ASSERT(group_value <= 0xffff);

      ++chars_in_group;
    }
  }

  if (chars_in_group > 0)
  {
    if (dst + 2 > ipv6_bytes_end)
    {
      DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                           "too many groups (2).", (int) str_length, str));
      return true;
    }

    dst[0]= (unsigned char) (group_value >> 8) & 0xff;
    dst[1]= (unsigned char) group_value & 0xff;
    dst += 2;
  }

  if (gap_ptr)
  {
    if (dst == ipv6_bytes_end)
    {
      DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                           "no room for a gap (::).", (int) str_length, str));
      return true;
    }

    int bytes_to_move= (int)(dst - gap_ptr);

    for (int i= 1; i <= bytes_to_move; ++i)
    {
      ipv6_bytes_end[-i]= gap_ptr[bytes_to_move - i];
      gap_ptr[bytes_to_move - i]= 0;
    }

    dst= ipv6_bytes_end;
  }

  if (dst < ipv6_bytes_end)
  {
    DBUG_PRINT("error", ("ascii_to_ipv6(%.*s): invalid IPv6 address: "
                         "too few groups.", (int) str_length, str));
    return true;
  }

  return false;
}


/**
  Converts IPv4-binary-address to a string. This function is a portable
  alternative to inet_ntop(AF_INET).

  @param[in] ipv4 IPv4-address data (byte array)
  @param[out] dst A buffer to store string representation of IPv4-address.
  @param[in]  dstsize Number of bytes avaiable in "dst"

  @note The problem with inet_ntop() is that it is available starting from
  Windows Vista, but the minimum supported version is Windows 2000.
*/

size_t Inet4::to_string(char *dst, size_t dstsize) const
{
  return (size_t) my_snprintf(dst, dstsize, "%d.%d.%d.%d",
                              (uchar) m_buffer[0], (uchar) m_buffer[1],
                              (uchar) m_buffer[2], (uchar) m_buffer[3]);
}


/**
  Converts IPv6-binary-address to a string. This function is a portable
  alternative to inet_ntop(AF_INET6).

  @param[in] ipv6 IPv6-address data (byte array)
  @param[out] dst A buffer to store string representation of IPv6-address.
                  It must be at least of INET6_ADDRSTRLEN.
  @param[in] dstsize Number of bytes available dst.

  @note The problem with inet_ntop() is that it is available starting from
  Windows Vista, but out the minimum supported version is Windows 2000.
*/

size_t Inet6::to_string(char *dst, size_t dstsize) const
{
  struct Region
  {
    int pos;
    int length;
  };

  const char *ipv6= m_buffer;
  char *dstend= dst + dstsize;
  const unsigned char *ipv6_bytes= (const unsigned char *) ipv6;

  // 1. Translate IPv6-address bytes to words.
  // We can't just cast to short, because it's not guaranteed
  // that sizeof (short) == 2. So, we have to make a copy.

  uint16 ipv6_words[IN6_ADDR_NUM_WORDS];

  DBUG_ASSERT(dstsize > 0); // Need a space at least for the trailing '\0'
  for (size_t i= 0; i < IN6_ADDR_NUM_WORDS; ++i)
    ipv6_words[i]= (ipv6_bytes[2 * i] << 8) + ipv6_bytes[2 * i + 1];

  // 2. Find "the gap" -- longest sequence of zeros in IPv6-address.

  Region gap= { -1, -1 };

  {
    Region rg= { -1, -1 };

    for (size_t i= 0; i < IN6_ADDR_NUM_WORDS; ++i)
    {
      if (ipv6_words[i] != 0)
      {
        if (rg.pos >= 0)
        {
          if (rg.length > gap.length)
            gap= rg;

          rg.pos= -1;
          rg.length= -1;
        }
      }
      else
      {
        if (rg.pos >= 0)
        {
          ++rg.length;
        }
        else
        {
          rg.pos= (int) i;
          rg.length= 1;
        }
      }
    }

    if (rg.pos >= 0)
    {
      if (rg.length > gap.length)
        gap= rg;
    }
  }

  // 3. Convert binary data to string.

  char *p= dst;

  for (int i= 0; i < (int) IN6_ADDR_NUM_WORDS; ++i)
  {
    DBUG_ASSERT(dstend >= p);
    size_t dstsize_available= dstend - p;
    if (dstsize_available < 5)
      break;
    if (i == gap.pos)
    {
      // We're at the gap position. We should put trailing ':' and jump to
      // the end of the gap.

      if (i == 0)
      {
        // The gap starts from the beginning of the data -- leading ':'
        // should be put additionally.

        *p= ':';
        ++p;
      }

      *p= ':';
      ++p;

      i += gap.length - 1;
    }
    else if (i == 6 && gap.pos == 0 &&
             (gap.length == 6 ||                           // IPv4-compatible
              (gap.length == 5 && ipv6_words[5] == 0xffff) // IPv4-mapped
             ))
    {
      // The data represents either IPv4-compatible or IPv4-mapped address.
      // The IPv6-part (zeros or zeros + ffff) has been already put into
      // the string (dst). Now it's time to dump IPv4-part.

      return (size_t) (p - dst) +
             Inet4_null((const char *) (ipv6_bytes + 12), 4).
               to_string(p, dstsize_available);
    }
    else
    {
      // Usual IPv6-address-field. Print it out using lower-case
      // hex-letters without leading zeros (recommended IPv6-format).
      //
      // If it is not the last field, append closing ':'.

      p += sprintf(p, "%x", ipv6_words[i]);

      if (i + 1 != IN6_ADDR_NUM_WORDS)
      {
        *p= ':';
        ++p;
      }
    }
  }

  *p= 0;
  return (size_t) (p - dst);
}


bool Inet6::fix_fields_maybe_null_on_conversion_to_inet6(Item *item)
{
  if (item->maybe_null())
    return true;
  if (item->type_handler() == &type_handler_inet6)
    return false;
  if (!item->const_item() || item->is_expensive())
    return true;
  return Inet6_null(item, false).is_null();
}


bool Inet6::make_from_item(Item *item, bool warn)
{
  if (item->type_handler() == &type_handler_inet6)
  {
    Native tmp(m_buffer, sizeof(m_buffer));
    bool rc= item->val_native(current_thd, &tmp);
    if (rc)
      return true;
    DBUG_ASSERT(tmp.length() == sizeof(m_buffer));
    if (tmp.ptr() != m_buffer)
      memcpy(m_buffer, tmp.ptr(), sizeof(m_buffer));
    return false;
  }
  StringBufferInet6 tmp;
  String *str= item->val_str(&tmp);
  return str ? make_from_character_or_binary_string(str, warn) : true;
}


bool Inet6::make_from_character_or_binary_string(const String *str, bool warn)
{
  static Name name= type_handler_inet6.name();
  if (str->charset() != &my_charset_bin)
  {
    bool rc= character_string_to_ipv6(str->ptr(), str->length(),
                                      str->charset());
    if (rc && warn)
      current_thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                            name.ptr(),
                                            ErrConvString(str).ptr());
    return rc;
  }
  if (str->length() != sizeof(m_buffer))
  {
    if (warn)
      current_thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                            name.ptr(),
                                            ErrConvString(str).ptr());
    return true;
  }
  DBUG_ASSERT(str->ptr() != m_buffer);
  memcpy(m_buffer, str->ptr(), sizeof(m_buffer));
  return false;
};


/********************************************************************/


class cmp_item_inet6: public cmp_item_scalar
{
  Inet6 m_native;
public:
  cmp_item_inet6()
   :cmp_item_scalar(),
    m_native(Inet6_zero())
  { }
  void store_value(Item *item) override
  {
    m_native= Inet6(item, &m_null_value);
  }
  int cmp_not_null(const Value *val) override
  {
    DBUG_ASSERT(!val->is_null());
    DBUG_ASSERT(val->is_string());
    Inet6_null tmp(val->m_string);
    DBUG_ASSERT(!tmp.is_null());
    return m_native.cmp(tmp);
  }
  int cmp(Item *arg) override
  {
    Inet6_null tmp(arg);
    return m_null_value || tmp.is_null() ? UNKNOWN : m_native.cmp(tmp) != 0;
  }
  int compare(cmp_item *ci) override
  {
    cmp_item_inet6 *tmp= static_cast<cmp_item_inet6*>(ci);
    DBUG_ASSERT(!m_null_value);
    DBUG_ASSERT(!tmp->m_null_value);
    return m_native.cmp(tmp->m_native);
  }
  cmp_item *make_same(THD *thd) override
  {
    return new (thd->mem_root) cmp_item_inet6();
  }
};


class Field_inet6: public Field
{
  static void set_min_value(char *ptr)
  {
    memset(ptr, 0, Inet6::binary_length());
  }
  static void set_max_value(char *ptr)
  {
    memset(ptr, 0xFF, Inet6::binary_length());
  }
  void store_warning(const ErrConv &str,
                     Sql_condition::enum_warning_level level)
  {
    static const Name type_name= type_handler_inet6.name();
    if (get_thd()->count_cuted_fields <= CHECK_FIELD_EXPRESSION)
      return;
    const TABLE_SHARE *s= table->s;
    get_thd()->push_warning_truncated_value_for_field(level, type_name.ptr(),
                                                      str.ptr(),
                                                      s ? s->db.str : nullptr,
                                                      s ? s->table_name.str
                                                      : nullptr,
                                                      field_name.str);
  }
  int set_null_with_warn(const ErrConv &str)
  {
    store_warning(str, Sql_condition::WARN_LEVEL_WARN);
    set_null();
    return 1;
  }
  int set_min_value_with_warn(const ErrConv &str)
  {
    store_warning(str, Sql_condition::WARN_LEVEL_WARN);
    set_min_value((char*) ptr);
    return 1;
  }
  int set_max_value_with_warn(const ErrConv &str)
  {
    store_warning(str, Sql_condition::WARN_LEVEL_WARN);
    set_max_value((char*) ptr);
    return 1;
  }
  int store_inet6_null_with_warn(const Inet6_null &inet6,
                                 const ErrConvString &err)
  {
    DBUG_ASSERT(marked_for_write_or_computed());
    if (inet6.is_null())
      return maybe_null() ? set_null_with_warn(err) :
                            set_min_value_with_warn(err);
    inet6.to_binary((char *) ptr, Inet6::binary_length());
    return 0;
  }

public:
  Field_inet6(const LEX_CSTRING *field_name_arg, const Record_addr &rec)
    :Field(rec.ptr(), Inet6::max_char_length(),
           rec.null_ptr(), rec.null_bit(), Field::NONE, field_name_arg)
  {
    flags|= BINARY_FLAG | UNSIGNED_FLAG;
  }
  const Type_handler *type_handler() const override
  {
    return &type_handler_inet6;
  }
  uint32 max_display_length() const override { return field_length; }
  bool str_needs_quotes() const override { return true; }
  const DTCollation &dtcollation() const override
  {
    static DTCollation_numeric c;
    return c;
  }
  CHARSET_INFO *charset(void) const override { return &my_charset_numeric; }
  const CHARSET_INFO *sort_charset(void) const override { return &my_charset_bin; }
  /**
    This makes client-server protocol convert the value according
    to @@character_set_client.
  */
  bool binary() const override { return false; }
  enum ha_base_keytype key_type() const override { return HA_KEYTYPE_BINARY; }

  bool is_equal(const Column_definition &new_field) const override
  {
    return new_field.type_handler() == type_handler();
  }
  bool eq_def(const Field *field) const override
  {
    return Field::eq_def(field);
  }
  double pos_in_interval(Field *min, Field *max) override
  {
    return pos_in_interval_val_str(min, max, 0);
  }
  int cmp(const uchar *a, const uchar *b) const override
  { return memcmp(a, b, pack_length()); }

  void sort_string(uchar *to, uint length) override
  {
    DBUG_ASSERT(length == pack_length());
    memcpy(to, ptr, length);
  }
  uint32 pack_length() const override
  {
    return Inet6::binary_length();
  }
  uint pack_length_from_metadata(uint field_metadata) const override
  {
    return Inet6::binary_length();
  }

  void sql_type(String &str) const override
  {
    static Name name= type_handler_inet6.name();
    str.set_ascii(name.ptr(), name.length());
  }

  void make_send_field(Send_field *to) override
  {
    Field::make_send_field(to);
    to->set_data_type_name(type_handler_inet6.name().lex_cstring());
  }

  bool validate_value_in_record(THD *thd, const uchar *record) const override
  {
    return false;
  }

  String *val_str(String *val_buffer,
                  String *val_ptr __attribute__((unused))) override
  {
    DBUG_ASSERT(marked_for_read());
    Inet6_null tmp((const char *) ptr, pack_length());
    return tmp.to_string(val_buffer) ? NULL : val_buffer;
  }

  my_decimal *val_decimal(my_decimal *to) override
  {
    DBUG_ASSERT(marked_for_read());
    my_decimal_set_zero(to);
    return to;
  }

  longlong val_int() override
  {
    DBUG_ASSERT(marked_for_read());
    return 0;
  }

  double val_real() override
  {
    DBUG_ASSERT(marked_for_read());
    return 0;
  }

  bool get_date(MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    DBUG_ASSERT(marked_for_read());
    set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
    return false;
  }

  bool val_bool(void) override
  {
    DBUG_ASSERT(marked_for_read());
    return !Inet6::only_zero_bytes((const char *) ptr, Inet6::binary_length());
  }

  int store_native(const Native &value) override
  {
    DBUG_ASSERT(marked_for_write_or_computed());
    DBUG_ASSERT(value.length() == Inet6::binary_length());
    memcpy(ptr, value.ptr(), value.length());
    return 0;
  }

  int store(const char *str, size_t length, CHARSET_INFO *cs) override
  {
    return cs == &my_charset_bin ? store_binary(str, length) :
                                   store_text(str, length, cs);
  }

  int store_text(const char *str, size_t length, CHARSET_INFO *cs) override
  {
    return store_inet6_null_with_warn(Inet6_null(str, length, cs),
                                      ErrConvString(str, length, cs));
  }

  int store_binary(const char *str, size_t length) override
  {
    return store_inet6_null_with_warn(Inet6_null(str, length),
                                      ErrConvString(str, length,
                                                    &my_charset_bin));
  }

  int store_hex_hybrid(const char *str, size_t length) override
  {
    return Field_inet6::store_binary(str, length);
  }

  int store_decimal(const my_decimal *num) override
  {
    DBUG_ASSERT(marked_for_write_or_computed());
    return set_min_value_with_warn(ErrConvDecimal(num));
  }

  int store(longlong nr, bool unsigned_flag) override
  {
    DBUG_ASSERT(marked_for_write_or_computed());
    return set_min_value_with_warn(
            ErrConvInteger(Longlong_hybrid(nr, unsigned_flag)));
  }

  int store(double nr) override
  {
    DBUG_ASSERT(marked_for_write_or_computed());
    return set_min_value_with_warn(ErrConvDouble(nr));
  }

  int store_time_dec(const MYSQL_TIME *ltime, uint dec) override
  {
    DBUG_ASSERT(marked_for_write_or_computed());
    return set_min_value_with_warn(ErrConvTime(ltime));
  }

  /*** Field conversion routines ***/
  int store_field(Field *from) override
  {
    // INSERT INTO t1 (inet6_field) SELECT different_field_type FROM t2;
    return from->save_in_field(this);
  }
  int save_in_field(Field *to) override
  {
    // INSERT INTO t2 (different_field_type) SELECT inet6_field FROM t1;
    if (to->charset() == &my_charset_bin &&
        dynamic_cast<const Type_handler_general_purpose_string*>
          (to->type_handler()))
    {
      NativeBufferInet6 res;
      val_native(&res);
      return to->store(res.ptr(), res.length(), &my_charset_bin);
    }
    return save_in_field_str(to);
  }
  Copy_func *get_copy_func(const Field *from) const override
  {
    // ALTER to INET6 from another field
    return do_field_string;
  }

  Copy_func *get_copy_func_to(const Field *to) const override
  {
    if (type_handler() == to->type_handler())
    {
      // ALTER from INET6 to INET6
      DBUG_ASSERT(pack_length() == to->pack_length());
      DBUG_ASSERT(charset() == to->charset());
      DBUG_ASSERT(sort_charset() == to->sort_charset());
      return Field::do_field_eq;
    }
    // ALTER from INET6 to another data type
    if (to->charset() == &my_charset_bin &&
        dynamic_cast<const Type_handler_general_purpose_string*>
          (to->type_handler()))
    {
      /*
        ALTER from INET6 to a binary string type, e.g.:
          BINARY, TINYBLOB, BLOB, MEDIUMBLOB, LONGBLOB
      */
      return do_field_inet6_native_to_binary;
    }
    return do_field_string;
  }

  static void do_field_inet6_native_to_binary(Copy_field *copy)
  {
    NativeBufferInet6 res;
    copy->from_field->val_native(&res);
    copy->to_field->store(res.ptr(), res.length(), &my_charset_bin);
  }

  bool memcpy_field_possible(const Field *from) const override
  {
    // INSERT INTO t1 (inet6_field) SELECT field2 FROM t2;
    return type_handler() == from->type_handler();
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override
  {
    if (type_handler() == source.type_handler() ||
        (source.type_handler() == &type_handler_string &&
         source.type_handler()->max_display_length_for_field(source) ==
         Inet6::binary_length()))
      return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
    return CONV_TYPE_IMPOSSIBLE;
  }

  /*** Optimizer routines ***/
  bool test_if_equality_guarantees_uniqueness(const Item *const_item) const override
  {
    /*
      This condition:
        WHERE inet6_field=const
      should return a single distinct value only,
      as comparison is done according to INET6.
    */
    return true;
  }
  bool can_be_substituted_to_equal_item(const Context &ctx,
                                        const Item_equal *item_equal)
                                        override
  {
    switch (ctx.subst_constraint()) {
    case ANY_SUBST:
      return ctx.compare_type_handler() == item_equal->compare_type_handler();
    case IDENTITY_SUBST:
      return true;
    }
    return false;
  }
  Item *get_equal_const_item(THD *thd, const Context &ctx,
                             Item *const_item) override;
  bool can_optimize_keypart_ref(const Item_bool_func *cond,
                                const Item *item) const override
  {
    /*
      Mixing of two different non-traditional types is currently prevented.
      This may change in the future. For example, INET4 and INET6
      data types can be made comparable.
    */
    DBUG_ASSERT(item->type_handler()->is_traditional_scalar_type() ||
                item->type_handler() == type_handler());
    return true;
  }
  /**
    Test if Field can use range optimizer for a standard comparison operation:
      <=, <, =, <=>, >, >=
    Note, this method does not cover spatial operations.
  */
  bool can_optimize_range(const Item_bool_func *cond,
                          const Item *item,
                          bool is_eq_func) const override
  {
    // See the DBUG_ASSERT comment in can_optimize_keypart_ref()
    DBUG_ASSERT(item->type_handler()->is_traditional_scalar_type() ||
                item->type_handler() == type_handler());
    return true;
  }
  SEL_ARG *get_mm_leaf(RANGE_OPT_PARAM *prm, KEY_PART *key_part,
                       const Item_bool_func *cond,
                       scalar_comparison_op op, Item *value) override
  {
    DBUG_ENTER("Field_inet6::get_mm_leaf");
    if (!can_optimize_scalar_range(prm, key_part, cond, op, value))
      DBUG_RETURN(0);
    int err= value->save_in_field_no_warnings(this, 1);
    if ((op != SCALAR_CMP_EQUAL && is_real_null()) || err < 0)
      DBUG_RETURN(&null_element);
    if (err > 0)
    {
      if (op == SCALAR_CMP_EQ || op == SCALAR_CMP_EQUAL)
        DBUG_RETURN(new (prm->mem_root) SEL_ARG_IMPOSSIBLE(this));
      DBUG_RETURN(NULL); /*  Cannot infer anything */
    }
    DBUG_RETURN(stored_field_make_mm_leaf(prm, key_part, op, value));
  }
  bool can_optimize_hash_join(const Item_bool_func *cond,
                                      const Item *item) const override
  {
    return can_optimize_keypart_ref(cond, item);
  }
  bool can_optimize_group_min_max(const Item_bool_func *cond,
                                  const Item *const_item) const override
  {
    return true;
  }

  uint row_pack_length() const override { return pack_length(); }

  Binlog_type_info binlog_type_info() const override
  {
    DBUG_ASSERT(type() == binlog_type());
    return Binlog_type_info_fixed_string(Field_inet6::binlog_type(),
                                         Inet6::binary_length(),
                                         &my_charset_bin);
  }

  uchar *pack(uchar *to, const uchar *from, uint max_length) override
  {
    DBUG_PRINT("debug", ("Packing field '%s'", field_name.str));
    return StringPack(&my_charset_bin, Inet6::binary_length()).
             pack(to, from, max_length);
  }

  const uchar *unpack(uchar *to, const uchar *from, const uchar *from_end,
                      uint param_data) override
  {
    return StringPack(&my_charset_bin, Inet6::binary_length()).
             unpack(to, from, from_end, param_data);
  }

  uint max_packed_col_length(uint max_length) override
  {
    return StringPack::max_packed_col_length(max_length);
  }

  uint packed_col_length(const uchar *data_ptr, uint length) override
  {
    return StringPack::packed_col_length(data_ptr, length);
  }

  /**********/
  uint size_of() const override { return sizeof(*this); }
};


class Item_typecast_inet6: public Item_func
{
public:
  Item_typecast_inet6(THD *thd, Item *a) :Item_func(thd, a) {}

  const Type_handler *type_handler() const override
  { return &type_handler_inet6; }

  enum Functype functype() const override { return CHAR_TYPECAST_FUNC; }
  bool eq(const Item *item, bool binary_cmp) const override
  {
    if (this == item)
      return true;
    if (item->type() != FUNC_ITEM ||
        functype() != ((Item_func*)item)->functype())
      return false;
    if (type_handler() != item->type_handler())
      return false;
    Item_typecast_inet6 *cast= (Item_typecast_inet6*) item;
    return args[0]->eq(cast->args[0], binary_cmp);
  }
  const char *func_name() const override { return "cast_as_inet6"; }
  void print(String *str, enum_query_type query_type) override
  {
    str->append(STRING_WITH_LEN("cast("));
    args[0]->print(str, query_type);
    str->append(STRING_WITH_LEN(" as inet6)"));
  }
  bool fix_length_and_dec() override
  {
    Type_std_attributes::operator=(Type_std_attributes_inet6());
    if (Inet6::fix_fields_maybe_null_on_conversion_to_inet6(args[0]))
      flags|= ITEM_FLAG_MAYBE_NULL;
    return false;
  }
  String *val_str(String *to) override
  {
    Inet6_null tmp(args[0]);
    return (null_value= tmp.is_null() || tmp.to_string(to)) ? NULL : to;
  }
  longlong val_int() override
  {
    return 0;
  }
  double val_real() override
  {
    return 0;
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    my_decimal_set_zero(to);
    return to;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
    return false;
  }
  bool val_native(THD *thd, Native *to) override
  {
    Inet6_null tmp(args[0]);
    return null_value= tmp.is_null() || tmp.to_native(to);
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_typecast_inet6>(thd, this); }
};


class Item_cache_inet6: public Item_cache
{
  NativeBufferInet6 m_value;
public:
  Item_cache_inet6(THD *thd)
   :Item_cache(thd, &type_handler_inet6)
  { }
  Item *get_copy(THD *thd)
  { return get_item_copy<Item_cache_inet6>(thd, this); }
  bool cache_value()
  {
    if (!example)
      return false;
    value_cached= true;
    null_value= example->val_native_with_conversion_result(current_thd,
                                                           &m_value,
                                                           type_handler());
    return true;
  }
  String* val_str(String *to)
  {
    if (!has_value())
      return NULL;
    Inet6_null tmp(m_value.ptr(), m_value.length());
    return tmp.is_null() || tmp.to_string(to) ? NULL : to;
  }
  my_decimal *val_decimal(my_decimal *to)
  {
    if (!has_value())
      return NULL;
    my_decimal_set_zero(to);
    return to;
  }
  longlong val_int()
  {
    if (!has_value())
      return 0;
    return 0;
  }
  double val_real()
  {
    if (!has_value())
      return 0;
    return 0;
  }
  longlong val_datetime_packed(THD *thd)
  {
    DBUG_ASSERT(0);
    if (!has_value())
      return 0;
    return 0;
  }
  longlong val_time_packed(THD *thd)
  {
    DBUG_ASSERT(0);
    if (!has_value())
      return 0;
    return 0;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate)
  {
    if (!has_value())
      return true;
    set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
    return false;
  }
  bool val_native(THD *thd, Native *to)
  {
    if (!has_value())
      return true;
    return to->copy(m_value.ptr(), m_value.length());
  }
};


class Item_literal_inet6: public Item_literal
{
  Inet6 m_value;
public:
  Item_literal_inet6(THD *thd)
   :Item_literal(thd),
    m_value(Inet6_zero())
  { }
  Item_literal_inet6(THD *thd, const Inet6 &value)
   :Item_literal(thd),
    m_value(value)
  { }
  const Type_handler *type_handler() const override
  {
    return &type_handler_inet6;
  }
  longlong val_int() override
  {
    return 0;
  }
  double val_real() override
  {
    return 0;
  }
  String *val_str(String *to) override
  {
    return m_value.to_string(to) ? NULL : to;
  }
  my_decimal *val_decimal(my_decimal *to) override
  {
    my_decimal_set_zero(to);
    return to;
  }
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  {
    set_zero_time(ltime, MYSQL_TIMESTAMP_TIME);
    return false;
  }
  bool val_native(THD *thd, Native *to) override
  {
    return m_value.to_native(to);
  }
  void print(String *str, enum_query_type query_type) override
  {
    StringBufferInet6 tmp;
    m_value.to_string(&tmp);
    str->append("INET6'");
    str->append(tmp);
    str->append('\'');
  }
  Item *get_copy(THD *thd) override
  { return get_item_copy<Item_literal_inet6>(thd, this); }

  // Non-overriding methods
  void set_value(const Inet6 &value)
  {
    m_value= value;
  }
};


class in_inet6 :public in_vector
{
  Inet6 m_value;
  static int cmp_inet6(void *cmp_arg, Inet6 *a, Inet6 *b)
  {
    return a->cmp(*b);
  }
public:
  in_inet6(THD *thd, uint elements)
   :in_vector(thd, elements, sizeof(Inet6), (qsort2_cmp) cmp_inet6, 0),
    m_value(Inet6_zero())
  { }
  const Type_handler *type_handler() const override
  {
    return &type_handler_inet6;
  }
  void set(uint pos, Item *item) override
  {
    Inet6 *buff= &((Inet6 *) base)[pos];
    Inet6_null value(item);
    if (value.is_null())
      *buff= Inet6_zero();
    else
      *buff= value;
  }
  uchar *get_value(Item *item) override
  {
    Inet6_null value(item);
    if (value.is_null())
      return 0;
    m_value= value;
    return (uchar *) &m_value;
  }
  Item* create_item(THD *thd) override
  {
    return new (thd->mem_root) Item_literal_inet6(thd);
  }
  void value_to_item(uint pos, Item *item) override
  {
    const Inet6 &buff= (((Inet6*) base)[pos]);
    static_cast<Item_literal_inet6*>(item)->set_value(buff);
  }
};


class Item_char_typecast_func_handler_inet6_to_binary:
                                       public Item_handled_func::Handler_str
{
public:
  const Type_handler *return_type_handler(const Item_handled_func *item)
                                          const override
  {
    if (item->max_length > MAX_FIELD_VARCHARLENGTH)
      return Type_handler::blob_type_handler(item->max_length);
    if (item->max_length > 255)
      return &type_handler_varchar;
    return &type_handler_string;
  }
  bool fix_length_and_dec(Item_handled_func *xitem) const override
  {
    return false;
  }
  String *val_str(Item_handled_func *item, String *to) const override
  {
    DBUG_ASSERT(dynamic_cast<const Item_char_typecast*>(item));
    return static_cast<Item_char_typecast*>(item)->
             val_str_binary_from_native(to);
  }
};


static Item_char_typecast_func_handler_inet6_to_binary
         item_char_typecast_func_handler_inet6_to_binary;


bool Type_handler_inet6::
  Item_char_typecast_fix_length_and_dec(Item_char_typecast *item) const
{
  if (item->cast_charset() == &my_charset_bin)
  {
    item->fix_length_and_dec_native_to_binary(Inet6::binary_length());
    item->set_func_handler(&item_char_typecast_func_handler_inet6_to_binary);
    return false;
  }
  item->fix_length_and_dec_str();
  return false;
}


bool
Type_handler_inet6::character_or_binary_string_to_native(THD *thd,
                                                         const String *str,
                                                         Native *to) const
{
  if (str->charset() == &my_charset_bin)
  {
    // Convert from a binary string
    if (str->length() != Inet6::binary_length() ||
        to->copy(str->ptr(), str->length()))
    {
      thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                    name().ptr(),
                                    ErrConvString(str).ptr());
      return true;
    }
    return false;
  }
  // Convert from a character string
  Inet6_null tmp(*str);
  if (tmp.is_null())
    thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                  name().ptr(),
                                  ErrConvString(str).ptr());
  return tmp.is_null() || tmp.to_native(to);
}


bool
Type_handler_inet6::Item_save_in_value(THD *thd,
                                       Item *item,
                                       st_value *value) const
{
  value->m_type= DYN_COL_STRING;
  String *str= item->val_str(&value->m_string);
  if (str != &value->m_string && !item->null_value)
  {
    // "item" returned a non-NULL value
    if (Inet6_null(*str).is_null())
    {
      /*
        The value was not-null, but conversion to INET6 failed:
          SELECT a, DECODE_ORACLE(inet6col, 'garbage', '<NULL>', '::01', '01')
          FROM t1;
      */
      thd->push_warning_wrong_value(Sql_condition::WARN_LEVEL_WARN,
                                    name().ptr(),
                                    ErrConvString(str).ptr());
      value->m_type= DYN_COL_NULL;
      return true;
    }
    // "item" returned a non-NULL value, and it was a valid INET6
    value->m_string.set(str->ptr(), str->length(), str->charset());
  }
  return check_null(item, value);
}


void Type_handler_inet6::Item_param_setup_conversion(THD *thd,
                                                     Item_param *param) const
{
  param->setup_conversion_string(thd, thd->variables.character_set_client);
}


void Type_handler_inet6::make_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  DBUG_ASSERT(item->type_handler() == this);
  NativeBufferInet6 tmp;
  item->val_native_result(current_thd, &tmp);
  if (item->maybe_null())
  {
    if (item->null_value)
    {
      memset(to, 0, Inet6::binary_length() + 1);
      return;
    }
    *to++= 1;
  }
  DBUG_ASSERT(!item->null_value);
  DBUG_ASSERT(Inet6::binary_length() == tmp.length());
  DBUG_ASSERT(Inet6::binary_length() == sort_field->length);
  memcpy(to, tmp.ptr(), tmp.length());
}

uint
Type_handler_inet6::make_packed_sort_key_part(uchar *to, Item *item,
                                            const SORT_FIELD_ATTR *sort_field,
                                            Sort_param *param) const
{
  DBUG_ASSERT(item->type_handler() == this);
  NativeBufferInet6 tmp;
  item->val_native_result(current_thd, &tmp);
  if (item->maybe_null())
  {
    if (item->null_value)
    {
      *to++=0;
      return 0;
    }
    *to++= 1;
  }
  DBUG_ASSERT(!item->null_value);
  DBUG_ASSERT(Inet6::binary_length() == tmp.length());
  DBUG_ASSERT(Inet6::binary_length() == sort_field->length);
  memcpy(to, tmp.ptr(), tmp.length());
  return tmp.length();
}

void Type_handler_inet6::sort_length(THD *thd,
                                     const Type_std_attributes *item,
                                     SORT_FIELD_ATTR *attr) const
{
  attr->original_length= attr->length= Inet6::binary_length();
  attr->suffix_length= 0;
}


cmp_item *Type_handler_inet6::make_cmp_item(THD *thd, CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_inet6;
}



in_vector *
Type_handler_inet6::make_in_vector(THD *thd, const Item_func_in *func,
                                   uint nargs) const
{
  return new (thd->mem_root) in_inet6(thd, nargs);
}


Item *Type_handler_inet6::create_typecast_item(THD *thd, Item *item,
                                               const Type_cast_attributes &attr)
                                               const
{
  return new (thd->mem_root) Item_typecast_inet6(thd, item);
}


Item_cache *Type_handler_inet6::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_inet6(thd);
}


Item *
Type_handler_inet6::make_const_item_for_comparison(THD *thd,
                                                   Item *src,
                                                   const Item *cmp) const
{
  Inet6_null tmp(src);
  if (tmp.is_null())
    return new (thd->mem_root) Item_null(thd, src->name.str);
  return new (thd->mem_root) Item_literal_inet6(thd, tmp);
}


Item *Field_inet6::get_equal_const_item(THD *thd, const Context &ctx,
                                        Item *const_item)
{
  Inet6_null tmp(const_item);
  if (tmp.is_null())
    return NULL;
  return new (thd->mem_root) Item_literal_inet6(thd, tmp);
}


Field *
Type_handler_inet6::make_table_field_from_def(
                                     TABLE_SHARE *share,
                                     MEM_ROOT *mem_root,
                                     const LEX_CSTRING *name,
                                     const Record_addr &addr,
                                     const Bit_addr &bit,
                                     const Column_definition_attributes *attr,
                                     uint32 flags) const
{
  return new (mem_root) Field_inet6(name, addr);
}


Field *Type_handler_inet6::make_table_field(MEM_ROOT *root,
                                            const LEX_CSTRING *name,
                                            const Record_addr &addr,
                                            const Type_all_attributes &attr,
                                            TABLE_SHARE *share) const
{
  return new (root) Field_inet6(name, addr);
}


Field *Type_handler_inet6::make_conversion_table_field(MEM_ROOT *root,
                                                       TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  const Record_addr tmp(NULL, Bit_addr(true));
  return new (table->in_use->mem_root) Field_inet6(&empty_clex_str, tmp);
}


bool Type_handler_inet6::partition_field_check(const LEX_CSTRING &field_name,
                                               Item *item_expr) const
{
  if (item_expr->cmp_type() != STRING_RESULT)
  {
    my_error(ER_WRONG_TYPE_COLUMN_VALUE_ERROR, MYF(0));
    return true;
  }
  return false;
}


bool
Type_handler_inet6::partition_field_append_value(
                                          String *to,
                                          Item *item_expr,
                                          CHARSET_INFO *field_cs,
                                          partition_value_print_mode_t mode)
                                          const
{
  StringBufferInet6 inet6str;
  Inet6_null inet6(item_expr);
  if (inet6.is_null())
  {
    my_error(ER_PARTITION_FUNCTION_IS_NOT_ALLOWED, MYF(0));
    return true;
  }
  return inet6.to_string(&inet6str) ||
         to->append('\'') ||
         to->append(inet6str) ||
         to->append('\'');
}


/***************************************************************/


class Type_collection_inet: public Type_collection
{
  const Type_handler *aggregate_common(const Type_handler *a,
                                       const Type_handler *b) const
  {
    if (a == b)
      return a;
    return NULL;
  }
  const Type_handler *aggregate_if_string(const Type_handler *a,
                                          const Type_handler *b) const
  {
    static const Type_aggregator::Pair agg[]=
    {
      {&type_handler_inet6, &type_handler_null,        &type_handler_inet6},
      {&type_handler_inet6, &type_handler_varchar,     &type_handler_inet6},
      {&type_handler_inet6, &type_handler_string,      &type_handler_inet6},
      {&type_handler_inet6, &type_handler_tiny_blob,   &type_handler_inet6},
      {&type_handler_inet6, &type_handler_blob,        &type_handler_inet6},
      {&type_handler_inet6, &type_handler_medium_blob, &type_handler_inet6},
      {&type_handler_inet6, &type_handler_long_blob,   &type_handler_inet6},
      {&type_handler_inet6, &type_handler_hex_hybrid,  &type_handler_inet6},
      {NULL,NULL,NULL}
    };
    return Type_aggregator::find_handler_in_array(agg, a, b, true);
  }
public:
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    const Type_handler *h;
    if ((h= aggregate_common(a, b)) ||
        (h= aggregate_if_string(a, b)))
      return h;
    return NULL;
  }

  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  {
    return aggregate_for_result(a, b);
  }

  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  {
    if (const Type_handler *h= aggregate_common(a, b))
      return h;
    static const Type_aggregator::Pair agg[]=
    {
      {&type_handler_inet6, &type_handler_null,      &type_handler_inet6},
      {&type_handler_inet6, &type_handler_long_blob, &type_handler_inet6},
      {NULL,NULL,NULL}
    };
    return Type_aggregator::find_handler_in_array(agg, a, b, true);
  }

  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  {
    return NULL;
  }

  const Type_handler *handler_by_name(const LEX_CSTRING &name) const override
  {
    if (type_handler_inet6.name().eq(name))
      return &type_handler_inet6;
    return NULL;
  }
};


const Type_collection *Type_handler_inet6::type_collection() const
{
  static Type_collection_inet type_collection_inet;
  return &type_collection_inet;
}
