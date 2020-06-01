/*
   Copyright (c) 2015, Facebook, Inc.

   This program is f
   i the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <my_global.h>

/* This C++ file's header file */
#include "./rdb_converter.h"

/* Standard C++ header files */
#include <algorithm>
#include <map>
#include <string>
#include <vector>

/* MySQL header files */
#include "./field.h"
#include "./key.h"
#include "./m_ctype.h"
#include "./my_bit.h"
#include "./my_bitmap.h"
#include "./sql_table.h"


/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./ha_rocksdb_proto.h"
#include "./my_stacktrace.h"
#include "./rdb_cf_manager.h"
#include "./rdb_psi.h"
#include "./rdb_utils.h"


namespace myrocks {

void dbug_modify_key_varchar8(String *on_disk_rec) {
  std::string res;
  // The key starts with index number
  res.append(on_disk_rec->ptr(), Rdb_key_def::INDEX_NUMBER_SIZE);

  // Then, a mem-comparable form of a varchar(8) value.
  res.append("ABCDE\0\0\0\xFC", 9);
  on_disk_rec->length(0);
  on_disk_rec->append(res.data(), res.size());
}

/*
  Convert field from rocksdb storage format into Mysql Record format
  @param    buf         OUT          start memory to fill converted data
  @param    offset      IN/OUT       decoded data is stored in buf + offset
  @param    table       IN           current table
  @param    field       IN           current field
  @param    reader      IN           rocksdb value slice reader
  @param    decode      IN           whether to decode current field
  @return
    0      OK
    other  HA_ERR error code (can be SE-specific)
*/
int Rdb_convert_to_record_value_decoder::decode(uchar *const buf, uint *offset,
                                                TABLE *table,
                                                my_core::Field *field,
                                                Rdb_field_encoder *field_dec,
                                                Rdb_string_reader *reader,
                                                bool decode, bool is_null) {
  int err = HA_EXIT_SUCCESS;

  uint field_offset = field->ptr - table->record[0];
  *offset = field_offset;
  uint null_offset = field->null_offset();
  bool maybe_null = field->real_maybe_null();
  field->move_field(buf + field_offset,
                    maybe_null ? buf + null_offset : nullptr, field->null_bit);

  if (is_null) {
    if (decode) {
      // This sets the NULL-bit of this record
      field->set_null();
      /*
        Besides that, set the field value to default value. CHECKSUM TABLE
        depends on this.
      */
      memcpy(field->ptr, table->s->default_values + field_offset,
             field->pack_length());
    }
  } else {
    if (decode) {
      // sets non-null bits for this record
      field->set_notnull();
    }

    if (field_dec->m_field_type == MYSQL_TYPE_BLOB) {
      err = decode_blob(table, field, reader, decode);
    } else if (field_dec->m_field_type == MYSQL_TYPE_VARCHAR) {
      err = decode_varchar(field, reader, decode);
    } else {
      err = decode_fixed_length_field(field, field_dec, reader, decode);
    }
  }

  // Restore field->ptr and field->null_ptr
  field->move_field(table->record[0] + field_offset,
                    maybe_null ? table->record[0] + null_offset : nullptr,
                    field->null_bit);

  return err;
}

/*
  Convert blob from rocksdb storage format into Mysql Record format
  @param    table       IN           current table
  @param    field       IN           current field
  @param    reader      IN           rocksdb value slice reader
  @param    decode      IN           whether to decode current field
  @return
    0      OK
    other  HA_ERR error code (can be SE-specific)
*/
int Rdb_convert_to_record_value_decoder::decode_blob(TABLE *table, Field *field,
                                                     Rdb_string_reader *reader,
                                                     bool decode) {
  my_core::Field_blob *blob = (my_core::Field_blob *)field;

  // Get the number of bytes needed to store length
  const uint length_bytes = blob->pack_length() - portable_sizeof_char_ptr;

  const char *data_len_str;
  if (!(data_len_str = reader->read(length_bytes))) {
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }

  memcpy(blob->ptr, data_len_str, length_bytes);
  uint32 data_len =
      blob->get_length(reinterpret_cast<const uchar *>(data_len_str),
                       length_bytes);
  const char *blob_ptr;
  if (!(blob_ptr = reader->read(data_len))) {
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }

  if (decode) {
    // set 8-byte pointer to 0, like innodb does (relevant for 32-bit
    // platforms)
    memset(blob->ptr + length_bytes, 0, 8);
    memcpy(blob->ptr + length_bytes, &blob_ptr, sizeof(uchar **));
  }

  return HA_EXIT_SUCCESS;
}

/*
  Convert fixed length field from rocksdb storage format into Mysql Record
  format
  @param    field       IN           current field
  @param    field_dec   IN           data structure conttain field encoding data
  @param    reader      IN           rocksdb value slice reader
  @param    decode      IN           whether to decode current field
  @return
    0      OK
    other  HA_ERR error code (can be SE-specific)
*/
int Rdb_convert_to_record_value_decoder::decode_fixed_length_field(
    my_core::Field *const field, Rdb_field_encoder *field_dec,
    Rdb_string_reader *const reader, bool decode) {
  uint len = field_dec->m_pack_length_in_rec;
  if (len > 0) {
    const char *data_bytes;
    if ((data_bytes = reader->read(len)) == nullptr) {
      return HA_ERR_ROCKSDB_CORRUPT_DATA;
    }

    if (decode) {
      memcpy(field->ptr, data_bytes, len);
    }
  }

  return HA_EXIT_SUCCESS;
}

/*
  Convert varchar field from rocksdb storage format into Mysql Record format
  @param    field       IN           current field
  @param    field_dec   IN           data structure conttain field encoding data
  @param    reader      IN           rocksdb value slice reader
  @param    decode      IN           whether to decode current field
  @return
    0      OK
    other  HA_ERR error code (can be SE-specific)
*/
int Rdb_convert_to_record_value_decoder::decode_varchar(
    Field *field, Rdb_string_reader *const reader, bool decode) {
  my_core::Field_varstring *const field_var = (my_core::Field_varstring *)field;

  const char *data_len_str;
  if (!(data_len_str = reader->read(field_var->length_bytes))) {
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }

  uint data_len;
  // field_var->length_bytes is 1 or 2
  if (field_var->length_bytes == 1) {
    data_len = (uchar)data_len_str[0];
  } else {
    DBUG_ASSERT(field_var->length_bytes == 2);
    data_len = uint2korr(data_len_str);
  }

  if (data_len > field_var->field_length) {
    // The data on disk is longer than table DDL allows?
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }

  if (!reader->read(data_len)) {
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }

  if (decode) {
    memcpy(field_var->ptr, data_len_str, field_var->length_bytes + data_len);
  }

  return HA_EXIT_SUCCESS;
}

template <typename value_field_decoder>
Rdb_value_field_iterator<value_field_decoder>::Rdb_value_field_iterator(
    TABLE *table, Rdb_string_reader *value_slice_reader,
    const Rdb_converter *rdb_converter, uchar *const buf)
    : m_buf(buf) {
  DBUG_ASSERT(table != nullptr);
  DBUG_ASSERT(buf != nullptr);

  m_table = table;
  m_value_slice_reader = value_slice_reader;
  auto fields = rdb_converter->get_decode_fields();
  m_field_iter = fields->begin();
  m_field_end = fields->end();
  m_null_bytes = rdb_converter->get_null_bytes();
  m_offset = 0;
}

// Iterate each requested field and decode one by one
template <typename value_field_decoder>
int Rdb_value_field_iterator<value_field_decoder>::next() {
  int err = HA_EXIT_SUCCESS;
  while (m_field_iter != m_field_end) {
    m_field_dec = m_field_iter->m_field_enc;
    bool decode = m_field_iter->m_decode;
    bool maybe_null = m_field_dec->maybe_null();
    // This is_null value is bind to how stroage format store its value
    m_is_null = maybe_null && ((m_null_bytes[m_field_dec->m_null_offset] &
                                m_field_dec->m_null_mask) != 0);

    // Skip the bytes we need to skip
    int skip = m_field_iter->m_skip;
    if (skip && !m_value_slice_reader->read(skip)) {
      return HA_ERR_ROCKSDB_CORRUPT_DATA;
    }

    m_field = m_table->field[m_field_dec->m_field_index];
    // Decode each field
    err = value_field_decoder::decode(m_buf, &m_offset, m_table, m_field,
                                      m_field_dec, m_value_slice_reader, decode,
                                      m_is_null);
    if (err != HA_EXIT_SUCCESS) {
      return err;
    }
    m_field_iter++;
    // Only break for the field that are actually decoding rather than skipping
    if (decode) {
      break;
    }
  }
  return err;
}

template <typename value_field_decoder>
bool Rdb_value_field_iterator<value_field_decoder>::end_of_fields() const {
  return m_field_iter == m_field_end;
}

template <typename value_field_decoder>
Field *Rdb_value_field_iterator<value_field_decoder>::get_field() const {
  DBUG_ASSERT(m_field != nullptr);
  return m_field;
}

template <typename value_field_decoder>
void *Rdb_value_field_iterator<value_field_decoder>::get_dst() const {
  DBUG_ASSERT(m_buf != nullptr);
  return m_buf + m_offset;
}

template <typename value_field_decoder>
int Rdb_value_field_iterator<value_field_decoder>::get_field_index() const {
  DBUG_ASSERT(m_field_dec != nullptr);
  return m_field_dec->m_field_index;
}

template <typename value_field_decoder>
enum_field_types Rdb_value_field_iterator<value_field_decoder>::get_field_type()
    const {
  DBUG_ASSERT(m_field_dec != nullptr);
  return m_field_dec->m_field_type;
}

template <typename value_field_decoder>
bool Rdb_value_field_iterator<value_field_decoder>::is_null() const {
  DBUG_ASSERT(m_field != nullptr);
  return m_is_null;
}

/*
  Initialize Rdb_converter with table data
  @param    thd        IN      Thread context
  @param    tbl_def    IN      MyRocks table definition
  @param    table      IN      Current open table
*/
Rdb_converter::Rdb_converter(const THD *thd, const Rdb_tbl_def *tbl_def,
                             TABLE *table)
    : m_thd(thd), m_tbl_def(tbl_def), m_table(table) {
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(tbl_def != nullptr);
  DBUG_ASSERT(table != nullptr);

  m_key_requested = false;
  m_verify_row_debug_checksums = false;
  m_maybe_unpack_info = false;
  m_row_checksums_checked = 0;
  m_null_bytes = nullptr;
  setup_field_encoders();
}

Rdb_converter::~Rdb_converter() {
  my_free(m_encoder_arr);
  m_encoder_arr = nullptr;
  // These are needed to suppress valgrind errors in rocksdb.partition
  m_storage_record.free();
}

/*
  Decide storage type for each encoder
*/
void Rdb_converter::get_storage_type(Rdb_field_encoder *const encoder,
                                     const uint kp) {
  auto pk_descr =
      m_tbl_def->m_key_descr_arr[ha_rocksdb::pk_index(m_table, m_tbl_def)];
  // STORE_SOME uses unpack_info.
  if (pk_descr->has_unpack_info(kp)) {
    DBUG_ASSERT(pk_descr->can_unpack(kp));
    encoder->m_storage_type = Rdb_field_encoder::STORE_SOME;
    m_maybe_unpack_info = true;
  } else if (pk_descr->can_unpack(kp)) {
    encoder->m_storage_type = Rdb_field_encoder::STORE_NONE;
  }
}

/*
  @brief
    Setup which fields will be unpacked when reading rows

  @detail
    Three special cases when we still unpack all fields:
    - When client requires decode_all_fields, such as this table is being
  updated (m_lock_rows==RDB_LOCK_WRITE).
    - When @@rocksdb_verify_row_debug_checksums is ON (In this mode, we need to
  read all fields to find whether there is a row checksum at the end. We could
  skip the fields instead of decoding them, but currently we do decoding.)
    - On index merge as bitmap is cleared during that operation

  @seealso
    Rdb_converter::setup_field_encoders()
    Rdb_converter::convert_record_from_storage_format()
*/
void Rdb_converter::setup_field_decoders(const MY_BITMAP *field_map,
                                         bool decode_all_fields) {
  m_key_requested = false;
  m_decoders_vect.clear();
  int last_useful = 0;
  int skip_size = 0;

  for (uint i = 0; i < m_table->s->fields; i++) {
    // bitmap is cleared on index merge, but it still needs to decode columns
    bool field_requested =
        decode_all_fields || m_verify_row_debug_checksums ||
        bitmap_is_clear_all(field_map) ||
        bitmap_is_set(field_map, m_table->field[i]->field_index);

    // We only need the decoder if the whole record is stored.
    if (m_encoder_arr[i].m_storage_type != Rdb_field_encoder::STORE_ALL) {
      // the field potentially needs unpacking
      if (field_requested) {
        // the field is in the read set
        m_key_requested = true;
      }
      continue;
    }

    if (field_requested) {
      // We will need to decode this field
      m_decoders_vect.push_back({&m_encoder_arr[i], true, skip_size});
      last_useful = m_decoders_vect.size();
      skip_size = 0;
    } else {
      if (m_encoder_arr[i].uses_variable_len_encoding() ||
          m_encoder_arr[i].maybe_null()) {
        // For variable-length field, we need to read the data and skip it
        m_decoders_vect.push_back({&m_encoder_arr[i], false, skip_size});
        skip_size = 0;
      } else {
        // Fixed-width field can be skipped without looking at it.
        // Add appropriate skip_size to the next field.
        skip_size += m_encoder_arr[i].m_pack_length_in_rec;
      }
    }
  }

  // It could be that the last few elements are varchars that just do
  // skipping. Remove them.
  m_decoders_vect.erase(m_decoders_vect.begin() + last_useful,
                        m_decoders_vect.end());
}

void Rdb_converter::setup_field_encoders() {
  uint null_bytes_length = 0;
  uchar cur_null_mask = 0x1;

  m_encoder_arr = static_cast<Rdb_field_encoder *>(
      my_malloc(PSI_INSTRUMENT_ME, m_table->s->fields * sizeof(Rdb_field_encoder), MYF(0)));
  if (m_encoder_arr == nullptr) {
    return;
  }

  for (uint i = 0; i < m_table->s->fields; i++) {
    Field *const field = m_table->field[i];
    m_encoder_arr[i].m_storage_type = Rdb_field_encoder::STORE_ALL;

    /*
      Check if this field is
      - a part of primary key, and
      - it can be decoded back from its key image.
      If both hold, we don't need to store this field in the value part of
      RocksDB's key-value pair.

      If hidden pk exists, we skip this check since the field will never be
      part of the hidden pk.
    */
    if (!Rdb_key_def::table_has_hidden_pk(m_table)) {
      KEY *const pk_info = &m_table->key_info[m_table->s->primary_key];
      for (uint kp = 0; kp < pk_info->user_defined_key_parts; kp++) {
        // key_part->fieldnr is counted from 1
        if (field->field_index + 1 == pk_info->key_part[kp].fieldnr) {
          get_storage_type(&m_encoder_arr[i], kp);
          break;
        }
      }
    }

    m_encoder_arr[i].m_field_type = field->real_type();
    m_encoder_arr[i].m_field_index = i;
    m_encoder_arr[i].m_pack_length_in_rec = field->pack_length_in_rec();

    if (field->real_maybe_null()) {
      m_encoder_arr[i].m_null_mask = cur_null_mask;
      m_encoder_arr[i].m_null_offset = null_bytes_length;
      if (cur_null_mask == 0x80) {
        cur_null_mask = 0x1;
        null_bytes_length++;
      } else {
        cur_null_mask = cur_null_mask << 1;
      }
    } else {
      m_encoder_arr[i].m_null_mask = 0;
    }
  }

  // Count the last, unfinished NULL-bits byte
  if (cur_null_mask != 0x1) {
    null_bytes_length++;
  }

  m_null_bytes_length_in_record = null_bytes_length;
}

/*
  EntryPoint for Decode:
  Decode key slice(if requested) and value slice using built-in field
  decoders
  @param     key_def        IN          key definition to decode
  @param     dst            OUT         Mysql buffer to fill decoded content
  @param     key_slice      IN          RocksDB key slice to decode
  @param     value_slice    IN          RocksDB value slice to decode
  @return
    0      OK
    other  HA_ERR error code (can be SE-specific)
*/
int Rdb_converter::decode(const std::shared_ptr<Rdb_key_def> &key_def,
                          uchar *dst,  // address to fill data
                          const rocksdb::Slice *key_slice,
                          const rocksdb::Slice *value_slice) {
  // Currently only support decode primary key, Will add decode secondary later
  DBUG_ASSERT(key_def->m_index_type == Rdb_key_def::INDEX_TYPE_PRIMARY ||
              key_def->m_index_type == Rdb_key_def::INDEX_TYPE_HIDDEN_PRIMARY);

  const rocksdb::Slice *updated_key_slice = key_slice;
#ifndef DBUG_OFF
  String last_rowkey;
  last_rowkey.copy(key_slice->data(), key_slice->size(), &my_charset_bin);
  DBUG_EXECUTE_IF("myrocks_simulate_bad_pk_read1",
                  { dbug_modify_key_varchar8(&last_rowkey); });
  rocksdb::Slice rowkey_slice(last_rowkey.ptr(), last_rowkey.length());
  updated_key_slice = &rowkey_slice;
#endif
  return convert_record_from_storage_format(key_def, updated_key_slice,
                                            value_slice, dst);
}

/*
  Decode value slice header
  @param    reader         IN          value slice reader
  @param    pk_def         IN          key definition to decode
  @param    unpack_slice   OUT         unpack info slice
  @return
    0      OK
    other  HA_ERR error code (can be SE-specific)
*/
int Rdb_converter::decode_value_header(
    Rdb_string_reader *reader, const std::shared_ptr<Rdb_key_def> &pk_def,
    rocksdb::Slice *unpack_slice) {
  /* If it's a TTL record, skip the 8 byte TTL value */
  if (pk_def->has_ttl()) {
    const char *ttl_bytes;
    if ((ttl_bytes = reader->read(ROCKSDB_SIZEOF_TTL_RECORD))) {
      memcpy(m_ttl_bytes, ttl_bytes, ROCKSDB_SIZEOF_TTL_RECORD);
    } else {
      return HA_ERR_ROCKSDB_CORRUPT_DATA;
    }
  }

  /* Other fields are decoded from the value */
  if (m_null_bytes_length_in_record &&
      !(m_null_bytes = reader->read(m_null_bytes_length_in_record))) {
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }

  if (m_maybe_unpack_info) {
    const char *unpack_info = reader->get_current_ptr();
    if (!unpack_info || !Rdb_key_def::is_unpack_data_tag(unpack_info[0]) ||
        !reader->read(Rdb_key_def::get_unpack_header_size(unpack_info[0]))) {
      return HA_ERR_ROCKSDB_CORRUPT_DATA;
    }

    uint16 unpack_info_len =
        rdb_netbuf_to_uint16(reinterpret_cast<const uchar *>(unpack_info + 1));
    *unpack_slice = rocksdb::Slice(unpack_info, unpack_info_len);

    reader->read(unpack_info_len -
                 Rdb_key_def::get_unpack_header_size(unpack_info[0]));
  }

  return HA_EXIT_SUCCESS;
}

/*
  Convert RocksDb key slice and value slice to Mysql format
  @param      key_def        IN           key definition to decode
  @param      key_slice      IN           RocksDB key slice
  @param      value_slice    IN           RocksDB value slice
  @param      dst            OUT          MySql format address
  @return
    0      OK
    other  HA_ERR error code (can be SE-specific)
*/
int Rdb_converter::convert_record_from_storage_format(
    const std::shared_ptr<Rdb_key_def> &pk_def,
    const rocksdb::Slice *const key_slice,
    const rocksdb::Slice *const value_slice, uchar *const dst) {
  int err = HA_EXIT_SUCCESS;

  Rdb_string_reader value_slice_reader(value_slice);
  rocksdb::Slice unpack_slice;
  err = decode_value_header(&value_slice_reader, pk_def, &unpack_slice);
  if (err != HA_EXIT_SUCCESS) {
    return err;
  }

  /*
    Decode PK fields from the key
  */
  if (m_key_requested) {
    err = pk_def->unpack_record(m_table, dst, key_slice,
                                !unpack_slice.empty() ? &unpack_slice : nullptr,
                                false /* verify_checksum */);
  }
  if (err != HA_EXIT_SUCCESS) {
    return err;
  }

  Rdb_value_field_iterator<Rdb_convert_to_record_value_decoder>
      value_field_iterator(m_table, &value_slice_reader, this, dst);

  // Decode value slices
  while (!value_field_iterator.end_of_fields()) {
    err = value_field_iterator.next();

    if (err != HA_EXIT_SUCCESS) {
      return err;
    }
  }

  if (m_verify_row_debug_checksums) {
    return verify_row_debug_checksum(pk_def, &value_slice_reader, key_slice,
                                     value_slice);
  }
  return HA_EXIT_SUCCESS;
}

/*
  Verify checksum for row
  @param      pk_def   IN     key def
  @param      reader   IN     RocksDB value slice reader
  @param      key      IN     RocksDB key slice
  @param      value    IN     RocksDB value slice
  @return
    0      OK
    other  HA_ERR error code (can be SE-specific)
*/
int Rdb_converter::verify_row_debug_checksum(
    const std::shared_ptr<Rdb_key_def> &pk_def, Rdb_string_reader *reader,
    const rocksdb::Slice *key, const rocksdb::Slice *value) {
  if (reader->remaining_bytes() == RDB_CHECKSUM_CHUNK_SIZE &&
      reader->read(1)[0] == RDB_CHECKSUM_DATA_TAG) {
    uint32_t stored_key_chksum =
        rdb_netbuf_to_uint32((const uchar *)reader->read(RDB_CHECKSUM_SIZE));
    uint32_t stored_val_chksum =
        rdb_netbuf_to_uint32((const uchar *)reader->read(RDB_CHECKSUM_SIZE));

    const uint32_t computed_key_chksum =
        my_core::my_checksum(0, rdb_slice_to_uchar_ptr(key), key->size());
    const uint32_t computed_val_chksum =
        my_core::my_checksum(0, rdb_slice_to_uchar_ptr(value),
                       value->size() - RDB_CHECKSUM_CHUNK_SIZE);

    DBUG_EXECUTE_IF("myrocks_simulate_bad_pk_checksum1", stored_key_chksum++;);

    if (stored_key_chksum != computed_key_chksum) {
      pk_def->report_checksum_mismatch(true, key->data(), key->size());
      return HA_ERR_ROCKSDB_CHECKSUM_MISMATCH;
    }

    DBUG_EXECUTE_IF("myrocks_simulate_bad_pk_checksum2", stored_val_chksum++;);
    if (stored_val_chksum != computed_val_chksum) {
      pk_def->report_checksum_mismatch(false, value->data(), value->size());
      return HA_ERR_ROCKSDB_CHECKSUM_MISMATCH;
    }

    m_row_checksums_checked++;
  }
  if (reader->remaining_bytes()) {
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }
  return HA_EXIT_SUCCESS;
}

/**
  Convert record from table->record[0] form into a form that can be written
  into rocksdb.

  @param pk_def               IN        Current key def
  @pk_unpack_info             IN        Unpack info generated during key pack
  @is_update_row              IN        Whether it is update row
  @store_row_debug_checksums  IN        Whether to store checksums
  @param ttl_bytes            IN/OUT    Old ttl value from previous record and
                                        ttl value during current encode
  @is_ttl_bytes_updated       OUT       Whether ttl bytes is updated
  @param value_slice          OUT       Data slice with record data.
*/
int Rdb_converter::encode_value_slice(
    const std::shared_ptr<Rdb_key_def> &pk_def,
    const rocksdb::Slice &pk_packed_slice, Rdb_string_writer *pk_unpack_info,
    bool is_update_row, bool store_row_debug_checksums, char *ttl_bytes,
    bool *is_ttl_bytes_updated, rocksdb::Slice *const value_slice) {
  DBUG_ASSERT(pk_def != nullptr);
  // Currently only primary key will store value slice
  DBUG_ASSERT(pk_def->m_index_type == Rdb_key_def::INDEX_TYPE_PRIMARY ||
              pk_def->m_index_type == Rdb_key_def::INDEX_TYPE_HIDDEN_PRIMARY);
  DBUG_ASSERT_IMP(m_maybe_unpack_info, pk_unpack_info);

  bool has_ttl = pk_def->has_ttl();
  bool has_ttl_column = !pk_def->m_ttl_column.empty();

  m_storage_record.length(0);

  if (has_ttl) {
    /* If it's a TTL record, reserve space for 8 byte TTL value in front. */
    m_storage_record.fill(
        ROCKSDB_SIZEOF_TTL_RECORD + m_null_bytes_length_in_record, 0);
    // NOTE: is_ttl_bytes_updated is only used for update case
    // During update, skip update sk key/values slice iff none of sk fields
    // have changed and ttl bytes isn't changed. see
    // ha_rocksdb::update_write_sk() for more info
    *is_ttl_bytes_updated = false;
    char *const data = const_cast<char *>(m_storage_record.ptr());
    if (has_ttl_column) {
      DBUG_ASSERT(pk_def->get_ttl_field_index() != UINT_MAX);
      Field *const field = m_table->field[pk_def->get_ttl_field_index()];
      DBUG_ASSERT(field->pack_length_in_rec() == ROCKSDB_SIZEOF_TTL_RECORD);
      DBUG_ASSERT(field->real_type() == MYSQL_TYPE_LONGLONG);

      uint64 ts = uint8korr(field->ptr);
#ifndef DBUG_OFF
      ts += rdb_dbug_set_ttl_rec_ts();
#endif
      rdb_netbuf_store_uint64(reinterpret_cast<uchar *>(data), ts);
      if (is_update_row) {
        *is_ttl_bytes_updated =
            memcmp(ttl_bytes, data, ROCKSDB_SIZEOF_TTL_RECORD);
      }
      // Also store in m_ttl_bytes to propagate to update_write_sk
      memcpy(ttl_bytes, data, ROCKSDB_SIZEOF_TTL_RECORD);
    } else {
      /*
        For implicitly generated TTL records we need to copy over the old
        TTL value from the old record in the event of an update. It was stored
        in m_ttl_bytes.

        Otherwise, generate a timestamp using the current time.
      */
      if (is_update_row) {
        memcpy(data, ttl_bytes, sizeof(uint64));
      } else {
        uint64 ts = static_cast<uint64>(std::time(nullptr));
#ifndef DBUG_OFF
        ts += rdb_dbug_set_ttl_rec_ts();
#endif
        rdb_netbuf_store_uint64(reinterpret_cast<uchar *>(data), ts);
        // Also store in m_ttl_bytes to propagate to update_write_sk
        memcpy(ttl_bytes, data, ROCKSDB_SIZEOF_TTL_RECORD);
      }
    }
  } else {
    /* All NULL bits are initially 0 */
    m_storage_record.fill(m_null_bytes_length_in_record, 0);
  }

  // If a primary key may have non-empty unpack_info for certain values,
  // (m_maybe_unpack_info=TRUE), we write the unpack_info block. The block
  // itself was prepared in Rdb_key_def::pack_record.
  if (m_maybe_unpack_info) {
    m_storage_record.append(reinterpret_cast<char *>(pk_unpack_info->ptr()),
                            pk_unpack_info->get_current_pos());
  }
  for (uint i = 0; i < m_table->s->fields; i++) {
    Rdb_field_encoder &encoder = m_encoder_arr[i];
    /* Don't pack decodable PK key parts */
    if (encoder.m_storage_type != Rdb_field_encoder::STORE_ALL) {
      continue;
    }

    Field *const field = m_table->field[i];
    if (encoder.maybe_null()) {
      char *data = const_cast<char *>(m_storage_record.ptr());
      if (has_ttl) {
        data += ROCKSDB_SIZEOF_TTL_RECORD;
      }

      if (field->is_null()) {
        data[encoder.m_null_offset] |= encoder.m_null_mask;
        /* Don't write anything for NULL values */
        continue;
      }
    }

    if (encoder.m_field_type == MYSQL_TYPE_BLOB) {
      my_core::Field_blob *blob =
          reinterpret_cast<my_core::Field_blob *>(field);
      /* Get the number of bytes needed to store length*/
      const uint length_bytes = blob->pack_length() - portable_sizeof_char_ptr;

      /* Store the length of the value */
      m_storage_record.append(reinterpret_cast<char *>(blob->ptr),
                              length_bytes);

      /* Store the blob value itself */
      char *data_ptr;
      memcpy(&data_ptr, blob->ptr + length_bytes, sizeof(uchar **));
      m_storage_record.append(data_ptr, blob->get_length());
    } else if (encoder.m_field_type == MYSQL_TYPE_VARCHAR) {
      Field_varstring *const field_var =
          reinterpret_cast<Field_varstring *>(field);
      uint data_len;
      /* field_var->length_bytes is 1 or 2 */
      if (field_var->length_bytes == 1) {
        data_len = field_var->ptr[0];
      } else {
        DBUG_ASSERT(field_var->length_bytes == 2);
        data_len = uint2korr(field_var->ptr);
      }
      m_storage_record.append(reinterpret_cast<char *>(field_var->ptr),
                              field_var->length_bytes + data_len);
    } else {
      /* Copy the field data */
      const uint len = field->pack_length_in_rec();
      m_storage_record.append(reinterpret_cast<char *>(field->ptr), len);
    }
  }

  if (store_row_debug_checksums) {
    const uint32_t key_crc32 = my_core::my_checksum(
        0, rdb_slice_to_uchar_ptr(&pk_packed_slice), pk_packed_slice.size());
    const uint32_t val_crc32 =
        my_core::my_checksum(0, rdb_mysql_str_to_uchar_str(&m_storage_record),
                       m_storage_record.length());
    uchar key_crc_buf[RDB_CHECKSUM_SIZE];
    uchar val_crc_buf[RDB_CHECKSUM_SIZE];
    rdb_netbuf_store_uint32(key_crc_buf, key_crc32);
    rdb_netbuf_store_uint32(val_crc_buf, val_crc32);
    m_storage_record.append((const char *)&RDB_CHECKSUM_DATA_TAG, 1);
    m_storage_record.append((const char *)key_crc_buf, RDB_CHECKSUM_SIZE);
    m_storage_record.append((const char *)val_crc_buf, RDB_CHECKSUM_SIZE);
  }

  *value_slice =
      rocksdb::Slice(m_storage_record.ptr(), m_storage_record.length());

  return HA_EXIT_SUCCESS;
}
}  // namespace myrocks
