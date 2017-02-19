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
#pragma once

/* C++ standard header files */
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

/* C standard header files */
#include <arpa/inet.h>

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./properties_collector.h"
#include "./rdb_buff.h"
#include "./rdb_utils.h"

namespace myrocks {

class Rdb_dict_manager;
class Rdb_key_def;
class Rdb_field_packing;
class Rdb_cf_manager;
class Rdb_ddl_manager;

/*
  @brief
  Field packing context.
  The idea is to ensure that a call to rdb_index_field_pack_t function
  is followed by a call to rdb_make_unpack_info_t.

  @detail
  For some datatypes, unpack_info is produced as a side effect of
  rdb_index_field_pack_t function call.
  For other datatypes, packing is just calling make_sort_key(), while
  rdb_make_unpack_info_t is a custom function.
  In order to accommodate both cases, we require both calls to be made and
  unpack_info is passed as context data between the two.
*/
class Rdb_pack_field_context {
public:
  Rdb_pack_field_context(const Rdb_pack_field_context &) = delete;
  Rdb_pack_field_context &operator=(const Rdb_pack_field_context &) = delete;

  explicit Rdb_pack_field_context(Rdb_string_writer *const writer_arg)
      : writer(writer_arg) {}

  // NULL means we're not producing unpack_info.
  Rdb_string_writer *writer;
};

struct Rdb_collation_codec;

/*
  C-style "virtual table" allowing different handling of packing logic based
  on the field type. See Rdb_field_packing::setup() implementation.
  */
using rdb_make_unpack_info_t = void (*)(const Rdb_collation_codec *codec,
                                        const Field *field,
                                        Rdb_pack_field_context *pack_ctx);
using rdb_index_field_unpack_t = int (*)(Rdb_field_packing *fpi, Field *field,
                                         uchar *field_ptr,
                                         Rdb_string_reader *reader,
                                         Rdb_string_reader *unpack_reader);
using rdb_index_field_skip_t = int (*)(const Rdb_field_packing *fpi,
                                       const Field *field,
                                       Rdb_string_reader *reader);
using rdb_index_field_pack_t = void (*)(Rdb_field_packing *fpi, Field *field,
                                        uchar *buf, uchar **dst,
                                        Rdb_pack_field_context *pack_ctx);

const uint RDB_INVALID_KEY_LEN = uint(-1);

/* How much one checksum occupies when stored in the record */
const size_t RDB_CHECKSUM_SIZE = sizeof(uint32_t);

/*
  How much the checksum data occupies in record, in total.
  It is storing two checksums plus 1 tag-byte.
*/
const size_t RDB_CHECKSUM_CHUNK_SIZE = 2 * RDB_CHECKSUM_SIZE + 1;

/*
  Checksum data starts from CHECKSUM_DATA_TAG which is followed by two CRC32
  checksums.
*/
const char RDB_CHECKSUM_DATA_TAG = 0x01;

/*
  Unpack data is variable length. It is a 1 tag-byte plus a
  two byte length field. The length field includes the header as well.
*/
const char RDB_UNPACK_DATA_TAG = 0x02;
const size_t RDB_UNPACK_DATA_LEN_SIZE = sizeof(uint16_t);
const size_t RDB_UNPACK_HEADER_SIZE =
    sizeof(RDB_UNPACK_DATA_TAG) + RDB_UNPACK_DATA_LEN_SIZE;

// Possible return values for rdb_index_field_unpack_t functions.
enum {
  UNPACK_SUCCESS = 0,
  UNPACK_FAILURE = 1,
};

/*
  An object of this class represents information about an index in an SQL
  table. It provides services to encode and decode index tuples.

  Note: a table (as in, on-disk table) has a single Rdb_key_def object which
  is shared across multiple TABLE* objects and may be used simultaneously from
  different threads.

  There are several data encodings:

  === SQL LAYER ===
  SQL layer uses two encodings:

  - "Table->record format". This is the format that is used for the data in
     the record buffers, table->record[i]

  - KeyTupleFormat (see opt_range.cc) - this is used in parameters to index
    lookup functions, like handler::index_read_map().

  === Inside RocksDB ===
  Primary Key is stored as a mapping:

    index_tuple -> StoredRecord

  StoredRecord is in Table->record format, except for blobs, which are stored
  in-place. See ha_rocksdb::convert_record_to_storage_format for details.

  Secondary indexes are stored as one of two variants:

    index_tuple -> unpack_info
    index_tuple -> empty_string

  index_tuple here is the form of key that can be compared with memcmp(), aka
  "mem-comparable form".

  unpack_info is extra data that allows to restore the original value from its
  mem-comparable form. It is present only if the index supports index-only
  reads.
*/

class Rdb_key_def {
public:
  /* Convert a key from KeyTupleFormat to mem-comparable form */
  uint pack_index_tuple(TABLE *const tbl, uchar *const pack_buffer,
                        uchar *const packed_tuple, const uchar *const key_tuple,
                        const key_part_map &keypart_map) const;

