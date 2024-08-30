/* Copyright (c) 2006, 2013, Oracle and/or its affiliates.
   Copyright (c) 2011, 2013, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include "mariadb.h"
#include <my_bit.h>
#include "rpl_utility.h"
#include "log_event.h"

#if defined(MYSQL_CLIENT)
#error MYSQL_CLIENT must not be defined here
#endif

#if !defined(MYSQL_SERVER)
#error MYSQL_SERVER must be defined here
#endif

#if defined(HAVE_REPLICATION)
#include "rpl_rli.h"
#include "sql_select.h"
#endif


/**
   Compute the maximum display length of a field.

   @param sql_type Type of the field
   @param metadata The metadata from the master for the field.
   @return Maximum length of the field in bytes.

   The precise values calculated by field->max_display_length() and
   calculated by max_display_length_for_field() can differ (by +1 or -1)
   for integer data types (TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT).
   This slight difference is not important here, because we call
   this function only for two *different* integer data types.
 */
static uint32
max_display_length_for_field(const Conv_source &source)
{
  DBUG_PRINT("debug", ("sql_type: %s, metadata: 0x%x",
                       source.type_handler()->name().ptr(), source.metadata()));
  return source.type_handler()->max_display_length_for_field(source);
}


/*
  Compare the pack lengths of a source field (on the master) and a
  target field (on the slave).

  @param sh             Source type handler
  @param source_length  Source length
  @param th             Target type hander
  @param target_length  Target length

  @retval CONV_TYPE_SUBSET_TO_SUPERSET The length of the source field is
                                       smaller than the target field.
  @retval CONV_TYPE_PRECISE            The length of the source and
                                       the target fields are equal.
  @retval CONV_TYPE_SUPERSET_TO_SUBSET The length of the source field is
                                       greater than the target field.
 */
static enum_conv_type
compare_lengths(const Type_handler *sh, uint32 source_length,
                const Type_handler *th, uint32 target_length)
{
  DBUG_ENTER("compare_lengths");
  DBUG_PRINT("debug", ("source_length: %lu, source_type: %s,"
                       " target_length: %lu, target_type: %s",
                       (unsigned long) source_length, sh->name().ptr(),
                       (unsigned long) target_length, th->name().ptr()));
  enum_conv_type result=
    source_length < target_length ? CONV_TYPE_SUBSET_TO_SUPERSET :
    source_length > target_length ? CONV_TYPE_SUPERSET_TO_SUBSET :
                                    CONV_TYPE_PRECISE;
  DBUG_PRINT("result", ("%d", result));
  DBUG_RETURN(result);
}


/**
  Calculate display length for MySQL56 temporal data types from their metadata.
  It contains fractional precision in the low 16-bit word.
*/
static uint32
max_display_length_for_temporal2_field(uint32 int_display_length,
                                       unsigned int metadata)
{
  metadata&= 0x00ff;
  return int_display_length + metadata + (metadata ? 1 : 0);
}


uint32
Type_handler_newdecimal::max_display_length_for_field(const Conv_source &src)
                                                      const
{
  return src.metadata() >> 8;
}


uint32
Type_handler_typelib::max_display_length_for_field(const Conv_source &src)
                                                   const
{
  /*
    Field_enum::rpl_conv_type_from() does not use  compare_lengths().
    So we should not come here.
  */
  DBUG_ASSERT(0);
  return src.metadata() & 0x00ff;
}


uint32
Type_handler_string::max_display_length_for_field(const Conv_source &src)
                                                  const
{
  /*
    ENUM and SET are transferred using as STRING,
    with the exact type code in metadata.
    Make sure that we previously detected ENUM/SET and
    translated them into a proper type handler.
    See table_def::field_type_handler() for details.
  */
  DBUG_ASSERT((src.metadata() >> 8) != MYSQL_TYPE_SET);
  DBUG_ASSERT((src.metadata() >> 8) != MYSQL_TYPE_ENUM);
  /* This is taken from Field_string::unpack. */
  return (((src.metadata() >> 4) & 0x300) ^ 0x300) + (src.metadata() & 0x00ff);
}


uint32
Type_handler_time2::max_display_length_for_field(const Conv_source &src)
                                                 const
{
  return max_display_length_for_temporal2_field(MIN_TIME_WIDTH,
                                                src.metadata());
}


uint32
Type_handler_timestamp2::max_display_length_for_field(const Conv_source &src)
                                                      const
{
  return max_display_length_for_temporal2_field(MAX_DATETIME_WIDTH,
                                                src.metadata());
}


