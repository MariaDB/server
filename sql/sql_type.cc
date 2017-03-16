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

#include "sql_type.h"
#include "sql_const.h"
#include "sql_class.h"
#include "item.h"
#include "log.h"

static Type_handler_tiny        type_handler_tiny;
static Type_handler_short       type_handler_short;
static Type_handler_long        type_handler_long;
static Type_handler_int24       type_handler_int24;
static Type_handler_year        type_handler_year;
static Type_handler_float       type_handler_float;
static Type_handler_time        type_handler_time;
static Type_handler_time2       type_handler_time2;
static Type_handler_date        type_handler_date;
static Type_handler_newdate     type_handler_newdate;
static Type_handler_datetime2   type_handler_datetime2;
static Type_handler_timestamp   type_handler_timestamp;
static Type_handler_timestamp2  type_handler_timestamp2;
static Type_handler_olddecimal  type_handler_olddecimal;
static Type_handler_string      type_handler_string;
static Type_handler_tiny_blob   type_handler_tiny_blob;
static Type_handler_medium_blob type_handler_medium_blob;
static Type_handler_long_blob   type_handler_long_blob;
static Type_handler_blob        type_handler_blob;
static Type_handler_enum        type_handler_enum;
static Type_handler_set         type_handler_set;


Type_handler_null        type_handler_null;
Type_handler_row         type_handler_row;
Type_handler_varchar     type_handler_varchar;
Type_handler_longlong    type_handler_longlong;
Type_handler_double      type_handler_double;
Type_handler_newdecimal  type_handler_newdecimal;
Type_handler_datetime    type_handler_datetime;
Type_handler_bit         type_handler_bit;

#ifdef HAVE_SPATIAL
Type_handler_geometry    type_handler_geometry;
#endif


Type_aggregator type_aggregator_for_result;
Type_aggregator type_aggregator_for_comparison;


class Static_data_initializer
{
public:
  static Static_data_initializer m_singleton;
  Static_data_initializer()
  {
#ifdef HAVE_SPATIAL
    type_aggregator_for_result.add(&type_handler_geometry,
                                   &type_handler_null,
                                   &type_handler_geometry);
    type_aggregator_for_result.add(&type_handler_geometry,
                                   &type_handler_geometry,
                                   &type_handler_geometry);
    type_aggregator_for_result.add(&type_handler_geometry,
                                   &type_handler_blob,
                                   &type_handler_long_blob);
    type_aggregator_for_result.add(&type_handler_geometry,
                                   &type_handler_varchar,
                                   &type_handler_long_blob);
    type_aggregator_for_result.add(&type_handler_geometry,
                                   &type_handler_string,
                                   &type_handler_long_blob);

    type_aggregator_for_comparison.add(&type_handler_geometry,
                                       &type_handler_geometry,
                                       &type_handler_geometry);
    type_aggregator_for_comparison.add(&type_handler_geometry,
                                       &type_handler_null,
                                       &type_handler_geometry);
    type_aggregator_for_comparison.add(&type_handler_geometry,
                                       &type_handler_long_blob,
                                       &type_handler_long_blob);
#endif
  }
};

Static_data_initializer Static_data_initializer::m_singleton;


void Type_std_attributes::set(const Field *field)
{
  decimals= field->decimals();
  unsigned_flag= MY_TEST(field->flags & UNSIGNED_FLAG);
  collation.set(field->charset(), field->derivation(), field->repertoire());
  fix_char_length(field->char_length());
}


/**
  This method is used by:
  - Item_user_var_as_out_param::field_type()
  - Item_func_udf_str::field_type()
  - Item_empty_string::make_field()

  TODO: type_handler_adjusted_to_max_octet_length() and string_type_handler()
  provide very similar functionality, to properly choose between
  VARCHAR/VARBINARY vs TEXT/BLOB variations taking into accoung maximum
  possible octet length.

  We should probably get rid of either of them and use the same method
  all around the code.
*/
const Type_handler *
Type_handler::string_type_handler(uint max_octet_length)
{
  if (max_octet_length >= 16777216)
    return &type_handler_long_blob;
  else if (max_octet_length >= 65536)
    return &type_handler_medium_blob;
  return &type_handler_varchar;
}


/**
  This method is used by:
  - Item_sum_hybrid, e.g. MAX(item), MIN(item).
  - Item_func_set_user_var
*/
const Type_handler *
Type_handler_string_result::type_handler_adjusted_to_max_octet_length(
                                                        uint max_octet_length,
                                                        CHARSET_INFO *cs) const
{
  if (max_octet_length / cs->mbmaxlen <= CONVERT_IF_BIGGER_TO_BLOB)
    return &type_handler_varchar; // See also Item::too_big_for_varchar()
  if (max_octet_length >= 16777216)
    return &type_handler_long_blob;
  else if (max_octet_length >= 65536)
    return &type_handler_medium_blob;
  return &type_handler_blob;
}


CHARSET_INFO *Type_handler::charset_for_protocol(const Item *item) const
{
  /*
    For backward compatibility, to make numeric
    data types return "binary" charset in client-side metadata.
  */
  return &my_charset_bin;
}


CHARSET_INFO *
Type_handler_string_result::charset_for_protocol(const Item *item) const
{
  return item->collation.collation;
}


const Type_handler *
Type_handler::get_handler_by_cmp_type(Item_result type)
{
  switch (type) {
  case REAL_RESULT:       return &type_handler_double;
  case INT_RESULT:        return &type_handler_longlong;
  case DECIMAL_RESULT:    return &type_handler_newdecimal;
  case STRING_RESULT:     return &type_handler_long_blob;
  case TIME_RESULT:       return &type_handler_datetime;
  case ROW_RESULT:        return &type_handler_row;
  }
  DBUG_ASSERT(0);
  return &type_handler_string;
}


Type_handler_hybrid_field_type::Type_handler_hybrid_field_type()
  :m_type_handler(&type_handler_double)
{
}


/***************************************************************************/
const Name Type_handler_row::m_name_row(C_STRING_WITH_LEN("row"));

const Name Type_handler_null::m_name_null(C_STRING_WITH_LEN("null"));

const Name
  Type_handler_string::m_name_char(C_STRING_WITH_LEN("char")),
  Type_handler_varchar::m_name_varchar(C_STRING_WITH_LEN("varchar")),
  Type_handler_tiny_blob::m_name_tinyblob(C_STRING_WITH_LEN("tinyblob")),
  Type_handler_medium_blob::m_name_mediumblob(C_STRING_WITH_LEN("mediumblob")),
  Type_handler_long_blob::m_name_longblob(C_STRING_WITH_LEN("longblob")),
  Type_handler_blob::m_name_blob(C_STRING_WITH_LEN("blob"));

const Name
  Type_handler_enum::m_name_enum(C_STRING_WITH_LEN("enum")),
  Type_handler_set::m_name_set(C_STRING_WITH_LEN("set"));

const Name
  Type_handler_tiny::m_name_tiny(C_STRING_WITH_LEN("tinyint")),
  Type_handler_short::m_name_short(C_STRING_WITH_LEN("smallint")),
  Type_handler_long::m_name_int(C_STRING_WITH_LEN("int")),
  Type_handler_longlong::m_name_longlong(C_STRING_WITH_LEN("bigint")),
  Type_handler_int24::m_name_mediumint(C_STRING_WITH_LEN("mediumint")),
  Type_handler_year::m_name_year(C_STRING_WITH_LEN("year")),
  Type_handler_bit::m_name_bit(C_STRING_WITH_LEN("bit"));

const Name
  Type_handler_float::m_name_float(C_STRING_WITH_LEN("float")),
  Type_handler_double::m_name_double(C_STRING_WITH_LEN("double"));

const Name
  Type_handler_olddecimal::m_name_decimal(C_STRING_WITH_LEN("decimal")),
  Type_handler_newdecimal::m_name_decimal(C_STRING_WITH_LEN("decimal"));

