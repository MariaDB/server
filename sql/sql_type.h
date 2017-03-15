#ifndef SQL_TYPE_H_INCLUDED
#define SQL_TYPE_H_INCLUDED
/*
   Copyright (c) 2015  MariaDB Foundation.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif


#include "mysqld.h"
#include "sql_array.h"

class Field;
class Item;
class Item_cache;
class Item_sum_hybrid;
class Item_func_hex;
class Item_hybrid_func;
class Item_func_min_max;
class Item_func_hybrid_field_type;
class Item_bool_func2;
class Item_func_between;
class Item_func_in;
class Item_func_round;
class cmp_item;
class in_vector;
class Type_std_attributes;
class Sort_param;
class Arg_comparator;
struct TABLE;
struct SORT_FIELD_ATTR;


/*
  Flags for collation aggregation modes, used in TDCollation::agg():

  MY_COLL_ALLOW_SUPERSET_CONV  - allow conversion to a superset
  MY_COLL_ALLOW_COERCIBLE_CONV - allow conversion of a coercible value
                                 (i.e. constant).
  MY_COLL_ALLOW_CONV           - allow any kind of conversion
                                 (combination of the above two)
  MY_COLL_ALLOW_NUMERIC_CONV   - if all items were numbers, convert to
                                 @@character_set_connection
  MY_COLL_DISALLOW_NONE        - don't allow return DERIVATION_NONE
                                 (e.g. when aggregating for comparison)
  MY_COLL_CMP_CONV             - combination of MY_COLL_ALLOW_CONV
                                 and MY_COLL_DISALLOW_NONE
*/

#define MY_COLL_ALLOW_SUPERSET_CONV   1
#define MY_COLL_ALLOW_COERCIBLE_CONV  2
#define MY_COLL_DISALLOW_NONE         4
#define MY_COLL_ALLOW_NUMERIC_CONV    8

#define MY_COLL_ALLOW_CONV (MY_COLL_ALLOW_SUPERSET_CONV | MY_COLL_ALLOW_COERCIBLE_CONV)
#define MY_COLL_CMP_CONV   (MY_COLL_ALLOW_CONV | MY_COLL_DISALLOW_NONE)


#define my_charset_numeric      my_charset_latin1
#define MY_REPERTOIRE_NUMERIC   MY_REPERTOIRE_ASCII


enum Derivation
{
  DERIVATION_IGNORABLE= 6,
  DERIVATION_NUMERIC= 5,
  DERIVATION_COERCIBLE= 4,
  DERIVATION_SYSCONST= 3,
  DERIVATION_IMPLICIT= 2,
  DERIVATION_NONE= 1,
  DERIVATION_EXPLICIT= 0
};


/**
   "Declared Type Collation"
   A combination of collation and its derivation.
*/

class DTCollation {
public:
  CHARSET_INFO     *collation;
  enum Derivation derivation;
  uint repertoire;

