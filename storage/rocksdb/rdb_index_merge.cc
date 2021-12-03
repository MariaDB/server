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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include <my_global.h>

/* This C++ file's header file */
#include "./rdb_index_merge.h"

/* MySQL header files */
#include "../sql/sql_class.h"

/* MyRocks header files */
#include "./ha_rocksdb.h"
#include "./rdb_datadic.h"

namespace myrocks {

Rdb_index_merge::Rdb_index_merge(const char *const tmpfile_path,
                                 const ulonglong merge_buf_size,
                                 const ulonglong merge_combine_read_size,
                                 const ulonglong merge_tmp_file_removal_delay,
                                 rocksdb::ColumnFamilyHandle *cf)
    : m_tmpfile_path(tmpfile_path),
      m_merge_buf_size(merge_buf_size),
      m_merge_combine_read_size(merge_combine_read_size),
      m_merge_tmp_file_removal_delay(merge_tmp_file_removal_delay),
      m_cf_handle(cf),
      m_rec_buf_unsorted(nullptr),
      m_output_buf(nullptr) {}

Rdb_index_merge::~Rdb_index_merge() {
  /*
    If merge_tmp_file_removal_delay is set, sleep between calls to chsize.

    This helps mitigate potential trim stalls on flash when large files are
    being deleted too quickly.
  */
  if (m_merge_tmp_file_removal_delay > 0) {
    uint64 curr_size = m_merge_buf_size * m_merge_file.m_num_sort_buffers;
    for (uint i = 0; i < m_merge_file.m_num_sort_buffers; i++) {
      if (my_chsize(m_merge_file.m_fd, curr_size, 0, MYF(MY_WME))) {
        // NO_LINT_DEBUG
        sql_print_error("Error truncating file during fast index creation.");
      }

      my_sleep(m_merge_tmp_file_removal_delay * 1000);
      // Not aborting on fsync error since the tmp file is not used anymore
      if (mysql_file_sync(m_merge_file.m_fd, MYF(MY_WME))) {
        // NO_LINT_DEBUG
        sql_print_error("Error flushing truncated MyRocks merge buffer.");
      }
      curr_size -= m_merge_buf_size;
    }
  }

  /*
    Close file descriptor, we don't need to worry about deletion,
    mysql handles it.
  */
  my_close(m_merge_file.m_fd, MYF(MY_WME));
}

int Rdb_index_merge::init() {
  /*
    Create a temporary merge file on disk to store sorted chunks during
    inplace index creation.
  */
  if (merge_file_create()) {
    return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
  }

  /*
    Then, allocate buffer to store unsorted records before they are written
    to disk. They will be written to disk sorted. A sorted tree is used to
    keep track of the offset of each record within the unsorted buffer.
  */
  m_rec_buf_unsorted =
      std::shared_ptr<merge_buf_info>(new merge_buf_info(m_merge_buf_size));

  /*
    Allocate output buffer that will contain sorted block that is written to
    disk.
  */
  m_output_buf =
      std::shared_ptr<merge_buf_info>(new merge_buf_info(m_merge_buf_size));

  return HA_EXIT_SUCCESS;
}

/**
  Create a merge file in the given location.
*/
int Rdb_index_merge::merge_file_create() {
  DBUG_ASSERT(m_merge_file.m_fd == -1);

  int fd;
#ifdef MARIAROCKS_NOT_YET // mysql_tmpfile_path use 
  /* If no path set for tmpfile, use mysql_tmpdir by default */
  if (m_tmpfile_path == nullptr) {
    fd = mysql_tmpfile("myrocks");
  } else {
    fd = mysql_tmpfile_path(m_tmpfile_path, "myrocks");
  }
#else
  fd = mysql_tmpfile("myrocks");
#endif
  if (fd < 0) {
    // NO_LINT_DEBUG
    sql_print_error("Failed to create temp file during fast index creation.");
    return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
  }

  m_merge_file.m_fd = fd;
  m_merge_file.m_num_sort_buffers = 0;

  return HA_EXIT_SUCCESS;
}

/**
  Add record to offset tree (and unsorted merge buffer) in preparation for
  writing out to disk in sorted chunks.

  If buffer in memory is full, write the buffer out to disk sorted using the
  offset tree, and clear the tree. (Happens in merge_buf_write)
*/
int Rdb_index_merge::add(const rocksdb::Slice &key, const rocksdb::Slice &val) {
  /* Adding a record after heap is already created results in error */
  DBUG_ASSERT(m_merge_min_heap.empty());

  /*
    Check if sort buffer is going to be out of space, if so write it
    out to disk in sorted order using offset tree.
  */
  const uint total_offset = RDB_MERGE_CHUNK_LEN +
                            m_rec_buf_unsorted->m_curr_offset +
                            RDB_MERGE_KEY_DELIMITER + RDB_MERGE_VAL_DELIMITER +
                            key.size() + val.size();
  if (total_offset >= m_rec_buf_unsorted->m_total_size) {
    /*
      If the offset tree is empty here, that means that the proposed key to
      add is too large for the buffer.
    */
    if (m_offset_tree.empty()) {
      // NO_LINT_DEBUG
      sql_print_error(
          "Sort buffer size is too small to process merge. "
          "Please set merge buffer size to a higher value.");
      return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
    }

    if (merge_buf_write()) {
      // NO_LINT_DEBUG
      sql_print_error("Error writing sort buffer to disk.");
      return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
    }
  }

  const ulonglong rec_offset = m_rec_buf_unsorted->m_curr_offset;

  /*
    Store key and value in temporary unsorted in memory buffer pointed to by
    offset tree.
  */
  m_rec_buf_unsorted->store_key_value(key, val);

  /* Find sort order of the new record */
  auto res =
      m_offset_tree.emplace(m_rec_buf_unsorted->m_block.get() + rec_offset,
                            m_cf_handle->GetComparator());
  if (!res.second) {
    my_printf_error(ER_DUP_ENTRY,
                    "Failed to insert the record: the key already exists",
                    MYF(0));
    return ER_DUP_ENTRY;
  }

  return HA_EXIT_SUCCESS;
}

/**
  Sort + write merge buffer chunk out to disk.
*/
int Rdb_index_merge::merge_buf_write() {
  DBUG_ASSERT(m_merge_file.m_fd != -1);
  DBUG_ASSERT(m_rec_buf_unsorted != nullptr);
  DBUG_ASSERT(m_output_buf != nullptr);
  DBUG_ASSERT(!m_offset_tree.empty());

  /* Write actual chunk size to first 8 bytes of the merge buffer */
  merge_store_uint64(m_output_buf->m_block.get(),
                     m_rec_buf_unsorted->m_curr_offset + RDB_MERGE_CHUNK_LEN);
  m_output_buf->m_curr_offset += RDB_MERGE_CHUNK_LEN;

  /*
    Iterate through the offset tree.  Should be ordered by the secondary key
    at this point.
  */
  for (const auto &rec : m_offset_tree) {
    DBUG_ASSERT(m_output_buf->m_curr_offset <= m_merge_buf_size);

    /* Read record from offset (should never fail) */
    rocksdb::Slice key;
    rocksdb::Slice val;
    merge_read_rec(rec.m_block, &key, &val);

    /* Store key and value into sorted output buffer */
    m_output_buf->store_key_value(key, val);
  }

  DBUG_ASSERT(m_output_buf->m_curr_offset <= m_output_buf->m_total_size);

  /*
    Write output buffer to disk.

    Need to position cursor to the chunk it needs to be at on filesystem
    then write into the respective merge buffer.
  */
  if (my_seek(m_merge_file.m_fd,
              m_merge_file.m_num_sort_buffers * m_merge_buf_size, SEEK_SET,
              MYF(0)) == MY_FILEPOS_ERROR) {
    // NO_LINT_DEBUG
    sql_print_error("Error seeking to location in merge file on disk.");
    return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
  }

  /*
    Add a file sync call here to flush the data out. Otherwise, the filesystem
    cache can flush out all of the files at the same time, causing a write
    burst.
  */
  if (my_write(m_merge_file.m_fd, m_output_buf->m_block.get(),
               m_output_buf->m_total_size, MYF(MY_WME | MY_NABP)) ||
      mysql_file_sync(m_merge_file.m_fd, MYF(MY_WME))) {
    // NO_LINT_DEBUG
    sql_print_error("Error writing sorted merge buffer to disk.");
    return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
  }

  /* Increment merge file offset to track number of merge buffers written */
  m_merge_file.m_num_sort_buffers += 1;

  /* Reset everything for next run */
  merge_reset();

  return HA_EXIT_SUCCESS;
}

/**
  Prepare n-way merge of n sorted buffers on disk, using a heap sorted by
  secondary key records.
*/
int Rdb_index_merge::merge_heap_prepare() {
  DBUG_ASSERT(m_merge_min_heap.empty());

  /*
    If the offset tree is not empty, there are still some records that need to
    be written to disk. Write them out now.
  */
  if (!m_offset_tree.empty() && merge_buf_write()) {
    return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
  }

  DBUG_ASSERT(m_merge_file.m_num_sort_buffers > 0);

  /*
    For an n-way merge, we need to read chunks of each merge file
    simultaneously.
  */
  ulonglong chunk_size =
      m_merge_combine_read_size / m_merge_file.m_num_sort_buffers;
  if (chunk_size >= m_merge_buf_size) {
    chunk_size = m_merge_buf_size;
  }

  /* Allocate buffers for each chunk */
  for (ulonglong i = 0; i < m_merge_file.m_num_sort_buffers; i++) {
    const auto entry =
        std::make_shared<merge_heap_entry>(m_cf_handle->GetComparator());

    /*
      Read chunk_size bytes from each chunk on disk, and place inside
      respective chunk buffer.
    */
    const size_t total_size =
        entry->prepare(m_merge_file.m_fd, i * m_merge_buf_size, chunk_size);

    if (total_size == (size_t)-1) {
      return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
    }

    /* Can reach this condition if an index was added on table w/ no rows */
    if (total_size - RDB_MERGE_CHUNK_LEN == 0) {
      break;
    }

    /* Read the first record from each buffer to initially populate the heap */
    if (entry->read_rec(&entry->m_key, &entry->m_val)) {
      // NO_LINT_DEBUG
      sql_print_error("Chunk size is too small to process merge.");
      return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
    }

    m_merge_min_heap.push(std::move(entry));
  }

  return HA_EXIT_SUCCESS;
}

/**
  Create and/or iterate through keys in the merge heap.
*/
int Rdb_index_merge::next(rocksdb::Slice *const key,
                          rocksdb::Slice *const val) {
  /*
    If table fits in one sort buffer, we can optimize by writing
    the sort buffer directly through to the sstfilewriter instead of
    needing to create tmp files/heap to merge the sort buffers.

    If there are no sort buffer records (alters on empty tables),
    also exit here.
  */
  if (m_merge_file.m_num_sort_buffers == 0) {
    if (m_offset_tree.empty()) {
      return -1;
    }

    const auto rec = m_offset_tree.begin();

    /* Read record from offset */
    merge_read_rec(rec->m_block, key, val);

    m_offset_tree.erase(rec);
    return HA_EXIT_SUCCESS;
  }

  int res;

  /*
    If heap and heap chunk info are empty, we must be beginning the merge phase
    of the external sort. Populate the heap with initial values from each
    disk chunk.
  */
  if (m_merge_min_heap.empty()) {
    if ((res = merge_heap_prepare())) {
      // NO_LINT_DEBUG
      sql_print_error("Error during preparation of heap.");
      return res;
    }

    /*
      Return the first top record without popping, as we haven't put this
      inside the SST file yet.
    */
    merge_heap_top(key, val);
    return HA_EXIT_SUCCESS;
  }

  DBUG_ASSERT(!m_merge_min_heap.empty());
  return merge_heap_pop_and_get_next(key, val);
}

/**
  Get current top record from the heap.
*/
void Rdb_index_merge::merge_heap_top(rocksdb::Slice *const key,
                                     rocksdb::Slice *const val) {
  DBUG_ASSERT(!m_merge_min_heap.empty());

  const std::shared_ptr<merge_heap_entry> &entry = m_merge_min_heap.top();
  *key = entry->m_key;
  *val = entry->m_val;
}

/**
  Pops the top record, and uses it to read next record from the
  corresponding sort buffer and push onto the heap.

  Returns -1 when there are no more records in the heap.
*/
int Rdb_index_merge::merge_heap_pop_and_get_next(rocksdb::Slice *const key,
                                                 rocksdb::Slice *const val) {
  /*
    Make a new reference to shared ptr so it doesn't get destroyed
    during pop(). We are going to push this entry back onto the heap.
  */
  const std::shared_ptr<merge_heap_entry> entry = m_merge_min_heap.top();
  m_merge_min_heap.pop();

  /*
    We are finished w/ current chunk if:
    current_offset + disk_offset == m_total_size

    Return without adding entry back onto heap.
    If heap is also empty, we must be finished with merge.
  */
  if (entry->m_chunk_info->is_chunk_finished()) {
    if (m_merge_min_heap.empty()) {
      return -1;
    }

    merge_heap_top(key, val);
    return HA_EXIT_SUCCESS;
  }

  /*
    Make sure we haven't reached the end of the chunk.
  */
  DBUG_ASSERT(!entry->m_chunk_info->is_chunk_finished());

  /*
    If merge_read_rec fails, it means the either the chunk was cut off
    or we've reached the end of the respective chunk.
  */
  if (entry->read_rec(&entry->m_key, &entry->m_val)) {
    if (entry->read_next_chunk_from_disk(m_merge_file.m_fd)) {
      return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
    }

    /* Try reading record again, should never fail. */
    if (entry->read_rec(&entry->m_key, &entry->m_val)) {
      return HA_ERR_ROCKSDB_MERGE_FILE_ERR;
    }
  }

  /* Push entry back on to the heap w/ updated buffer + offset ptr */
  m_merge_min_heap.push(std::move(entry));

  /* Return the current top record on heap */
  merge_heap_top(key, val);
  return HA_EXIT_SUCCESS;
}

int Rdb_index_merge::merge_heap_entry::read_next_chunk_from_disk(File fd) {
  if (m_chunk_info->read_next_chunk_from_disk(fd)) {
    return HA_EXIT_FAILURE;
  }

  m_block = m_chunk_info->m_block.get();
  return HA_EXIT_SUCCESS;
}

int Rdb_index_merge::merge_buf_info::read_next_chunk_from_disk(File fd) {
  m_disk_curr_offset += m_curr_offset;

  if (my_seek(fd, m_disk_curr_offset, SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    // NO_LINT_DEBUG
    sql_print_error("Error seeking to location in merge file on disk.");
    return HA_EXIT_FAILURE;
  }

  /* Overwrite the old block */
  const size_t bytes_read =
      my_read(fd, m_block.get(), m_block_len, MYF(MY_WME));
  if (bytes_read == (size_t)-1) {
    // NO_LINT_DEBUG
    sql_print_error("Error reading merge file from disk.");
    return HA_EXIT_FAILURE;
  }

  m_curr_offset = 0;
  return HA_EXIT_SUCCESS;
}

/**
  Get records from offset within sort buffer and compare them.
  Sort by least to greatest.
*/
int Rdb_index_merge::merge_record_compare(
    const uchar *const a_block, const uchar *const b_block,
    const rocksdb::Comparator *const comparator) {
  return comparator->Compare(as_slice(a_block), as_slice(b_block));
}

/**
  Given an offset in a merge sort buffer, read out the keys + values.
  After this, block will point to the next record in the buffer.
**/
void Rdb_index_merge::merge_read_rec(const uchar *const block,
                                     rocksdb::Slice *const key,
                                     rocksdb::Slice *const val) {
  /* Read key at block offset into key slice and the value into value slice*/
  read_slice(key, block);
  read_slice(val, block + RDB_MERGE_REC_DELIMITER + key->size());
}

void Rdb_index_merge::read_slice(rocksdb::Slice *slice,
                                 const uchar *block_ptr) {
  uint64 slice_len;
  merge_read_uint64(&block_ptr, &slice_len);

  *slice = rocksdb::Slice(reinterpret_cast<const char *>(block_ptr), slice_len);
}

int Rdb_index_merge::merge_heap_entry::read_rec(rocksdb::Slice *const key,
                                                rocksdb::Slice *const val) {
  const uchar *block_ptr = m_block;
  const auto orig_offset = m_chunk_info->m_curr_offset;
  const auto orig_block = m_block;

  /* Read key at block offset into key slice and the value into value slice*/
  if (read_slice(key, &block_ptr) != 0) {
    return HA_EXIT_FAILURE;
  }

  m_chunk_info->m_curr_offset += (uintptr_t)block_ptr - (uintptr_t)m_block;
  m_block += (uintptr_t)block_ptr - (uintptr_t)m_block;

  if (read_slice(val, &block_ptr) != 0) {
    m_chunk_info->m_curr_offset = orig_offset;
    m_block = orig_block;
    return HA_EXIT_FAILURE;
  }

  m_chunk_info->m_curr_offset += (uintptr_t)block_ptr - (uintptr_t)m_block;
  m_block += (uintptr_t)block_ptr - (uintptr_t)m_block;

  return HA_EXIT_SUCCESS;
}

int Rdb_index_merge::merge_heap_entry::read_slice(rocksdb::Slice *const slice,
                                                  const uchar **block_ptr) {
  if (!m_chunk_info->has_space(RDB_MERGE_REC_DELIMITER)) {
    return HA_EXIT_FAILURE;
  }

  uint64 slice_len;
  merge_read_uint64(block_ptr, &slice_len);
  if (!m_chunk_info->has_space(RDB_MERGE_REC_DELIMITER + slice_len)) {
    return HA_EXIT_FAILURE;
  }

  *slice =
      rocksdb::Slice(reinterpret_cast<const char *>(*block_ptr), slice_len);
  *block_ptr += slice_len;
  return HA_EXIT_SUCCESS;
}

size_t Rdb_index_merge::merge_heap_entry::prepare(File fd, ulonglong f_offset,
                                                  ulonglong chunk_size) {
  m_chunk_info = std::make_shared<merge_buf_info>(chunk_size);
  const size_t res = m_chunk_info->prepare(fd, f_offset);
  if (res != (size_t)-1) {
    m_block = m_chunk_info->m_block.get() + RDB_MERGE_CHUNK_LEN;
  }

  return res;
}

size_t Rdb_index_merge::merge_buf_info::prepare(File fd, ulonglong f_offset) {
  m_disk_start_offset = f_offset;
  m_disk_curr_offset = f_offset;

  /*
    Need to position cursor to the chunk it needs to be at on filesystem
    then read 'chunk_size' bytes into the respective chunk buffer.
  */
  if (my_seek(fd, f_offset, SEEK_SET, MYF(0)) == MY_FILEPOS_ERROR) {
    // NO_LINT_DEBUG
    sql_print_error("Error seeking to location in merge file on disk.");
    return (size_t)-1;
  }

  const size_t bytes_read =
      my_read(fd, m_block.get(), m_total_size, MYF(MY_WME));
  if (bytes_read == (size_t)-1) {
    // NO_LINT_DEBUG
    sql_print_error("Error reading merge file from disk.");
    return (size_t)-1;
  }

  /*
    Read the first 8 bytes of each chunk, this gives us the actual
    size of each chunk.
  */
  const uchar *block_ptr = m_block.get();
  merge_read_uint64(&block_ptr, &m_total_size);
  m_curr_offset += RDB_MERGE_CHUNK_LEN;
  return m_total_size;
}

/* Store key and value w/ their respective delimiters at the given offset */
void Rdb_index_merge::merge_buf_info::store_key_value(
    const rocksdb::Slice &key, const rocksdb::Slice &val) {
  store_slice(key);
  store_slice(val);
}

void Rdb_index_merge::merge_buf_info::store_slice(const rocksdb::Slice &slice) {
  /* Store length delimiter */
  merge_store_uint64(&m_block[m_curr_offset], slice.size());

  /* Store slice data */
  memcpy(&m_block[m_curr_offset + RDB_MERGE_REC_DELIMITER], slice.data(),
         slice.size());

  m_curr_offset += slice.size() + RDB_MERGE_REC_DELIMITER;
}

void Rdb_index_merge::merge_reset() {
  /*
    Either error, or all values in the sort buffer have been written to disk,
    so we need to clear the offset tree.
  */
  m_offset_tree.clear();

  /* Reset sort buffer block */
  if (m_rec_buf_unsorted && m_rec_buf_unsorted->m_block) {
    m_rec_buf_unsorted->m_curr_offset = 0;
  }

  /* Reset output buf */
  if (m_output_buf && m_output_buf->m_block) {
    m_output_buf->m_curr_offset = 0;
  }
}

}  // namespace myrocks