  /* Convert a key from Table->record format to mem-comparable form */
  uint pack_record(const TABLE *const tbl, uchar *const pack_buffer,
                   const uchar *const record, uchar *const packed_tuple,
                   Rdb_string_writer *const unpack_info,
                   const bool &should_store_row_debug_checksums,
                   const longlong &hidden_pk_id = 0, uint n_key_parts = 0,
                   uint *const n_null_fields = nullptr) const;
  /* Pack the hidden primary key into mem-comparable form. */
  uint pack_hidden_pk(const longlong &hidden_pk_id,
                      uchar *const packed_tuple) const;
  int unpack_record(TABLE *const table, uchar *const buf,
                    const rocksdb::Slice *const packed_key,
                    const rocksdb::Slice *const unpack_info,
                    const bool &verify_row_debug_checksums) const;

  static bool unpack_info_has_checksum(const rocksdb::Slice &unpack_info);
  int compare_keys(const rocksdb::Slice *key1, const rocksdb::Slice *key2,
                   std::size_t *const column_index) const;

  size_t key_length(const TABLE *const table, const rocksdb::Slice &key) const;

  /* Get the key that is the "infimum" for this index */
  inline void get_infimum_key(uchar *const key, uint *const size) const {
    rdb_netbuf_store_index(key, m_index_number);
    *size = INDEX_NUMBER_SIZE;
  }

  /* Get the key that is a "supremum" for this index */
  inline void get_supremum_key(uchar *const key, uint *const size) const {
    rdb_netbuf_store_index(key, m_index_number + 1);
    *size = INDEX_NUMBER_SIZE;
  }

  /* Make a key that is right after the given key. */
  static int successor(uchar *const packed_tuple, const uint &len);

  /*
    This can be used to compare prefixes.
    if  X is a prefix of Y, then we consider that X = Y.
  */
  // b describes the lookup key, which can be a prefix of a.
  int cmp_full_keys(const rocksdb::Slice &a, const rocksdb::Slice &b) const {
    DBUG_ASSERT(covers_key(a));
    DBUG_ASSERT(covers_key(b));

    return memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
  }

  /* Check if given mem-comparable key belongs to this index */
  bool covers_key(const rocksdb::Slice &slice) const {
    if (slice.size() < INDEX_NUMBER_SIZE)
      return false;

    if (memcmp(slice.data(), m_index_number_storage_form, INDEX_NUMBER_SIZE))
      return false;

    return true;
  }

  /*
    Return true if the passed mem-comparable key
    - is from this index, and
    - it matches the passed key prefix (the prefix is also in mem-comparable
      form)
  */
  bool value_matches_prefix(const rocksdb::Slice &value,
                            const rocksdb::Slice &prefix) const {
    return covers_key(value) && !cmp_full_keys(value, prefix);
  }

  uint32 get_keyno() const { return m_keyno; }

  uint32 get_index_number() const { return m_index_number; }

  GL_INDEX_ID get_gl_index_id() const {
    const GL_INDEX_ID gl_index_id = {m_cf_handle->GetID(), m_index_number};
    return gl_index_id;
  }

  int read_memcmp_key_part(const TABLE *table_arg, Rdb_string_reader *reader,
                           const uint part_num) const;

  /* Must only be called for secondary keys: */
  uint get_primary_key_tuple(const TABLE *const tbl,
                             const Rdb_key_def &pk_descr,
                             const rocksdb::Slice *const key,
                             uchar *const pk_buffer) const;

  uint get_memcmp_sk_parts(const TABLE *table, const rocksdb::Slice &key,
                           uchar *sk_buffer, uint *n_null_fields) const;

  /* Return max length of mem-comparable form */
  uint max_storage_fmt_length() const { return m_maxlength; }

  uint get_key_parts() const { return m_key_parts; }

  /*
    Get a field object for key part #part_no

    @detail
      SQL layer thinks unique secondary indexes and indexes in partitioned
      tables are not "Extended" with Primary Key columns.

      Internally, we always extend all indexes with PK columns. This function
      uses our definition of how the index is Extended.
  */
  inline Field *get_table_field_for_part_no(TABLE *table, uint part_no) const;

  const std::string &get_name() const { return m_name; }

  const rocksdb::SliceTransform *get_extractor() const {
    return m_prefix_extractor.get();
  }

  Rdb_key_def &operator=(const Rdb_key_def &) = delete;
  Rdb_key_def(const Rdb_key_def &k);
  Rdb_key_def(uint indexnr_arg, uint keyno_arg,
              rocksdb::ColumnFamilyHandle *cf_handle_arg,
              uint16_t index_dict_version_arg, uchar index_type_arg,
              uint16_t kv_format_version_arg, bool is_reverse_cf_arg,
              bool is_auto_cf_arg, const char *name,
              Rdb_index_stats stats = Rdb_index_stats());
  ~Rdb_key_def();