  void set_repertoire_from_charset(CHARSET_INFO *cs)
  {
    repertoire= cs->state & MY_CS_PUREASCII ?
                MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
  }
  DTCollation()
  {
    collation= &my_charset_bin;
    derivation= DERIVATION_NONE;
    repertoire= MY_REPERTOIRE_UNICODE30;
  }
  DTCollation(CHARSET_INFO *collation_arg, Derivation derivation_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  DTCollation(CHARSET_INFO *collation_arg,
              Derivation derivation_arg,
              uint repertoire_arg)
   :collation(collation_arg),
    derivation(derivation_arg),
    repertoire(repertoire_arg)
  { }
  void set(const DTCollation &dt)
  {
    collation= dt.collation;
    derivation= dt.derivation;
    repertoire= dt.repertoire;
  }
  void set(CHARSET_INFO *collation_arg, Derivation derivation_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  void set(CHARSET_INFO *collation_arg,
           Derivation derivation_arg,
           uint repertoire_arg)
  {
    collation= collation_arg;
    derivation= derivation_arg;
    repertoire= repertoire_arg;
  }
  void set_numeric()
  {
    collation= &my_charset_numeric;
    derivation= DERIVATION_NUMERIC;
    repertoire= MY_REPERTOIRE_NUMERIC;
  }
  void set(CHARSET_INFO *collation_arg)
  {
    collation= collation_arg;
    set_repertoire_from_charset(collation_arg);
  }
  void set(Derivation derivation_arg)
  { derivation= derivation_arg; }
  bool aggregate(const DTCollation &dt, uint flags= 0);
  bool set(DTCollation &dt1, DTCollation &dt2, uint flags= 0)
  { set(dt1); return aggregate(dt2, flags); }
  const char *derivation_name() const
  {
    switch(derivation)
    {
      case DERIVATION_NUMERIC:   return "NUMERIC";
      case DERIVATION_IGNORABLE: return "IGNORABLE";
      case DERIVATION_COERCIBLE: return "COERCIBLE";
      case DERIVATION_IMPLICIT:  return "IMPLICIT";
      case DERIVATION_SYSCONST:  return "SYSCONST";
      case DERIVATION_EXPLICIT:  return "EXPLICIT";
      case DERIVATION_NONE:      return "NONE";
      default: return "UNKNOWN";
    }
  }
  int sortcmp(const String *s, const String *t) const
  {
    return collation->coll->strnncollsp(collation,
                                        (uchar *) s->ptr(), s->length(),
                                        (uchar *) t->ptr(), t->length());
  }
};


static inline uint32
char_to_byte_length_safe(size_t char_length_arg, uint32 mbmaxlen_arg)
{
  ulonglong tmp= ((ulonglong) char_length_arg) * mbmaxlen_arg;
  return tmp > UINT_MAX32 ? UINT_MAX32 : static_cast<uint32>(tmp);
}

/**
  A class to store type attributes for the standard data types.
  Does not include attributes for the extended data types
  such as ENUM, SET, GEOMETRY.
*/
class Type_std_attributes
{
public:
  DTCollation collation;
  uint decimals;
  /*
    The maximum value length in characters multiplied by collation->mbmaxlen.
    Almost always it's the maximum value length in bytes.
  */
  uint32 max_length;
  bool unsigned_flag;
  Type_std_attributes()
   :collation(&my_charset_bin, DERIVATION_COERCIBLE),
    decimals(0), max_length(0), unsigned_flag(false)
  { }
  Type_std_attributes(const Type_std_attributes *other)
   :collation(other->collation),
    decimals(other->decimals),
    max_length(other->max_length),
    unsigned_flag(other->unsigned_flag)
  { }
  Type_std_attributes(uint32 max_length_arg, uint decimals_arg,
                      bool unsigned_flag_arg, const DTCollation &dtc)
    :collation(dtc),
     decimals(decimals_arg),
     max_length(max_length_arg),
     unsigned_flag(unsigned_flag_arg)
  { }
  void set(const Type_std_attributes *other)
  {
    *this= *other;
  }
  void set(const Type_std_attributes &other)
  {
    *this= other;
  }
  void set(const Field *field);
  uint32 max_char_length() const
  { return max_length / collation.collation->mbmaxlen; }
  void fix_length_and_charset(uint32 max_char_length_arg, CHARSET_INFO *cs)
  {
    max_length= char_to_byte_length_safe(max_char_length_arg, cs->mbmaxlen);
    collation.collation= cs;
  }
  void fix_char_length(uint32 max_char_length_arg)
  {
    max_length= char_to_byte_length_safe(max_char_length_arg,
                                         collation.collation->mbmaxlen);
  }
};


class Name: private LEX_CSTRING
{
public:
  Name(const char *str_arg, uint length_arg)
  {
    LEX_CSTRING::str= str_arg;
    LEX_CSTRING::length= length_arg;
  }
  const char *ptr() const { return LEX_CSTRING::str; }
  uint length() const { return LEX_CSTRING::length; }
};


class Type_handler
{
protected:
  String *print_item_value_csstr(THD *thd, Item *item, String *str) const;
  String *print_item_value_temporal(THD *thd, Item *item, String *str,
                                     const Name &type_name, String *buf) const;
  void make_sort_key_longlong(uchar *to,
                              bool maybe_null, bool null_value,
                              bool unsigned_flag,
                              longlong value) const;
public:
  static const Type_handler *string_type_handler(uint max_octet_length);
  static const Type_handler *get_handler_by_field_type(enum_field_types type);
  static const Type_handler *get_handler_by_real_type(enum_field_types type);
  static const Type_handler *get_handler_by_cmp_type(Item_result type);
  static const Type_handler *get_handler_by_result_type(Item_result type)
  {
    /*
      As result_type() returns STRING_RESULT for temporal Items,
      type should never be equal to TIME_RESULT here.
    */
    DBUG_ASSERT(type != TIME_RESULT);
    return get_handler_by_cmp_type(type);
  }
  static const
  Type_handler *aggregate_for_result_traditional(const Type_handler *h1,
                                                 const Type_handler *h2);

  virtual const Name name() const= 0;
  virtual enum_field_types field_type() const= 0;
  virtual enum_field_types real_field_type() const { return field_type(); }
  virtual Item_result result_type() const= 0;
  virtual Item_result cmp_type() const= 0;
  virtual const Type_handler *type_handler_for_comparison() const= 0;
  virtual CHARSET_INFO *charset_for_protocol(const Item *item) const;
  virtual const Type_handler*
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const
  { return this; }
  virtual ~Type_handler() {}
  /**
    Determines MariaDB traditional data types that always present
    in the server.
  */
  virtual bool is_traditional_type() const
  {
    return true;
  }
  /**
    Makes a temporary table Field to handle numeric aggregate functions,
    e.g. SUM(DISTINCT expr), AVG(DISTINCT expr), etc.
  */
  virtual Field *make_num_distinct_aggregator_field(MEM_ROOT *,
                                                    const Item *) const;
  /**
    Makes a temporary table Field to handle RBR replication type conversion.
    @param TABLE    - The conversion table the field is going to be added to.
                      It's used to access to table->in_use->mem_root,
                      to create the new field on the table memory root,
                      as well as to increment statistics in table->share
                      (e.g. table->s->blob_count).
    @param metadata - Metadata from the binary log.
    @param target   - The field in the target table on the slave.

    Note, the data types of "target" and of "this" are not necessarily
    always the same, in general case it's possible that:
            this->field_type() != target->field_type()
    and/or
            this->real_type( ) != target->real_type()

    This method decodes metadata according to this->real_type()
    and creates a new field also according to this->real_type().

    In some cases it lurks into "target", to get some extra information, e.g.:
    - unsigned_flag for numeric fields
    - charset() for string fields
    - typelib and field_length for SET and ENUM
    - geom_type and srid for GEOMETRY
    This information is not available in the binary log, so
    we assume that these fields are the same on the master and on the slave.
  */
  virtual Field *make_conversion_table_field(TABLE *TABLE,
                                             uint metadata,
                                             const Field *target) const= 0;
  virtual void make_sort_key(uchar *to, Item *item,
                             const SORT_FIELD_ATTR *sort_field,
                             Sort_param *param) const= 0;
  virtual void sortlength(THD *thd,
                          const Type_std_attributes *item,
                          SORT_FIELD_ATTR *attr) const= 0;

  virtual uint32 max_display_length(const Item *item) const= 0;
  virtual int Item_save_in_field(Item *item, Field *field,
                                 bool no_conversions) const= 0;

  /**
    Return a string representation of the Item value.

    @param thd     thread handle
    @param str     string buffer for representation of the value

    @note
      If the item has a string result type, the string is escaped
      according to its character set.

    @retval
      NULL      on error
    @retval
      non-NULL  a pointer to a a valid string on success
  */
  virtual String *print_item_value(THD *thd, Item *item, String *str) const= 0;

  /**
    Check if
      WHERE expr=value AND expr=const
    can be rewritten as:
      WHERE const=value AND expr=const

    "this" is the comparison handler that is used by "target".

    @param target       - the predicate expr=value,
                          whose "expr" argument will be replaced to "const".
    @param target_expr  - the target's "expr" which will be replaced to "const".
    @param target_value - the target's second argument, it will remain unchanged.
    @param source       - the equality predicate expr=const (or expr<=>const)
                          that can be used to rewrite the "target" part
                          (under certain conditions, see the code).
    @param source_expr  - the source's "expr". It should be exactly equal to
                          the target's "expr" to make condition rewrite possible.
    @param source_const - the source's "const" argument, it will be inserted
                          into "target" instead of "expr".
  */
  virtual bool
  can_change_cond_ref_to_const(Item_bool_func2 *target,
                               Item *target_expr, Item *target_value,
                               Item_bool_func2 *source,
                               Item *source_expr, Item *source_const) const= 0;
  virtual Item_cache *Item_get_cache(THD *thd, const Item *item) const= 0;
  virtual bool set_comparator_func(Arg_comparator *cmp) const= 0;
  virtual bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                               Item **items,
                                               uint nitems) const= 0;
  virtual bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *) const= 0;
  virtual String *Item_func_hex_val_str_ascii(Item_func_hex *item,
                                              String *str) const= 0;

