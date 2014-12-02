/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012-2014 Kouhei Sutou <kou@clear-code.com>
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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <mrn_mysql.h>

#include "mrn_multiple_column_key_codec.hpp"
#include "mrn_field_normalizer.hpp"
#include "mrn_smart_grn_obj.hpp"

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
#else /* WORDS_BIGENDIAN */
#define mrn_byte_order_host_to_network(buf, key, size)  \
{                                                       \
  uint32 size_ = (uint32)(size);                        \
  uint8 *buf_ = (uint8 *)(buf);                         \
  uint8 *key_ = (uint8 *)(key) + size_;                 \
  while (size_--) { *buf_++ = *(--key_); }              \
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
      DBUG_PRINT("info", ("mroonga: key_part->length=%u", key_part->length));

      if (field->null_bit) {
        DBUG_PRINT("info", ("mroonga: field has null bit"));
        *current_grn_key = 0;
        current_mysql_key += 1;
        current_grn_key += 1;
        (*grn_key_length)++;
      }

      DataType data_type = TYPE_UNKNOWN;
      uint data_size = 0;
      get_key_info(key_part, &data_type, &data_size);

      switch (data_type) {
      case TYPE_UNKNOWN:
        // TODO: This will not be happen. This is just for
        // suppressing warnings by gcc -O2. :<
        error = HA_ERR_UNSUPPORTED;
        break;
      case TYPE_LONG_LONG_NUMBER:
        {
          long long int long_long_value = 0;
          switch (data_size) {
          case 3:
            long_long_value = (long long int)sint3korr(current_mysql_key);
            break;
          case 8:
            long_long_value = (long long int)sint8korr(current_mysql_key);
            break;
          }
          mrn_byte_order_host_to_network(current_grn_key, &long_long_value,
                                         data_size);
          *((uint8 *)(current_grn_key)) ^= 0x80;
        }
        break;
      case TYPE_NUMBER:
        mrn_byte_order_host_to_network(current_grn_key, current_mysql_key, data_size);
        {
          Field_num *number_field = (Field_num *)field;
          if (!number_field->unsigned_flag) {
            *((uint8 *)(current_grn_key)) ^= 0x80;
          }
        }
        break;
      case TYPE_FLOAT:
        {
          float value;
          float4get(value, current_mysql_key);
          encode_float(value, data_size, current_grn_key);
        }
        break;
      case TYPE_DOUBLE:
        {
          double value;
          float8get(value, current_mysql_key);
          encode_double(value, data_size, current_grn_key);
        }
        break;
      case TYPE_BYTE_SEQUENCE:
        memcpy(current_grn_key, current_mysql_key, data_size);
        break;
      case TYPE_BYTE_REVERSE:
        encode_reverse(current_mysql_key, data_size, current_grn_key);
        break;
      case TYPE_BYTE_BLOB:
        encode_blob(field, current_mysql_key, current_grn_key, &data_size);
        break;
      }

      if (error) {
        break;
      }

      current_mysql_key += data_size;
      current_grn_key += data_size;
      *grn_key_length += data_size;
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

      switch (data_type) {
      case TYPE_UNKNOWN:
        // TODO: This will not be happen. This is just for
        // suppressing warnings by gcc -O2. :<
        error = HA_ERR_UNSUPPORTED;
        break;
      case TYPE_LONG_LONG_NUMBER:
        {
          long long int long_long_value = 0;
          switch (data_size) {
          case 3:
            long_long_value = (long long int)sint3korr(current_grn_key);
            break;
          case 8:
            long_long_value = (long long int)sint8korr(current_grn_key);
            break;
          }
          *((uint8 *)(&long_long_value)) ^= 0x80;
          mrn_byte_order_host_to_network(current_mysql_key, &long_long_value,
                                         data_size);
        }
        break;
      case TYPE_NUMBER:
        {
          uchar buffer[8];
          memcpy(buffer, current_grn_key, data_size);
          Field_num *number_field = (Field_num *)field;
          if (!number_field->unsigned_flag) {
            buffer[0] ^= 0x80;
          }
          mrn_byte_order_host_to_network(current_mysql_key, buffer,
                                         data_size);
        }
        break;
      case TYPE_FLOAT:
        decode_float(current_grn_key, current_mysql_key, data_size);
        break;
      case TYPE_DOUBLE:
        decode_double(current_grn_key, current_mysql_key, data_size);
        break;
      case TYPE_BYTE_SEQUENCE:
        memcpy(current_mysql_key, current_grn_key, data_size);
        break;
      case TYPE_BYTE_REVERSE:
        decode_reverse(current_grn_key, current_mysql_key, data_size);
        break;
      case TYPE_BYTE_BLOB:
        memcpy(current_mysql_key,
               current_grn_key + data_size,
               HA_KEY_BLOB_LENGTH);
        memcpy(current_mysql_key + HA_KEY_BLOB_LENGTH,
               current_grn_key,
               data_size);
        data_size += HA_KEY_BLOB_LENGTH;
        break;
      }

      if (error) {
        break;
      }

      current_grn_key += data_size;
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
      total_size += data_size;
      if (data_type == TYPE_BYTE_BLOB) {
        total_size += HA_KEY_BLOB_LENGTH;
      }
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
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
      DBUG_PRINT("info", ("mroonga: MYSQL_TYPE_DATETIME"));
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
      *data_type = TYPE_LONG_LONG_NUMBER;
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
      *data_type = TYPE_BYTE_SEQUENCE;
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
    }
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_float(volatile float value, uint data_size,
                                            uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    int n_bits = (data_size * 8 - 1);
    volatile int *int_value_pointer = (int *)(&value);
    int int_value = *int_value_pointer;
    int_value ^= ((int_value >> n_bits) | (1 << n_bits));
    mrn_byte_order_host_to_network(grn_key, &int_value, data_size);
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::decode_float(const uchar *grn_key,
                                            uchar *mysql_key,
                                            uint data_size) {
    MRN_DBUG_ENTER_METHOD();
    int int_value;
    mrn_byte_order_host_to_network(&int_value, grn_key, data_size);
    int max_bit = (data_size * 8 - 1);
    *((int *)mysql_key) =
      int_value ^ (((int_value ^ (1 << max_bit)) >> max_bit) |
                   (1 << max_bit));
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_double(volatile double value, uint data_size,
                                             uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    int n_bits = (data_size * 8 - 1);
    volatile long long int *long_long_value_pointer = (long long int *)(&value);
    volatile long long int long_long_value = *long_long_value_pointer;
    long_long_value ^= ((long_long_value >> n_bits) | (1LL << n_bits));
    mrn_byte_order_host_to_network(grn_key, &long_long_value, data_size);
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::decode_double(const uchar *grn_key,
                                             uchar *mysql_key,
                                             uint data_size) {
    MRN_DBUG_ENTER_METHOD();
    long long int long_long_value;
    mrn_byte_order_host_to_network(&long_long_value, grn_key, data_size);
    int max_bit = (data_size * 8 - 1);
    *((long long int *)mysql_key) =
      long_long_value ^ (((long_long_value ^ (1LL << max_bit)) >> max_bit) |
                         (1LL << max_bit));
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_reverse(const uchar *mysql_key, uint data_size,
                                              uchar *grn_key) {
    MRN_DBUG_ENTER_METHOD();
    for (uint i = 0; i < data_size; i++) {
      grn_key[i] = mysql_key[data_size - i - 1];
    }
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::decode_reverse(const uchar *grn_key,
                                              uchar *mysql_key,
                                              uint data_size) {
    MRN_DBUG_ENTER_METHOD();
    for (uint i = 0; i < data_size; i++) {
      mysql_key[i] = grn_key[data_size - i - 1];
    }
    DBUG_VOID_RETURN;
  }

  void MultipleColumnKeyCodec::encode_blob(Field *field,
                                           const uchar *mysql_key,
                                           uchar *grn_key,
                                           uint *data_size) {
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
        if (normalized_length < *data_size) {
          memset(grn_key + normalized_length,
                 '\0', *data_size - normalized_length);
        }
        new_blob_data_length = normalized_length;
      } else {
        push_warning_printf(thread_,
                            Sql_condition::WARN_LEVEL_WARN,
                            WARN_DATA_TRUNCATED,
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
      memcpy(grn_key + *data_size, &new_blob_data_length, HA_KEY_BLOB_LENGTH);
    } else {
      memcpy(grn_key + *data_size, mysql_key, HA_KEY_BLOB_LENGTH);
      memcpy(grn_key, mysql_key + HA_KEY_BLOB_LENGTH, *data_size);
    }
    *data_size += HA_KEY_BLOB_LENGTH;
  }
}
