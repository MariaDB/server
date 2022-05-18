/*****************************************************************************

Copyright (c) 2019, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**
@file include/mtr0log.h
Mini-transaction log record encoding and decoding
*******************************************************/

#pragma once
#include "mtr0mtr.h"

/** The smallest invalid page identifier for persistent tablespaces */
constexpr page_id_t end_page_id{SRV_SPACE_ID_UPPER_BOUND, 0};

/** The minimum 2-byte integer (0b10xxxxxx xxxxxxxx) */
constexpr uint32_t MIN_2BYTE= 1 << 7;
/** The minimum 3-byte integer (0b110xxxxx xxxxxxxx xxxxxxxx) */
constexpr uint32_t MIN_3BYTE= MIN_2BYTE + (1 << 14);
/** The minimum 4-byte integer (0b1110xxxx xxxxxxxx xxxxxxxx xxxxxxxx) */
constexpr uint32_t MIN_4BYTE= MIN_3BYTE + (1 << 21);
/** Minimum 5-byte integer (0b11110000 xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx) */
constexpr uint32_t MIN_5BYTE= MIN_4BYTE + (1 << 28);

/** Error from mlog_decode_varint() */
constexpr uint32_t MLOG_DECODE_ERROR= ~0U;

/** Decode the length of a variable-length encoded integer.
@param first  first byte of the encoded integer
@return the length, in bytes */
inline uint8_t mlog_decode_varint_length(byte first)
{
  uint8_t len= 1;
  for (; first & 0x80; len++, first= static_cast<uint8_t>(first << 1));
  return len;
}

/** Decode an integer in a redo log record.
@param log    redo log record buffer
@return the decoded integer
@retval MLOG_DECODE_ERROR on error */
template<typename byte_pointer>
inline uint32_t mlog_decode_varint(const byte_pointer log)
{
  uint32_t i= *log;
  if (i < MIN_2BYTE)
    return i;
  if (i < 0xc0)
    return MIN_2BYTE + ((i & ~0x80) << 8 | log[1]);
  if (i < 0xe0)
    return MIN_3BYTE + ((i & ~0xc0) << 16 | uint32_t{log[1]} << 8 | log[2]);
  if (i < 0xf0)
    return MIN_4BYTE + ((i & ~0xe0) << 24 | uint32_t{log[1]} << 16 |
                        uint32_t{log[2]} << 8 | log[3]);
  if (i == 0xf0)
  {
    i= uint32_t{log[1]} << 24 | uint32_t{log[2]} << 16 |
      uint32_t{log[3]} << 8 | log[4];
    if (i <= ~MIN_5BYTE)
      return MIN_5BYTE + i;
  }
  return MLOG_DECODE_ERROR;
}

/** Encode an integer in a redo log record.
@param log  redo log record buffer
@param i    the integer to encode
@return end of the encoded integer */
inline byte *mlog_encode_varint(byte *log, size_t i)
{
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wconversion" /* GCC 4 and 5 need this here */
#endif
  if (i < MIN_2BYTE)
  {
  }
  else if (i < MIN_3BYTE)
  {
    i-= MIN_2BYTE;
    static_assert(MIN_3BYTE - MIN_2BYTE == 1 << 14, "compatibility");
    *log++= 0x80 | static_cast<byte>(i >> 8);
  }
  else if (i < MIN_4BYTE)
  {
    i-= MIN_3BYTE;
    static_assert(MIN_4BYTE - MIN_3BYTE == 1 << 21, "compatibility");
    *log++= 0xc0 | static_cast<byte>(i >> 16);
    goto last2;
  }
  else if (i < MIN_5BYTE)
  {
    i-= MIN_4BYTE;
    static_assert(MIN_5BYTE - MIN_4BYTE == 1 << 28, "compatibility");
    *log++= 0xe0 | static_cast<byte>(i >> 24);
    goto last3;
  }
  else
  {
    ut_ad(i < MLOG_DECODE_ERROR);
    i-= MIN_5BYTE;
    *log++= 0xf0;
    *log++= static_cast<byte>(i >> 24);
last3:
    *log++= static_cast<byte>(i >> 16);
last2:
    *log++= static_cast<byte>(i >> 8);
  }
#if defined __GNUC__ && !defined __clang__ && __GNUC__ < 6
# pragma GCC diagnostic pop
#endif
  *log++= static_cast<byte>(i);
  return log;
}

