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
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif


#include "mysqld.h"

class Field;
class Item;
class Type_std_attributes;
class Sort_param;
struct TABLE;
struct SORT_FIELD_ATTR;

class Type_handler
{
protected:
  const Type_handler *string_type_handler(uint max_octet_length) const;
  void make_sort_key_longlong(uchar *to,
                              bool maybe_null, bool null_value,
                              bool unsigned_flag,
                              longlong value) const;
public:
  static const Type_handler *get_handler_by_field_type(enum_field_types type);
  static const Type_handler *get_handler_by_real_type(enum_field_types type);
  virtual enum_field_types field_type() const= 0;
  virtual enum_field_types real_field_type() const { return field_type(); }
  virtual Item_result result_type() const= 0;
  virtual Item_result cmp_type() const= 0;
  virtual const Type_handler*
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const
  { return this; }
  virtual ~Type_handler() {}
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
};


/*** Abstract classes for every XXX_RESULT */

class Type_handler_real_result: public Type_handler
{
public:
  Item_result result_type() const { return REAL_RESULT; }
  Item_result cmp_type() const { return REAL_RESULT; }
  virtual ~Type_handler_real_result() {}
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
};


class Type_handler_decimal_result: public Type_handler
{
public:
  Item_result result_type() const { return DECIMAL_RESULT; }
  Item_result cmp_type() const { return DECIMAL_RESULT; }
  virtual ~Type_handler_decimal_result() {};
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
};


class Type_handler_int_result: public Type_handler
{
public:
  Item_result result_type() const { return INT_RESULT; }
  Item_result cmp_type() const { return INT_RESULT; }
  virtual ~Type_handler_int_result() {}
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
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
};


class Type_handler_string_result: public Type_handler
{
public:
  Item_result result_type() const { return STRING_RESULT; }
  Item_result cmp_type() const { return STRING_RESULT; }
  virtual ~Type_handler_string_result() {}
  const Type_handler *
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const;
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const;
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const;
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
public:
  virtual ~Type_handler_tiny() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TINY; }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
};


class Type_handler_short: public Type_handler_int_result
{
public:
  virtual ~Type_handler_short() {}
  enum_field_types field_type() const { return MYSQL_TYPE_SHORT; }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
};


class Type_handler_long: public Type_handler_int_result
{
public:
  virtual ~Type_handler_long() {}
  enum_field_types field_type() const { return MYSQL_TYPE_LONG; }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
};


class Type_handler_longlong: public Type_handler_int_result
{
public:
  virtual ~Type_handler_longlong() {}
  enum_field_types field_type() const { return MYSQL_TYPE_LONGLONG; }
  Field *make_conversion_table_field(TABLE *TABLE, uint metadata,
                                     const Field *target) const;
};