uint32
Type_handler_datetime2::max_display_length_for_field(const Conv_source &src)
                                                     const
{
  return max_display_length_for_temporal2_field(MAX_DATETIME_WIDTH,
                                                src.metadata());
}


uint32
Type_handler_bit::max_display_length_for_field(const Conv_source &src)
                                               const
{
  /*
    Decode the size of the bit field from the master.
  */
  DBUG_ASSERT((src.metadata() & 0xff) <= 7);
  return 8 * (src.metadata() >> 8U) + (src.metadata() & 0x00ff);
}


uint32
Type_handler_var_string::max_display_length_for_field(const Conv_source &src)
                                                      const
{
  return src.metadata();
}


uint32
Type_handler_varchar::max_display_length_for_field(const Conv_source &src)
                                                   const
{
  return src.metadata();
}


uint32
Type_handler_varchar_compressed::
  max_display_length_for_field(const Conv_source &src) const
{
  DBUG_ASSERT(src.metadata() > 0);
  return src.metadata() - 1;
}


/*
  The actual length for these types does not really matter since
  they are used to calc_pack_length, which ignores the given
  length for these types.

  Since we want this to be accurate for other uses, we return the
  maximum size in bytes of these BLOBs.
*/
uint32
Type_handler_tiny_blob::max_display_length_for_field(const Conv_source &src)
                                                     const
{
  return (uint32) my_set_bits(1 * 8);
}


uint32
Type_handler_medium_blob::max_display_length_for_field(const Conv_source &src)
                                                       const
{
  return (uint32) my_set_bits(3 * 8);
}


uint32
Type_handler_blob::max_display_length_for_field(const Conv_source &src)
                                                const
{
  /*
    For the blob type, Field::real_type() lies and say that all
    blobs are of type MYSQL_TYPE_BLOB. In that case, we have to look
    at the length instead to decide what the max display size is.
   */
  return (uint32) my_set_bits(src.metadata() * 8);
}


uint32
Type_handler_blob_compressed::max_display_length_for_field(const Conv_source &src)
                                                    const
{
  return (uint32) my_set_bits(src.metadata() * 8);
}


uint32
Type_handler_long_blob::max_display_length_for_field(const Conv_source &src)
                                                     const
{
  return (uint32) my_set_bits(4 * 8);
}


uint32
Type_handler_olddecimal::max_display_length_for_field(const Conv_source &src)
                                                      const
{
  return ~(uint32) 0;
}


void Type_handler::show_binlog_type(const Conv_source &src, const Field &,
                                    String *str) const
{
  str->set_ascii(name().ptr(), name().length());
}


void Type_handler_var_string::show_binlog_type(const Conv_source &src,
                                               const Field &dst,
                                               String *str) const
{
  CHARSET_INFO *cs= str->charset();
  const char* fmt= dst.cmp_type() != STRING_RESULT || dst.has_charset()
    ? "char(%u octets)" : "binary(%u)";
  size_t length= cs->cset->snprintf(cs, (char*) str->ptr(),
                                    str->alloced_length(),
                                    fmt, src.metadata());
  str->length(length);
}


void Type_handler_varchar::show_binlog_type(const Conv_source &src,
                                            const Field &dst,
                                            String *str) const
{
  CHARSET_INFO *cs= str->charset();
  const char* fmt= dst.cmp_type() != STRING_RESULT || dst.has_charset()
    ? "varchar(%u octets)" : "varbinary(%u)";
  size_t length= cs->cset->snprintf(cs, (char*) str->ptr(),
                                    str->alloced_length(),
                                    fmt, src.metadata());
  str->length(length);
}


void Type_handler_varchar_compressed::show_binlog_type(const Conv_source &src,
                                                       const Field &dst,
                                                       String *str) const
{
  CHARSET_INFO *cs= str->charset();
  const char* fmt= dst.cmp_type() != STRING_RESULT || dst.has_charset()
    ? "varchar(%u octets) compressed" : "varbinary(%u) compressed";
  size_t length= cs->cset->snprintf(cs, (char*) str->ptr(),
                                    str->alloced_length(),
                                    fmt, src.metadata());
  str->length(length);
}

void Type_handler_bit::show_binlog_type(const Conv_source &src, const Field &,
                                        String *str) const
{
  CHARSET_INFO *cs= str->charset();
  int bit_length= 8 * (src.metadata() >> 8) + (src.metadata() & 0xFF);
  size_t length=
    cs->cset->snprintf(cs, (char*) str->ptr(), str->alloced_length(),
                       "bit(%d)", bit_length);
  str->length(length);
}