  enum {
    INDEX_NUMBER_SIZE = 4,
    VERSION_SIZE = 2,
    CF_NUMBER_SIZE = 4,
    CF_FLAG_SIZE = 4,
    PACKED_SIZE = 4, // one int
  };

  // bit flags for combining bools when writing to disk
  enum {
    REVERSE_CF_FLAG = 1,
    AUTO_CF_FLAG = 2,
  };

  // Data dictionary types
  enum DATA_DICT_TYPE {
    DDL_ENTRY_INDEX_START_NUMBER = 1,
    INDEX_INFO = 2,
    CF_DEFINITION = 3,
    BINLOG_INFO_INDEX_NUMBER = 4,
    DDL_DROP_INDEX_ONGOING = 5,
    INDEX_STATISTICS = 6,
    MAX_INDEX_ID = 7,
    DDL_CREATE_INDEX_ONGOING = 8,
    END_DICT_INDEX_ID = 255
  };

  // Data dictionary schema version. Introduce newer versions
  // if changing schema layout
  enum {
    DDL_ENTRY_INDEX_VERSION = 1,
    CF_DEFINITION_VERSION = 1,
    BINLOG_INFO_INDEX_NUMBER_VERSION = 1,
    DDL_DROP_INDEX_ONGOING_VERSION = 1,
    MAX_INDEX_ID_VERSION = 1,
    DDL_CREATE_INDEX_ONGOING_VERSION = 1,
    // Version for index stats is stored in IndexStats struct
  };

  // Index info version.  Introduce newer versions when changing the
  // INDEX_INFO layout. Update INDEX_INFO_VERSION_LATEST to point to the
  // latest version number.
  enum {
    INDEX_INFO_VERSION_INITIAL = 1, // Obsolete
    INDEX_INFO_VERSION_KV_FORMAT,
    INDEX_INFO_VERSION_GLOBAL_ID,
    // There is no change to data format in this version, but this version
    // verifies KV format version, whereas previous versions do not. A version
    // bump is needed to prevent older binaries from skipping the KV version
    // check inadvertently.
    INDEX_INFO_VERSION_VERIFY_KV_FORMAT,
    // This normally point to the latest (currently it does).
    INDEX_INFO_VERSION_LATEST = INDEX_INFO_VERSION_VERIFY_KV_FORMAT,
  };

  // MyRocks index types
  enum {
    INDEX_TYPE_PRIMARY = 1,
    INDEX_TYPE_SECONDARY = 2,
    INDEX_TYPE_HIDDEN_PRIMARY = 3,
  };

  // Key/Value format version for each index type
  enum {
    PRIMARY_FORMAT_VERSION_INITIAL = 10,
    // This change includes:
    //  - For columns that can be unpacked with unpack_info, PK
    //    stores the unpack_info.
    //  - DECIMAL datatype is no longer stored in the row (because
    //    it can be decoded from its mem-comparable form)
    //  - VARCHAR-columns use endspace-padding.
    PRIMARY_FORMAT_VERSION_UPDATE1 = 11,
    PRIMARY_FORMAT_VERSION_LATEST = PRIMARY_FORMAT_VERSION_UPDATE1,

    SECONDARY_FORMAT_VERSION_INITIAL = 10,
    // This change the SK format to include unpack_info.
    SECONDARY_FORMAT_VERSION_UPDATE1 = 11,
    SECONDARY_FORMAT_VERSION_LATEST = SECONDARY_FORMAT_VERSION_UPDATE1,
  };

  void setup(const TABLE *const table, const Rdb_tbl_def *const tbl_def);

  rocksdb::ColumnFamilyHandle *get_cf() const { return m_cf_handle; }

  /* Check if keypart #kp can be unpacked from index tuple */
  inline bool can_unpack(const uint &kp) const;
  /* Check if keypart #kp needs unpack info */
  inline bool has_unpack_info(const uint &kp) const;

  /* Check if given table has a primary key */
  static bool table_has_hidden_pk(const TABLE *const table);

  void report_checksum_mismatch(const bool &is_key, const char *const data,
                                const size_t data_size) const;

  /* Check if index is at least pk_min if it is a PK,
    or at least sk_min if SK.*/
  bool index_format_min_check(const int &pk_min, const int &sk_min) const;

private:
#ifndef DBUG_OFF
  inline bool is_storage_available(const int &offset, const int &needed) const {
    const int storage_length = static_cast<int>(max_storage_fmt_length());
    return (storage_length - offset) >= needed;
  }
#endif // DBUG_OFF

  /* Global number of this index (used as prefix in StorageFormat) */
  const uint32 m_index_number;

  uchar m_index_number_storage_form[INDEX_NUMBER_SIZE];

  rocksdb::ColumnFamilyHandle *m_cf_handle;

public:
  uint16_t m_index_dict_version;
  uchar m_index_type;
  /* KV format version for the index id */
  uint16_t m_kv_format_version;
  /* If true, the column family stores data in the reverse order */
  bool m_is_reverse_cf;

