/* Copyright (c) 2024 Bytedance Ltd. and/or its affiliates

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "videx_log_utils.h"

VidexLogUtils videx_log_ins;
void VidexLogUtils::markPassbyUnexpected(const std::string &func,
                                           const std::string &file,
                                           const int line) {
  markHaFuncPassby(func, file, line, "NOOOO!", true);
}

void VidexLogUtils::NotMarkPassby(const std::string &, const std::string &,
                                    const int) {
  // For things that are explicitly known to be unrelated to the query but will be used during explain,
  // use this function. Actually, nothing is printed.
  return;
}

void VidexLogUtils::markHaFuncPassby(const std::string &func,
                                       const std::string &file, const int line,
                                       const std::string &others, bool silent) {
  count++;
  if (silent) {
    return;
  }
  std::stringstream ss;
  ss << "VIDEX_PASSBY[" << count << "]<" << this->tag << "> ";
  if (!others.empty()) {
    ss << "___MSG:{" << others << "} ";
  }

  ss << " ____ " << func << " ____ File: " << file << ":" << line;
  if (enable_cout) {
    std::cout << ss.str() << std::endl;
  }
  if (enable_trace) {
    // TODO not support for now, need to set thd and initialize trace_object
  }
}


/**
  Print a key to a string
  referring to print_key_value - sql/range_optimizer/range_optimizer.cc:1429

  @param[out] out          String the key is appended to
  @param[in]  key_part     Index components description
  @param[in]  key          Key tuple
*/
void videx_print_key_value(String *out, const KEY_PART_INFO *key_part,
                           const uchar *key) {
    Field *field = key_part->field;
    if (field->is_array()) {
        field = down_cast<Field_typed_array *>(field)->get_conv_field();
    }

    if (field->is_flag_set(BLOB_FLAG)) {
        // Byte 0 of a nullable key is the null-byte. If set, key is NULL.
        if (field->is_nullable() && *key) {
          out->append(STRING_WITH_LEN("NULL"));
          return;
        }
        else
            if (field->type() == MYSQL_TYPE_GEOMETRY) {
              out->append(STRING_WITH_LEN("unprintable_geometry_value"));
              return;
            } else {
              // if uncomment, videx will return fixed "unprintable_blob_value"
              // out->append(STRING_WITH_LEN("unprintable_blob_value"));
              // return;
            }
    }

    uint store_length = key_part->store_length;

    if (field->is_nullable()) {
        /*
          Byte 0 of key is the null-byte. If set, key is NULL.
          Otherwise, print the key value starting immediately after the
          null-byte
        */
        if (*key) {
            out->append(STRING_WITH_LEN("NULL"));
            return;
        }
        key++;  // Skip null byte
        store_length--;
    }

    /*
      Binary data cannot be converted to UTF8 which is what the
      optimizer trace expects. If the column is binary, the hex
      representation is printed to the trace instead.
    */
    if (field->result_type() == STRING_RESULT &&
        field->charset() == &my_charset_bin) {
        out->append("0x");
        for (uint i = 0; i < store_length; i++) {
            out->append(_dig_vec_lower[*(key + i) >> 4]);
            out->append(_dig_vec_lower[*(key + i) & 0x0F]);
        }
        return;
    }

    StringBuffer<128> tmp(system_charset_info);
    bool add_quotes = field->result_type() == STRING_RESULT;

    TABLE *table = field->table;
    my_bitmap_map *old_sets[2];

    dbug_tmp_use_all_columns(table, old_sets, table->read_set, table->write_set);

    field->set_key_image(key, key_part->length);
    if (field->type() == MYSQL_TYPE_BIT) {
        (void)field->val_int_as_str(&tmp, true);  // may change tmp's charset
        add_quotes = false;
    } else {
        field->val_str(&tmp);  // may change tmp's charset
    }

    dbug_tmp_restore_column_maps(table->read_set, table->write_set, old_sets);

    if (add_quotes) {
        out->append('\'');
        // Worst case: Every character is escaped.
        const size_t buffer_size = tmp.length() * 2 + 1;
        char *quoted_string = current_thd->mem_root->ArrayAlloc<char>(buffer_size);
        // char *quoted_string = new char[buffer_size];
        const size_t quoted_length = escape_string_for_mysql(
                tmp.charset(), quoted_string, buffer_size, tmp.ptr(), tmp.length());
        if (quoted_length == static_cast<size_t>(-1)) {
            // Overflow. Our worst case estimate for the buffer size was too low.
            assert(false);
            return;
        }
        out->append(quoted_string, quoted_length, tmp.charset());
        out->append('\'');
        // delete[] quoted_string;
    } else {
        out->append(tmp.ptr(), tmp.length(), tmp.charset());
    }
}