void Type_handler_olddecimal::show_binlog_type(const Conv_source &src,
                                               const Field &,
                                               String *str) const
{
  CHARSET_INFO *cs= str->charset();
  size_t length=
    cs->cset->snprintf(cs, (char*) str->ptr(), str->alloced_length(),
                       "decimal(%d,?)/*old*/", src.metadata());
  str->length(length);

}


void Type_handler_newdecimal::show_binlog_type(const Conv_source &src,
                                               const Field &,
                                               String *str) const
{
  CHARSET_INFO *cs= str->charset();
  size_t length=
    cs->cset->snprintf(cs, (char*) str->ptr(), str->alloced_length(),
                       "decimal(%d,%d)",
                       src.metadata() >> 8, src.metadata() & 0xff);
  str->length(length);
}


void Type_handler_blob_compressed::show_binlog_type(const Conv_source &src,
                                                    const Field &,
                                                    String *str) const
{
  /*
    Field::real_type() lies regarding the actual type of a BLOB, so
    it is necessary to check the pack length to figure out what kind
    of blob it really is.
   */
  switch (src.metadata()) {
    case 1:
      str->set_ascii(STRING_WITH_LEN("tinyblob compressed"));
      break;
    case 2:
      str->set_ascii(STRING_WITH_LEN("blob compressed"));
      break;
    case 3:
      str->set_ascii(STRING_WITH_LEN("mediumblob compressed"));
      break;
    default:
      DBUG_ASSERT(0);
      // Fall through
    case 4:
      str->set_ascii(STRING_WITH_LEN("longblob compressed"));
  }
}


void Type_handler_string::show_binlog_type(const Conv_source &src,
                                           const Field &dst,
                                           String *str) const
{
  /*
    This is taken from Field_string::unpack.
  */
  CHARSET_INFO *cs= str->charset();
  uint bytes= (((src.metadata() >> 4) & 0x300) ^ 0x300) +
              (src.metadata() & 0x00ff);
  const char* fmt= dst.cmp_type() != STRING_RESULT || dst.has_charset()
    ? "char(%u octets)" : "binary(%u)";
  size_t length= cs->cset->snprintf(cs, (char*) str->ptr(),
                                    str->alloced_length(),
                                    fmt, bytes);
  str->length(length);
}


enum_conv_type
Field::rpl_conv_type_from_same_data_type(uint16 metadata,
                                         const Relay_log_info *rli,
                                         const Conv_param &param) const
{
  if (metadata == 0) // Metadata can only be zero if no metadata was provided
  {
    /*
      If there is no metadata, we either have an old event where no
      metadata were supplied, or a type that does not require any
      metadata. In either case, conversion can be done but no
      conversion table is necessary.
     */
    DBUG_PRINT("debug", ("Base types are identical, but there is no metadata"));
    return CONV_TYPE_PRECISE;
  }

  DBUG_PRINT("debug", ("Base types are identical, doing field size comparison"));
  int order= 0;
  if (!compatible_field_size(metadata, rli, param.table_def_flags(), &order))
    return CONV_TYPE_IMPOSSIBLE;
  return order == 0 ? CONV_TYPE_PRECISE :
         order < 0  ? CONV_TYPE_SUBSET_TO_SUPERSET :
                      CONV_TYPE_SUPERSET_TO_SUBSET;
}


enum_conv_type
Field_new_decimal::rpl_conv_type_from(const Conv_source &source,
                                      const Relay_log_info *rli,
                                      const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  if (source.type_handler() == &type_handler_olddecimal ||
      source.type_handler() == &type_handler_newdecimal ||
      source.type_handler() == &type_handler_float ||
      source.type_handler() == &type_handler_double)
  {
    /*
      Then the other type is either FLOAT, DOUBLE, or old style
      DECIMAL, so we require lossy conversion.
    */
    return CONV_TYPE_SUPERSET_TO_SUBSET;
  }
  return CONV_TYPE_IMPOSSIBLE;
}


