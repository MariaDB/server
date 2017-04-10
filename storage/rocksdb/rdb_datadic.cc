/*
   Copyright (c) 2012,2013 Monty Program Ab

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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation // gcc: Class implementation
#endif

/* For use of 'PRIu64': */
#define __STDC_FORMAT_MACROS

#include <my_config.h>
/* This C++ file's header file */
#include "./rdb_datadic.h"

#include <inttypes.h>
/* C++ standard header files */
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

/* MySQL header files */
#include "./key.h"
#include "./m_ctype.h"
#include "./my_bit.h"

/* MyRocks header files */
#include "./ha_rocksdb_proto.h"
#include "./my_stacktrace.h"
#include "./rdb_cf_manager.h"
#include "./rdb_utils.h"

namespace myrocks {

void get_mem_comparable_space(const CHARSET_INFO *cs,
                              const std::vector<uchar> **xfrm, size_t *xfrm_len,
                              size_t *mb_len);

/*
  Rdb_key_def class implementation
*/

Rdb_key_def::Rdb_key_def(uint indexnr_arg, uint keyno_arg,
                         rocksdb::ColumnFamilyHandle *cf_handle_arg,
                         uint16_t index_dict_version_arg, uchar index_type_arg,
                         uint16_t kv_format_version_arg, bool is_reverse_cf_arg,
                         bool is_auto_cf_arg, bool is_per_partition_cf_arg,
                         const char *_name, Rdb_index_stats _stats)
    : m_index_number(indexnr_arg), m_cf_handle(cf_handle_arg),
      m_index_dict_version(index_dict_version_arg),
      m_index_type(index_type_arg), m_kv_format_version(kv_format_version_arg),
      m_is_reverse_cf(is_reverse_cf_arg), m_is_auto_cf(is_auto_cf_arg),
      m_is_per_partition_cf(is_per_partition_cf_arg),
      m_name(_name), m_stats(_stats), m_pk_part_no(nullptr),
      m_pack_info(nullptr), m_keyno(keyno_arg), m_key_parts(0),
      m_prefix_extractor(nullptr), m_maxlength(0) // means 'not intialized'
{
  mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
  rdb_netbuf_store_index(m_index_number_storage_form, m_index_number);
  DBUG_ASSERT(m_cf_handle != nullptr);
}

Rdb_key_def::Rdb_key_def(const Rdb_key_def &k)
    : m_index_number(k.m_index_number), m_cf_handle(k.m_cf_handle),
      m_is_reverse_cf(k.m_is_reverse_cf), m_is_auto_cf(k.m_is_auto_cf),
      m_is_per_partition_cf(k.m_is_per_partition_cf),
      m_name(k.m_name), m_stats(k.m_stats), m_pk_part_no(k.m_pk_part_no),
      m_pack_info(k.m_pack_info), m_keyno(k.m_keyno),
      m_key_parts(k.m_key_parts), m_prefix_extractor(k.m_prefix_extractor),
      m_maxlength(k.m_maxlength) {
  mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
  rdb_netbuf_store_index(m_index_number_storage_form, m_index_number);
  if (k.m_pack_info) {
    const size_t size = sizeof(Rdb_field_packing) * k.m_key_parts;
    m_pack_info =
        reinterpret_cast<Rdb_field_packing *>(my_malloc(size, MYF(0)));
    memcpy(m_pack_info, k.m_pack_info, size);
  }

  if (k.m_pk_part_no) {
    const size_t size = sizeof(uint) * m_key_parts;
    m_pk_part_no = reinterpret_cast<uint *>(my_malloc(size, MYF(0)));
    memcpy(m_pk_part_no, k.m_pk_part_no, size);
  }
}

Rdb_key_def::~Rdb_key_def() {
  mysql_mutex_destroy(&m_mutex);

  my_free(m_pk_part_no);
  m_pk_part_no = nullptr;

  my_free(m_pack_info);
  m_pack_info = nullptr;
}

void Rdb_key_def::setup(const TABLE *const tbl,
                        const Rdb_tbl_def *const tbl_def) {
  DBUG_ASSERT(tbl != nullptr);
  DBUG_ASSERT(tbl_def != nullptr);

  /*
    Set max_length based on the table.  This can be called concurrently from
    multiple threads, so there is a mutex to protect this code.
  */
  const bool is_hidden_pk = (m_index_type == INDEX_TYPE_HIDDEN_PRIMARY);
  const bool hidden_pk_exists = table_has_hidden_pk(tbl);
  const bool secondary_key = (m_index_type == INDEX_TYPE_SECONDARY);
  if (!m_maxlength) {
    RDB_MUTEX_LOCK_CHECK(m_mutex);
    if (m_maxlength != 0) {
      RDB_MUTEX_UNLOCK_CHECK(m_mutex);
      return;
    }

    KEY *key_info = nullptr;
    KEY *pk_info = nullptr;
    if (!is_hidden_pk) {
      key_info = &tbl->key_info[m_keyno];
      if (!hidden_pk_exists)
        pk_info = &tbl->key_info[tbl->s->primary_key];
      m_name = std::string(key_info->name);
    } else {
      m_name = HIDDEN_PK_NAME;
    }

    if (secondary_key)
      m_pk_key_parts= hidden_pk_exists ? 1 : pk_info->ext_key_parts;
    else {
      pk_info = nullptr;
      m_pk_key_parts = 0;
    }

    // "unique" secondary keys support:
    m_key_parts= is_hidden_pk ? 1 : key_info->ext_key_parts;

    if (secondary_key) {
      /*
        In most cases, SQL layer puts PK columns as invisible suffix at the
        end of secondary key. There are cases where this doesn't happen:
        - unique secondary indexes.
        - partitioned tables.

        Internally, we always need PK columns as suffix (and InnoDB does,
        too, if you were wondering).

        The loop below will attempt to put all PK columns at the end of key
        definition.  Columns that are already included in the index (either
        by the user or by "extended keys" feature) are not included for the
        second time.
      */
      m_key_parts += m_pk_key_parts;
    }

    if (secondary_key)
      m_pk_part_no = reinterpret_cast<uint *>(
          my_malloc(sizeof(uint) * m_key_parts, MYF(0)));
    else
      m_pk_part_no = nullptr;

    const size_t size = sizeof(Rdb_field_packing) * m_key_parts;
    m_pack_info =
        reinterpret_cast<Rdb_field_packing *>(my_malloc(size, MYF(0)));

    size_t max_len = INDEX_NUMBER_SIZE;
    int unpack_len = 0;
    int max_part_len = 0;
    bool simulating_extkey = false;
    uint dst_i = 0;

    uint keyno_to_set = m_keyno;
    uint keypart_to_set = 0;

    if (is_hidden_pk) {
      Field *field = nullptr;
      m_pack_info[dst_i].setup(this, field, keyno_to_set, 0, 0);
      m_pack_info[dst_i].m_unpack_data_offset = unpack_len;
      max_len += m_pack_info[dst_i].m_max_image_len;
      max_part_len = std::max(max_part_len, m_pack_info[dst_i].m_max_image_len);
      dst_i++;
    } else {
      KEY_PART_INFO *key_part = key_info->key_part;

      /* this loop also loops over the 'extended key' tail */
      for (uint src_i = 0; src_i < m_key_parts; src_i++, keypart_to_set++) {
        Field *const field = key_part ? key_part->field : nullptr;

        if (simulating_extkey && !hidden_pk_exists) {
          DBUG_ASSERT(secondary_key);
          /* Check if this field is already present in the key definition */
          bool found = false;
          for (uint j= 0; j < key_info->ext_key_parts; j++) {
            if (field->field_index ==
                    key_info->key_part[j].field->field_index &&
                key_part->length == key_info->key_part[j].length) {
              found = true;
              break;
            }
          }

          if (found) {
            key_part++;
            continue;
          }
        }

        if (field && field->real_maybe_null())
          max_len += 1; // NULL-byte

        m_pack_info[dst_i].setup(this, field, keyno_to_set, keypart_to_set,
                                 key_part ? key_part->length : 0);
        m_pack_info[dst_i].m_unpack_data_offset = unpack_len;

        if (pk_info) {
          m_pk_part_no[dst_i] = -1;
          for (uint j = 0; j < m_pk_key_parts; j++) {
            if (field->field_index == pk_info->key_part[j].field->field_index) {
              m_pk_part_no[dst_i] = j;
              break;
            }
          }
        } else if (secondary_key && hidden_pk_exists) {
          /*
            The hidden pk can never be part of the sk.  So it is always
            appended to the end of the sk.
          */
          m_pk_part_no[dst_i] = -1;
          if (simulating_extkey)
            m_pk_part_no[dst_i] = 0;
        }

        max_len += m_pack_info[dst_i].m_max_image_len;

        max_part_len =
            std::max(max_part_len, m_pack_info[dst_i].m_max_image_len);

        key_part++;
        /*
          For "unique" secondary indexes, pretend they have
          "index extensions".

          MariaDB also has this property: if an index has a partially-covered
          column like KEY(varchar_col(N)), then the SQL layer will think it is
          not "extended" with PK columns. The code below handles this case,
          also.
         */
        if (secondary_key && src_i+1 == key_info->ext_key_parts) {
          simulating_extkey = true;
          if (!hidden_pk_exists) {
            keyno_to_set = tbl->s->primary_key;
            key_part = pk_info->key_part;
            keypart_to_set = (uint)-1;
          } else {
            keyno_to_set = tbl_def->m_key_count - 1;
            key_part = nullptr;
            keypart_to_set = 0;
          }
        }

        dst_i++;
      }
    }

    m_key_parts = dst_i;

    /* Initialize the memory needed by the stats structure */
    m_stats.m_distinct_keys_per_prefix.resize(get_key_parts());

    /* Cache prefix extractor for bloom filter usage later */
    rocksdb::Options opt = rdb_get_rocksdb_db()->GetOptions(get_cf());
    m_prefix_extractor = opt.prefix_extractor;

    /*
      This should be the last member variable set before releasing the mutex
      so that other threads can't see the object partially set up.
     */
    m_maxlength = max_len;

    RDB_MUTEX_UNLOCK_CHECK(m_mutex);
  }
}

/**
  Read a memcmp key part from a slice using the passed in reader.

  Returns -1 if field was null, 1 if error, 0 otherwise.
*/
int Rdb_key_def::read_memcmp_key_part(const TABLE *table_arg,
                                      Rdb_string_reader *reader,
                                      const uint part_num) const {
  /* It is impossible to unpack the column. Skip it. */
  if (m_pack_info[part_num].m_maybe_null) {
    const char *nullp;
    if (!(nullp = reader->read(1)))
      return 1;
    if (*nullp == 0) {
      /* This is a NULL value */
      return -1;
    } else {
      /* If NULL marker is not '0', it can be only '1'  */
      if (*nullp != 1)
        return 1;
    }
  }

  Rdb_field_packing *fpi = &m_pack_info[part_num];
  DBUG_ASSERT(table_arg->s != nullptr);

  bool is_hidden_pk_part = (part_num + 1 == m_key_parts) &&
                           (table_arg->s->primary_key == MAX_INDEXES);
  Field *field = nullptr;
  if (!is_hidden_pk_part)
    field = fpi->get_field_in_table(table_arg);
  if (fpi->m_skip_func(fpi, field, reader))
    return 1;

  return 0;
}

/**
  Get a mem-comparable form of Primary Key from mem-comparable form of this key

  @param
    pk_descr        Primary Key descriptor
    key             Index tuple from this key in mem-comparable form
    pk_buffer  OUT  Put here mem-comparable form of the Primary Key.

  @note
    It may or may not be possible to restore primary key columns to their
    mem-comparable form.  To handle all cases, this function copies mem-
    comparable forms directly.

    RocksDB SE supports "Extended keys". This means that PK columns are present
    at the end of every key.  If the key already includes PK columns, then
    these columns are not present at the end of the key.

    Because of the above, we copy each primary key column.

  @todo
    If we checked crc32 checksums in this function, we would catch some CRC
    violations that we currently don't. On the other hand, there is a broader
    set of queries for which we would check the checksum twice.
*/

uint Rdb_key_def::get_primary_key_tuple(const TABLE *const table,
                                        const Rdb_key_def &pk_descr,
                                        const rocksdb::Slice *const key,
                                        uchar *const pk_buffer) const {
  DBUG_ASSERT(table != nullptr);
  DBUG_ASSERT(key != nullptr);
  DBUG_ASSERT(pk_buffer);

  uint size = 0;
  uchar *buf = pk_buffer;
  DBUG_ASSERT(m_pk_key_parts);

  /* Put the PK number */
  rdb_netbuf_store_index(buf, pk_descr.m_index_number);
  buf += INDEX_NUMBER_SIZE;
  size += INDEX_NUMBER_SIZE;

  const char *start_offs[MAX_REF_PARTS];
  const char *end_offs[MAX_REF_PARTS];
  int pk_key_part;
  uint i;
  Rdb_string_reader reader(key);

  // Skip the index number
  if ((!reader.read(INDEX_NUMBER_SIZE)))
    return RDB_INVALID_KEY_LEN;

  for (i = 0; i < m_key_parts; i++) {
    if ((pk_key_part = m_pk_part_no[i]) != -1) {
      start_offs[pk_key_part] = reader.get_current_ptr();
    }

    if (read_memcmp_key_part(table, &reader, i) > 0) {
      return RDB_INVALID_KEY_LEN;
    }

    if (pk_key_part != -1) {
      end_offs[pk_key_part] = reader.get_current_ptr();
    }
  }

  for (i = 0; i < m_pk_key_parts; i++) {
    const uint part_size = end_offs[i] - start_offs[i];
    memcpy(buf, start_offs[i], end_offs[i] - start_offs[i]);
    buf += part_size;
    size += part_size;
  }

  return size;
}

/**
  Get a mem-comparable form of Secondary Key from mem-comparable form of this
  key, without the extended primary key tail.

  @param
    key                Index tuple from this key in mem-comparable form
    sk_buffer     OUT  Put here mem-comparable form of the Secondary Key.
    n_null_fields OUT  Put number of null fields contained within sk entry
*/
uint Rdb_key_def::get_memcmp_sk_parts(const TABLE *table,
                                      const rocksdb::Slice &key,
                                      uchar *sk_buffer,
                                      uint *n_null_fields) const {
  DBUG_ASSERT(table != nullptr);
  DBUG_ASSERT(sk_buffer != nullptr);
  DBUG_ASSERT(n_null_fields != nullptr);
  DBUG_ASSERT(m_keyno != table->s->primary_key && !table_has_hidden_pk(table));

  uchar *buf = sk_buffer;

  int res;
  Rdb_string_reader reader(&key);
  const char *start = reader.get_current_ptr();

  // Skip the index number
  if ((!reader.read(INDEX_NUMBER_SIZE)))
    return RDB_INVALID_KEY_LEN;

  for (uint i = 0; i < table->key_info[m_keyno].user_defined_key_parts; i++) {
    if ((res = read_memcmp_key_part(table, &reader, i)) > 0) {
      return RDB_INVALID_KEY_LEN;
    } else if (res == -1) {
      (*n_null_fields)++;
    }
  }

  uint sk_memcmp_len = reader.get_current_ptr() - start;
  memcpy(buf, start, sk_memcmp_len);
  return sk_memcmp_len;
}

/**
  Convert index tuple into storage (i.e. mem-comparable) format

  @detail
    Currently this is done by unpacking into table->record[0] and then
    packing index columns into storage format.

  @param pack_buffer Temporary area for packing varchar columns. Its
                     size is at least max_storage_fmt_length() bytes.
*/

uint Rdb_key_def::pack_index_tuple(TABLE *const tbl, uchar *const pack_buffer,
                                   uchar *const packed_tuple,
                                   const uchar *const key_tuple,
                                   const key_part_map &keypart_map) const {
  DBUG_ASSERT(tbl != nullptr);
  DBUG_ASSERT(pack_buffer != nullptr);
  DBUG_ASSERT(packed_tuple != nullptr);
  DBUG_ASSERT(key_tuple != nullptr);

  /* We were given a record in KeyTupleFormat. First, save it to record */
  const uint key_len = calculate_key_len(tbl, m_keyno, key_tuple, keypart_map);
  key_restore(tbl->record[0], key_tuple, &tbl->key_info[m_keyno], key_len);

  uint n_used_parts = my_count_bits(keypart_map);
  if (keypart_map == HA_WHOLE_KEY)
    n_used_parts = 0; // Full key is used

  /* Then, convert the record into a mem-comparable form */
  return pack_record(tbl, pack_buffer, tbl->record[0], packed_tuple, nullptr,
                     false, 0, n_used_parts);
}

/**
  @brief
    Check if "unpack info" data includes checksum.

  @detail
    This is used only by CHECK TABLE to count the number of rows that have
    checksums.
*/

bool Rdb_key_def::unpack_info_has_checksum(const rocksdb::Slice &unpack_info) {
  const uchar *ptr = (const uchar *)unpack_info.data();
  size_t size = unpack_info.size();

  // Skip unpack info if present.
  if (size >= RDB_UNPACK_HEADER_SIZE && ptr[0] == RDB_UNPACK_DATA_TAG) {
    const uint16 skip_len = rdb_netbuf_to_uint16(ptr + 1);
    SHIP_ASSERT(size >= skip_len);

    size -= skip_len;
    ptr += skip_len;
  }

  return (size == RDB_CHECKSUM_CHUNK_SIZE && ptr[0] == RDB_CHECKSUM_DATA_TAG);
}

/*
  @return Number of bytes that were changed
*/
int Rdb_key_def::successor(uchar *const packed_tuple, const uint &len) {
  DBUG_ASSERT(packed_tuple != nullptr);

  int changed = 0;
  uchar *p = packed_tuple + len - 1;
  for (; p > packed_tuple; p--) {
    changed++;
    if (*p != uchar(0xFF)) {
      *p = *p + 1;
      break;
    }
    *p = '\0';
  }
  return changed;
}

uchar *Rdb_key_def::pack_field(
  Field *const             field,
  Rdb_field_packing       *pack_info,
  uchar *                  tuple,
  uchar *const             packed_tuple,
  uchar *const             pack_buffer,
  Rdb_string_writer *const unpack_info,
  uint *const              n_null_fields) const
{
  if (field->real_maybe_null()) {
    DBUG_ASSERT(is_storage_available(tuple - packed_tuple, 1));
    if (field->is_real_null()) {
      /* NULL value. store '\0' so that it sorts before non-NULL values */
      *tuple++ = 0;
      /* That's it, don't store anything else */
      if (n_null_fields)
        (*n_null_fields)++;
      return tuple;
    } else {
      /* Not a NULL value. Store '1' */
      *tuple++ = 1;
    }
  }

  const bool create_unpack_info =
      (unpack_info &&  // we were requested to generate unpack_info
       pack_info->uses_unpack_info());  // and this keypart uses it
  Rdb_pack_field_context pack_ctx(unpack_info);

  // Set the offset for methods which do not take an offset as an argument
  DBUG_ASSERT(is_storage_available(tuple - packed_tuple,
                                   pack_info->m_max_image_len));

  pack_info->m_pack_func(pack_info, field, pack_buffer, &tuple, &pack_ctx);

  /* Make "unpack info" to be stored in the value */
  if (create_unpack_info) {
    pack_info->m_make_unpack_info_func(pack_info->m_charset_codec, field,
                                       &pack_ctx);
  }

  return tuple;
}

/**
  Get index columns from the record and pack them into mem-comparable form.

  @param
    tbl                   Table we're working on
    record           IN   Record buffer with fields in table->record format
    pack_buffer      IN   Temporary area for packing varchars. The size is
                          at least max_storage_fmt_length() bytes.
    packed_tuple     OUT  Key in the mem-comparable form
    unpack_info      OUT  Unpack data
    unpack_info_len  OUT  Unpack data length
    n_key_parts           Number of keyparts to process. 0 means all of them.
    n_null_fields    OUT  Number of key fields with NULL value.

  @detail
    Some callers do not need the unpack information, they can pass
    unpack_info=nullptr, unpack_info_len=nullptr.

  @return
    Length of the packed tuple
*/

uint Rdb_key_def::pack_record(const TABLE *const tbl, uchar *const pack_buffer,
                              const uchar *const record,
                              uchar *const packed_tuple,
                              Rdb_string_writer *const unpack_info,
                              const bool &should_store_row_debug_checksums,
                              const longlong &hidden_pk_id, uint n_key_parts,
                              uint *const n_null_fields) const {
  DBUG_ASSERT(tbl != nullptr);
  DBUG_ASSERT(pack_buffer != nullptr);
  DBUG_ASSERT(record != nullptr);
  DBUG_ASSERT(packed_tuple != nullptr);
  // Checksums for PKs are made when record is packed.
  // We should never attempt to make checksum just from PK values
  DBUG_ASSERT_IMP(should_store_row_debug_checksums,
                  (m_index_type == INDEX_TYPE_SECONDARY));

  uchar *tuple = packed_tuple;
  size_t unpack_len_pos = size_t(-1);
  const bool hidden_pk_exists = table_has_hidden_pk(tbl);

  rdb_netbuf_store_index(tuple, m_index_number);
  tuple += INDEX_NUMBER_SIZE;

  // If n_key_parts is 0, it means all columns.
  // The following includes the 'extended key' tail.
  // The 'extended key' includes primary key. This is done to 'uniqify'
  // non-unique indexes
  const bool use_all_columns = n_key_parts == 0 || n_key_parts == MAX_REF_PARTS;

  // If hidden pk exists, but hidden pk wasnt passed in, we can't pack the
  // hidden key part.  So we skip it (its always 1 part).
  if (hidden_pk_exists && !hidden_pk_id && use_all_columns)
    n_key_parts = m_key_parts - 1;
  else if (use_all_columns)
    n_key_parts = m_key_parts;

  if (n_null_fields)
    *n_null_fields = 0;

  if (unpack_info) {
    unpack_info->clear();
    unpack_info->write_uint8(RDB_UNPACK_DATA_TAG);
    unpack_len_pos = unpack_info->get_current_pos();
    // we don't know the total length yet, so write a zero
    unpack_info->write_uint16(0);
  }

  for (uint i = 0; i < n_key_parts; i++) {
    // Fill hidden pk id into the last key part for secondary keys for tables
    // with no pk
    if (hidden_pk_exists && hidden_pk_id && i + 1 == n_key_parts) {
      m_pack_info[i].fill_hidden_pk_val(&tuple, hidden_pk_id);
      break;
    }

    Field *const field = m_pack_info[i].get_field_in_table(tbl);
    DBUG_ASSERT(field != nullptr);

    uint field_offset = field->ptr - tbl->record[0];
    uint null_offset = field->null_offset(tbl->record[0]);
    bool maybe_null = field->real_maybe_null();
    field->move_field(const_cast<uchar*>(record) + field_offset,
        maybe_null ? const_cast<uchar*>(record) + null_offset : nullptr,
        field->null_bit);
    // WARNING! Don't return without restoring field->ptr and field->null_ptr

    tuple = pack_field(field, &m_pack_info[i], tuple, packed_tuple, pack_buffer,
                       unpack_info, n_null_fields);

    // Restore field->ptr and field->null_ptr
    field->move_field(tbl->record[0] + field_offset,
                      maybe_null ? tbl->record[0] + null_offset : nullptr,
                      field->null_bit);
  }

  if (unpack_info) {
    const size_t len = unpack_info->get_current_pos();
    DBUG_ASSERT(len <= std::numeric_limits<uint16_t>::max());

    // Don't store the unpack_info if it has only the header (that is, there's
    // no meaningful content).
    // Primary Keys are special: for them, store the unpack_info even if it's
    // empty (provided m_maybe_unpack_info==true, see
    // ha_rocksdb::convert_record_to_storage_format)
    if (len == RDB_UNPACK_HEADER_SIZE &&
        m_index_type != Rdb_key_def::INDEX_TYPE_PRIMARY) {
      unpack_info->clear();
    } else {
      unpack_info->write_uint16_at(unpack_len_pos, len);
    }

    //
    // Secondary keys have key and value checksums in the value part
    // Primary key is a special case (the value part has non-indexed columns),
    // so the checksums are computed and stored by
    // ha_rocksdb::convert_record_to_storage_format
    //
    if (should_store_row_debug_checksums) {
      const uint32_t key_crc32 = crc32(0, packed_tuple, tuple - packed_tuple);
      const uint32_t val_crc32 =
          crc32(0, unpack_info->ptr(), unpack_info->get_current_pos());

      unpack_info->write_uint8(RDB_CHECKSUM_DATA_TAG);
      unpack_info->write_uint32(key_crc32);
      unpack_info->write_uint32(val_crc32);
    }
  }

  DBUG_ASSERT(is_storage_available(tuple - packed_tuple, 0));

  return tuple - packed_tuple;
}

/**
  Pack the hidden primary key into mem-comparable form.

  @param
    tbl                   Table we're working on
    hidden_pk_id     IN   New value to be packed into key
    packed_tuple     OUT  Key in the mem-comparable form

  @return
    Length of the packed tuple
*/

uint Rdb_key_def::pack_hidden_pk(const longlong &hidden_pk_id,
                                 uchar *const packed_tuple) const {
  DBUG_ASSERT(packed_tuple != nullptr);

  uchar *tuple = packed_tuple;
  rdb_netbuf_store_index(tuple, m_index_number);
  tuple += INDEX_NUMBER_SIZE;
  DBUG_ASSERT(m_key_parts == 1);
  DBUG_ASSERT(is_storage_available(tuple - packed_tuple,
                                   m_pack_info[0].m_max_image_len));

  m_pack_info[0].fill_hidden_pk_val(&tuple, hidden_pk_id);

  DBUG_ASSERT(is_storage_available(tuple - packed_tuple, 0));
  return tuple - packed_tuple;
}

/*
  Function of type rdb_index_field_pack_t
*/

void rdb_pack_with_make_sort_key(
    Rdb_field_packing *const fpi, Field *const field,
    uchar *const buf MY_ATTRIBUTE((__unused__)), uchar **dst,
    Rdb_pack_field_context *const pack_ctx MY_ATTRIBUTE((__unused__))) {
  DBUG_ASSERT(fpi != nullptr);
  DBUG_ASSERT(field != nullptr);
  DBUG_ASSERT(dst != nullptr);
  DBUG_ASSERT(*dst != nullptr);

  const int max_len = fpi->m_max_image_len;
  my_bitmap_map *old_map;

  old_map= dbug_tmp_use_all_columns(field->table,
                                    field->table->read_set);
  field->sort_string(*dst, max_len);
  dbug_tmp_restore_column_map(field->table->read_set, old_map);
  *dst += max_len;
}

/*
  Compares two keys without unpacking

  @detail
  @return
    0 - Ok. column_index is the index of the first column which is different.
          -1 if two kes are equal
    1 - Data format error.
*/
int Rdb_key_def::compare_keys(const rocksdb::Slice *key1,
                              const rocksdb::Slice *key2,
                              std::size_t *const column_index) const {
  DBUG_ASSERT(key1 != nullptr);
  DBUG_ASSERT(key2 != nullptr);
  DBUG_ASSERT(column_index != nullptr);

  // the caller should check the return value and
  // not rely on column_index being valid
  *column_index = 0xbadf00d;

  Rdb_string_reader reader1(key1);
  Rdb_string_reader reader2(key2);

  // Skip the index number
  if ((!reader1.read(INDEX_NUMBER_SIZE)))
    return HA_EXIT_FAILURE;

  if ((!reader2.read(INDEX_NUMBER_SIZE)))
    return HA_EXIT_FAILURE;

  for (uint i = 0; i < m_key_parts; i++) {
    const Rdb_field_packing *const fpi = &m_pack_info[i];
    if (fpi->m_maybe_null) {
      const auto nullp1 = reader1.read(1);
      const auto nullp2 = reader2.read(1);

      if (nullp1 == nullptr || nullp2 == nullptr) {
        return HA_EXIT_FAILURE;
      }

      if (*nullp1 != *nullp2) {
        *column_index = i;
        return HA_EXIT_SUCCESS;
      }

      if (*nullp1 == 0) {
        /* This is a NULL value */
        continue;
      }
    }

    const auto before_skip1 = reader1.get_current_ptr();
    const auto before_skip2 = reader2.get_current_ptr();
    DBUG_ASSERT(fpi->m_skip_func);
    if (fpi->m_skip_func(fpi, nullptr, &reader1))
      return HA_EXIT_FAILURE;
    if (fpi->m_skip_func(fpi, nullptr, &reader2))
      return HA_EXIT_FAILURE;
    const auto size1 = reader1.get_current_ptr() - before_skip1;
    const auto size2 = reader2.get_current_ptr() - before_skip2;
    if (size1 != size2) {
      *column_index = i;
      return HA_EXIT_SUCCESS;
    }

    if (memcmp(before_skip1, before_skip2, size1) != 0) {
      *column_index = i;
      return HA_EXIT_SUCCESS;
    }
  }

  *column_index = m_key_parts;
  return HA_EXIT_SUCCESS;
}

/*
  @brief
    Given a zero-padded key, determine its real key length

  @detail
    Fixed-size skip functions just read.
*/

size_t Rdb_key_def::key_length(const TABLE *const table,
                               const rocksdb::Slice &key) const {
  DBUG_ASSERT(table != nullptr);

  Rdb_string_reader reader(&key);

  if ((!reader.read(INDEX_NUMBER_SIZE)))
    return size_t(-1);

  for (uint i = 0; i < m_key_parts; i++) {
    const Rdb_field_packing *fpi = &m_pack_info[i];
    const Field *field = nullptr;
    if (m_index_type != INDEX_TYPE_HIDDEN_PRIMARY)
      field = fpi->get_field_in_table(table);
    if (fpi->m_skip_func(fpi, field, &reader))
      return size_t(-1);
  }
  return key.size() - reader.remaining_bytes();
}

int Rdb_key_def::unpack_field(
    Rdb_field_packing *const fpi,
    Field *const             field,
    Rdb_string_reader*       reader,
    const uchar *const       default_value,
    Rdb_string_reader*       unp_reader) const
{
  if (fpi->m_maybe_null) {
    const char *nullp;
    if (!(nullp = reader->read(1))) {
      return HA_EXIT_FAILURE;
    }

    if (*nullp == 0) {
      /* Set the NULL-bit of this field */
      field->set_null();
      /* Also set the field to its default value */
      memcpy(field->ptr, default_value, field->pack_length());
      return HA_EXIT_SUCCESS;
    } else if (*nullp == 1) {
      field->set_notnull();
    } else {
      return HA_EXIT_FAILURE;
    }
  }

  return fpi->m_unpack_func(fpi, field, field->ptr, reader, unp_reader);
}

/*
  Take mem-comparable form and unpack_info and unpack it to Table->record

  @detail
    not all indexes support this

  @return
    UNPACK_SUCCESS - Ok
    UNPACK_FAILURE - Data format error.
*/

int Rdb_key_def::unpack_record(TABLE *const table, uchar *const buf,
                               const rocksdb::Slice *const packed_key,
                               const rocksdb::Slice *const unpack_info,
                               const bool &verify_row_debug_checksums) const {
  Rdb_string_reader reader(packed_key);
  Rdb_string_reader unp_reader = Rdb_string_reader::read_or_empty(unpack_info);

  const bool is_hidden_pk = (m_index_type == INDEX_TYPE_HIDDEN_PRIMARY);
  const bool hidden_pk_exists = table_has_hidden_pk(table);
  const bool secondary_key = (m_index_type == INDEX_TYPE_SECONDARY);
  // There is no checksuming data after unpack_info for primary keys, because
  // the layout there is different. The checksum is verified in
  // ha_rocksdb::convert_record_from_storage_format instead.
  DBUG_ASSERT_IMP(!secondary_key, !verify_row_debug_checksums);

  // Skip the index number
  if ((!reader.read(INDEX_NUMBER_SIZE))) {
    return HA_EXIT_FAILURE;
  }

  // For secondary keys, we expect the value field to contain unpack data and
  // checksum data in that order. One or both can be missing, but they cannot
  // be reordered.
  const bool has_unpack_info =
      unp_reader.remaining_bytes() &&
      *unp_reader.get_current_ptr() == RDB_UNPACK_DATA_TAG;
  if (has_unpack_info && !unp_reader.read(RDB_UNPACK_HEADER_SIZE)) {
    return HA_EXIT_FAILURE;
  }

  for (uint i = 0; i < m_key_parts; i++) {
    Rdb_field_packing *const fpi = &m_pack_info[i];

    /*
      Hidden pk field is packed at the end of the secondary keys, but the SQL
      layer does not know about it. Skip retrieving field if hidden pk.
    */
    if ((secondary_key && hidden_pk_exists && i + 1 == m_key_parts) ||
        is_hidden_pk) {
      DBUG_ASSERT(fpi->m_unpack_func);
      if (fpi->m_skip_func(fpi, nullptr, &reader)) {
        return HA_EXIT_FAILURE;
      }
      continue;
    }

    Field *const field = fpi->get_field_in_table(table);

    if (fpi->m_unpack_func) {
      /* It is possible to unpack this column. Do it. */

      uint field_offset = field->ptr - table->record[0];
      uint null_offset = field->null_offset();
      bool maybe_null = field->real_maybe_null();
      field->move_field(buf + field_offset,
                        maybe_null ? buf + null_offset : nullptr,
                        field->null_bit);
      // WARNING! Don't return without restoring field->ptr and field->null_ptr

      // If we need unpack info, but there is none, tell the unpack function
      // this by passing unp_reader as nullptr. If we never read unpack_info
      // during unpacking anyway, then there won't an error.
      const bool maybe_missing_unpack =
          !has_unpack_info && fpi->uses_unpack_info();
      int res = unpack_field(fpi, field, &reader,
                             table->s->default_values + field_offset,
                             maybe_missing_unpack ? nullptr : &unp_reader);

      // Restore field->ptr and field->null_ptr
      field->move_field(table->record[0] + field_offset,
                        maybe_null ? table->record[0] + null_offset : nullptr,
                        field->null_bit);

      if (res) {
        return res;
      }
    } else {
      /* It is impossible to unpack the column. Skip it. */
      if (fpi->m_maybe_null) {
        const char *nullp;
        if (!(nullp = reader.read(1)))
          return HA_EXIT_FAILURE;
        if (*nullp == 0) {
          /* This is a NULL value */
          continue;
        }
        /* If NULL marker is not '0', it can be only '1'  */
        if (*nullp != 1)
          return HA_EXIT_FAILURE;
      }
      if (fpi->m_skip_func(fpi, field, &reader))
        return HA_EXIT_FAILURE;
    }
  }

  /*
    Check checksum values if present
  */
  const char *ptr;
  if ((ptr = unp_reader.read(1)) && *ptr == RDB_CHECKSUM_DATA_TAG) {
    if (verify_row_debug_checksums) {
      uint32_t stored_key_chksum = rdb_netbuf_to_uint32(
          (const uchar *)unp_reader.read(RDB_CHECKSUM_SIZE));
      const uint32_t stored_val_chksum = rdb_netbuf_to_uint32(
          (const uchar *)unp_reader.read(RDB_CHECKSUM_SIZE));

      const uint32_t computed_key_chksum =
          crc32(0, (const uchar *)packed_key->data(), packed_key->size());
      const uint32_t computed_val_chksum =
          crc32(0, (const uchar *)unpack_info->data(),
                unpack_info->size() - RDB_CHECKSUM_CHUNK_SIZE);

      DBUG_EXECUTE_IF("myrocks_simulate_bad_key_checksum1",
                      stored_key_chksum++;);

      if (stored_key_chksum != computed_key_chksum) {
        report_checksum_mismatch(true, packed_key->data(), packed_key->size());
        return HA_EXIT_FAILURE;
      }

      if (stored_val_chksum != computed_val_chksum) {
        report_checksum_mismatch(false, unpack_info->data(),
                                 unpack_info->size() - RDB_CHECKSUM_CHUNK_SIZE);
        return HA_EXIT_FAILURE;
      }
    } else {
      /* The checksums are present but we are not checking checksums */
    }
  }

  if (reader.remaining_bytes())
    return HA_EXIT_FAILURE;

  return HA_EXIT_SUCCESS;
}

bool Rdb_key_def::table_has_hidden_pk(const TABLE *const table) {
  return table->s->primary_key == MAX_INDEXES;
}

void Rdb_key_def::report_checksum_mismatch(const bool &is_key,
                                           const char *const data,
                                           const size_t data_size) const {
  // NO_LINT_DEBUG
  sql_print_error("Checksum mismatch in %s of key-value pair for index 0x%x",
                  is_key ? "key" : "value", get_index_number());

  const std::string buf = rdb_hexdump(data, data_size, RDB_MAX_HEXDUMP_LEN);
  // NO_LINT_DEBUG
  sql_print_error("Data with incorrect checksum (%" PRIu64 " bytes): %s",
                  (uint64_t)data_size, buf.c_str());

  my_error(ER_INTERNAL_ERROR, MYF(0), "Record checksum mismatch");
}

bool Rdb_key_def::index_format_min_check(const int &pk_min,
                                         const int &sk_min) const {
  switch (m_index_type) {
  case INDEX_TYPE_PRIMARY:
  case INDEX_TYPE_HIDDEN_PRIMARY:
    return (m_kv_format_version >= pk_min);
  case INDEX_TYPE_SECONDARY:
    return (m_kv_format_version >= sk_min);
  default:
    DBUG_ASSERT(0);
    return false;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////
// Rdb_field_packing
///////////////////////////////////////////////////////////////////////////////////////////

/*
  Function of type rdb_index_field_skip_t
*/

int rdb_skip_max_length(const Rdb_field_packing *const fpi,
                        const Field *const field MY_ATTRIBUTE((__unused__)),
                        Rdb_string_reader *const reader) {
  if (!reader->read(fpi->m_max_image_len))
    return HA_EXIT_FAILURE;
  return HA_EXIT_SUCCESS;
}

/*
  (RDB_ESCAPE_LENGTH-1) must be an even number so that pieces of lines are not
  split in the middle of an UTF-8 character. See the implementation of
  rdb_unpack_binary_or_utf8_varchar.
*/

const uint RDB_ESCAPE_LENGTH = 9;
static_assert((RDB_ESCAPE_LENGTH - 1) % 2 == 0,
              "RDB_ESCAPE_LENGTH-1 must be even.");

/*
  Function of type rdb_index_field_skip_t
*/

static int rdb_skip_variable_length(
    const Rdb_field_packing *const fpi MY_ATTRIBUTE((__unused__)),
    const Field *const field, Rdb_string_reader *const reader) {
  const uchar *ptr;
  bool finished = false;

  size_t dst_len; /* How much data can be there */
  if (field) {
    const Field_varstring *const field_var =
        static_cast<const Field_varstring *>(field);
    dst_len = field_var->pack_length() - field_var->length_bytes;
  } else {
    dst_len = UINT_MAX;
  }

  /* Decode the length-emitted encoding here */
  while ((ptr = (const uchar *)reader->read(RDB_ESCAPE_LENGTH))) {
    /* See rdb_pack_with_varchar_encoding. */
    const uchar pad =
        255 - ptr[RDB_ESCAPE_LENGTH - 1]; // number of padding bytes
    const uchar used_bytes = RDB_ESCAPE_LENGTH - 1 - pad;

    if (used_bytes > RDB_ESCAPE_LENGTH - 1 || used_bytes > dst_len) {
      return HA_EXIT_FAILURE; /* cannot store that much, invalid data */
    }

    if (used_bytes < RDB_ESCAPE_LENGTH - 1) {
      finished = true;
      break;
    }
    dst_len -= used_bytes;
  }

  if (!finished) {
    return HA_EXIT_FAILURE;
  }

  return HA_EXIT_SUCCESS;
}

const int VARCHAR_CMP_LESS_THAN_SPACES = 1;
const int VARCHAR_CMP_EQUAL_TO_SPACES = 2;
const int VARCHAR_CMP_GREATER_THAN_SPACES = 3;

/*
  Skip a keypart that uses Variable-Length Space-Padded encoding
*/

static int rdb_skip_variable_space_pad(const Rdb_field_packing *const fpi,
                                       const Field *const field,
                                       Rdb_string_reader *const reader) {
  const uchar *ptr;
  bool finished = false;

  size_t dst_len = UINT_MAX; /* How much data can be there */

  if (field) {
    const Field_varstring *const field_var =
        static_cast<const Field_varstring *>(field);
    dst_len = field_var->pack_length() - field_var->length_bytes;
  }

  /* Decode the length-emitted encoding here */
  while ((ptr = (const uchar *)reader->read(fpi->m_segment_size))) {
    // See rdb_pack_with_varchar_space_pad
    const uchar c = ptr[fpi->m_segment_size - 1];
    if (c == VARCHAR_CMP_EQUAL_TO_SPACES) {
      // This is the last segment
      finished = true;
      break;
    } else if (c == VARCHAR_CMP_LESS_THAN_SPACES ||
               c == VARCHAR_CMP_GREATER_THAN_SPACES) {
      // This is not the last segment
      if ((fpi->m_segment_size - 1) > dst_len) {
        // The segment is full of data but the table field can't hold that
        // much! This must be data corruption.
        return HA_EXIT_FAILURE;
      }
      dst_len -= (fpi->m_segment_size - 1);
    } else {
      // Encountered a value that's none of the VARCHAR_CMP* constants
      // It's data corruption.
      return HA_EXIT_FAILURE;
    }
  }
  return finished ? HA_EXIT_SUCCESS : HA_EXIT_FAILURE;
}

/*
  Function of type rdb_index_field_unpack_t
*/

int rdb_unpack_integer(Rdb_field_packing *const fpi, Field *const field,
                       uchar *const to, Rdb_string_reader *const reader,
                       Rdb_string_reader *const unp_reader
                           MY_ATTRIBUTE((__unused__))) {
  const int length = fpi->m_max_image_len;

  const uchar *from;
  if (!(from = (const uchar *)reader->read(length)))
    return UNPACK_FAILURE; /* Mem-comparable image doesn't have enough bytes */

#ifdef WORDS_BIGENDIAN
  {
    if (((Field_num *)field)->unsigned_flag)
      to[0] = from[0];
    else
      to[0] = (char)(from[0] ^ 128); // Reverse the sign bit.
    memcpy(to + 1, from + 1, length - 1);
  }
#else
  {
    const int sign_byte = from[0];
    if (((Field_num *)field)->unsigned_flag)
      to[length - 1] = sign_byte;
    else
      to[length - 1] =
          static_cast<char>(sign_byte ^ 128); // Reverse the sign bit.
    for (int i = 0, j = length - 1; i < length - 1; ++i, --j)
      to[i] = from[j];
  }
#endif
  return UNPACK_SUCCESS;
}

#if !defined(WORDS_BIGENDIAN)
static void rdb_swap_double_bytes(uchar *const dst, const uchar *const src) {
#if defined(__FLOAT_WORD_ORDER) && (__FLOAT_WORD_ORDER == __BIG_ENDIAN)
  // A few systems store the most-significant _word_ first on little-endian
  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
  dst[4] = src[7];
  dst[5] = src[6];
  dst[6] = src[5];
  dst[7] = src[4];
#else
  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
#endif
}

static void rdb_swap_float_bytes(uchar *const dst, const uchar *const src) {
  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}
#else
#define rdb_swap_double_bytes nullptr
#define rdb_swap_float_bytes nullptr
#endif

static int rdb_unpack_floating_point(
    uchar *const dst, Rdb_string_reader *const reader, const size_t &size,
    const int &exp_digit, const uchar *const zero_pattern,
    const uchar *const zero_val, void (*swap_func)(uchar *, const uchar *)) {
  const uchar *const from = (const uchar *)reader->read(size);
  if (from == nullptr)
    return UNPACK_FAILURE; /* Mem-comparable image doesn't have enough bytes */

  /* Check to see if the value is zero */
  if (memcmp(from, zero_pattern, size) == 0) {
    memcpy(dst, zero_val, size);
    return UNPACK_SUCCESS;
  }

#if defined(WORDS_BIGENDIAN)
  // On big-endian, output can go directly into result
  uchar *const tmp = dst;
#else
  // Otherwise use a temporary buffer to make byte-swapping easier later
  uchar tmp[8];
#endif

  memcpy(tmp, from, size);

  if (tmp[0] & 0x80) {
    // If the high bit is set the original value was positive so
    // remove the high bit and subtract one from the exponent.
    ushort exp_part = ((ushort)tmp[0] << 8) | (ushort)tmp[1];
    exp_part &= 0x7FFF;                            // clear high bit;
    exp_part -= (ushort)1 << (16 - 1 - exp_digit); // subtract from exponent
    tmp[0] = (uchar)(exp_part >> 8);
    tmp[1] = (uchar)exp_part;
  } else {
    // Otherwise the original value was negative and all bytes have been
    // negated.
    for (size_t ii = 0; ii < size; ii++)
      tmp[ii] ^= 0xFF;
  }

#if !defined(WORDS_BIGENDIAN)
  // On little-endian, swap the bytes around
  swap_func(dst, tmp);
#else
  DBUG_ASSERT(swap_func == nullptr);
#endif

  return UNPACK_SUCCESS;
}

#if !defined(DBL_EXP_DIG)
#define DBL_EXP_DIG (sizeof(double) * 8 - DBL_MANT_DIG)
#endif

/*
  Function of type rdb_index_field_unpack_t

  Unpack a double by doing the reverse action of change_double_for_sort
  (sql/filesort.cc).  Note that this only works on IEEE values.
  Note also that this code assumes that NaN and +/-Infinity are never
  allowed in the database.
*/
static int rdb_unpack_double(
    Rdb_field_packing *const fpi MY_ATTRIBUTE((__unused__)),
    Field *const field MY_ATTRIBUTE((__unused__)), uchar *const field_ptr,
    Rdb_string_reader *const reader,
    Rdb_string_reader *const unp_reader MY_ATTRIBUTE((__unused__))) {
  static double zero_val = 0.0;
  static const uchar zero_pattern[8] = {128, 0, 0, 0, 0, 0, 0, 0};

  return rdb_unpack_floating_point(
      field_ptr, reader, sizeof(double), DBL_EXP_DIG, zero_pattern,
      (const uchar *)&zero_val, rdb_swap_double_bytes);
}

#if !defined(FLT_EXP_DIG)
#define FLT_EXP_DIG (sizeof(float) * 8 - FLT_MANT_DIG)
#endif

/*
  Function of type rdb_index_field_unpack_t

  Unpack a float by doing the reverse action of Field_float::make_sort_key
  (sql/field.cc).  Note that this only works on IEEE values.
  Note also that this code assumes that NaN and +/-Infinity are never
  allowed in the database.
*/
static int rdb_unpack_float(
    Rdb_field_packing *const, Field *const field MY_ATTRIBUTE((__unused__)),
    uchar *const field_ptr, Rdb_string_reader *const reader,
    Rdb_string_reader *const unp_reader MY_ATTRIBUTE((__unused__))) {
  static float zero_val = 0.0;
  static const uchar zero_pattern[4] = {128, 0, 0, 0};

  return rdb_unpack_floating_point(
      field_ptr, reader, sizeof(float), FLT_EXP_DIG, zero_pattern,
      (const uchar *)&zero_val, rdb_swap_float_bytes);
}

/*
  Function of type rdb_index_field_unpack_t used to
  Unpack by doing the reverse action to Field_newdate::make_sort_key.
*/

int rdb_unpack_newdate(Rdb_field_packing *const fpi, Field *constfield,
                       uchar *const field_ptr, Rdb_string_reader *const reader,
                       Rdb_string_reader *const unp_reader
                           MY_ATTRIBUTE((__unused__))) {
  const char *from;
  DBUG_ASSERT(fpi->m_max_image_len == 3);

  if (!(from = reader->read(3)))
    return UNPACK_FAILURE; /* Mem-comparable image doesn't have enough bytes */

  field_ptr[0] = from[2];
  field_ptr[1] = from[1];
  field_ptr[2] = from[0];
  return UNPACK_SUCCESS;
}

/*
  Function of type rdb_index_field_unpack_t, used to
  Unpack the string by copying it over.
  This is for BINARY(n) where the value occupies the whole length.
*/

static int rdb_unpack_binary_str(
    Rdb_field_packing *const fpi, Field *const field, uchar *const to,
    Rdb_string_reader *const reader,
    Rdb_string_reader *const unp_reader MY_ATTRIBUTE((__unused__))) {
  const char *from;
  if (!(from = reader->read(fpi->m_max_image_len)))
    return UNPACK_FAILURE; /* Mem-comparable image doesn't have enough bytes */

  memcpy(to, from, fpi->m_max_image_len);
  return UNPACK_SUCCESS;
}

/*
  Function of type rdb_index_field_unpack_t.
  For UTF-8, we need to convert 2-byte wide-character entities back into
  UTF8 sequences.
*/

static int rdb_unpack_utf8_str(Rdb_field_packing *const fpi, Field *const field,
                               uchar *dst, Rdb_string_reader *const reader,
                               Rdb_string_reader *const unp_reader
                                   MY_ATTRIBUTE((__unused__))) {
  my_core::CHARSET_INFO *const cset = (my_core::CHARSET_INFO *)field->charset();
  const uchar *src;
  if (!(src = (const uchar *)reader->read(fpi->m_max_image_len)))
    return UNPACK_FAILURE; /* Mem-comparable image doesn't have enough bytes */

  const uchar *const src_end = src + fpi->m_max_image_len;
  uchar *const dst_end = dst + field->pack_length();

  while (src < src_end) {
    my_wc_t wc = (src[0] << 8) | src[1];
    src += 2;
    int res = cset->cset->wc_mb(cset, wc, dst, dst_end);
    DBUG_ASSERT(res > 0 && res <= 3);
    if (res < 0)
      return UNPACK_FAILURE;
    dst += res;
  }

  cset->cset->fill(cset, reinterpret_cast<char *>(dst), dst_end - dst,
                   cset->pad_char);
  return UNPACK_SUCCESS;
}

/*
  Function of type rdb_index_field_pack_t
*/

static void rdb_pack_with_varchar_encoding(
    Rdb_field_packing *const fpi, Field *const field, uchar *buf, uchar **dst,
    Rdb_pack_field_context *const pack_ctx MY_ATTRIBUTE((__unused__))) {
  /*
    Use a flag byte every Nth byte. Set it to (255 - #pad) where #pad is 0
    when the var length field filled all N-1 previous bytes and #pad is
    otherwise the number of padding bytes used.

    If N=8 and the field is:
    * 3 bytes (1, 2, 3) this is encoded as: 1, 2, 3, 0, 0, 0, 0, 251
    * 4 bytes (1, 2, 3, 0) this is encoded as: 1, 2, 3, 0, 0, 0, 0, 252
    And the 4 byte string compares as greater than the 3 byte string
  */
  const CHARSET_INFO *const charset = field->charset();
  Field_varstring *const field_var = (Field_varstring *)field;

  const size_t value_length = (field_var->length_bytes == 1)
                                  ? (uint)*field->ptr
                                  : uint2korr(field->ptr);
  size_t xfrm_len = charset->coll->strnxfrm(
      charset, buf, fpi->m_max_image_len, field_var->char_length(),
      field_var->ptr + field_var->length_bytes, value_length, 0);

  /* Got a mem-comparable image in 'buf'. Now, produce varlength encoding */

  size_t encoded_size = 0;
  uchar *ptr = *dst;
  while (1) {
    const size_t copy_len = std::min((size_t)RDB_ESCAPE_LENGTH - 1, xfrm_len);
    const size_t padding_bytes = RDB_ESCAPE_LENGTH - 1 - copy_len;
    memcpy(ptr, buf, copy_len);
    ptr += copy_len;
    buf += copy_len;
    // pad with zeros if necessary;
    for (size_t idx = 0; idx < padding_bytes; idx++)
      *(ptr++) = 0;
    *(ptr++) = 255 - (uchar)padding_bytes;

    xfrm_len -= copy_len;
    encoded_size += RDB_ESCAPE_LENGTH;
    if (padding_bytes != 0)
      break;
  }
  *dst += encoded_size;
}

/*
  Compare the string in [buf..buf_end) with a string that is an infinite
  sequence of strings in space_xfrm
*/

static int
rdb_compare_string_with_spaces(const uchar *buf, const uchar *const buf_end,
                               const std::vector<uchar> *const space_xfrm) {
  int cmp = 0;
  while (buf < buf_end) {
    size_t bytes = std::min((size_t)(buf_end - buf), space_xfrm->size());
    if ((cmp = memcmp(buf, space_xfrm->data(), bytes)) != 0)
      break;
    buf += bytes;
  }
  return cmp;
}

static const int RDB_TRIMMED_CHARS_OFFSET = 8;
/*
  Pack the data with Variable-Length Space-Padded Encoding.

  The encoding is there to meet two goals:

  Goal#1. Comparison. The SQL standard says

    " If the collation for the comparison has the PAD SPACE characteristic,
    for the purposes of the comparison, the shorter value is effectively
    extended to the length of the longer by concatenation of <space>s on the
    right.

  At the moment, all MySQL collations except one have the PAD SPACE
  characteristic.  The exception is the "binary" collation that is used by
  [VAR]BINARY columns. (Note that binary collations for specific charsets,
  like utf8_bin or latin1_bin are not the same as "binary" collation, they have
  the PAD SPACE characteristic).

  Goal#2 is to preserve the number of trailing spaces in the original value.

  This is achieved by using the following encoding:
  The key part:
  - Stores mem-comparable image of the column
  - It is stored in chunks of fpi->m_segment_size bytes (*)
    = If the remainder of the chunk is not occupied, it is padded with mem-
      comparable image of the space character (cs->pad_char to be precise).
  - The last byte of the chunk shows how the rest of column's mem-comparable
    image would compare to mem-comparable image of the column extended with
    spaces. There are three possible values.
     - VARCHAR_CMP_LESS_THAN_SPACES,
     - VARCHAR_CMP_EQUAL_TO_SPACES
     - VARCHAR_CMP_GREATER_THAN_SPACES

  VARCHAR_CMP_EQUAL_TO_SPACES means that this chunk is the last one (the rest
  is spaces, or something that sorts as spaces, so there is no reason to store
  it).

  Example: if fpi->m_segment_size=5, and the collation is latin1_bin:

   'abcd\0'   => [ 'abcd' <VARCHAR_CMP_LESS> ]['\0    ' <VARCHAR_CMP_EQUAL> ]
   'abcd'     => [ 'abcd' <VARCHAR_CMP_EQUAL>]
   'abcd   '  => [ 'abcd' <VARCHAR_CMP_EQUAL>]
   'abcdZZZZ' => [ 'abcd' <VARCHAR_CMP_GREATER>][ 'ZZZZ' <VARCHAR_CMP_EQUAL>]

  As mentioned above, the last chunk is padded with mem-comparable images of
  cs->pad_char. It can be 1-byte long (latin1), 2 (utf8_bin), 3 (utf8mb4), etc.

  fpi->m_segment_size depends on the used collation. It is chosen to be such
  that no mem-comparable image of space will ever stretch across the segments
  (see get_segment_size_from_collation).

  == The value part (aka unpack_info) ==
  The value part stores the number of space characters that one needs to add
  when unpacking the string.
  - If the number is positive, it means add this many spaces at the end
  - If the number is negative, it means padding has added extra spaces which
    must be removed.

  Storage considerations
  - depending on column's max size, the number may occupy 1 or 2 bytes
  - the number of spaces that need to be removed is not more than
    RDB_TRIMMED_CHARS_OFFSET=8, so we offset the number by that value and
    then store it as unsigned.

  @seealso
    rdb_unpack_binary_or_utf8_varchar_space_pad
    rdb_unpack_simple_varchar_space_pad
    rdb_dummy_make_unpack_info
    rdb_skip_variable_space_pad
*/

static void
rdb_pack_with_varchar_space_pad(Rdb_field_packing *const fpi,
                                Field *const field, uchar *buf, uchar **dst,
                                Rdb_pack_field_context *const pack_ctx) {
  Rdb_string_writer *const unpack_info = pack_ctx->writer;
  const CHARSET_INFO *const charset = field->charset();
  const auto field_var = static_cast<Field_varstring *>(field);

  const size_t value_length = (field_var->length_bytes == 1)
                                  ? (uint)*field->ptr
                                  : uint2korr(field->ptr);

  const size_t trimmed_len = charset->cset->lengthsp(
      charset, (const char *)field_var->ptr + field_var->length_bytes,
      value_length);
  const size_t xfrm_len = charset->coll->strnxfrm(
      charset, buf, fpi->m_max_image_len, field_var->char_length(),
      field_var->ptr + field_var->length_bytes, trimmed_len, 0);

  /* Got a mem-comparable image in 'buf'. Now, produce varlength encoding */
  uchar *const buf_end = buf + xfrm_len;

  size_t encoded_size = 0;
  uchar *ptr = *dst;
  size_t padding_bytes;
  while (true) {
    const size_t copy_len =
        std::min<size_t>(fpi->m_segment_size - 1, buf_end - buf);
    padding_bytes = fpi->m_segment_size - 1 - copy_len;
    memcpy(ptr, buf, copy_len);
    ptr += copy_len;
    buf += copy_len;

    if (padding_bytes) {
      memcpy(ptr, fpi->space_xfrm->data(), padding_bytes);
      ptr += padding_bytes;
      *ptr = VARCHAR_CMP_EQUAL_TO_SPACES; // last segment
    } else {
      // Compare the string suffix with a hypothetical infinite string of
      // spaces. It could be that the first difference is beyond the end of
      // current chunk.
      const int cmp =
          rdb_compare_string_with_spaces(buf, buf_end, fpi->space_xfrm);

      if (cmp < 0)
        *ptr = VARCHAR_CMP_LESS_THAN_SPACES;
      else if (cmp > 0)
        *ptr = VARCHAR_CMP_GREATER_THAN_SPACES;
      else {
        // It turns out all the rest are spaces.
        *ptr = VARCHAR_CMP_EQUAL_TO_SPACES;
      }
    }
    encoded_size += fpi->m_segment_size;

    if (*(ptr++) == VARCHAR_CMP_EQUAL_TO_SPACES)
      break;
  }

  // m_unpack_info_stores_value means unpack_info stores the whole original
  // value. There is no need to store the number of trimmed/padded endspaces
  // in that case.
  if (unpack_info && !fpi->m_unpack_info_stores_value) {
    // (value_length - trimmed_len) is the number of trimmed space *characters*
    // then, padding_bytes is the number of *bytes* added as padding
    // then, we add 8, because we don't store negative values.
    DBUG_ASSERT(padding_bytes % fpi->space_xfrm_len == 0);
    DBUG_ASSERT((value_length - trimmed_len) % fpi->space_mb_len == 0);
    const size_t removed_chars =
        RDB_TRIMMED_CHARS_OFFSET +
        (value_length - trimmed_len) / fpi->space_mb_len -
        padding_bytes / fpi->space_xfrm_len;

    if (fpi->m_unpack_info_uses_two_bytes) {
      unpack_info->write_uint16(removed_chars);
    } else {
      DBUG_ASSERT(removed_chars < 0x100);
      unpack_info->write_uint8(removed_chars);
    }
  }

  *dst += encoded_size;
}

/*
  Function of type rdb_index_field_unpack_t
*/

static int rdb_unpack_binary_or_utf8_varchar(
    Rdb_field_packing *const fpi, Field *const field, uchar *dst,
    Rdb_string_reader *const reader,
    Rdb_string_reader *const unp_reader MY_ATTRIBUTE((__unused__))) {
  const uchar *ptr;
  size_t len = 0;
  bool finished = false;
  uchar *d0 = dst;
  Field_varstring *const field_var = (Field_varstring *)field;
  dst += field_var->length_bytes;
  // How much we can unpack
  size_t dst_len = field_var->pack_length() - field_var->length_bytes;
  uchar *const dst_end = dst + dst_len;

  /* Decode the length-emitted encoding here */
  while ((ptr = (const uchar *)reader->read(RDB_ESCAPE_LENGTH))) {
    /* See rdb_pack_with_varchar_encoding. */
    uchar pad = 255 - ptr[RDB_ESCAPE_LENGTH - 1]; // number of padding bytes
    uchar used_bytes = RDB_ESCAPE_LENGTH - 1 - pad;

    if (used_bytes > RDB_ESCAPE_LENGTH - 1) {
      return UNPACK_FAILURE; /* cannot store that much, invalid data */
    }

    if (dst_len < used_bytes) {
      /* Encoded index tuple is longer than the size in the record buffer? */
      return UNPACK_FAILURE;
    }

    /*
      Now, we need to decode used_bytes of data and append them to the value.
    */
    if (fpi->m_varchar_charset->number == COLLATION_UTF8_BIN) {
      if (used_bytes & 1) {
        /*
          UTF-8 characters are encoded into two-byte entities. There is no way
          we can have an odd number of bytes after encoding.
        */
        return UNPACK_FAILURE;
      }

      const uchar *src = ptr;
      const uchar *src_end = ptr + used_bytes;
      while (src < src_end) {
        my_wc_t wc = (src[0] << 8) | src[1];
        src += 2;
        const CHARSET_INFO *cset = fpi->m_varchar_charset;
        int res = cset->cset->wc_mb(cset, wc, dst, dst_end);
        DBUG_ASSERT(res > 0 && res <= 3);
        if (res < 0)
          return UNPACK_FAILURE;
        dst += res;
        len += res;
        dst_len -= res;
      }
    } else {
      memcpy(dst, ptr, used_bytes);
      dst += used_bytes;
      dst_len -= used_bytes;
      len += used_bytes;
    }

    if (used_bytes < RDB_ESCAPE_LENGTH - 1) {
      finished = true;
      break;
    }
  }

  if (!finished)
    return UNPACK_FAILURE;

  /* Save the length */
  if (field_var->length_bytes == 1) {
    d0[0] = (uchar)len;
  } else {
    DBUG_ASSERT(field_var->length_bytes == 2);
    int2store(d0, len);
  }
  return UNPACK_SUCCESS;
}

/*
  @seealso
    rdb_pack_with_varchar_space_pad - packing function
    rdb_unpack_simple_varchar_space_pad - unpacking function for 'simple'
    charsets.
    rdb_skip_variable_space_pad - skip function
*/
static int rdb_unpack_binary_or_utf8_varchar_space_pad(
    Rdb_field_packing *const fpi, Field *const field, uchar *dst,
    Rdb_string_reader *const reader, Rdb_string_reader *const unp_reader) {
  const uchar *ptr;
  size_t len = 0;
  bool finished = false;
  Field_varstring *const field_var = static_cast<Field_varstring *>(field);
  uchar *d0 = dst;
  uchar *dst_end = dst + field_var->pack_length();
  dst += field_var->length_bytes;

  uint space_padding_bytes = 0;
  uint extra_spaces;
  if ((fpi->m_unpack_info_uses_two_bytes
           ? unp_reader->read_uint16(&extra_spaces)
           : unp_reader->read_uint8(&extra_spaces))) {
    return UNPACK_FAILURE;
  }

  if (extra_spaces <= RDB_TRIMMED_CHARS_OFFSET) {
    space_padding_bytes =
        -(static_cast<int>(extra_spaces) - RDB_TRIMMED_CHARS_OFFSET);
    extra_spaces = 0;
  } else
    extra_spaces -= RDB_TRIMMED_CHARS_OFFSET;

  space_padding_bytes *= fpi->space_xfrm_len;

  /* Decode the length-emitted encoding here */
  while ((ptr = (const uchar *)reader->read(fpi->m_segment_size))) {
    const char last_byte = ptr[fpi->m_segment_size - 1];
    size_t used_bytes;
    if (last_byte == VARCHAR_CMP_EQUAL_TO_SPACES) // this is the last segment
    {
      if (space_padding_bytes > (fpi->m_segment_size - 1))
        return UNPACK_FAILURE; // Cannot happen, corrupted data
      used_bytes = (fpi->m_segment_size - 1) - space_padding_bytes;
      finished = true;
    } else {
      if (last_byte != VARCHAR_CMP_LESS_THAN_SPACES &&
          last_byte != VARCHAR_CMP_GREATER_THAN_SPACES) {
        return UNPACK_FAILURE; // Invalid value
      }
      used_bytes = fpi->m_segment_size - 1;
    }

    // Now, need to decode used_bytes of data and append them to the value.
    if (fpi->m_varchar_charset->number == COLLATION_UTF8_BIN) {
      if (used_bytes & 1) {
        /*
          UTF-8 characters are encoded into two-byte entities. There is no way
          we can have an odd number of bytes after encoding.
        */
        return UNPACK_FAILURE;
      }

      const uchar *src = ptr;
      const uchar *const src_end = ptr + used_bytes;
      while (src < src_end) {
        my_wc_t wc = (src[0] << 8) | src[1];
        src += 2;
        const CHARSET_INFO *cset = fpi->m_varchar_charset;
        int res = cset->cset->wc_mb(cset, wc, dst, dst_end);
        DBUG_ASSERT(res <= 3);
        if (res <= 0)
          return UNPACK_FAILURE;
        dst += res;
        len += res;
      }
    } else {
      if (dst + used_bytes > dst_end)
        return UNPACK_FAILURE;
      memcpy(dst, ptr, used_bytes);
      dst += used_bytes;
      len += used_bytes;
    }

    if (finished) {
      if (extra_spaces) {
        // Both binary and UTF-8 charset store space as ' ',
        // so the following is ok:
        if (dst + extra_spaces > dst_end)
          return UNPACK_FAILURE;
        memset(dst, fpi->m_varchar_charset->pad_char, extra_spaces);
        len += extra_spaces;
      }
      break;
    }
  }

  if (!finished)
    return UNPACK_FAILURE;

  /* Save the length */
  if (field_var->length_bytes == 1) {
    d0[0] = (uchar)len;
  } else {
    DBUG_ASSERT(field_var->length_bytes == 2);
    int2store(d0, len);
  }
  return UNPACK_SUCCESS;
}

/////////////////////////////////////////////////////////////////////////

/*
  Function of type rdb_make_unpack_info_t
*/

static void rdb_make_unpack_unknown(
    const Rdb_collation_codec *codec MY_ATTRIBUTE((__unused__)),
    const Field *const field, Rdb_pack_field_context *const pack_ctx) {
  pack_ctx->writer->write(field->ptr, field->pack_length());
}

/*
  This point of this function is only to indicate that unpack_info is
  available.

  The actual unpack_info data is produced by the function that packs the key,
  that is, rdb_pack_with_varchar_space_pad.
*/

static void rdb_dummy_make_unpack_info(
    const Rdb_collation_codec *codec MY_ATTRIBUTE((__unused__)),
    const Field *field MY_ATTRIBUTE((__unused__)),
    Rdb_pack_field_context *pack_ctx MY_ATTRIBUTE((__unused__))) {}

/*
  Function of type rdb_index_field_unpack_t
*/

static int rdb_unpack_unknown(Rdb_field_packing *const fpi, Field *const field,
                              uchar *const dst, Rdb_string_reader *const reader,
                              Rdb_string_reader *const unp_reader) {
  const uchar *ptr;
  const uint len = fpi->m_unpack_data_len;
  // We don't use anything from the key, so skip over it.
  if (rdb_skip_max_length(fpi, field, reader)) {
    return UNPACK_FAILURE;
  }

  DBUG_ASSERT_IMP(len > 0, unp_reader != nullptr);

  if ((ptr = (const uchar *)unp_reader->read(len))) {
    memcpy(dst, ptr, len);
    return UNPACK_SUCCESS;
  }
  return UNPACK_FAILURE;
}

/*
  Function of type rdb_make_unpack_info_t
*/

static void rdb_make_unpack_unknown_varchar(
    const Rdb_collation_codec *const codec MY_ATTRIBUTE((__unused__)),
    const Field *const field, Rdb_pack_field_context *const pack_ctx) {
  const auto f = static_cast<const Field_varstring *>(field);
  uint len = f->length_bytes == 1 ? (uint)*f->ptr : uint2korr(f->ptr);
  len += f->length_bytes;
  pack_ctx->writer->write(field->ptr, len);
}

/*
  Function of type rdb_index_field_unpack_t

  @detail
  Unpack a key part in an "unknown" collation from its
  (mem_comparable_form, unpack_info) form.

  "Unknown" means we have no clue about how mem_comparable_form is made from
  the original string, so we keep the whole original string in the unpack_info.

  @seealso
    rdb_make_unpack_unknown, rdb_unpack_unknown
*/

static int rdb_unpack_unknown_varchar(Rdb_field_packing *const fpi,
                                      Field *const field, uchar *dst,
                                      Rdb_string_reader *const reader,
                                      Rdb_string_reader *const unp_reader) {
  const uchar *ptr;
  uchar *const d0 = dst;
  const auto f = static_cast<Field_varstring *>(field);
  dst += f->length_bytes;
  const uint len_bytes = f->length_bytes;
  // We don't use anything from the key, so skip over it.
  if (fpi->m_skip_func(fpi, field, reader)) {
    return UNPACK_FAILURE;
  }

  DBUG_ASSERT(len_bytes > 0);
  DBUG_ASSERT(unp_reader != nullptr);

  if ((ptr = (const uchar *)unp_reader->read(len_bytes))) {
    memcpy(d0, ptr, len_bytes);
    const uint len = len_bytes == 1 ? (uint)*ptr : uint2korr(ptr);
    if ((ptr = (const uchar *)unp_reader->read(len))) {
      memcpy(dst, ptr, len);
      return UNPACK_SUCCESS;
    }
  }
  return UNPACK_FAILURE;
}

/*
  Write unpack_data for a "simple" collation
*/
static void rdb_write_unpack_simple(Rdb_bit_writer *const writer,
                                    const Rdb_collation_codec *const codec,
                                    const uchar *const src,
                                    const size_t src_len) {
  for (uint i = 0; i < src_len; i++) {
    writer->write(codec->m_enc_size[src[i]], codec->m_enc_idx[src[i]]);
  }
}

static uint rdb_read_unpack_simple(Rdb_bit_reader *const reader,
                                   const Rdb_collation_codec *const codec,
                                   const uchar *const src,
                                   const size_t &src_len, uchar *const dst) {
  for (uint i = 0; i < src_len; i++) {
    if (codec->m_dec_size[src[i]] > 0) {
      uint *ret;
      DBUG_ASSERT(reader != nullptr);

      if ((ret = reader->read(codec->m_dec_size[src[i]])) == nullptr) {
        return UNPACK_FAILURE;
      }
      dst[i] = codec->m_dec_idx[*ret][src[i]];
    } else {
      dst[i] = codec->m_dec_idx[0][src[i]];
    }
  }

  return UNPACK_SUCCESS;
}

/*
  Function of type rdb_make_unpack_info_t

  @detail
    Make unpack_data for VARCHAR(n) in a "simple" charset.
*/

static void
rdb_make_unpack_simple_varchar(const Rdb_collation_codec *const codec,
                               const Field *const field,
                               Rdb_pack_field_context *const pack_ctx) {
  const auto f = static_cast<const Field_varstring *>(field);
  uchar *const src = f->ptr + f->length_bytes;
  const size_t src_len =
      f->length_bytes == 1 ? (uint)*f->ptr : uint2korr(f->ptr);
  Rdb_bit_writer bit_writer(pack_ctx->writer);
  // The std::min compares characters with bytes, but for simple collations,
  // mbmaxlen = 1.
  rdb_write_unpack_simple(&bit_writer, codec, src,
                          std::min((size_t)f->char_length(), src_len));
}

/*
  Function of type rdb_index_field_unpack_t

  @seealso
    rdb_pack_with_varchar_space_pad - packing function
    rdb_unpack_binary_or_utf8_varchar_space_pad - a similar unpacking function
*/

int rdb_unpack_simple_varchar_space_pad(Rdb_field_packing *const fpi,
                                        Field *const field, uchar *dst,
                                        Rdb_string_reader *const reader,
                                        Rdb_string_reader *const unp_reader) {
  const uchar *ptr;
  size_t len = 0;
  bool finished = false;
  uchar *d0 = dst;
  const Field_varstring *const field_var =
      static_cast<Field_varstring *>(field);
  // For simple collations, char_length is also number of bytes.
  DBUG_ASSERT((size_t)fpi->m_max_image_len >= field_var->char_length());
  uchar *dst_end = dst + field_var->pack_length();
  dst += field_var->length_bytes;
  Rdb_bit_reader bit_reader(unp_reader);

  uint space_padding_bytes = 0;
  uint extra_spaces;
  DBUG_ASSERT(unp_reader != nullptr);

  if ((fpi->m_unpack_info_uses_two_bytes
           ? unp_reader->read_uint16(&extra_spaces)
           : unp_reader->read_uint8(&extra_spaces))) {
    return UNPACK_FAILURE;
  }

  if (extra_spaces <= 8) {
    space_padding_bytes = -(static_cast<int>(extra_spaces) - 8);
    extra_spaces = 0;
  } else
    extra_spaces -= 8;

  space_padding_bytes *= fpi->space_xfrm_len;

  /* Decode the length-emitted encoding here */
  while ((ptr = (const uchar *)reader->read(fpi->m_segment_size))) {
    const char last_byte =
        ptr[fpi->m_segment_size - 1]; // number of padding bytes
    size_t used_bytes;
    if (last_byte == VARCHAR_CMP_EQUAL_TO_SPACES) {
      // this is the last one
      if (space_padding_bytes > (fpi->m_segment_size - 1))
        return UNPACK_FAILURE; // Cannot happen, corrupted data
      used_bytes = (fpi->m_segment_size - 1) - space_padding_bytes;
      finished = true;
    } else {
      if (last_byte != VARCHAR_CMP_LESS_THAN_SPACES &&
          last_byte != VARCHAR_CMP_GREATER_THAN_SPACES) {
        return UNPACK_FAILURE;
      }
      used_bytes = fpi->m_segment_size - 1;
    }

    if (dst + used_bytes > dst_end) {
      // The value on disk is longer than the field definition allows?
      return UNPACK_FAILURE;
    }

    uint ret;
    if ((ret = rdb_read_unpack_simple(&bit_reader, fpi->m_charset_codec, ptr,
                                      used_bytes, dst)) != UNPACK_SUCCESS) {
      return ret;
    }

    dst += used_bytes;
    len += used_bytes;

    if (finished) {
      if (extra_spaces) {
        if (dst + extra_spaces > dst_end)
          return UNPACK_FAILURE;
        // pad_char has a 1-byte form in all charsets that
        // are handled by rdb_init_collation_mapping.
        memset(dst, field_var->charset()->pad_char, extra_spaces);
        len += extra_spaces;
      }
      break;
    }
  }

  if (!finished)
    return UNPACK_FAILURE;

  /* Save the length */
  if (field_var->length_bytes == 1) {
    d0[0] = (uchar)len;
  } else {
    DBUG_ASSERT(field_var->length_bytes == 2);
    int2store(d0, len);
  }
  return UNPACK_SUCCESS;
}

/*
  Function of type rdb_make_unpack_info_t

  @detail
    Make unpack_data for CHAR(n) value in a "simple" charset.
    It is CHAR(N), so SQL layer has padded the value with spaces up to N chars.

  @seealso
    The VARCHAR variant is in rdb_make_unpack_simple_varchar
*/

static void rdb_make_unpack_simple(const Rdb_collation_codec *const codec,
                                   const Field *const field,
                                   Rdb_pack_field_context *const pack_ctx) {
  const uchar *const src = field->ptr;
  Rdb_bit_writer bit_writer(pack_ctx->writer);
  rdb_write_unpack_simple(&bit_writer, codec, src, field->pack_length());
}

/*
  Function of type rdb_index_field_unpack_t
*/

static int rdb_unpack_simple(Rdb_field_packing *const fpi,
                             Field *const field MY_ATTRIBUTE((__unused__)),
                             uchar *const dst, Rdb_string_reader *const reader,
                             Rdb_string_reader *const unp_reader) {
  const uchar *ptr;
  const uint len = fpi->m_max_image_len;
  Rdb_bit_reader bit_reader(unp_reader);

  if (!(ptr = (const uchar *)reader->read(len))) {
    return UNPACK_FAILURE;
  }

  return rdb_read_unpack_simple(unp_reader ? &bit_reader : nullptr,
                                fpi->m_charset_codec, ptr, len, dst);
}

// See Rdb_charset_space_info::spaces_xfrm
const int RDB_SPACE_XFRM_SIZE = 32;

// A class holding information about how space character is represented in a
// charset.
class Rdb_charset_space_info {
public:
  Rdb_charset_space_info(const Rdb_charset_space_info &) = delete;
  Rdb_charset_space_info &operator=(const Rdb_charset_space_info &) = delete;
  Rdb_charset_space_info() = default;

  // A few strxfrm'ed space characters, at least RDB_SPACE_XFRM_SIZE bytes
  std::vector<uchar> spaces_xfrm;

  // length(strxfrm(' '))
  size_t space_xfrm_len;

  // length of the space character itself
  // Typically space is just 0x20 (length=1) but in ucs2 it is 0x00 0x20
  // (length=2)
  size_t space_mb_len;
};

static std::array<std::unique_ptr<Rdb_charset_space_info>, MY_ALL_CHARSETS_SIZE>
    rdb_mem_comparable_space;

/*
  @brief
  For a given charset, get
   - strxfrm('    '), a sample that is at least RDB_SPACE_XFRM_SIZE bytes long.
   - length of strxfrm(charset, ' ')
   - length of the space character in the charset

  @param cs  IN    Charset to get the space for
  @param ptr OUT   A few space characters
  @param len OUT   Return length of the space (in bytes)

  @detail
    It is tempting to pre-generate mem-comparable form of space character for
    every charset on server startup.
    One can't do that: some charsets are not initialized until somebody
    attempts to use them (e.g. create or open a table that has a field that
    uses the charset).
*/

static void rdb_get_mem_comparable_space(const CHARSET_INFO *const cs,
                                         const std::vector<uchar> **xfrm,
                                         size_t *const xfrm_len,
                                         size_t *const mb_len) {
  DBUG_ASSERT(cs->number < MY_ALL_CHARSETS_SIZE);
  if (!rdb_mem_comparable_space[cs->number].get()) {
    RDB_MUTEX_LOCK_CHECK(rdb_mem_cmp_space_mutex);
    if (!rdb_mem_comparable_space[cs->number].get()) {
      // Upper bound of how many bytes can be occupied by multi-byte form of a
      // character in any charset.
      const int MAX_MULTI_BYTE_CHAR_SIZE = 4;
      DBUG_ASSERT(cs->mbmaxlen <= MAX_MULTI_BYTE_CHAR_SIZE);

      // multi-byte form of the ' ' (space) character
      uchar space_mb[MAX_MULTI_BYTE_CHAR_SIZE];

      const size_t space_mb_len = cs->cset->wc_mb(
          cs, (my_wc_t)cs->pad_char, space_mb, space_mb + sizeof(space_mb));

      uchar space[20]; // mem-comparable image of the space character

      const size_t space_len = cs->coll->strnxfrm(cs, space, sizeof(space), 1,
                                                  space_mb, space_mb_len, 0);
      Rdb_charset_space_info *const info = new Rdb_charset_space_info;
      info->space_xfrm_len = space_len;
      info->space_mb_len = space_mb_len;
      while (info->spaces_xfrm.size() < RDB_SPACE_XFRM_SIZE) {
        info->spaces_xfrm.insert(info->spaces_xfrm.end(), space,
                                 space + space_len);
      }
      rdb_mem_comparable_space[cs->number].reset(info);
    }
    RDB_MUTEX_UNLOCK_CHECK(rdb_mem_cmp_space_mutex);
  }

  *xfrm = &rdb_mem_comparable_space[cs->number]->spaces_xfrm;
  *xfrm_len = rdb_mem_comparable_space[cs->number]->space_xfrm_len;
  *mb_len = rdb_mem_comparable_space[cs->number]->space_mb_len;
}

mysql_mutex_t rdb_mem_cmp_space_mutex;

std::array<const Rdb_collation_codec *, MY_ALL_CHARSETS_SIZE>
    rdb_collation_data;
mysql_mutex_t rdb_collation_data_mutex;

static bool rdb_is_collation_supported(const my_core::CHARSET_INFO *const cs) {
  return cs->strxfrm_multiply==1 && cs->mbmaxlen == 1 &&
         !(cs->state & (MY_CS_BINSORT | MY_CS_NOPAD));
}

static const Rdb_collation_codec *
rdb_init_collation_mapping(const my_core::CHARSET_INFO *const cs) {
  DBUG_ASSERT(cs && cs->state & MY_CS_AVAILABLE);
  const Rdb_collation_codec *codec = rdb_collation_data[cs->number];

  if (codec == nullptr && rdb_is_collation_supported(cs)) {
    RDB_MUTEX_LOCK_CHECK(rdb_collation_data_mutex);

    codec = rdb_collation_data[cs->number];
    if (codec == nullptr) {
      Rdb_collation_codec *cur = nullptr;

      // Compute reverse mapping for simple collations.
      if (rdb_is_collation_supported(cs)) {
        cur = new Rdb_collation_codec;
        std::map<uchar, std::vector<uchar>> rev_map;
        size_t max_conflict_size = 0;
        for (int src = 0; src < 256; src++) {
          uchar dst = cs->sort_order[src];
          rev_map[dst].push_back(src);
          max_conflict_size = std::max(max_conflict_size, rev_map[dst].size());
        }
        cur->m_dec_idx.resize(max_conflict_size);

        for (auto const &p : rev_map) {
          uchar dst = p.first;
          for (uint idx = 0; idx < p.second.size(); idx++) {
            uchar src = p.second[idx];
            uchar bits =
                my_bit_log2(my_round_up_to_next_power(p.second.size()));
            cur->m_enc_idx[src] = idx;
            cur->m_enc_size[src] = bits;
            cur->m_dec_size[dst] = bits;
            cur->m_dec_idx[idx][dst] = src;
          }
        }

        cur->m_make_unpack_info_func = {
            {rdb_make_unpack_simple_varchar, rdb_make_unpack_simple}};
        cur->m_unpack_func = {
            {rdb_unpack_simple_varchar_space_pad, rdb_unpack_simple}};
      } else {
        // Out of luck for now.
      }

      if (cur != nullptr) {
        codec = cur;
        cur->m_cs = cs;
        rdb_collation_data[cs->number] = cur;
      }
    }

    RDB_MUTEX_UNLOCK_CHECK(rdb_collation_data_mutex);
  }

  return codec;
}

static int get_segment_size_from_collation(const CHARSET_INFO *const cs) {
  int ret;
  if (cs->number == COLLATION_UTF8MB4_BIN || cs->number == COLLATION_UTF16_BIN ||
      cs->number == COLLATION_UTF16LE_BIN || cs->number == COLLATION_UTF32_BIN) {
    /*
      In these collations, a character produces one weight, which is 3 bytes.
      Segment has 3 characters, add one byte for VARCHAR_CMP_* marker, and we
      get 3*3+1=10
    */
    ret = 10;
  } else {
    /*
      All other collations. There are two classes:
      - Unicode-based, except for collations mentioned in the if-condition.
        For these all weights are 2 bytes long, a character may produce 0..8
        weights.
        in any case, 8 bytes of payload in the segment guarantee that the last
        space character won't span across segments.

      - Collations not based on unicode. These have length(strxfrm(' '))=1,
        there nothing to worry about.

      In both cases, take 8 bytes payload + 1 byte for VARCHAR_CMP* marker.
    */
    ret = 9;
  }
  DBUG_ASSERT(ret < RDB_SPACE_XFRM_SIZE);
  return ret;
}

/*
  @brief
    Setup packing of index field into its mem-comparable form

  @detail
    - It is possible produce mem-comparable form for any datatype.
    - Some datatypes also allow to unpack the original value from its
      mem-comparable form.
      = Some of these require extra information to be stored in "unpack_info".
        unpack_info is not a part of mem-comparable form, it is only used to
        restore the original value

  @param
    field  IN  field to be packed/un-packed

  @return
    TRUE  -  Field can be read with index-only reads
    FALSE -  Otherwise
*/

bool Rdb_field_packing::setup(const Rdb_key_def *const key_descr,
                              const Field *const field, const uint &keynr_arg,
                              const uint &key_part_arg,
                              const uint16 &key_length) {
  int res = false;
  enum_field_types type = field ? field->real_type() : MYSQL_TYPE_LONGLONG;

  m_keynr = keynr_arg;
  m_key_part = key_part_arg;

  m_maybe_null = field ? field->real_maybe_null() : false;
  m_unpack_func = nullptr;
  m_make_unpack_info_func = nullptr;
  m_unpack_data_len = 0;
  space_xfrm = nullptr; // safety

  /* Calculate image length. By default, is is pack_length() */
  m_max_image_len =
      field ? field->pack_length() : ROCKSDB_SIZEOF_HIDDEN_PK_COLUMN;
  m_skip_func = rdb_skip_max_length;
  m_pack_func = rdb_pack_with_make_sort_key;

  switch (type) {
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_TINY:
    m_unpack_func = rdb_unpack_integer;
    return true;

  case MYSQL_TYPE_DOUBLE:
    m_unpack_func = rdb_unpack_double;
    return true;

  case MYSQL_TYPE_FLOAT:
    m_unpack_func = rdb_unpack_float;
    return true;

  case MYSQL_TYPE_NEWDECIMAL:
  /*
    Decimal is packed with Field_new_decimal::make_sort_key, which just
    does memcpy.
    Unpacking decimal values was supported only after fix for issue#253,
    because of that ha_rocksdb::get_storage_type() handles decimal values
    in a special way.
  */
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP2:
  /* These are packed with Field_temporal_with_date_and_timef::make_sort_key */
  case MYSQL_TYPE_TIME2: /* TIME is packed with Field_timef::make_sort_key */
  case MYSQL_TYPE_YEAR:  /* YEAR is packed with  Field_tiny::make_sort_key */
    /* Everything that comes here is packed with just a memcpy(). */
    m_unpack_func = rdb_unpack_binary_str;
    return true;

  case MYSQL_TYPE_NEWDATE:
    /*
      This is packed by Field_newdate::make_sort_key. It assumes the data is
      3 bytes, and packing is done by swapping the byte order (for both big-
      and little-endian)
    */
    m_unpack_func = rdb_unpack_newdate;
    return true;
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB: {
    if (key_descr) {
      // The my_charset_bin collation is special in that it will consider
      // shorter strings sorting as less than longer strings.
      //
      // See Field_blob::make_sort_key for details.
      m_max_image_len =
          key_length + (field->charset()->number == COLLATION_BINARY
                            ? reinterpret_cast<const Field_blob *>(field)
                                  ->pack_length_no_ptr()
                            : 0);
      // Return false because indexes on text/blob will always require
      // a prefix. With a prefix, the optimizer will not be able to do an
      // index-only scan since there may be content occuring after the prefix
      // length.
      return false;
    }
  }
  default:
    break;
  }

  m_unpack_info_stores_value = false;
  /* Handle [VAR](CHAR|BINARY) */

  if (type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_STRING) {
    /*
      For CHAR-based columns, check how strxfrm image will take.
      field->field_length = field->char_length() * cs->mbmaxlen.
    */
    const CHARSET_INFO *cs = field->charset();
    m_max_image_len = cs->coll->strnxfrmlen(cs, field->field_length);
  }
  const bool is_varchar = (type == MYSQL_TYPE_VARCHAR);
  const CHARSET_INFO *cs = field->charset();
  // max_image_len before chunking is taken into account
  const int max_image_len_before_chunks = m_max_image_len;

  if (is_varchar) {
    // The default for varchar is variable-length, without space-padding for
    // comparisons
    m_varchar_charset = cs;
    m_skip_func = rdb_skip_variable_length;
    m_pack_func = rdb_pack_with_varchar_encoding;
    m_max_image_len =
        (m_max_image_len / (RDB_ESCAPE_LENGTH - 1) + 1) * RDB_ESCAPE_LENGTH;

    const auto field_var = static_cast<const Field_varstring *>(field);
    m_unpack_info_uses_two_bytes = (field_var->field_length + 8 >= 0x100);
  }

  if (type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_STRING) {
    // See http://dev.mysql.com/doc/refman/5.7/en/string-types.html for
    // information about character-based datatypes are compared.
    bool use_unknown_collation = false;
    DBUG_EXECUTE_IF("myrocks_enable_unknown_collation_index_only_scans",
                    use_unknown_collation = true;);

    if (cs->number == COLLATION_BINARY) {
      // - SQL layer pads BINARY(N) so that it always is N bytes long.
      // - For VARBINARY(N), values may have different lengths, so we're using
      //   variable-length encoding. This is also the only charset where the
      //   values are not space-padded for comparison.
      m_unpack_func = is_varchar ? rdb_unpack_binary_or_utf8_varchar
                                 : rdb_unpack_binary_str;
      res = true;
    } else if (cs->number == COLLATION_LATIN1_BIN || cs->number == COLLATION_UTF8_BIN) {
      // For _bin collations, mem-comparable form of the string is the string
      // itself.

      if (is_varchar) {
        // VARCHARs - are compared as if they were space-padded - but are
        // not actually space-padded (reading the value back produces the
        // original value, without the padding)
        m_unpack_func = rdb_unpack_binary_or_utf8_varchar_space_pad;
        m_skip_func = rdb_skip_variable_space_pad;
        m_pack_func = rdb_pack_with_varchar_space_pad;
        m_make_unpack_info_func = rdb_dummy_make_unpack_info;
        m_segment_size = get_segment_size_from_collation(cs);
        m_max_image_len =
            (max_image_len_before_chunks / (m_segment_size - 1) + 1) *
            m_segment_size;
        rdb_get_mem_comparable_space(cs, &space_xfrm, &space_xfrm_len,
                                     &space_mb_len);
      } else {
        // SQL layer pads CHAR(N) values to their maximum length.
        // We just store that and restore it back.
        m_unpack_func = (cs->number == COLLATION_LATIN1_BIN) ? rdb_unpack_binary_str
                                                       : rdb_unpack_utf8_str;
      }
      res = true;
    } else {
      // This is [VAR]CHAR(n) and the collation is not $(charset_name)_bin

      res = true; // index-only scans are possible
      m_unpack_data_len = is_varchar ? 0 : field->field_length;
      const uint idx = is_varchar ? 0 : 1;
      const Rdb_collation_codec *codec = nullptr;

      if (is_varchar) {
        // VARCHAR requires space-padding for doing comparisons
        //
        // The check for cs->levels_for_order is to catch
        // latin2_czech_cs and cp1250_czech_cs - multi-level collations
        // that Variable-Length Space Padded Encoding can't handle.
        // It is not expected to work for any other multi-level collations,
        // either.
        // Currently we handle these collations as NO_PAD, even if they have
        // PAD_SPACE attribute.
        if (cs->levels_for_order == 1) {
          m_pack_func = rdb_pack_with_varchar_space_pad;
          m_skip_func = rdb_skip_variable_space_pad;
          m_segment_size = get_segment_size_from_collation(cs);
          m_max_image_len =
              (max_image_len_before_chunks / (m_segment_size - 1) + 1) *
              m_segment_size;
          rdb_get_mem_comparable_space(cs, &space_xfrm, &space_xfrm_len,
                                       &space_mb_len);
        } else {
          //  NO_LINT_DEBUG
          sql_print_warning("RocksDB: you're trying to create an index "
                            "with a multi-level collation %s",
                            cs->name);
          //  NO_LINT_DEBUG
          sql_print_warning("MyRocks will handle this collation internally "
                            " as if it had a NO_PAD attribute.");
          m_pack_func = rdb_pack_with_varchar_encoding;
          m_skip_func = rdb_skip_variable_length;
        }
      }

      if ((codec = rdb_init_collation_mapping(cs)) != nullptr) {
        // The collation allows to store extra information in the unpack_info
        // which can be used to restore the original value from the
        // mem-comparable form.
        m_make_unpack_info_func = codec->m_make_unpack_info_func[idx];
        m_unpack_func = codec->m_unpack_func[idx];
        m_charset_codec = codec;
      } else if (use_unknown_collation) {
        // We have no clue about how this collation produces mem-comparable
        // form. Our way of restoring the original value is to keep a copy of
        // the original value in unpack_info.
        m_unpack_info_stores_value = true;
        m_make_unpack_info_func = is_varchar ? rdb_make_unpack_unknown_varchar
                                             : rdb_make_unpack_unknown;
        m_unpack_func =
            is_varchar ? rdb_unpack_unknown_varchar : rdb_unpack_unknown;
      } else {
        // Same as above: we don't know how to restore the value from its
        // mem-comparable form.
        // Here, we just indicate to the SQL layer we can't do it.
        DBUG_ASSERT(m_unpack_func == nullptr);
        m_unpack_info_stores_value = false;
        res = false; // Indicate that index-only reads are not possible
      }
    }

    // Make an adjustment: unpacking partially covered columns is not
    // possible. field->table is populated when called through
    // Rdb_key_def::setup, but not during ha_rocksdb::index_flags.
    if (field->table) {
      // Get the original Field object and compare lengths. If this key part is
      // a prefix of a column, then we can't do index-only scans.
      if (field->table->field[field->field_index]->field_length != key_length) {
        m_unpack_func = nullptr;
        m_make_unpack_info_func = nullptr;
        m_unpack_info_stores_value = true;
        res = false;
      }
    } else {
      if (field->field_length != key_length) {
        m_unpack_func = nullptr;
        m_make_unpack_info_func = nullptr;
        m_unpack_info_stores_value = true;
        res = false;
      }
    }
  }
  return res;
}

Field *Rdb_field_packing::get_field_in_table(const TABLE *const tbl) const {
  return tbl->key_info[m_keynr].key_part[m_key_part].field;
}

void Rdb_field_packing::fill_hidden_pk_val(uchar **dst,
                                           const longlong &hidden_pk_id) const {
  DBUG_ASSERT(m_max_image_len == 8);

  String to;
  rdb_netstr_append_uint64(&to, hidden_pk_id);
  memcpy(*dst, to.ptr(), m_max_image_len);

  *dst += m_max_image_len;
}

///////////////////////////////////////////////////////////////////////////////////////////
// Rdb_ddl_manager
///////////////////////////////////////////////////////////////////////////////////////////

Rdb_tbl_def::~Rdb_tbl_def() {
  auto ddl_manager = rdb_get_ddl_manager();
  /* Don't free key definitions */
  if (m_key_descr_arr) {
    for (uint i = 0; i < m_key_count; i++) {
      if (ddl_manager && m_key_descr_arr[i]) {
        ddl_manager->erase_index_num(m_key_descr_arr[i]->get_gl_index_id());
      }

      m_key_descr_arr[i] = nullptr;
    }

    delete[] m_key_descr_arr;
    m_key_descr_arr = nullptr;
  }
}

/*
  Put table definition DDL entry. Actual write is done at
  Rdb_dict_manager::commit.

  We write
    dbname.tablename -> version + {key_entry, key_entry, key_entry, ... }

  Where key entries are a tuple of
    ( cf_id, index_nr )
*/

bool Rdb_tbl_def::put_dict(Rdb_dict_manager *const dict,
                           rocksdb::WriteBatch *const batch, uchar *const key,
                           const size_t &keylen) {
  StringBuffer<8 * Rdb_key_def::PACKED_SIZE> indexes;
  indexes.alloc(Rdb_key_def::VERSION_SIZE +
                m_key_count * Rdb_key_def::PACKED_SIZE * 2);
  rdb_netstr_append_uint16(&indexes, Rdb_key_def::DDL_ENTRY_INDEX_VERSION);

  for (uint i = 0; i < m_key_count; i++) {
    const Rdb_key_def &kd = *m_key_descr_arr[i];

    uchar flags =
        (kd.m_is_reverse_cf ? Rdb_key_def::REVERSE_CF_FLAG : 0) |
        (kd.m_is_auto_cf ? Rdb_key_def::AUTO_CF_FLAG : 0) |
        (kd.m_is_per_partition_cf ? Rdb_key_def::PER_PARTITION_CF_FLAG : 0);

    const uint cf_id = kd.get_cf()->GetID();
    /*
      If cf_id already exists, cf_flags must be the same.
      To prevent race condition, reading/modifying/committing CF flags
      need to be protected by mutex (dict_manager->lock()).
      When RocksDB supports transaction with pessimistic concurrency
      control, we can switch to use it and removing mutex.
    */
    uint existing_cf_flags;
    const std::string cf_name = kd.get_cf()->GetName();

    if (dict->get_cf_flags(cf_id, &existing_cf_flags)) {
      // For the purposes of comparison we'll clear the partitioning bit. The
      // intent here is to make sure that both partitioned and non-partitioned
      // tables can refer to the same CF.
      existing_cf_flags &= ~Rdb_key_def::CF_FLAGS_TO_IGNORE;
      flags &= ~Rdb_key_def::CF_FLAGS_TO_IGNORE;

      if (existing_cf_flags != flags) {
        my_printf_error(ER_UNKNOWN_ERROR,
                        "Column family ('%s') flag (%d) is different from an "
                        "existing flag (%d). Assign a new CF flag, or do not "
                        "change existing CF flag.", MYF(0), cf_name.c_str(),
                        flags, existing_cf_flags);
        return true;
      }
    } else {
      dict->add_cf_flags(batch, cf_id, flags);
    }

    rdb_netstr_append_uint32(&indexes, cf_id);
    rdb_netstr_append_uint32(&indexes, kd.m_index_number);
    dict->add_or_update_index_cf_mapping(batch, kd.m_index_type,
                                         kd.m_kv_format_version,
                                         kd.m_index_number, cf_id);
  }

  const rocksdb::Slice skey((char *)key, keylen);
  const rocksdb::Slice svalue(indexes.c_ptr(), indexes.length());

  dict->put_key(batch, skey, svalue);
  return false;
}

void Rdb_tbl_def::check_if_is_mysql_system_table() {
  static const char *const system_dbs[] = {
      "mysql", "performance_schema", "information_schema",
  };

  m_is_mysql_system_table = false;
  for (uint ii = 0; ii < array_elements(system_dbs); ii++) {
    if (strcmp(m_dbname.c_str(), system_dbs[ii]) == 0) {
      m_is_mysql_system_table = true;
      break;
    }
  }
}

void Rdb_tbl_def::set_name(const std::string &name) {
  int err MY_ATTRIBUTE((__unused__));

  m_dbname_tablename = name;
  err = rdb_split_normalized_tablename(name, &m_dbname, &m_tablename,
                                       &m_partition);
  DBUG_ASSERT(err == 0);

  check_if_is_mysql_system_table();
}

/*
  Static function of type my_hash_get_key that gets invoked by
  the m_ddl_hash object of type my_core::HASH.
  It manufactures a key (db+table name in our case) from a record
  (Rdb_tbl_def in our case).
*/
const uchar *
Rdb_ddl_manager::get_hash_key(Rdb_tbl_def *const rec, size_t *const length,
                              my_bool not_used MY_ATTRIBUTE((__unused__))) {
  const std::string &dbname_tablename = rec->full_tablename();
  *length = dbname_tablename.size();
  return reinterpret_cast<const uchar *>(dbname_tablename.c_str());
}

/*
  Static function of type void (*my_hash_free_element_func_t)(void*) that gets
  invoked by the m_ddl_hash object of type my_core::HASH.
  It deletes a record (Rdb_tbl_def in our case).
*/
void Rdb_ddl_manager::free_hash_elem(void *const data) {
  Rdb_tbl_def *elem = reinterpret_cast<Rdb_tbl_def *>(data);
  delete elem;
}

void Rdb_ddl_manager::erase_index_num(const GL_INDEX_ID &gl_index_id) {
  m_index_num_to_keydef.erase(gl_index_id);
}

void Rdb_ddl_manager::add_uncommitted_keydefs(
    const std::unordered_set<std::shared_ptr<Rdb_key_def>> &indexes) {
  mysql_rwlock_wrlock(&m_rwlock);
  for (const auto &index : indexes) {
    m_index_num_to_uncommitted_keydef[index->get_gl_index_id()] = index;
  }
  mysql_rwlock_unlock(&m_rwlock);
}

void Rdb_ddl_manager::remove_uncommitted_keydefs(
    const std::unordered_set<std::shared_ptr<Rdb_key_def>> &indexes) {
  mysql_rwlock_wrlock(&m_rwlock);
  for (const auto &index : indexes) {
    m_index_num_to_uncommitted_keydef.erase(index->get_gl_index_id());
  }
  mysql_rwlock_unlock(&m_rwlock);
}

namespace // anonymous namespace = not visible outside this source file
{
struct Rdb_validate_tbls : public Rdb_tables_scanner {
  using tbl_info_t = std::pair<std::string, bool>;
  using tbl_list_t = std::map<std::string, std::set<tbl_info_t>>;

  tbl_list_t m_list;

  int add_table(Rdb_tbl_def *tdef) override;

  bool compare_to_actual_tables(const std::string &datadir, bool *has_errors);

  bool scan_for_frms(const std::string &datadir, const std::string &dbname,
                     bool *has_errors);

  bool check_frm_file(const std::string &fullpath, const std::string &dbname,
                      const std::string &tablename, bool *has_errors);
};
} // anonymous namespace

/*
  Get a list of tables that we expect to have .frm files for.  This will use the
  information just read from the RocksDB data dictionary.
*/
int Rdb_validate_tbls::add_table(Rdb_tbl_def *tdef) {
  DBUG_ASSERT(tdef != nullptr);

  /* Add the database/table into the list */
  bool is_partition = tdef->base_partition().size() != 0;
  m_list[tdef->base_dbname()].insert(
      tbl_info_t(tdef->base_tablename(), is_partition));

  return HA_EXIT_SUCCESS;
}

/*
  Access the .frm file for this dbname/tablename and see if it is a RocksDB
  table (or partition table).
*/
bool Rdb_validate_tbls::check_frm_file(const std::string &fullpath,
                                       const std::string &dbname,
                                       const std::string &tablename,
                                       bool *has_errors) {
  /* Check this .frm file to see what engine it uses */
  String fullfilename(fullpath.c_str(), &my_charset_bin);
  fullfilename.append(FN_DIRSEP);
  fullfilename.append(tablename.c_str());
  fullfilename.append(".frm");

  /*
    This function will return the legacy_db_type of the table.  Currently
    it does not reference the first parameter (THD* thd), but if it ever
    did in the future we would need to make a version that does it without
    the connection handle as we don't have one here.
  */
  char eng_type_buf[NAME_CHAR_LEN+1];
  LEX_STRING eng_type_str = {eng_type_buf, 0}; 
  //enum legacy_db_type eng_type;
  frm_type_enum type = dd_frm_type(nullptr, fullfilename.c_ptr(), &eng_type_str);
  if (type == FRMTYPE_ERROR) {
    sql_print_warning("RocksDB: Failed to open/read .from file: %s",
                      fullfilename.ptr());
    return false;
  }

  if (type == FRMTYPE_TABLE) {
    /* For a RocksDB table do we have a reference in the data dictionary? */
    if (!strncmp(eng_type_str.str, "ROCKSDB", eng_type_str.length)) {
      /*
        Attempt to remove the table entry from the list of tables.  If this
        fails then we know we had a .frm file that wasn't registered in RocksDB.
      */
      tbl_info_t element(tablename, false);
      if (m_list.count(dbname) == 0 || m_list[dbname].erase(element) == 0) {
        sql_print_warning("RocksDB: Schema mismatch - "
                          "A .frm file exists for table %s.%s, "
                          "but that table is not registered in RocksDB",
                          dbname.c_str(), tablename.c_str());
        *has_errors = true;
      }
    } else if (!strncmp(eng_type_str.str, "partition", eng_type_str.length)) {
      /*
        For partition tables, see if it is in the m_list as a partition,
        but don't generate an error if it isn't there - we don't know that the
        .frm is for RocksDB.
      */
      if (m_list.count(dbname) > 0) {
        m_list[dbname].erase(tbl_info_t(tablename, true));
      }
    }
  }

  return true;
}

/* Scan the database subdirectory for .frm files */
bool Rdb_validate_tbls::scan_for_frms(const std::string &datadir,
                                      const std::string &dbname,
                                      bool *has_errors) {
  bool result = true;
  std::string fullpath = datadir + dbname;
  struct st_my_dir *dir_info = my_dir(fullpath.c_str(), MYF(MY_DONT_SORT));

  /* Access the directory */
  if (dir_info == nullptr) {
    sql_print_warning("RocksDB: Could not open database directory: %s",
                      fullpath.c_str());
    return false;
  }

  /* Scan through the files in the directory */
  struct fileinfo *file_info = dir_info->dir_entry;
  for (uint ii = 0; ii < dir_info->number_of_files; ii++, file_info++) {
    /* Find .frm files that are not temp files (those that start with '#') */
    const char *ext = strrchr(file_info->name, '.');
    if (ext != nullptr && !is_prefix(file_info->name, tmp_file_prefix) &&
        strcmp(ext, ".frm") == 0) {
      std::string tablename =
          std::string(file_info->name, ext - file_info->name);

      /* Check to see if the .frm file is from RocksDB */
      if (!check_frm_file(fullpath, dbname, tablename, has_errors)) {
        result = false;
        break;
      }
    }
  }

  /* Remove any databases who have no more tables listed */
  if (m_list.count(dbname) == 1 && m_list[dbname].size() == 0) {
    m_list.erase(dbname);
  }

  /* Release the directory entry */
  my_dirend(dir_info);

  return result;
}

/*
  Scan the datadir for all databases (subdirectories) and get a list of .frm
  files they contain
*/
bool Rdb_validate_tbls::compare_to_actual_tables(const std::string &datadir,
                                                 bool *has_errors) {
  bool result = true;
  struct st_my_dir *dir_info;
  struct fileinfo *file_info;

  dir_info = my_dir(datadir.c_str(), MYF(MY_DONT_SORT | MY_WANT_STAT));
  if (dir_info == nullptr) {
    sql_print_warning("RocksDB: could not open datadir: %s", datadir.c_str());
    return false;
  }

  file_info = dir_info->dir_entry;
  for (uint ii = 0; ii < dir_info->number_of_files; ii++, file_info++) {
    /* Ignore files/dirs starting with '.' */
    if (file_info->name[0] == '.')
      continue;

    /* Ignore all non-directory files */
    if (!MY_S_ISDIR(file_info->mystat->st_mode))
      continue;

    /* Scan all the .frm files in the directory */
    if (!scan_for_frms(datadir, file_info->name, has_errors)) {
      result = false;
      break;
    }
  }

  /* Release the directory info */
  my_dirend(dir_info);

  return result;
}

/*
  Validate that all the tables in the RocksDB database dictionary match the .frm
  files in the datdir
*/
bool Rdb_ddl_manager::validate_schemas(void) {
  bool has_errors = false;
  const std::string datadir = std::string(mysql_real_data_home);
  Rdb_validate_tbls table_list;

  /* Get the list of tables from the database dictionary */
  if (scan_for_tables(&table_list) != 0) {
    return false;
  }

  /* Compare that to the list of actual .frm files */
  if (!table_list.compare_to_actual_tables(datadir, &has_errors)) {
    return false;
  }

  /*
    Any tables left in the tables list are ones that are registered in RocksDB
    but don't have .frm files.
  */
  for (const auto &db : table_list.m_list) {
    for (const auto &table : db.second) {
      sql_print_warning("RocksDB: Schema mismatch - "
                        "Table %s.%s is registered in RocksDB "
                        "but does not have a .frm file",
                        db.first.c_str(), table.first.c_str());
      has_errors = true;
    }
  }

  return !has_errors;
}

bool Rdb_ddl_manager::init(Rdb_dict_manager *const dict_arg,
                           Rdb_cf_manager *const cf_manager,
                           const uint32_t &validate_tables) {
  const ulong TABLE_HASH_SIZE = 32;
  m_dict = dict_arg;
  mysql_rwlock_init(0, &m_rwlock);
  (void)my_hash_init(&m_ddl_hash,
                     /*system_charset_info*/ &my_charset_bin, TABLE_HASH_SIZE,
                     0, 0, (my_hash_get_key)Rdb_ddl_manager::get_hash_key,
                     Rdb_ddl_manager::free_hash_elem, 0);

  /* Read the data dictionary and populate the hash */
  uchar ddl_entry[Rdb_key_def::INDEX_NUMBER_SIZE];
  rdb_netbuf_store_index(ddl_entry, Rdb_key_def::DDL_ENTRY_INDEX_START_NUMBER);
  const rocksdb::Slice ddl_entry_slice((char *)ddl_entry,
                                       Rdb_key_def::INDEX_NUMBER_SIZE);

  /* Reading data dictionary should always skip bloom filter */
  rocksdb::Iterator *it = m_dict->new_iterator();
  int i = 0;

  uint max_index_id_in_dict = 0;
  m_dict->get_max_index_id(&max_index_id_in_dict);

  for (it->Seek(ddl_entry_slice); it->Valid(); it->Next()) {
    const uchar *ptr;
    const uchar *ptr_end;
    const rocksdb::Slice key = it->key();
    const rocksdb::Slice val = it->value();

    if (key.size() >= Rdb_key_def::INDEX_NUMBER_SIZE &&
        memcmp(key.data(), ddl_entry, Rdb_key_def::INDEX_NUMBER_SIZE))
      break;

    if (key.size() <= Rdb_key_def::INDEX_NUMBER_SIZE) {
      sql_print_error("RocksDB: Table_store: key has length %d (corruption?)",
                      (int)key.size());
      return true;
    }

    Rdb_tbl_def *const tdef =
        new Rdb_tbl_def(key, Rdb_key_def::INDEX_NUMBER_SIZE);

    // Now, read the DDLs.
    const int real_val_size = val.size() - Rdb_key_def::VERSION_SIZE;
    if (real_val_size % Rdb_key_def::PACKED_SIZE * 2) {
      sql_print_error("RocksDB: Table_store: invalid keylist for table %s",
                      tdef->full_tablename().c_str());
      return true;
    }
    tdef->m_key_count = real_val_size / (Rdb_key_def::PACKED_SIZE * 2);
    tdef->m_key_descr_arr = new std::shared_ptr<Rdb_key_def>[tdef->m_key_count];

    ptr = reinterpret_cast<const uchar *>(val.data());
    const int version = rdb_netbuf_read_uint16(&ptr);
    if (version != Rdb_key_def::DDL_ENTRY_INDEX_VERSION) {
      sql_print_error("RocksDB: DDL ENTRY Version was not expected."
                      "Expected: %d, Actual: %d",
                      Rdb_key_def::DDL_ENTRY_INDEX_VERSION, version);
      return true;
    }
    ptr_end = ptr + real_val_size;
    for (uint keyno = 0; ptr < ptr_end; keyno++) {
      GL_INDEX_ID gl_index_id;
      rdb_netbuf_read_gl_index(&ptr, &gl_index_id);
      uint16 m_index_dict_version = 0;
      uchar m_index_type = 0;
      uint16 kv_version = 0;
      uint flags = 0;
      if (!m_dict->get_index_info(gl_index_id, &m_index_dict_version,
                                  &m_index_type, &kv_version)) {
        sql_print_error("RocksDB: Could not get index information "
                        "for Index Number (%u,%u), table %s",
                        gl_index_id.cf_id, gl_index_id.index_id,
                        tdef->full_tablename().c_str());
        return true;
      }
      if (max_index_id_in_dict < gl_index_id.index_id) {
        sql_print_error("RocksDB: Found max index id %u from data dictionary "
                        "but also found larger index id %u from dictionary. "
                        "This should never happen and possibly a bug.",
                        max_index_id_in_dict, gl_index_id.index_id);
        return true;
      }
      if (!m_dict->get_cf_flags(gl_index_id.cf_id, &flags)) {
        sql_print_error("RocksDB: Could not get Column Family Flags "
                        "for CF Number %d, table %s",
                        gl_index_id.cf_id, tdef->full_tablename().c_str());
        return true;
      }

      rocksdb::ColumnFamilyHandle *const cfh =
          cf_manager->get_cf(gl_index_id.cf_id);
      DBUG_ASSERT(cfh != nullptr);

      /*
        We can't fully initialize Rdb_key_def object here, because full
        initialization requires that there is an open TABLE* where we could
        look at Field* objects and set max_length and other attributes
      */
      tdef->m_key_descr_arr[keyno] = std::make_shared<Rdb_key_def>(
          gl_index_id.index_id, keyno, cfh, m_index_dict_version, m_index_type,
          kv_version, flags & Rdb_key_def::REVERSE_CF_FLAG,
          flags & Rdb_key_def::AUTO_CF_FLAG,
          flags & Rdb_key_def::PER_PARTITION_CF_FLAG, "",
          m_dict->get_stats(gl_index_id));
    }
    put(tdef);
    i++;
  }

  /*
    If validate_tables is greater than 0 run the validation.  Only fail the
    initialzation if the setting is 1.  If the setting is 2 we continue.
  */
  if (validate_tables > 0 && !validate_schemas()) {
    if (validate_tables == 1) {
      sql_print_error("RocksDB: Problems validating data dictionary "
                      "against .frm files, exiting");
      return true;
    }
  }

  // index ids used by applications should not conflict with
  // data dictionary index ids
  if (max_index_id_in_dict < Rdb_key_def::END_DICT_INDEX_ID) {
    max_index_id_in_dict = Rdb_key_def::END_DICT_INDEX_ID;
  }

  m_sequence.init(max_index_id_in_dict + 1);

  if (!it->status().ok()) {
    const std::string s = it->status().ToString();
    sql_print_error("RocksDB: Table_store: load error: %s", s.c_str());
    return true;
  }
  delete it;
  sql_print_information("RocksDB: Table_store: loaded DDL data for %d tables",
                        i);
  return false;
}

Rdb_tbl_def *Rdb_ddl_manager::find(const std::string &table_name,
                                   const bool &lock) {
  if (lock) {
    mysql_rwlock_rdlock(&m_rwlock);
  }

  Rdb_tbl_def *const rec = reinterpret_cast<Rdb_tbl_def *>(my_hash_search(
      &m_ddl_hash, reinterpret_cast<const uchar *>(table_name.c_str()),
      table_name.size()));

  if (lock) {
    mysql_rwlock_unlock(&m_rwlock);
  }

  return rec;
}

// this is a safe version of the find() function below.  It acquires a read
// lock on m_rwlock to make sure the Rdb_key_def is not discarded while we
// are finding it.  Copying it into 'ret' increments the count making sure
// that the object will not be discarded until we are finished with it.
std::shared_ptr<const Rdb_key_def>
Rdb_ddl_manager::safe_find(GL_INDEX_ID gl_index_id) {
  std::shared_ptr<const Rdb_key_def> ret(nullptr);

  mysql_rwlock_rdlock(&m_rwlock);

  auto it = m_index_num_to_keydef.find(gl_index_id);
  if (it != m_index_num_to_keydef.end()) {
    const auto table_def = find(it->second.first, false);
    if (table_def && it->second.second < table_def->m_key_count) {
      const auto &kd = table_def->m_key_descr_arr[it->second.second];
      if (kd->max_storage_fmt_length() != 0) {
        ret = kd;
      }
    }
  } else {
    auto it = m_index_num_to_uncommitted_keydef.find(gl_index_id);
    if (it != m_index_num_to_uncommitted_keydef.end()) {
      const auto &kd = it->second;
      if (kd->max_storage_fmt_length() != 0) {
        ret = kd;
      }
    }
  }

  mysql_rwlock_unlock(&m_rwlock);

  return ret;
}

// this method assumes at least read-only lock on m_rwlock
const std::shared_ptr<Rdb_key_def> &
Rdb_ddl_manager::find(GL_INDEX_ID gl_index_id) {
  auto it = m_index_num_to_keydef.find(gl_index_id);
  if (it != m_index_num_to_keydef.end()) {
    auto table_def = find(it->second.first, false);
    if (table_def) {
      if (it->second.second < table_def->m_key_count) {
        return table_def->m_key_descr_arr[it->second.second];
      }
    }
  } else {
    auto it = m_index_num_to_uncommitted_keydef.find(gl_index_id);
    if (it != m_index_num_to_uncommitted_keydef.end()) {
      return it->second;
    }
  }

  static std::shared_ptr<Rdb_key_def> empty = nullptr;

  return empty;
}

void Rdb_ddl_manager::set_stats(
    const std::unordered_map<GL_INDEX_ID, Rdb_index_stats> &stats) {
  mysql_rwlock_wrlock(&m_rwlock);
  for (auto src : stats) {
    const auto &keydef = find(src.second.m_gl_index_id);
    if (keydef) {
      keydef->m_stats = src.second;
      m_stats2store[keydef->m_stats.m_gl_index_id] = keydef->m_stats;
    }
  }
  mysql_rwlock_unlock(&m_rwlock);
}

void Rdb_ddl_manager::adjust_stats(
    const std::vector<Rdb_index_stats> &new_data,
    const std::vector<Rdb_index_stats> &deleted_data) {
  mysql_rwlock_wrlock(&m_rwlock);
  int i = 0;
  for (const auto &data : {new_data, deleted_data}) {
    for (const auto &src : data) {
      const auto &keydef = find(src.m_gl_index_id);
      if (keydef) {
        keydef->m_stats.m_distinct_keys_per_prefix.resize(
            keydef->get_key_parts());
        keydef->m_stats.merge(src, i == 0, keydef->max_storage_fmt_length());
        m_stats2store[keydef->m_stats.m_gl_index_id] = keydef->m_stats;
      }
    }
    i++;
  }
  const bool should_save_stats = !m_stats2store.empty();
  mysql_rwlock_unlock(&m_rwlock);
  if (should_save_stats) {
    // Queue an async persist_stats(false) call to the background thread.
    rdb_queue_save_stats_request();
  }
}

void Rdb_ddl_manager::persist_stats(const bool &sync) {
  mysql_rwlock_wrlock(&m_rwlock);
  const auto local_stats2store = std::move(m_stats2store);
  m_stats2store.clear();
  mysql_rwlock_unlock(&m_rwlock);

  // Persist stats
  const std::unique_ptr<rocksdb::WriteBatch> wb = m_dict->begin();
  std::vector<Rdb_index_stats> stats;
  std::transform(local_stats2store.begin(), local_stats2store.end(),
                 std::back_inserter(stats),
                 [](const std::pair<GL_INDEX_ID, Rdb_index_stats> &s) {
                   return s.second;
                 });
  m_dict->add_stats(wb.get(), stats);
  m_dict->commit(wb.get(), sync);
}

/*
  Put table definition of `tbl` into the mapping, and also write it to the
  on-disk data dictionary.
*/

int Rdb_ddl_manager::put_and_write(Rdb_tbl_def *const tbl,
                                   rocksdb::WriteBatch *const batch) {
  uchar buf[FN_LEN * 2 + Rdb_key_def::INDEX_NUMBER_SIZE];
  uint pos = 0;

  rdb_netbuf_store_index(buf, Rdb_key_def::DDL_ENTRY_INDEX_START_NUMBER);
  pos += Rdb_key_def::INDEX_NUMBER_SIZE;

  const std::string &dbname_tablename = tbl->full_tablename();
  memcpy(buf + pos, dbname_tablename.c_str(), dbname_tablename.size());
  pos += dbname_tablename.size();

  int res;
  if ((res = tbl->put_dict(m_dict, batch, buf, pos))) {
    return res;
  }
  if ((res = put(tbl))) {
    return res;
  }
  return HA_EXIT_SUCCESS;
}

/* Return 0 - ok, other value - error */
/* TODO:
  This function modifies m_ddl_hash and m_index_num_to_keydef.
  However, these changes need to be reversed if dict_manager.commit fails
  See the discussion here: https://reviews.facebook.net/D35925#inline-259167
  Tracked by https://github.com/facebook/mysql-5.6/issues/33
*/
int Rdb_ddl_manager::put(Rdb_tbl_def *const tbl, const bool &lock) {
  Rdb_tbl_def *rec;
  my_bool result;
  const std::string &dbname_tablename = tbl->full_tablename();

  if (lock)
    mysql_rwlock_wrlock(&m_rwlock);

  // We have to do this find because 'tbl' is not yet in the list.  We need
  // to find the one we are replacing ('rec')
  rec = find(dbname_tablename, false);
  if (rec) {
    // this will free the old record.
    my_hash_delete(&m_ddl_hash, reinterpret_cast<uchar *>(rec));
  }
  result = my_hash_insert(&m_ddl_hash, reinterpret_cast<uchar *>(tbl));

  for (uint keyno = 0; keyno < tbl->m_key_count; keyno++) {
    m_index_num_to_keydef[tbl->m_key_descr_arr[keyno]->get_gl_index_id()] =
        std::make_pair(dbname_tablename, keyno);
  }

  if (lock)
    mysql_rwlock_unlock(&m_rwlock);
  return result;
}

void Rdb_ddl_manager::remove(Rdb_tbl_def *const tbl,
                             rocksdb::WriteBatch *const batch,
                             const bool &lock) {
  if (lock)
    mysql_rwlock_wrlock(&m_rwlock);

  uchar buf[FN_LEN * 2 + Rdb_key_def::INDEX_NUMBER_SIZE];
  uint pos = 0;

  rdb_netbuf_store_index(buf, Rdb_key_def::DDL_ENTRY_INDEX_START_NUMBER);
  pos += Rdb_key_def::INDEX_NUMBER_SIZE;

  const std::string &dbname_tablename = tbl->full_tablename();
  memcpy(buf + pos, dbname_tablename.c_str(), dbname_tablename.size());
  pos += dbname_tablename.size();

  const rocksdb::Slice tkey((char *)buf, pos);
  m_dict->delete_key(batch, tkey);

  /* The following will also delete the object: */
  my_hash_delete(&m_ddl_hash, reinterpret_cast<uchar *>(tbl));

  if (lock)
    mysql_rwlock_unlock(&m_rwlock);
}

bool Rdb_ddl_manager::rename(const std::string &from, const std::string &to,
                             rocksdb::WriteBatch *const batch) {
  Rdb_tbl_def *rec;
  Rdb_tbl_def *new_rec;
  bool res = true;
  uchar new_buf[FN_LEN * 2 + Rdb_key_def::INDEX_NUMBER_SIZE];
  uint new_pos = 0;

  mysql_rwlock_wrlock(&m_rwlock);
  if (!(rec = find(from, false))) {
    mysql_rwlock_unlock(&m_rwlock);
    return true;
  }

  new_rec = new Rdb_tbl_def(to);

  new_rec->m_key_count = rec->m_key_count;
  new_rec->m_auto_incr_val =
      rec->m_auto_incr_val.load(std::memory_order_relaxed);
  new_rec->m_key_descr_arr = rec->m_key_descr_arr;
  // so that it's not free'd when deleting the old rec
  rec->m_key_descr_arr = nullptr;

  // Create a new key
  rdb_netbuf_store_index(new_buf, Rdb_key_def::DDL_ENTRY_INDEX_START_NUMBER);
  new_pos += Rdb_key_def::INDEX_NUMBER_SIZE;

  const std::string &dbname_tablename = new_rec->full_tablename();
  memcpy(new_buf + new_pos, dbname_tablename.c_str(), dbname_tablename.size());
  new_pos += dbname_tablename.size();

  // Create a key to add
  if (!new_rec->put_dict(m_dict, batch, new_buf, new_pos)) {
    remove(rec, batch, false);
    put(new_rec, false);
    res = false; // ok
  }

  mysql_rwlock_unlock(&m_rwlock);
  return res;
}

void Rdb_ddl_manager::cleanup() {
  my_hash_free(&m_ddl_hash);
  mysql_rwlock_destroy(&m_rwlock);
  m_sequence.cleanup();
}

int Rdb_ddl_manager::scan_for_tables(Rdb_tables_scanner *const tables_scanner) {
  int i, ret;
  Rdb_tbl_def *rec;

  DBUG_ASSERT(tables_scanner != nullptr);

  mysql_rwlock_rdlock(&m_rwlock);

  ret = 0;
  i = 0;

  while ((
      rec = reinterpret_cast<Rdb_tbl_def *>(my_hash_element(&m_ddl_hash, i)))) {
    ret = tables_scanner->add_table(rec);
    if (ret)
      break;
    i++;
  }

  mysql_rwlock_unlock(&m_rwlock);
  return ret;
}

/*
  Rdb_binlog_manager class implementation
*/

bool Rdb_binlog_manager::init(Rdb_dict_manager *const dict_arg) {
  DBUG_ASSERT(dict_arg != nullptr);
  m_dict = dict_arg;

  rdb_netbuf_store_index(m_key_buf, Rdb_key_def::BINLOG_INFO_INDEX_NUMBER);
  m_key_slice = rocksdb::Slice(reinterpret_cast<char *>(m_key_buf),
                               Rdb_key_def::INDEX_NUMBER_SIZE);
  return false;
}

void Rdb_binlog_manager::cleanup() {}

/**
  Set binlog name, pos and optionally gtid into WriteBatch.
  This function should be called as part of transaction commit,
  since binlog info is set only at transaction commit.
  Actual write into RocksDB is not done here, so checking if
  write succeeded or not is not possible here.
  @param binlog_name   Binlog name
  @param binlog_pos    Binlog pos
  @param binlog_gtid   Binlog max GTID
  @param batch         WriteBatch
*/
void Rdb_binlog_manager::update(const char *const binlog_name,
                                const my_off_t binlog_pos,
                                const char *const binlog_max_gtid,
                                rocksdb::WriteBatchBase *const batch) {
  if (binlog_name && binlog_pos) {
    // max binlog length (512) + binlog pos (4) + binlog gtid (57) < 1024
    const size_t RDB_MAX_BINLOG_INFO_LEN = 1024;
    uchar value_buf[RDB_MAX_BINLOG_INFO_LEN];
    m_dict->put_key(
        batch, m_key_slice,
        pack_value(value_buf, binlog_name, binlog_pos, binlog_max_gtid));
  }
}

/**
  Read binlog committed entry stored in RocksDB, then unpack
  @param[OUT] binlog_name  Binlog name
  @param[OUT] binlog_pos   Binlog pos
  @param[OUT] binlog_gtid  Binlog GTID
  @return
    true is binlog info was found (valid behavior)
    false otherwise
*/
bool Rdb_binlog_manager::read(char *const binlog_name,
                              my_off_t *const binlog_pos,
                              char *const binlog_gtid) const {
  bool ret = false;
  if (binlog_name) {
    std::string value;
    rocksdb::Status status = m_dict->get_value(m_key_slice, &value);
    if (status.ok()) {
      if (!unpack_value((const uchar *)value.c_str(), binlog_name, binlog_pos,
                        binlog_gtid))
        ret = true;
    }
  }
  return ret;
}

/**
  Pack binlog_name, binlog_pos, binlog_gtid into preallocated
  buffer, then converting and returning a RocksDB Slice
  @param buf           Preallocated buffer to set binlog info.
  @param binlog_name   Binlog name
  @param binlog_pos    Binlog pos
  @param binlog_gtid   Binlog GTID
  @return              rocksdb::Slice converted from buf and its length
*/
rocksdb::Slice
Rdb_binlog_manager::pack_value(uchar *const buf, const char *const binlog_name,
                               const my_off_t &binlog_pos,
                               const char *const binlog_gtid) const {
  uint pack_len = 0;

  // store version
  rdb_netbuf_store_uint16(buf, Rdb_key_def::BINLOG_INFO_INDEX_NUMBER_VERSION);
  pack_len += Rdb_key_def::VERSION_SIZE;

  // store binlog file name length
  DBUG_ASSERT(strlen(binlog_name) <= FN_REFLEN);
  const uint16_t binlog_name_len = (uint16_t)strlen(binlog_name);
  rdb_netbuf_store_uint16(buf + pack_len, binlog_name_len);
  pack_len += sizeof(uint16);

  // store binlog file name
  memcpy(buf + pack_len, binlog_name, binlog_name_len);
  pack_len += binlog_name_len;

  // store binlog pos
  rdb_netbuf_store_uint32(buf + pack_len, binlog_pos);
  pack_len += sizeof(uint32);

  // store binlog gtid length.
  // If gtid was not set, store 0 instead
  const uint16_t binlog_gtid_len = binlog_gtid ? (uint16_t)strlen(binlog_gtid) : 0;
  rdb_netbuf_store_uint16(buf + pack_len, binlog_gtid_len);
  pack_len += sizeof(uint16);

  if (binlog_gtid_len > 0) {
    // store binlog gtid
    memcpy(buf + pack_len, binlog_gtid, binlog_gtid_len);
    pack_len += binlog_gtid_len;
  }

  return rocksdb::Slice((char *)buf, pack_len);
}

/**
  Unpack value then split into binlog_name, binlog_pos (and binlog_gtid)
  @param[IN]  value        Binlog state info fetched from RocksDB
  @param[OUT] binlog_name  Binlog name
  @param[OUT] binlog_pos   Binlog pos
  @param[OUT] binlog_gtid  Binlog GTID
  @return     true on error
*/
bool Rdb_binlog_manager::unpack_value(const uchar *const value,
                                      char *const binlog_name,
                                      my_off_t *const binlog_pos,
                                      char *const binlog_gtid) const {
  uint pack_len = 0;

  DBUG_ASSERT(binlog_pos != nullptr);

  // read version
  const uint16_t version = rdb_netbuf_to_uint16(value);
  pack_len += Rdb_key_def::VERSION_SIZE;
  if (version != Rdb_key_def::BINLOG_INFO_INDEX_NUMBER_VERSION)
    return true;

  // read binlog file name length
  const uint16_t binlog_name_len = rdb_netbuf_to_uint16(value + pack_len);
  pack_len += sizeof(uint16);
  if (binlog_name_len) {
    // read and set binlog name
    memcpy(binlog_name, value + pack_len, binlog_name_len);
    binlog_name[binlog_name_len] = '\0';
    pack_len += binlog_name_len;

    // read and set binlog pos
    *binlog_pos = rdb_netbuf_to_uint32(value + pack_len);
    pack_len += sizeof(uint32);

    // read gtid length
    const uint16_t binlog_gtid_len = rdb_netbuf_to_uint16(value + pack_len);
    pack_len += sizeof(uint16);
    if (binlog_gtid && binlog_gtid_len > 0) {
      // read and set gtid
      memcpy(binlog_gtid, value + pack_len, binlog_gtid_len);
      binlog_gtid[binlog_gtid_len] = '\0';
      pack_len += binlog_gtid_len;
    }
  }
  return false;
}

/**
  Inserts a row into mysql.slave_gtid_info table. Doing this inside
  storage engine is more efficient than inserting/updating through MySQL.

  @param[IN] id Primary key of the table.
  @param[IN] db Database name. This is column 2 of the table.
  @param[IN] gtid Gtid in human readable form. This is column 3 of the table.
  @param[IN] write_batch Handle to storage engine writer.
*/
void Rdb_binlog_manager::update_slave_gtid_info(
    const uint &id, const char *const db, const char *const gtid,
    rocksdb::WriteBatchBase *const write_batch) {
  if (id && db && gtid) {
    // Make sure that if the slave_gtid_info table exists we have a
    // pointer to it via m_slave_gtid_info_tbl.
    if (!m_slave_gtid_info_tbl.load()) {
      m_slave_gtid_info_tbl.store(
          rdb_get_ddl_manager()->find("mysql.slave_gtid_info"));
    }
    if (!m_slave_gtid_info_tbl.load()) {
      // slave_gtid_info table is not present. Simply return.
      return;
    }
    DBUG_ASSERT(m_slave_gtid_info_tbl.load()->m_key_count == 1);

    const std::shared_ptr<const Rdb_key_def> &kd =
        m_slave_gtid_info_tbl.load()->m_key_descr_arr[0];
    String value;

    // Build key
    uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE + 4] = {0};
    uchar *buf = key_buf;
    rdb_netbuf_store_index(buf, kd->get_index_number());
    buf += Rdb_key_def::INDEX_NUMBER_SIZE;
    rdb_netbuf_store_uint32(buf, id);
    buf += 4;
    const rocksdb::Slice key_slice =
        rocksdb::Slice((const char *)key_buf, buf - key_buf);

    // Build value
    uchar value_buf[128] = {0};
    DBUG_ASSERT(gtid);
    const uint db_len = strlen(db);
    const uint gtid_len = strlen(gtid);
    buf = value_buf;
    // 1 byte used for flags. Empty here.
    buf++;

    // Write column 1.
    DBUG_ASSERT(strlen(db) <= 64);
    rdb_netbuf_store_byte(buf, db_len);
    buf++;
    memcpy(buf, db, db_len);
    buf += db_len;

    // Write column 2.
    DBUG_ASSERT(gtid_len <= 56);
    rdb_netbuf_store_byte(buf, gtid_len);
    buf++;
    memcpy(buf, gtid, gtid_len);
    buf += gtid_len;
    const rocksdb::Slice value_slice =
        rocksdb::Slice((const char *)value_buf, buf - value_buf);

    write_batch->Put(kd->get_cf(), key_slice, value_slice);
  }
}

bool Rdb_dict_manager::init(rocksdb::DB *const rdb_dict,
                            Rdb_cf_manager *const cf_manager) {
  mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
  m_db = rdb_dict;
  bool is_automatic;
  m_system_cfh = cf_manager->get_or_create_cf(m_db, DEFAULT_SYSTEM_CF_NAME, "",
                                              nullptr, &is_automatic);
  rdb_netbuf_store_index(m_key_buf_max_index_id, Rdb_key_def::MAX_INDEX_ID);
  m_key_slice_max_index_id =
      rocksdb::Slice(reinterpret_cast<char *>(m_key_buf_max_index_id),
                     Rdb_key_def::INDEX_NUMBER_SIZE);
  resume_drop_indexes();
  rollback_ongoing_index_creation();

  return (m_system_cfh == nullptr);
}

std::unique_ptr<rocksdb::WriteBatch> Rdb_dict_manager::begin() const {
  return std::unique_ptr<rocksdb::WriteBatch>(new rocksdb::WriteBatch);
}

void Rdb_dict_manager::put_key(rocksdb::WriteBatchBase *const batch,
                               const rocksdb::Slice &key,
                               const rocksdb::Slice &value) const {
  batch->Put(m_system_cfh, key, value);
}

rocksdb::Status Rdb_dict_manager::get_value(const rocksdb::Slice &key,
                                            std::string *const value) const {
  rocksdb::ReadOptions options;
  options.total_order_seek = true;
  return m_db->Get(options, m_system_cfh, key, value);
}

void Rdb_dict_manager::delete_key(rocksdb::WriteBatchBase *batch,
                                  const rocksdb::Slice &key) const {
  batch->Delete(m_system_cfh, key);
}

rocksdb::Iterator *Rdb_dict_manager::new_iterator() const {
  /* Reading data dictionary should always skip bloom filter */
  rocksdb::ReadOptions read_options;
  read_options.total_order_seek = true;
  return m_db->NewIterator(read_options, m_system_cfh);
}

int Rdb_dict_manager::commit(rocksdb::WriteBatch *const batch,
                             const bool &sync) const {
  if (!batch)
    return HA_EXIT_FAILURE;
  int res = 0;
  rocksdb::WriteOptions options;
  options.sync = sync;
  rocksdb::Status s = m_db->Write(options, batch);
  res = !s.ok(); // we return true when something failed
  if (res) {
    rdb_handle_io_error(s, RDB_IO_ERROR_DICT_COMMIT);
  }
  batch->Clear();
  return res;
}

void Rdb_dict_manager::dump_index_id(uchar *const netbuf,
                                     Rdb_key_def::DATA_DICT_TYPE dict_type,
                                     const GL_INDEX_ID &gl_index_id) {
  rdb_netbuf_store_uint32(netbuf, dict_type);
  rdb_netbuf_store_uint32(netbuf + Rdb_key_def::INDEX_NUMBER_SIZE,
                          gl_index_id.cf_id);
  rdb_netbuf_store_uint32(netbuf + 2 * Rdb_key_def::INDEX_NUMBER_SIZE,
                          gl_index_id.index_id);
}

void Rdb_dict_manager::delete_with_prefix(
    rocksdb::WriteBatch *const batch, Rdb_key_def::DATA_DICT_TYPE dict_type,
    const GL_INDEX_ID &gl_index_id) const {
  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 3] = {0};
  dump_index_id(key_buf, dict_type, gl_index_id);
  rocksdb::Slice key = rocksdb::Slice((char *)key_buf, sizeof(key_buf));

  delete_key(batch, key);
}

void Rdb_dict_manager::add_or_update_index_cf_mapping(
    rocksdb::WriteBatch *batch, const uchar m_index_type,
    const uint16_t kv_version, const uint32_t index_id,
    const uint32_t cf_id) const {
  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 3] = {0};
  uchar value_buf[256] = {0};
  GL_INDEX_ID gl_index_id = {cf_id, index_id};
  dump_index_id(key_buf, Rdb_key_def::INDEX_INFO, gl_index_id);
  const rocksdb::Slice key = rocksdb::Slice((char *)key_buf, sizeof(key_buf));