const Name
  Type_handler_time_common::m_name_time(C_STRING_WITH_LEN("time")),
  Type_handler_date_common::m_name_date(C_STRING_WITH_LEN("date")),
  Type_handler_datetime_common::m_name_datetime(C_STRING_WITH_LEN("datetime")),
  Type_handler_timestamp_common::m_name_timestamp(C_STRING_WITH_LEN("timestamp"));

/***************************************************************************/

const Type_handler *Type_handler_null::type_handler_for_comparison() const
{
  return &type_handler_null;
}


const Type_handler *Type_handler_int_result::type_handler_for_comparison() const
{
  return &type_handler_longlong;
}


const Type_handler *Type_handler_string_result::type_handler_for_comparison() const
{
  return &type_handler_long_blob;
}


const Type_handler *Type_handler_decimal_result::type_handler_for_comparison() const
{
  return &type_handler_newdecimal;
}


const Type_handler *Type_handler_real_result::type_handler_for_comparison() const
{
  return &type_handler_double;
}


const Type_handler *Type_handler_time_common::type_handler_for_comparison() const
{
  return &type_handler_time;
}

const Type_handler *Type_handler_temporal_with_date::type_handler_for_comparison() const
{
  return &type_handler_datetime;
}


const Type_handler *Type_handler_row::type_handler_for_comparison() const
{
  return &type_handler_row;
}


/***************************************************************************/

bool
Type_handler_hybrid_field_type::aggregate_for_result(const Type_handler *other)
{
  if (m_type_handler->is_traditional_type() && other->is_traditional_type())
  {
    m_type_handler=
      Type_handler::aggregate_for_result_traditional(m_type_handler, other);
    return false;
  }
  other= type_aggregator_for_result.find_handler(m_type_handler, other);
  if (!other)
    return true;
  m_type_handler= other;
  return false;
}


/**
  @brief Aggregates field types from the array of items.

  @param[in] items  array of items to aggregate the type from
  @param[in] nitems number of items in the array
  @param[in] treat_bit_as_number - if BIT should be aggregated to a non-BIT
             counterpart as a LONGLONG number or as a VARBINARY string.

             Currently behaviour depends on the function:
             - LEAST/GREATEST treat BIT as VARBINARY when
               aggregating with a non-BIT counterpart.
               Note, UNION also works this way.

             - CASE, COALESCE, IF, IFNULL treat BIT as LONGLONG when
               aggregating with a non-BIT counterpart;

             This inconsistency may be changed in the future. See MDEV-8867.

             Note, independently from "treat_bit_as_number":
             - a single BIT argument gives BIT as a result
             - two BIT couterparts give BIT as a result

  @details This function aggregates field types from the array of items.
    Found type is supposed to be used later as the result field type
    of a multi-argument function.
    Aggregation itself is performed by Type_handler::aggregate_for_result().

  @note The term "aggregation" is used here in the sense of inferring the
    result type of a function from its argument types.

  @retval false - on success
  @retval true  - on error
*/

bool
Type_handler_hybrid_field_type::aggregate_for_result(const char *funcname,
                                                     Item **items, uint nitems,
                                                     bool treat_bit_as_number)
{
  if (!nitems || items[0]->result_type() == ROW_RESULT)
  {
    DBUG_ASSERT(0);
    set_handler(&type_handler_null);
    return true;
  }
  set_handler(items[0]->type_handler());
  uint unsigned_count= items[0]->unsigned_flag;
  for (uint i= 1 ; i < nitems ; i++)
  {
    const Type_handler *cur= items[i]->type_handler();
    if (treat_bit_as_number &&
        ((type_handler() == &type_handler_bit) ^ (cur == &type_handler_bit)))
    {
      if (type_handler() == &type_handler_bit)
        set_handler(&type_handler_longlong); // BIT + non-BIT
      else
        cur= &type_handler_longlong; // non-BIT + BIT
    }
    if (aggregate_for_result(cur))
    {
      my_error(ER_ILLEGAL_PARAMETER_DATA_TYPES2_FOR_OPERATION, MYF(0),
               type_handler()->name().ptr(), cur->name().ptr(), funcname);
      return true;
    }
    unsigned_count+= items[i]->unsigned_flag;
  }
  switch (field_type()) {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_BIT:
    if (unsigned_count != 0 && unsigned_count != nitems)
    {
      /*
        If all arguments are of INT-alike type but have different
        unsigned_flag, then convert to DECIMAL.
      */
      set_handler(&type_handler_newdecimal);
    }
  default:
    break;
  }
  return false;
}

/**
  Collect built-in data type handlers for comparison.
  This method is very similar to item_cmp_type() defined in item.cc.
  Now they coexist. Later item_cmp_type() will be removed.
  In addition to item_cmp_type(), this method correctly aggregates
  TIME with DATETIME/TIMESTAMP/DATE, so no additional find_date_time_item()
  is needed after this call.
*/

bool
Type_handler_hybrid_field_type::aggregate_for_comparison(const Type_handler *h)
{
  DBUG_ASSERT(m_type_handler == m_type_handler->type_handler_for_comparison());
  DBUG_ASSERT(h == h->type_handler_for_comparison());

  if (!m_type_handler->is_traditional_type() ||
      !h->is_traditional_type())
  {
    h= type_aggregator_for_comparison.find_handler(m_type_handler, h);
    if (!h)
      return true;
    m_type_handler= h;
    DBUG_ASSERT(m_type_handler == m_type_handler->type_handler_for_comparison());
    return false;
  }

  Item_result a= cmp_type();
  Item_result b= h->cmp_type();
  if (a == STRING_RESULT && b == STRING_RESULT)
    m_type_handler= &type_handler_long_blob;
  else if (a == INT_RESULT && b == INT_RESULT)
    m_type_handler= &type_handler_longlong;
  else if (a == ROW_RESULT || b == ROW_RESULT)
    m_type_handler= &type_handler_row;
  else if (a == TIME_RESULT || b == TIME_RESULT)
  {
    if ((a == TIME_RESULT) + (b == TIME_RESULT) == 1)
    {
      /*
        We're here if there's only one temporal data type:
        either m_type_handler or h.
      */
      if (b == TIME_RESULT)
        m_type_handler= h; // Temporal types bit non-temporal types
    }
    else
    {
      /*
        We're here if both m_type_handler and h are temporal data types.
      */
      if (field_type() != MYSQL_TYPE_TIME || h->field_type() != MYSQL_TYPE_TIME)
        m_type_handler= &type_handler_datetime; // DATETIME bits TIME
    }
  }
  else if ((a == INT_RESULT || a == DECIMAL_RESULT) &&
           (b == INT_RESULT || b == DECIMAL_RESULT))
  {
    m_type_handler= &type_handler_newdecimal;
  }
  else
    m_type_handler= &type_handler_double;
  DBUG_ASSERT(m_type_handler == m_type_handler->type_handler_for_comparison());
  return false;
}


/***************************************************************************/