  virtual
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const= 0;
  virtual
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const= 0;
  virtual
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const= 0;
  virtual
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const= 0;
  virtual
  bool Item_func_hybrid_field_type_get_date(Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            ulonglong fuzzydate) const= 0;
  virtual
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const= 0;
  virtual
  double Item_func_min_max_val_real(Item_func_min_max *) const= 0;
  virtual
  longlong Item_func_min_max_val_int(Item_func_min_max *) const= 0;
  virtual
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const= 0;
  virtual
  bool Item_func_min_max_get_date(Item_func_min_max*,
                                  MYSQL_TIME *, ulonglong fuzzydate) const= 0;
  virtual longlong
  Item_func_between_val_int(Item_func_between *func) const= 0;

  virtual cmp_item *
  make_cmp_item(THD *thd, CHARSET_INFO *cs) const= 0;

  virtual in_vector *
  make_in_vector(THD *thd, const Item_func_in *func, uint nargs) const= 0;

  virtual bool
  Item_func_in_fix_comparator_compatible_types(THD *thd, Item_func_in *)
                                                               const= 0;

  virtual bool
  Item_func_round_fix_length_and_dec(Item_func_round *round) const= 0;
};


/*
  Special handler for ROW
*/
class Type_handler_row: public Type_handler
{
  static const Name m_name_row;
public:
  virtual ~Type_handler_row() {}
  const Name name() const { return m_name_row; }
  enum_field_types field_type() const
  {
    DBUG_ASSERT(0);
    return MYSQL_TYPE_NULL;
  };
  Item_result result_type() const
  {
    return ROW_RESULT;
  }
  Item_result cmp_type() const
  {
    return ROW_RESULT;
  }
  const Type_handler *type_handler_for_comparison() const;
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  Field *make_conversion_table_field(TABLE *TABLE,
                                     uint metadata,
                                     const Field *target) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  void make_sort_key(uchar *to, Item *item,
                     const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const
  {
    DBUG_ASSERT(0);
  }
  void sortlength(THD *thd, const Type_std_attributes *item,
                            SORT_FIELD_ATTR *attr) const
  {
    DBUG_ASSERT(0);
  }
  uint32 max_display_length(const Item *item) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const
  {
    DBUG_ASSERT(0);
    return 1;
  }
  String *print_item_value(THD *thd, Item *item, String *str) const;
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const
  {
    DBUG_ASSERT(0);
    return false;
  }
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const
  {
    DBUG_ASSERT(0);
    return 0.0;
  }
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool Item_func_hybrid_field_type_get_date(Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            ulonglong fuzzydate) const
  {
    DBUG_ASSERT(0);
    return true;
  }

  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  double Item_func_min_max_val_real(Item_func_min_max *) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  longlong Item_func_min_max_val_int(Item_func_min_max *) const
  {
    DBUG_ASSERT(0);
    return 0;
  }
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const
  {
    DBUG_ASSERT(0);
    return NULL;
  }
  bool Item_func_min_max_get_date(Item_func_min_max*,
                                  MYSQL_TIME *, ulonglong fuzzydate) const
  {
    DBUG_ASSERT(0);
    return true;
  }
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *thd, const Item_func_in *f, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
};


/*
  A common parent class for numeric data type handlers
*/
class Type_handler_numeric: public Type_handler
{
protected:
  bool Item_sum_hybrid_fix_length_and_dec_numeric(Item_sum_hybrid *func,
                                                  const Type_handler *handler)
                                                  const;
public:
  String *print_item_value(THD *thd, Item *item, String *str) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_func_min_max_get_date(Item_func_min_max*,
                                  MYSQL_TIME *, ulonglong fuzzydate) const;
  virtual ~Type_handler_numeric() { }
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const;
};


/*** Abstract classes for every XXX_RESULT */

class Type_handler_real_result: public Type_handler_numeric
{
public:
  Item_result result_type() const { return REAL_RESULT; }
  Item_result cmp_type() const { return REAL_RESULT; }
  virtual ~Type_handler_real_result() {}
  const Type_handler *type_handler_for_comparison() const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            ulonglong fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;

  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
};


class Type_handler_decimal_result: public Type_handler_numeric
{
public:
  Item_result result_type() const { return DECIMAL_RESULT; }
  Item_result cmp_type() const { return DECIMAL_RESULT; }
  virtual ~Type_handler_decimal_result() {};
  const Type_handler *type_handler_for_comparison() const;
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  uint32 max_display_length(const Item *item) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            ulonglong fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
};


class Type_handler_int_result: public Type_handler_numeric
{
public:
  Item_result result_type() const { return INT_RESULT; }
  Item_result cmp_type() const { return INT_RESULT; }
  virtual ~Type_handler_int_result() {}
  const Type_handler *type_handler_for_comparison() const;
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            ulonglong fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
};


class Type_handler_temporal_result: public Type_handler
{
public:
  Item_result result_type() const { return STRING_RESULT; }
  Item_result cmp_type() const { return TIME_RESULT; }
  virtual ~Type_handler_temporal_result() {}
  void make_sort_key(uchar *to, Item *item,  const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  uint32 max_display_length(const Item *item) const;
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            ulonglong fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_func_min_max_get_date(Item_func_min_max*,
                                  MYSQL_TIME *, ulonglong fuzzydate) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
};


class Type_handler_string_result: public Type_handler
{
public:
  Item_result result_type() const { return STRING_RESULT; }
  Item_result cmp_type() const { return STRING_RESULT; }
  CHARSET_INFO *charset_for_protocol(const Item *item) const;
  virtual ~Type_handler_string_result() {}
  const Type_handler *type_handler_for_comparison() const;
  const Type_handler *
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
  uint32 max_display_length(const Item *item) const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  String *print_item_value(THD *thd, Item *item, String *str) const
  {
    return print_item_value_csstr(thd, item, str);
  }
  bool can_change_cond_ref_to_const(Item_bool_func2 *target,
                                   Item *target_expr, Item *target_value,
                                   Item_bool_func2 *source,
                                   Item *source_expr, Item *source_const) const;
  Item_cache *Item_get_cache(THD *thd, const Item *item) const;
  bool set_comparator_func(Arg_comparator *cmp) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const;
  bool Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const;
  String *Item_func_hex_val_str_ascii(Item_func_hex *item, String *str) const;
  String *Item_func_hybrid_field_type_val_str(Item_func_hybrid_field_type *,
                                              String *) const;
  double Item_func_hybrid_field_type_val_real(Item_func_hybrid_field_type *)
                                              const;
  longlong Item_func_hybrid_field_type_val_int(Item_func_hybrid_field_type *)
                                               const;
  my_decimal *Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *,
                                              my_decimal *) const;
  bool Item_func_hybrid_field_type_get_date(Item_func_hybrid_field_type *,
                                            MYSQL_TIME *,
                                            ulonglong fuzzydate) const;
  String *Item_func_min_max_val_str(Item_func_min_max *, String *) const;
  double Item_func_min_max_val_real(Item_func_min_max *) const;
  longlong Item_func_min_max_val_int(Item_func_min_max *) const;
  my_decimal *Item_func_min_max_val_decimal(Item_func_min_max *,
                                            my_decimal *) const;
  bool Item_func_min_max_get_date(Item_func_min_max*,
                                  MYSQL_TIME *, ulonglong fuzzydate) const;
  longlong Item_func_between_val_int(Item_func_between *func) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
  bool Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *) const;
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
};


