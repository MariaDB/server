/*
   Copyright (c) 2018, Facebook, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#pragma once

// C++ standard header files
#include <string>
#include <vector>

// MySQL header files
#include "./handler.h"    // handler
#include "./my_global.h"  // ulonglong
#include "./sql_string.h"
#include "./ut0counter.h"

// MyRocks header files
#include "./ha_rocksdb.h"
#include "./rdb_datadic.h"

namespace myrocks {
class Rdb_field_encoder;

/**
  Describes instructions on how to decode the field for value slice
*/
struct READ_FIELD {
  // Points to Rdb_field_encoder describing the field
  Rdb_field_encoder *m_field_enc;
  // if true, decode the field, otherwise skip it
  bool m_decode;
  // Skip this many bytes before reading (or skipping) this field
  int m_skip;
};

/**
 Class to convert rocksdb value slice from storage format to mysql record
 format.
*/
class Rdb_convert_to_record_value_decoder {
 public:
  Rdb_convert_to_record_value_decoder() = delete;
  Rdb_convert_to_record_value_decoder(
      const Rdb_convert_to_record_value_decoder &decoder) = delete;
  Rdb_convert_to_record_value_decoder &operator=(
      const Rdb_convert_to_record_value_decoder &decoder) = delete;

  static int decode(uchar *const buf, uint *offset, TABLE *table,
                    my_core::Field *field, Rdb_field_encoder *field_dec,
                    Rdb_string_reader *reader, bool decode, bool is_null);

 private:
  static int decode_blob(TABLE *table, Field *field, Rdb_string_reader *reader,
                         bool decode);
  static int decode_fixed_length_field(Field *const field,
                                       Rdb_field_encoder *field_dec,
                                       Rdb_string_reader *const reader,
                                       bool decode);

  static int decode_varchar(Field *const field, Rdb_string_reader *const reader,
                            bool decode);
};

/**
  Class to iterator fields in RocksDB value slice
  A template class instantiation represent a way to decode the data.
  The reason to use template class instead of normal class is to elimate
  virtual method call.
*/
template <typename value_field_decoder>
class Rdb_value_field_iterator {
 private:
  bool m_is_null;
  std::vector<READ_FIELD>::const_iterator m_field_iter;
  std::vector<READ_FIELD>::const_iterator m_field_end;
  Rdb_string_reader *m_value_slice_reader;
  // null value map
  const char *m_null_bytes;
  // The current open table
  TABLE *m_table;
  // The current field
  Field *m_field;
  Rdb_field_encoder *m_field_dec;
  uchar *const m_buf;
  uint m_offset;

 public:
  Rdb_value_field_iterator(TABLE *table, Rdb_string_reader *value_slice_reader,
                           const Rdb_converter *rdb_converter,
                           uchar *const buf);
  Rdb_value_field_iterator(const Rdb_value_field_iterator &field_iterator) =
      delete;
  Rdb_value_field_iterator &operator=(
      const Rdb_value_field_iterator &field_iterator) = delete;

  /*
    Move and decode next field
    Run next() before accessing data
  */
  int next();
  // Whether current field is the end of fields
  bool end_of_fields() const;
  void *get_dst() const;
  // Whether the value of current field is null
  bool is_null() const;
  // get current field index
  int get_field_index() const;
  // get current field type
  enum_field_types get_field_type() const;
  // get current field
  Field *get_field() const;
};

/**
  Class to convert Mysql formats to rocksdb storage format, and vice versa.
*/
class Rdb_converter {
 public:
  /*
    Initialize converter with table data
  */
  Rdb_converter(const THD *thd, const Rdb_tbl_def *tbl_def, TABLE *table);
  Rdb_converter(const Rdb_converter &decoder) = delete;
  Rdb_converter &operator=(const Rdb_converter &decoder) = delete;
  ~Rdb_converter();