const Type_handler *
Type_handler::get_handler_by_field_type(enum_field_types type)
{
  switch (type) {
  case MYSQL_TYPE_DECIMAL:     return &type_handler_olddecimal;
  case MYSQL_TYPE_NEWDECIMAL:  return &type_handler_newdecimal;
  case MYSQL_TYPE_TINY:        return &type_handler_tiny;
  case MYSQL_TYPE_SHORT:       return &type_handler_short;
  case MYSQL_TYPE_LONG:        return &type_handler_long;
  case MYSQL_TYPE_LONGLONG:    return &type_handler_longlong;
  case MYSQL_TYPE_INT24:       return &type_handler_int24;
  case MYSQL_TYPE_YEAR:        return &type_handler_year;
  case MYSQL_TYPE_BIT:         return &type_handler_bit;
  case MYSQL_TYPE_FLOAT:       return &type_handler_float;
  case MYSQL_TYPE_DOUBLE:      return &type_handler_double;
  case MYSQL_TYPE_NULL:        return &type_handler_null;
  case MYSQL_TYPE_VARCHAR:     return &type_handler_varchar;
  case MYSQL_TYPE_TINY_BLOB:   return &type_handler_tiny_blob;
  case MYSQL_TYPE_MEDIUM_BLOB: return &type_handler_medium_blob;
  case MYSQL_TYPE_LONG_BLOB:   return &type_handler_long_blob;
  case MYSQL_TYPE_BLOB:        return &type_handler_blob;
  case MYSQL_TYPE_VAR_STRING:  return &type_handler_varchar; // Map to VARCHAR 
  case MYSQL_TYPE_STRING:      return &type_handler_string;
  case MYSQL_TYPE_ENUM:        return &type_handler_varchar; // Map to VARCHAR
  case MYSQL_TYPE_SET:         return &type_handler_varchar; // Map to VARCHAR
  case MYSQL_TYPE_GEOMETRY:
#ifdef HAVE_SPATIAL
    return &type_handler_geometry;
#else
    return NULL;
#endif
  case MYSQL_TYPE_TIMESTAMP:   return &type_handler_timestamp2;// Map to timestamp2
  case MYSQL_TYPE_TIMESTAMP2:  return &type_handler_timestamp2;
  case MYSQL_TYPE_DATE:        return &type_handler_newdate;   // Map to newdate
  case MYSQL_TYPE_TIME:        return &type_handler_time2;     // Map to time2
  case MYSQL_TYPE_TIME2:       return &type_handler_time2;
  case MYSQL_TYPE_DATETIME:    return &type_handler_datetime2; // Map to datetime2
  case MYSQL_TYPE_DATETIME2:   return &type_handler_datetime2;
  case MYSQL_TYPE_NEWDATE:
    /*
      NEWDATE is actually a real_type(), not a field_type(),
      but it's used around the code in field_type() context.
      We should probably clean up the code not to use MYSQL_TYPE_NEWDATE
      in field_type() context and add DBUG_ASSERT(0) here.
    */
    return &type_handler_newdate;
  };
  DBUG_ASSERT(0);
  return &type_handler_string;
}


const Type_handler *
Type_handler::get_handler_by_real_type(enum_field_types type)
{
  switch (type) {
  case MYSQL_TYPE_DECIMAL:     return &type_handler_olddecimal;
  case MYSQL_TYPE_NEWDECIMAL:  return &type_handler_newdecimal;
  case MYSQL_TYPE_TINY:        return &type_handler_tiny;
  case MYSQL_TYPE_SHORT:       return &type_handler_short;
  case MYSQL_TYPE_LONG:        return &type_handler_long;
  case MYSQL_TYPE_LONGLONG:    return &type_handler_longlong;
  case MYSQL_TYPE_INT24:       return &type_handler_int24;
  case MYSQL_TYPE_YEAR:        return &type_handler_year;
  case MYSQL_TYPE_BIT:         return &type_handler_bit;
  case MYSQL_TYPE_FLOAT:       return &type_handler_float;
  case MYSQL_TYPE_DOUBLE:      return &type_handler_double;
  case MYSQL_TYPE_NULL:        return &type_handler_null;
  case MYSQL_TYPE_VARCHAR:     return &type_handler_varchar;
  case MYSQL_TYPE_TINY_BLOB:   return &type_handler_tiny_blob;
  case MYSQL_TYPE_MEDIUM_BLOB: return &type_handler_medium_blob;
  case MYSQL_TYPE_LONG_BLOB:   return &type_handler_long_blob;
  case MYSQL_TYPE_BLOB:        return &type_handler_blob;
  case MYSQL_TYPE_VAR_STRING:
    /*
      VAR_STRING is actually a field_type(), not a real_type(),
      but it's used around the code in real_type() context.
      We should clean up the code and add DBUG_ASSERT(0) here.
    */
    return &type_handler_string;
  case MYSQL_TYPE_STRING:      return &type_handler_string;
  case MYSQL_TYPE_ENUM:        return &type_handler_enum;
  case MYSQL_TYPE_SET:         return &type_handler_set;
  case MYSQL_TYPE_GEOMETRY:
#ifdef HAVE_SPATIAL
    return &type_handler_geometry;
#else
    return NULL;
#endif
  case MYSQL_TYPE_TIMESTAMP:   return &type_handler_timestamp;
  case MYSQL_TYPE_TIMESTAMP2:  return &type_handler_timestamp2;
  case MYSQL_TYPE_DATE:        return &type_handler_date;
  case MYSQL_TYPE_TIME:        return &type_handler_time;
  case MYSQL_TYPE_TIME2:       return &type_handler_time2;
  case MYSQL_TYPE_DATETIME:    return &type_handler_datetime;
  case MYSQL_TYPE_DATETIME2:   return &type_handler_datetime2;
  case MYSQL_TYPE_NEWDATE:     return &type_handler_newdate;
  };
  DBUG_ASSERT(0);
  return &type_handler_string;
}


/**
  Create a DOUBLE field by default.
*/
Field *
Type_handler::make_num_distinct_aggregator_field(MEM_ROOT *mem_root,
                                                 const Item *item) const
{
  return new(mem_root)
         Field_double(NULL, item->max_length,
                      (uchar *) (item->maybe_null ? "" : 0),
                      item->maybe_null ? 1 : 0, Field::NONE,
                      item->name, item->decimals, 0, item->unsigned_flag);
}


Field *
Type_handler_float::make_num_distinct_aggregator_field(MEM_ROOT *mem_root,
                                                       const Item *item)
                                                       const
{
  return new(mem_root)
         Field_float(NULL, item->max_length,
                     (uchar *) (item->maybe_null ? "" : 0),
                     item->maybe_null ? 1 : 0, Field::NONE,
                     item->name, item->decimals, 0, item->unsigned_flag);
}


Field *
Type_handler_decimal_result::make_num_distinct_aggregator_field(
                                                            MEM_ROOT *mem_root,
                                                            const Item *item)
                                                            const
{
  DBUG_ASSERT(item->decimals <= DECIMAL_MAX_SCALE);
  return new (mem_root)
         Field_new_decimal(NULL, item->max_length,
                           (uchar *) (item->maybe_null ? "" : 0),
                           item->maybe_null ? 1 : 0, Field::NONE,
                           item->name, item->decimals, 0, item->unsigned_flag);
}


Field *
Type_handler_int_result::make_num_distinct_aggregator_field(MEM_ROOT *mem_root,
                                                            const Item *item)
                                                            const
{
  /**
    Make a longlong field for all INT-alike types. It could create
    smaller fields for TINYINT, SMALLINT, MEDIUMINT, INT though.
  */
  return new(mem_root)
         Field_longlong(NULL, item->max_length,
                        (uchar *) (item->maybe_null ? "" : 0),
                        item->maybe_null ? 1 : 0, Field::NONE,
                        item->name, 0, item->unsigned_flag);
}


/***********************************************************************/

#define TMPNAME ""

Field *Type_handler_tiny::make_conversion_table_field(TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  /*
    As we don't know if the integer was signed or not on the master,
    assume we have same sign on master and slave.  This is true when not
    using conversions so it should be true also when using conversions.
  */
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (table->in_use->mem_root)
         Field_tiny(NULL, 4 /*max_length*/, (uchar *) "", 1, Field::NONE,
                    TMPNAME, 0/*zerofill*/, unsigned_flag);
}


Field *Type_handler_short::make_conversion_table_field(TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (table->in_use->mem_root)
         Field_short(NULL, 6 /*max_length*/, (uchar *) "", 1, Field::NONE,
                     TMPNAME, 0/*zerofill*/, unsigned_flag);
}


