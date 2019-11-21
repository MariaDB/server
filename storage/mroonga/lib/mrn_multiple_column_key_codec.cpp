/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012-2015 Kouhei Sutou <kou@clear-code.com>
  Copyright(C) 2013 Kentoku SHIBA

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include <mrn_mysql.h>

#include "mrn_multiple_column_key_codec.hpp"
#include "mrn_field_normalizer.hpp"
#include "mrn_smart_grn_obj.hpp"
#include "mrn_time_converter.hpp"
#include "mrn_value_decoder.hpp"

// for debug
#define MRN_CLASS_NAME "mrn::MultipleColumnKeyCodec"

#ifdef WORDS_BIGENDIAN
#define mrn_byte_order_host_to_network(buf, key, size)  \
{                                                       \
  uint32 size_ = (uint32)(size);                        \
  uint8 *buf_ = (uint8 *)(buf);                         \
  uint8 *key_ = (uint8 *)(key);                         \
  while (size_--) { *buf_++ = *key_++; }                \
}
#define mrn_byte_order_network_to_host(buf, key, size)  \
{                                                       \
  uint32 size_ = (uint32)(size);                        \
  uint8 *buf_ = (uint8 *)(buf);                         \
  uint8 *key_ = (uint8 *)(key);                         \
  while (size_) { *buf_++ = *key_++; size_--; }         \
}
#else /* WORDS_BIGENDIAN */
#define mrn_byte_order_host_to_network(buf, key, size)  \
{                                                       \
  uint32 size_ = (uint32)(size);                        \
  uint8 *buf_ = (uint8 *)(buf);                         \
  uint8 *key_ = (uint8 *)(key) + size_;                 \
  while (size_--) { *buf_++ = *(--key_); }              \
}
#define mrn_byte_order_network_to_host(buf, key, size)  \
{                                                       \
  uint32 size_ = (uint32)(size);                        \
  uint8 *buf_ = (uint8 *)(buf);                         \
  uint8 *key_ = (uint8 *)(key) + size_;                 \
  while (size_) { *buf_++ = *(--key_); size_--; }       \
}
#endif /* WORDS_BIGENDIAN */

namespace mrn {
  MultipleColumnKeyCodec::MultipleColumnKeyCodec(grn_ctx *ctx,
                                                 THD *thread,
                                                 KEY *key_info)
    : ctx_(ctx),
      thread_(thread),
      key_info_(key_info) {
  }

  MultipleColumnKeyCodec::~MultipleColumnKeyCodec() {
  }