  uchar *ptr = value_buf;
  rdb_netbuf_store_uint16(ptr, Rdb_key_def::INDEX_INFO_VERSION_LATEST);
  ptr += 2;
  rdb_netbuf_store_byte(ptr, m_index_type);
  ptr += 1;
  rdb_netbuf_store_uint16(ptr, kv_version);
  ptr += 2;

  const rocksdb::Slice value =
      rocksdb::Slice((char *)value_buf, ptr - value_buf);
  batch->Put(m_system_cfh, key, value);
}

void Rdb_dict_manager::add_cf_flags(rocksdb::WriteBatch *const batch,
                                    const uint32_t &cf_id,
                                    const uint32_t &cf_flags) const {
  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2] = {0};
  uchar value_buf[Rdb_key_def::VERSION_SIZE + Rdb_key_def::INDEX_NUMBER_SIZE] =
      {0};
  rdb_netbuf_store_uint32(key_buf, Rdb_key_def::CF_DEFINITION);
  rdb_netbuf_store_uint32(key_buf + Rdb_key_def::INDEX_NUMBER_SIZE, cf_id);
  const rocksdb::Slice key = rocksdb::Slice((char *)key_buf, sizeof(key_buf));

  rdb_netbuf_store_uint16(value_buf, Rdb_key_def::CF_DEFINITION_VERSION);
  rdb_netbuf_store_uint32(value_buf + Rdb_key_def::VERSION_SIZE, cf_flags);
  const rocksdb::Slice value =
      rocksdb::Slice((char *)value_buf, sizeof(value_buf));
  batch->Put(m_system_cfh, key, value);
}