  bool m_is_auto_cf;
  std::string m_name;
  mutable Rdb_index_stats m_stats;

private:
  friend class Rdb_tbl_def; // for m_index_number above

  /* Number of key parts in the primary key*/
  uint m_pk_key_parts;

  /*
     pk_part_no[X]=Y means that keypart #X of this key is key part #Y of the
     primary key.  Y==-1 means this column is not present in the primary key.
  */
  uint *m_pk_part_no;

  /* Array of index-part descriptors. */
  Rdb_field_packing *m_pack_info;

  uint m_keyno; /* number of this index in the table */

  /*
    Number of key parts in the index (including "index extension"). This is how
    many elements are in the m_pack_info array.
  */
  uint m_key_parts;

  /* Prefix extractor for the column family of the key definiton */
  std::shared_ptr<const rocksdb::SliceTransform> m_prefix_extractor;

  /* Maximum length of the mem-comparable form. */
  uint m_maxlength;

  /* mutex to protect setup */
  mysql_mutex_t m_mutex;
};

// "Simple" collations (those specified in strings/ctype-simple.c) are simple
// because their strnxfrm function maps one byte to one byte. However, the
// mapping is not injective, so the inverse function will take in an extra
// index parameter containing information to disambiguate what the original
// character was.
//
// The m_enc* members are for encoding. Generally, we want encoding to be:
//      src -> (dst, idx)
//
// Since strnxfrm already gives us dst, we just need m_enc_idx[src] to give us
// idx.
//
// For the inverse, we have:
//      (dst, idx) -> src
//
// We have m_dec_idx[idx][dst] = src to get our original character back.
//
struct Rdb_collation_codec {
  const my_core::CHARSET_INFO *m_cs;
  // The first element unpacks VARCHAR(n), the second one - CHAR(n).
  std::array<rdb_make_unpack_info_t, 2> m_make_unpack_info_func;
  std::array<rdb_index_field_unpack_t, 2> m_unpack_func;

  std::array<uchar, 256> m_enc_idx;
  std::array<uchar, 256> m_enc_size;

  std::array<uchar, 256> m_dec_size;
  std::vector<std::array<uchar, 256>> m_dec_idx;
};

extern mysql_mutex_t rdb_collation_data_mutex;
extern mysql_mutex_t rdb_mem_cmp_space_mutex;
extern std::array<const Rdb_collation_codec *, MY_ALL_CHARSETS_SIZE>
    rdb_collation_data;

class Rdb_field_packing {
public:
  Rdb_field_packing(const Rdb_field_packing &) = delete;
  Rdb_field_packing &operator=(const Rdb_field_packing &) = delete;
  Rdb_field_packing() = default;

  /* Length of mem-comparable image of the field, in bytes */
  int m_max_image_len;

  /* Length of image in the unpack data */
  int m_unpack_data_len;
  int m_unpack_data_offset;

  bool m_maybe_null; /* TRUE <=> NULL-byte is stored */

  /*
    Valid only for VARCHAR fields.
  */
  const CHARSET_INFO *m_varchar_charset;

  // (Valid when Variable Length Space Padded Encoding is used):
  uint m_segment_size; // size of segment used

  // number of bytes used to store number of trimmed (or added)
  // spaces in the upack_info
  bool m_unpack_info_uses_two_bytes;

  const std::vector<uchar> *space_xfrm;
  size_t space_xfrm_len;
  size_t space_mb_len;

  const Rdb_collation_codec *m_charset_codec;

  /*
    @return TRUE: this field makes use of unpack_info.
  */
  bool uses_unpack_info() const { return (m_make_unpack_info_func != nullptr); }

  /* TRUE means unpack_info stores the original field value */
  bool m_unpack_info_stores_value;

  rdb_index_field_pack_t m_pack_func;
  rdb_make_unpack_info_t m_make_unpack_info_func;

  /*
    This function takes
    - mem-comparable form
    - unpack_info data
    and restores the original value.
  */
  rdb_index_field_unpack_t m_unpack_func;