Field *Type_handler_int24::make_conversion_table_field(TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (table->in_use->mem_root)
         Field_medium(NULL, 9 /*max_length*/, (uchar *) "", 1, Field::NONE,
                      TMPNAME, 0/*zerofill*/, unsigned_flag);
}


Field *Type_handler_long::make_conversion_table_field(TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (table->in_use->mem_root)
         Field_long(NULL, 11 /*max_length*/, (uchar *) "", 1, Field::NONE,
         TMPNAME, 0/*zerofill*/, unsigned_flag);
}


Field *Type_handler_longlong::make_conversion_table_field(TABLE *table,
                                                          uint metadata,
                                                          const Field *target)
                                                          const
{
  bool unsigned_flag= ((Field_num*) target)->unsigned_flag;
  return new (table->in_use->mem_root)
         Field_longlong(NULL, 20 /*max_length*/,(uchar *) "", 1, Field::NONE,
                        TMPNAME, 0/*zerofill*/, unsigned_flag);
}



Field *Type_handler_float::make_conversion_table_field(TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  return new (table->in_use->mem_root)
         Field_float(NULL, 12 /*max_length*/, (uchar *) "", 1, Field::NONE,
                     TMPNAME, 0/*dec*/, 0/*zerofill*/, 0/*unsigned_flag*/);
}


Field *Type_handler_double::make_conversion_table_field(TABLE *table,
                                                        uint metadata,
                                                        const Field *target)
                                                        const
{
  return new (table->in_use->mem_root)
         Field_double(NULL, 22 /*max_length*/, (uchar *) "", 1, Field::NONE,
                      TMPNAME, 0/*dec*/, 0/*zerofill*/, 0/*unsigned_flag*/);
}


Field *Type_handler_newdecimal::make_conversion_table_field(TABLE *table,
                                                            uint metadata,
                                                            const Field *target)
                                                            const
{
  int  precision= metadata >> 8;
  uint decimals= metadata & 0x00ff;
  uint32 max_length= my_decimal_precision_to_length(precision, decimals, false);
  DBUG_ASSERT(decimals <= DECIMAL_MAX_SCALE);
  return new (table->in_use->mem_root)
         Field_new_decimal(NULL, max_length, (uchar *) "", 1, Field::NONE,
                           TMPNAME, decimals, 0/*zerofill*/, 0/*unsigned*/);
}


Field *Type_handler_olddecimal::make_conversion_table_field(TABLE *table,
                                                            uint metadata,
                                                            const Field *target)
                                                            const
{
  sql_print_error("In RBR mode, Slave received incompatible DECIMAL field "
                  "(old-style decimal field) from Master while creating "
                  "conversion table. Please consider changing datatype on "
                  "Master to new style decimal by executing ALTER command for"
                  " column Name: %s.%s.%s.",
                  target->table->s->db.str,
                  target->table->s->table_name.str,
                  target->field_name);
  return NULL;
}


Field *Type_handler_year::make_conversion_table_field(TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new(table->in_use->mem_root)
         Field_year(NULL, 4, (uchar *) "", 1, Field::NONE, TMPNAME);
}


Field *Type_handler_null::make_conversion_table_field(TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new(table->in_use->mem_root)
         Field_null(NULL, 0, Field::NONE, TMPNAME, target->charset());
}


Field *Type_handler_timestamp::make_conversion_table_field(TABLE *table,
                                                           uint metadata,
                                                           const Field *target)
                                                           const
{
  return new_Field_timestamp(table->in_use->mem_root, NULL, (uchar *) "", 1,
                           Field::NONE, TMPNAME, table->s, target->decimals());
}


Field *Type_handler_timestamp2::make_conversion_table_field(TABLE *table,
                                                            uint metadata,
                                                            const Field *target)
                                                            const
{
  return new(table->in_use->mem_root)
         Field_timestampf(NULL, (uchar *) "", 1, Field::NONE,
                          TMPNAME, table->s, metadata);
}


Field *Type_handler_newdate::make_conversion_table_field(TABLE *table,
                                                         uint metadata,
                                                         const Field *target)
                                                         const
{
  return new(table->in_use->mem_root)
         Field_newdate(NULL, (uchar *) "", 1, Field::NONE, TMPNAME);
}


Field *Type_handler_date::make_conversion_table_field(TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new(table->in_use->mem_root)
         Field_date(NULL, (uchar *) "", 1, Field::NONE, TMPNAME);
}


Field *Type_handler_time::make_conversion_table_field(TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new_Field_time(table->in_use->mem_root, NULL, (uchar *) "", 1,
                        Field::NONE, TMPNAME, target->decimals());
}


Field *Type_handler_time2::make_conversion_table_field(TABLE *table,
                                                       uint metadata,
                                                       const Field *target)
                                                       const
{
  return new(table->in_use->mem_root)
         Field_timef(NULL, (uchar *) "", 1, Field::NONE, TMPNAME, metadata);
}


Field *Type_handler_datetime::make_conversion_table_field(TABLE *table,
                                                          uint metadata,
                                                          const Field *target)
                                                          const
{
  return new_Field_datetime(table->in_use->mem_root, NULL, (uchar *) "", 1,
                            Field::NONE, TMPNAME, target->decimals());
}


Field *Type_handler_datetime2::make_conversion_table_field(TABLE *table,
                                                           uint metadata,
                                                           const Field *target)
                                                           const
{
  return new(table->in_use->mem_root)
         Field_datetimef(NULL, (uchar *) "", 1,
                         Field::NONE, TMPNAME, metadata);
}


Field *Type_handler_bit::make_conversion_table_field(TABLE *table,
                                                     uint metadata,
                                                     const Field *target)
                                                     const
{
  DBUG_ASSERT((metadata & 0xff) <= 7);
  uint32 max_length= 8 * (metadata >> 8U) + (metadata & 0x00ff);
  return new(table->in_use->mem_root)
         Field_bit_as_char(NULL, max_length, (uchar *) "", 1,
                           Field::NONE, TMPNAME);
}


Field *Type_handler_string::make_conversion_table_field(TABLE *table,
                                                        uint metadata,
                                                        const Field *target)
                                                        const
{
  /* This is taken from Field_string::unpack. */
  uint32 max_length= (((metadata >> 4) & 0x300) ^ 0x300) + (metadata & 0x00ff);
  return new(table->in_use->mem_root)
         Field_string(NULL, max_length, (uchar *) "", 1,
                      Field::NONE, TMPNAME, target->charset());
}


Field *Type_handler_varchar::make_conversion_table_field(TABLE *table,
                                                         uint metadata,
                                                         const Field *target)
                                                         const
{
  return new(table->in_use->mem_root)
         Field_varstring(NULL, metadata, HA_VARCHAR_PACKLENGTH(metadata),
                         (uchar *) "", 1, Field::NONE, TMPNAME,
                         table->s, target->charset());
}


Field *Type_handler_tiny_blob::make_conversion_table_field(TABLE *table,
                                                           uint metadata,
                                                           const Field *target)
                                                           const
{
  return new(table->in_use->mem_root)
         Field_blob(NULL, (uchar *) "", 1, Field::NONE, TMPNAME,
                    table->s, 1, target->charset());
}


Field *Type_handler_blob::make_conversion_table_field(TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  return new(table->in_use->mem_root)
         Field_blob(NULL, (uchar *) "", 1, Field::NONE, TMPNAME,
                    table->s, 2, target->charset());
}


Field *Type_handler_medium_blob::make_conversion_table_field(TABLE *table,
                                                           uint metadata,
                                                           const Field *target)
                                                           const
{
  return new(table->in_use->mem_root)
         Field_blob(NULL, (uchar *) "", 1, Field::NONE, TMPNAME,
                    table->s, 3, target->charset());
}