  int MultipleColumnKeyCodec::encode(const uchar *mysql_key,
                                     uint mysql_key_length,
                                     uchar *grn_key,
                                     uint *grn_key_length) {
    MRN_DBUG_ENTER_METHOD();
    int error = 0;
    const uchar *current_mysql_key = mysql_key;
    const uchar *mysql_key_end = mysql_key + mysql_key_length;
    uchar *current_grn_key = grn_key;

    int n_key_parts = KEY_N_KEY_PARTS(key_info_);
    DBUG_PRINT("info", ("mroonga: n_key_parts=%d", n_key_parts));
    *grn_key_length = 0;
    for (int i = 0; i < n_key_parts && current_mysql_key < mysql_key_end; i++) {
      KEY_PART_INFO *key_part = &(key_info_->key_part[i]);
      Field *field = key_part->field;
      bool is_null = false;
      DBUG_PRINT("info", ("mroonga: key_part->length=%u", key_part->length));

      if (field->null_bit) {
        DBUG_PRINT("info", ("mroonga: field has null bit"));
        *current_grn_key = 0;
        is_null = *current_mysql_key;
        current_mysql_key += 1;
        current_grn_key += 1;
        (*grn_key_length)++;
      }

      DataType data_type = TYPE_UNKNOWN;
      uint data_size = 0;
      get_key_info(key_part, &data_type, &data_size);
      uint grn_key_data_size = data_size;

      switch (data_type) {
      case TYPE_UNKNOWN:
        // TODO: This will not be happen. This is just for
        // suppressing warnings by gcc -O2. :<
        error = HA_ERR_UNSUPPORTED;
        break;
      case TYPE_LONG_LONG_NUMBER:
        {
          long long int long_long_value = 0;
          long_long_value = sint8korr(current_mysql_key);
          encode_long_long_int(long_long_value, current_grn_key);
        }
        break;
      case TYPE_NUMBER:
        {
          Field_num *number_field = static_cast<Field_num *>(field);
          encode_number(current_mysql_key,
                        data_size,
                        !number_field->unsigned_flag,
                        current_grn_key);
        }
        break;
      case TYPE_FLOAT:
        {
          float value;
          value_decoder::decode(&value, current_mysql_key);
          encode_float(value, data_size, current_grn_key);
        }
        break;
      case TYPE_DOUBLE:
        {
          double value;
          value_decoder::decode(&value, current_mysql_key);
          encode_double(value, data_size, current_grn_key);
        }
        break;
      case TYPE_DATETIME:
        {
          long long int mysql_datetime;
#ifdef WORDS_BIGENDIAN
          if (field->table && field->table->s->db_low_byte_first) {
            mysql_datetime = sint8korr(current_mysql_key);
          } else
#endif
          {
            value_decoder::decode(&mysql_datetime, current_mysql_key);
          }
          TimeConverter time_converter;
          bool truncated;
          long long int grn_time =
            time_converter.mysql_datetime_to_grn_time(mysql_datetime,
                                                      &truncated);
          encode_long_long_int(grn_time, current_grn_key);
        }
        break;
#ifdef MRN_HAVE_MYSQL_TYPE_DATETIME2
      case TYPE_DATETIME2:
        {
          Field_datetimef *datetimef_field =
            static_cast<Field_datetimef *>(field);
          long long int mysql_datetime_packed = is_null ? 0 :
            my_datetime_packed_from_binary(current_mysql_key,
                                           datetimef_field->decimals());
          MYSQL_TIME mysql_time;
          TIME_from_longlong_datetime_packed(&mysql_time, mysql_datetime_packed);
          TimeConverter time_converter;
          bool truncated;
          long long int grn_time =
            time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
          grn_key_data_size = 8;
          encode_long_long_int(grn_time, current_grn_key);
        }
        break;
#endif
      case TYPE_BYTE_SEQUENCE:
        memcpy(current_grn_key, current_mysql_key, data_size);
        break;
      case TYPE_BYTE_REVERSE:
        encode_reverse(current_mysql_key, data_size, current_grn_key);
        break;
      case TYPE_BYTE_BLOB:
        encode_blob(current_mysql_key, &data_size, field, current_grn_key);
        grn_key_data_size = data_size;
        break;
      }

      if (error) {
        break;
      }

      current_mysql_key += data_size;
      current_grn_key += grn_key_data_size;
      *grn_key_length += grn_key_data_size;
    }

    DBUG_RETURN(error);
  }

  int MultipleColumnKeyCodec::decode(const uchar *grn_key,
                                     uint grn_key_length,
                                     uchar *mysql_key,
                                     uint *mysql_key_length) {
    MRN_DBUG_ENTER_METHOD();
    int error = 0;
    const uchar *current_grn_key = grn_key;
    const uchar *grn_key_end = grn_key + grn_key_length;
    uchar *current_mysql_key = mysql_key;

    int n_key_parts = KEY_N_KEY_PARTS(key_info_);
    DBUG_PRINT("info", ("mroonga: n_key_parts=%d", n_key_parts));
    *mysql_key_length = 0;
    for (int i = 0; i < n_key_parts && current_grn_key < grn_key_end; i++) {
      KEY_PART_INFO *key_part = &(key_info_->key_part[i]);
      Field *field = key_part->field;
      DBUG_PRINT("info", ("mroonga: key_part->length=%u", key_part->length));

      if (field->null_bit) {
        DBUG_PRINT("info", ("mroonga: field has null bit"));
        *current_mysql_key = 0;
        current_grn_key += 1;
        current_mysql_key += 1;
        (*mysql_key_length)++;
      }

      DataType data_type = TYPE_UNKNOWN;
      uint data_size = 0;
      get_key_info(key_part, &data_type, &data_size);
      uint grn_key_data_size = data_size;

      switch (data_type) {
      case TYPE_UNKNOWN:
        // TODO: This will not be happen. This is just for
        // suppressing warnings by gcc -O2. :<
        error = HA_ERR_UNSUPPORTED;
        break;
      case TYPE_LONG_LONG_NUMBER:
        {
          long long int value;
          decode_long_long_int(current_grn_key, &value);
          int8store(current_mysql_key, value);
        }
        break;
      case TYPE_NUMBER:
        {
          Field_num *number_field = static_cast<Field_num *>(field);
          decode_number(current_grn_key,
                        grn_key_data_size,
                        !number_field->unsigned_flag,
                        current_mysql_key);
        }
        break;
      case TYPE_FLOAT:
        decode_float(current_grn_key, grn_key_data_size, current_mysql_key);
        break;
      case TYPE_DOUBLE:
        decode_double(current_grn_key, grn_key_data_size, current_mysql_key);
        break;
      case TYPE_DATETIME:
        {
          long long int grn_time;
          decode_long_long_int(current_grn_key, &grn_time);
          TimeConverter time_converter;
          long long int mysql_datetime =
            time_converter.grn_time_to_mysql_datetime(grn_time);
#ifdef WORDS_BIGENDIAN
          if (field->table && field->table->s->db_low_byte_first) {
            int8store(current_mysql_key, mysql_datetime);
          } else
#endif
          {
            longlongstore(current_mysql_key, mysql_datetime);
          }
        }
        break;
#ifdef MRN_HAVE_MYSQL_TYPE_DATETIME2
      case TYPE_DATETIME2:
        {
          Field_datetimef *datetimef_field =
            static_cast<Field_datetimef *>(field);
          long long int grn_time;
          grn_key_data_size = 8;
          decode_long_long_int(current_grn_key, &grn_time);
          TimeConverter time_converter;
          MYSQL_TIME mysql_time;
          mysql_time.neg = FALSE;
          mysql_time.time_type = MYSQL_TIMESTAMP_DATETIME;
          time_converter.grn_time_to_mysql_time(grn_time, &mysql_time);
          long long int mysql_datetime_packed =
            TIME_to_longlong_datetime_packed(&mysql_time);
          my_datetime_packed_to_binary(mysql_datetime_packed,
                                       current_mysql_key,
                                       datetimef_field->decimals());
        }
        break;
#endif
      case TYPE_BYTE_SEQUENCE:
        memcpy(current_mysql_key, current_grn_key, grn_key_data_size);
        break;
      case TYPE_BYTE_REVERSE:
        decode_reverse(current_grn_key, grn_key_data_size, current_mysql_key);
        break;
      case TYPE_BYTE_BLOB:
        memcpy(current_mysql_key,
               current_grn_key + data_size,
               HA_KEY_BLOB_LENGTH);
        memcpy(current_mysql_key + HA_KEY_BLOB_LENGTH,
               current_grn_key,
               data_size);
        data_size += HA_KEY_BLOB_LENGTH;
        grn_key_data_size = data_size;
        break;
      }

      if (error) {
        break;
      }

      current_grn_key += grn_key_data_size;
      current_mysql_key += data_size;
      *mysql_key_length += data_size;
    }

    DBUG_RETURN(error);
  }