class Type_handler_int24: public Type_handler_int_result
{
public:
  virtual ~Type_handler_int24() {}
  enum_field_types field_type() const { return MYSQL_TYPE_INT24; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_year: public Type_handler_int_result
{
public:
  virtual ~Type_handler_year() {}
  enum_field_types field_type() const { return MYSQL_TYPE_YEAR; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_bit: public Type_handler_int_result
{
public:
  virtual ~Type_handler_bit() {}
  enum_field_types field_type() const { return MYSQL_TYPE_BIT; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_float: public Type_handler_real_result
{
public:
  virtual ~Type_handler_float() {}
  enum_field_types field_type() const { return MYSQL_TYPE_FLOAT; }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *, const Item *) const;
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_double: public Type_handler_real_result
{
public:
  virtual ~Type_handler_double() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DOUBLE; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_time: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_time() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TIME; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_time2: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_time2() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TIME; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_TIME2; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_date: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_date() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_newdate: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_newdate() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DATE; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_datetime: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_datetime() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DATETIME; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_datetime2: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_datetime2() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DATETIME; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_DATETIME2; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_timestamp: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_timestamp() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TIMESTAMP; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_timestamp2: public Type_handler_temporal_result
{
public:
  virtual ~Type_handler_timestamp2() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TIMESTAMP; }
  enum_field_types real_field_type() const { return MYSQL_TYPE_TIMESTAMP2; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_olddecimal: public Type_handler_decimal_result
{
public:
  virtual ~Type_handler_olddecimal() {}
  enum_field_types field_type() const { return MYSQL_TYPE_DECIMAL; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_newdecimal: public Type_handler_decimal_result
{
public:
  virtual ~Type_handler_newdecimal() {}
  enum_field_types field_type() const { return MYSQL_TYPE_NEWDECIMAL; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_null: public Type_handler_string_result
{
public:
  virtual ~Type_handler_null() {}
  enum_field_types field_type() const { return MYSQL_TYPE_NULL; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_string: public Type_handler_string_result
{
public:
  virtual ~Type_handler_string() {}
  enum_field_types field_type() const { return MYSQL_TYPE_STRING; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_varchar: public Type_handler_string_result
{
public:
  virtual ~Type_handler_varchar() {}
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_blob_common: public Type_handler_string_result
{
public:
  virtual ~Type_handler_blob_common() { }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_tiny_blob: public Type_handler_blob_common
{
public:
  virtual ~Type_handler_tiny_blob() {}
  enum_field_types field_type() const { return MYSQL_TYPE_TINY_BLOB; }
};


class Type_handler_medium_blob: public Type_handler_blob_common
{
public:
  virtual ~Type_handler_medium_blob() {}
  enum_field_types field_type() const { return MYSQL_TYPE_MEDIUM_BLOB; }
};


class Type_handler_long_blob: public Type_handler_blob_common
{
public:
  virtual ~Type_handler_long_blob() {}
  enum_field_types field_type() const { return MYSQL_TYPE_LONG_BLOB; }
};


class Type_handler_blob: public Type_handler_blob_common
{
public:
  virtual ~Type_handler_blob() {}
  enum_field_types field_type() const { return MYSQL_TYPE_BLOB; }
};


#ifdef HAVE_SPATIAL
class Type_handler_geometry: public Type_handler_string_result
{
public:
  virtual ~Type_handler_geometry() {}
  enum_field_types field_type() const { return MYSQL_TYPE_GEOMETRY; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};
#endif


class Type_handler_enum: public Type_handler_string_result
{
public:
  virtual ~Type_handler_enum() {}
  enum_field_types field_type() const { return MYSQL_TYPE_VARCHAR; }
  virtual enum_field_types real_field_type() const { return MYSQL_TYPE_ENUM; }
  Field *make_conversion_table_field(TABLE *, uint metadata,
                                     const Field *target) const;
};


class Type_handler_set: public Type_handler_string_result
{
public:
  virtual ~Type_handler_set() {}
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
class Type_handler_hybrid_field_type: public Type_handler
{
  const Type_handler *m_type_handler;
  const Type_handler *get_handler_by_result_type(Item_result type) const;
public:
  Type_handler_hybrid_field_type();
  Type_handler_hybrid_field_type(const Type_handler *handler)
   :m_type_handler(handler)
  { }
  Type_handler_hybrid_field_type(enum_field_types type)
    :m_type_handler(get_handler_by_field_type(type))
  { }
  Type_handler_hybrid_field_type(const Type_handler_hybrid_field_type *other)
    :m_type_handler(other->m_type_handler)
  { }
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
    return (m_type_handler= get_handler_by_result_type(type));
  }
  const Type_handler *set_handler_by_result_type(Item_result type,
                                                 uint max_octet_length,
                                                 CHARSET_INFO *cs)
  {
    m_type_handler= get_handler_by_result_type(type);
    return m_type_handler=
      m_type_handler->type_handler_adjusted_to_max_octet_length(max_octet_length,
                                                                cs);
  }
  const Type_handler *set_handler_by_field_type(enum_field_types type)
  {
    return (m_type_handler= get_handler_by_field_type(type));
  }
  const Type_handler *set_handler_by_real_type(enum_field_types type)
  {
    return (m_type_handler= get_handler_by_real_type(type));
  }
  const Type_handler *
  type_handler_adjusted_to_max_octet_length(uint max_octet_length,
                                            CHARSET_INFO *cs) const
  {
    return
      m_type_handler->type_handler_adjusted_to_max_octet_length(max_octet_length,
                                                                cs);
  }
  Field *make_num_distinct_aggregator_field(MEM_ROOT *mem_root,
                                            const Item *item) const
  {
    return m_type_handler->make_num_distinct_aggregator_field(mem_root, item);
  }
  Field *make_conversion_table_field(TABLE *table, uint metadata,
                                     const Field *target) const
  {
    return m_type_handler->make_conversion_table_field(table, metadata, target);
  }
  void make_sort_key(uchar *to, Item *item, const SORT_FIELD_ATTR *sort_field,
                     Sort_param *param) const
  {
    m_type_handler->make_sort_key(to, item, sort_field, param);
  }
  void sortlength(THD *thd,
                  const Type_std_attributes *item,
                  SORT_FIELD_ATTR *attr) const
  {
    m_type_handler->sortlength(thd, item, attr);
  }

};


/**
  This class is used for Item_type_holder, which preserves real_type.
*/
class Type_handler_hybrid_real_field_type:
  public Type_handler_hybrid_field_type
{
public:
  Type_handler_hybrid_real_field_type(enum_field_types type)
    :Type_handler_hybrid_field_type(get_handler_by_real_type(type))
  { }
};


#endif /* SQL_TYPE_H_INCLUDED */
