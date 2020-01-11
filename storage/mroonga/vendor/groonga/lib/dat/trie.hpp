/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#include "array.hpp"
#include "header.hpp"
#include "node.hpp"
#include "block.hpp"
#include "entry.hpp"
#include "key.hpp"
#include "file.hpp"

namespace grn {
namespace dat {

class GRN_DAT_API Trie {
 public:
  Trie();
  ~Trie();

  void create(const char *file_name = NULL,
              UInt64 file_size = 0,
              UInt32 max_num_keys = 0,
              double num_nodes_per_key = 0.0,
              double average_key_length = 0.0);

  void create(const Trie &trie,
              const char *file_name = NULL,
              UInt64 file_size = 0,
              UInt32 max_num_keys = 0,
              double num_nodes_per_key = 0.0,
              double average_key_length = 0.0);

  void repair(const Trie &trie, const char *file_name = NULL);

  void open(const char *file_name);
  void close();

  void swap(Trie *trie);

  // Users can access a key by its position or ID.
  const Key &get_key(UInt32 key_pos) const {
    GRN_DAT_DEBUG_THROW_IF(key_pos >= next_key_pos());
    return *reinterpret_cast<const Key *>(key_buf_.ptr() + key_pos);
  }
  // If a specified ID is invalid, e.g. the key is already deleted, this
  // function returns a reference to an invalid key object whose id() returns
  // INVALID_KEY_ID.
  const Key &ith_key(UInt32 key_id) const {
    if ((key_id >= min_key_id()) && (key_id <= max_key_id()) &&
        ith_entry(key_id).is_valid()) {
      return get_key(ith_entry(key_id).key_pos());
    }
    return Key::invalid_key();
  }

  bool search(const void *ptr, UInt32 length, UInt32 *key_pos = NULL) const {
    return search_key(static_cast<const UInt8 *>(ptr), length, key_pos);
  }
  // Longest prefix match search.
  bool lcp_search(const void *ptr, UInt32 length,
                  UInt32 *key_pos = NULL) const {
    return lcp_search_key(static_cast<const UInt8 *>(ptr), length, key_pos);
  }

  bool remove(UInt32 key_id) {
    const Key &key = ith_key(key_id);
    if (key.is_valid()) {
      return remove(key.ptr(), key.length());
    }
    return false;
  }
  bool remove(const void *ptr, UInt32 length) {
    return remove_key(static_cast<const UInt8 *>(ptr), length);
  }

  bool insert(const void *ptr, UInt32 length, UInt32 *key_pos = NULL) {
    return insert_key(static_cast<const UInt8 *>(ptr), length, key_pos);
  }

  bool update(UInt32 key_id, const void *ptr, UInt32 length,
              UInt32 *key_pos = NULL) {
    return update_key(ith_key(key_id), static_cast<const UInt8 *>(ptr),
                      length, key_pos);
  }
  bool update(const void *src_ptr, UInt32 src_length,
              const void *dest_ptr, UInt32 dest_length,
              UInt32 *key_pos = NULL) {
    UInt32 src_key_pos;
    if (!search(src_ptr, src_length, &src_key_pos)) {
      return false;
    }
    const Key &src_key = get_key(src_key_pos);
    return update_key(src_key, static_cast<const UInt8 *>(dest_ptr),
                      dest_length, key_pos);
  }

  const Node &ith_node(UInt32 i) const {
    GRN_DAT_DEBUG_THROW_IF(i >= num_nodes());
    return nodes_[i];
  }
  const Block &ith_block(UInt32 i) const {
    GRN_DAT_DEBUG_THROW_IF(i >= num_blocks());
    return blocks_[i];
  }
  const Entry &ith_entry(UInt32 i) const {
    GRN_DAT_DEBUG_THROW_IF(i < min_key_id());
    GRN_DAT_DEBUG_THROW_IF(i > max_key_id());
    return entries_[i];
  }

  const Header &header() const {
    return *header_;
  }