  void setup_field_decoders(const MY_BITMAP *field_map,
                            bool decode_all_fields = false);

  int decode(const std::shared_ptr<Rdb_key_def> &key_def, uchar *dst,
             const rocksdb::Slice *key_slice,
             const rocksdb::Slice *value_slice);

  int encode_value_slice(const std::shared_ptr<Rdb_key_def> &pk_def,
                         const rocksdb::Slice &pk_packed_slice,
                         Rdb_string_writer *pk_unpack_info, bool is_update_row,
                         bool store_row_debug_checksums, char *ttl_bytes,
                         bool *is_ttl_bytes_updated,
                         rocksdb::Slice *const value_slice);

  my_core::ha_rows get_row_checksums_checked() const {
    return m_row_checksums_checked;
  }
  bool get_verify_row_debug_checksums() const {
    return m_verify_row_debug_checksums;
  }
  void set_verify_row_debug_checksums(bool verify_row_debug_checksums) {
    m_verify_row_debug_checksums = verify_row_debug_checksums;
  }

  const Rdb_field_encoder *get_encoder_arr() const { return m_encoder_arr; }
  int get_null_bytes_in_record() { return m_null_bytes_length_in_record; }
  const char *get_null_bytes() const { return m_null_bytes; }
  void set_is_key_requested(bool key_requested) {
    m_key_requested = key_requested;
  }
  bool get_maybe_unpack_info() const { return m_maybe_unpack_info; }

  char *get_ttl_bytes_buffer() { return m_ttl_bytes; }

  const std::vector<READ_FIELD> *get_decode_fields() const {
    return &m_decoders_vect;
  }

 private:
  int decode_value_header(Rdb_string_reader *reader,
                          const std::shared_ptr<Rdb_key_def> &pk_def,
                          rocksdb::Slice *unpack_slice);

  void setup_field_encoders();

  void get_storage_type(Rdb_field_encoder *const encoder, const uint kp);

  int convert_record_from_storage_format(
      const std::shared_ptr<Rdb_key_def> &pk_def,
      const rocksdb::Slice *const key, const rocksdb::Slice *const value,
      uchar *const buf);

  int verify_row_debug_checksum(const std::shared_ptr<Rdb_key_def> &pk_def,
                                Rdb_string_reader *reader,
                                const rocksdb::Slice *key,
                                const rocksdb::Slice *value);

 private:
  /*
    This tells if any field which is part of the key needs to be unpacked and
    decoded.
  */
  bool m_key_requested;
  /*
   Controls whether verifying checksums during reading, This is updated from
  the session variable at the start of each query.
  */
  bool m_verify_row_debug_checksums;
  // Thread handle
  const THD *m_thd;
  /* MyRocks table definition*/
  const Rdb_tbl_def *m_tbl_def;
  /* The current open table */
  TABLE *m_table;
  /*
    Number of bytes in on-disk (storage) record format that are used for
    storing SQL NULL flags.
  */
  int m_null_bytes_length_in_record;
  /*
    Pointer to null bytes value
  */
  const char *m_null_bytes;
  /*
   TRUE <=> Some fields in the PK may require unpack_info.
  */
  bool m_maybe_unpack_info;
  /*
    Pointer to the original TTL timestamp value (8 bytes) during UPDATE.
  */
  char m_ttl_bytes[ROCKSDB_SIZEOF_TTL_RECORD];
  /*
    Array of table->s->fields elements telling how to store fields in the
    record.
  */
  Rdb_field_encoder *m_encoder_arr;
  /*
    Array of request fields telling how to decode data in RocksDB format
  */
  std::vector<READ_FIELD> m_decoders_vect;
  /*
    A counter of how many row checksums were checked for this table. Note that
    this does not include checksums for secondary index entries.
  */
  my_core::ha_rows m_row_checksums_checked;
  // buffer to hold data during encode_value_slice
  String m_storage_record;
};
}  // namespace myrocks