void Rdb_dict_manager::delete_index_info(rocksdb::WriteBatch *batch,
                                         const GL_INDEX_ID &gl_index_id) const {
  delete_with_prefix(batch, Rdb_key_def::INDEX_INFO, gl_index_id);
  delete_with_prefix(batch, Rdb_key_def::INDEX_STATISTICS, gl_index_id);
}

bool Rdb_dict_manager::get_index_info(const GL_INDEX_ID &gl_index_id,
                                      uint16_t *m_index_dict_version,
                                      uchar *m_index_type,
                                      uint16_t *kv_version) const {
  bool found = false;
  bool error = false;
  std::string value;
  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 3] = {0};
  dump_index_id(key_buf, Rdb_key_def::INDEX_INFO, gl_index_id);
  const rocksdb::Slice &key = rocksdb::Slice((char *)key_buf, sizeof(key_buf));

  const rocksdb::Status &status = get_value(key, &value);
  if (status.ok()) {
    const uchar *const val = (const uchar *)value.c_str();
    const uchar *ptr = val;
    *m_index_dict_version = rdb_netbuf_to_uint16(val);
    *kv_version = 0;
    *m_index_type = 0;
    ptr += 2;
    switch (*m_index_dict_version) {

    case Rdb_key_def::INDEX_INFO_VERSION_VERIFY_KV_FORMAT:
    case Rdb_key_def::INDEX_INFO_VERSION_GLOBAL_ID:
      *m_index_type = rdb_netbuf_to_byte(ptr);
      ptr += 1;
      *kv_version = rdb_netbuf_to_uint16(ptr);
      found = true;
      break;

    default:
      error = true;
      break;
    }

    switch (*m_index_type) {
    case Rdb_key_def::INDEX_TYPE_PRIMARY:
    case Rdb_key_def::INDEX_TYPE_HIDDEN_PRIMARY: {
      error = *kv_version > Rdb_key_def::PRIMARY_FORMAT_VERSION_LATEST;
      break;
    }
    case Rdb_key_def::INDEX_TYPE_SECONDARY:
      error = *kv_version > Rdb_key_def::SECONDARY_FORMAT_VERSION_LATEST;
      break;
    default:
      error = true;
      break;
    }
  }

  if (error) {
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Found invalid key version number (%u, %u, %u) "
                    "from data dictionary. This should never happen "
                    "and it may be a bug.",
                    *m_index_dict_version, *m_index_type, *kv_version);
    abort_with_stack_traces();
  }

  return found;
}