/** Determine the length of a log record.
@param log  start of log record
@param end  end of the log record buffer
@return the length of the record, in bytes
@retval 0                 if the log extends past the end
@retval MLOG_DECODE_ERROR if the record is corrupted */
inline uint32_t mlog_decode_len(const byte *log, const byte *end)
{
  ut_ad(log < end);
  uint32_t i= *log;
  if (!i)
    return 0; /* end of mini-transaction */
  if (~i & 15)
    return (i & 15) + 1; /* 1..16 bytes */
  if (UNIV_UNLIKELY(++log == end))
    return 0; /* end of buffer */
  i= *log;
  if (UNIV_LIKELY(i < MIN_2BYTE)) /* 1 additional length byte: 16..143 bytes */
    return 16 + i;
  if (i < 0xc0) /* 2 additional length bytes: 144..16,527 bytes */
  {
    if (UNIV_UNLIKELY(log + 1 == end))
      return 0; /* end of buffer */
    return 16 + MIN_2BYTE + ((i & ~0xc0) << 8 | log[1]);
  }
  if (i < 0xe0) /* 3 additional length bytes: 16528..1065103 bytes */
  {
    if (UNIV_UNLIKELY(log + 2 == end))
      return 0; /* end of buffer */
    return 16 + MIN_3BYTE + ((i & ~0xe0) << 16 |
                             static_cast<uint32_t>(log[1]) << 8 | log[2]);
  }
  /* 1,065,103 bytes per log record ought to be enough for everyone */
  return MLOG_DECODE_ERROR;
}

/** Write 1, 2, 4, or 8 bytes to a file page.
@param[in]      block   file page
@param[in,out]  ptr     pointer in file page
@param[in]      val     value to write
@tparam l       number of bytes to write
@tparam w       write request type
@tparam V       type of val
@return whether any log was written */
template<unsigned l,mtr_t::write_type w,typename V>
inline bool mtr_t::write(const buf_block_t &block, void *ptr, V val)
{
  ut_ad(ut_align_down(ptr, srv_page_size) == block.page.frame);
  static_assert(l == 1 || l == 2 || l == 4 || l == 8, "wrong length");
  byte buf[l];

  switch (l) {
  case 1:
    ut_ad(val == static_cast<byte>(val));
    buf[0]= static_cast<byte>(val);
    break;
  case 2:
    ut_ad(val == static_cast<uint16_t>(val));
    mach_write_to_2(buf, static_cast<uint16_t>(val));
    break;
  case 4:
    ut_ad(val == static_cast<uint32_t>(val));
    mach_write_to_4(buf, static_cast<uint32_t>(val));
    break;
  case 8:
    mach_write_to_8(buf, val);
    break;
  }
  byte *p= static_cast<byte*>(ptr);
  const byte *const end= p + l;
  if (w != FORCED && m_log_mode == MTR_LOG_ALL)
  {
    const byte *b= buf;
    while (*p++ == *b++)
    {
      if (p == end)
      {
        ut_ad(w == MAYBE_NOP);
        return false;
      }
    }
    p--;
  }
  ::memcpy(ptr, buf, l);
  memcpy_low(block, static_cast<uint16_t>
             (ut_align_offset(p, srv_page_size)), p, end - p);
  return true;
}

/** Log an initialization of a string of bytes.
@param[in]      b       buffer page
@param[in]      ofs     byte offset from b->frame
@param[in]      len     length of the data to write
@param[in]      val     the data byte to write */
inline void mtr_t::memset(const buf_block_t &b, ulint ofs, ulint len, byte val)
{
  ut_ad(len);
  set_modified(b);
  if (m_log_mode != MTR_LOG_ALL)
    return;

  static_assert(MIN_4BYTE > UNIV_PAGE_SIZE_MAX, "consistency");
  size_t lenlen= (len < MIN_2BYTE ? 1 + 1 : len < MIN_3BYTE ? 2 + 1 : 3 + 1);
  byte *l= log_write<MEMSET>(b.page.id(), &b.page, lenlen, true, ofs);
  l= mlog_encode_varint(l, len);
  *l++= val;
  m_log.close(l);
  m_last_offset= static_cast<uint16_t>(ofs + len);
}

