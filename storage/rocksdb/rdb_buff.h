/*
   Portions Copyright (c) 2016-Present, Facebook, Inc.
   Portions Copyright (c) 2012,2013 Monty Program Ab

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
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#define htobe64 _byteswap_uint64
#define be64toh _byteswap_uint64
#define htobe32 _byteswap_ulong
#define be32toh _byteswap_ulong
#define htobe16 _byteswap_ushort
#define be16toh _byteswap_ushort
#endif

#if __APPLE__
#include <libkern/OSByteOrder.h>
#define htobe64(x) OSSwapHostToBigInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define htobe16(x) OSSwapHostToBigInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#endif

/* MySQL header files */
#include "./my_global.h"

/* MyRocks header files */
#include "./rdb_global.h"
#include "./rdb_utils.h"

/* RocksDB header files */
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace myrocks {

/*
  Basic composition functions for a network buffer presented as a MySQL String
  ("netstr") which stores data in Network Byte Order (Big Endian).
*/

inline void rdb_netstr_append_uint64(my_core::String *const out_netstr,
                                     const uint64 val) {
  DBUG_ASSERT(out_netstr != nullptr);

  // Convert from host machine byte order (usually Little Endian) to network
  // byte order (Big Endian).
  uint64 net_val = htobe64(val);
  out_netstr->append(reinterpret_cast<char *>(&net_val), sizeof(net_val));
}

inline void rdb_netstr_append_uint32(my_core::String *const out_netstr,
                                     const uint32 val) {
  DBUG_ASSERT(out_netstr != nullptr);

  // Convert from host machine byte order (usually Little Endian) to network
  // byte order (Big Endian).
  uint32 net_val = htobe32(val);
  out_netstr->append(reinterpret_cast<char *>(&net_val), sizeof(net_val));
}

inline void rdb_netstr_append_uint16(my_core::String *const out_netstr,
                                     const uint16 val) {
  DBUG_ASSERT(out_netstr != nullptr);

  // Convert from host machine byte order (usually Little Endian) to network
  // byte order (Big Endian).
  uint16 net_val = htobe16(val);
  out_netstr->append(reinterpret_cast<char *>(&net_val), sizeof(net_val));
}

/*
  Basic network buffer ("netbuf") write helper functions.
*/

inline void rdb_netbuf_store_uint64(uchar *const dst_netbuf, const uint64 n) {
  DBUG_ASSERT(dst_netbuf != nullptr);

  // Convert from host byte order (usually Little Endian) to network byte order
  // (Big Endian).
  uint64 net_val = htobe64(n);
  memcpy(dst_netbuf, &net_val, sizeof(net_val));
}

inline void rdb_netbuf_store_uint32(uchar *const dst_netbuf, const uint32 n) {
  DBUG_ASSERT(dst_netbuf != nullptr);

  // Convert from host byte order (usually Little Endian) to network byte order
  // (Big Endian).
  uint32 net_val = htobe32(n);
  memcpy(dst_netbuf, &net_val, sizeof(net_val));
}

inline void rdb_netbuf_store_uint16(uchar *const dst_netbuf, const uint16 n) {
  DBUG_ASSERT(dst_netbuf != nullptr);

  // Convert from host byte order (usually Little Endian) to network byte order
  // (Big Endian).
  uint16 net_val = htobe16(n);
  memcpy(dst_netbuf, &net_val, sizeof(net_val));
}

inline void rdb_netbuf_store_byte(uchar *const dst_netbuf, const uchar c) {
  DBUG_ASSERT(dst_netbuf != nullptr);

  *dst_netbuf = c;
}

inline void rdb_netbuf_store_index(uchar *const dst_netbuf,
                                   const uint32 number) {
  DBUG_ASSERT(dst_netbuf != nullptr);

  rdb_netbuf_store_uint32(dst_netbuf, number);
}

/*
  Basic conversion helper functions from network byte order (Big Endian) to host
  machine byte order (usually Little Endian).
*/

inline uint64 rdb_netbuf_to_uint64(const uchar *const netbuf) {
  DBUG_ASSERT(netbuf != nullptr);

  uint64 net_val;
  memcpy(&net_val, netbuf, sizeof(net_val));

  // Convert from network byte order (Big Endian) to host machine byte order
  // (usually Little Endian).
  return be64toh(net_val);
}

inline uint32 rdb_netbuf_to_uint32(const uchar *const netbuf) {
  DBUG_ASSERT(netbuf != nullptr);

  uint32 net_val;
  memcpy(&net_val, netbuf, sizeof(net_val));

  // Convert from network byte order (Big Endian) to host machine byte order
  // (usually Little Endian).
  return be32toh(net_val);
}

inline uint16 rdb_netbuf_to_uint16(const uchar *const netbuf) {
  DBUG_ASSERT(netbuf != nullptr);

  uint16 net_val;
  memcpy(&net_val, netbuf, sizeof(net_val));

  // Convert from network byte order (Big Endian) to host machine byte order
  // (usually Little Endian).
  return be16toh(net_val);
}

inline uchar rdb_netbuf_to_byte(const uchar *const netbuf) {
  DBUG_ASSERT(netbuf != nullptr);

  return (uchar)netbuf[0];
}

/*
  Basic network buffer ("netbuf") read helper functions.
  Network buffer stores data in Network Byte Order (Big Endian).
  NB: The netbuf is passed as an input/output param, hence after reading,
      the netbuf pointer gets advanced to the following byte.
*/

inline uint64 rdb_netbuf_read_uint64(const uchar **netbuf_ptr) {
  DBUG_ASSERT(netbuf_ptr != nullptr);

  // Convert from network byte order (Big Endian) to host machine byte order
  // (usually Little Endian).
  const uint64 host_val = rdb_netbuf_to_uint64(*netbuf_ptr);

  // Advance pointer.
  *netbuf_ptr += sizeof(host_val);

  return host_val;
}

inline uint32 rdb_netbuf_read_uint32(const uchar **netbuf_ptr) {
  DBUG_ASSERT(netbuf_ptr != nullptr);

  // Convert from network byte order (Big Endian) to host machine byte order
  // (usually Little Endian).
  const uint32 host_val = rdb_netbuf_to_uint32(*netbuf_ptr);

  // Advance pointer.
  *netbuf_ptr += sizeof(host_val);

  return host_val;
}

inline uint16 rdb_netbuf_read_uint16(const uchar **netbuf_ptr) {
  DBUG_ASSERT(netbuf_ptr != nullptr);

  // Convert from network byte order (Big Endian) to host machine byte order
  // (usually Little Endian).
  const uint16 host_val = rdb_netbuf_to_uint16(*netbuf_ptr);

  // Advance pointer.
  *netbuf_ptr += sizeof(host_val);

  return host_val;
}

inline void rdb_netbuf_read_gl_index(const uchar **netbuf_ptr,
                                     GL_INDEX_ID *const gl_index_id) {
  DBUG_ASSERT(gl_index_id != nullptr);
  DBUG_ASSERT(netbuf_ptr != nullptr);

  gl_index_id->cf_id = rdb_netbuf_read_uint32(netbuf_ptr);
  gl_index_id->index_id = rdb_netbuf_read_uint32(netbuf_ptr);
}

/*
  A simple string reader:
  - it keeps position within the string that we read from
  - it prevents one from reading beyond the end of the string.
*/

class Rdb_string_reader {
  const char *m_ptr;
  uint m_len;

 private:
  Rdb_string_reader &operator=(const Rdb_string_reader &) = default;

 public:
  Rdb_string_reader(const Rdb_string_reader &) = default;
  /* named constructor */
  static Rdb_string_reader read_or_empty(const rocksdb::Slice *const slice) {
    if (!slice) {
      return Rdb_string_reader("");
    } else {
      return Rdb_string_reader(slice);
    }
  }

  explicit Rdb_string_reader(const std::string &str) {
    m_len = str.length();
    if (m_len) {
      m_ptr = &str.at(0);
    } else {
      /*
        One can a create a Rdb_string_reader for reading from an empty string
        (although attempts to read anything will fail).
        We must not access str.at(0), since len==0, we can set ptr to any
        value.
      */
      m_ptr = nullptr;
    }
  }

  explicit Rdb_string_reader(const rocksdb::Slice *const slice) {
    m_ptr = slice->data();
    m_len = slice->size();
  }

  /*
    Read the next @param size bytes. Returns pointer to the bytes read, or
    nullptr if the remaining string doesn't have that many bytes.
  */
  const char *read(const uint size) {
    const char *res;
    if (m_len < size) {
      res = nullptr;
    } else {
      res = m_ptr;
      m_ptr += size;
      m_len -= size;
    }
    return res;
  }

  bool read_uint8(uint *const res) {
    const uchar *p;
    if (!(p = reinterpret_cast<const uchar *>(read(1)))) {
      return true;  // error
    } else {
      *res = *p;
      return false;  // Ok
    }
  }

  bool read_uint16(uint *const res) {
    const uchar *p;
    if (!(p = reinterpret_cast<const uchar *>(read(2)))) {
      return true;  // error
    } else {
      *res = rdb_netbuf_to_uint16(p);
      return false;  // Ok
    }
  }

  bool read_uint64(uint64 *const res) {
    const uchar *p;
    if (!(p = reinterpret_cast<const uchar *>(read(sizeof(uint64))))) {
      return true;  // error
    } else {
      *res = rdb_netbuf_to_uint64(p);
      return false;  // Ok
    }
  }

  uint remaining_bytes() const { return m_len; }

  /*
    Return pointer to data that will be read by next read() call (if there is
    nothing left to read, returns pointer to beyond the end of previous read()
    call)
  */
  const char *get_current_ptr() const { return m_ptr; }
};

/*
  @brief
  A buffer one can write the data to.

  @detail
  Suggested usage pattern:

    writer->clear();
    writer->write_XXX(...);
    ...
    // Ok, writer->ptr() points to the data written so far,
    // and writer->get_current_pos() is the length of the data

*/

class Rdb_string_writer {
  std::vector<uchar> m_data;

 public:
  Rdb_string_writer(const Rdb_string_writer &) = delete;
  Rdb_string_writer &operator=(const Rdb_string_writer &) = delete;
  Rdb_string_writer() = default;

  void clear() { m_data.clear(); }
  void write_uint8(const uint val) {
    m_data.push_back(static_cast<uchar>(val));
  }

  void write_uint16(const uint val) {
    const auto size = m_data.size();
    m_data.resize(size + 2);
    rdb_netbuf_store_uint16(m_data.data() + size, val);
  }

  void write_uint32(const uint val) {
    const auto size = m_data.size();
    m_data.resize(size + 4);
    rdb_netbuf_store_uint32(m_data.data() + size, val);
  }

  void write(const uchar *const new_data, const size_t len) {
    DBUG_ASSERT(new_data != nullptr);
    m_data.insert(m_data.end(), new_data, new_data + len);
  }

  uchar *ptr() { return m_data.data(); }
  size_t get_current_pos() const { return m_data.size(); }

  void write_uint8_at(const size_t pos, const uint new_val) {
    // This function will only overwrite what was written
    DBUG_ASSERT(pos < get_current_pos());
    m_data.data()[pos] = new_val;
  }

  void write_uint16_at(const size_t pos, const uint new_val) {
    // This function will only overwrite what was written
    DBUG_ASSERT(pos < get_current_pos() && (pos + 1) < get_current_pos());
    rdb_netbuf_store_uint16(m_data.data() + pos, new_val);
  }

  void truncate(const size_t pos) {
    DBUG_ASSERT(pos < m_data.size());
    m_data.resize(pos);
  }

  void allocate(const size_t len, const uchar val = 0) {
    DBUG_ASSERT(len > 0);
    m_data.resize(m_data.size() + len, val);
  }

  /*
    An awful hack to deallocate the buffer without relying on the deconstructor.
    This is needed to suppress valgrind errors in rocksdb.partition
  */
  void free() { std::vector<uchar>().swap(m_data); }
};

/*
   A helper class for writing bits into Rdb_string_writer.

   The class assumes (but doesn't check) that nobody tries to write
   anything to the Rdb_string_writer that it is writing to.
*/
class Rdb_bit_writer {
  Rdb_string_writer *m_writer;
  uchar m_offset;

 public:
  Rdb_bit_writer(const Rdb_bit_writer &) = delete;
  Rdb_bit_writer &operator=(const Rdb_bit_writer &) = delete;

  explicit Rdb_bit_writer(Rdb_string_writer *writer_arg)
      : m_writer(writer_arg), m_offset(0) {}

  void write(uint size, const uint value) {
    DBUG_ASSERT((value & ((1 << size) - 1)) == value);

    while (size > 0) {
      if (m_offset == 0) {
        m_writer->write_uint8(0);
      }
      // number of bits to put in this byte
      const uint bits = std::min(size, (uint)(8 - m_offset));
      uchar *const last_byte =
          m_writer->ptr() + m_writer->get_current_pos() - 1;
      *last_byte |= (uchar)((value >> (size - bits)) & ((1 << bits) - 1))
                    << m_offset;
      size -= bits;
      m_offset = (m_offset + bits) & 0x7;
    }
  }
};

class Rdb_bit_reader {
  const uchar *m_cur;
  uchar m_offset;
  uint m_ret;
  Rdb_string_reader *const m_reader;

 public:
  Rdb_bit_reader(const Rdb_bit_reader &) = delete;
  Rdb_bit_reader &operator=(const Rdb_bit_reader &) = delete;

  explicit Rdb_bit_reader(Rdb_string_reader *const reader)
      : m_cur(nullptr), m_offset(0), m_reader(reader) {}

  // Returns a pointer to an uint containing the bits read. On subsequent
  // reads, the value being pointed to will be overwritten.  Returns nullptr
  // on failure.
  uint *read(uint size) {
    m_ret = 0;
    DBUG_ASSERT(size <= 32);

    while (size > 0) {
      if (m_offset == 0) {
        m_cur = (const uchar *)m_reader->read(1);
        if (m_cur == nullptr) {
          return nullptr;
        }
      }
      // how many bits from the current byte?
      const uint bits = std::min((uint)(8 - m_offset), size);
      m_ret <<= bits;
      m_ret |= (*m_cur >> m_offset) & ((1 << bits) - 1);
      size -= bits;
      m_offset = (m_offset + bits) & 0x7;
    }

    return &m_ret;
  }
};

template <size_t buf_length>
class Rdb_buf_writer {
 public:
  Rdb_buf_writer(const Rdb_buf_writer &) = delete;
  Rdb_buf_writer &operator=(const Rdb_buf_writer &) = delete;
  Rdb_buf_writer() { reset(); }

  void write_uint32(const uint32 n) {
    DBUG_ASSERT(m_ptr + sizeof(n) <= m_buf.data() + buf_length);
    rdb_netbuf_store_uint32(m_ptr, n);
    m_ptr += sizeof(n);
  }

  void write_uint64(const uint64 n) {
    DBUG_ASSERT(m_ptr + sizeof(n) <= m_buf.data() + buf_length);
    rdb_netbuf_store_uint64(m_ptr, n);
    m_ptr += sizeof(n);
  }

  void write_uint16(const uint16 n) {
    DBUG_ASSERT(m_ptr + sizeof(n) <= m_buf.data() + buf_length);
    rdb_netbuf_store_uint16(m_ptr, n);
    m_ptr += sizeof(n);
  }

  void write_byte(const uchar c) {
    DBUG_ASSERT(m_ptr + sizeof(c) <= m_buf.data() + buf_length);
    rdb_netbuf_store_byte(m_ptr, c);
    m_ptr += sizeof(c);
  }

  void write_index(const uint32 n) { write_uint32(n); }

  void write(const char *buf, const size_t size) {
    DBUG_ASSERT(m_ptr + size <= m_buf.data() + buf_length);
    memcpy(m_ptr, buf, size);
    m_ptr += size;
  }

  void write(const uchar *buf, const size_t size) {
    DBUG_ASSERT(m_ptr + size <= m_buf.data() + buf_length);
    memcpy(m_ptr, buf, size);
    m_ptr += size;
  }

  void reset() { m_ptr = m_buf.data(); }

  const char *data() const {
    return reinterpret_cast<const char *>(m_buf.data());
  }

  size_t capacity() { return buf_length; }

  /** Returns actual size of the buffer that has data */
  size_t size() { return m_ptr - m_buf.data(); }

  rocksdb::Slice to_slice() { return rocksdb::Slice(data(), size()); }

 private:
  std::array<uchar, buf_length> m_buf;
  uchar *m_ptr;
};

}  // namespace myrocks
