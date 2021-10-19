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
class UUID: public FixedBinTypeStorage<MY_UUID_SIZE, MY_UUID_STRING_LENGTH>
{
public:
  using FixedBinTypeStorage::FixedBinTypeStorage;
  bool ascii_to_fbt(const char *str, size_t str_length);
  size_t to_string(char *dst, size_t dstsize) const;
  static const Name &default_value();

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
    void memory_to_record(char *to, const char *from) const
    {
      memcpy(to + m_record_pos, from + m_memory_pos, m_length);
    }
    void record_to_memory(char *to, const char * from) const
    {
      memcpy(to + m_memory_pos, from + m_record_pos, m_length);
    }
    int cmp_memory(const char *a, const char *b) const
    {
      return memcmp(a + m_memory_pos, b + m_memory_pos, m_length);
    }
    void hash_record(const uchar *ptr, ulong *nr, ulong *nr2) const
    {
      my_charset_bin.hash_sort(ptr + m_record_pos, m_length, nr, nr2);
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

  // Convert the in-memory representation to the in-record representation
  static void memory_to_record(char *to, const char *from)
  {
    segment(0).memory_to_record(to, from);
    segment(1).memory_to_record(to, from);
    segment(2).memory_to_record(to, from);
    segment(3).memory_to_record(to, from);
    segment(4).memory_to_record(to, from);
  }

  // Convert the in-record representation to the in-memory representation
  static void record_to_memory(char *to, const char *from)
  {
    segment(0).record_to_memory(to, from);
    segment(1).record_to_memory(to, from);
    segment(2).record_to_memory(to, from);
    segment(3).record_to_memory(to, from);
    segment(4).record_to_memory(to, from);
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
  static void hash_record(const uchar *ptr, ulong *nr, ulong *nr2)
  {
    segment(0).hash_record(ptr, nr, nr2);
    segment(1).hash_record(ptr, nr, nr2);
    segment(2).hash_record(ptr, nr, nr2);
    segment(3).hash_record(ptr, nr, nr2);
    segment(4).hash_record(ptr, nr, nr2);
  }

  // Compare two in-memory values
  static int cmp(const LEX_CSTRING &a, const LEX_CSTRING &b)
  {
    DBUG_ASSERT(a.length == binary_length());
    DBUG_ASSERT(b.length == binary_length());
    int res;
    if ((res= segment(4).cmp_memory(a.str, b.str)) ||
        (res= segment(3).cmp_memory(a.str, b.str)) ||
        (res= segment(2).cmp_memory(a.str, b.str)) ||
        (res= segment(1).cmp_memory(a.str, b.str)) ||
        (res= segment(0).cmp_memory(a.str, b.str)))
      return  res;
    return 0;
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


#include "sql_type_fixedbin.h"
typedef FixedBinTypeBundle<UUID> UUIDBundle;

#endif // SQL_TYPE_UUID_INCLUDED