bool Rdb_dict_manager::get_cf_flags(const uint32_t &cf_id,
                                    uint32_t *const cf_flags) const {
  bool found = false;
  std::string value;
  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2] = {0};
  rdb_netbuf_store_uint32(key_buf, Rdb_key_def::CF_DEFINITION);
  rdb_netbuf_store_uint32(key_buf + Rdb_key_def::INDEX_NUMBER_SIZE, cf_id);
  const rocksdb::Slice key = rocksdb::Slice((char *)key_buf, sizeof(key_buf));

  const rocksdb::Status status = get_value(key, &value);
  if (status.ok()) {
    const uchar *val = (const uchar *)value.c_str();
    uint16_t version = rdb_netbuf_to_uint16(val);
    if (version == Rdb_key_def::CF_DEFINITION_VERSION) {
      *cf_flags = rdb_netbuf_to_uint32(val + Rdb_key_def::VERSION_SIZE);
      found = true;
    }
  }
  return found;
}

/*
  Returning index ids that were marked as deleted (via DROP TABLE) but
  still not removed by drop_index_thread yet, or indexes that are marked as
  ongoing creation.
 */
void Rdb_dict_manager::get_ongoing_index_operation(
    std::unordered_set<GL_INDEX_ID> *gl_index_ids,
    Rdb_key_def::DATA_DICT_TYPE dd_type) const {
  DBUG_ASSERT(dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING ||
              dd_type == Rdb_key_def::DDL_CREATE_INDEX_ONGOING);

  uchar index_buf[Rdb_key_def::INDEX_NUMBER_SIZE];
  rdb_netbuf_store_uint32(index_buf, dd_type);
  const rocksdb::Slice index_slice(reinterpret_cast<char *>(index_buf),
                                   Rdb_key_def::INDEX_NUMBER_SIZE);

  rocksdb::Iterator *it = new_iterator();
  for (it->Seek(index_slice); it->Valid(); it->Next()) {
    rocksdb::Slice key = it->key();
    const uchar *const ptr = (const uchar *)key.data();

    /*
      Ongoing drop/create index operations require key to be of the form:
      dd_type + cf_id + index_id (== INDEX_NUMBER_SIZE * 3)

      This may need to be changed in the future if we want to process a new
      ddl_type with different format.
    */
    if (key.size() != Rdb_key_def::INDEX_NUMBER_SIZE * 3 ||
        rdb_netbuf_to_uint32(ptr) != dd_type) {
      break;
    }

    // We don't check version right now since currently we always store only
    // Rdb_key_def::DDL_DROP_INDEX_ONGOING_VERSION = 1 as a value.
    // If increasing version number, we need to add version check logic here.
    GL_INDEX_ID gl_index_id;
    gl_index_id.cf_id =
        rdb_netbuf_to_uint32(ptr + Rdb_key_def::INDEX_NUMBER_SIZE);
    gl_index_id.index_id =
        rdb_netbuf_to_uint32(ptr + 2 * Rdb_key_def::INDEX_NUMBER_SIZE);
    gl_index_ids->insert(gl_index_id);
  }
  delete it;
}