  UInt64 file_size() const {
    return header_->file_size();
  }
  UInt64 virtual_size() const {
    return sizeof(Header)
        + (sizeof(Entry) * num_keys())
        + (sizeof(Block) * num_blocks())
        + (sizeof(Node) * num_nodes())
        + total_key_length();
  }
  UInt32 total_key_length() const {
    return header_->total_key_length();
  }
  UInt32 num_keys() const {
    return header_->num_keys();
  }
  UInt32 min_key_id() const {
    return header_->min_key_id();
  }
  UInt32 next_key_id() const {
    return header_->next_key_id();
  }
  UInt32 max_key_id() const {
    return header_->max_key_id();
  }
  UInt32 max_num_keys() const {
    return header_->max_num_keys();
  }
  UInt32 num_nodes() const {
    return header_->num_nodes();
  }
  UInt32 num_phantoms() const {
    return header_->num_phantoms();
  }
  UInt32 num_zombies() const {
    return header_->num_zombies();
  }
  UInt32 max_num_nodes() const {
    return header_->max_num_nodes();
  }
  UInt32 num_blocks() const {
    return header_->num_blocks();
  }
  UInt32 max_num_blocks() const {
    return header_->max_num_blocks();
  }
  UInt32 next_key_pos() const {
    return header_->next_key_pos();
  }
  UInt32 key_buf_size() const {
    return header_->key_buf_size();
  }
  UInt32 status_flags() const {
    return header_->status_flags();
  }

  void clear_status_flags() {
    header_->set_status_flags(status_flags() & ~CHANGING_MASK);
  }

  void flush();

 private:
  File file_;
  Header *header_;
  Array<Node> nodes_;
  Array<Block> blocks_;
  Array<Entry> entries_;
  Array<UInt32> key_buf_;

  void create_file(const char *file_name,
                   UInt64 file_size,
                   UInt32 max_num_keys,
                   double num_nodes_per_key,
                   double average_key_length);
  void create_file(const char *file_name,
                   UInt64 file_size,
                   UInt32 max_num_keys,
                   UInt32 max_num_blocks,
                   UInt32 key_buf_size);

  void open_file(const char *file_name);

  void map_address(void *address);

  void build_from_trie(const Trie &trie);
  void build_from_trie(const Trie &trie, UInt32 src, UInt32 dest);

  void repair_trie(const Trie &trie);
  void build_from_keys(const UInt32 *begin, const UInt32 *end,
                       UInt32 depth, UInt32 node_id);

  void mkq_sort(UInt32 *l, UInt32 *r, UInt32 depth);
  void insertion_sort(UInt32 *l, UInt32 *r, UInt32 depth);

  inline int get_label(UInt32 key_id, UInt32 depth) const;
  inline int get_median(UInt32 a, UInt32 b, UInt32 c, UInt32 depth) const;
  inline bool less_than(UInt32 lhs, UInt32 rhs, UInt32 depth) const;
  inline static void swap_ids(UInt32 *lhs, UInt32 *rhs);

  bool search_key(const UInt8 *ptr, UInt32 length, UInt32 *key_pos) const;
  bool search_linker(const UInt8 *ptr, UInt32 length,
                     UInt32 &node_id, UInt32 &query_pos) const;

  bool lcp_search_key(const UInt8 *ptr, UInt32 length, UInt32 *key_pos) const;

  bool remove_key(const UInt8 *ptr, UInt32 length);

  bool insert_key(const UInt8 *ptr, UInt32 length, UInt32 *key_pos);
  bool insert_linker(const UInt8 *ptr, UInt32 length,
                     UInt32 &node_id, UInt32 query_pos);

  bool update_key(const Key &key, const UInt8 *ptr, UInt32 length,
                  UInt32 *key_pos);

  UInt32 insert_node(UInt32 node_id, UInt16 label);
  UInt32 append_key(const UInt8 *ptr, UInt32 length, UInt32 key_id);

  UInt32 separate(const UInt8 *ptr, UInt32 length,
                  UInt32 node_id, UInt32 i);
  void resolve(UInt32 node_id, UInt16 label);
  void migrate_nodes(UInt32 node_id, UInt32 dest_offset,
                     const UInt16 *labels, UInt32 num_labels);

  UInt32 find_offset(const UInt16 *labels, UInt32 num_labels);

  void reserve_node(UInt32 node_id);
  void reserve_block(UInt32 block_id);

  void update_block_level(UInt32 block_id, UInt32 level);
  void set_block_level(UInt32 block_id, UInt32 level);
  void unset_block_level(UInt32 block_id);

  Node &ith_node(UInt32 i) {
    GRN_DAT_DEBUG_THROW_IF(i >= num_nodes());
    return nodes_[i];
  }
  Block &ith_block(UInt32 i) {
    GRN_DAT_DEBUG_THROW_IF(i >= num_blocks());
    return blocks_[i];
  }
  Entry &ith_entry(UInt32 i) {
    GRN_DAT_DEBUG_THROW_IF(i < min_key_id());
    GRN_DAT_DEBUG_THROW_IF(i > (max_key_id() + 1));
    return entries_[i];
  }

  // Disallows copy and assignment.
  Trie(const Trie &);
  Trie &operator=(const Trie &);
};

}  // namespace dat
}  // namespace grn
