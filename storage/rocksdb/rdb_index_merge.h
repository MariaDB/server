/*
   Copyright (c) 2016, Facebook, Inc.

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

/* MySQL header files */
#include "../sql/log.h"
#include "./handler.h"                     /* handler */
#include "./my_global.h"                   /* ulonglong */

/* C++ standard header files */
#include <set>
#include <vector>
#include <queue>

/* RocksDB header files */
#include "rocksdb/db.h"

/* MyRocks header files */
#include "./rdb_comparator.h"

namespace myrocks {

/*
  Length of delimiters used during inplace index creation.
*/
#define RDB_MERGE_CHUNK_LEN sizeof(size_t)
#define RDB_MERGE_REC_DELIMITER sizeof(size_t)
#define RDB_MERGE_KEY_DELIMITER RDB_MERGE_REC_DELIMITER
#define RDB_MERGE_VAL_DELIMITER RDB_MERGE_REC_DELIMITER

class Rdb_key_def;
class Rdb_tbl_def;

class Rdb_index_merge {
  Rdb_index_merge(const Rdb_index_merge& p)= delete;
  Rdb_index_merge& operator=(const Rdb_index_merge& p)= delete;

 public:
  /* Information about temporary files used in external merge sort */
  struct merge_file_info {
    File  fd= -1; /* file descriptor */
    ulong num_sort_buffers; /* number of sort buffers in temp file */
  };

  /* Buffer for sorting in main memory. */
  struct merge_buf_info {
    /* heap memory allocated for main memory sort/merge  */
    std::unique_ptr<uchar[]> block;
    ulonglong block_len; /* amount of data bytes allocated for block above */
    ulonglong curr_offset; /* offset of the record pointer for the block */
    ulonglong disk_start_offset; /* where the chunk starts on disk */
    ulonglong disk_curr_offset; /* current offset on disk */
    ulonglong total_size; /* total # of data bytes in chunk */

    void store_key_value(const rocksdb::Slice& key, const rocksdb::Slice& val)
      __attribute__((__nonnull__));

    void store_slice(const rocksdb::Slice& slice)
      __attribute__((__nonnull__));

    size_t prepare(File fd, ulonglong f_offset)
      __attribute__((__nonnull__));

    int read_next_chunk_from_disk(File fd)
      __attribute__((__nonnull__, __warn_unused_result__));

    inline bool is_chunk_finished()
    {
      return curr_offset + disk_curr_offset - disk_start_offset == total_size;
    }

    inline bool has_space(uint64 needed)
    {
      return curr_offset + needed <= block_len;
    }

    explicit merge_buf_info(const ulonglong merge_block_size) :
      block(nullptr), block_len(merge_block_size), curr_offset(0),
      disk_start_offset(0), disk_curr_offset(0), total_size(merge_block_size)
    {
      /* Will throw an exception if it runs out of memory here */
      block= std::unique_ptr<uchar[]>(new uchar[merge_block_size]);

      /* Initialize entire buffer to 0 to avoid valgrind errors */
      memset(block.get(), 0, merge_block_size);
    }
  };

  /* Represents an entry in the heap during merge phase of external sort */
  struct merge_heap_entry
  {
    std::shared_ptr<merge_buf_info> chunk_info; /* pointer to buffer info */
    uchar* block; /* pointer to heap memory where record is stored */
    const rocksdb::Comparator* const comparator;
    rocksdb::Slice key; /* current key pointed to by block ptr */
    rocksdb::Slice val;

    size_t prepare(File fd, ulonglong f_offset, ulonglong chunk_size)
      __attribute__((__nonnull__));

    int read_next_chunk_from_disk(File fd)
      __attribute__((__nonnull__, __warn_unused_result__));

    int read_rec(rocksdb::Slice *key, rocksdb::Slice *val)
      __attribute__((__nonnull__, __warn_unused_result__));