/*
  Returning true if index_id is create/delete ongoing (undergoing creation or
  marked as deleted via DROP TABLE but drop_index_thread has not wiped yet)
  or not.
 */
bool Rdb_dict_manager::is_index_operation_ongoing(
    const GL_INDEX_ID &gl_index_id, Rdb_key_def::DATA_DICT_TYPE dd_type) const {
  DBUG_ASSERT(dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING ||
              dd_type == Rdb_key_def::DDL_CREATE_INDEX_ONGOING);

  bool found = false;
  std::string value;
  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 3] = {0};
  dump_index_id(key_buf, dd_type, gl_index_id);
  const rocksdb::Slice key = rocksdb::Slice((char *)key_buf, sizeof(key_buf));

  const rocksdb::Status status = get_value(key, &value);
  if (status.ok()) {
    found = true;
  }
  return found;
}

/*
  Adding index_id to data dictionary so that the index id is removed
  by drop_index_thread, or to track online index creation.
 */
void Rdb_dict_manager::start_ongoing_index_operation(
    rocksdb::WriteBatch *const batch, const GL_INDEX_ID &gl_index_id,
    Rdb_key_def::DATA_DICT_TYPE dd_type) const {
  DBUG_ASSERT(dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING ||
              dd_type == Rdb_key_def::DDL_CREATE_INDEX_ONGOING);

  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 3] = {0};
  uchar value_buf[Rdb_key_def::VERSION_SIZE] = {0};
  dump_index_id(key_buf, dd_type, gl_index_id);

  // version as needed
  if (dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING) {
    rdb_netbuf_store_uint16(value_buf,
                            Rdb_key_def::DDL_DROP_INDEX_ONGOING_VERSION);
  } else {
    rdb_netbuf_store_uint16(value_buf,
                            Rdb_key_def::DDL_CREATE_INDEX_ONGOING_VERSION);
  }

  const rocksdb::Slice key = rocksdb::Slice((char *)key_buf, sizeof(key_buf));
  const rocksdb::Slice value =
      rocksdb::Slice((char *)value_buf, sizeof(value_buf));
  batch->Put(m_system_cfh, key, value);
}