/***
  Instantiable classes for every MYSQL_TYPE_XXX

  There are no Type_handler_xxx for the following types:
  - MYSQL_TYPE_VAR_STRING (old VARCHAR) - mapped to MYSQL_TYPE_VARSTRING
  - MYSQL_TYPE_ENUM                     - mapped to MYSQL_TYPE_VARSTRING
  - MYSQL_TYPE_SET:                     - mapped to MYSQL_TYPE_VARSTRING

  because the functionality that currently uses Type_handler
  (e.g. hybrid type functions) does not need to distinguish between
  these types and VARCHAR.
  For example:
    CREATE TABLE t2 AS SELECT COALESCE(enum_column) FROM t1;
  creates a VARCHAR column.

  There most likely be Type_handler_enum and Type_handler_set later,
  when the Type_handler infrastructure gets used in more pieces of the code.
*/


class Type_handler_tiny: public Type_handler_int_result
{
  static const Name m_name_tiny;
public:
  virtual ~Type_handler_tiny() {}
  const Name name() const { return m_name_tiny; }
  enum_field_types field_type() const { return MYSQL_TYPE_TINY; }
  uint32 max_display_length(const Item *item) const { return 4; }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
};


class Type_handler_short: public Type_handler_int_result
{
  static const Name m_name_short;
public:
  virtual ~Type_handler_short() {}
  const Name name() const { return m_name_short; }
  enum_field_types field_type() const { return MYSQL_TYPE_SHORT; }
  uint32 max_display_length(const Item *item) const { return 6; }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
};


