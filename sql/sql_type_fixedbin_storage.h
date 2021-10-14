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
*/

/***********************************************************************/

template<size_t NATIVE_LEN, size_t MAX_CHAR_LEN>
class FixedBinTypeStorage
{
protected:
  char m_buffer[NATIVE_LEN];

  // Non-initializing constructor
  FixedBinTypeStorage()
  { }

  FixedBinTypeStorage & set_zero()
  {
    bzero(&m_buffer, sizeof(m_buffer));
    return *this;
  }
public:

  // Initialize from binary representation
  FixedBinTypeStorage(const char *str, size_t length)
  {
    if (length != binary_length())
      set_zero();
    else
      memcpy(&m_buffer, str, sizeof(m_buffer));
  }

  static constexpr uint binary_length() { return NATIVE_LEN; }
  static constexpr uint max_char_length() { return MAX_CHAR_LEN; }

  static bool only_zero_bytes(const char *ptr, size_t length)
  {
    for (uint i= 0 ; i < length; i++)
    {
      if (ptr[i] != 0)
        return false;
    }
    return true;
  }

};
#endif /* SQL_TYPE_FIXEDBIN_STORAGE */