    int read_slice(rocksdb::Slice* slice, const uchar** block_ptr)
      __attribute__((__nonnull__, __warn_unused_result__));

    explicit merge_heap_entry(const rocksdb::Comparator* const comparator) :
      chunk_info(nullptr), block(nullptr), comparator(comparator) {}
  };

  struct merge_heap_comparator
  {
    bool operator() (const std::shared_ptr<merge_heap_entry>& lhs,
                     const std::shared_ptr<merge_heap_entry>& rhs)
    {
      return lhs->comparator->Compare(rhs->key, lhs->key) < 0;
    }
  };

  /* Represents a record in unsorted buffer */
  struct merge_record
  {
    uchar* block; /* points to offset of key in sort buffer */
    const rocksdb::Comparator* const comparator;

    bool operator< (const merge_record &record) const
    {
      return merge_record_compare(this->block, record.block, comparator) < 0;
    }

    merge_record(uchar* block, const rocksdb::Comparator* const comparator) :
      block(block), comparator(comparator) {}
  };

 private:
  const ulonglong                               m_merge_buf_size;
  const ulonglong                               m_merge_combine_read_size;
  const rocksdb::Comparator*                    m_comparator;
  struct merge_file_info                        m_merge_file;
  std::shared_ptr<merge_buf_info>               m_rec_buf_unsorted;
  std::shared_ptr<merge_buf_info>               m_output_buf;
  std::set<merge_record>                        m_offset_tree;
  std::priority_queue<std::shared_ptr<merge_heap_entry>,
                      std::vector<std::shared_ptr<merge_heap_entry>>,
                      merge_heap_comparator>    m_merge_min_heap;

  static inline void merge_store_uint64(uchar *dst, uint64 n)
  {
    memcpy(dst, &n, sizeof(n));
  }

  static inline void merge_read_uint64(const uchar **buf_ptr, uint64 *dst)
  {
    DBUG_ASSERT(buf_ptr != nullptr);
    memcpy(dst, *buf_ptr, sizeof(uint64));
    *buf_ptr += sizeof(uint64);
  }

  static inline rocksdb::Slice as_slice(const uchar* block)
  {
    uint64 len;
    merge_read_uint64(&block, &len);

    return rocksdb::Slice(reinterpret_cast<const char*>(block), len);
  }

  static int merge_record_compare(const uchar* a_block, const uchar* b_block,
                                  const rocksdb::Comparator* const comparator)
    __attribute__((__nonnull__, __warn_unused_result__));

  void merge_read_rec(const uchar* block, rocksdb::Slice* key,
                     rocksdb::Slice* val)
    __attribute__((__nonnull__));

  void read_slice(rocksdb::Slice* slice, const uchar* block_ptr)
    __attribute__((__nonnull__));

 public:
  Rdb_index_merge(const ulonglong merge_buf_size,
                  const ulonglong merge_combine_read_size,
                  const rocksdb::Comparator* comparator);
  ~Rdb_index_merge();

  int init()
    __attribute__((__nonnull__, __warn_unused_result__));

  int merge_file_create()
    __attribute__((__nonnull__, __warn_unused_result__));

  int add(const rocksdb::Slice& key, const rocksdb::Slice& val)
    __attribute__((__nonnull__, __warn_unused_result__));

  int merge_buf_write()
    __attribute__((__nonnull__, __warn_unused_result__));

  int next(rocksdb::Slice* key, rocksdb::Slice* val)
    __attribute__((__nonnull__, __warn_unused_result__));

  int merge_heap_prepare()
    __attribute__((__nonnull__, __warn_unused_result__));

  void merge_heap_top(rocksdb::Slice* key, rocksdb::Slice* val)
    __attribute__((__nonnull__));

  int merge_heap_pop_and_get_next(rocksdb::Slice* key, rocksdb::Slice* val)
    __attribute__((__nonnull__, __warn_unused_result__));

  void merge_reset();
};

}  // namespace myrocks

