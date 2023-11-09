/*
   Copyright (c) 2023, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef CSET_NARROWING_H_INCLUDED
#define CSET_NARROWING_H_INCLUDED

/*
  A singleton class to provide "utf8mb3_from_mb4.charset()".

  This is a variant of utf8mb3_general_ci that one can use when they have data
  in MB4 and want to make index lookup keys in MB3.
*/
extern
class Charset_utf8narrow
{
  struct my_charset_handler_st cset_handler;
  struct charset_info_st cset;
public:
  Charset_utf8narrow() :
    cset_handler(*my_charset_utf8mb3_general_ci.cset),
    cset(my_charset_utf8mb3_general_ci) /* Copy the CHARSET_INFO structure */
  {
    /* Insert our function wc_mb */
    cset_handler.wc_mb= my_wc_mb_utf8mb4_bmp_only;
    cset.cset=&cset_handler;

    /* Charsets are compared by their name, so assign a different name */
    LEX_CSTRING tmp= {STRING_WITH_LEN("utf8_mb4_to_mb3")};
    cset.cs_name= tmp;
  }

  CHARSET_INFO *charset() { return &cset; }

} utf8mb3_from_mb4;


/*
  A class to temporary change a field that uses utf8mb3_general_ci to enable
  correct lookup key construction from string value in utf8mb4_general_ci

  Intended usage:

    // can do this in advance:
    bool do_narrowing= Utf8_narrow::should_do_narrowing(field, value_cset);
    ...

    // This sets the field to do narrowing if necessary:
    Utf8_narrow narrow(field, do_narrowing);

    // write to 'field' here
    // item->save_in_field(field) or something else

    // Stop doing narrowing
    narrow.stop();
*/

class Utf8_narrow
{
  Field *field;
  DTCollation save_collation;

public:
  static bool should_do_narrowing(const THD *thd, CHARSET_INFO *field_cset,
                                  CHARSET_INFO *value_cset);

  static bool should_do_narrowing(const Field *field, CHARSET_INFO *value_cset)
  {
    CHARSET_INFO *field_cset= field->charset();
    THD *thd= field->table->in_use;
    return should_do_narrowing(thd, field_cset, value_cset);
  }

  Utf8_narrow(Field *field_arg, bool is_applicable)
  {
    field= NULL;
    if (is_applicable)
    {
      DTCollation mb3_from_mb4= utf8mb3_from_mb4.charset();
      field= field_arg;
      save_collation= field->dtcollation();
      field->change_charset(mb3_from_mb4);
    }
  }

  void stop()
  {
    if (field)
     field->change_charset(save_collation);
#ifndef NDEBUG
    field= NULL;
#endif
  }

  ~Utf8_narrow()
  {
    DBUG_ASSERT(!field);
  }
};


/*
  @brief
  Check if two fields can participate in a multiple equality using charset
  narrowing.

  @detail
    Normally, check_simple_equality() checks this by calling:

      left_field->eq_def(right_field)

    This function does the same but takes into account we might use charset
    narrowing:
     - collations are not the same but rather an utf8mb{3,4}_general_ci pair
     - for field lengths, should compare # characters, not #bytes.
*/

inline
bool fields_equal_using_narrowing(const THD *thd, const Field *left, const Field *right)
{
  return
    dynamic_cast<const Field_longstr*>(left) &&
    dynamic_cast<const Field_longstr*>(right) &&
    left->real_type() == right->real_type() &&
    (Utf8_narrow::should_do_narrowing(left, right->charset()) ||
     Utf8_narrow::should_do_narrowing(right, left->charset())) &&
    left->char_length() == right->char_length();
};


#endif /* CSET_NARROWING_H_INCLUDED */