Field *Type_handler_long_blob::make_conversion_table_field(TABLE *table,
                                                           uint metadata,
                                                           const Field *target)
                                                           const
{
  return new(table->in_use->mem_root)
         Field_blob(NULL, (uchar *) "", 1, Field::NONE, TMPNAME,
                    table->s, 4, target->charset());
}


#ifdef HAVE_SPATIAL
const Name Type_handler_geometry::m_name_geometry(C_STRING_WITH_LEN("geometry"));


const Type_handler *Type_handler_geometry::type_handler_for_comparison() const
{
  return &type_handler_geometry;
}


Field *Type_handler_geometry::make_conversion_table_field(TABLE *table,
                                                          uint metadata,
                                                          const Field *target)
                                                          const
{
  DBUG_ASSERT(target->type() == MYSQL_TYPE_GEOMETRY);
  /*
    We do not do not update feature_gis statistics here:
    status_var_increment(target->table->in_use->status_var.feature_gis);
    as this is only a temporary field.
    The statistics was already incremented when "target" was created.
  */
  return new(table->in_use->mem_root)
         Field_geom(NULL, (uchar *) "", 1, Field::NONE, TMPNAME, table->s, 4,
                    ((const Field_geom*) target)->geom_type,
                    ((const Field_geom*) target)->srid);
}
#endif

Field *Type_handler_enum::make_conversion_table_field(TABLE *table,
                                                      uint metadata,
                                                      const Field *target)
                                                      const
{
  DBUG_ASSERT(target->type() == MYSQL_TYPE_STRING);
  DBUG_ASSERT(target->real_type() == MYSQL_TYPE_ENUM);
  return new(table->in_use->mem_root)
         Field_enum(NULL, target->field_length,
                    (uchar *) "", 1, Field::NONE, TMPNAME,
                    metadata & 0x00ff/*pack_length()*/,
                    ((const Field_enum*) target)->typelib, target->charset());
}


Field *Type_handler_set::make_conversion_table_field(TABLE *table,
                                                     uint metadata,
                                                     const Field *target)
                                                     const
{
  DBUG_ASSERT(target->type() == MYSQL_TYPE_STRING);
  DBUG_ASSERT(target->real_type() == MYSQL_TYPE_SET);
  return new(table->in_use->mem_root)
         Field_set(NULL, target->field_length,
                   (uchar *) "", 1, Field::NONE, TMPNAME,
                   metadata & 0x00ff/*pack_length()*/,
                   ((const Field_enum*) target)->typelib, target->charset());
}

/*************************************************************************/

uint32 Type_handler_decimal_result::max_display_length(const Item *item) const
{
  return item->max_length;
}


uint32 Type_handler_temporal_result::max_display_length(const Item *item) const
{
  return item->max_length;
}


uint32 Type_handler_string_result::max_display_length(const Item *item) const
{
  return item->max_length;
}


uint32 Type_handler_year::max_display_length(const Item *item) const
{
  return item->max_length;
}


uint32 Type_handler_bit::max_display_length(const Item *item) const
{
  return item->max_length;
}

/*************************************************************************/

int Type_handler_time_common::Item_save_in_field(Item *item, Field *field,
                                                 bool no_conversions) const
{
  return item->save_time_in_field(field, no_conversions);
}

int Type_handler_temporal_with_date::Item_save_in_field(Item *item,
                                                        Field *field,
                                                        bool no_conversions)
                                                        const
{
  return item->save_date_in_field(field, no_conversions);
}


int Type_handler_string_result::Item_save_in_field(Item *item, Field *field,
                                                   bool no_conversions) const
{
  return item->save_str_in_field(field, no_conversions);
}


int Type_handler_real_result::Item_save_in_field(Item *item, Field *field,
                                                 bool no_conversions) const
{
  return item->save_real_in_field(field, no_conversions);
}


int Type_handler_decimal_result::Item_save_in_field(Item *item, Field *field,
                                                    bool no_conversions) const
{
  return item->save_decimal_in_field(field, no_conversions);
}


int Type_handler_int_result::Item_save_in_field(Item *item, Field *field,
                                                bool no_conversions) const
{
  return item->save_int_in_field(field, no_conversions);
}


/***********************************************************************/

bool Type_handler_row::set_comparator_func(Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_row();
}

bool Type_handler_int_result::set_comparator_func(Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_int();
}

bool Type_handler_real_result::set_comparator_func(Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_real();
}

bool Type_handler_decimal_result::set_comparator_func(Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_decimal();
}

bool Type_handler_string_result::set_comparator_func(Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_string();
}

bool Type_handler_temporal_result::set_comparator_func(Arg_comparator *cmp) const
{
  return cmp->set_cmp_func_temporal();
}


/*************************************************************************/

bool Type_handler_temporal_result::
       can_change_cond_ref_to_const(Item_bool_func2 *target,
                                    Item *target_expr, Item *target_value,
                                    Item_bool_func2 *source,
                                    Item *source_expr, Item *source_const)
                                    const
{
  if (source->compare_type_handler()->cmp_type() != TIME_RESULT)
    return false;

  /*
    Can't rewrite:
      WHERE COALESCE(time_column)='00:00:00'
        AND COALESCE(time_column)=DATE'2015-09-11'
    to
      WHERE DATE'2015-09-11'='00:00:00'
        AND COALESCE(time_column)=DATE'2015-09-11'
    because the left part will erroneously try to parse '00:00:00'
    as DATE, not as TIME.

    TODO: It could still be rewritten to:
      WHERE DATE'2015-09-11'=TIME'00:00:00'
        AND COALESCE(time_column)=DATE'2015-09-11'
    i.e. we need to replace both target_expr and target_value
    at the same time. This is not supported yet.
  */
  return target_value->cmp_type() == TIME_RESULT;
}


bool Type_handler_string_result::
       can_change_cond_ref_to_const(Item_bool_func2 *target,
                                    Item *target_expr, Item *target_value,
                                    Item_bool_func2 *source,
                                    Item *source_expr, Item *source_const)
                                    const
{
  if (source->compare_type_handler()->cmp_type() != STRING_RESULT)
    return false;
  /*
    In this example:
      SET NAMES utf8 COLLATE utf8_german2_ci;
      DROP TABLE IF EXISTS t1;
      CREATE TABLE t1 (a CHAR(10) CHARACTER SET utf8);
      INSERT INTO t1 VALUES ('o-umlaut'),('oe');
      SELECT * FROM t1 WHERE a='oe' COLLATE utf8_german2_ci AND a='oe';

    the query should return only the row with 'oe'.
    It should not return 'o-umlaut', because 'o-umlaut' does not match
    the right part of the condition: a='oe'
    ('o-umlaut' is not equal to 'oe' in utf8_general_ci,
     which is the collation of the field "a").

    If we change the right part from:
       ... AND a='oe'
    to
       ... AND 'oe' COLLATE utf8_german2_ci='oe'
    it will be evalulated to TRUE and removed from the condition,
    so the overall query will be simplified to:

      SELECT * FROM t1 WHERE a='oe' COLLATE utf8_german2_ci;

    which will erroneously start to return both 'oe' and 'o-umlaut'.
    So changing "expr" to "const" is not possible if the effective
    collations of "target" and "source" are not exactly the same.

    Note, the code before the fix for MDEV-7152 only checked that
    collations of "source_const" and "target_value" are the same.
    This was not enough, as the bug report demonstrated.
  */
  return
    target->compare_collation() == source->compare_collation() &&
    target_value->collation.collation == source_const->collation.collation;
}


bool Type_handler_numeric::
       can_change_cond_ref_to_const(Item_bool_func2 *target,
                                    Item *target_expr, Item *target_value,
                                    Item_bool_func2 *source,
                                    Item *source_expr, Item *source_const)
                                    const
{
  /*
   The collations of "target" and "source" do not make sense for numeric
   data types.
  */
  return target->compare_type_handler() == source->compare_type_handler();
}