  uint MultipleColumnKeyCodec::size() {
    MRN_DBUG_ENTER_METHOD();

    int n_key_parts = KEY_N_KEY_PARTS(key_info_);
    DBUG_PRINT("info", ("mroonga: n_key_parts=%d", n_key_parts));

    uint total_size = 0;
    for (int i = 0; i < n_key_parts; ++i) {
      KEY_PART_INFO *key_part = &(key_info_->key_part[i]);
      Field *field = key_part->field;
      DBUG_PRINT("info", ("mroonga: key_part->length=%u", key_part->length));

      if (field->null_bit) {
        DBUG_PRINT("info", ("mroonga: field has null bit"));
        ++total_size;
      }

      DataType data_type = TYPE_UNKNOWN;
      uint data_size = 0;
      get_key_info(key_part, &data_type, &data_size);
      switch (data_type) {
#ifdef MRN_HAVE_MYSQL_TYPE_DATETIME2
      case TYPE_DATETIME2:
        data_size = 8;
        break;
#endif
      case TYPE_BYTE_BLOB:
        data_size += HA_KEY_BLOB_LENGTH;
        break;
      default:
        break;
      }
      total_size += data_size;
    }

    DBUG_RETURN(total_size);
  }

  void MultipleColumnKeyCodec::get_key_info(KEY_PART_INFO *key_part,
                                            DataType *data_type,
                                            uint *data_size) {
    MRN_DBUG_ENTER_METHOD();

    *data_type = TYPE_UNKNOWN;
    *data_size = 0;

    Field *field = key_part->field;
    switch (field->real_type()) {
    case MYSQL_TYPE_DECIMAL:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_DECIMAL"));
      *data_type = TYPE_BYTE_SEQUENCE;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_YEAR:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_TINY"));
      *data_type = TYPE_NUMBER;
      *data_size = 1;
      break;
    case MYSQL_TYPE_SHORT:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_SHORT"));
      *data_type = TYPE_NUMBER;
      *data_size = 2;
      break;
    case MYSQL_TYPE_LONG:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_LONG"));
      *data_type = TYPE_NUMBER;
      *data_size = 4;
      break;
    case MYSQL_TYPE_FLOAT:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_FLOAT"));
      *data_type = TYPE_FLOAT;
      *data_size = 4;
      break;
    case MYSQL_TYPE_DOUBLE:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_DOUBLE"));
      *data_type = TYPE_DOUBLE;
      *data_size = 8;
      break;
    case MYSQL_TYPE_NULL:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_NULL"));
      *data_type = TYPE_NUMBER;
      *data_size = 1;
      break;
    case MYSQL_TYPE_TIMESTAMP:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_TIMESTAMP"));
      *data_type = TYPE_BYTE_REVERSE;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_DATE:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_DATE"));
      *data_type = TYPE_BYTE_REVERSE;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_DATETIME:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_DATETIME"));
      *data_type = TYPE_DATETIME;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_NEWDATE:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_NEWDATE"));
      *data_type = TYPE_BYTE_REVERSE;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_LONGLONG:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_LONGLONG"));
      *data_type = TYPE_NUMBER;
      *data_size = 8;
      break;
    case MYSQL_TYPE_INT24:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_INT24"));
      *data_type = TYPE_NUMBER;
      *data_size = 3;
      break;
    case MYSQL_TYPE_TIME:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_TIME"));
      *data_type = TYPE_NUMBER;
      *data_size = 3;
      break;
    case MYSQL_TYPE_VARCHAR:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_VARCHAR"));
      *data_type = TYPE_BYTE_BLOB;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_BIT:
      // TODO
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_BIT"));
      *data_type = TYPE_NUMBER;
      *data_size = 1;
      break;
#ifdef MRN_HAVE_MYSQL_TYPE_TIMESTAMP2
    case MYSQL_TYPE_TIMESTAMP2:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_TIMESTAMP2"));
      *data_type = TYPE_BYTE_SEQUENCE;
      *data_size = key_part->length;
      break;
#endif
#ifdef MRN_HAVE_MYSQL_TYPE_DATETIME2
    case MYSQL_TYPE_DATETIME2:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_DATETIME2"));
      *data_type = TYPE_DATETIME2;
      *data_size = key_part->length;
      break;
#endif
#ifdef MRN_HAVE_MYSQL_TYPE_TIME2
    case MYSQL_TYPE_TIME2:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_TIME2"));
      *data_type = TYPE_BYTE_SEQUENCE;
      *data_size = key_part->length;
      break;
#endif
    case MYSQL_TYPE_NEWDECIMAL:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_NEWDECIMAL"));
      *data_type = TYPE_BYTE_SEQUENCE;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_ENUM:
      // TODO
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_ENUM"));
      *data_type = TYPE_NUMBER;
      *data_size = 1;
      break;
    case MYSQL_TYPE_SET:
      // TODO
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_SET"));
      *data_type = TYPE_NUMBER;
      *data_size = 1;
      break;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      // TODO
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_BLOB"));
      *data_type = TYPE_BYTE_BLOB;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      // TODO
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_STRING"));
      *data_type = TYPE_BYTE_SEQUENCE;
      *data_size = key_part->length;
      break;
    case MYSQL_TYPE_GEOMETRY:
      // TODO
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_GEOMETRY"));
      *data_type = TYPE_BYTE_SEQUENCE;
      *data_size = key_part->length;
      break;
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
    case MYSQL_TYPE_JSON:
      // TODO
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_JSON"));
      *data_type = TYPE_BYTE_SEQUENCE;
      *data_size = key_part->length;
      break;
#endif
    }
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_number(const uchar *mysql_key,
                                             uint mysql_key_size,
                                             bool is_signed,
                                             uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    mrn_byte_order_host_to_network(grn_key, mysql_key, mysql_key_size);
    if (is_signed) {
      grn_key[0] ^= 0x80;
    }
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::decode_number(const uchar *grn_key,
                                             uint grn_key_size,
                                             bool is_signed,
                                             uchar *mysql_key) {
    MRN_DBUG_ENTER_METHOD();
    uchar buffer[8];
    memcpy(buffer, grn_key, grn_key_size);
    if (is_signed) {
      buffer[0] ^= 0x80;
    }
    mrn_byte_order_network_to_host(mysql_key, buffer, grn_key_size);
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_long_long_int(volatile long long int value,
                                                    uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    uint value_size = 8;
    mrn_byte_order_host_to_network(grn_key, &value, value_size);
    grn_key[0] ^= 0x80;
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::decode_long_long_int(const uchar *grn_key,
                                                    long long int *value) {
    MRN_DBUG_ENTER_METHOD();
    uint grn_key_size = 8;
    uchar buffer[8];
    memcpy(buffer, grn_key, grn_key_size);
    buffer[0] ^= 0x80;
    mrn_byte_order_network_to_host(value, buffer, grn_key_size);
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_float(volatile float value,
                                            uint value_size,
                                            uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    int n_bits = (value_size * 8 - 1);
    volatile int *int_value_pointer = (int *)(&value);
    int int_value = *int_value_pointer;
    int_value ^= ((int_value >> n_bits) | (1 << n_bits));
    mrn_byte_order_host_to_network(grn_key, &int_value, value_size);
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::decode_float(const uchar *grn_key,
                                            uint grn_key_size,
                                            uchar *mysql_key) {
    MRN_DBUG_ENTER_METHOD();
    int int_value;
    mrn_byte_order_network_to_host(&int_value, grn_key, grn_key_size);
    int max_bit = (grn_key_size * 8 - 1);
    *((int *)mysql_key) =
      int_value ^ (((int_value ^ (1 << max_bit)) >> max_bit) |
                   (1 << max_bit));
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_double(volatile double value,
                                             uint value_size,
                                             uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    int n_bits = (value_size * 8 - 1);
    volatile long long int *long_long_value_pointer = (long long int *)(&value);
    volatile long long int long_long_value = *long_long_value_pointer;
    long_long_value ^= ((long_long_value >> n_bits) | (1LL << n_bits));
    mrn_byte_order_host_to_network(grn_key, &long_long_value, value_size);
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::decode_double(const uchar *grn_key,
                                             uint grn_key_size,
                                             uchar *mysql_key) {
    MRN_DBUG_ENTER_METHOD();
    long long int long_long_value;
    mrn_byte_order_network_to_host(&long_long_value, grn_key, grn_key_size);
    int max_bit = (grn_key_size * 8 - 1);
    *((long long int *)mysql_key) =
      long_long_value ^ (((long_long_value ^ (1LL << max_bit)) >> max_bit) |
                         (1LL << max_bit));
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_reverse(const uchar *mysql_key,
                                              uint mysql_key_size,
                                              uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    for (uint i = 0; i < mysql_key_size; i++) {
      grn_key[i] = mysql_key[mysql_key_size - i - 1];
    }
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::decode_reverse(const uchar *grn_key,
                                              uint grn_key_size,
                                              uchar *mysql_key) {
    MRN_DBUG_ENTER_METHOD();
    for (uint i = 0; i < grn_key_size; i++) {
      mysql_key[i] = grn_key[grn_key_size - i - 1];
    }
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_blob(const uchar *mysql_key,
                                           uint *mysql_key_size,
                                           Field *field,
                                           uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    FieldNormalizer normalizer(ctx_, thread_, field);
    if (normalizer.should_normalize()) {
#if HA_KEY_BLOB_LENGTH != 2
#  error "TODO: support HA_KEY_BLOB_LENGTH != 2 case if it is needed"
#endif
      const char *blob_data =
        reinterpret_cast<const char *>(mysql_key + HA_KEY_BLOB_LENGTH);
      uint16 blob_data_length = *((uint16 *)(mysql_key));
      grn_obj *grn_string = normalizer.normalize(blob_data,
                                                 blob_data_length);
      mrn::SmartGrnObj smart_grn_string(ctx_, grn_string);
      const char *normalized;
      unsigned int normalized_length = 0;
      grn_string_get_normalized(ctx_, grn_string,
                                &normalized, &normalized_length, NULL);
      uint16 new_blob_data_length;
      if (normalized_length <= UINT_MAX16) {
        memcpy(grn_key, normalized, normalized_length);
        if (normalized_length < *mysql_key_size) {
          memset(grn_key + normalized_length,
                 '\0', *mysql_key_size - normalized_length);
        }
        new_blob_data_length = normalized_length;
      } else {
        push_warning_printf(thread_,
                            MRN_SEVERITY_WARNING,
                            MRN_ERROR_CODE_DATA_TRUNCATE(thread_),
                            "normalized data truncated "
                            "for multiple column index: "
                            "normalized-data-size: <%u> "
                            "max-data-size: <%u> "
                            "column-name: <%s> "
                            "data: <%.*s>",
                            normalized_length,
                            UINT_MAX16,
                            field->field_name,
                            blob_data_length, blob_data);
        memcpy(grn_key, normalized, blob_data_length);
        new_blob_data_length = blob_data_length;
      }
      memcpy(grn_key + *mysql_key_size,
             &new_blob_data_length,
             HA_KEY_BLOB_LENGTH);
    } else {
      memcpy(grn_key + *mysql_key_size, mysql_key, HA_KEY_BLOB_LENGTH);
      memcpy(grn_key, mysql_key + HA_KEY_BLOB_LENGTH, *mysql_key_size);
    }
    *mysql_key_size += HA_KEY_BLOB_LENGTH;
    DBUG_VOID_RETURN;
  }
}
