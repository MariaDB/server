#ifndef SQL_DEBUG_INCLUDED
#define SQL_DEBUG_INCLUDED
/*
   Copyright (c) 2022, MariaDB

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/


class Debug_key: public String
{
public:
  Debug_key() { };
  void print(THD *thd) const
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_UNKNOWN_ERROR, "DBUG: %.*s", length(), ptr());
  }

  bool append_key_type(ha_base_keytype type)
  {
    static const char *names[20]=
    {
      "END",
      "TEXT",
      "BINARY",
      "SHORT_INT",
      "LONG_INT",
      "FLOAT",
      "DOUBLE",
      "NUM",
      "USHORT_INT",
      "ULONG_INT",
      "LONGLONG",
      "ULONGLONG",
      "INT24",
      "UINT24",
      "INT8",
      "VARTEXT1",
      "VARBINARY1",
      "VARTEXT2",
      "VARBINARY2",
      "BIT"
    };
    if ((uint) type >= array_elements(names))
      return append("???");
    return append(names[(uint) type]);
  }

  bool append_KEY_flag_names(ulong flags)
  {
    static const char *names[17]=
    {
      "HA_NOSAME",
      "HA_PACK_KEY",
      "HA_SPACE_PACK_USED",
      "HA_VAR_LENGTH_KEY",
      "HA_AUTO_KEY",
      "HA_BINARY_PACK_KEY",
      "HA_NULL_PART_KEY",
      "HA_FULLTEXT",
      "HA_UNIQUE_CHECK",
      "HA_SORT_ALLOWS_SAME",
      "HA_SPATIAL",
      "HA_NULL_ARE_EQUAL",
      "HA_GENERATED_KEY",
      "HA_USES_COMMENT",
      "HA_USES_PARSER",
      "HA_USES_BLOCK_SIZE",
      "HA_KEY_HAS_PART_KEY_SEG"
    };
    return append_flag32_names((uint) flags, names, array_elements(names));
  }

  bool append_HA_KEYSEG_flag_names(uint32 flags)
  {
    static const char *names[]=
    {
      "HA_SPACE_PACK",      // 1
      "??? 2 ???",          // 2
      "HA_PART_KEY_SEG",    // 4
      "HA_VAR_LENGTH_PART", // 8
      "HA_NULL_PART",       // 16
      "HA_BLOB_PART",       // 32
      "HA_SWAP_KEY",        // 64
      "HA_REVERSE_SORT",    // 128
      "HA_NO_SORT",         // 256
      "??? 512 ???",        // 512
      "HA_BIT_PART",        // 1024
      "HA_CAN_MEMCMP"       // 2048
    };
    return append_flag32_names(flags, names, array_elements(names));
  }

  bool append_HA_KEYSEG_type(ha_base_keytype type)
  {
    return append_ulonglong(type) ||
           append(' ') ||
           append_key_type(type);
  }

  bool append_HA_KEYSEG_flags(uint32 flags)
  {
    return append_hex_uint32(flags) ||
           append(' ') ||
           append_HA_KEYSEG_flag_names(flags);
  }

  bool append_key(const LEX_CSTRING &name, uint32 flags)
  {
    return
      append_name_value(Lex_cstring(STRING_WITH_LEN("name")), name, '`') ||
      append(Lex_cstring(STRING_WITH_LEN(" flags="))) ||
      append_hex_uint32(flags) ||
      append(' ') ||
      append_KEY_flag_names(flags);
  }

  bool append_KEY(const KEY &key)
  {
    return append_key(key.name, key.flags);
  }

  static void print_keysegs(THD *thd, const HA_KEYSEG *seg, uint count)
  {
    for (uint i= 0; i < count; i++)
    {
      Debug_key tmp;
      if (!tmp.append(Lex_cstring(STRING_WITH_LEN("  seg["))) &&
          !tmp.append_ulonglong(i) &&
          !tmp.append(Lex_cstring(STRING_WITH_LEN("].type="))) &&
          !tmp.append_HA_KEYSEG_type((ha_base_keytype) seg[i].type))
        tmp.print(thd);
      tmp.length(0);
      if (!tmp.append(Lex_cstring(STRING_WITH_LEN("  seg["))) &&
          !tmp.append_ulonglong(i) &&
          !tmp.append(Lex_cstring(STRING_WITH_LEN("].flag="))) &&
          !tmp.append_HA_KEYSEG_flags(seg[i].flag))
       tmp.print(thd);
    }
  }

  static void print_keys(THD *thd, const char *where,
                         const KEY *keys, uint key_count)
  {
    for (uint i= 0; i < key_count; i++)
    {
      Debug_key tmp;
      if (!tmp.append(where) && !tmp.append_KEY(keys[i]))
        tmp.print(thd);
    }
  }
};


#endif // SQL_DEBUG_INCLUDED