/** Initialize a string of bytes.
@param[in,out]  b       buffer page
@param[in]      ofs     byte offset from block->frame
@param[in]      len     length of the data to write
@param[in]      val     the data byte to write */
inline void mtr_t::memset(const buf_block_t *b, ulint ofs, ulint len, byte val)
{
  ut_ad(ofs <= ulint(srv_page_size));
  ut_ad(ofs + len <= ulint(srv_page_size));
  ::memset(ofs + b->page.frame, val, len);
  memset(*b, ofs, len, val);
}

/** Log an initialization of a repeating string of bytes.
@param[in]      b       buffer page
@param[in]      ofs     byte offset from b->frame
@param[in]      len     length of the data to write, in bytes
@param[in]      str     the string to write
@param[in]      size    size of str, in bytes */
inline void mtr_t::memset(const buf_block_t &b, ulint ofs, size_t len,
                          const void *str, size_t size)
{
  ut_ad(size);
  ut_ad(len > size); /* use mtr_t::memcpy() for shorter writes */
  set_modified(b);
  if (m_log_mode != MTR_LOG_ALL)
    return;

  static_assert(MIN_4BYTE > UNIV_PAGE_SIZE_MAX, "consistency");
  size_t lenlen= (len < MIN_2BYTE ? 1 : len < MIN_3BYTE ? 2 : 3);
  byte *l= log_write<MEMSET>(b.page.id(), &b.page, lenlen + size, true, ofs);
  l= mlog_encode_varint(l, len);
  ::memcpy(l, str, size);
  l+= size;
  m_log.close(l);
  m_last_offset= static_cast<uint16_t>(ofs + len);
}

/** Initialize a repeating string of bytes.
@param[in,out]  b       buffer page
@param[in]      ofs     byte offset from b->frame
@param[in]      len     length of the data to write, in bytes
@param[in]      str     the string to write
@param[in]      size    size of str, in bytes */
inline void mtr_t::memset(const buf_block_t *b, ulint ofs, size_t len,
                          const void *str, size_t size)
{
  ut_ad(ofs <= ulint(srv_page_size));
  ut_ad(ofs + len <= ulint(srv_page_size));
  ut_ad(len > size); /* use mtr_t::memcpy() for shorter writes */
  size_t s= 0;
  while (s < len)
  {
    ::memcpy(ofs + s + b->page.frame, str, size);
    s+= len;
  }
  ::memcpy(ofs + s + b->page.frame, str, len - s);
  memset(*b, ofs, len, str, size);
}

/** Log a write of a byte string to a page.
@param[in]      b       buffer page
@param[in]      offset  byte offset from b->frame
@param[in]      str     the data to write
@param[in]      len     length of the data to write */
inline void mtr_t::memcpy(const buf_block_t &b, ulint offset, ulint len)
{
  ut_ad(len);
  ut_ad(offset <= ulint(srv_page_size));
  ut_ad(offset + len <= ulint(srv_page_size));
  memcpy_low(b, uint16_t(offset), &b.page.frame[offset], len);
}

/** Log a write of a byte string to a page.
@param block   page
@param offset  byte offset within page
@param data    data to be written
@param len     length of the data, in bytes */
inline void mtr_t::memcpy_low(const buf_block_t &block, uint16_t offset,
                              const void *data, size_t len)
{
  ut_ad(len);
  set_modified(block);
  if (m_log_mode != MTR_LOG_ALL)
    return;
  if (len < mtr_buf_t::MAX_DATA_SIZE - (1 + 3 + 3 + 5 + 5))
  {
    byte *end= log_write<WRITE>(block.page.id(), &block.page, len, true,
                                offset);
    ::memcpy(end, data, len);
    m_log.close(end + len);
  }
  else
  {
    m_log.close(log_write<WRITE>(block.page.id(), &block.page, len, false,
                                 offset));
    m_log.push(static_cast<const byte*>(data), static_cast<uint32_t>(len));
  }
  m_last_offset= static_cast<uint16_t>(offset + len);
}