/*************************************************************************/

Item_cache *
Type_handler_row::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_row(thd);
}

Item_cache *
Type_handler_int_result::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_int(thd, item->field_type());
}

Item_cache *
Type_handler_real_result::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_real(thd);
}

Item_cache *
Type_handler_decimal_result::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_decimal(thd);
}

Item_cache *
Type_handler_string_result::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_str(thd, item);
}

Item_cache *
Type_handler_temporal_result::Item_get_cache(THD *thd, const Item *item) const
{
  return new (thd->mem_root) Item_cache_temporal(thd, item->field_type());
}

/*************************************************************************/

bool Type_handler_int_result::
       Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_int(items, nitems);
  return false;
}


bool Type_handler_real_result::
       Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_real(items, nitems);
  return false;
}


bool Type_handler_decimal_result::
       Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_decimal(items, nitems);
  return false;
}


bool Type_handler_string_result::
       Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
{
  return func->aggregate_attributes_string(items, nitems);
}


bool Type_handler_date_common::
       Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
{
  func->set_attributes_temporal(MAX_DATE_WIDTH, 0);
  return false;
}


bool Type_handler_time_common::
       Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_temporal(MIN_TIME_WIDTH, items, nitems);
  return false;
}


bool Type_handler_datetime_common::
       Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_temporal(MAX_DATETIME_WIDTH, items, nitems);
  return false;
}


bool Type_handler_timestamp_common::
       Item_hybrid_func_fix_attributes(THD *thd, Item_hybrid_func *func,
                                       Item **items, uint nitems) const
{
  func->aggregate_attributes_temporal(MAX_DATETIME_WIDTH, items, nitems);
  return false;
}


/*************************************************************************/

/**
  MAX/MIN for the traditional numeric types preserve the exact data type
  from Fields, but do not preserve the exact type from Items:
    MAX(float_field)              -> FLOAT
    MAX(smallint_field)           -> LONGLONG
    MAX(COALESCE(float_field))    -> DOUBLE
    MAX(COALESCE(smallint_field)) -> LONGLONG
  QQ: Items should probably be fixed to preserve the exact type.
*/
bool Type_handler_numeric::
       Item_sum_hybrid_fix_length_and_dec_numeric(Item_sum_hybrid *func,
                                                  const Type_handler *handler)
                                                  const
{
  Item *item= func->arguments()[0];
  Item *item2= item->real_item();
  func->Type_std_attributes::set(item);
  /* MIN/MAX can return NULL for empty set indepedent of the used column */
  func->maybe_null= func->null_value= true;
  if (item2->type() == Item::FIELD_ITEM)
    func->set_handler_by_field_type(item2->field_type());
  else
    func->set_handler(handler);
  return false;
}


bool Type_handler_int_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  return Item_sum_hybrid_fix_length_and_dec_numeric(func,
                                                    &type_handler_longlong);
}


bool Type_handler_real_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  (void) Item_sum_hybrid_fix_length_and_dec_numeric(func,
                                                    &type_handler_double);
  func->max_length= func->float_length(func->decimals);
  return false;
}


bool Type_handler_decimal_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  return Item_sum_hybrid_fix_length_and_dec_numeric(func,
                                                    &type_handler_newdecimal);
}


/**
   MAX(str_field) converts ENUM/SET to CHAR, and preserve all other types
   for Fields.
   QQ: This works differently from UNION, which preserve the exact data
   type for ENUM/SET if the joined ENUM/SET fields are equally defined.
   Perhaps should be fixed.
   MAX(str_item) chooses the best suitable string type.
*/
bool Type_handler_string_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  Item *item= func->arguments()[0];
  Item *item2= item->real_item();
  func->Type_std_attributes::set(item);
  func->maybe_null= func->null_value= true;
  if (item2->type() == Item::FIELD_ITEM)
  {
    // Fields: convert ENUM/SET to CHAR, preserve the type otherwise.
    func->set_handler_by_field_type(item->field_type());
  }
  else
  {
    // Items: choose VARCHAR/BLOB/MEDIUMBLOB/LONGBLOB, depending on length.
    func->set_handler(type_handler_varchar.
          type_handler_adjusted_to_max_octet_length(func->max_length,
                                                    func->collation.collation));
  }
  return false;
}


/**
  Traditional temporal types always preserve the type of the argument.
*/
bool Type_handler_temporal_result::
       Item_sum_hybrid_fix_length_and_dec(Item_sum_hybrid *func) const
{
  Item *item= func->arguments()[0];
  func->Type_std_attributes::set(item);
  func->maybe_null= func->null_value= true;
  func->set_handler(item->type_handler());
  return false;
}

/*************************************************************************/

String *
Type_handler_real_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                      String *str) const
{
  return item->val_str_ascii_from_val_real(str);
}


String *
Type_handler_decimal_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                         String *str) const
{
  return item->val_str_ascii_from_val_real(str);
}


String *
Type_handler_int_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                     String *str) const
{
  return item->val_str_ascii_from_val_int(str);
}


String *
Type_handler_temporal_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                          String *str) const
{
  return item->val_str_ascii_from_val_str(str);
}


String *
Type_handler_string_result::Item_func_hex_val_str_ascii(Item_func_hex *item,
                                                        String *str) const
{
  return item->val_str_ascii_from_val_str(str);
}

/***************************************************************************/

String *
Type_handler_decimal_result::Item_func_hybrid_field_type_val_str(
                                              Item_func_hybrid_field_type *item,
                                              String *str) const
{
  return item->val_str_from_decimal_op(str);
}


double
Type_handler_decimal_result::Item_func_hybrid_field_type_val_real(
                                              Item_func_hybrid_field_type *item)
                                              const
{
  return item->val_real_from_decimal_op();
}


longlong
Type_handler_decimal_result::Item_func_hybrid_field_type_val_int(
                                              Item_func_hybrid_field_type *item)
                                              const
{
  return item->val_int_from_decimal_op();
}


my_decimal *
Type_handler_decimal_result::Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *item,
                                              my_decimal *dec) const
{
  return item->val_decimal_from_decimal_op(dec);
}


bool
Type_handler_decimal_result::Item_func_hybrid_field_type_get_date(
                                             Item_func_hybrid_field_type *item,
                                             MYSQL_TIME *ltime,
                                             ulonglong fuzzydate) const
{
  return item->get_date_from_decimal_op(ltime, fuzzydate);
}


/***************************************************************************/


String *
Type_handler_int_result::Item_func_hybrid_field_type_val_str(
                                          Item_func_hybrid_field_type *item,
                                          String *str) const
{
  return item->val_str_from_int_op(str);
}


double
Type_handler_int_result::Item_func_hybrid_field_type_val_real(
                                          Item_func_hybrid_field_type *item)
                                          const
{
  return item->val_real_from_int_op();
}


longlong
Type_handler_int_result::Item_func_hybrid_field_type_val_int(
                                          Item_func_hybrid_field_type *item)
                                          const
{
  return item->val_int_from_int_op();
}


my_decimal *
Type_handler_int_result::Item_func_hybrid_field_type_val_decimal(
                                          Item_func_hybrid_field_type *item,
                                          my_decimal *dec) const
{
  return item->val_decimal_from_int_op(dec);
}


bool
Type_handler_int_result::Item_func_hybrid_field_type_get_date(
                                          Item_func_hybrid_field_type *item,
                                          MYSQL_TIME *ltime,
                                          ulonglong fuzzydate) const
{
  return item->get_date_from_int_op(ltime, fuzzydate);
}



/***************************************************************************/

String *
Type_handler_real_result::Item_func_hybrid_field_type_val_str(
                                           Item_func_hybrid_field_type *item,
                                           String *str) const
{
  return item->val_str_from_real_op(str);
}


double
Type_handler_real_result::Item_func_hybrid_field_type_val_real(
                                           Item_func_hybrid_field_type *item)
                                           const
{
  return item->val_real_from_real_op();
}