  /*
    This function skips over mem-comparable form.
  */
  rdb_index_field_skip_t m_skip_func;

private:
  /*
    Location of the field in the table (key number and key part number).

    Note that this describes not the field, but rather a position of field in
    the index. Consider an example:

      col1 VARCHAR (100),
      INDEX idx1 (col1)),
      INDEX idx2 (col1(10)),

    Here, idx2 has a special Field object that is set to describe a 10-char
    prefix of col1.

    We must also store the keynr. It is needed for implicit "extended keys".
    Every key in MyRocks needs to include PK columns.  Generally, SQL layer
    includes PK columns as part of its "Extended Keys" feature, but sometimes
    it does not (known examples are unique secondary indexes and partitioned
    tables).
    In that case, MyRocks's index descriptor has invisible suffix of PK
    columns (and the point is that these columns are parts of PK, not parts
    of the current index).
  */
  uint m_keynr;
  uint m_key_part;

public:
  bool setup(const Rdb_key_def *const key_descr, const Field *const field,
             const uint &keynr_arg, const uint &key_part_arg,
             const uint16 &key_length);
  Field *get_field_in_table(const TABLE *const tbl) const;
  void fill_hidden_pk_val(uchar **dst, const longlong &hidden_pk_id) const;
};

/*
  Descriptor telling how to decode/encode a field to on-disk record storage
  format. Not all information is in the structure yet, but eventually we
  want to have as much as possible there to avoid virtual calls.

  For encoding/decoding of index tuples, see Rdb_key_def.
  */
class Rdb_field_encoder {
public:
  Rdb_field_encoder(const Rdb_field_encoder &) = delete;
  Rdb_field_encoder &operator=(const Rdb_field_encoder &) = delete;
  /*
    STORE_NONE is set when a column can be decoded solely from their
    mem-comparable form.
    STORE_SOME is set when a column can be decoded from their mem-comparable
    form plus unpack_info.
    STORE_ALL is set when a column cannot be decoded, so its original value
    must be stored in the PK records.
    */
  enum STORAGE_TYPE {
    STORE_NONE,
    STORE_SOME,
    STORE_ALL,
  };
  STORAGE_TYPE m_storage_type;

  uint m_null_offset;
  uint16 m_field_index;

  uchar m_null_mask; // 0 means the field cannot be null

  my_core::enum_field_types m_field_type;

  uint m_pack_length_in_rec;

  bool maybe_null() const { return m_null_mask != 0; }

  bool uses_variable_len_encoding() const {
    return (m_field_type == MYSQL_TYPE_BLOB ||
            m_field_type == MYSQL_TYPE_VARCHAR);
  }
};

inline Field *Rdb_key_def::get_table_field_for_part_no(TABLE *table,
                                                       uint part_no) const {
  DBUG_ASSERT(part_no < get_key_parts());
  return m_pack_info[part_no].get_field_in_table(table);
}

inline bool Rdb_key_def::can_unpack(const uint &kp) const {
  DBUG_ASSERT(kp < m_key_parts);
  return (m_pack_info[kp].m_unpack_func != nullptr);
}

inline bool Rdb_key_def::has_unpack_info(const uint &kp) const {
  DBUG_ASSERT(kp < m_key_parts);
  return m_pack_info[kp].uses_unpack_info();
}

/*
  A table definition. This is an entry in the mapping

    dbname.tablename -> {index_nr, index_nr, ... }

  There is only one Rdb_tbl_def object for a given table.
  That's why we keep auto_increment value here, too.
*/

class Rdb_tbl_def {
private:
  void check_if_is_mysql_system_table();

  /* Stores 'dbname.tablename' */
  std::string m_dbname_tablename;

  /* Store the db name, table name, and partition name */
  std::string m_dbname;
  std::string m_tablename;
  std::string m_partition;

  void set_name(const std::string &name);

public:
  Rdb_tbl_def(const Rdb_tbl_def &) = delete;
  Rdb_tbl_def &operator=(const Rdb_tbl_def &) = delete;

  explicit Rdb_tbl_def(const std::string &name)
      : m_key_descr_arr(nullptr), m_hidden_pk_val(1), m_auto_incr_val(1) {
    set_name(name);
  }

  Rdb_tbl_def(const char *const name, const size_t &len)
      : m_key_descr_arr(nullptr), m_hidden_pk_val(1), m_auto_incr_val(1) {
    set_name(std::string(name, len));
  }

  explicit Rdb_tbl_def(const rocksdb::Slice &slice, const size_t &pos = 0)
      : m_key_descr_arr(nullptr), m_hidden_pk_val(1), m_auto_incr_val(1) {
    set_name(std::string(slice.data() + pos, slice.size() - pos));
  }

  ~Rdb_tbl_def();

  /* Number of indexes */
  uint m_key_count;

  /* Array of index descriptors */
  std::shared_ptr<Rdb_key_def> *m_key_descr_arr;

  std::atomic<longlong> m_hidden_pk_val;
  std::atomic<longlong> m_auto_incr_val;

  /* Is this a system table */
  bool m_is_mysql_system_table;

  bool put_dict(Rdb_dict_manager *const dict, rocksdb::WriteBatch *const batch,
                uchar *const key, const size_t &keylen);

  const std::string &full_tablename() const { return m_dbname_tablename; }
  const std::string &base_dbname() const { return m_dbname; }
  const std::string &base_tablename() const { return m_tablename; }
  const std::string &base_partition() const { return m_partition; }
};

/*
  A thread-safe sequential number generator. Its performance is not a concern
  hence it is ok to protect it by a mutex.
*/

class Rdb_seq_generator {
  uint m_next_number = 0;

  mysql_mutex_t m_mutex;

public:
  Rdb_seq_generator(const Rdb_seq_generator &) = delete;
  Rdb_seq_generator &operator=(const Rdb_seq_generator &) = delete;
  Rdb_seq_generator() = default;