/** Log that a string of bytes was copied from the same page.
@param[in]      b       buffer page
@param[in]      d       destination offset within the page
@param[in]      s       source offset within the page
@param[in]      len     length of the data to copy */
inline void mtr_t::memmove(const buf_block_t &b, ulint d, ulint s, ulint len)
{
  ut_ad(d >= 8);
  ut_ad(s >= 8);
  ut_ad(len);
  ut_ad(s <= ulint(srv_page_size));
  ut_ad(s + len <= ulint(srv_page_size));
  ut_ad(s != d);
  ut_ad(d <= ulint(srv_page_size));
  ut_ad(d + len <= ulint(srv_page_size));

  set_modified(b);
  if (m_log_mode != MTR_LOG_ALL)
    return;
  static_assert(MIN_4BYTE > UNIV_PAGE_SIZE_MAX, "consistency");
  size_t lenlen= (len < MIN_2BYTE ? 1 : len < MIN_3BYTE ? 2 : 3);
  /* The source offset is encoded relative to the destination offset,
  with the sign in the least significant bit. */
  if (s > d)
    s= (s - d) << 1;
  else
    s= (d - s) << 1 | 1;
  /* The source offset 0 is not possible. */
  s-= 1 << 1;
  size_t slen= (s < MIN_2BYTE ? 1 : s < MIN_3BYTE ? 2 : 3);
  byte *l= log_write<MEMMOVE>(b.page.id(), &b.page, lenlen + slen, true, d);
  l= mlog_encode_varint(l, len);
  l= mlog_encode_varint(l, s);
  m_log.close(l);
  m_last_offset= static_cast<uint16_t>(d + len);
}

/**
Write a log record.
@tparam type   redo log record type
@param id     persistent page identifier
@param bpage  buffer pool page, or nullptr
@param len    number of additional bytes to write
@param alloc  whether to allocate the additional bytes
@param offset byte offset, or 0 if the record type does not allow one
@return end of mini-transaction log, minus len */
template<byte type>
inline byte *mtr_t::log_write(const page_id_t id, const buf_page_t *bpage,
                              size_t len, bool alloc, size_t offset)
{
  static_assert(!(type & 15) && type != RESERVED && type != OPTION &&
                type <= FILE_CHECKPOINT, "invalid type");
  ut_ad(type >= FILE_CREATE || is_named_space(id.space()));
  ut_ad(!bpage || bpage->id() == id);
  ut_ad(id < end_page_id);
  constexpr bool have_len= type != INIT_PAGE && type != FREE_PAGE;
  constexpr bool have_offset= type == WRITE || type == MEMSET ||
    type == MEMMOVE;
  static_assert(!have_offset || have_len, "consistency");
  ut_ad(have_len || len == 0);
  ut_ad(have_len || !alloc);
  ut_ad(have_offset || offset == 0);
  ut_ad(offset + len <= srv_page_size);
  static_assert(MIN_4BYTE >= UNIV_PAGE_SIZE_MAX, "consistency");

  size_t max_len;
  if (!have_len)
    max_len= 1 + 5 + 5;
  else if (!have_offset)
    max_len= bpage && m_last == bpage
      ? 1 + 3
      : 1 + 3 + 5 + 5;
  else if (bpage && m_last == bpage && m_last_offset <= offset)
  {
    /* Encode the offset relative from m_last_offset. */
    offset-= m_last_offset;
    max_len= 1 + 3 + 3;
  }
  else
    max_len= 1 + 3 + 5 + 5 + 3;
  byte *const log_ptr= m_log.open(alloc ? max_len + len : max_len);
  byte *end= log_ptr + 1;
  const byte same_page= max_len < 1 + 5 + 5 ? 0x80 : 0;
  if (!same_page)
  {
    end= mlog_encode_varint(end, id.space());
    end= mlog_encode_varint(end, id.page_no());
    m_last= bpage;
  }
  if (have_offset)
  {
    byte* oend= mlog_encode_varint(end, offset);
    if (oend + len > &log_ptr[16])
    {
      len+= oend - log_ptr - 15;
      if (len >= MIN_3BYTE - 1)
        len+= 2;
      else if (len >= MIN_2BYTE)
        len++;

      *log_ptr= type | same_page;
      end= mlog_encode_varint(log_ptr + 1, len);
      if (!same_page)
      {
        end= mlog_encode_varint(end, id.space());
        end= mlog_encode_varint(end, id.page_no());
      }
      end= mlog_encode_varint(end, offset);
      return end;
    }
    else
      end= oend;
  }
  else if (len >= 3 && end + len > &log_ptr[16])
  {
    len+= end - log_ptr - 15;
    if (len >= MIN_3BYTE - 1)
      len+= 2;
    else if (len >= MIN_2BYTE)
      len++;

    end= log_ptr;
    *end++= type | same_page;
    end= mlog_encode_varint(end, len);

    if (!same_page)
    {
      end= mlog_encode_varint(end, id.space());
      end= mlog_encode_varint(end, id.page_no());
    }
    return end;
  }

  ut_ad(end + len >= &log_ptr[1] + !same_page);
  ut_ad(end + len <= &log_ptr[16]);
  ut_ad(end <= &log_ptr[max_len]);
  *log_ptr= type | same_page | static_cast<byte>(end + len - log_ptr - 1);
  ut_ad(*log_ptr & 15);
  return end;
}

