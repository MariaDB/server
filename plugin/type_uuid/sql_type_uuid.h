#ifndef SQL_TYPE_UUID_INCLUDED
#define SQL_TYPE_UUID_INCLUDED

/* Copyright (c) 2019,2021 MariaDB Corporation

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

#include "sql_type_fixedbin_storage.h"

template <bool force_swap>
class UUID: public FixedBinTypeStorage<MY_UUID_SIZE, MY_UUID_STRING_LENGTH>
{
  bool get_digit(char ch, uint *val)
  {
    if (ch >= '0' && ch <= '9')
    {
      *val= (uint) ch - '0';
      return false;
    }
    if (ch >= 'a' && ch <= 'f')
    {
      *val= (uint) ch - 'a' + 0x0a;
      return false;
    }
    if (ch >= 'A' && ch <= 'F')
    {
      *val= (uint) ch - 'A' + 0x0a;
      return false;
    }
    return true;
  }

  bool get_digit(uint *val, const char *str, const char *end)
  {
    if (str >= end)
      return true;
    return get_digit(*str, val);
  }

  size_t skip_hyphens(const char *str, const char *end)
  {
    const char *str0= str;
    for ( ; str < end; str++)
    {
      if (str[0] != '-')
        break;
    }
    return str - str0;
  }

  const char *get_two_digits(char *val, const char *str, const char *end)
  {
    uint hi, lo;
    if (get_digit(&hi, str++, end))
      return NULL;
    str+= skip_hyphens(str, end);
    if (get_digit(&lo, str++, end))
      return NULL;
    *val= (char) ((hi << 4) + lo);
    return str;
  }

public:
  using FixedBinTypeStorage::FixedBinTypeStorage;
  bool ascii_to_fbt(const char *str, size_t str_length)
  {
    const char *end= str + str_length;
    /*
      The format understood:
      - Hyphen is not allowed on the first and the last position.
      - Otherwise, hyphens are allowed on any (odd and even) position,
        with any amount.
    */
    if (str_length < 32)
      goto err;

    for (uint oidx= 0; oidx < binary_length(); oidx++)
    {
      if (!(str= get_two_digits(&m_buffer[oidx], str, end)))
        goto err;
      // Allow hypheps after two digits, but not after the last digit
      if (oidx + 1 < binary_length())
        str+= skip_hyphens(str, end);
    }
    if (str < end)
      goto err; // Some input left
    if (m_buffer[6] & -m_buffer[8] & 0x80)
      goto err; // impossible combination: version >= 8, variant = 0
    return false;
  err:
    bzero(m_buffer, sizeof(m_buffer));
    return true;
  }

  size_t to_string(char *dst, size_t dstsize) const
  {
    my_uuid2str((const uchar *) m_buffer, dst, 1);
    return MY_UUID_STRING_LENGTH;
  }

  static const Name &default_value()
  {
    static Name def(STRING_WITH_LEN("00000000-0000-0000-0000-000000000000"));
    return def;
  }

  /*
    Binary (in-memory) UUIDv1 representation:

      llllllll-mmmm-Vhhh-vsss-nnnnnnnnnnnn

    Binary sortable (in-record) representation:

      nnnnnnnnnnnn-vsss-Vhhh-mmmm-llllllll

    Sign           Section               Bits   Bytes  Pos   PosBinSortable
    -------------  -------               ----   -----  ---   --------------
    llllllll       time low              32     4        0   12
    mmmm           time mid              16     2        4   10
    Vhhh           version and time hi   16     2        6   8
    vsss           variant and clock seq 16     2        8   6
    nnnnnnnnnnnn   node ID               48     6       10   0
  */

  class Segment
  {
    size_t m_memory_pos;
    size_t m_record_pos;
    size_t m_length;
  public:
    constexpr Segment(size_t memory_pos, size_t record_pos, size_t length)
     :m_memory_pos(memory_pos), m_record_pos(record_pos), m_length(length)
    { }
    void mem2rec(char *to, const char *from) const
    {
      memcpy(to + m_record_pos, from + m_memory_pos, m_length);
    }
    void rec2mem(char *to, const char * from) const
    {
      memcpy(to + m_memory_pos, from + m_record_pos, m_length);
    }
    int cmp_memory(const char *a, const char *b) const
    {
      return memcmp(a + m_memory_pos, b + m_memory_pos, m_length);
    }
    int cmp_swap_noswap(const char *a, const char *b) const
    {
      return memcmp(a + m_memory_pos, b + m_record_pos, m_length);
    }
    void hash_record(const uchar *ptr, Hasher *hasher) const
    {
      hasher->add(&my_charset_bin, ptr + m_record_pos, m_length);
    }
  };

  static const Segment & segment(uint i)
  {
    static Segment segments[]=
      {
        {0, 12, 4}, // llllllll
        {4, 10, 2}, // mmmm
        {6,  8, 2}, // Vhhh
        {8,  6, 2}, // vsss
        {10, 0, 6}  // nnnnnnnnnnnn
      };
    return segments[i];
  }

  // version > 0 && version < 6 && variant != 0
  static bool mem_need_swap(const char *s)
  { return s[6] > 0 && s[6] < 0x60 && s[8] & 0x80; }

  // s[6] & 0x80 && s[8] > 0: this means a swapped uuid
  static bool rec_need_swap(const char *s)
  { return s[6] & -s[8] & 0x80; }

  // Convert the in-memory representation to the in-record representation
  static void memory_to_record(char *to, const char *from)
  {
    if (force_swap || mem_need_swap(from))
    {
      segment(0).mem2rec(to, from);
      segment(1).mem2rec(to, from);
      segment(2).mem2rec(to, from);
      segment(3).mem2rec(to, from);
      segment(4).mem2rec(to, from);
    }
    else
      memcpy(to, from, binary_length());
  }

  // Convert the in-record representation to the in-memory representation
  static void record_to_memory(char *to, const char *from)
  {
    if (force_swap || rec_need_swap(from))
    {
      segment(0).rec2mem(to, from);
      segment(1).rec2mem(to, from);
      segment(2).rec2mem(to, from);
      segment(3).rec2mem(to, from);
      segment(4).rec2mem(to, from);
    }
    else
      memcpy(to, from, binary_length());
  }

  /*
    Calculate a hash of the in-record representation.
    Used in Field_uuid::hash(), e.g. for KEY partitioning. This
    makes partition distribution for UUID and BINARY(16) equal,
    so for example:

    CREATE OR REPLACE TABLE t1 (c1 UUID) PARTITION BY KEY(c1) PARTITIONS 5;
    INSERT INTO t1 (c1) VALUES (UUID());

    and

    CREATE OR REPLACE TABLE t1 (c1 BINARY(16)) PARTITION BY KEY(c1) PARTITIONS 5;
    INSERT INTO t1 (c1) VALUES (UUID());

    put values into the same partition.
  */
  static void hash_record(const uchar *ptr, Hasher *hasher)
  {
    segment(0).hash_record(ptr, hasher);
    segment(1).hash_record(ptr, hasher);
    segment(2).hash_record(ptr, hasher);
    segment(3).hash_record(ptr, hasher);
    segment(4).hash_record(ptr, hasher);
  }

  static int cmp_swap_noswap(const LEX_CSTRING &a, const LEX_CSTRING &b)
  {
    int res;
    if ((res= segment(4).cmp_swap_noswap(a.str, b.str)) ||
        (res= segment(3).cmp_swap_noswap(a.str, b.str)) ||
        (res= segment(2).cmp_swap_noswap(a.str, b.str)) ||
        (res= segment(1).cmp_swap_noswap(a.str, b.str)) ||
        (res= segment(0).cmp_swap_noswap(a.str, b.str)))
      return  res;
    return 0;
  }

  // Compare two in-memory values
  static int cmp(const LEX_CSTRING &a, const LEX_CSTRING &b)
  {
    DBUG_ASSERT(a.length == binary_length());
    DBUG_ASSERT(b.length == binary_length());
    bool swap_a= force_swap || mem_need_swap(a.str);
    bool swap_b= force_swap || mem_need_swap(b.str);
    if (swap_a && swap_b)
    {
      int res;
      if ((res= segment(4).cmp_memory(a.str, b.str)) ||
          (res= segment(3).cmp_memory(a.str, b.str)) ||
          (res= segment(2).cmp_memory(a.str, b.str)) ||
          (res= segment(1).cmp_memory(a.str, b.str)) ||
          (res= segment(0).cmp_memory(a.str, b.str)))
        return  res;
      return 0;
    }
    else if (swap_a && !swap_b)
      return cmp_swap_noswap(a, b);
    else if (!swap_a && swap_b)
      return -cmp_swap_noswap(b, a);
    return memcmp(a.str, b.str, binary_length());
  }

  static ulong KEY_pack_flags(uint column_nr)
  {
    return HA_PACK_KEY;
  }

  /*
    Convert in-record representation to binlog representation.
    We tranfer UUID values in binlog by compressing in-memory representation.
    This makes replication between UUID and BINARY(16) simpler:

    Transferring by compressing the in-record representation would require
    extending the binary log format to put the extact data type name into
    the column metadata.
  */
  static uchar *pack(uchar *to, const uchar *from, uint max_length)
  {
    uchar buf[binary_length()];
    record_to_memory((char *) buf, (const char *) from);
    return StringPack(&my_charset_bin, binary_length()).
             pack(to, buf, max_length);
  }

  // Convert binlog representation to in-record representation
  static const uchar *unpack(uchar *to,
                             const uchar *from, const uchar *from_end,
                             uint param_data)
  {
    uchar buf[binary_length()];
    const uchar *rc= StringPack(&my_charset_bin, binary_length()).
                       unpack(buf, from, from_end, param_data);
    memory_to_record((char *) to, (const char *) buf);
    return rc;
  }

};

class Type_collection_uuid: public Type_collection
{
  const Type_handler *find_in_array(const Type_handler *what,
                                    const Type_handler *stop,
                                    bool for_comparison) const;
public:
  const Type_handler *aggregate_for_result(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  { return find_in_array(a, b, false); }
  const Type_handler *aggregate_for_min_max(const Type_handler *a,
                                            const Type_handler *b)
                                            const override
  { return find_in_array(a, b, false); }
  const Type_handler *aggregate_for_comparison(const Type_handler *a,
                                               const Type_handler *b)
                                               const override
  { return find_in_array(a, b, true); }
  const Type_handler *aggregate_for_num_op(const Type_handler *a,
                                           const Type_handler *b)
                                           const override
  { return NULL; }

  const Type_handler *type_handler_for_implicit_upgrade(
                                        const Type_handler *from) const;

  static Type_collection_uuid *singleton()
  {
    static Type_collection_uuid tc;
    return &tc;
  }
};

#include "sql_type_fixedbin.h"
typedef Type_handler_fbt<UUID<1>, Type_collection_uuid> Type_handler_uuid_old;
typedef Type_handler_fbt<UUID<0>, Type_collection_uuid> Type_handler_uuid_new;

#endif // SQL_TYPE_UUID_INCLUDED