/**
 * replace `HA_READ_KEY_EXACT`, `HA_READ_KEY_OR_NEXT` to =，>=
 */
std::string haRKeyFunctionToSymbol(ha_rkey_function function) {
    switch (function) {
        case HA_READ_KEY_EXACT:
            return "=";
        case HA_READ_KEY_OR_NEXT:
            return ">=";
        case HA_READ_KEY_OR_PREV:
            return "<=";
        case HA_READ_AFTER_KEY:
            return ">";
        case HA_READ_BEFORE_KEY:
            return "<";
        case HA_READ_PREFIX:
            return "=x%";
        case HA_READ_PREFIX_LAST:
            return "last_x%";
        case HA_READ_PREFIX_LAST_OR_PREV:
            return "<=last_x%";
        case HA_READ_MBR_CONTAIN:
            return "HA_READ_MBR_CONTAIN";
        case HA_READ_MBR_INTERSECT:
            return "HA_READ_MBR_INTERSECT";
        case HA_READ_MBR_WITHIN:
            return "HA_READ_MBR_WITHIN";
        case HA_READ_MBR_DISJOINT:
            return "HA_READ_MBR_DISJOINT";
        case HA_READ_MBR_EQUAL:
            return "HA_READ_MBR_EQUAL";
        case HA_READ_INVALID:
            return "HA_READ_INVALID";
        default:
            return "Unknown ha_rkey_function";
    }
}

inline void subha_append_range(String *out, const KEY_PART_INFO *key_part,
                               const uchar *min_key,
                               const uint, VidexJsonItem* range_json) {
    //   const char *sep = "__#@#__";
    const char *sep = "  ";
    if (out->length() > 0) out->append(sep);
    String tmp_str;
    tmp_str.set_charset(system_charset_info);
    tmp_str.length(0);
    std::stringstream ss;

    // TODO not support GEOM_FLAG temporarily
    //   if (flag & GEOM_FLAG) {
    //     /*
    //       The flags of GEOM ranges do not work the same way as for other
    //       range types, so printing "col < some_geom" doesn't make sense.
    //       Just print the column name, not operator.
    //     */
    //     out->append(key_part->field->field_name);
    //     out->append(STRING_WITH_LEN(" "));
    //     subha_print_key_value(out, key_part, min_key);
    //     return;
    //   }

    // Range scans over multi-valued indexes use a sequence of MEMBER OF
    // predicates ORed together.
    if (key_part->field->is_array()) {
        videx_print_key_value(&tmp_str, key_part, min_key);
        out->append(tmp_str);
        ss.write(tmp_str.ptr(), tmp_str.length());
        range_json->add_property("value", ss.str());
        tmp_str.length(0);

        out->append(STRING_WITH_LEN(" MEMBER OF ("));
        const std::string expression = ItemToString(
                down_cast<Item_func *>(key_part->field->gcol_info->expr_item)
                        ->get_arg(0));  // Strip off CAST(... AS <type> ARRAY).
        out->append(expression.data(), expression.size());
        out->append(')');

        range_json->add_property("column", expression);
        range_json->add_property("special_operator", "MEMBER OF");
        return;
    }

    //   if (!Overlaps(flag, NO_MIN_RANGE | NO_MAX_RANGE | NEAR_MIN | NEAR_MAX) &&
    //       range_is_equality(min_key, max_key, key_part->store_length,
    //                         key_part->field->is_nullable())) {
    //     out->append(get_field_name_or_expression(current_thd,
    //     key_part->field)); out->append(STRING_WITH_LEN(" = "));
    //     subha_print_key_value(out, key_part, min_key);
    //     return;
    //   }

    const char * field_or_expr = get_field_name_or_expression(current_thd, key_part->field);
    out->append(field_or_expr);
    range_json->add_property("column", field_or_expr);

    out->append("(");
    videx_print_key_value(&tmp_str, key_part, min_key);
    out->append(tmp_str);
    out->append("), ");
    ss.write(tmp_str.ptr(), tmp_str.length());
    range_json->add_property("value", ss.str());
    tmp_str.length(0);

    // TODO simplify print max
    //   if (!(flag & NO_MIN_RANGE)) {
    //     print_key_value(out, key_part, min_key);
    //     if (flag & NEAR_MIN)
    //       out->append(STRING_WITH_LEN(" < "));
    //     else
    //       out->append(STRING_WITH_LEN(" <= "));
    //   }

    //   out->append(get_field_name_or_expression(current_thd, key_part->field));

    //   if (!(flag & NO_MAX_RANGE)) {
    //     if (flag & NEAR_MAX)
    //       out->append(STRING_WITH_LEN(" < "));
    //     else
    //       out->append(STRING_WITH_LEN(" <= "));
    //     print_key_value(out, key_part, max_key);
    //   }
}