/** Write a byte string to a page.
@param[in]      b       buffer page
@param[in]      dest    destination within b.frame
@param[in]      str     the data to write
@param[in]      len     length of the data to write
@tparam w       write request type */
template<mtr_t::write_type w>
inline void mtr_t::memcpy(const buf_block_t &b, void *dest, const void *str,
                          ulint len)
{
  ut_ad(ut_align_down(dest, srv_page_size) == b.page.frame);
  char *d= static_cast<char*>(dest);
  const char *s= static_cast<const char*>(str);
  if (w != FORCED && m_log_mode == MTR_LOG_ALL)
  {
    ut_ad(len);
    const char *const end= d + len;
    while (*d++ == *s++)
    {
      if (d == end)
      {
        ut_ad(w == MAYBE_NOP);
        return;
      }
    }
    s--;
    d--;
    len= static_cast<ulint>(end - d);
  }
  ::memcpy(d, s, len);
  memcpy(b, ut_align_offset(d, srv_page_size), len);
}

/** Initialize an entire page.
@param[in,out]        b       buffer page */
inline void mtr_t::init(buf_block_t *b)
{
  const page_id_t id{b->page.id()};
  ut_ad(is_named_space(id.space()));
  ut_ad(!m_freed_pages == !m_freed_space);

  if (UNIV_LIKELY_NULL(m_freed_space) &&
      m_freed_space->id == id.space() &&
      m_freed_pages->remove_if_exists(b->page.id().page_no()) &&
      m_freed_pages->empty())
  {
    delete m_freed_pages;
    m_freed_pages= nullptr;
    m_freed_space= nullptr;
  }

  b->page.set_reinit(b->page.state() & buf_page_t::LRU_MASK);

  if (m_log_mode != MTR_LOG_ALL)
  {
    ut_ad(m_log_mode == MTR_LOG_NONE || m_log_mode == MTR_LOG_NO_REDO);
    return;
  }

  m_log.close(log_write<INIT_PAGE>(b->page.id(), &b->page));
  m_last_offset= FIL_PAGE_TYPE;
}

/** Free a page.
@param[in]	space 	tablespace contains page to be freed
@param[in]	offset	page offset to be freed */
inline void mtr_t::free(fil_space_t &space, uint32_t offset)
{
  ut_ad(is_named_space(&space));
  ut_ad(!m_freed_space || m_freed_space == &space);

  if (m_log_mode == MTR_LOG_ALL)
    m_log.close(log_write<FREE_PAGE>({space.id, offset}, nullptr));
}

/** Write an EXTENDED log record.
@param block  buffer pool page
@param type   extended record subtype; @see mrec_ext_t */
inline void mtr_t::log_write_extended(const buf_block_t &block, byte type)
{
  set_modified(block);
  if (m_log_mode != MTR_LOG_ALL)
    return;
  byte *l= log_write<EXTENDED>(block.page.id(), &block.page, 1, true);
  *l++= type;
  m_log.close(l);
  m_last_offset= FIL_PAGE_TYPE;
}

/** Write log for partly initializing a B-tree or R-tree page.
@param block    B-tree or R-tree page
@param comp     false=ROW_FORMAT=REDUNDANT, true=COMPACT or DYNAMIC */
inline void mtr_t::page_create(const buf_block_t &block, bool comp)
{
  static_assert(false == INIT_ROW_FORMAT_REDUNDANT, "encoding");
  static_assert(true == INIT_ROW_FORMAT_DYNAMIC, "encoding");
  log_write_extended(block, comp);
}