  void init(const uint &initial_number) {
    mysql_mutex_init(0, &m_mutex, MY_MUTEX_INIT_FAST);
    m_next_number = initial_number;
  }

  uint get_and_update_next_number(Rdb_dict_manager *const dict);

  void cleanup() { mysql_mutex_destroy(&m_mutex); }
};

interface Rdb_tables_scanner {
  virtual int add_table(Rdb_tbl_def * tdef) = 0;
};

/*
  This contains a mapping of

     dbname.table_name -> array{Rdb_key_def}.

  objects are shared among all threads.
*/

class Rdb_ddl_manager {
  Rdb_dict_manager *m_dict = nullptr;
  my_core::HASH m_ddl_hash; // Contains Rdb_tbl_def elements
  // maps index id to <table_name, index number>
  std::map<GL_INDEX_ID, std::pair<std::string, uint>> m_index_num_to_keydef;
  mysql_rwlock_t m_rwlock;

  Rdb_seq_generator m_sequence;
  // A queue of table stats to write into data dictionary
  // It is produced by event listener (ie compaction and flush threads)
  // and consumed by the rocksdb background thread
  std::map<GL_INDEX_ID, Rdb_index_stats> m_stats2store;

  const std::shared_ptr<Rdb_key_def> &find(GL_INDEX_ID gl_index_id);

public:
  Rdb_ddl_manager(const Rdb_ddl_manager &) = delete;
  Rdb_ddl_manager &operator=(const Rdb_ddl_manager &) = delete;
  Rdb_ddl_manager() {}

  /* Load the data dictionary from on-disk storage */
  bool init(Rdb_dict_manager *const dict_arg, Rdb_cf_manager *const cf_manager,
            const uint32_t &validate_tables);

  void cleanup();

  Rdb_tbl_def *find(const std::string &table_name, const bool &lock = true);
  std::shared_ptr<const Rdb_key_def> safe_find(GL_INDEX_ID gl_index_id);
  void set_stats(const std::unordered_map<GL_INDEX_ID, Rdb_index_stats> &stats);
  void adjust_stats(const std::vector<Rdb_index_stats> &new_data,
                    const std::vector<Rdb_index_stats> &deleted_data =
                        std::vector<Rdb_index_stats>());
  void persist_stats(const bool &sync = false);

  /* Modify the mapping and write it to on-disk storage */
  int put_and_write(Rdb_tbl_def *const key_descr,
                    rocksdb::WriteBatch *const batch);
  void remove(Rdb_tbl_def *const rec, rocksdb::WriteBatch *const batch,
              const bool &lock = true);
  bool rename(const std::string &from, const std::string &to,
              rocksdb::WriteBatch *const batch);

  uint get_and_update_next_number(Rdb_dict_manager *const dict) {
    return m_sequence.get_and_update_next_number(dict);
  }

  /* Walk the data dictionary */
  int scan_for_tables(Rdb_tables_scanner *tables_scanner);

  void erase_index_num(const GL_INDEX_ID &gl_index_id);

private:
  /* Put the data into in-memory table (only) */
  int put(Rdb_tbl_def *const key_descr, const bool &lock = true);

  /* Helper functions to be passed to my_core::HASH object */
  static const uchar *get_hash_key(Rdb_tbl_def *const rec, size_t *const length,
                                   my_bool not_used MY_ATTRIBUTE((unused)));
  static void free_hash_elem(void *const data);

  bool validate_schemas();
};

/*
  Writing binlog information into RocksDB at commit(),
  and retrieving binlog information at crash recovery.
  commit() and recovery are always executed by at most single client
  at the same time, so concurrency control is not needed.

  Binlog info is stored in RocksDB as the following.
   key: BINLOG_INFO_INDEX_NUMBER
   value: packed single row:
     binlog_name_length (2 byte form)
     binlog_name
     binlog_position (4 byte form)
     binlog_gtid_length (2 byte form)
     binlog_gtid
*/
class Rdb_binlog_manager {
public:
  Rdb_binlog_manager(const Rdb_binlog_manager &) = delete;
  Rdb_binlog_manager &operator=(const Rdb_binlog_manager &) = delete;
  Rdb_binlog_manager() = default;

  bool init(Rdb_dict_manager *const dict);
  void cleanup();
  void update(const char *const binlog_name, const my_off_t binlog_pos,
              const char *const binlog_max_gtid,
              rocksdb::WriteBatchBase *const batch);
  bool read(char *const binlog_name, my_off_t *const binlog_pos,
            char *const binlog_gtid) const;
  void update_slave_gtid_info(const uint &id, const char *const db,
                              const char *const gtid,
                              rocksdb::WriteBatchBase *const write_batch);

private:
  Rdb_dict_manager *m_dict = nullptr;
  uchar m_key_buf[Rdb_key_def::INDEX_NUMBER_SIZE] = {0};
  rocksdb::Slice m_key_slice;