/**
 * referring to append_range_to_string, especially
 * sql/range_optimizer/range_optimizer.cc:1732
 *
 */
void subha_parse_key_range(key_range *key_range, KEY *index,
                           String *out, VidexJsonItem *req_json) {
    //   const char *NO_KEY_RANGE = "NO_KEY_RANGE";
    const uint QUICK_RANGE_flag = -1;
    if (key_range == nullptr) {
        out->append("<NO_KEY_RANGE>");
        return;
    }
    KEY_PART_INFO *first_key_part = index->key_part;
    out->append(" ");
    std::string key_range_flag_str = haRKeyFunctionToSymbol(key_range->flag);
    out->append(key_range_flag_str);

    req_json->add_property("operator", key_range_flag_str);
    req_json->add_property_nonan("length", key_range->length);
    req_json->add_property("index_name", index->name);

    const uchar *uchar_key = key_range->key;
    //   KEY_PART_INFO *key_part;
    for (int keypart_idx : BitsSetIn(key_range->keypart_map)) {
        if (!IsBitSet(keypart_idx, key_range->keypart_map)) {
            out->append("<NO_KEY_RANGE for keypart_idx: ");
            out->append(keypart_idx);
            continue;
        }
        // key_part = &first_key_part[keypart_idx];
        VidexJsonItem *range_json = req_json->create("column_and_bound");
        subha_append_range(out, &first_key_part[keypart_idx], uchar_key,
                           QUICK_RANGE_flag, range_json);

        uchar_key += first_key_part[keypart_idx].store_length;
    }
    //
    // uchar *min_key, *max_key;
    // uint16 min_length, max_length;

    // /// Stores bitwise-or'ed bits defined in enum key_range_flags.
    // uint16 flag;

    // /**
    //     Stores one of the HA_READ_MBR_XXX items in enum ha_rkey_function, only
    //     effective when flag has a GEOM_FLAG bit.
    // */
    // enum ha_rkey_function rkey_func_flag;
    // key_part_map min_keypart_map,  // bitmap of used keyparts in min_key
    //     max_keypart_map;
    //  if (key != nullptr && first_key_part != nullptr) out->append(" <some try>
    //  ");
}

void VidexLogUtils::markRecordInRange([[maybe_unused]]const std::string &func, [[maybe_unused]]const std::string &file,
                                       [[maybe_unused]]const int line, key_range *min_key, key_range *max_key,
                                       KEY *key, VidexJsonItem *req_json) {
    String range_info;
    range_info.set_charset(system_charset_info);

    VidexJsonItem* min_json = req_json->create("min_key");
    subha_parse_key_range(min_key, key, &range_info, min_json);
    std::string std_info_min(range_info.ptr(), range_info.length());
    range_info.length(0);

    VidexJsonItem* max_json = req_json->create("max_key");
    subha_parse_key_range(max_key, key, &range_info, max_json);
    std::string std_info_max(range_info.ptr(), range_info.length());
    range_info.length(0);

    std::stringstream ss;
    ss << "KEY: " << key->name << "   MIN_KEY: {" << std_info_min << "}, MAX_KEY: {"<<std_info_max << "}";
    // ss.write(std_info_min.c_str(), std_info_min.length());
    // ss.write(std_info_max.c_str(), std_info_max.length());

    std::cout << std::endl << ss.str() << std::endl;

    std::cout << "req_json = " << req_json->to_json() << std::endl;
}