longlong
Type_handler_real_result::Item_func_hybrid_field_type_val_int(
                                           Item_func_hybrid_field_type *item)
                                           const
{
  return item->val_int_from_real_op();
}


my_decimal *
Type_handler_real_result::Item_func_hybrid_field_type_val_decimal(
                                           Item_func_hybrid_field_type *item,
                                           my_decimal *dec) const
{
  return item->val_decimal_from_real_op(dec);
}


bool
Type_handler_real_result::Item_func_hybrid_field_type_get_date(
                                             Item_func_hybrid_field_type *item,
                                             MYSQL_TIME *ltime,
                                             ulonglong fuzzydate) const
{
  return item->get_date_from_real_op(ltime, fuzzydate);
}


/***************************************************************************/

String *
Type_handler_temporal_result::Item_func_hybrid_field_type_val_str(
                                        Item_func_hybrid_field_type *item,
                                        String *str) const
{
  return item->val_str_from_date_op(str);
}


double
Type_handler_temporal_result::Item_func_hybrid_field_type_val_real(
                                        Item_func_hybrid_field_type *item)
                                        const
{
  return item->val_real_from_date_op();
}


longlong
Type_handler_temporal_result::Item_func_hybrid_field_type_val_int(
                                        Item_func_hybrid_field_type *item)
                                        const
{
  return item->val_int_from_date_op();
}


my_decimal *
Type_handler_temporal_result::Item_func_hybrid_field_type_val_decimal(
                                        Item_func_hybrid_field_type *item,
                                        my_decimal *dec) const
{
  return item->val_decimal_from_date_op(dec);
}


bool
Type_handler_temporal_result::Item_func_hybrid_field_type_get_date(
                                        Item_func_hybrid_field_type *item,
                                        MYSQL_TIME *ltime,
                                        ulonglong fuzzydate) const
{
  return item->get_date_from_date_op(ltime, fuzzydate);
}


/***************************************************************************/

String *
Type_handler_string_result::Item_func_hybrid_field_type_val_str(
                                             Item_func_hybrid_field_type *item,
                                             String *str) const
{
  return item->val_str_from_str_op(str);
}


double
Type_handler_string_result::Item_func_hybrid_field_type_val_real(
                                             Item_func_hybrid_field_type *item)
                                             const
{
  return item->val_real_from_str_op();
}


longlong
Type_handler_string_result::Item_func_hybrid_field_type_val_int(
                                             Item_func_hybrid_field_type *item)
                                             const
{
  return item->val_int_from_str_op();
}


my_decimal *
Type_handler_string_result::Item_func_hybrid_field_type_val_decimal(
                                              Item_func_hybrid_field_type *item,
                                              my_decimal *dec) const
{
  return item->val_decimal_from_str_op(dec);
}


bool
Type_handler_string_result::Item_func_hybrid_field_type_get_date(
                                             Item_func_hybrid_field_type *item,
                                             MYSQL_TIME *ltime,
                                             ulonglong fuzzydate) const
{
  return item->get_date_from_str_op(ltime, fuzzydate);
}

/***************************************************************************/

longlong Type_handler_row::
           Item_func_between_val_int(Item_func_between *func) const
{
  DBUG_ASSERT(0);
  func->null_value= true;
  return 0;
}

longlong Type_handler_string_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_string();
}

longlong Type_handler_temporal_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_temporal();
}

longlong Type_handler_int_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_int();
}

longlong Type_handler_real_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_real();
}

longlong Type_handler_decimal_result::
           Item_func_between_val_int(Item_func_between *func) const
{
  return func->val_int_cmp_decimal();
}

/***************************************************************************/

cmp_item *Type_handler_int_result::make_cmp_item(THD *thd,
                                                 CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_int;
}

cmp_item *Type_handler_real_result::make_cmp_item(THD *thd,
                                                 CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_real;
}

cmp_item *Type_handler_decimal_result::make_cmp_item(THD *thd,
                                                     CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_decimal;
}


cmp_item *Type_handler_string_result::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_sort_string(cs);
}

cmp_item *Type_handler_row::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_row;
}

cmp_item *Type_handler_time_common::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_time;
}

cmp_item *Type_handler_temporal_with_date::make_cmp_item(THD *thd,
                                                    CHARSET_INFO *cs) const
{
  return new (thd->mem_root) cmp_item_datetime;
}

/***************************************************************************/

static int srtcmp_in(CHARSET_INFO *cs, const String *x,const String *y)
{
  return cs->coll->strnncollsp(cs,
                               (uchar *) x->ptr(),x->length(),
                               (uchar *) y->ptr(),y->length());
}

in_vector *Type_handler_string_result::make_in_vector(THD *thd,
                                                      const Item_func_in *func,
                                                      uint nargs) const
{
  return new (thd->mem_root) in_string(thd, nargs, (qsort2_cmp) srtcmp_in,
                                       func->compare_collation());

}


in_vector *Type_handler_int_result::make_in_vector(THD *thd,
                                                   const Item_func_in *func,
                                                   uint nargs) const
{
  return new (thd->mem_root) in_longlong(thd, nargs);
}


in_vector *Type_handler_real_result::make_in_vector(THD *thd,
                                                    const Item_func_in *func,
                                                    uint nargs) const
{
  return new (thd->mem_root) in_double(thd, nargs);
}


in_vector *Type_handler_decimal_result::make_in_vector(THD *thd,
                                                       const Item_func_in *func,
                                                       uint nargs) const
{
  return new (thd->mem_root) in_decimal(thd, nargs);
}


in_vector *Type_handler_time_common::make_in_vector(THD *thd,
                                                    const Item_func_in *func,
                                                    uint nargs) const
{
  return new (thd->mem_root) in_time(thd, nargs);
}


in_vector *
Type_handler_temporal_with_date::make_in_vector(THD *thd,
                                                const Item_func_in *func,
                                                uint nargs) const
{
  return new (thd->mem_root) in_datetime(thd, nargs);
}


in_vector *Type_handler_row::make_in_vector(THD *thd,
                                            const Item_func_in *func,
                                            uint nargs) const
{
  return new (thd->mem_root) in_row(thd, nargs, 0);
}

/***************************************************************************/

bool Type_handler_string_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  if (func->agg_all_arg_charsets_for_comparison())
    return true;
  if (func->compatible_types_scalar_bisection_possible())
  {
    return func->value_list_convert_const_to_int(thd) ||
           func->fix_for_scalar_comparison_using_bisection(thd);
  }
  return
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) STRING_RESULT);
}


bool Type_handler_int_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  /*
     Does not need to call value_list_convert_const_to_int()
     as already handled by int handler.
  */
  return func->compatible_types_scalar_bisection_possible() ?
    func->fix_for_scalar_comparison_using_bisection(thd) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) INT_RESULT);
}


bool Type_handler_real_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  return func->compatible_types_scalar_bisection_possible() ?
    (func->value_list_convert_const_to_int(thd) ||
     func->fix_for_scalar_comparison_using_bisection(thd)) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) REAL_RESULT);
}


bool Type_handler_decimal_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  return func->compatible_types_scalar_bisection_possible() ?
    (func->value_list_convert_const_to_int(thd) ||
     func->fix_for_scalar_comparison_using_bisection(thd)) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) DECIMAL_RESULT);
}


bool Type_handler_temporal_result::
       Item_func_in_fix_comparator_compatible_types(THD *thd,
                                                    Item_func_in *func) const
{
  return func->compatible_types_scalar_bisection_possible() ?
    (func->value_list_convert_const_to_int(thd) ||
     func->fix_for_scalar_comparison_using_bisection(thd)) :
    func->fix_for_scalar_comparison_using_cmp_items(thd,
                                                    1U << (uint) TIME_RESULT);
}