  rocksdb::Slice pack_value(uchar *const buf, const char *const binlog_name,
                            const my_off_t &binlog_pos,
                            const char *const binlog_gtid) const;
  bool unpack_value(const uchar *const value, char *const binlog_name,
                    my_off_t *const binlog_pos, char *const binlog_gtid) const;

  std::atomic<Rdb_tbl_def *> m_slave_gtid_info_tbl;
};

/*
   Rdb_dict_manager manages how MySQL on RocksDB (MyRocks) stores its
  internal data dictionary.
   MyRocks stores data dictionary on dedicated system column family
  named __system__. The system column family is used by MyRocks
  internally only, and not used by applications.

   Currently MyRocks has the following data dictionary data models.

  1. Table Name => internal index id mappings
  key: Rdb_key_def::DDL_ENTRY_INDEX_START_NUMBER(0x1) + dbname.tablename
  value: version, {cf_id, index_id}*n_indexes_of_the_table
  version is 2 bytes. cf_id and index_id are 4 bytes.

  2. internal cf_id, index id => index information
  key: Rdb_key_def::INDEX_INFO(0x2) + cf_id + index_id
  value: version, index_type, kv_format_version
  index_type is 1 byte, version and kv_format_version are 2 bytes.

  3. CF id => CF flags
  key: Rdb_key_def::CF_DEFINITION(0x3) + cf_id
  value: version, {is_reverse_cf, is_auto_cf}
  cf_flags is 4 bytes in total.

  4. Binlog entry (updated at commit)
  key: Rdb_key_def::BINLOG_INFO_INDEX_NUMBER (0x4)
  value: version, {binlog_name,binlog_pos,binlog_gtid}

  5. Ongoing drop index entry
  key: Rdb_key_def::DDL_DROP_INDEX_ONGOING(0x5) + cf_id + index_id
  value: version

  6. index stats
  key: Rdb_key_def::INDEX_STATISTICS(0x6) + cf_id + index_id
  value: version, {materialized PropertiesCollector::IndexStats}

  7. maximum index id
  key: Rdb_key_def::MAX_INDEX_ID(0x7)
  value: index_id
  index_id is 4 bytes

  8. Ongoing create index entry
  key: Rdb_key_def::DDL_CREATE_INDEX_ONGOING(0x8) + cf_id + index_id
  value: version

  Data dictionary operations are atomic inside RocksDB. For example,
  when creating a table with two indexes, it is necessary to call Put
  three times. They have to be atomic. Rdb_dict_manager has a wrapper function
  begin() and commit() to make it easier to do atomic operations.

*/
class Rdb_dict_manager {
private:
  mysql_mutex_t m_mutex;
  rocksdb::DB *m_db = nullptr;
  rocksdb::ColumnFamilyHandle *m_system_cfh = nullptr;
  /* Utility to put INDEX_INFO and CF_DEFINITION */

  uchar m_key_buf_max_index_id[Rdb_key_def::INDEX_NUMBER_SIZE] = {0};
  rocksdb::Slice m_key_slice_max_index_id;

  static void dump_index_id(uchar *const netbuf,
                            Rdb_key_def::DATA_DICT_TYPE dict_type,
                            const GL_INDEX_ID &gl_index_id);
  void delete_with_prefix(rocksdb::WriteBatch *const batch,
                          Rdb_key_def::DATA_DICT_TYPE dict_type,
                          const GL_INDEX_ID &gl_index_id) const;
  /* Functions for fast DROP TABLE/INDEX */
  void resume_drop_indexes() const;
  void log_start_drop_table(const std::shared_ptr<Rdb_key_def> *const key_descr,
                            const uint32 &n_keys,
                            const char *const log_action) const;
  void log_start_drop_index(GL_INDEX_ID gl_index_id,
                            const char *log_action) const;

public:
  Rdb_dict_manager(const Rdb_dict_manager &) = delete;
  Rdb_dict_manager &operator=(const Rdb_dict_manager &) = delete;
  Rdb_dict_manager() = default;

  bool init(rocksdb::DB *const rdb_dict, Rdb_cf_manager *const cf_manager);

  inline void cleanup() { mysql_mutex_destroy(&m_mutex); }

  inline void lock() { mysql_mutex_lock(&m_mutex); }

  inline void unlock() { mysql_mutex_unlock(&m_mutex); }

  /* Raw RocksDB operations */
  std::unique_ptr<rocksdb::WriteBatch> begin() const;
  int commit(rocksdb::WriteBatch *const batch, const bool &sync = true) const;
  rocksdb::Status get_value(const rocksdb::Slice &key,
                            std::string *const value) const;
  void put_key(rocksdb::WriteBatchBase *const batch, const rocksdb::Slice &key,
               const rocksdb::Slice &value) const;
  void delete_key(rocksdb::WriteBatchBase *batch,
                  const rocksdb::Slice &key) const;
  rocksdb::Iterator *new_iterator() const;