/*
  Removing index_id from data dictionary to confirm drop_index_thread
  completed dropping entire key/values of the index_id
 */
void Rdb_dict_manager::end_ongoing_index_operation(
    rocksdb::WriteBatch *const batch, const GL_INDEX_ID &gl_index_id,
    Rdb_key_def::DATA_DICT_TYPE dd_type) const {
  DBUG_ASSERT(dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING ||
              dd_type == Rdb_key_def::DDL_CREATE_INDEX_ONGOING);

  delete_with_prefix(batch, dd_type, gl_index_id);
}

/*
  Returning true if there is no target index ids to be removed
  by drop_index_thread
 */
bool Rdb_dict_manager::is_drop_index_empty() const {
  std::unordered_set<GL_INDEX_ID> gl_index_ids;
  get_ongoing_drop_indexes(&gl_index_ids);
  return gl_index_ids.empty();
}

/*
  This function is supposed to be called by DROP TABLE. Logging messages
  that dropping indexes started, and adding data dictionary so that
  all associated indexes to be removed
 */
void Rdb_dict_manager::add_drop_table(
    std::shared_ptr<Rdb_key_def> *const key_descr, const uint32 &n_keys,
    rocksdb::WriteBatch *const batch) const {
  std::unordered_set<GL_INDEX_ID> dropped_index_ids;
  for (uint32 i = 0; i < n_keys; i++) {
    dropped_index_ids.insert(key_descr[i]->get_gl_index_id());
  }

  add_drop_index(dropped_index_ids, batch);
}

