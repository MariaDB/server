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
    static LEX_CSTRING names[20]=
    {
      {STRING_WITH_LEN("END")},
      {STRING_WITH_LEN("TEXT")},
      {STRING_WITH_LEN("BINARY")},
      {STRING_WITH_LEN("SHORT_INT")},
      {STRING_WITH_LEN("LONG_INT")},
      {STRING_WITH_LEN("FLOAT")},
      {STRING_WITH_LEN("DOUBLE")},
      {STRING_WITH_LEN("NUM")},
      {STRING_WITH_LEN("USHORT_INT")},
      {STRING_WITH_LEN("ULONG_INT")},
      {STRING_WITH_LEN("LONGLONG")},
      {STRING_WITH_LEN("ULONGLONG")},
      {STRING_WITH_LEN("INT24")},
      {STRING_WITH_LEN("UINT24")},
      {STRING_WITH_LEN("INT8")},
      {STRING_WITH_LEN("VARTEXT1")},
      {STRING_WITH_LEN("VARBINARY1")},
      {STRING_WITH_LEN("VARTEXT2")},
      {STRING_WITH_LEN("VARBINARY2")},
      {STRING_WITH_LEN("BIT")}
    };
    if ((uint) type >= array_elements(names))
      return append(STRING_WITH_LEN("???"));
    return append(names[(uint) type]);
  }

  bool append_KEY_flag_names(ulong flags)
  {
    static LEX_CSTRING names[17]=
    {
      {STRING_WITH_LEN("HA_NOSAME")},             // 1
      {STRING_WITH_LEN("HA_PACK_KEY")},           // 2; also in HA_KEYSEG
      {STRING_WITH_LEN("HA_SPACE_PACK_USED")},    // 4
      {STRING_WITH_LEN("HA_VAR_LENGTH_KEY")},     // 8
      {STRING_WITH_LEN("HA_AUTO_KEY")},           // 16
      {STRING_WITH_LEN("HA_BINARY_PACK_KEY")},    // 32
      {STRING_WITH_LEN("HA_NULL_PART_KEY")},      // 64
      {STRING_WITH_LEN("HA_FULLTEXT")},           // 128
      {STRING_WITH_LEN("HA_UNIQUE_CHECK")},       // 256
      {STRING_WITH_LEN("HA_SORT_ALLOWS_SAME")},   // 512
      {STRING_WITH_LEN("HA_SPATIAL")},            // 1024
      {STRING_WITH_LEN("HA_NULL_ARE_EQUAL")},     // 2048
      {STRING_WITH_LEN("HA_USES_COMMENT")},       // 4096
      {STRING_WITH_LEN("HA_GENERATED_KEY")},      // 8192
      {STRING_WITH_LEN("HA_USES_PARSER")},        // 16384
      {STRING_WITH_LEN("HA_USES_BLOCK_SIZE")},    // 32768
      {STRING_WITH_LEN("HA_KEY_HAS_PART_KEY_SEG")}// 65536
    };
    return append_flag32_names((uint) flags, names, array_elements(names));
  }

  bool append_HA_KEYSEG_flag_names(uint32 flags)
  {
    static LEX_CSTRING names[]=
    {
      {STRING_WITH_LEN("HA_SPACE_PACK")},      // 1
      {STRING_WITH_LEN("HA_PACK_KEY")},        // 2; also in KEY/MI/KEY_DEF
      {STRING_WITH_LEN("HA_PART_KEY_SEG")},    // 4
      {STRING_WITH_LEN("HA_VAR_LENGTH_PART")}, // 8
      {STRING_WITH_LEN("HA_NULL_PART")},       // 16
      {STRING_WITH_LEN("HA_BLOB_PART")},       // 32
      {STRING_WITH_LEN("HA_SWAP_KEY")},        // 64
      {STRING_WITH_LEN("HA_REVERSE_SORT")},    // 128
      {STRING_WITH_LEN("HA_NO_SORT")},         // 256
      {STRING_WITH_LEN("??? 512 ???")},        // 512
      {STRING_WITH_LEN("HA_BIT_PART")},        // 1024
      {STRING_WITH_LEN("HA_CAN_MEMCMP")}       // 2048
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
      if (!tmp.append(where, strlen(where)) && !tmp.append_KEY(keys[i]))
        tmp.print(thd);
    }
  }
};


#endif // SQL_DEBUG_INCLUDED