class Type_handler_long: public Type_handler_int_result
{
  static const Name m_name_int;
public:
  virtual ~Type_handler_long() {}
  const Name name() const { return m_name_int; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONG; }
  uint32 max_display_length(const Item *item) const
  {
    return MY_INT32_NUM_DECIMAL_DIGITS;
  }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
};


class Type_handler_longlong: public Type_handler_int_result
{
  static const Name m_name_longlong;
public:
  virtual ~Type_handler_longlong() {}
  const Name name() const { return m_name_longlong; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
  uint32 max_display_length(const Item *item) const { return 20; }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
};


class Type_handler_int24: public Type_handler_int_result
{
  static const Name m_name_mediumint;
public:
  virtual ~Type_handler_int24() {}
  const Name name() const { return m_name_mediumint; }
  enum_field_types field_type() const { return MYSQL_TYPE_INT24; }
  uint32 max_display_length(const Item *item) const { return 8; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_year: public Type_handler_int_result
{
  static const Name m_name_year;
public:
  virtual ~Type_handler_year() {}
  const Name name() const { return m_name_year; }
  enum_field_types field_type() const { return MYSQL_TYPE_YEAR; }
  uint32 max_display_length(const Item *item) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_bit: public Type_handler_int_result
{
  static const Name m_name_bit;
public:
  virtual ~Type_handler_bit() {}
  const Name name() const { return m_name_bit; }
  enum_field_types field_type() const { return MYSQL_TYPE_BIT; }
  uint32 max_display_length(const Item *item) const;
  String *print_item_value(THD *thd, Item *item, String *str) const
  {
    return print_item_value_csstr(thd, item, str);
  }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_float: public Type_handler_real_result
{
  static const Name m_name_float;
public:
  virtual ~Type_handler_float() {}
  const Name name() const { return m_name_float; }
  enum_field_types field_type() const { return MYSQL_TYPE_FLOAT; }
  uint32 max_display_length(const Item *item) const { return 25; }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_double: public Type_handler_real_result
{
  static const Name m_name_double;
public:
  virtual ~Type_handler_double() {}
  const Name name() const { return m_name_double; }
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  uint32 max_display_length(const Item *item) const { return 53; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_time_common: public Type_handler_temporal_result
{
  static const Name m_name_time;
public:
  virtual ~Type_handler_time_common() { }
  const Name name() const { return m_name_time; }
  enum_field_types field_type() const { return MYSQL_TYPE_TIME; }
  const Type_handler *type_handler_for_comparison() const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  String *print_item_value(THD *thd, Item *item, String *str) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
};


class Type_handler_time: public Type_handler_time_common
{
public:
  virtual ~Type_handler_time() {}
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_time2: public Type_handler_time_common
{
public:
  virtual ~Type_handler_time2() {}
  enum_field_types real_field_type() const { return MYSQL_TYPE_TIME2; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_temporal_with_date: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_temporal_with_date() {}
  const Type_handler *type_handler_for_comparison() const;
  int Item_save_in_field(Item *item, Field *field, bool no_conversions) const;
  cmp_item *make_cmp_item(THD *thd, CHARSET_INFO *cs) const;
  in_vector *make_in_vector(THD *, const Item_func_in *, uint nargs) const;
};


class Type_handler_date_common: public Type_handler_temporal_with_date
{
  static const Name m_name_date;
public:
  virtual ~Type_handler_date_common() {}
  const Name name() const { return m_name_date; }
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
  String *print_item_value(THD *thd, Item *item, String *str) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const;
};

class Type_handler_date: public Type_handler_date_common
{
public:
  virtual ~Type_handler_date() {}
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_newdate: public Type_handler_date_common
{
public:
  virtual ~Type_handler_newdate() {}
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_datetime_common: public Type_handler_temporal_with_date
{
  static const Name m_name_datetime;
public:
  virtual ~Type_handler_datetime_common() {}
  const Name name() const { return m_name_datetime; }
  enum_field_types field_type() const { return MYSQL_TYPE_DATETIME; }
  String *print_item_value(THD *thd, Item *item, String *str) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const;
};


class Type_handler_datetime: public Type_handler_datetime_common
{
public:
  virtual ~Type_handler_datetime() {}
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_datetime2: public Type_handler_datetime_common
{
public:
  virtual ~Type_handler_datetime2() {}
  enum_field_types real_field_type() const { return MYSQL_TYPE_DATETIME2; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_timestamp_common: public Type_handler_temporal_with_date
{
  static const Name m_name_timestamp;
public:
  virtual ~Type_handler_timestamp_common() {}
  const Name name() const { return m_name_timestamp; }
  enum_field_types field_type() const { return MYSQL_TYPE_TIMESTAMP; }
  String *print_item_value(THD *thd, Item *item, String *str) const;
  bool Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const;
};


class Type_handler_timestamp: public Type_handler_timestamp_common
{
public:
  virtual ~Type_handler_timestamp() {}
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_timestamp2: public Type_handler_timestamp_common
{
public:
  virtual ~Type_handler_timestamp2() {}
  enum_field_types real_field_type() const { return MYSQL_TYPE_TIMESTAMP2; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_olddecimal: public Type_handler_decimal_result
{
  static const Name m_name_decimal;
public:
  virtual ~Type_handler_olddecimal() {}
  const Name name() const { return m_name_decimal; }
  enum_field_types field_type() const { return MYSQL_TYPE_DECIMAL; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_newdecimal: public Type_handler_decimal_result
{
  static const Name m_name_decimal;
public:
  virtual ~Type_handler_newdecimal() {}
  const Name name() const { return m_name_decimal; }
  enum_field_types field_type() const { return MYSQL_TYPE_NEWDECIMAL; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_null: public Type_handler_string_result
{
  static const Name m_name_null;
public:
  virtual ~Type_handler_null() {}
  const Name name() const { return m_name_null; }
  enum_field_types field_type() const { return MYSQL_TYPE_NULL; }
  const Type_handler *type_handler_for_comparison() const;
  uint32 max_display_length(const Item *item) const { return 0; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_string: public Type_handler_string_result
{
  static const Name m_name_char;
public:
  virtual ~Type_handler_string() {}
  const Name name() const { return m_name_char; }
  enum_field_types field_type() const { return MYSQL_TYPE_STRING; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_varchar: public Type_handler_string_result
{
  static const Name m_name_varchar;
public:
  virtual ~Type_handler_varchar() {}
  const Name name() const { return m_name_varchar; }
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_tiny_blob: public Type_handler_string_result
{
  static const Name m_name_tinyblob;
public:
  virtual ~Type_handler_tiny_blob() {}
  const Name name() const { return m_name_tinyblob; }
  enum_field_types field_type() const { return MYSQL_TYPE_TINY_BLOB; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_medium_blob: public Type_handler_string_result
{
  static const Name m_name_mediumblob;
public:
  virtual ~Type_handler_medium_blob() {}
  const Name name() const { return m_name_mediumblob; }
  enum_field_types field_type() const { return MYSQL_TYPE_MEDIUM_BLOB; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_long_blob: public Type_handler_string_result
{
  static const Name m_name_longblob;
public:
  virtual ~Type_handler_long_blob() {}
  const Name name() const { return m_name_longblob; }
  enum_field_types field_type() const { return MYSQL_TYPE_LONG_BLOB; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_blob: public Type_handler_string_result
{
  static const Name m_name_blob;
public:
  virtual ~Type_handler_blob() {}
  const Name name() const { return m_name_blob; }
  enum_field_types field_type() const { return MYSQL_TYPE_BLOB; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


#ifdef HAVE_SPATIAL
class Type_handler_geometry: public Type_handler_string_result
{
  static const Name m_name_geometry;
public:
  virtual ~Type_handler_geometry() {}
  const Name name() const { return m_name_geometry; }
  enum_field_types field_type() const { return MYSQL_TYPE_GEOMETRY; }
  const Type_handler *type_handler_for_comparison() const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
  bool is_traditional_type() const
  {
    return false;
  }
  bool Item_func_round_fix_length_and_dec(Item_func_round *) const;
};
#endif


class Type_handler_enum: public Type_handler_string_result
{
  static const Name m_name_enum;
public:
  virtual ~Type_handler_enum() {}
  const Name name() const { return m_name_enum; }
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
  virtual enum_field_types real_field_type() const { return MYSQL_TYPE_ENUM; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_set: public Type_handler_string_result
{
  static const Name m_name_set;
public:
  virtual ~Type_handler_set() {}
  const Name name() const { return m_name_set; }
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
  virtual enum_field_types real_field_type() const { return MYSQL_TYPE_SET; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


/**
  A handler for hybrid type functions, e.g.
  COALESCE(), IF(), IFNULL(), NULLIF(), CASE,
  numeric operators,
  UNIX_TIMESTAMP(), TIME_TO_SEC().

  Makes sure that field_type(), cmp_type() and result_type()
  are always in sync to each other for hybrid functions.
*/
class Type_handler_hybrid_field_type
{
  const Type_handler *m_type_handler;
public:
  Type_handler_hybrid_field_type();
  Type_handler_hybrid_field_type(const Type_handler *handler)
   :m_type_handler(handler)
  { }
  Type_handler_hybrid_field_type(enum_field_types type)
    :m_type_handler(Type_handler::get_handler_by_field_type(type))
  { }
  Type_handler_hybrid_field_type(const Type_handler_hybrid_field_type *other)
    :m_type_handler(other->m_type_handler)
  { }
  const Type_handler *type_handler() const { return m_type_handler; }
  enum_field_types field_type() const { return m_type_handler->field_type(); }
  enum_field_types real_field_type() const
  {
    return m_type_handler->real_field_type();
  }
  Item_result result_type() const { return m_type_handler->result_type(); }
  Item_result cmp_type() const { return m_type_handler->cmp_type(); }
  void set_handler(const Type_handler *other)
  {
    m_type_handler= other;
  }
  const Type_handler *set_handler_by_result_type(Item_result type)
  {
    return (m_type_handler= Type_handler::get_handler_by_result_type(type));
  }
  const Type_handler *set_handler_by_cmp_type(Item_result type)
  {
    return (m_type_handler= Type_handler::get_handler_by_cmp_type(type));
  }
  const Type_handler *set_handler_by_result_type(Item_result type,
                                                 uint max_octet_length,
                                                 CHARSET_INFO *cs)
  {
    m_type_handler= Type_handler::get_handler_by_result_type(type);
    return m_type_handler=
      m_type_handler->type_handler_adjusted_to_max_octet_length(max_octet_length,
                                                                cs);
  }
  const Type_handler *set_handler_by_field_type(enum_field_types type)
  {
    return (m_type_handler= Type_handler::get_handler_by_field_type(type));
  }
  const Type_handler *set_handler_by_real_type(enum_field_types type)
  {
    return (m_type_handler= Type_handler::get_handler_by_real_type(type));
  }
  bool aggregate_for_comparison(const Type_handler *other);
  bool aggregate_for_comparison(const char *funcname,
                                Item **items, uint nitems,
                                bool treat_int_to_uint_as_decimal);
  bool aggregate_for_result(const Type_handler *other);
  bool aggregate_for_result(const char *funcname,
                            Item **item, uint nitems, bool treat_bit_as_number);
};


/**
  This class is used for Item_type_holder, which preserves real_type.
*/
class Type_handler_hybrid_real_field_type:
  public Type_handler_hybrid_field_type
{
public:
  Type_handler_hybrid_real_field_type(enum_field_types type)
    :Type_handler_hybrid_field_type(Type_handler::
                                    get_handler_by_real_type(type))
  { }
};


extern Type_handler_row   type_handler_row;
extern Type_handler_null  type_handler_null;
extern Type_handler_varchar type_handler_varchar;
extern Type_handler_longlong type_handler_longlong;
extern Type_handler_double type_handler_double;
extern Type_handler_newdecimal type_handler_newdecimal;
extern Type_handler_datetime type_handler_datetime;
extern Type_handler_longlong type_handler_longlong;
extern Type_handler_bit type_handler_bit;

class Type_aggregator
{
  class Pair
  {
  public:
    const Type_handler *m_handler1;
    const Type_handler *m_handler2;
    const Type_handler *m_result;
    Pair() { }
    Pair(const Type_handler *handler1,
         const Type_handler *handler2,
         const Type_handler *result)
     :m_handler1(handler1), m_handler2(handler2), m_result(result)
    { }
    bool eq(const Type_handler *handler1, const Type_handler *handler2) const
    {
      return m_handler1 == handler1 && m_handler2 == handler2;
    }
  };
  Dynamic_array<Pair> m_array;
  const Pair* find_pair(const Type_handler *handler1,
                        const Type_handler *handler2) const
  {
    for (uint i= 0; i < m_array.elements(); i++)
    {
      const Pair& el= m_array.at(i);
      if (el.eq(handler1, handler2) || el.eq(handler2, handler1))
        return &el;
    }
    return NULL;
  }
public:
  Type_aggregator()
  { }
  bool add(const Type_handler *handler1,
           const Type_handler *handler2,
           const Type_handler *result)
  {
    return m_array.append(Pair(handler1, handler2, result));
  }
  const Type_handler *find_handler(const Type_handler *handler1,
                                   const Type_handler *handler2) const
  {
    const Pair* el= find_pair(handler1, handler2);
    return el ? el->m_result : NULL;
  }
};

extern Type_aggregator type_aggregator_for_result;
extern Type_aggregator type_aggregator_for_comparison;

#endif /* SQL_TYPE_H_INCLUDED */