bool Type_handler_row::Item_func_in_fix_comparator_compatible_types(THD *thd,
                                              Item_func_in *func) const
{
  return func->compatible_types_row_bisection_possible() ?
         func->fix_for_row_comparison_using_bisection(thd) :
         func->fix_for_row_comparison_using_cmp_items(thd);
}

/***************************************************************************/

String *Type_handler_string_result::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return func->val_str_native(str);
}


String *Type_handler_temporal_result::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return func->val_string_from_date(str);
}


String *Type_handler_int_result::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return func->val_string_from_int(str);
}


String *Type_handler_decimal_result::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return func->val_string_from_decimal(str);
}


String *Type_handler_real_result::
          Item_func_min_max_val_str(Item_func_min_max *func, String *str) const
{
  return func->val_string_from_real(str);
}


double Type_handler_string_result::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  return func->val_real_native();
}


double Type_handler_temporal_result::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  MYSQL_TIME ltime;
  if (func->get_date(&ltime, 0))
    return 0;
  return TIME_to_double(&ltime);
}


double Type_handler_numeric::
         Item_func_min_max_val_real(Item_func_min_max *func) const
{
  return func->val_real_native();
}


longlong Type_handler_string_result::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  return func->val_int_native();
}


longlong Type_handler_temporal_result::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  MYSQL_TIME ltime;
  if (func->get_date(&ltime, 0))
    return 0;
  return TIME_to_ulonglong(&ltime);
}


longlong Type_handler_numeric::
         Item_func_min_max_val_int(Item_func_min_max *func) const
{
  return func->val_int_native();
}


my_decimal *Type_handler_string_result::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  return func->val_decimal_native(dec);
}


my_decimal *Type_handler_numeric::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  return func->val_decimal_native(dec);
}


my_decimal *Type_handler_temporal_result::
            Item_func_min_max_val_decimal(Item_func_min_max *func,
                                          my_decimal *dec) const
{
  MYSQL_TIME ltime;
  if (func->get_date(&ltime, 0))
    return 0;
  return date2my_decimal(&ltime, dec);
}


bool Type_handler_string_result::
       Item_func_min_max_get_date(Item_func_min_max *func,
                                  MYSQL_TIME *ltime, ulonglong fuzzydate) const
{
  /*
    just like ::val_int() method of a string item can be called,
    for example, SELECT CONCAT("10", "12") + 1,
    ::get_date() can be called for non-temporal values,
    for example, SELECT MONTH(GREATEST("2011-11-21", "2010-10-09"))
  */
  return func->Item::get_date(ltime, fuzzydate);
}


bool Type_handler_numeric::
       Item_func_min_max_get_date(Item_func_min_max *func,
                                  MYSQL_TIME *ltime, ulonglong fuzzydate) const
{
  return func->Item::get_date(ltime, fuzzydate);
}


bool Type_handler_temporal_result::
       Item_func_min_max_get_date(Item_func_min_max *func,
                                  MYSQL_TIME *ltime, ulonglong fuzzydate) const
{
  return func->get_date_native(ltime, fuzzydate);
}

/***************************************************************************/

/**
  Get a string representation of the Item value.
  See sql_type.h for details.
*/
String *Type_handler_row::
          print_item_value(THD *thd, Item *item, String *str) const
{
  DBUG_ASSERT(0);
  return NULL;
}


/**
  Get a string representation of the Item value,
  using the character string format with its charset and collation, e.g.
    latin1 'string' COLLATE latin1_german2_ci
*/
String *Type_handler::
          print_item_value_csstr(THD *thd, Item *item, String *str) const
{
  String *result= item->val_str(str);

  if (!result)
    return NULL;

  StringBuffer<STRING_BUFFER_USUAL_SIZE> buf(result->charset());
  CHARSET_INFO *cs= thd->variables.character_set_client;

  buf.append('_');
  buf.append(result->charset()->csname);
  if (cs->escape_with_backslash_is_dangerous)
    buf.append(' ');
  append_query_string(cs, &buf, result->ptr(), result->length(),
                     thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES);
  buf.append(" COLLATE '");
  buf.append(item->collation.collation->name);
  buf.append('\'');
  str->copy(buf);

  return str;
}


String *Type_handler_numeric::
          print_item_value(THD *thd, Item *item, String *str) const
{
  return item->val_str(str);
}


String *Type_handler::
          print_item_value_temporal(THD *thd, Item *item, String *str,
                                    const Name &type_name, String *buf) const
{
  String *result= item->val_str(buf);
  return !result ||
         str->realloc(type_name.length() + result->length() + 2) ||
         str->copy(type_name.ptr(), type_name.length(), &my_charset_latin1) ||
         str->append('\'') ||
         str->append(result->ptr(), result->length()) ||
         str->append('\'') ?
         NULL :
         str;
}


String *Type_handler_time_common::
          print_item_value(THD *thd, Item *item, String *str) const
{
  StringBuffer<MAX_TIME_FULL_WIDTH+1> buf;
  return print_item_value_temporal(thd, item, str,
                                   Name(C_STRING_WITH_LEN("TIME")), &buf);
}


String *Type_handler_date_common::
          print_item_value(THD *thd, Item *item, String *str) const
{
  StringBuffer<MAX_DATE_WIDTH+1> buf;
  return print_item_value_temporal(thd, item, str,
                                   Name(C_STRING_WITH_LEN("DATE")), &buf);
}


String *Type_handler_datetime_common::
          print_item_value(THD *thd, Item *item, String *str) const
{
  StringBuffer<MAX_DATETIME_FULL_WIDTH+1> buf;
  return print_item_value_temporal(thd, item, str,
                                   Name(C_STRING_WITH_LEN("TIMESTAMP")), &buf);
}


String *Type_handler_timestamp_common::
          print_item_value(THD *thd, Item *item, String *str) const
{
  StringBuffer<MAX_DATETIME_FULL_WIDTH+1> buf;
  return print_item_value_temporal(thd, item, str,
                                   Name(C_STRING_WITH_LEN("TIMESTAMP")), &buf);
}


/***************************************************************************/

bool Type_handler_row::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_double();
  return false;
}


bool Type_handler_string_result::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  item->fix_arg_double();
  return false;
}


#ifdef HAVE_SPATIAL
bool Type_handler_geometry::
       Item_func_round_fix_length_and_dec(Item_func_round *item) const
{
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           type_handler_geometry.name().ptr(), item->func_name());
  return false;
}
#endif

/***************************************************************************/

bool Type_handler_row::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_int_or_decimal();
  return false;
}


bool Type_handler_real_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_int_or_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_int_or_decimal();
  return false;
}


bool Type_handler_string_result::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


#ifdef HAVE_SPATIAL
bool Type_handler_geometry::
       Item_func_int_val_fix_length_and_dec(Item_func_int_val *item) const
{
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           type_handler_geometry.name().ptr(), item->func_name());
  return true;
}
#endif

/***************************************************************************/

bool Type_handler_row::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_string_result::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


#ifdef HAVE_SPATIAL
bool Type_handler_geometry::
       Item_func_abs_fix_length_and_dec(Item_func_abs *item) const
{
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           type_handler_geometry.name().ptr(), item->func_name());
  return true;
}
#endif

/***************************************************************************/

bool Type_handler_row::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  DBUG_ASSERT(0);
  return false;
}


bool Type_handler_int_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_int();
  return false;
}


bool Type_handler_real_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


bool Type_handler_decimal_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_temporal_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_decimal();
  return false;
}


bool Type_handler_string_result::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  item->fix_length_and_dec_double();
  return false;
}


#ifdef HAVE_SPATIAL
bool Type_handler_geometry::
       Item_func_neg_fix_length_and_dec(Item_func_neg *item) const
{
  my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
           type_handler_geometry.name().ptr(), item->func_name());
  return true;
}
#endif

/***************************************************************************/