/** Write log for deleting a B-tree or R-tree record in ROW_FORMAT=REDUNDANT.
@param block      B-tree or R-tree page
@param prev_rec   byte offset of the predecessor of the record to delete,
                  starting from PAGE_OLD_INFIMUM */
inline void mtr_t::page_delete(const buf_block_t &block, ulint prev_rec)
{
  ut_ad(!block.zip_size());
  ut_ad(prev_rec < block.physical_size());
  set_modified(block);
  if (m_log_mode != MTR_LOG_ALL)
    return;
  size_t len= (prev_rec < MIN_2BYTE ? 2 : prev_rec < MIN_3BYTE ? 3 : 4);
  byte *l= log_write<EXTENDED>(block.page.id(), &block.page, len, true);
  ut_d(byte *end= l + len);
  *l++= DELETE_ROW_FORMAT_REDUNDANT;
  l= mlog_encode_varint(l, prev_rec);
  ut_ad(end == l);
  m_log.close(l);
  m_last_offset= FIL_PAGE_TYPE;
}

/** Write log for deleting a COMPACT or DYNAMIC B-tree or R-tree record.
@param block      B-tree or R-tree page
@param prev_rec   byte offset of the predecessor of the record to delete,
                  starting from PAGE_NEW_INFIMUM
@param prev_rec   the predecessor of the record to delete
@param hdr_size   record header size, excluding REC_N_NEW_EXTRA_BYTES
@param data_size  data payload size, in bytes */
inline void mtr_t::page_delete(const buf_block_t &block, ulint prev_rec,
                               size_t hdr_size, size_t data_size)
{
  ut_ad(!block.zip_size());
  set_modified(block);
  ut_ad(hdr_size < MIN_3BYTE);
  ut_ad(prev_rec < block.physical_size());
  ut_ad(data_size < block.physical_size());
  if (m_log_mode != MTR_LOG_ALL)
    return;
  size_t len= prev_rec < MIN_2BYTE ? 2 : prev_rec < MIN_3BYTE ? 3 : 4;
  len+= hdr_size < MIN_2BYTE ? 1 : 2;
  len+= data_size < MIN_2BYTE ? 1 : data_size < MIN_3BYTE ? 2 : 3;
  byte *l= log_write<EXTENDED>(block.page.id(), &block.page, len, true);
  ut_d(byte *end= l + len);
  *l++= DELETE_ROW_FORMAT_DYNAMIC;
  l= mlog_encode_varint(l, prev_rec);
  l= mlog_encode_varint(l, hdr_size);
  l= mlog_encode_varint(l, data_size);
  ut_ad(end == l);
  m_log.close(l);
  m_last_offset= FIL_PAGE_TYPE;
}

/** Write log for initializing an undo log page.
@param block    undo page */
inline void mtr_t::undo_create(const buf_block_t &block)
{
  log_write_extended(block, UNDO_INIT);
}

/** Write log for appending an undo log record.
@param block    undo page
@param data     record within the undo page
@param len      length of the undo record, in bytes */
inline void mtr_t::undo_append(const buf_block_t &block,
                               const void *data, size_t len)
{
  ut_ad(len > 2);
  set_modified(block);
  if (m_log_mode != MTR_LOG_ALL)
    return;
  const bool small= len + 1 < mtr_buf_t::MAX_DATA_SIZE - (1 + 3 + 3 + 5 + 5);
  byte *end= log_write<EXTENDED>(block.page.id(), &block.page, len + 1, small);
  if (UNIV_LIKELY(small))
  {
    *end++= UNDO_APPEND;
    ::memcpy(end, data, len);
    m_log.close(end + len);
  }
  else
  {
    m_log.close(end);
    *m_log.push<byte*>(1)= UNDO_APPEND;
    m_log.push(static_cast<const byte*>(data), static_cast<uint32_t>(len));
  }
  m_last_offset= FIL_PAGE_TYPE;
}

/** Trim the end of a tablespace.
@param id       first page identifier that will not be in the file */
inline void mtr_t::trim_pages(const page_id_t id)
{
  if (m_log_mode != MTR_LOG_ALL)
    return;
  byte *l= log_write<EXTENDED>(id, nullptr, 1, true);
  *l++= TRIM_PAGES;
  m_log.close(l);
  set_trim_pages();
}