  /* Internal Index id => CF */
  void add_or_update_index_cf_mapping(rocksdb::WriteBatch *batch,
                                      const uchar index_type,
                                      const uint16_t kv_version,
                                      const uint index_id,
                                      const uint cf_id) const;
  void delete_index_info(rocksdb::WriteBatch *batch,
                         const GL_INDEX_ID &index_id) const;
  bool get_index_info(const GL_INDEX_ID &gl_index_id,
                      uint16_t *index_dict_version, uchar *index_type,
                      uint16_t *kv_version) const;

  /* CF id => CF flags */
  void add_cf_flags(rocksdb::WriteBatch *const batch, const uint &cf_id,
                    const uint &cf_flags) const;
  bool get_cf_flags(const uint &cf_id, uint *const cf_flags) const;

  /* Functions for fast CREATE/DROP TABLE/INDEX */
  void
  get_ongoing_index_operation(std::unordered_set<GL_INDEX_ID> *gl_index_ids,
                              Rdb_key_def::DATA_DICT_TYPE dd_type) const;
  bool is_index_operation_ongoing(const GL_INDEX_ID &gl_index_id,
                                  Rdb_key_def::DATA_DICT_TYPE dd_type) const;
  void start_ongoing_index_operation(rocksdb::WriteBatch *batch,
                                     const GL_INDEX_ID &gl_index_id,
                                     Rdb_key_def::DATA_DICT_TYPE dd_type) const;
  void end_ongoing_index_operation(rocksdb::WriteBatch *const batch,
                                   const GL_INDEX_ID &gl_index_id,
                                   Rdb_key_def::DATA_DICT_TYPE dd_type) const;
  bool is_drop_index_empty() const;
  void add_drop_table(std::shared_ptr<Rdb_key_def> *const key_descr,
                      const uint32 &n_keys,
                      rocksdb::WriteBatch *const batch) const;
  void add_drop_index(const std::unordered_set<GL_INDEX_ID> &gl_index_ids,
                      rocksdb::WriteBatch *const batch) const;
  void add_create_index(const std::unordered_set<GL_INDEX_ID> &gl_index_ids,
                        rocksdb::WriteBatch *const batch) const;
  void
  finish_indexes_operation(const std::unordered_set<GL_INDEX_ID> &gl_index_ids,
                           Rdb_key_def::DATA_DICT_TYPE dd_type) const;
  void rollback_ongoing_index_creation() const;

  inline void get_ongoing_drop_indexes(
      std::unordered_set<GL_INDEX_ID> *gl_index_ids) const {
    get_ongoing_index_operation(gl_index_ids,
                                Rdb_key_def::DDL_DROP_INDEX_ONGOING);
  }
  inline void get_ongoing_create_indexes(
      std::unordered_set<GL_INDEX_ID> *gl_index_ids) const {
    get_ongoing_index_operation(gl_index_ids,
                                Rdb_key_def::DDL_CREATE_INDEX_ONGOING);
  }
  inline void start_drop_index(rocksdb::WriteBatch *wb,
                               const GL_INDEX_ID &gl_index_id) const {
    start_ongoing_index_operation(wb, gl_index_id,
                                  Rdb_key_def::DDL_DROP_INDEX_ONGOING);
  }
  inline void start_create_index(rocksdb::WriteBatch *wb,
                                 const GL_INDEX_ID &gl_index_id) const {
    start_ongoing_index_operation(wb, gl_index_id,
                                  Rdb_key_def::DDL_CREATE_INDEX_ONGOING);
  }
  inline void finish_drop_indexes(
      const std::unordered_set<GL_INDEX_ID> &gl_index_ids) const {
    finish_indexes_operation(gl_index_ids, Rdb_key_def::DDL_DROP_INDEX_ONGOING);
  }
  inline void finish_create_indexes(
      const std::unordered_set<GL_INDEX_ID> &gl_index_ids) const {
    finish_indexes_operation(gl_index_ids,
                             Rdb_key_def::DDL_CREATE_INDEX_ONGOING);
  }
  inline bool is_drop_index_ongoing(const GL_INDEX_ID &gl_index_id) const {
    return is_index_operation_ongoing(gl_index_id,
                                      Rdb_key_def::DDL_DROP_INDEX_ONGOING);
  }
  inline bool is_create_index_ongoing(const GL_INDEX_ID &gl_index_id) const {
    return is_index_operation_ongoing(gl_index_id,
                                      Rdb_key_def::DDL_CREATE_INDEX_ONGOING);
  }

  bool get_max_index_id(uint32_t *const index_id) const;
  bool update_max_index_id(rocksdb::WriteBatch *const batch,
                           const uint32_t &index_id) const;
  void add_stats(rocksdb::WriteBatch *const batch,
                 const std::vector<Rdb_index_stats> &stats) const;
  Rdb_index_stats get_stats(GL_INDEX_ID gl_index_id) const;
};

} // namespace myrocks