/*
  Called during inplace index drop operations. Logging messages
  that dropping indexes started, and adding data dictionary so that
  all associated indexes to be removed
 */
void Rdb_dict_manager::add_drop_index(
    const std::unordered_set<GL_INDEX_ID> &gl_index_ids,
    rocksdb::WriteBatch *const batch) const {
  for (const auto &gl_index_id : gl_index_ids) {
    log_start_drop_index(gl_index_id, "Begin");
    start_drop_index(batch, gl_index_id);
  }
}

/*
  Called during inplace index creation operations. Logging messages
  that adding indexes started, and updates data dictionary with all associated
  indexes to be added.
 */
void Rdb_dict_manager::add_create_index(
    const std::unordered_set<GL_INDEX_ID> &gl_index_ids,
    rocksdb::WriteBatch *const batch) const {
  for (const auto &gl_index_id : gl_index_ids) {
    // NO_LINT_DEBUG
    sql_print_information("RocksDB: Begin index creation (%u,%u)",
                          gl_index_id.cf_id, gl_index_id.index_id);
    start_create_index(batch, gl_index_id);
  }
}

/*
  This function is supposed to be called by drop_index_thread, when it
  finished dropping any index, or at the completion of online index creation.
 */
void Rdb_dict_manager::finish_indexes_operation(
    const std::unordered_set<GL_INDEX_ID> &gl_index_ids,
    Rdb_key_def::DATA_DICT_TYPE dd_type) const {
  DBUG_ASSERT(dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING ||
              dd_type == Rdb_key_def::DDL_CREATE_INDEX_ONGOING);

  const std::unique_ptr<rocksdb::WriteBatch> wb = begin();
  rocksdb::WriteBatch *const batch = wb.get();

  std::unordered_set<GL_INDEX_ID> incomplete_create_indexes;
  get_ongoing_create_indexes(&incomplete_create_indexes);

  for (const auto &gl_index_id : gl_index_ids) {
    if (is_index_operation_ongoing(gl_index_id, dd_type)) {
      // NO_LINT_DEBUG
      sql_print_information("RocksDB: Finished %s (%u,%u)",
                            dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING
                                ? "filtering dropped index"
                                : "index creation",
                            gl_index_id.cf_id, gl_index_id.index_id);

      end_ongoing_index_operation(batch, gl_index_id, dd_type);

      /*
        Remove the corresponding incomplete create indexes from data
        dictionary as well
      */
      if (dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING) {
        if (incomplete_create_indexes.count(gl_index_id)) {
          end_ongoing_index_operation(batch, gl_index_id,
                                      Rdb_key_def::DDL_CREATE_INDEX_ONGOING);
        }
      }
    }

    if (dd_type == Rdb_key_def::DDL_DROP_INDEX_ONGOING) {
      delete_index_info(batch, gl_index_id);
    }
  }
  commit(batch);
}

