#ifndef SQL_TYPE_FIXEDBIN_STORAGE
#define SQL_TYPE_FIXEDBIN_STORAGE
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

/*
  This is a common code for plugin (?) types that are generally
  handled like strings, but have their own fixed size on-disk binary storage
  format and their own (variable size) canonical string representation.

  Examples are INET6 and UUID types.

  The MariaDB server uses three binary representations of a data type:

  1. In-memory binary representation (user visible)
    This representation:
    - can be used in INSERT..VALUES (X'AABBCC')
    - can be used in WHERE conditions: WHERE c1=X'AABBCC'
    - is returned by CAST(x AS BINARY(N))
    - is returned by Field::val_native() and Item::val_native()

  2. In-record binary representation (user invisible)
    This representation:
    - is used in records (is pointed by Field::ptr)
    - must be comparable by memcmp()

  3. Binlog binary (row) representation
    Usually, for string data types the binlog representation
    is based on the in-record representation with trailing byte compression:
    - trailing space compression for text string data types
    - trailing zero compression for binary string data types

  We have to have separate in-memory and in-record representations
  because we use HA_KEYTYPE_BINARY for indexing. The engine API
  does not have a way to pass a comparison function as a parameter.

  The default implementation below assumes that:
  - the in-memory and in-record representations are equal
  - the binlog representation is compatible with BINARY(N)
  This is OK for simple data types, like INET6.

  Data type implementations that need different representations
  can override the default implementation (like e.g. UUID does).
*/

/***********************************************************************/

template<size_t NATIVE_LEN, size_t MAX_CHAR_LEN>
class FixedBinTypeStorage
{
protected:
  // The buffer that stores the in-memory binary representation
  char m_buffer[NATIVE_LEN];

  FixedBinTypeStorage() = default;

  FixedBinTypeStorage & set_zero()
  {
    bzero(&m_buffer, sizeof(m_buffer));
    return *this;
  }
public:

  // Initialize from the in-memory binary representation
  FixedBinTypeStorage(const char *str, size_t length)
  {
    if (length != binary_length())
      set_zero();
    else
      memcpy(&m_buffer, str, sizeof(m_buffer));
  }

  // Return the buffer with the in-memory representation
  Lex_cstring to_lex_cstring() const
  {
    return Lex_cstring(m_buffer, sizeof(m_buffer));
  }

  static constexpr uint binary_length() { return NATIVE_LEN; }
  static constexpr uint max_char_length() { return MAX_CHAR_LEN; }

  // Compare the in-memory binary representations of two values
  static int cmp(const LEX_CSTRING &a, const LEX_CSTRING &b)
  {
    DBUG_ASSERT(a.length == binary_length());
    DBUG_ASSERT(b.length == binary_length());
    return memcmp(a.str, b.str, b.length);
  }

  /*
    Convert from the in-memory to the in-record representation.
    Used in Field::store_native().
  */
  static void memory_to_record(char *to, const char *from)
  {
    memcpy(to, from, NATIVE_LEN);
  }
  /*
    Convert from the in-record to the in-memory representation
    Used in Field::val_native().
  */
  static void record_to_memory(char *to, const char *from)
  {
    memcpy(to, from, NATIVE_LEN);
  }

  /*
    Hash the in-record representation
    Used in Field::hash().
  */
  static void hash_record(uchar *ptr, Hasher *hasher)
  {
    hasher->add(&my_charset_bin, ptr, binary_length());
  }

  static bool only_zero_bytes(const char *ptr, size_t length)
  {
    for (uint i= 0 ; i < length; i++)
    {
      if (ptr[i] != 0)
        return false;
    }
    return true;
  }

  static ulong KEY_pack_flags(uint column_nr)
  {
    /*
      Return zero by default. A particular data type can override
      this method return some flags, e.g. HA_PACK_KEY to enable
      key prefix compression.
    */
    return 0;
  }

  /*
    Convert from the in-record to the binlog representation.
    Used in Field::pack(), and in filesort to store the addon fields.
    By default, do what BINARY(N) does.
  */
  static uchar *pack(uchar *to, const uchar *from, uint max_length)
  {
    return StringPack(&my_charset_bin, binary_length()).pack(to, from, max_length);
  }

  /*
    Convert from the in-binary-log to the in-record representation.
    Used in Field::unpack().
    By default, do what BINARY(N) does.
  */
  static const uchar *unpack(uchar *to, const uchar *from, const uchar *from_end,
                             uint param_data)
  {
    return StringPack(&my_charset_bin, binary_length()).unpack(to, from, from_end,
                                                               param_data);
  }

};
#endif /* SQL_TYPE_FIXEDBIN_STORAGE */
