/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2011-2015 Brazil

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

#include "trie.hpp"

#include <algorithm>
#include <cstring>

#include "vector.hpp"

namespace grn {
namespace dat {
namespace {

class StatusFlagManager {
 public:
  StatusFlagManager(Header *header, UInt32 status_flag)
      : header_(header), status_flag_(status_flag) {
    header_->set_status_flags(header_->status_flags() | status_flag_);
  }
  ~StatusFlagManager() {
    header_->set_status_flags(header_->status_flags() & ~status_flag_);
  }

 private:
  Header *header_;
  UInt32 status_flag_;

  // Disallows copy and assignment.
  StatusFlagManager(const StatusFlagManager &);
  StatusFlagManager &operator=(const StatusFlagManager &);
};

}  // namespace

Trie::Trie()
    : file_(),
      header_(NULL),
      nodes_(),
      blocks_(),
      entries_(),
      key_buf_() {}

Trie::~Trie() {}

void Trie::create(const char *file_name,
                  UInt64 file_size,
                  UInt32 max_num_keys,
                  double num_nodes_per_key,
                  double average_key_length) {
  GRN_DAT_THROW_IF(PARAM_ERROR, (file_size != 0) && (max_num_keys != 0));

  if (num_nodes_per_key < 1.0) {
    num_nodes_per_key = DEFAULT_NUM_NODES_PER_KEY;
  }
  if (num_nodes_per_key > MAX_NUM_NODES_PER_KEY) {
    num_nodes_per_key = MAX_NUM_NODES_PER_KEY;
  }
  GRN_DAT_THROW_IF(PARAM_ERROR, num_nodes_per_key < 1.0);
  GRN_DAT_THROW_IF(PARAM_ERROR, num_nodes_per_key > MAX_NUM_NODES_PER_KEY);

  if (average_key_length < 1.0) {
    average_key_length = DEFAULT_AVERAGE_KEY_LENGTH;
  }
  GRN_DAT_THROW_IF(PARAM_ERROR, average_key_length < 1.0);
  GRN_DAT_THROW_IF(PARAM_ERROR, average_key_length > MAX_KEY_LENGTH);

  if (max_num_keys == 0) {
    if (file_size == 0) {
      file_size = DEFAULT_FILE_SIZE;
    } else {
      GRN_DAT_THROW_IF(PARAM_ERROR, file_size < MIN_FILE_SIZE);
      GRN_DAT_THROW_IF(PARAM_ERROR, file_size > MAX_FILE_SIZE);
    }
  } else {
    GRN_DAT_THROW_IF(PARAM_ERROR, max_num_keys > MAX_NUM_KEYS);
  }

  Trie new_trie;
  new_trie.create_file(file_name, file_size, max_num_keys,
                       num_nodes_per_key, average_key_length);
  new_trie.swap(this);
}

void Trie::create(const Trie &trie,
                  const char *file_name,
                  UInt64 file_size,
                  UInt32 max_num_keys,
                  double num_nodes_per_key,
                  double average_key_length) {
  GRN_DAT_THROW_IF(PARAM_ERROR, (file_size != 0) && (max_num_keys != 0));

  if (num_nodes_per_key < 1.0) {
    if (trie.num_keys() == 0) {
      num_nodes_per_key = DEFAULT_NUM_NODES_PER_KEY;
    } else {
      num_nodes_per_key = 1.0 * trie.num_nodes() / trie.num_keys();
      if (num_nodes_per_key > MAX_NUM_NODES_PER_KEY) {
        num_nodes_per_key = MAX_NUM_NODES_PER_KEY;
      }
    }
  }
  GRN_DAT_THROW_IF(PARAM_ERROR, num_nodes_per_key < 1.0);
  GRN_DAT_THROW_IF(PARAM_ERROR, num_nodes_per_key > MAX_NUM_NODES_PER_KEY);

  if (average_key_length < 1.0) {
    if (trie.num_keys() == 0) {
      average_key_length = DEFAULT_AVERAGE_KEY_LENGTH;
    } else {
      average_key_length = 1.0 * trie.total_key_length() / trie.num_keys();
    }
  }
  GRN_DAT_THROW_IF(PARAM_ERROR, average_key_length < 1.0);
  GRN_DAT_THROW_IF(PARAM_ERROR, average_key_length > MAX_KEY_LENGTH);

  if (max_num_keys == 0) {
    if (file_size == 0) {
      file_size = trie.file_size();
    }
    GRN_DAT_THROW_IF(PARAM_ERROR, file_size < MIN_FILE_SIZE);
    GRN_DAT_THROW_IF(PARAM_ERROR, file_size > MAX_FILE_SIZE);
    GRN_DAT_THROW_IF(PARAM_ERROR, file_size < trie.virtual_size());
  } else {
    GRN_DAT_THROW_IF(PARAM_ERROR, max_num_keys < trie.num_keys());
    GRN_DAT_THROW_IF(PARAM_ERROR, max_num_keys < trie.max_key_id());
    GRN_DAT_THROW_IF(PARAM_ERROR, max_num_keys > MAX_NUM_KEYS);
  }

  Trie new_trie;
  new_trie.create_file(file_name, file_size, max_num_keys,
                       num_nodes_per_key, average_key_length);
  new_trie.build_from_trie(trie);
  new_trie.swap(this);
}

void Trie::repair(const Trie &trie, const char *file_name) {
  Trie new_trie;
  new_trie.create_file(file_name, trie.file_size(), trie.max_num_keys(),
                       trie.max_num_blocks(), trie.key_buf_size());
  new_trie.repair_trie(trie);
  new_trie.swap(this);
}

void Trie::open(const char *file_name) {
  GRN_DAT_THROW_IF(PARAM_ERROR, file_name == NULL);

  Trie new_trie;
  new_trie.open_file(file_name);
  new_trie.swap(this);
}

void Trie::close() {
  Trie().swap(this);
}

void Trie::swap(Trie *trie) {
  file_.swap(&trie->file_);
  std::swap(header_, trie->header_);
  nodes_.swap(&trie->nodes_);
  blocks_.swap(&trie->blocks_);
  entries_.swap(&trie->entries_);
  key_buf_.swap(&trie->key_buf_);
}

void Trie::flush() {
  file_.flush();
}

void Trie::create_file(const char *file_name,
                       UInt64 file_size,
                       UInt32 max_num_keys,
                       double num_nodes_per_key,
                       double average_key_length) {
  GRN_DAT_THROW_IF(PARAM_ERROR, (file_size == 0) && (max_num_keys == 0));
  GRN_DAT_THROW_IF(PARAM_ERROR, (file_size != 0) && (max_num_keys != 0));
  if (max_num_keys == 0) {
    const UInt64 avail = file_size - sizeof(Header);
    const double num_bytes_per_key = (sizeof(Node) * num_nodes_per_key)
        + (1.0 * sizeof(Block) / BLOCK_SIZE * num_nodes_per_key)
        + sizeof(Entry)
        + sizeof(UInt32) + sizeof(UInt8) + average_key_length + 1.5;
    if ((avail / num_bytes_per_key) > MAX_NUM_KEYS) {
      max_num_keys = MAX_NUM_KEYS;
    } else {
      max_num_keys = (UInt32)(avail / num_bytes_per_key);
    }
    GRN_DAT_THROW_IF(PARAM_ERROR, max_num_keys == 0);
  }

  UInt32 max_num_blocks;
  {
    const double max_num_nodes = num_nodes_per_key * max_num_keys;
    GRN_DAT_THROW_IF(PARAM_ERROR, (max_num_nodes - 1.0) >= MAX_NUM_NODES);
    max_num_blocks = ((UInt32)max_num_nodes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    GRN_DAT_THROW_IF(PARAM_ERROR, max_num_blocks == 0);
    GRN_DAT_THROW_IF(PARAM_ERROR, max_num_blocks > MAX_NUM_BLOCKS);
  }

  UInt32 key_buf_size;
  if (file_size == 0) {
    const double total_key_length = average_key_length * max_num_keys;
    GRN_DAT_THROW_IF(PARAM_ERROR,
        (total_key_length - 1.0) >= MAX_TOTAL_KEY_LENGTH);

    // The last term is the estimated number of bytes that will be used for
    // 32-bit alignment.
    const UInt64 total_num_bytes = (UInt64)(total_key_length)
        + (UInt64)(sizeof(UInt32) + sizeof(UInt8)) * max_num_keys
        + (UInt32)(max_num_keys * 1.5);
    GRN_DAT_THROW_IF(PARAM_ERROR,
        (total_num_bytes / sizeof(UInt32)) >= MAX_KEY_BUF_SIZE);
    key_buf_size = (UInt32)(total_num_bytes / sizeof(UInt32));

    file_size = sizeof(Header)
        + (sizeof(Block) * max_num_blocks)
        + (sizeof(Node) * BLOCK_SIZE * max_num_blocks)
        + (sizeof(Entry) * max_num_keys)
        + (sizeof(UInt32) * key_buf_size);
  } else {
    const UInt64 avail = file_size - sizeof(Header)
        - (sizeof(Block) * max_num_blocks)
        - (sizeof(Node) * BLOCK_SIZE * max_num_blocks)
        - (sizeof(Entry) * max_num_keys);
    GRN_DAT_THROW_IF(PARAM_ERROR, (avail / sizeof(UInt32)) > MAX_KEY_BUF_SIZE);
    key_buf_size = (UInt32)(avail / sizeof(UInt32));
  }

  create_file(file_name, file_size, max_num_keys,
              max_num_blocks, key_buf_size);
}

void Trie::create_file(const char *file_name,
                       UInt64 file_size,
                       UInt32 max_num_keys,
                       UInt32 max_num_blocks,
                       UInt32 key_buf_size) {
  GRN_DAT_THROW_IF(PARAM_ERROR, file_size < (sizeof(Header)
      + (sizeof(Block) * max_num_blocks)
      + (sizeof(Node) * BLOCK_SIZE * max_num_blocks)
      + (sizeof(Entry) * max_num_keys)
      + (sizeof(UInt32) * key_buf_size)));

  file_.create(file_name, file_size);

  Header * const header = static_cast<Header *>(file_.ptr());
  *header = Header();
  header->set_file_size(file_size);
  header->set_max_num_keys(max_num_keys);
  header->set_max_num_blocks(max_num_blocks);
  header->set_key_buf_size(key_buf_size);

  map_address(file_.ptr());

  reserve_node(ROOT_NODE_ID);
  ith_node(INVALID_OFFSET).set_is_offset(true);
}

void Trie::open_file(const char *file_name) {
  GRN_DAT_THROW_IF(PARAM_ERROR, file_name == NULL);

  file_.open(file_name);
  map_address(file_.ptr());
  GRN_DAT_THROW_IF(FORMAT_ERROR, file_size() != file_.size());
}

void Trie::map_address(void *address) {
  GRN_DAT_THROW_IF(PARAM_ERROR, address == NULL);

  header_ = static_cast<Header *>(address);
  nodes_.assign(header_ + 1, max_num_nodes());
  blocks_.assign(nodes_.end(), max_num_blocks());
  entries_.assign(reinterpret_cast<Entry *>(blocks_.end()) - 1,
      max_num_keys() + 1);
  key_buf_.assign(entries_.end(), key_buf_size());

  GRN_DAT_THROW_IF(UNEXPECTED_ERROR, static_cast<void *>(key_buf_.end())
      > static_cast<void *>(static_cast<char *>(address) + file_size()));
}

void Trie::build_from_trie(const Trie &trie) {
  GRN_DAT_THROW_IF(SIZE_ERROR, max_num_keys() < trie.num_keys());
  GRN_DAT_THROW_IF(SIZE_ERROR, max_num_keys() < trie.max_key_id());

  header_->set_total_key_length(trie.total_key_length());
  header_->set_num_keys(trie.num_keys());
  header_->set_max_key_id(trie.max_key_id());
  header_->set_next_key_id(trie.next_key_id());
  for (UInt32 i = min_key_id(); i <= max_key_id(); ++i) {
    ith_entry(i) = trie.ith_entry(i);
  }
  build_from_trie(trie, ROOT_NODE_ID, ROOT_NODE_ID);
}

void Trie::build_from_trie(const Trie &trie, UInt32 src, UInt32 dest) {
  // Keys are sorted in lexicographic order.
  if (trie.ith_node(src).is_linker()) {
    const Key &key = trie.get_key(trie.ith_node(src).key_pos());
    Key::create(key_buf_.ptr() + next_key_pos(),
        key.id(), key.ptr(), key.length());
    ith_node(dest).set_key_pos(next_key_pos());
    ith_entry(key.id()).set_key_pos(next_key_pos());
    header_->set_next_key_pos(
        next_key_pos() + Key::estimate_size(key.length()));
    return;
  }

  const UInt32 src_offset = trie.ith_node(src).offset();
  UInt32 dest_offset;
  {
    UInt16 labels[MAX_LABEL + 1];
    UInt32 num_labels = 0;

    UInt32 label = trie.ith_node(src).child();
    while (label != INVALID_LABEL) {
      GRN_DAT_DEBUG_THROW_IF(label > MAX_LABEL);
      const UInt32 child = src_offset ^ label;
      if (trie.ith_node(child).is_linker() ||
          (trie.ith_node(child).child() != INVALID_LABEL)) {
        labels[num_labels++] = label;
      }
      label = trie.ith_node(child).sibling();
    }
    if (num_labels == 0) {
      return;
    }

    dest_offset = find_offset(labels, num_labels);
    for (UInt32 i = 0; i < num_labels; ++i) {
      const UInt32 child = dest_offset ^ labels[i];
      reserve_node(child);
      ith_node(child).set_label(labels[i]);
      if ((i + 1) < num_labels) {
        ith_node(child).set_sibling(labels[i + 1]);
      }
    }

    GRN_DAT_DEBUG_THROW_IF(ith_node(dest_offset).is_offset());
    ith_node(dest_offset).set_is_offset(true);
    ith_node(dest).set_offset(dest_offset);
    ith_node(dest).set_child(labels[0]);
  }

  UInt32 label = ith_node(dest).child();
  while (label != INVALID_LABEL) {
    build_from_trie(trie, src_offset ^ label, dest_offset ^ label);
    label = ith_node(dest_offset ^ label).sibling();
  }
}

void Trie::repair_trie(const Trie &trie) {
  Vector<UInt32> valid_ids;
  header_->set_max_key_id(trie.max_key_id());
  header_->set_next_key_id(trie.max_key_id() + 1);
  UInt32 prev_invalid_key_id = INVALID_KEY_ID;
  for (UInt32 i = min_key_id(); i <= max_key_id(); ++i) {
    const Entry &entry = trie.ith_entry(i);
    if (entry.is_valid()) {
      valid_ids.push_back(i);
      ith_entry(i) = entry;
      const Key &key = trie.get_key(entry.key_pos());
      Key::create(key_buf_.ptr() + next_key_pos(),
          key.id(), key.ptr(), key.length());
      ith_entry(i).set_key_pos(next_key_pos());
      header_->set_next_key_pos(
          next_key_pos() + Key::estimate_size(key.length()));
      header_->set_total_key_length(
          total_key_length() + key.length());
      header_->set_num_keys(num_keys() + 1);
    } else {
      if (prev_invalid_key_id == INVALID_KEY_ID) {
        header_->set_next_key_id(i);
      } else {
        ith_entry(prev_invalid_key_id).set_next(i);
      }
      prev_invalid_key_id = i;
    }
  }
  if (prev_invalid_key_id != INVALID_KEY_ID) {
    ith_entry(prev_invalid_key_id).set_next(max_key_id() + 1);
  }
  mkq_sort(valid_ids.begin(), valid_ids.end(), 0);
  build_from_keys(valid_ids.begin(), valid_ids.end(), 0, ROOT_NODE_ID);
}

void Trie::build_from_keys(const UInt32 *begin, const UInt32 *end,
                           UInt32 depth, UInt32 node_id) {
  if ((end - begin) == 1) {
    ith_node(node_id).set_key_pos(ith_entry(*begin).key_pos());
    return;
  }

  UInt32 offset;
  {
    UInt16 labels[MAX_LABEL + 2];
    UInt32 num_labels = 0;

    const UInt32 *it = begin;
    if (ith_key(*it).length() == depth) {
      labels[num_labels++] = TERMINAL_LABEL;
      ++it;
    }

    labels[num_labels++] = (UInt8)ith_key(*it)[depth];
    for (++it; it < end; ++it) {
      const Key &key = ith_key(*it);
      if ((UInt8)key[depth] != labels[num_labels - 1]) {
        labels[num_labels++] = (UInt8)key[depth];
      }
    }
    labels[num_labels] = INVALID_LABEL;

    offset = find_offset(labels, num_labels);
    ith_node(node_id).set_child(labels[0]);
    for (UInt32 i = 0; i < num_labels; ++i) {
      const UInt32 next = offset ^ labels[i];
      reserve_node(next);
      ith_node(next).set_label(labels[i]);
      ith_node(next).set_sibling(labels[i + 1]);
    }

    if (offset >= num_nodes()) {
      reserve_block(num_blocks());
    }
    ith_node(offset).set_is_offset(true);
    ith_node(node_id).set_offset(offset);
  }

  if (ith_key(*begin).length() == depth) {
    build_from_keys(begin, begin + 1, depth + 1, offset ^ TERMINAL_LABEL);
    ++begin;
  }

  UInt16 label = ith_key(*begin)[depth];
  for (const UInt32 *it = begin + 1; it < end; ++it) {
    const Key &key = ith_key(*it);
    if ((UInt8)key[depth] != label) {
      build_from_keys(begin, it, depth + 1, offset ^ label);
      label = (UInt8)key[depth];
      begin = it;
    }
  }
  build_from_keys(begin, end, depth + 1, offset ^ label);
}

void Trie::mkq_sort(UInt32 *l, UInt32 *r, UInt32 depth) {
  while ((r - l) >= MKQ_SORT_THRESHOLD) {
    UInt32 *pl = l;
    UInt32 *pr = r;
    UInt32 *pivot_l = l;
    UInt32 *pivot_r = r;

    const int pivot = get_median(*l, *(l + (r - l) / 2), *(r - 1), depth);
    for ( ; ; ) {
      while (pl < pr) {
        const int label = get_label(*pl, depth);
        if (label > pivot) {
          break;
        } else if (label == pivot) {
          swap_ids(pl, pivot_l);
          ++pivot_l;
        }
        ++pl;
      }
      while (pl < pr) {
        const int label = get_label(*--pr, depth);
        if (label < pivot) {
          break;
        } else if (label == pivot) {
          swap_ids(pr, --pivot_r);
        }
      }
      if (pl >= pr) {
        break;
      }
      swap_ids(pl, pr);
      ++pl;
    }
    while (pivot_l > l) {
      swap_ids(--pivot_l, --pl);
    }
    while (pivot_r < r) {
      swap_ids(pivot_r, pr);
      ++pivot_r;
      ++pr;
    }

    if (((pl - l) > (pr - pl)) || ((r - pr) > (pr - pl))) {
      if ((pr - pl) > 1) {
        mkq_sort(pl, pr, depth + 1);
      }

      if ((pl - l) < (r - pr)) {
        if ((pl - l) > 1) {
          mkq_sort(l, pl, depth);
        }
        l = pr;
      } else {
        if ((r - pr) > 1) {
          mkq_sort(pr, r, depth);
        }
        r = pl;
      }
    } else {
      if ((pl - l) > 1) {
        mkq_sort(l, pl, depth);
      }

      if ((r - pr) > 1) {
        mkq_sort(pr, r, depth);
      }

      l = pl, r = pr;
      if ((pr - pl) > 1) {
        ++depth;
      }
    }
  }

  if ((r - l) > 1) {
    insertion_sort(l, r, depth);
  }
}

void Trie::insertion_sort(UInt32 *l, UInt32 *r, UInt32 depth) {
  for (UInt32 *i = l + 1; i < r; ++i) {
    for (UInt32 *j = i; j > l; --j) {
      if (less_than(*(j - 1), *j, depth)) {
        break;
      }
      swap_ids(j - 1, j);
    }
  }
}

int Trie::get_median(UInt32 a, UInt32 b, UInt32 c, UInt32 depth) const {
  const int x = get_label(a, depth);
  const int y = get_label(b, depth);
  const int z = get_label(c, depth);
  if (x < y) {
    if (y < z) {
      return y;
    } else if (x < z) {
      return z;
    }
    return x;
  } else if (x < z) {
    return x;
  } else if (y < z) {
    return z;
  }
  return y;
}

int Trie::get_label(UInt32 key_id, UInt32 depth) const {
  const Key &key = ith_key(key_id);
  if (depth == key.length()) {
    return -1;
  } else {
    return (UInt8)key[depth];
  }
}

bool Trie::less_than(UInt32 lhs, UInt32 rhs, UInt32 depth) const {
  const Key &lhs_key = ith_key(lhs);
  const Key &rhs_key = ith_key(rhs);
  const UInt32 length = (lhs_key.length() < rhs_key.length()) ?
      lhs_key.length() : rhs_key.length();
  for (UInt32 i = depth; i < length; ++i) {
    if (lhs_key[i] != rhs_key[i]) {
      return (UInt8)lhs_key[i] < (UInt8)rhs_key[i];
    }
  }
  return lhs_key.length() < rhs_key.length();
}

void Trie::swap_ids(UInt32 *lhs, UInt32 *rhs) {
  UInt32 temp = *lhs;
  *lhs = *rhs;
  *rhs = temp;
}

bool Trie::search_key(const UInt8 *ptr, UInt32 length, UInt32 *key_pos) const {
  GRN_DAT_DEBUG_THROW_IF((ptr == NULL) && (length != 0));

  UInt32 node_id = ROOT_NODE_ID;
  UInt32 query_pos = 0;
  if (!search_linker(ptr, length, node_id, query_pos)) {
    return false;
  }

  const Base base = ith_node(node_id).base();
  if (!base.is_linker()) {
    return false;
  }

  if (get_key(base.key_pos()).equals_to(ptr, length, query_pos)) {
    if (key_pos != NULL) {
      *key_pos = base.key_pos();
    }
    return true;
  }
  return false;
}

bool Trie::search_linker(const UInt8 *ptr, UInt32 length,
                         UInt32 &node_id, UInt32 &query_pos) const {
  GRN_DAT_DEBUG_THROW_IF((ptr == NULL) && (length != 0));

  for ( ; query_pos < length; ++query_pos) {
    const Base base = ith_node(node_id).base();
    if (base.is_linker()) {
      return true;
    }

    const UInt32 next = base.offset() ^ ptr[query_pos];
    if (ith_node(next).label() != ptr[query_pos]) {
      return false;
    }
    node_id = next;
  }

  const Base base = ith_node(node_id).base();
  if (base.is_linker()) {
    return true;
  }

  const UInt32 next = base.offset() ^ TERMINAL_LABEL;
  if (ith_node(next).label() != TERMINAL_LABEL) {
    return false;
  }
  node_id = next;
  return ith_node(next).is_linker();
}

bool Trie::lcp_search_key(const UInt8 *ptr, UInt32 length,
                          UInt32 *key_pos) const {
  GRN_DAT_DEBUG_THROW_IF((ptr == NULL) && (length != 0));

  bool found = false;
  UInt32 node_id = ROOT_NODE_ID;
  UInt32 query_pos = 0;

  for ( ; query_pos < length; ++query_pos) {
    const Base base = ith_node(node_id).base();
    if (base.is_linker()) {
      const Key &key = get_key(base.key_pos());
      if ((key.length() <= length) &&
          key.equals_to(ptr, key.length(), query_pos)) {
        if (key_pos != NULL) {
          *key_pos = base.key_pos();
        }
        found = true;
      }
      return found;
    }

    if (ith_node(node_id).child() == TERMINAL_LABEL) {
      const Base linker_base =
          ith_node(base.offset() ^ TERMINAL_LABEL).base();
      if (linker_base.is_linker()) {
        if (key_pos != NULL) {
          *key_pos = linker_base.key_pos();
        }
        found = true;
      }
    }

    node_id = base.offset() ^ ptr[query_pos];
    if (ith_node(node_id).label() != ptr[query_pos]) {
      return found;
    }
  }

  const Base base = ith_node(node_id).base();
  if (base.is_linker()) {
    const Key &key = get_key(base.key_pos());
    if (key.length() <= length) {
      if (key_pos != NULL) {
        *key_pos = base.key_pos();
      }
      found = true;
    }
  } else if (ith_node(node_id).child() == TERMINAL_LABEL) {
    const Base linker_base = ith_node(base.offset() ^ TERMINAL_LABEL).base();
    if (linker_base.is_linker()) {
      if (key_pos != NULL) {
        *key_pos = linker_base.key_pos();
      }
      found = true;
    }
  }
  return found;
}

bool Trie::remove_key(const UInt8 *ptr, UInt32 length) {
  GRN_DAT_THROW_IF(STATUS_ERROR, (status_flags() & CHANGING_MASK) != 0);
  StatusFlagManager status_flag_manager(header_, REMOVING_FLAG);

  GRN_DAT_DEBUG_THROW_IF((ptr == NULL) && (length != 0));

  UInt32 node_id = ROOT_NODE_ID;
  UInt32 query_pos = 0;
  if (!search_linker(ptr, length, node_id, query_pos)) {
    return false;
  }

  const UInt32 key_pos = ith_node(node_id).key_pos();
  if (!get_key(key_pos).equals_to(ptr, length, query_pos)) {
    return false;
  }

  const UInt32 key_id = get_key(key_pos).id();
  ith_node(node_id).set_offset(INVALID_OFFSET);
  ith_entry(key_id).set_next(next_key_id());

  header_->set_next_key_id(key_id);
  header_->set_total_key_length(total_key_length() - length);
  header_->set_num_keys(num_keys() - 1);
  return true;
}

bool Trie::insert_key(const UInt8 *ptr, UInt32 length, UInt32 *key_pos) {
  GRN_DAT_THROW_IF(STATUS_ERROR, (status_flags() & CHANGING_MASK) != 0);
  StatusFlagManager status_flag_manager(header_, INSERTING_FLAG);

  GRN_DAT_DEBUG_THROW_IF((ptr == NULL) && (length != 0));

  UInt32 node_id = ROOT_NODE_ID;
  UInt32 query_pos = 0;

  search_linker(ptr, length, node_id, query_pos);
  if (!insert_linker(ptr, length, node_id, query_pos)) {
    if (key_pos != NULL) {
      *key_pos = ith_node(node_id).key_pos();
    }
    return false;
  }

  const UInt32 new_key_id = next_key_id();
  const UInt32 new_key_pos = append_key(ptr, length, new_key_id);

  header_->set_total_key_length(total_key_length() + length);
  header_->set_num_keys(num_keys() + 1);
  if (new_key_id > max_key_id()) {
    header_->set_max_key_id(new_key_id);
    header_->set_next_key_id(new_key_id + 1);
  } else {
    header_->set_next_key_id(ith_entry(new_key_id).next());
  }

  ith_entry(new_key_id).set_key_pos(new_key_pos);
  ith_node(node_id).set_key_pos(new_key_pos);
  if (key_pos != NULL) {
    *key_pos = new_key_pos;
  }
  return true;
}

bool Trie::insert_linker(const UInt8 *ptr, UInt32 length,
                         UInt32 &node_id, UInt32 query_pos) {
  if (ith_node(node_id).is_linker()) {
    const Key &key = get_key(ith_node(node_id).key_pos());
    UInt32 i = query_pos;
    while ((i < length) && (i < key.length())) {
      if (ptr[i] != key[i]) {
        break;
      }
      ++i;
    }
    if ((i == length) && (i == key.length())) {
      return false;
    }
    GRN_DAT_THROW_IF(SIZE_ERROR, num_keys() >= max_num_keys());
    GRN_DAT_DEBUG_THROW_IF(next_key_id() > max_num_keys());

    for (UInt32 j = query_pos; j < i; ++j) {
      node_id = insert_node(node_id, ptr[j]);
    }
    node_id = separate(ptr, length, node_id, i);
    return true;
  } else if (ith_node(node_id).label() == TERMINAL_LABEL) {
    return true;
  } else {
    GRN_DAT_THROW_IF(SIZE_ERROR, num_keys() >= max_num_keys());
    const UInt16 label = (query_pos < length) ?
        (UInt16)ptr[query_pos] : (UInt16)TERMINAL_LABEL;
    const Base base = ith_node(node_id).base();
    if ((base.offset() == INVALID_OFFSET) ||
        !ith_node(base.offset() ^ label).is_phantom()) {
      resolve(node_id, label);
    }
    node_id = insert_node(node_id, label);
    return true;
  }
}

bool Trie::update_key(const Key &key, const UInt8 *ptr, UInt32 length,
                      UInt32 *key_pos) {
  GRN_DAT_THROW_IF(STATUS_ERROR, (status_flags() & CHANGING_MASK) != 0);
  StatusFlagManager status_flag_manager(header_, UPDATING_FLAG);

  GRN_DAT_DEBUG_THROW_IF((ptr == NULL) && (length != 0));

  if (!key.is_valid()) {
    return false;
  }

  UInt32 node_id = ROOT_NODE_ID;
  UInt32 query_pos = 0;

  search_linker(ptr, length, node_id, query_pos);
  if (!insert_linker(ptr, length, node_id, query_pos)) {
    if (key_pos != NULL) {
      *key_pos = ith_node(node_id).key_pos();
    }
    return false;
  }

  const UInt32 new_key_pos = append_key(ptr, length, key.id());
  header_->set_total_key_length(total_key_length() + length - key.length());
  ith_entry(key.id()).set_key_pos(new_key_pos);
  ith_node(node_id).set_key_pos(new_key_pos);
  if (key_pos != NULL) {
    *key_pos = new_key_pos;
  }

  node_id = ROOT_NODE_ID;
  query_pos = 0;
  GRN_DAT_THROW_IF(UNEXPECTED_ERROR,
      !search_linker(static_cast<const UInt8 *>(key.ptr()), key.length(),
                     node_id, query_pos));
  ith_node(node_id).set_offset(INVALID_OFFSET);
  return true;
}

UInt32 Trie::insert_node(UInt32 node_id, UInt16 label) {
  GRN_DAT_DEBUG_THROW_IF(node_id >= num_nodes());
  GRN_DAT_DEBUG_THROW_IF(label > MAX_LABEL);

  const Base base = ith_node(node_id).base();
  UInt32 offset;
  if (base.is_linker() || (base.offset() == INVALID_OFFSET)) {
    offset = find_offset(&label, 1);
  } else {
    offset = base.offset();
  }

  const UInt32 next = offset ^ label;
  reserve_node(next);

  ith_node(next).set_label(label);
  if (base.is_linker()) {
    GRN_DAT_DEBUG_THROW_IF(ith_node(offset).is_offset());
    ith_node(offset).set_is_offset(true);
    ith_node(next).set_key_pos(base.key_pos());
  } else if (base.offset() == INVALID_OFFSET) {
    GRN_DAT_DEBUG_THROW_IF(ith_node(offset).is_offset());
    ith_node(offset).set_is_offset(true);
  } else {
    GRN_DAT_DEBUG_THROW_IF(!ith_node(offset).is_offset());
  }
  ith_node(node_id).set_offset(offset);

  const UInt32 child_label = ith_node(node_id).child();
  GRN_DAT_DEBUG_THROW_IF(child_label == label);
  if (child_label == INVALID_LABEL) {
    ith_node(node_id).set_child(label);
  } else if ((label == TERMINAL_LABEL) ||
             ((child_label != TERMINAL_LABEL) && (label < child_label))) {
    GRN_DAT_DEBUG_THROW_IF(ith_node(offset ^ child_label).is_phantom());
    GRN_DAT_DEBUG_THROW_IF(ith_node(offset ^ child_label).label() != child_label);
    ith_node(next).set_sibling(child_label);
    ith_node(node_id).set_child(label);
  } else {
    UInt32 prev = offset ^ child_label;
    GRN_DAT_DEBUG_THROW_IF(ith_node(prev).label() != child_label);
    UInt32 sibling_label = ith_node(prev).sibling();
    while (label > sibling_label) {
      prev = offset ^ sibling_label;
      GRN_DAT_DEBUG_THROW_IF(ith_node(prev).label() != sibling_label);
      sibling_label = ith_node(prev).sibling();
    }
    GRN_DAT_DEBUG_THROW_IF(label == sibling_label);
    ith_node(next).set_sibling(ith_node(prev).sibling());
    ith_node(prev).set_sibling(label);
  }
  return next;
}

UInt32 Trie::append_key(const UInt8 *ptr, UInt32 length, UInt32 key_id) {
  GRN_DAT_THROW_IF(SIZE_ERROR, key_id > max_num_keys());

  const UInt32 key_pos = next_key_pos();
  const UInt32 key_size = Key::estimate_size(length);

  GRN_DAT_THROW_IF(SIZE_ERROR, key_size > (key_buf_size() - key_pos));
  Key::create(key_buf_.ptr() + key_pos, key_id, ptr, length);

  header_->set_next_key_pos(key_pos + key_size);
  return key_pos;
}

UInt32 Trie::separate(const UInt8 *ptr, UInt32 length,
                      UInt32 node_id, UInt32 i) {
  GRN_DAT_DEBUG_THROW_IF(node_id >= num_nodes());
  GRN_DAT_DEBUG_THROW_IF(!ith_node(node_id).is_linker());
  GRN_DAT_DEBUG_THROW_IF(i > length);

  const UInt32 key_pos = ith_node(node_id).key_pos();
  const Key &key = get_key(key_pos);

  UInt16 labels[2];
  labels[0] = (i < key.length()) ? (UInt16)key[i] : (UInt16)TERMINAL_LABEL;
  labels[1] = (i < length) ? (UInt16)ptr[i] : (UInt16)TERMINAL_LABEL;
  GRN_DAT_DEBUG_THROW_IF(labels[0] == labels[1]);

  const UInt32 offset = find_offset(labels, 2);

  UInt32 next = offset ^ labels[0];
  reserve_node(next);
  GRN_DAT_DEBUG_THROW_IF(ith_node(offset).is_offset());

  ith_node(next).set_label(labels[0]);
  ith_node(next).set_key_pos(key_pos);

  next = offset ^ labels[1];
  reserve_node(next);

  ith_node(next).set_label(labels[1]);

  ith_node(offset).set_is_offset(true);
  ith_node(node_id).set_offset(offset);

  if ((labels[0] == TERMINAL_LABEL) ||
      ((labels[1] != TERMINAL_LABEL) && (labels[0] < labels[1]))) {
    ith_node(node_id).set_child(labels[0]);
    ith_node(offset ^ labels[0]).set_sibling(labels[1]);
  } else {
    ith_node(node_id).set_child(labels[1]);
    ith_node(offset ^ labels[1]).set_sibling(labels[0]);
  }
  return next;
}

void Trie::resolve(UInt32 node_id, UInt16 label) {
  GRN_DAT_DEBUG_THROW_IF(node_id >= num_nodes());
  GRN_DAT_DEBUG_THROW_IF(ith_node(node_id).is_linker());
  GRN_DAT_DEBUG_THROW_IF(label > MAX_LABEL);

  UInt32 offset = ith_node(node_id).offset();
  if (offset != INVALID_OFFSET) {
    UInt16 labels[MAX_LABEL + 1];
    UInt32 num_labels = 0;

    UInt32 next_label = ith_node(node_id).child();
    GRN_DAT_DEBUG_THROW_IF(next_label == INVALID_LABEL);
    while (next_label != INVALID_LABEL) {
      GRN_DAT_DEBUG_THROW_IF(next_label > MAX_LABEL);
      labels[num_labels++] = next_label;
      next_label = ith_node(offset ^ next_label).sibling();
    }
    GRN_DAT_DEBUG_THROW_IF(num_labels == 0);

    labels[num_labels] = label;
    offset = find_offset(labels, num_labels + 1);
    migrate_nodes(node_id, offset, labels, num_labels);
  } else {
    offset = find_offset(&label, 1);
    if (offset >= num_nodes()) {
      GRN_DAT_DEBUG_THROW_IF((offset / BLOCK_SIZE) != num_blocks());
      reserve_block(num_blocks());
    }
    ith_node(offset).set_is_offset(true);
    ith_node(node_id).set_offset(offset);
  }
}

void Trie::migrate_nodes(UInt32 node_id, UInt32 dest_offset,
                         const UInt16 *labels, UInt32 num_labels) {
  GRN_DAT_DEBUG_THROW_IF(node_id >= num_nodes());
  GRN_DAT_DEBUG_THROW_IF(ith_node(node_id).is_linker());
  GRN_DAT_DEBUG_THROW_IF(labels == NULL);
  GRN_DAT_DEBUG_THROW_IF(num_labels == 0);
  GRN_DAT_DEBUG_THROW_IF(num_labels > (MAX_LABEL + 1));

  const UInt32 src_offset = ith_node(node_id).offset();
  GRN_DAT_DEBUG_THROW_IF(src_offset == INVALID_OFFSET);
  GRN_DAT_DEBUG_THROW_IF(!ith_node(src_offset).is_offset());

  for (UInt32 i = 0; i < num_labels; ++i) {
    const UInt32 src_node_id = src_offset ^ labels[i];
    const UInt32 dest_node_id = dest_offset ^ labels[i];
    GRN_DAT_DEBUG_THROW_IF(ith_node(src_node_id).is_phantom());
    GRN_DAT_DEBUG_THROW_IF(ith_node(src_node_id).label() != labels[i]);

    reserve_node(dest_node_id);
    ith_node(dest_node_id).set_except_is_offset(
        ith_node(src_node_id).except_is_offset());
    ith_node(dest_node_id).set_base(ith_node(src_node_id).base());
  }
  header_->set_num_zombies(num_zombies() + num_labels);

  GRN_DAT_DEBUG_THROW_IF(ith_node(dest_offset).is_offset());
  ith_node(dest_offset).set_is_offset(true);
  ith_node(node_id).set_offset(dest_offset);
}

UInt32 Trie::find_offset(const UInt16 *labels, UInt32 num_labels) {
  GRN_DAT_DEBUG_THROW_IF(labels == NULL);
  GRN_DAT_DEBUG_THROW_IF(num_labels == 0);
  GRN_DAT_DEBUG_THROW_IF(num_labels > (MAX_LABEL + 1));

  // Blocks are tested in descending order of level. Basically, a lower level
  // block contains more phantom nodes.
  UInt32 level = 1;
  while (num_labels >= (1U << level)) {
    ++level;
  }
  level = (level < MAX_BLOCK_LEVEL) ? (MAX_BLOCK_LEVEL - level) : 0;

  UInt32 block_count = 0;
  do {
    UInt32 leader = header_->ith_leader(level);
    if (leader == INVALID_LEADER) {
      // This level has no blocks and it is thus skipped.
      continue;
    }

    UInt32 block_id = leader;
    do {
      const Block &block = ith_block(block_id);
      GRN_DAT_DEBUG_THROW_IF(block.level() != level);

      const UInt32 first = (block_id * BLOCK_SIZE) | block.first_phantom();
      UInt32 node_id = first;
      do {
        GRN_DAT_DEBUG_THROW_IF(!ith_node(node_id).is_phantom());
        const UInt32 offset = node_id ^ labels[0];
        if (!ith_node(offset).is_offset()) {
          UInt32 i = 1;
          for ( ; i < num_labels; ++i) {
            if (!ith_node(offset ^ labels[i]).is_phantom()) {
              break;
            }
          }
          if (i >= num_labels) {
            return offset;
          }
        }
        node_id = (block_id * BLOCK_SIZE) | ith_node(node_id).next();
      } while (node_id != first);

      const UInt32 prev = block_id;
      const UInt32 next = block.next();
      block_id = next;
      ith_block(prev).set_failure_count(ith_block(prev).failure_count() + 1);

      // The level of a block is updated when this function fails many times,
      // actually MAX_FAILURE_COUNT times, in that block.
      if (ith_block(prev).failure_count() == MAX_FAILURE_COUNT) {
        update_block_level(prev, level + 1);
        if (next == leader) {
          break;
        } else {
          // Note that the leader might be updated in the level update.
          leader = header_->ith_leader(level);
          continue;
        }
      }
    } while ((++block_count < MAX_BLOCK_COUNT) && (block_id != leader));
  } while ((block_count < MAX_BLOCK_COUNT) && (level-- != 0));

  return num_nodes() ^ labels[0];
}

void Trie::reserve_node(UInt32 node_id) {
  GRN_DAT_DEBUG_THROW_IF(node_id > num_nodes());
  if (node_id >= num_nodes()) {
    reserve_block(node_id / BLOCK_SIZE);
  }

  Node &node = ith_node(node_id);
  GRN_DAT_DEBUG_THROW_IF(!node.is_phantom());

  const UInt32 block_id = node_id / BLOCK_SIZE;
  Block &block = ith_block(block_id);
  GRN_DAT_DEBUG_THROW_IF(block.num_phantoms() == 0);

  const UInt32 next = (block_id * BLOCK_SIZE) | node.next();
  const UInt32 prev = (block_id * BLOCK_SIZE) | node.prev();
  GRN_DAT_DEBUG_THROW_IF(next >= num_nodes());
  GRN_DAT_DEBUG_THROW_IF(prev >= num_nodes());

  if ((node_id & BLOCK_MASK) == block.first_phantom()) {
    // The first phantom node is removed from the block and the second phantom
    // node comes first.
    block.set_first_phantom(next & BLOCK_MASK);
  }

  ith_node(next).set_prev(prev & BLOCK_MASK);
  ith_node(prev).set_next(next & BLOCK_MASK);

  if (block.level() != MAX_BLOCK_LEVEL) {
    const UInt32 threshold = 1U << ((MAX_BLOCK_LEVEL - block.level() - 1) * 2);
    if (block.num_phantoms() == threshold) {
      update_block_level(block_id, block.level() + 1);
    }
  }
  block.set_num_phantoms(block.num_phantoms() - 1);

  node.set_is_phantom(false);

  GRN_DAT_DEBUG_THROW_IF(node.offset() != INVALID_OFFSET);
  GRN_DAT_DEBUG_THROW_IF(node.label() != INVALID_LABEL);

  header_->set_num_phantoms(num_phantoms() - 1);
}

void Trie::reserve_block(UInt32 block_id) {
  GRN_DAT_DEBUG_THROW_IF(block_id != num_blocks());
  GRN_DAT_THROW_IF(SIZE_ERROR, block_id >= max_num_blocks());

  header_->set_num_blocks(block_id + 1);
  ith_block(block_id).set_failure_count(0);
  ith_block(block_id).set_first_phantom(0);
  ith_block(block_id).set_num_phantoms(BLOCK_SIZE);

  const UInt32 begin = block_id * BLOCK_SIZE;
  const UInt32 end = begin + BLOCK_SIZE;
  GRN_DAT_DEBUG_THROW_IF(end != num_nodes());

  Base base;
  base.set_offset(INVALID_OFFSET);

  Check check;
  check.set_is_phantom(true);

  for (UInt32 i = begin; i < end; ++i) {
    check.set_prev((i - 1) & BLOCK_MASK);
    check.set_next((i + 1) & BLOCK_MASK);
    ith_node(i).set_base(base);
    ith_node(i).set_check(check);
  }

  // The level of the new block is 0.
  set_block_level(block_id, 0);
  header_->set_num_phantoms(num_phantoms() + BLOCK_SIZE);
}

void Trie::update_block_level(UInt32 block_id, UInt32 level) {
  GRN_DAT_DEBUG_THROW_IF(block_id >= num_blocks());
  GRN_DAT_DEBUG_THROW_IF(level > MAX_BLOCK_LEVEL);

  unset_block_level(block_id);
  set_block_level(block_id, level);
}

void Trie::set_block_level(UInt32 block_id, UInt32 level) {
  GRN_DAT_DEBUG_THROW_IF(block_id >= num_blocks());
  GRN_DAT_DEBUG_THROW_IF(level > MAX_BLOCK_LEVEL);

  const UInt32 leader = header_->ith_leader(level);
  if (leader == INVALID_LEADER) {
    // The new block becomes the only one block of the linked list.
    ith_block(block_id).set_next(block_id);
    ith_block(block_id).set_prev(block_id);
    header_->set_ith_leader(level, block_id);
  } else {
    // The new block is added to the end of the list.
    const UInt32 next = leader;
    const UInt32 prev = ith_block(leader).prev();
    GRN_DAT_DEBUG_THROW_IF(next >= num_blocks());
    GRN_DAT_DEBUG_THROW_IF(prev >= num_blocks());
    ith_block(block_id).set_next(next);
    ith_block(block_id).set_prev(prev);
    ith_block(next).set_prev(block_id);
    ith_block(prev).set_next(block_id);
  }
  ith_block(block_id).set_level(level);
  ith_block(block_id).set_failure_count(0);
}

void Trie::unset_block_level(UInt32 block_id) {
  GRN_DAT_DEBUG_THROW_IF(block_id >= num_blocks());

  const UInt32 level = ith_block(block_id).level();
  GRN_DAT_DEBUG_THROW_IF(level > MAX_BLOCK_LEVEL);

  const UInt32 leader = header_->ith_leader(level);
  GRN_DAT_DEBUG_THROW_IF(leader == INVALID_LEADER);

  const UInt32 next = ith_block(block_id).next();
  const UInt32 prev = ith_block(block_id).prev();
  GRN_DAT_DEBUG_THROW_IF(next >= num_blocks());
  GRN_DAT_DEBUG_THROW_IF(prev >= num_blocks());

  if (next == block_id) {
    // The linked list becomes empty.
    header_->set_ith_leader(level, INVALID_LEADER);
  } else {
    ith_block(next).set_prev(prev);
    ith_block(prev).set_next(next);
    if (block_id == leader) {
      // The second block becomes the leader of the linked list.
      header_->set_ith_leader(level, next);
    }
  }
}

}  // namespace dat
}  // namespace grn