/*
  This function is supposed to be called when initializing
  Rdb_dict_manager (at startup). If there is any index ids that are
  drop ongoing, printing out messages for diagnostics purposes.
 */
void Rdb_dict_manager::resume_drop_indexes() const {
  std::unordered_set<GL_INDEX_ID> gl_index_ids;
  get_ongoing_drop_indexes(&gl_index_ids);

  uint max_index_id_in_dict = 0;
  get_max_index_id(&max_index_id_in_dict);

  for (const auto &gl_index_id : gl_index_ids) {
    log_start_drop_index(gl_index_id, "Resume");
    if (max_index_id_in_dict < gl_index_id.index_id) {
      sql_print_error("RocksDB: Found max index id %u from data dictionary "
                      "but also found dropped index id (%u,%u) from drop_index "
                      "dictionary. This should never happen and is possibly a "
                      "bug.",
                      max_index_id_in_dict, gl_index_id.cf_id,
                      gl_index_id.index_id);
      abort_with_stack_traces();
    }
  }
}

void Rdb_dict_manager::rollback_ongoing_index_creation() const {
  const std::unique_ptr<rocksdb::WriteBatch> wb = begin();
  rocksdb::WriteBatch *const batch = wb.get();

  std::unordered_set<GL_INDEX_ID> gl_index_ids;
  get_ongoing_create_indexes(&gl_index_ids);

  for (const auto &gl_index_id : gl_index_ids) {
    // NO_LINT_DEBUG
    sql_print_information("RocksDB: Removing incomplete create index (%u,%u)",
                          gl_index_id.cf_id, gl_index_id.index_id);

    start_drop_index(batch, gl_index_id);
  }

  commit(batch);
}

void Rdb_dict_manager::log_start_drop_table(
    const std::shared_ptr<Rdb_key_def> *const key_descr, const uint32 &n_keys,
    const char *const log_action) const {
  for (uint32 i = 0; i < n_keys; i++) {
    log_start_drop_index(key_descr[i]->get_gl_index_id(), log_action);
  }
}

void Rdb_dict_manager::log_start_drop_index(GL_INDEX_ID gl_index_id,
                                            const char *log_action) const {
  uint16 m_index_dict_version = 0;
  uchar m_index_type = 0;
  uint16 kv_version = 0;

  if (!get_index_info(gl_index_id, &m_index_dict_version, &m_index_type,
                      &kv_version)) {
    /*
      If we don't find the index info, it could be that it's because it was a
      partially created index that isn't in the data dictionary yet that needs
      to be rolled back.
    */
    std::unordered_set<GL_INDEX_ID> incomplete_create_indexes;
    get_ongoing_create_indexes(&incomplete_create_indexes);

    if (!incomplete_create_indexes.count(gl_index_id)) {
      /* If it's not a partially created index, something is very wrong. */
      sql_print_error("RocksDB: Failed to get column family info "
                      "from index id (%u,%u). MyRocks data dictionary may "
                      "get corrupted.",
                      gl_index_id.cf_id, gl_index_id.index_id);
      abort_with_stack_traces();
    }
  }
  sql_print_information("RocksDB: %s filtering dropped index (%u,%u)",
                        log_action, gl_index_id.cf_id, gl_index_id.index_id);
}

bool Rdb_dict_manager::get_max_index_id(uint32_t *const index_id) const {
  bool found = false;
  std::string value;

  const rocksdb::Status status = get_value(m_key_slice_max_index_id, &value);
  if (status.ok()) {
    const uchar *const val = (const uchar *)value.c_str();
    const uint16_t &version = rdb_netbuf_to_uint16(val);
    if (version == Rdb_key_def::MAX_INDEX_ID_VERSION) {
      *index_id = rdb_netbuf_to_uint32(val + Rdb_key_def::VERSION_SIZE);
      found = true;
    }
  }
  return found;
}

bool Rdb_dict_manager::update_max_index_id(rocksdb::WriteBatch *const batch,
                                           const uint32_t &index_id) const {
  DBUG_ASSERT(batch != nullptr);

  uint32_t old_index_id = -1;
  if (get_max_index_id(&old_index_id)) {
    if (old_index_id > index_id) {
      sql_print_error("RocksDB: Found max index id %u from data dictionary "
                      "but trying to update to older value %u. This should "
                      "never happen and possibly a bug.",
                      old_index_id, index_id);
      return true;
    }
  }

  uchar value_buf[Rdb_key_def::VERSION_SIZE + Rdb_key_def::INDEX_NUMBER_SIZE] =
      {0};
  rdb_netbuf_store_uint16(value_buf, Rdb_key_def::MAX_INDEX_ID_VERSION);
  rdb_netbuf_store_uint32(value_buf + Rdb_key_def::VERSION_SIZE, index_id);
  const rocksdb::Slice value =
      rocksdb::Slice((char *)value_buf, sizeof(value_buf));
  batch->Put(m_system_cfh, m_key_slice_max_index_id, value);
  return false;
}

void Rdb_dict_manager::add_stats(
    rocksdb::WriteBatch *const batch,
    const std::vector<Rdb_index_stats> &stats) const {
  DBUG_ASSERT(batch != nullptr);

  for (const auto &it : stats) {
    uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 3] = {0};
    dump_index_id(key_buf, Rdb_key_def::INDEX_STATISTICS, it.m_gl_index_id);

    // IndexStats::materialize takes complete care of serialization including
    // storing the version
    const auto value =
        Rdb_index_stats::materialize(std::vector<Rdb_index_stats>{it}, 1.);

    batch->Put(m_system_cfh, rocksdb::Slice((char *)key_buf, sizeof(key_buf)),
               value);
  }
}

Rdb_index_stats Rdb_dict_manager::get_stats(GL_INDEX_ID gl_index_id) const {
  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 3] = {0};
  dump_index_id(key_buf, Rdb_key_def::INDEX_STATISTICS, gl_index_id);

  std::string value;
  const rocksdb::Status status = get_value(
      rocksdb::Slice(reinterpret_cast<char *>(key_buf), sizeof(key_buf)),
      &value);
  if (status.ok()) {
    std::vector<Rdb_index_stats> v;
    // unmaterialize checks if the version matches
    if (Rdb_index_stats::unmaterialize(value, &v) == 0 && v.size() == 1) {
      return v[0];
    }
  }

  return Rdb_index_stats();
}

uint Rdb_seq_generator::get_and_update_next_number(
    Rdb_dict_manager *const dict) {
  DBUG_ASSERT(dict != nullptr);

  uint res;
  RDB_MUTEX_LOCK_CHECK(m_mutex);

  res = m_next_number++;

  const std::unique_ptr<rocksdb::WriteBatch> wb = dict->begin();
  rocksdb::WriteBatch *const batch = wb.get();

  DBUG_ASSERT(batch != nullptr);
  dict->update_max_index_id(batch, res);
  dict->commit(batch);

  RDB_MUTEX_UNLOCK_CHECK(m_mutex);

  return res;
}

} // namespace myrocks