/*
  This covers FLOAT, DOUBLE and old DECIMAL
*/
enum_conv_type
Field_real::rpl_conv_type_from(const Conv_source &source,
                               const Relay_log_info *rli,
                               const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  if (source.type_handler() == &type_handler_olddecimal ||
      source.type_handler() == &type_handler_newdecimal)
    return CONV_TYPE_SUPERSET_TO_SUBSET;  // Always require lossy conversions
  if (source.type_handler() == &type_handler_float ||
      source.type_handler() == &type_handler_double)
  {
    enum_conv_type order= compare_lengths(source.type_handler(),
                                          max_display_length_for_field(source),
                                          type_handler(), max_display_length());
    DBUG_ASSERT(order != CONV_TYPE_PRECISE);
    return order;
  }
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_int::rpl_conv_type_from(const Conv_source &source,
                              const Relay_log_info *rli,
                              const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  /*
    The length comparison check will do the correct job of comparing
    the field lengths (in bytes) of two integer types.
  */
  if (source.type_handler() == &type_handler_stiny  ||
      source.type_handler() == &type_handler_sshort ||
      source.type_handler() == &type_handler_sint24 ||
      source.type_handler() == &type_handler_slong  ||
      source.type_handler() == &type_handler_slonglong)
  {
    /*
      max_display_length_for_field() is not fully precise for the integer
      data types. So its result cannot be compared to the result of
      max_dispay_length() when the table field and the binlog field
      are of the same type.
      This code should eventually be rewritten not to use
      compare_lengths(), to detect subtype/supetype relations
      just using the type codes.
    */
    DBUG_ASSERT(source.real_field_type() != real_type());
    enum_conv_type order= compare_lengths(source.type_handler(),
                                          max_display_length_for_field(source),
                                          type_handler(), max_display_length());
    DBUG_ASSERT(order != CONV_TYPE_PRECISE);
    return order;
  }
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_enum::rpl_conv_type_from(const Conv_source &source,
                               const Relay_log_info *rli,
                               const Conv_param &param) const
{
  /*
    For some reasons Field_enum and Field_set store MYSQL_TYPE_STRING
    as a type code in the binary log and encode the real type in metadata.
    So we need to test real_type() here instread of binlog_type().
  */
  return real_type() == source.real_field_type() ?
         rpl_conv_type_from_same_data_type(source.metadata(), rli, param) :
         CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_longstr::rpl_conv_type_from(const Conv_source &source,
                                  const Relay_log_info *rli,
                                  const Conv_param &param) const
{
  /**
    @todo
      Implement Field_varstring_compressed::real_type() and
      Field_blob_compressed::real_type() properly. All occurencies
      of Field::real_type() have to be inspected and adjusted if needed.

      Until it is not ready we have to compare source_type against
      binlog_type() when replicating from or to compressed data types.

      @sa Comment for Field::binlog_type()
  */
  bool same_type;
  if (source.real_field_type() == MYSQL_TYPE_VARCHAR_COMPRESSED ||
      source.real_field_type() == MYSQL_TYPE_BLOB_COMPRESSED ||
      binlog_type() == MYSQL_TYPE_VARCHAR_COMPRESSED ||
      binlog_type() == MYSQL_TYPE_BLOB_COMPRESSED)
    same_type= binlog_type() == source.real_field_type();
  else if (Type_handler_json_common::is_json_type_handler(type_handler()))
    same_type= type_handler()->type_handler_base() == source.type_handler();
  else
    same_type= type_handler() == source.type_handler();

  if (same_type)
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);

  if (source.type_handler() == &type_handler_tiny_blob ||
      source.type_handler() == &type_handler_medium_blob ||
      source.type_handler() == &type_handler_long_blob ||
      source.type_handler() == &type_handler_blob ||
      source.type_handler() == &type_handler_blob_compressed ||
      source.type_handler() == &type_handler_string ||
      source.type_handler() == &type_handler_var_string ||
      source.type_handler() == &type_handler_varchar ||
      source.type_handler() == &type_handler_varchar_compressed)
  {
    enum_conv_type order= compare_lengths(source.type_handler(),
                                          max_display_length_for_field(source),
                                          type_handler(), max_display_length());
    /*
      Here we know that the types are different, so if the order
      gives that they do not require any conversion, we still need
      to have non-lossy conversion enabled to allow conversion
      between different (string) types of the same length.

      Also, if all conversions are disabled, it is not allowed to convert
      between these types. Since the TEXT vs. BINARY is distinguished by
      the charset, and the charset is not replicated, we cannot
      currently distinguish between , e.g., TEXT and BLOB.
     */
    if (order == CONV_TYPE_PRECISE)
      order= CONV_TYPE_SUBSET_TO_SUPERSET;
    return order;
  }
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_newdate::rpl_conv_type_from(const Conv_source &source,
                                  const Relay_log_info *rli,
                                  const Conv_param &param) const
{
  if (real_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  if (source.type_handler() == &type_handler_datetime2)
    return CONV_TYPE_SUPERSET_TO_SUBSET;
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_time::rpl_conv_type_from(const Conv_source &source,
                               const Relay_log_info *rli,
                               const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  // 'MySQL56 TIME(N)' -> 'MariaDB-5.3 TIME(N)' is non-lossy
  if (decimals() == source.metadata() &&
       source.type_handler() == &type_handler_time2)
    return CONV_TYPE_VARIANT; // TODO: conversion from FSP1>FSP2
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_timef::rpl_conv_type_from(const Conv_source &source,
                                const Relay_log_info *rli,
                                const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  /*
    See comment in Field_datetimef::rpl_conv_type_from()
    'MariaDB-5.3 TIME(0)' to 'MySQL56 TIME(0)' is non-lossy
  */
  if (source.metadata() == 0 && source.type_handler() == &type_handler_time)
    return CONV_TYPE_VARIANT;
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_timestamp::rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  // 'MySQL56 TIMESTAMP(N)' -> MariaDB-5.3 TIMESTAMP(N)' is non-lossy
  if (source.metadata() == decimals() &&
      source.type_handler() == &type_handler_timestamp2)
    return CONV_TYPE_VARIANT; // TODO: conversion from FSP1>FSP2
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_timestampf::rpl_conv_type_from(const Conv_source &source,
                                     const Relay_log_info *rli,
                                     const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  /*
    See comment in Field_datetimef::rpl_conv_type_from()
    'MariaDB-5.3 TIMESTAMP(0)' to 'MySQL56 TIMESTAMP(0)' is non-lossy
  */
  if (source.metadata() == 0 &&
      source.type_handler() == &type_handler_timestamp)
    return CONV_TYPE_VARIANT;
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_datetime::rpl_conv_type_from(const Conv_source &source,
                                   const Relay_log_info *rli,
                                   const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  // 'MySQL56 DATETIME(N)' -> MariaDB-5.3 DATETIME(N) is non-lossy
  if (source.metadata() == decimals() &&
      source.type_handler() == &type_handler_datetime2)
    return CONV_TYPE_VARIANT; // TODO: conversion from FSP1>FSP2
  if (source.type_handler() == &type_handler_newdate)
    return CONV_TYPE_SUBSET_TO_SUPERSET;
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_datetimef::rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const
{
  if (binlog_type() == source.real_field_type())
    return rpl_conv_type_from_same_data_type(source.metadata(), rli, param);
  /*
    'MariaDB-5.3 DATETIME(N)' does not provide information about fractional
    precision in metadata. So we assume the precision on the master is equal
    to the precision on the slave.
    TODO: See MDEV-17394 what happend in case precisions are in case different
    'MariaDB-5.3 DATETIME(0)' to 'MySQL56 DATETIME(0)' is non-lossy
  */
  if (source.metadata() == 0 &&
      source.type_handler() == &type_handler_datetime)
    return CONV_TYPE_VARIANT;
  if (source.type_handler() == &type_handler_newdate)
    return CONV_TYPE_SUBSET_TO_SUPERSET;
  return CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_date::rpl_conv_type_from(const Conv_source &source,
                               const Relay_log_info *rli,
                               const Conv_param &param) const
{
  // old DATE
  return binlog_type() == source.real_field_type() ?
         rpl_conv_type_from_same_data_type(source.metadata(), rli, param) :
         CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_bit::rpl_conv_type_from(const Conv_source &source,
                              const Relay_log_info *rli,
                              const Conv_param &param) const
{
  return binlog_type() == source.real_field_type() ?
         rpl_conv_type_from_same_data_type(source.metadata(), rli, param) :
         CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_year::rpl_conv_type_from(const Conv_source &source,
                               const Relay_log_info *rli,
                               const Conv_param &param) const
{
  return binlog_type() == source.real_field_type() ?
         rpl_conv_type_from_same_data_type(source.metadata(), rli, param) :
         CONV_TYPE_IMPOSSIBLE;
}


enum_conv_type
Field_null::rpl_conv_type_from(const Conv_source &source,
                               const Relay_log_info *rli,
                               const Conv_param &param) const
{
  DBUG_ASSERT(0);
  return CONV_TYPE_IMPOSSIBLE;
}


/**********************************************************************/


#if defined(HAVE_REPLICATION)

/**
 */
static void show_sql_type(const Conv_source &src, const Field &dst,
                          String *str)
{
  DBUG_ENTER("show_sql_type");
  DBUG_ASSERT(src.type_handler() != NULL);
  DBUG_PRINT("enter", ("type: %s, metadata: 0x%x",
                       src.type_handler()->name().ptr(), src.metadata()));
  src.type_handler()->show_binlog_type(src, dst, str);
  DBUG_VOID_RETURN;
}


/**
   Check the order variable and print errors if the order is not
   acceptable according to the current settings.

   @param order  The computed order of the conversion needed.
   @param rli    The relay log info data structure: for error reporting.
 */
static bool is_conversion_ok(enum_conv_type type, const Relay_log_info *rli,
                             ulonglong type_conversion_options)
{
  DBUG_ENTER("is_conversion_ok");
  bool allow_non_lossy, allow_lossy;

  allow_non_lossy= type_conversion_options &
                    (1ULL << SLAVE_TYPE_CONVERSIONS_ALL_NON_LOSSY);
  allow_lossy= type_conversion_options &
               (1ULL << SLAVE_TYPE_CONVERSIONS_ALL_LOSSY);

  DBUG_PRINT("enter", ("order: %d, flags:%s%s", (int) type,
                       allow_non_lossy ? " ALL_NON_LOSSY" : "",
                       allow_lossy ? " ALL_LOSSY" : ""));
  switch (type) {
  case CONV_TYPE_PRECISE:
  case CONV_TYPE_VARIANT:
    DBUG_RETURN(true);
  case CONV_TYPE_SUBSET_TO_SUPERSET:
    /* !!! Add error message saying that non-lossy conversions need to be allowed. */
    DBUG_RETURN(allow_non_lossy);
  case CONV_TYPE_SUPERSET_TO_SUBSET:
    /* !!! Add error message saying that lossy conversions need to be allowed. */
    DBUG_RETURN(allow_lossy);
  case CONV_TYPE_IMPOSSIBLE:
    DBUG_RETURN(false);
  }

  DBUG_RETURN(false);
}


/**
   Can a type potentially be converted to another type?

   This function check if the types are convertible and what
   conversion is required.

   If conversion is not possible, and error is printed.

   If conversion is possible:

   - *order will be set to -1 if source type is smaller than target
     type and a non-lossy conversion can be required. This includes
     the case where the field types are different but types could
     actually be converted in either direction.

   - *order will be set to 0 if no conversion is required.

   - *order will be set to 1 if the source type is strictly larger
      than the target type and that conversion is potentially lossy.

   @param[in] field    Target field
   @param[in] type     Source field type
   @param[in] metadata Source field metadata
   @param[in] rli      Relay log info (for error reporting)
   @param[in] mflags   Flags from the table map event
   @param[out] order   Order between source field and target field

   @return @c true if conversion is possible according to the current
   settings, @c false if conversion is not possible according to the
   current setting.
 */
static enum_conv_type
can_convert_field_to(Field *field, const Conv_source &source,
                     const Relay_log_info *rli,
                     const Conv_param &param)
{
  DBUG_ENTER("can_convert_field_to");
#ifndef DBUG_OFF
  char field_type_buf[MAX_FIELD_WIDTH];
  String field_type(field_type_buf, sizeof(field_type_buf), &my_charset_latin1);
  field->sql_type(field_type);
  DBUG_PRINT("enter", ("field_type: %s, target_type: %d, source_type: %d, source_metadata: 0x%x",
                       field_type.c_ptr_safe(), field->real_type(),
                       source.real_field_type(), source.metadata()));
#endif
  DBUG_RETURN(field->rpl_conv_type_from(source, rli, param));
}


const Type_handler *table_def::field_type_handler(uint col) const
{
  enum_field_types typecode= binlog_type(col);
  uint16 metadata= field_metadata(col);
  DBUG_ASSERT(typecode != MYSQL_TYPE_ENUM);
  DBUG_ASSERT(typecode != MYSQL_TYPE_SET);

  if (typecode == MYSQL_TYPE_BLOB)
  {
    switch (metadata & 0xff) {
    case 1: return &type_handler_tiny_blob;
    case 2: return &type_handler_blob;
    case 3: return &type_handler_medium_blob;
    case 4: return &type_handler_long_blob;
    default: return NULL;
    }
  }
  if (typecode == MYSQL_TYPE_STRING)
  {
    uchar typecode2= metadata >> 8;
    if (typecode2 == MYSQL_TYPE_SET)
      return &type_handler_set;
    if (typecode2 == MYSQL_TYPE_ENUM)
      return &type_handler_enum;
    return &type_handler_string;
  }
  /*
    This type has not been used since before row-based replication,
    so we can safely assume that it really is MYSQL_TYPE_NEWDATE.
  */
  if (typecode == MYSQL_TYPE_DATE)
    return &type_handler_newdate;
  return Type_handler::get_handler_by_real_type(typecode);
}


/**
  Is the definition compatible with a table?

  This function will compare the master table with an existing table
  on the slave and see if they are compatible with respect to the
  current settings of @c SLAVE_TYPE_CONVERSIONS.

  If the tables are compatible and conversions are required, @c
  *tmp_table_var will be set to a virtual temporary table with field
  pointers for the fields that require conversions.  This allow simple
  checking of whether a conversion are to be applied or not.

  If tables are compatible, but no conversions are necessary, @c
  *tmp_table_var will be set to NULL.

  @param rli_arg[in]
  Relay log info, for error reporting.

  @param table[in]
  Table to compare with

  @param tmp_table_var[out]
  Virtual temporary table for performing conversions, if necessary.

  @retval true Master table is compatible with slave table.
  @retval false Master table is not compatible with slave table.
*/
bool
table_def::compatible_with(THD *thd, rpl_group_info *rgi,
                           TABLE *table, TABLE **conv_table_var)
  const
{
  /*
    We only check the initial columns for the tables.
  */
  uint const cols_to_check= MY_MIN(table->s->fields, size());
  Relay_log_info *rli= rgi->rli;
  TABLE *tmp_table= NULL;

  for (uint col= 0 ; col < cols_to_check ; ++col)
  {
    Field *const field= table->field[col];
    const Type_handler *h= field_type_handler(col);
    if (!h)
    {
      sql_print_error("In RBR mode, Slave received unknown field type field %d "
                      " for column Name: %s.%s.%s.",
                      binlog_type(col),
                      field->table->s->db.str,
                      field->table->s->table_name.str,
                      field->field_name.str);
      return false;
    }

    if (!h)
      return false; // An unknown data type found in the binary log
    Conv_source source(h, field_metadata(col), field->charset());
    enum_conv_type convtype= can_convert_field_to(field, source, rli,
                                                  Conv_param(m_flags));
    if (is_conversion_ok(convtype, rli, slave_type_conversions_options))
    {
      DBUG_PRINT("debug", ("Checking column %d -"
                           " field '%s' can be converted - order: %d",
                           col, field->field_name.str, convtype));
      /*
        If conversion type is not CONV_TYPE_RECISE, a conversion is required,
        so we need to set up the conversion table.
       */
      if (convtype != CONV_TYPE_PRECISE && tmp_table == NULL)
      {
        /*
          This will create the full table with all fields. This is
          necessary to ge the correct field lengths for the record.
        */
        tmp_table= create_conversion_table(thd, rgi, table);
        if (tmp_table == NULL)
            return false;
        /*
          Clear all fields up to, but not including, this column.
        */
        for (unsigned int i= 0; i < col; ++i)
          tmp_table->field[i]= NULL;
      }

      if (convtype == CONV_TYPE_PRECISE && tmp_table != NULL)
        tmp_table->field[col]= NULL;
    }
    else
    {
      DBUG_PRINT("debug", ("Checking column %d -"
                           " field '%s' can not be converted",
                           col, field->field_name.str));
      DBUG_ASSERT(col < size() && col < table->s->fields);
      DBUG_ASSERT(table->s->db.str && table->s->table_name.str);
      DBUG_ASSERT(table->in_use);
      const char *db_name= table->s->db.str;
      const char *tbl_name= table->s->table_name.str;
      StringBuffer<MAX_FIELD_WIDTH> source_type(&my_charset_latin1);
      StringBuffer<MAX_FIELD_WIDTH> target_type(&my_charset_latin1);
      THD *thd= table->in_use;

      show_sql_type(source, *field, &source_type);
      field->sql_rpl_type(&target_type);
      DBUG_ASSERT(source_type.length() > 0);
      DBUG_ASSERT(target_type.length() > 0);
      rli->report(ERROR_LEVEL, ER_SLAVE_CONVERSION_FAILED, rgi->gtid_info(),
                  ER_THD(thd, ER_SLAVE_CONVERSION_FAILED),
                  col, db_name, tbl_name,
                  source_type.c_ptr_safe(), target_type.c_ptr_safe());
      return false;
    }
  }

#ifndef DBUG_OFF
  if (tmp_table)
  {
    for (unsigned int col= 0; col < tmp_table->s->fields; ++col)
      if (tmp_table->field[col])
      {
        char source_buf[MAX_FIELD_WIDTH];
        char target_buf[MAX_FIELD_WIDTH];
        String source_type(source_buf, sizeof(source_buf), &my_charset_latin1);
        String target_type(target_buf, sizeof(target_buf), &my_charset_latin1);
        tmp_table->field[col]->sql_type(source_type);
        table->field[col]->sql_type(target_type);
        DBUG_PRINT("debug", ("Field %s - conversion required."
                             " Source type: '%s', Target type: '%s'",
                             tmp_table->field[col]->field_name.str,
                             source_type.c_ptr_safe(), target_type.c_ptr_safe()));
      }
  }
#endif

  *conv_table_var= tmp_table;
  return true;
}


/**
  A wrapper to Virtual_tmp_table, to get access to its constructor,
  which is protected for safety purposes (against illegal use on stack).
*/
class Virtual_conversion_table: public Virtual_tmp_table
{
public:
  Virtual_conversion_table(THD *thd) :Virtual_tmp_table(thd) { }
  /**
    Add a new field into the virtual table.
    @param handler      - The type handler of the field.
    @param metadata     - The RBR binary log metadata for this field.
    @param target_field - The field from the target table, to get extra
                          attributes from (e.g. typelib in case of ENUM).
  */
  bool add(const Type_handler *handler,
           uint16 metadata, const Field *target_field)
  {
    Field *tmp= handler->make_conversion_table_field(in_use->mem_root,
                                                     this, metadata,
                                                     target_field);
    if (!tmp)
      return true;
    Virtual_tmp_table::add(tmp);
    DBUG_PRINT("debug", ("sql_type: %s, target_field: '%s', max_length: %d, decimals: %d,"
                         " maybe_null: %d, unsigned_flag: %d, pack_length: %u",
                         handler->name().ptr(), target_field->field_name.str,
                         tmp->field_length, tmp->decimals(), TRUE,
                         tmp->flags, tmp->pack_length()));
    return false;
  }
};


/**
  Create a conversion table.

  If the function is unable to create the conversion table, an error
  will be printed and NULL will be returned.

  @return Pointer to conversion table, or NULL if unable to create
  conversion table.
 */

TABLE *table_def::create_conversion_table(THD *thd, rpl_group_info *rgi,
                                          TABLE *target_table) const
{
  DBUG_ENTER("table_def::create_conversion_table");

  Virtual_conversion_table *conv_table;
  Relay_log_info *rli= rgi->rli;
  /*
    At slave, columns may differ. So we should create
    MY_MIN(columns@master, columns@slave) columns in the
    conversion table.
  */
  uint const cols_to_create= MY_MIN(target_table->s->fields, size());
  if (!(conv_table= new(thd) Virtual_conversion_table(thd)) ||
      conv_table->init(cols_to_create))
    goto err;
  for (uint col= 0 ; col < cols_to_create; ++col)
  {
    const Type_handler *ha= field_type_handler(col);
    DBUG_ASSERT(ha); // Checked at compatible_with() time
    if (conv_table->add(ha, field_metadata(col), target_table->field[col]))
    {
      DBUG_PRINT("debug", ("binlog_type: %d, metadata: %04X, target_field: '%s'"
                           " make_conversion_table_field() failed",
                           binlog_type(col), field_metadata(col),
                           target_table->field[col]->field_name.str));
      goto err;
    }
  }

  if (conv_table->open())
    goto err; // Could not allocate record buffer?

  DBUG_RETURN(conv_table);

err:
  if (conv_table)
    delete conv_table;
  rli->report(ERROR_LEVEL, ER_SLAVE_CANT_CREATE_CONVERSION, rgi->gtid_info(),
              ER_THD(thd, ER_SLAVE_CANT_CREATE_CONVERSION),
              target_table->s->db.str,
              target_table->s->table_name.str);
  DBUG_RETURN(NULL);
}



Deferred_log_events::Deferred_log_events(Relay_log_info *rli) : last_added(NULL)
{
  my_init_dynamic_array(PSI_INSTRUMENT_ME, &array, sizeof(Log_event *), 32, 16, MYF(0));
}

Deferred_log_events::~Deferred_log_events()
{
  delete_dynamic(&array);
}

int Deferred_log_events::add(Log_event *ev)
{
  last_added= ev;
  insert_dynamic(&array, (uchar*) &ev);
  return 0;
}

bool Deferred_log_events::is_empty()
{
  return array.elements == 0;
}

bool Deferred_log_events::execute(rpl_group_info *rgi)
{
  bool res= false;
  DBUG_ENTER("Deferred_log_events::execute");
  DBUG_ASSERT(rgi->deferred_events_collecting);

  rgi->deferred_events_collecting= false;
  for (uint i=  0; !res && i < array.elements; i++)
  {
    Log_event *ev= (* (Log_event **)
                    dynamic_array_ptr(&array, i));
    res= ev->apply_event(rgi);
  }
  rgi->deferred_events_collecting= true;
  DBUG_RETURN(res);
}

void Deferred_log_events::rewind()
{
  /*
    Reset preceding Query log event events which execution was
    deferred because of slave side filtering.
  */
  if (!is_empty())
  {
    for (uint i=  0; i < array.elements; i++)
    {
      Log_event *ev= *(Log_event **) dynamic_array_ptr(&array, i);
      delete ev;
    }
    last_added= NULL;
    if (array.elements > array.max_element)
      freeze_size(&array);
    reset_dynamic(&array);
  }
  last_added= NULL;
}

#endif // defined(HAVE_REPLICATION)

