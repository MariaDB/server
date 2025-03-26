/*****************************************************************************

Copyright (c) 1997, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2022, MariaDB Corporation.

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

/**************************************************//**
@file log/log0recv.cc
Recovery

Created 9/20/1997 Heikki Tuuri
*******************************************************/

#include "univ.i"

#include <map>
#include <string>
#include <my_service_manager.h>

#include "log0recv.h"

#ifdef HAVE_MY_AES_H
#include <my_aes.h>
#endif

#include "log0crypt.h"
#include "mem0mem.h"
#include "buf0buf.h"
#include "buf0dblwr.h"
#include "buf0flu.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "page0page.h"
#include "page0cur.h"
#include "trx0undo.h"
#include "ibuf0ibuf.h"
#include "trx0undo.h"
#include "trx0rec.h"
#include "fil0fil.h"
#include "buf0rea.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "fil0pagecompress.h"
#include "log.h"

/** The recovery system */
recv_sys_t	recv_sys;
/** TRUE when recv_init_crash_recovery() has been called. */
bool	recv_needed_recovery;
#ifdef UNIV_DEBUG
/** TRUE if writing to the redo log (mtr_commit) is forbidden.
Protected by log_sys.mutex. */
bool	recv_no_log_write = false;
#endif /* UNIV_DEBUG */

/** If the following is TRUE, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this becomes TRUE if
the log record hash table becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

true means that recovery is running and no operations on the log file
are allowed yet: the variable name is misleading. */
bool	recv_no_ibuf_operations;

/** Stored physical log record */
struct log_phys_t : public log_rec_t
{
  /** start LSN of the mini-transaction (not necessarily of this record) */
  const lsn_t start_lsn;
private:
  /** @return the start of length and data */
  const byte *start() const
  {
    return my_assume_aligned<sizeof(size_t)>
      (reinterpret_cast<const byte*>(&start_lsn + 1));
  }
  /** @return the start of length and data */
  byte *start()
  { return const_cast<byte*>(const_cast<const log_phys_t*>(this)->start()); }
  /** @return the length of the following record */
  uint16_t len() const { uint16_t i; memcpy(&i, start(), 2); return i; }

  /** @return start of the log records */
  byte *begin() { return start() + 2; }
  /** @return end of the log records */
  byte *end() { byte *e= begin() + len(); ut_ad(!*e); return e; }
public:
  /** @return start of the log records */
  const byte *begin() const { return const_cast<log_phys_t*>(this)->begin(); }
  /** @return end of the log records */
  const byte *end() const { return const_cast<log_phys_t*>(this)->end(); }

  /** Determine the allocated size of the object.
  @param len  length of recs, excluding terminating NUL byte
  @return the total allocation size */
  static inline size_t alloc_size(size_t len);

  /** Constructor.
  @param start_lsn start LSN of the mini-transaction
  @param lsn  mtr_t::commit_lsn() of the mini-transaction
  @param recs the first log record for the page in the mini-transaction
  @param size length of recs, in bytes, excluding terminating NUL byte */
  log_phys_t(lsn_t start_lsn, lsn_t lsn, const byte *recs, size_t size) :
    log_rec_t(lsn), start_lsn(start_lsn)
  {
    ut_ad(start_lsn);
    ut_ad(start_lsn < lsn);
    const uint16_t len= static_cast<uint16_t>(size);
    ut_ad(len == size);
    memcpy(start(), &len, 2);
    reinterpret_cast<byte*>(memcpy(begin(), recs, size))[size]= 0;
  }

  /** Append a record to the log.
  @param recs  log to append
  @param size  size of the log, in bytes */
  void append(const byte *recs, size_t size)
  {
    ut_ad(start_lsn < lsn);
    uint16_t l= len();
    reinterpret_cast<byte*>(memcpy(end(), recs, size))[size]= 0;
    l= static_cast<uint16_t>(l + size);
    memcpy(start(), &l, 2);
  }

  /** Apply an UNDO_APPEND record.
  @see mtr_t::undo_append()
  @param block   undo log page
  @param data    undo log record
  @param len     length of the undo log record
  @return whether the operation failed (inconcistency was noticed) */
  static bool undo_append(const buf_block_t &block, const byte *data,
                          size_t len)
  {
    ut_ad(len > 2);
    byte *free_p= my_assume_aligned<2>
      (TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE + block.page.frame);
    const uint16_t free= mach_read_from_2(free_p);
    if (UNIV_UNLIKELY(free < TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE ||
                      free + len + 6 >= srv_page_size - FIL_PAGE_DATA_END))
    {
      ib::error() << "Not applying UNDO_APPEND due to corruption on "
                  << block.page.id();
      return true;
    }

    byte *p= block.page.frame + free;
    mach_write_to_2(free_p, free + 4 + len);
    memcpy(p, free_p, 2);
    p+= 2;
    memcpy(p, data, len);
    p+= len;
    mach_write_to_2(p, free);
    return false;
  }

  /** Check an OPT_PAGE_CHECKSUM record.
  @see mtr_t::page_checksum()
  @param block   buffer page
  @param l       pointer to checksum
  @return whether an unrecoverable mismatch was found */
  static bool page_checksum(const buf_block_t &block, const byte *l)
  {
    size_t size;
    const byte *page= block.page.zip.data;
    if (UNIV_LIKELY_NULL(page))
      size= (UNIV_ZIP_SIZE_MIN >> 1) << block.page.zip.ssize;
    else
    {
      page= block.page.frame;
      size= srv_page_size;
    }
    if (UNIV_LIKELY(my_crc32c(my_crc32c(my_crc32c(0, page + FIL_PAGE_OFFSET,
                                                  FIL_PAGE_LSN -
                                                  FIL_PAGE_OFFSET),
                                        page + FIL_PAGE_TYPE, 2),
                              page + FIL_PAGE_SPACE_ID,
                              size - (FIL_PAGE_SPACE_ID + 8)) ==
                    mach_read_from_4(l)))
      return false;

    ib::error() << "OPT_PAGE_CHECKSUM mismatch on " << block.page.id();
    return !srv_force_recovery;
  }

  /** The status of apply() */
  enum apply_status {
    /** The page was not affected */
    APPLIED_NO= 0,
    /** The page was modified */
    APPLIED_YES,
    /** The page was modified, affecting the encryption parameters */
    APPLIED_TO_ENCRYPTION,
    /** The page was modified, affecting the tablespace header */
    APPLIED_TO_FSP_HEADER,
    /** The page was found to be corrupted */
    APPLIED_CORRUPTED,
  };

  /** Apply log to a page frame.
  @param[in,out] block         buffer block
  @param[in,out] last_offset   last byte offset, for same_page records
  @return whether any log was applied to the page */
  apply_status apply(const buf_block_t &block, uint16_t &last_offset) const
  {
    const byte * const recs= begin();
    byte *const frame= block.page.zip.data
      ? block.page.zip.data : block.page.frame;
    const size_t size= block.physical_size();
    apply_status applied= APPLIED_NO;

    for (const byte *l= recs;;)
    {
      const byte b= *l++;
      if (!b)
        return applied;
      ut_ad((b & 0x70) != RESERVED);
      size_t rlen= b & 0xf;
      if (!rlen)
      {
        const size_t lenlen= mlog_decode_varint_length(*l);
        const uint32_t addlen= mlog_decode_varint(l);
        ut_ad(addlen != MLOG_DECODE_ERROR);
        rlen= addlen + 15 - lenlen;
        l+= lenlen;
      }
      if (!(b & 0x80))
      {
        /* Skip the page identifier. It has already been validated. */
        size_t idlen= mlog_decode_varint_length(*l);
        ut_ad(idlen <= 5);
        ut_ad(idlen < rlen);
        ut_ad(mlog_decode_varint(l) == block.page.id().space());
        l+= idlen;
        rlen-= idlen;
        idlen= mlog_decode_varint_length(*l);
        ut_ad(idlen <= 5);
        ut_ad(idlen <= rlen);
        ut_ad(mlog_decode_varint(l) == block.page.id().page_no());
        l+= idlen;
        rlen-= idlen;
        last_offset= 0;
      }

      switch (b & 0x70) {
      case FREE_PAGE:
        ut_ad(last_offset == 0);
        goto next_not_same_page;
      case INIT_PAGE:
        if (UNIV_LIKELY(rlen == 0))
        {
          memset_aligned<UNIV_ZIP_SIZE_MIN>(frame, 0, size);
          mach_write_to_4(frame + FIL_PAGE_OFFSET, block.page.id().page_no());
          memset_aligned<8>(FIL_PAGE_PREV + frame, 0xff, 8);
          mach_write_to_4(frame + FIL_PAGE_SPACE_ID, block.page.id().space());
          last_offset= FIL_PAGE_TYPE;
      next_after_applying:
          if (applied == APPLIED_NO)
            applied= APPLIED_YES;
        }
        else
        {
      record_corrupted:
          if (!srv_force_recovery)
          {
            recv_sys.set_corrupt_log();
            return applied;
          }
      next_not_same_page:
          last_offset= 1; /* the next record must not be same_page  */
        }
        l+= rlen;
        continue;
      case OPTION:
        ut_ad(rlen == 5);
        ut_ad(*l == OPT_PAGE_CHECKSUM);
        if (page_checksum(block, l + 1))
        {
page_corrupted:
          sql_print_error("InnoDB: Set innodb_force_recovery=1"
                          " to ignore corruption.");
          return APPLIED_CORRUPTED;
        }
        goto next_after_applying;
      }

      ut_ad(mach_read_from_4(frame + FIL_PAGE_OFFSET) ==
            block.page.id().page_no());
      ut_ad(mach_read_from_4(frame + FIL_PAGE_SPACE_ID) ==
            block.page.id().space());
      ut_ad(last_offset <= 1 || last_offset > 8);
      ut_ad(last_offset <= size);

      switch (b & 0x70) {
      case EXTENDED:
        if (UNIV_UNLIKELY(block.page.id().page_no() < 3 ||
                          block.page.zip.ssize))
          goto record_corrupted;
        static_assert(INIT_ROW_FORMAT_REDUNDANT == 0, "compatiblity");
        static_assert(INIT_ROW_FORMAT_DYNAMIC == 1, "compatibility");
        if (UNIV_UNLIKELY(!rlen))
          goto record_corrupted;
        switch (const byte subtype= *l) {
          uint8_t ll;
          size_t prev_rec, hdr_size;
        default:
          goto record_corrupted;
        case INIT_ROW_FORMAT_REDUNDANT:
        case INIT_ROW_FORMAT_DYNAMIC:
          if (UNIV_UNLIKELY(rlen != 1))
            goto record_corrupted;
          page_create_low(&block, *l != INIT_ROW_FORMAT_REDUNDANT);
          break;
        case UNDO_INIT:
          if (UNIV_UNLIKELY(rlen != 1))
            goto record_corrupted;
          trx_undo_page_init(block);
          break;
        case UNDO_APPEND:
          if (UNIV_UNLIKELY(rlen <= 3))
            goto record_corrupted;
          if (undo_append(block, ++l, --rlen) && !srv_force_recovery)
            goto page_corrupted;
          break;
        case INSERT_HEAP_REDUNDANT:
        case INSERT_REUSE_REDUNDANT:
        case INSERT_HEAP_DYNAMIC:
        case INSERT_REUSE_DYNAMIC:
          if (UNIV_UNLIKELY(rlen < 2))
            goto record_corrupted;
          rlen--;
          ll= mlog_decode_varint_length(*++l);
          if (UNIV_UNLIKELY(ll > 3 || ll >= rlen))
            goto record_corrupted;
          prev_rec= mlog_decode_varint(l);
          ut_ad(prev_rec != MLOG_DECODE_ERROR);
          rlen-= ll;
          l+= ll;
          ll= mlog_decode_varint_length(*l);
          static_assert(INSERT_HEAP_REDUNDANT == 4, "compatibility");
          static_assert(INSERT_REUSE_REDUNDANT == 5, "compatibility");
          static_assert(INSERT_HEAP_DYNAMIC == 6, "compatibility");
          static_assert(INSERT_REUSE_DYNAMIC == 7, "compatibility");
          if (subtype & 2)
          {
            size_t shift= 0;
            if (subtype & 1)
            {
              if (UNIV_UNLIKELY(ll > 3 || ll >= rlen))
                goto record_corrupted;
              shift= mlog_decode_varint(l);
              ut_ad(shift != MLOG_DECODE_ERROR);
              rlen-= ll;
              l+= ll;
              ll= mlog_decode_varint_length(*l);
            }
            if (UNIV_UNLIKELY(ll > 3 || ll >= rlen))
              goto record_corrupted;
            size_t enc_hdr_l= mlog_decode_varint(l);
            ut_ad(enc_hdr_l != MLOG_DECODE_ERROR);
            rlen-= ll;
            l+= ll;
            ll= mlog_decode_varint_length(*l);
            if (UNIV_UNLIKELY(ll > 2 || ll >= rlen))
              goto record_corrupted;
            size_t hdr_c= mlog_decode_varint(l);
            ut_ad(hdr_c != MLOG_DECODE_ERROR);
            rlen-= ll;
            l+= ll;
            ll= mlog_decode_varint_length(*l);
            if (UNIV_UNLIKELY(ll > 3 || ll > rlen))
              goto record_corrupted;
            size_t data_c= mlog_decode_varint(l);
            ut_ad(data_c != MLOG_DECODE_ERROR);
            rlen-= ll;
            l+= ll;
            if (page_apply_insert_dynamic(block, subtype & 1, prev_rec,
                                          shift, enc_hdr_l, hdr_c, data_c,
                                          l, rlen) && !srv_force_recovery)
              goto page_corrupted;
          }
          else
          {
            if (UNIV_UNLIKELY(ll > 2 || ll >= rlen))
              goto record_corrupted;
            size_t header= mlog_decode_varint(l);
            ut_ad(header != MLOG_DECODE_ERROR);
            rlen-= ll;
            l+= ll;
            ll= mlog_decode_varint_length(*l);
            if (UNIV_UNLIKELY(ll > 2 || ll >= rlen))
              goto record_corrupted;
            size_t hdr_c= mlog_decode_varint(l);
            ut_ad(hdr_c != MLOG_DECODE_ERROR);
            rlen-= ll;
            l+= ll;
            ll= mlog_decode_varint_length(*l);
            if (UNIV_UNLIKELY(ll > 2 || ll > rlen))
              goto record_corrupted;
            size_t data_c= mlog_decode_varint(l);
            rlen-= ll;
            l+= ll;
            if (page_apply_insert_redundant(block, subtype & 1, prev_rec,
                                            header, hdr_c, data_c,
                                            l, rlen) && !srv_force_recovery)
              goto page_corrupted;
          }
          break;
        case DELETE_ROW_FORMAT_REDUNDANT:
          if (UNIV_UNLIKELY(rlen < 2 || rlen > 4))
            goto record_corrupted;
          rlen--;
          ll= mlog_decode_varint_length(*++l);
          if (UNIV_UNLIKELY(ll != rlen))
            goto record_corrupted;
          if (page_apply_delete_redundant(block, mlog_decode_varint(l)) &&
              !srv_force_recovery)
            goto page_corrupted;
          break;
        case DELETE_ROW_FORMAT_DYNAMIC:
          if (UNIV_UNLIKELY(rlen < 2))
            goto record_corrupted;
          rlen--;
          ll= mlog_decode_varint_length(*++l);
          if (UNIV_UNLIKELY(ll > 3 || ll >= rlen))
            goto record_corrupted;
          prev_rec= mlog_decode_varint(l);
          ut_ad(prev_rec != MLOG_DECODE_ERROR);
          rlen-= ll;
          l+= ll;
          ll= mlog_decode_varint_length(*l);
          if (UNIV_UNLIKELY(ll > 2 || ll >= rlen))
            goto record_corrupted;
          hdr_size= mlog_decode_varint(l);
          ut_ad(hdr_size != MLOG_DECODE_ERROR);
          rlen-= ll;
          l+= ll;
          ll= mlog_decode_varint_length(*l);
          if (UNIV_UNLIKELY(ll > 3 || ll != rlen))
            goto record_corrupted;
          if (page_apply_delete_dynamic(block, prev_rec, hdr_size,
                                        mlog_decode_varint(l)) &&
              !srv_force_recovery)
            goto page_corrupted;
          break;
        }
        last_offset= FIL_PAGE_TYPE;
        goto next_after_applying;
      case WRITE:
      case MEMSET:
      case MEMMOVE:
        if (UNIV_UNLIKELY(last_offset == 1))
          goto record_corrupted;
        const size_t olen= mlog_decode_varint_length(*l);
        if (UNIV_UNLIKELY(olen >= rlen) || UNIV_UNLIKELY(olen > 3))
          goto record_corrupted;
        const uint32_t offset= mlog_decode_varint(l);
        ut_ad(offset != MLOG_DECODE_ERROR);
        static_assert(FIL_PAGE_OFFSET == 4, "compatibility");
        if (UNIV_UNLIKELY(offset >= size))
          goto record_corrupted;
        if (UNIV_UNLIKELY(offset + last_offset < 8 ||
                          offset + last_offset >= size))
          goto record_corrupted;
        last_offset= static_cast<uint16_t>(last_offset + offset);
        l+= olen;
        rlen-= olen;
        size_t llen= rlen;
        if ((b & 0x70) == WRITE)
        {
          if (UNIV_UNLIKELY(rlen + last_offset > size))
            goto record_corrupted;
          memcpy(frame + last_offset, l, llen);
          if (UNIV_LIKELY(block.page.id().page_no()));
          else if (llen == 11 + MY_AES_BLOCK_SIZE &&
                   last_offset == FSP_HEADER_OFFSET + MAGIC_SZ +
                   fsp_header_get_encryption_offset(block.zip_size()))
            applied= APPLIED_TO_ENCRYPTION;
          else if (last_offset < FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN + 4 &&
                   last_offset + llen >= FSP_HEADER_OFFSET + FSP_SIZE)
            applied= APPLIED_TO_FSP_HEADER;
        next_after_applying_write:
          ut_ad(llen + last_offset <= size);
          last_offset= static_cast<uint16_t>(last_offset + llen);
          goto next_after_applying;
        }
        llen= mlog_decode_varint_length(*l);
        if (UNIV_UNLIKELY(llen > rlen || llen > 3))
          goto record_corrupted;
        const uint32_t len= mlog_decode_varint(l);
        ut_ad(len != MLOG_DECODE_ERROR);
        if (UNIV_UNLIKELY(len + last_offset > size))
          goto record_corrupted;
        l+= llen;
        rlen-= llen;
        llen= len;
        if ((b & 0x70) == MEMSET)
        {
          ut_ad(rlen <= llen);
          if (UNIV_UNLIKELY(rlen != 1))
          {
            size_t s;
            for (s= 0; s < llen; s+= rlen)
              memcpy(frame + last_offset + s, l, rlen);
            memcpy(frame + last_offset + s, l, llen - s);
          }
          else
            memset(frame + last_offset, *l, llen);
          goto next_after_applying_write;
        }
        const size_t slen= mlog_decode_varint_length(*l);
        if (UNIV_UNLIKELY(slen != rlen || slen > 3))
          goto record_corrupted;
        uint32_t s= mlog_decode_varint(l);
        ut_ad(slen != MLOG_DECODE_ERROR);
        if (s & 1)
          s= last_offset - (s >> 1) - 1;
        else
          s= last_offset + (s >> 1) + 1;
        if (UNIV_LIKELY(s >= 8 && s + llen <= size))
        {
          memmove(frame + last_offset, frame + s, llen);
          goto next_after_applying_write;
        }
      }
      goto record_corrupted;
    }
  }
};


inline size_t log_phys_t::alloc_size(size_t len)
{
  return len + (1 + 2 + sizeof(log_phys_t));
}


/** Tablespace item during recovery */
struct file_name_t {
	/** Tablespace file name (FILE_MODIFY) */
	std::string	name;
	/** Tablespace object (NULL if not valid or not found) */
	fil_space_t*	space = nullptr;

	/** Tablespace status. */
	enum fil_status {
		/** Normal tablespace */
		NORMAL,
		/** Deleted tablespace */
		DELETED,
		/** Missing tablespace */
		MISSING
	};

	/** Status of the tablespace */
	fil_status	status;

	/** FSP_SIZE of tablespace */
	uint32_t	size = 0;

	/** Freed pages of tablespace */
	range_set	freed_ranges;

	/** Dummy flags before they have been read from the .ibd file */
	static constexpr uint32_t initial_flags = FSP_FLAGS_FCRC32_MASK_MARKER;
	/** FSP_SPACE_FLAGS of tablespace */
	uint32_t	flags = initial_flags;

	/** Constructor */
	file_name_t(std::string name_, bool deleted)
		: name(std::move(name_)), status(deleted ? DELETED: NORMAL) {}

  /** Add the freed pages */
  void add_freed_page(uint32_t page_no) { freed_ranges.add_value(page_no); }

  /** Remove the freed pages */
  void remove_freed_page(uint32_t page_no)
  {
    if (freed_ranges.empty()) return;
    freed_ranges.remove_value(page_no);
  }
};

/** Map of dirty tablespaces during recovery */
typedef std::map<
	uint32_t,
	file_name_t,
	std::less<ulint>,
	ut_allocator<std::pair<const uint32_t, file_name_t> > >	recv_spaces_t;

static recv_spaces_t	recv_spaces;

/** The last parsed FILE_RENAME records */
static std::map<uint32_t,std::string> renamed_spaces;

/** Files for which fil_ibd_load() returned FIL_LOAD_DEFER */
static struct
{
  /** Maintains the last opened defer file name along with lsn */
  struct item
  {
    /** Log sequence number of latest add() called by fil_name_process() */
    lsn_t lsn;
    /** File name from the FILE_ record */
    std::string file_name;
    /** whether a FILE_DELETE record was encountered */
    mutable bool deleted;
  };

  using map= std::map<const uint32_t, item, std::less<const uint32_t>,
                      ut_allocator<std::pair<const uint32_t, item> > >;

  /** Map of defer tablespaces */
  map defers;

  /** Add the deferred space only if it is latest one
  @param space  space identifier
  @param f_name file name
  @param lsn    log sequence number of the FILE_ record */
  void add(uint32_t space, const std::string &f_name, lsn_t lsn)
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);
    const char *filename= f_name.c_str();

    if (srv_operation == SRV_OPERATION_RESTORE)
    {
      /* Replace absolute DATA DIRECTORY file paths with
      short names relative to the backup directory. */
      const char *name= strrchr(filename, '/');
#ifdef _WIN32
      if (const char *last= strrchr(filename, '\\'))
        if (last > name)
          name= last;
#endif
      if (name)
      {
        while (--name > filename &&
#ifdef _WIN32
               *name != '\\' &&
#endif
               *name != '/');
        if (name > filename)
          filename= name + 1;
       }
    }

    char *fil_path= fil_make_filepath(nullptr, {filename, strlen(filename)},
                                      IBD, false);
    const item defer{lsn, fil_path, false};
    ut_free(fil_path);

    /* The file name must be unique. Keep the one with the latest LSN. */
    auto d= defers.begin();

    while (d != defers.end())
    {
      if (d->second.file_name != defer.file_name)
        ++d;
      else if (d->first == space)
      {
        /* Neither the file name nor the tablespace ID changed.
        Update the LSN if needed. */
        if (d->second.lsn < lsn)
          d->second.lsn= lsn;
        return;
      }
      else if (d->second.lsn < lsn)
      {
        /* Reset the old tablespace name in recovered spaces list */
        recv_spaces_t::iterator it{recv_spaces.find(d->first)};
        if (it != recv_spaces.end() &&
            it->second.name == d->second.file_name)
          it->second.name = "";
        defers.erase(d++);
      }
      else
      {
        ut_ad(d->second.lsn != lsn);
        return; /* A later tablespace already has this name. */
      }
    }

    auto p= defers.emplace(space, defer);
    if (!p.second && p.first->second.lsn <= lsn)
    {
      p.first->second.lsn= lsn;
      p.first->second.file_name= defer.file_name;
    }
    /* Add the newly added defered space and change the file name */
    recv_spaces_t::iterator it{recv_spaces.find(space)};
    if (it != recv_spaces.end())
      it->second.name = defer.file_name;
  }

  void remove(uint32_t space)
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);
    defers.erase(space);
  }

  /** Look up a tablespace that was found corrupted during recovery.
  @param id   tablespace id
  @return tablespace whose creation was deferred
  @retval nullptr if no such tablespace was found */
  item *find(uint32_t id)
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);
    auto it= defers.find(id);
    if (it != defers.end())
      return &it->second;
    return nullptr;
  }

  void clear()
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);
    defers.clear();
  }

  /** Initialize all deferred tablespaces.
  @return whether any deferred initialization failed */
  bool reinit_all()
  {
retry:
    mysql_mutex_unlock(&log_sys.mutex);
    fil_space_t *space= fil_system.sys_space;
    buf_block_t *free_block= buf_LRU_get_free_block(false);
    mysql_mutex_lock(&log_sys.mutex);
    mysql_mutex_lock(&recv_sys.mutex);

    for (auto d= defers.begin(); d != defers.end(); )
    {
      const uint32_t space_id{d->first};
      recv_sys_t::map::iterator p{recv_sys.pages.lower_bound({space_id,0})};

      if (d->second.deleted ||
          p == recv_sys.pages.end() || p->first.space() != space_id)
      {
        /* We found a FILE_DELETE record for the tablespace, or
        there were no buffered records. Either way, we must create a
        dummy tablespace with the latest known name,
        for dict_drop_index_tree(). */
        recv_sys.pages_it_invalidate(space_id);
        while (p != recv_sys.pages.end() && p->first.space() == space_id)
        {
          ut_ad(!p->second.being_processed);
          recv_sys_t::map::iterator r= p++;
          recv_sys.erase(r);
        }
        recv_spaces_t::iterator it{recv_spaces.find(space_id)};
        if (it != recv_spaces.end())
        {
          const std::string *name= &d->second.file_name;
          if (d->second.deleted)
          {
            const auto r= renamed_spaces.find(space_id);
            if (r != renamed_spaces.end())
              name= &r->second;
            bool exists;
            os_file_type_t ftype;
            if (!os_file_status(name->c_str(), &exists, &ftype) || !exists)
              goto processed;
          }
          if (create(it, *name, static_cast<uint32_t>
                     (1U << FSP_FLAGS_FCRC32_POS_MARKER |
                      FSP_FLAGS_FCRC32_PAGE_SSIZE()), nullptr, 0))
            mysql_mutex_unlock(&fil_system.mutex);
        }
      }
      else
        space= recv_sys.recover_deferred(p, d->second.file_name, free_block);
processed:
      auto e= d++;
      defers.erase(e);
      if (!space)
        break;
      if (space != fil_system.sys_space)
        space->release();
      if (free_block)
        continue;
      mysql_mutex_unlock(&recv_sys.mutex);
      goto retry;
    }

    clear();
    mysql_mutex_unlock(&recv_sys.mutex);
    if (free_block)
      buf_pool.free_block(free_block);
    return !space;
  }

  /** Create tablespace metadata for a data file that was initially
  found corrupted during recovery.
  @param it         tablespace iterator
  @param name       latest file name
  @param flags      FSP_SPACE_FLAGS
  @param crypt_data encryption metadata
  @param size       tablespace size in pages
  @return tablespace; the caller must release fil_system.mutex
  @retval nullptr   if crypt_data is invalid */
  static fil_space_t *create(const recv_spaces_t::const_iterator &it,
                             const std::string &name, uint32_t flags,
                             fil_space_crypt_t *crypt_data, uint32_t size)
  {
    if (crypt_data && !fil_crypt_check(crypt_data, name.c_str()))
      return nullptr;
    mysql_mutex_lock(&fil_system.mutex);
    fil_space_t *space= fil_space_t::create(it->first, flags, false,
                                            crypt_data);
    ut_ad(space);
    const char *filename= name.c_str();
    if (srv_operation == SRV_OPERATION_RESTORE)
    {
      const char* tbl_name = strrchr(filename, '/');
#ifdef _WIN32
      if (const char *last = strrchr(filename, '\\'))
      {
        if (last > tbl_name)
          tbl_name = last;
      }
#endif
      if (tbl_name)
      {
        while (--tbl_name > filename &&
#ifdef _WIN32
               *tbl_name != '\\' &&
#endif
               *tbl_name != '/');
        if (tbl_name > filename)
          filename= tbl_name + 1;
      }
    }
    pfs_os_file_t handle= OS_FILE_CLOSED;
    if (srv_operation == SRV_OPERATION_RESTORE)
    {
      /* During mariadb-backup --backup, a table could be renamed,
      created and dropped, and we may be missing the file at this
      point of --prepare. Try to create the file if it does not exist
      already. If the file exists, we'll pass handle=OS_FILE_CLOSED
      and the file will be opened normally in fil_space_t::acquire()
      inside recv_sys_t::recover_deferred(). */
      bool success;
      handle= os_file_create(innodb_data_file_key, filename,
                             OS_FILE_CREATE | OS_FILE_ON_ERROR_NO_EXIT |
                             OS_FILE_ON_ERROR_SILENT,
                             OS_DATA_FILE, false, &success);
    }
    space->add(filename, handle, size, false, false);
    space->recv_size= it->second.size;
    space->size_in_header= size;
    return space;
  }

  /** Attempt to recover pages from the doublewrite buffer.
  This is invoked if we found neither a valid first page in the
  data file nor redo log records that would initialize the first
  page. */
  void deferred_dblwr(lsn_t max_lsn)
  {
    for (auto d= defers.begin(); d != defers.end(); )
    {
      if (d->second.deleted)
      {
      next_item:
        d++;
        continue;
      }
      const page_id_t page_id{d->first, 0};
      const byte *page= recv_sys.dblwr.find_page(page_id, max_lsn);
      if (!page)
        goto next_item;
      const uint32_t space_id= mach_read_from_4(page + FIL_PAGE_SPACE_ID);
      const uint32_t flags= fsp_header_get_flags(page);
      const uint32_t page_no= mach_read_from_4(page + FIL_PAGE_OFFSET);
      const uint32_t size= fsp_header_get_field(page, FSP_SIZE);

      if (page_no == 0 && space_id == d->first && size >= 4 &&
          fil_space_t::is_valid_flags(flags, space_id) &&
          fil_space_t::logical_size(flags) == srv_page_size)
      {
        recv_spaces_t::iterator it {recv_spaces.find(d->first)};
        ut_ad(it != recv_spaces.end());

        fil_space_t *space= create(
          it, d->second.file_name.c_str(), flags,
          fil_space_read_crypt_data(fil_space_t::zip_size(flags), page),
          size);

        if (!space)
          goto next_item;

        space->free_limit= fsp_header_get_field(page, FSP_FREE_LIMIT);
        space->free_len= flst_get_len(FSP_HEADER_OFFSET + FSP_FREE + page);
        fil_node_t *node= UT_LIST_GET_FIRST(space->chain);
        mysql_mutex_unlock(&fil_system.mutex);
        if (!space->acquire())
        {
free_space:
          fil_space_free(it->first, false);
          goto next_item;
        }
        if (os_file_write(IORequestWrite, node->name, node->handle,
                          page, 0, fil_space_t::physical_size(flags)) !=
            DB_SUCCESS)
        {
          space->release();
          goto free_space;
        }
        space->release();
        it->second.space= space;
        defers.erase(d++);
        continue;
      }
      goto next_item;
    }
  }
}
deferred_spaces;

/** Report an operation to create, delete, or rename a file during backup.
@param[in]	space_id	tablespace identifier
@param[in]	type		redo log type
@param[in]	name		file name (not NUL-terminated)
@param[in]	len		length of name, in bytes
@param[in]	new_name	new file name (NULL if not rename)
@param[in]	new_len		length of new_name, in bytes (0 if NULL) */
void (*log_file_op)(uint32_t space_id, int type,
		    const byte* name, size_t len,
		    const byte* new_name, size_t new_len);

void (*undo_space_trunc)(uint32_t space_id);

void (*first_page_init)(uint32_t space_id);

/** Information about initializing page contents during redo log processing.
FIXME: Rely on recv_sys.pages! */
class mlog_init_t
{
  using map= std::map<const page_id_t, recv_init,
                      std::less<const page_id_t>,
                      ut_allocator<std::pair<const page_id_t, recv_init>>>;
  /** Map of page initialization operations.
  FIXME: Merge this to recv_sys.pages! */
  map inits;

  /** Iterator to the last add() or will_avoid_read(), for speeding up
  will_avoid_read(). */
  map::iterator i;
public:
  /** Constructor */
  mlog_init_t() : i(inits.end()) {}

  /** Record that a page will be initialized by the redo log.
  @param page_id     page identifier
  @param lsn         log sequence number
  @return whether the state was changed */
  bool add(const page_id_t page_id, lsn_t lsn)
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);
    const recv_init init = { lsn, false };
    std::pair<map::iterator, bool> p=
      inits.insert(map::value_type(page_id, init));
    ut_ad(!p.first->second.created);
    if (p.second) return true;
    if (p.first->second.lsn >= lsn) return false;
    p.first->second = init;
    i = p.first;
    return true;
  }

  /** Get the last stored lsn of the page id and its respective
  init/load operation.
  @param page_id    page identifier
  @return the latest page initialization;
  not valid after releasing recv_sys.mutex. */
  recv_init &last(page_id_t page_id)
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);
    return inits.find(page_id)->second;
  }

  /** Determine if a page will be initialized or freed after a time.
  @param page_id      page identifier
  @param lsn          log sequence number
  @return whether page_id will be freed or initialized after lsn */
  bool will_avoid_read(page_id_t page_id, lsn_t lsn)
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);
    if (i != inits.end() && i->first == page_id)
      return i->second.lsn > lsn;
    i = inits.lower_bound(page_id);
    return i != inits.end() && i->first == page_id && i->second.lsn > lsn;
  }

  /** At the end of each recovery batch, reset the 'created' flags. */
  void reset()
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);
    ut_ad(recv_no_ibuf_operations);
    for (map::value_type &i : inits)
      i.second.created= false;
  }

  /** During the last recovery batch, mark whether there exist
  buffered changes for the pages that were initialized
  by buf_page_create() and still reside in the buffer pool. */
  void mark_ibuf_exist()
  {
    mysql_mutex_assert_owner(&recv_sys.mutex);

    for (const map::value_type &i : inits)
      if (i.second.created)
      {
        auto &chain= buf_pool.page_hash.cell_get(i.first.fold());
        page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);

        hash_lock.lock_shared();
        buf_block_t *block= reinterpret_cast<buf_block_t*>
          (buf_pool.page_hash.get(i.first, chain));
        bool got_latch= block && block->page.lock.x_lock_try();
        hash_lock.unlock_shared();

        if (!block)
          continue;

        uint32_t state;

        if (!got_latch)
        {
          mysql_mutex_lock(&buf_pool.mutex);
          block= reinterpret_cast<buf_block_t*>
            (buf_pool.page_hash.get(i.first, chain));
          if (!block)
          {
            mysql_mutex_unlock(&buf_pool.mutex);
            continue;
          }

          state= block->page.fix();
          mysql_mutex_unlock(&buf_pool.mutex);
          if (state < buf_page_t::UNFIXED)
          {
            block->page.unfix();
            continue;
          }
          block->page.lock.x_lock();
          state= block->page.unfix();
          ut_ad(state < buf_page_t::READ_FIX);
          if (state >= buf_page_t::UNFIXED && block->page.id() == i.first)
            goto check_ibuf;
        }
        else
        {
          state= block->page.state();
          ut_ad(state >= buf_page_t::FREED);
          ut_ad(state < buf_page_t::READ_FIX);

          if (state >= buf_page_t::UNFIXED)
          {
          check_ibuf:
            mysql_mutex_unlock(&recv_sys.mutex);
            if (ibuf_page_exists(block->page.id(), block->zip_size()))
              block->page.set_ibuf_exist();
            mysql_mutex_lock(&recv_sys.mutex);
          }
        }

        block->page.lock.x_unlock();
      }
  }

  /** Clear the data structure */
  void clear() { inits.clear(); i = inits.end(); }
};

static mlog_init_t mlog_init;

/** Try to recover a tablespace that was not readable earlier
@param p          iterator to the page
@param name       tablespace file name
@param free_block spare buffer block
@return recovered tablespace
@retval nullptr if recovery failed */
fil_space_t *recv_sys_t::recover_deferred(const recv_sys_t::map::iterator &p,
                                          const std::string &name,
                                          buf_block_t *&free_block)
{
  mysql_mutex_assert_owner(&mutex);

  ut_ad(p->first.space());

  recv_spaces_t::iterator it{recv_spaces.find(p->first.space())};
  ut_ad(it != recv_spaces.end());

  if (!p->first.page_no() && p->second.skip_read)
  {
    mtr_t mtr;
    ut_ad(!p->second.being_processed);
    p->second.being_processed= 1;
    init &init= mlog_init.last(p->first);
    mysql_mutex_unlock(&mutex);
    buf_block_t *block= recover_low(p, mtr, free_block, init);
    mysql_mutex_lock(&mutex);
    p->second.being_processed= -1;
    ut_ad(block == free_block || block == reinterpret_cast<buf_block_t*>(-1));
    free_block= nullptr;
    if (UNIV_UNLIKELY(!block || block == reinterpret_cast<buf_block_t*>(-1)))
      goto fail;
    const byte *page= UNIV_LIKELY_NULL(block->page.zip.data)
      ? block->page.zip.data
      : block->page.frame;
    const uint32_t space_id= mach_read_from_4(page + FIL_PAGE_SPACE_ID);
    const uint32_t flags= fsp_header_get_flags(page);
    const uint32_t page_no= mach_read_from_4(page + FIL_PAGE_OFFSET);
    const uint32_t size= fsp_header_get_field(page, FSP_SIZE);

    if (page_id_t{space_id, page_no} == p->first && size >= 4 &&
        fil_space_t::is_valid_flags(flags, space_id) &&
        fil_space_t::logical_size(flags) == srv_page_size)
    {
      fil_space_t *space= deferred_spaces.create(it, name, flags,
                                                 fil_space_read_crypt_data
                                                 (fil_space_t::zip_size(flags),
                                                  page), size);
      if (!space)
        goto release_and_fail;
      space->free_limit= fsp_header_get_field(page, FSP_FREE_LIMIT);
      space->free_len= flst_get_len(FSP_HEADER_OFFSET + FSP_FREE + page);
      fil_node_t *node= UT_LIST_GET_FIRST(space->chain);
      node->deferred= true;
      mysql_mutex_unlock(&fil_system.mutex);
      if (!space->acquire())
        goto release_and_fail;
      fil_names_dirty(space);
      const bool is_compressed= fil_space_t::is_compressed(flags);
#ifdef _WIN32
      const bool is_sparse= is_compressed;
      if (is_compressed)
        os_file_set_sparse_win32(node->handle);
#else
      const bool is_sparse= is_compressed &&
        DB_SUCCESS == os_file_punch_hole(node->handle, 0, 4096) &&
        !my_test_if_thinly_provisioned(node->handle);
#endif
      /* Mimic fil_node_t::read_page0() in case the file exists and
      has already been extended to a larger size. */
      ut_ad(node->size == size);
      const os_offset_t file_size= os_file_get_size(node->handle);
      if (file_size != os_offset_t(-1))
      {
        const uint32_t n_pages=
          uint32_t(file_size / fil_space_t::physical_size(flags));
        if (n_pages > size)
        {
          mysql_mutex_lock(&fil_system.mutex);
          space->size= node->size= n_pages;
          space->set_committed_size();
          mysql_mutex_unlock(&fil_system.mutex);
          goto size_set;
        }
      }
      if (!os_file_set_size(node->name, node->handle,
                            (size * fil_space_t::physical_size(flags)) &
                            ~4095ULL, is_sparse))
      {
        space->release();
        goto release_and_fail;
      }
    size_set:
      node->deferred= false;
      it->second.space= space;
      block->page.lock.x_unlock();
      p->second.being_processed= -1;
      return space;
    }

  release_and_fail:
    block->page.lock.x_unlock();
  }

fail:
  ib::error() << "Cannot apply log to " << p->first
              << " of corrupted file '" << name << "'";
  return nullptr;
}

/** Process a record that indicates that a tablespace is
being shrunk in size.
@param page_id	first page identifier that is not in the file
@param lsn	log sequence number of the shrink operation */
inline void recv_sys_t::trim(const page_id_t page_id, lsn_t lsn)
{
  DBUG_ENTER("recv_sys_t::trim");
  DBUG_LOG("ib_log", "discarding log beyond end of tablespace "
           << page_id << " before LSN " << lsn);
  mysql_mutex_assert_owner(&mutex);
  if (pages_it != pages.end() && pages_it->first.space() == page_id.space())
    pages_it= pages.end();
  for (recv_sys_t::map::iterator p = pages.lower_bound(page_id);
       p != pages.end() && p->first.space() == page_id.space();)
  {
    recv_sys_t::map::iterator r = p++;
    if (r->second.trim(lsn))
    {
      ut_ad(!r->second.being_processed);
      pages.erase(r);
    }
  }
  DBUG_VOID_RETURN;
}

void recv_sys_t::open_log_files_if_needed()
{
  if (!recv_sys.files.empty())
    return;

  for (auto &&path : get_existing_log_files_paths())
  {
    recv_sys.files.emplace_back(std::move(path));
    ut_a(recv_sys.files.back().open(true) == DB_SUCCESS);
  }
}

MY_ATTRIBUTE((warn_unused_result))
dberr_t recv_sys_t::read(os_offset_t total_offset, span<byte> buf)
{
  open_log_files_if_needed();

  size_t file_idx= static_cast<size_t>(total_offset / log_sys.log.file_size);
  os_offset_t offset= total_offset % log_sys.log.file_size;
  return recv_sys.files[file_idx].read(offset, buf);
}

inline size_t recv_sys_t::files_size()
{
  open_log_files_if_needed();
  return files.size();
}

/** Process a file name from a FILE_* record.
@param[in]	name		file name
@param[in]	len		length of the file name
@param[in]	space_id	the tablespace ID
@param[in]	ftype		FILE_MODIFY, FILE_DELETE, or FILE_RENAME
@param[in]	lsn		lsn of the redo log
@param[in]	store		whether the redo log has to be stored */
static void fil_name_process(const char *name, ulint len, uint32_t space_id,
                             mfile_type_t ftype, lsn_t lsn, store_t store)
{
	if (srv_operation == SRV_OPERATION_BACKUP
	    || srv_operation == SRV_OPERATION_BACKUP_NO_DEFER) {
		return;
	}

	ut_ad(srv_operation <= SRV_OPERATION_EXPORT_RESTORED
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	/* We will also insert space=NULL into the map, so that
	further checks can ensure that a FILE_MODIFY record was
	scanned before applying any page records for the space_id. */

	const bool deleted{ftype == FILE_DELETE};
	const file_name_t fname(std::string(name, len), deleted);
	std::pair<recv_spaces_t::iterator,bool> p = recv_spaces.emplace(
		space_id, fname);
	ut_ad(p.first->first == space_id);

	file_name_t&	f = p.first->second;

	auto d = deferred_spaces.find(space_id);
	if (d) {
		if (deleted) {
			d->deleted = true;
			goto got_deleted;
		}
		goto reload;
	}

	if (deleted) {
got_deleted:
		/* Got FILE_DELETE */
		if (!p.second && f.status != file_name_t::DELETED) {
			f.status = file_name_t::DELETED;
			if (f.space != NULL) {
				fil_space_free(space_id, false);
				f.space = NULL;
			}
		}

		ut_ad(f.space == NULL);
	} else if (p.second // the first FILE_MODIFY or FILE_RENAME
		   || f.name != fname.name) {
reload:
		fil_space_t*	space;

		/* Check if the tablespace file exists and contains
		the space_id. If not, ignore the file after displaying
		a note. Abort if there are multiple files with the
		same space_id. */
		switch (fil_ibd_load(space_id, fname.name.c_str(), space)) {
		case FIL_LOAD_OK:
			ut_ad(space != NULL);

			deferred_spaces.remove(space_id);
			if (!f.space) {
				if (f.size
				    || f.flags != f.initial_flags) {
					fil_space_set_recv_size_and_flags(
						space->id, f.size, f.flags);
				}

				f.space = space;
				goto same_space;
			} else if (f.space == space) {
same_space:
				f.name = fname.name;
				f.status = file_name_t::NORMAL;
			} else {
				ib::error() << "Tablespace " << space_id
					<< " has been found in two places: '"
					<< f.name << "' and '"
					<< fname.name << "'."
					" You must delete one of them.";
				recv_sys.set_corrupt_fs();
			}
			break;

		case FIL_LOAD_ID_CHANGED:
			ut_ad(space == NULL);
			break;

		case FIL_LOAD_NOT_FOUND:
			/* No matching tablespace was found; maybe it
			was renamed, and we will find a subsequent
			FILE_* record. */
			ut_ad(space == NULL);

			if (srv_operation == SRV_OPERATION_RESTORE && d
			    && ftype == FILE_RENAME) {
rename:
				d->file_name = fname.name;
				f.name = fname.name;
				break;
			}

			if (srv_force_recovery
			    || srv_operation == SRV_OPERATION_RESTORE) {
				/* Without innodb_force_recovery,
				missing tablespaces will only be
				reported in
				recv_init_crash_recovery_spaces().
				Enable some more diagnostics when
				forcing recovery. */
				sql_print_information("InnoDB: At LSN: " LSN_PF
						      ": unable to open file "
						      "%s for tablespace "
						      UINT32PF,
						      recv_sys.recovered_lsn,
						      fname.name.c_str(),
						      space_id);
			}
			break;

		case FIL_LOAD_DEFER:
			if (d && ftype == FILE_RENAME
			    && srv_operation == SRV_OPERATION_RESTORE) {
				goto rename;
			}
			/* Skip the deferred spaces
			when lsn is already processed */
			if (store != store_t::STORE_IF_EXISTS) {
				deferred_spaces.add(
					space_id, fname.name.c_str(), lsn);
			}
			break;
		case FIL_LOAD_INVALID:
			ut_ad(space == NULL);
			if (srv_force_recovery == 0) {
				sql_print_error("InnoDB: Recovery cannot access"
						" file %.*s (tablespace "
						UINT32PF ")", int(len), name,
						space_id);
				sql_print_information("InnoDB: You may set "
						      "innodb_force_recovery=1"
						      " to ignore this and"
						      " possibly get a"
						      " corrupted database.");
				recv_sys.set_corrupt_fs();
				break;
			}

			sql_print_warning("InnoDB: Ignoring changes to"
					  " file %.*s (tablespace "
					  UINT32PF ")"
					  " due to innodb_force_recovery",
					  int(len), name, space_id);
		}
	}
}

/** Clean up after recv_sys_t::create() */
void recv_sys_t::close()
{
  ut_ad(this == &recv_sys);

  if (is_initialised())
  {
    dblwr.pages.clear();
    ut_d(mysql_mutex_lock(&mutex));
    clear();
    deferred_spaces.clear();
    ut_d(mysql_mutex_unlock(&mutex));

    if (buf)
    {
      ut_free_dodump(buf, RECV_PARSING_BUF_SIZE);
      buf= nullptr;
    }

    last_stored_lsn= 0;
    mysql_mutex_destroy(&mutex);
  }

  recv_spaces.clear();
  renamed_spaces.clear();
  mlog_init.clear();
  close_files();
}

/** Initialize the redo log recovery subsystem. */
void recv_sys_t::create()
{
	ut_ad(this == &recv_sys);
	ut_ad(!is_initialised());
	mysql_mutex_init(recv_sys_mutex_key, &mutex, nullptr);

	apply_log_recs = false;

	buf = static_cast<byte*>(ut_malloc_dontdump(RECV_PARSING_BUF_SIZE,
						    PSI_INSTRUMENT_ME));
	len = 0;
	parse_start_lsn = 0;
	scanned_lsn = 0;
	scanned_checkpoint_no = 0;
	recovered_offset = 0;
	recovered_lsn = 0;
	found_corrupt_log = false;
	found_corrupt_fs = false;
	mlog_checkpoint_lsn = 0;

	progress_time = time(NULL);
	ut_ad(pages.empty());
	pages_it = pages.end();

	memset(truncated_undo_spaces, 0, sizeof truncated_undo_spaces);
	last_stored_lsn = 1;
	UT_LIST_INIT(blocks, &buf_block_t::unzip_LRU);
}

/** Clear a fully processed set of stored redo log records. */
void recv_sys_t::clear()
{
  mysql_mutex_assert_owner(&mutex);
  apply_log_recs= false;
  ut_ad(!after_apply || found_corrupt_fs || !UT_LIST_GET_LAST(blocks));
  pages.clear();
  pages_it= pages.end();

  for (buf_block_t *block= UT_LIST_GET_LAST(blocks); block; )
  {
    buf_block_t *prev_block= UT_LIST_GET_PREV(unzip_LRU, block);
    ut_ad(block->page.state() == buf_page_t::MEMORY);
    block->page.hash= nullptr;
    UT_LIST_REMOVE(blocks, block);
    MEM_MAKE_ADDRESSABLE(block->page.frame, srv_page_size);
    buf_block_free(block);
    block= prev_block;
  }
}

/** Free most recovery data structures. */
void recv_sys_t::debug_free()
{
  ut_ad(this == &recv_sys);
  ut_ad(is_initialised());
  mysql_mutex_lock(&mutex);

  recovery_on= false;
  pages.clear();
  pages_it= pages.end();
  ut_free_dodump(buf, RECV_PARSING_BUF_SIZE);

  buf= nullptr;

  mysql_mutex_unlock(&mutex);
}


/** Free a redo log snippet.
@param data buffer allocated in add() */
inline void recv_sys_t::free(const void *data)
{
  ut_ad(!ut_align_offset(data, ALIGNMENT));
  mysql_mutex_assert_owner(&mutex);

  /* MDEV-14481 FIXME: To prevent race condition with buf_pool.resize(),
  we must acquire and hold the buffer pool mutex here. */
  ut_ad(!buf_pool.resize_in_progress());

  auto *chunk= buf_pool.chunks;
  for (auto i= buf_pool.n_chunks; i--; chunk++)
  {
    if (data < chunk->blocks->page.frame)
      continue;
    const size_t offs= (reinterpret_cast<const byte*>(data) -
                        chunk->blocks->page.frame) >> srv_page_size_shift;
    if (offs >= chunk->size)
      continue;
    buf_block_t *block= &chunk->blocks[offs];
    ut_ad(block->page.frame == page_align(data));
    ut_ad(block->page.state() == buf_page_t::MEMORY);
    ut_ad(uint16_t(block->page.free_offset - 1) < srv_page_size);
    ut_ad(block->page.used_records);
    if (!--block->page.used_records)
    {
      block->page.hash= nullptr;
      UT_LIST_REMOVE(blocks, block);
      MEM_MAKE_ADDRESSABLE(block->page.frame, srv_page_size);
      buf_block_free(block);
    }
    return;
  }
  ut_ad(0);
}


/** Read a log segment to log_sys.buf.
@param[in,out]	start_lsn	in: read area start,
out: the last read valid lsn
@param[in]	end_lsn		read area end
@return	whether no invalid blocks (e.g checksum mismatch) were found */
bool log_t::file::read_log_seg(lsn_t* start_lsn, lsn_t end_lsn)
{
	ulint	len;
	bool success = true;
	mysql_mutex_assert_owner(&log_sys.mutex);
	ut_ad(!(*start_lsn % OS_FILE_LOG_BLOCK_SIZE));
	ut_ad(!(end_lsn % OS_FILE_LOG_BLOCK_SIZE));
	byte* buf = log_sys.buf;
loop:
	lsn_t source_offset = calc_lsn_offset_old(*start_lsn);

	ut_a(end_lsn - *start_lsn <= ULINT_MAX);
	len = (ulint) (end_lsn - *start_lsn);

	ut_ad(len != 0);

	const bool at_eof = (source_offset % file_size) + len > file_size;
	if (at_eof) {
		/* If the above condition is true then len (which is ulint)
		is > the expression below, so the typecast is ok */
		len = ulint(file_size - (source_offset % file_size));
	}

	log_sys.n_log_ios++;

	ut_a((source_offset >> srv_page_size_shift) <= ULINT_MAX);

	if (recv_sys.read(source_offset, {buf, len}))
		return false;

	for (ulint l = 0; l < len; l += OS_FILE_LOG_BLOCK_SIZE,
		     buf += OS_FILE_LOG_BLOCK_SIZE,
		     (*start_lsn) += OS_FILE_LOG_BLOCK_SIZE) {
		const ulint block_number = log_block_get_hdr_no(buf);

		if (block_number != log_block_convert_lsn_to_no(*start_lsn)) {
			/* Garbage or an incompletely written log block.
			We will not report any error, because this can
			happen when InnoDB was killed while it was
			writing redo log. We simply treat this as an
			abrupt end of the redo log. */
fail:
			end_lsn = *start_lsn;
			success = false;
			break;
		}

		ulint crc = log_block_calc_checksum_crc32(buf);
		ulint cksum = log_block_get_checksum(buf);

		DBUG_EXECUTE_IF("log_intermittent_checksum_mismatch", {
				static int block_counter;
				if (block_counter++ == 0) {
					cksum = crc + 1;
				}
			});

		DBUG_EXECUTE_IF("log_checksum_mismatch", { cksum = crc + 1; });

		if (UNIV_UNLIKELY(crc != cksum)) {
			ib::error_or_warn(srv_operation!=SRV_OPERATION_BACKUP)
				<< "Invalid log block checksum. block: "
				<< block_number
				<< " checkpoint no: "
				<< log_block_get_checkpoint_no(buf)
				<< " expected: " << crc
				<< " found: " << cksum;
			goto fail;
		}

		if (is_encrypted()
		    && !log_crypt(buf, *start_lsn,
				  OS_FILE_LOG_BLOCK_SIZE,
				  LOG_DECRYPT)) {
			goto fail;
		}

		ulint dl = log_block_get_data_len(buf);
		if (dl < LOG_BLOCK_HDR_SIZE
		    || (dl != OS_FILE_LOG_BLOCK_SIZE
			&& dl > log_sys.trailer_offset())) {
			recv_sys.set_corrupt_log();
			goto fail;
		}
	}

	if (recv_sys.report(time(NULL))) {
		ib::info() << "Read redo log up to LSN=" << *start_lsn;
		service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
			"Read redo log up to LSN=" LSN_PF,
			*start_lsn);
	}

	if (*start_lsn != end_lsn) {
		goto loop;
	}

	return(success);
}



/********************************************************//**
Copies a log segment from the most up-to-date log group to the other log
groups, so that they all contain the latest log data. Also writes the info
about the latest checkpoint to the groups, and inits the fields in the group
memory structs to up-to-date values. */
static
void
recv_synchronize_groups()
{
	const lsn_t recovered_lsn = recv_sys.recovered_lsn;

	/* Read the last recovered log block to the recovery system buffer:
	the block is always incomplete */

	lsn_t start_lsn = ut_uint64_align_down(recovered_lsn,
					       OS_FILE_LOG_BLOCK_SIZE);
	log_sys.log.read_log_seg(&start_lsn,
				 start_lsn + OS_FILE_LOG_BLOCK_SIZE);
	log_sys.log.set_fields(recovered_lsn);

	/* Copy the checkpoint info to the log; remember that we have
	incremented checkpoint_no by one, and the info will not be written
	over the max checkpoint info, thus making the preservation of max
	checkpoint info on disk certain */

	if (!srv_read_only_mode) {
		log_write_checkpoint_info(0);
		mysql_mutex_lock(&log_sys.mutex);
	}
}

/** Check the consistency of a log header block.
@param[in]	log header block
@return true if ok */
static
bool
recv_check_log_header_checksum(
	const byte*	buf)
{
	return(log_block_get_checksum(buf)
	       == log_block_calc_checksum_crc32(buf));
}

static bool redo_file_sizes_are_correct()
{
  auto paths= get_existing_log_files_paths();
  auto get_size= [](const std::string &path) {
    return os_file_get_size(path.c_str()).m_total_size;
  };
  os_offset_t size= get_size(paths[0]);

  auto it=
      std::find_if(paths.begin(), paths.end(), [&](const std::string &path) {
        return get_size(path) != size;
      });

  if (it == paths.end())
    return true;

  ib::error() << "Log file " << *it << " is of different size "
              << get_size(*it) << " bytes than other log files " << size
              << " bytes!";
  return false;
}

/** Calculate the checksum for a log block using the pre-10.2.2 algorithm. */
inline uint32_t log_block_calc_checksum_format_0(const byte *b)
{
  uint32_t sum= 1;
  const byte *const end= &b[512 - 4];

  for (uint32_t sh= 0; b < end; )
  {
    sum&= 0x7FFFFFFFUL;
    sum+= uint32_t{*b} << sh++;
    sum+= *b++;
    if (sh > 24)
      sh= 0;
  }

  return sum;
}

/** Determine if a redo log from before MariaDB 10.2.2 is clean.
@return error code
@retval DB_SUCCESS      if the redo log is clean
@retval DB_CORRUPTION   if the redo log is corrupted
@retval DB_ERROR        if the redo log is not empty */
ATTRIBUTE_COLD static dberr_t recv_log_recover_pre_10_2()
{
  uint64_t max_no= 0;
  byte *buf= log_sys.buf;

  ut_ad(log_sys.log.format == 0);

  if (!redo_file_sizes_are_correct())
    return DB_CORRUPTION;

  /** Offset of the first checkpoint checksum */
  constexpr uint CHECKSUM_1= 288;
  /** Offset of the second checkpoint checksum */
  constexpr uint CHECKSUM_2= CHECKSUM_1 + 4;
  /** the checkpoint LSN field */
  constexpr uint CHECKPOINT_LSN= 8;
  /** Most significant bits of the checkpoint offset */
  constexpr uint OFFS_HI= CHECKSUM_2 + 12;
  /** Least significant bits of the checkpoint offset */
  constexpr uint OFFS_LO= 16;

  lsn_t lsn= 0;

  for (ulint field= LOG_CHECKPOINT_1; field <= LOG_CHECKPOINT_2;
       field += LOG_CHECKPOINT_2 - LOG_CHECKPOINT_1)
  {
    if (dberr_t err= log_sys.log.read(field, {buf, OS_FILE_LOG_BLOCK_SIZE}))
      return err;

    if (static_cast<uint32_t>(ut_fold_binary(buf, CHECKSUM_1)) !=
        mach_read_from_4(buf + CHECKSUM_1) ||
        static_cast<uint32_t>(ut_fold_binary(buf + CHECKPOINT_LSN,
                                             CHECKSUM_2 - CHECKPOINT_LSN)) !=
        mach_read_from_4(buf + CHECKSUM_2))
     {
       DBUG_LOG("ib_log", "invalid pre-10.2.2 checkpoint " << field);
       continue;
     }

    if (!log_crypt_101_read_checkpoint(buf))
    {
      sql_print_error("InnoDB: Decrypting checkpoint failed");
      continue;
    }

    const uint64_t checkpoint_no= mach_read_from_8(buf);

    DBUG_PRINT("ib_log", ("checkpoint " UINT64PF " at " LSN_PF " found",
                          checkpoint_no,
                          mach_read_from_8(buf + CHECKPOINT_LSN)));

    if (checkpoint_no >= max_no)
    {
      max_no= checkpoint_no;
      lsn= mach_read_from_8(buf + CHECKPOINT_LSN);
      log_sys.log.set_lsn(lsn);
      log_sys.log.set_lsn_offset(lsn_t{mach_read_from_4(buf + OFFS_HI)} << 32 |
                                 mach_read_from_4(buf + OFFS_LO));
    }
  }

  if (!lsn)
  {
    sql_print_error("InnoDB: Upgrade after a crash is not supported."
                    " This redo log was created before MariaDB 10.2.2,"
                    " and we did not find a valid checkpoint."
                    " Please follow the instructions at"
                    " https://mariadb.com/kb/en/library/upgrading/");
    return DB_ERROR;
  }

  log_sys.set_lsn(lsn);
  log_sys.set_flushed_lsn(lsn);
  const lsn_t source_offset= log_sys.log.calc_lsn_offset_old(lsn);

  static constexpr char NO_UPGRADE_RECOVERY_MSG[]=
    "InnoDB: Upgrade after a crash is not supported."
    " This redo log was created before MariaDB 10.2.2";

  if (dberr_t err= recv_sys.read(source_offset & ~511, {buf, 512}))
    return err;

  if (log_block_calc_checksum_format_0(buf) != log_block_get_checksum(buf) &&
      !log_crypt_101_read_block(buf, lsn))
  {
    sql_print_error("%s, and it appears corrupted.", NO_UPGRADE_RECOVERY_MSG);
    return DB_CORRUPTION;
  }

  if (mach_read_from_2(buf + 4) == (source_offset & 511))
  {
    /* Mark the redo log for upgrading. */
    srv_log_file_size= 0;
    recv_sys.parse_start_lsn= recv_sys.recovered_lsn= recv_sys.scanned_lsn=
      recv_sys.mlog_checkpoint_lsn = lsn;
    log_sys.last_checkpoint_lsn= log_sys.next_checkpoint_lsn=
      log_sys.write_lsn= log_sys.current_flush_lsn= lsn;
    log_sys.next_checkpoint_no= 0;
    return DB_SUCCESS;
  }

  if (buf[20 + 32 * 9] == 2)
    sql_print_error("InnoDB: Cannot decrypt log for upgrading."
                    " The encrypted log was created before MariaDB 10.2.2.");
  else
    sql_print_error("%s. You must start up and shut down"
                    " MariaDB 10.1 or MySQL 5.6 or earlier"
                    " on the data directory.",
                    NO_UPGRADE_RECOVERY_MSG);

  return DB_ERROR;
}

/** Calculate the offset of a log sequence number
in an old redo log file (during upgrade check).
@param[in]	lsn	log sequence number
@return byte offset within the log */
inline lsn_t log_t::file::calc_lsn_offset_old(lsn_t lsn) const
{
  const lsn_t size= capacity() * recv_sys.files_size();
  lsn_t l= lsn - this->lsn;
  if (longlong(l) < 0)
  {
    l= lsn_t(-longlong(l)) % size;
    l= size - l;
  }

  l+= lsn_offset - LOG_FILE_HDR_SIZE * (1 + lsn_offset / file_size);
  l%= size;
  return l + LOG_FILE_HDR_SIZE * (1 + l / (file_size - LOG_FILE_HDR_SIZE));
}

/** Determine if a redo log from MariaDB 10.2.2+, 10.3, or 10.4 is clean.
@return	error code
@retval	DB_SUCCESS	if the redo log is clean
@retval	DB_CORRUPTION	if the redo log is corrupted
@retval	DB_ERROR	if the redo log is not empty */
static dberr_t recv_log_recover_10_4()
{
	const lsn_t	lsn = log_sys.log.get_lsn();
	const lsn_t	source_offset =	log_sys.log.calc_lsn_offset_old(lsn);
	byte*		buf = log_sys.buf;

	if (!redo_file_sizes_are_correct()) {
		return DB_CORRUPTION;
	}

	if (dberr_t err=
	    recv_sys.read(source_offset & ~lsn_t(OS_FILE_LOG_BLOCK_SIZE - 1),
		      {buf, OS_FILE_LOG_BLOCK_SIZE}))
		return err;

	ulint crc = log_block_calc_checksum_crc32(buf);
	ulint cksum = log_block_get_checksum(buf);

	if (UNIV_UNLIKELY(crc != cksum)) {
		ib::error() << "Invalid log block checksum."
			    << " block: "
			    << log_block_get_hdr_no(buf)
			    << " checkpoint no: "
			    << log_block_get_checkpoint_no(buf)
			    << " expected: " << crc
			    << " found: " << cksum;
		return DB_CORRUPTION;
	}

	if (log_sys.log.is_encrypted()
	    && !log_crypt(buf, lsn & ~511, 512, LOG_DECRYPT)) {
		return DB_ERROR;
	}

	/* On a clean shutdown, the redo log will be logically empty
	after the checkpoint lsn. */

	if (log_block_get_data_len(buf)
	    != (source_offset & (OS_FILE_LOG_BLOCK_SIZE - 1))) {
		return DB_ERROR;
	}

	/* Mark the redo log for upgrading. */
	srv_log_file_size = 0;
	recv_sys.parse_start_lsn = recv_sys.recovered_lsn
		= recv_sys.scanned_lsn
		= recv_sys.mlog_checkpoint_lsn = lsn;
	log_sys.set_lsn(lsn);
	log_sys.set_flushed_lsn(lsn);
	log_sys.last_checkpoint_lsn = log_sys.next_checkpoint_lsn
		= log_sys.write_lsn = log_sys.current_flush_lsn = lsn;
	log_sys.next_checkpoint_no = 0;
	return DB_SUCCESS;
}

/** Find the latest checkpoint in the log header.
@param[out]	max_field	LOG_CHECKPOINT_1 or LOG_CHECKPOINT_2
@return error code or DB_SUCCESS */
dberr_t
recv_find_max_checkpoint(ulint* max_field)
{
	ib_uint64_t	max_no;
	ib_uint64_t	checkpoint_no;
	ulint		field;
	byte*		buf;

	max_no = 0;
	*max_field = 0;

	buf = log_sys.checkpoint_buf;

	if (dberr_t err= log_sys.log.read(0, {buf, OS_FILE_LOG_BLOCK_SIZE}))
		return err;
	/* Check the header page checksum. There was no
	checksum in the first redo log format (version 0). */
	log_sys.log.format = mach_read_from_4(buf + LOG_HEADER_FORMAT);
	log_sys.log.subformat = log_sys.log.format != log_t::FORMAT_3_23
		? mach_read_from_4(buf + LOG_HEADER_SUBFORMAT)
		: 0;
	if (log_sys.log.format != log_t::FORMAT_3_23
	    && !recv_check_log_header_checksum(buf)) {
		sql_print_error("InnoDB: Invalid redo log header checksum.");
		return DB_CORRUPTION;
	}

	char creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR + 1];

	memcpy(creator, buf + LOG_HEADER_CREATOR, sizeof creator);
	/* Ensure that the string is NUL-terminated. */
	creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR] = 0;

	switch (log_sys.log.format) {
	case log_t::FORMAT_3_23:
		return recv_log_recover_pre_10_2();
	case log_t::FORMAT_10_2:
	case log_t::FORMAT_10_2 | log_t::FORMAT_ENCRYPTED:
	case log_t::FORMAT_10_3:
	case log_t::FORMAT_10_3 | log_t::FORMAT_ENCRYPTED:
	case log_t::FORMAT_10_4:
	case log_t::FORMAT_10_4 | log_t::FORMAT_ENCRYPTED:
	case log_t::FORMAT_10_5:
	case log_t::FORMAT_10_5 | log_t::FORMAT_ENCRYPTED:
		break;
	default:
		sql_print_error("InnoDB: Unsupported redo log format."
				" The redo log was created with %s.", creator);
		return DB_ERROR;
	}

	for (field = LOG_CHECKPOINT_1; field <= LOG_CHECKPOINT_2;
	     field += LOG_CHECKPOINT_2 - LOG_CHECKPOINT_1) {
		if (dberr_t err= log_sys.log.read(field, {buf, OS_FILE_LOG_BLOCK_SIZE}))
			return err;

		const ulint crc32 = log_block_calc_checksum_crc32(buf);
		const ulint cksum = log_block_get_checksum(buf);

		if (crc32 != cksum) {
			DBUG_PRINT("ib_log",
				   ("invalid checkpoint,"
				    " at " ULINTPF
				    ", checksum " ULINTPFx
				    " expected " ULINTPFx,
				    field, cksum, crc32));
			continue;
		}

		if (log_sys.is_encrypted()
		    && !log_crypt_read_checkpoint_buf(buf)) {
			sql_print_error("InnoDB: Reading checkpoint"
					" encryption info failed.");
			continue;
		}

		checkpoint_no = mach_read_from_8(
			buf + LOG_CHECKPOINT_NO);

		DBUG_PRINT("ib_log",
			   ("checkpoint " UINT64PF " at " LSN_PF " found",
			    checkpoint_no, mach_read_from_8(
				    buf + LOG_CHECKPOINT_LSN)));

		if (checkpoint_no >= max_no) {
			*max_field = field;
			max_no = checkpoint_no;
			log_sys.log.set_lsn(mach_read_from_8(
				buf + LOG_CHECKPOINT_LSN));
			log_sys.log.set_lsn_offset(mach_read_from_8(
				buf + LOG_CHECKPOINT_OFFSET));
			log_sys.next_checkpoint_no = checkpoint_no;
		}
	}

	if (*max_field == 0) {
		/* Before 10.2.2, we could get here during database
		initialization if we created an LOG_FILE_NAME file that
		was filled with zeroes, and were killed. After
		10.2.2, we would reject such a file already earlier,
		when checking the file header. */
		sql_print_error("InnoDB: No valid checkpoint found"
				" (corrupted redo log)."
				" You can try --innodb-force-recovery=6"
				" as a last resort.");
		return DB_ERROR;
	}

	switch (log_sys.log.format) {
	case log_t::FORMAT_10_5:
	case log_t::FORMAT_10_5 | log_t::FORMAT_ENCRYPTED:
		break;
	default:
		if (dberr_t err = recv_log_recover_10_4()) {
			sql_print_error("InnoDB: Upgrade after a crash "
					"is not supported."
					" The redo log was created with %s%s.",
					creator,
					err == DB_ERROR
					? ". You must start up and shut down"
					" MariaDB 10.4 or earlier"
					" on the data directory"
					: ", and it appears corrupted");
			return err;
		}
	}

	return(DB_SUCCESS);
}

/*******************************************************//**
Calculates the new value for lsn when more data is added to the log. */
static
lsn_t
recv_calc_lsn_on_data_add(
/*======================*/
	lsn_t		lsn,	/*!< in: old lsn */
	ib_uint64_t	len)	/*!< in: this many bytes of data is
				added, log block headers not included */
{
	unsigned frag_len = static_cast<unsigned>(lsn % OS_FILE_LOG_BLOCK_SIZE)
		- LOG_BLOCK_HDR_SIZE;
	unsigned payload_size = log_sys.payload_size();
	ut_ad(frag_len < payload_size);
	lsn_t lsn_len = len;
	lsn_len += (lsn_len + frag_len) / payload_size
		* (OS_FILE_LOG_BLOCK_SIZE - payload_size);

	return(lsn + lsn_len);
}

/** Trim old log records for a page.
@param start_lsn oldest log sequence number to preserve
@return whether all the log for the page was trimmed */
inline bool page_recv_t::trim(lsn_t start_lsn)
{
  while (log.head)
  {
    if (log.head->lsn > start_lsn) return false;
    last_offset= 1; /* the next record must not be same_page */
    log_rec_t *next= log.head->next;
    recv_sys.free(log.head);
    log.head= next;
  }
  log.tail= nullptr;
  return true;
}


void page_recv_t::recs_t::rewind(lsn_t start_lsn)
{
  mysql_mutex_assert_owner(&recv_sys.mutex);
  log_phys_t *trim= static_cast<log_phys_t*>(head);
  ut_ad(trim);
  while (log_phys_t *next= static_cast<log_phys_t*>(trim->next))
  {
    ut_ad(trim->start_lsn < start_lsn);
    if (next->start_lsn == start_lsn)
      break;
    trim= next;
  }
  tail= trim;
  log_rec_t *l= tail->next;
  tail->next= nullptr;
  while (l)
  {
    log_rec_t *next= l->next;
    recv_sys.free(l);
    l= next;
  }
}


void page_recv_t::recs_t::clear()
{
  mysql_mutex_assert_owner(&recv_sys.mutex);
  for (const log_rec_t *l= head; l; )
  {
    const log_rec_t *next= l->next;
    recv_sys.free(l);
    l= next;
  }
  head= tail= nullptr;
}

/** Ignore any earlier redo log records for this page. */
inline void page_recv_t::will_not_read()
{
  ut_ad(!being_processed);
  skip_read= true;
  log.clear();
}

void recv_sys_t::erase(map::iterator p)
{
  ut_ad(p->second.being_processed <= 0);
  p->second.log.clear();
  pages.erase(p);
}

/** Free log for processed pages. */
void recv_sys_t::garbage_collect()
{
  mysql_mutex_assert_owner(&mutex);

  if (pages_it != pages.end() && pages_it->second.being_processed < 0)
    pages_it= pages.end();

  for (map::iterator p= pages.begin(); p != pages.end(); )
  {
    if (p->second.being_processed < 0)
    {
      map::iterator r= p++;
      erase(r);
    }
    else
      p++;
  }
}

/** Allocate a block from the buffer pool for recv_sys.pages */
ATTRIBUTE_COLD buf_block_t *recv_sys_t::add_block()
{
  for (bool freed= false;;)
  {
    const auto rs= UT_LIST_GET_LEN(blocks) * 2;
    mysql_mutex_lock(&buf_pool.mutex);
    const auto bs=
      UT_LIST_GET_LEN(buf_pool.free) + UT_LIST_GET_LEN(buf_pool.LRU);
    if (UNIV_LIKELY(bs > BUF_LRU_MIN_LEN || rs < bs))
    {
      buf_block_t *block= buf_LRU_get_free_block(true);
      mysql_mutex_unlock(&buf_pool.mutex);
      return block;
    }
    /* out of memory: redo log occupies more than 1/3 of buf_pool
    and there are fewer than BUF_LRU_MIN_LEN pages left */
    mysql_mutex_unlock(&buf_pool.mutex);
    if (freed)
      return nullptr;
    freed= true;
    garbage_collect();
  }
}

/** Wait for buffer pool to become available. */
ATTRIBUTE_COLD void recv_sys_t::wait_for_pool(size_t pages)
{
  mysql_mutex_unlock(&mutex);
  os_aio_wait_until_no_pending_reads(false);
  mysql_mutex_lock(&mutex);
  garbage_collect();
  mysql_mutex_lock(&buf_pool.mutex);
  bool need_more= UT_LIST_GET_LEN(buf_pool.free) < pages;
  mysql_mutex_unlock(&buf_pool.mutex);
  if (need_more)
    buf_flush_sync_batch(recovered_lsn);
}

/** Register a redo log snippet for a page.
@param it       page iterator
@param start_lsn start LSN of the mini-transaction
@param lsn      @see mtr_t::commit_lsn()
@param l        redo log snippet
@param len      length of l, in bytes
@return whether we ran out of memory */
ATTRIBUTE_NOINLINE
bool recv_sys_t::add(map::iterator it, lsn_t start_lsn, lsn_t lsn,
                     const byte *l, size_t len)
{
  mysql_mutex_assert_owner(&mutex);
  page_recv_t &recs= it->second;
  buf_block_t *block;

  switch (*l & 0x70) {
  case FREE_PAGE: case INIT_PAGE:
    recs.will_not_read();
    mlog_init.add(it->first, start_lsn); /* FIXME: remove this! */
    /* fall through */
  default:
    log_phys_t *tail= static_cast<log_phys_t*>(recs.log.last());
    if (!tail)
      break;
    if (tail->start_lsn != start_lsn)
      break;
    ut_ad(tail->lsn == lsn);
    block= UT_LIST_GET_LAST(blocks);
    ut_ad(block);
    const size_t used= uint16_t(block->page.free_offset - 1) + 1;
    ut_ad(used >= ALIGNMENT);
    const byte *end= const_cast<const log_phys_t*>(tail)->end();
    if (!((reinterpret_cast<size_t>(end + len) ^
           reinterpret_cast<size_t>(end)) & ~(ALIGNMENT - 1)))
    {
      /* Use already allocated 'padding' bytes */
append:
      MEM_MAKE_ADDRESSABLE(end + 1, len);
      /* Append to the preceding record for the page */
      tail->append(l, len);
      return false;
    }
    if (end <= &block->page.frame[used - ALIGNMENT] ||
        &block->page.frame[used] >= end)
      break; /* Not the last allocated record in the page */
    const size_t new_used= static_cast<size_t>
      (end - block->page.frame + len + 1);
    ut_ad(new_used > used);
    if (new_used > srv_page_size)
      break;
    block->page.free_offset=
      ut_calc_align<uint16_t>(static_cast<uint16_t>(new_used), ALIGNMENT);
    goto append;
  }

  const size_t size{log_phys_t::alloc_size(len)};
  ut_ad(size <= srv_page_size);
  void *buf;
  block= UT_LIST_GET_FIRST(blocks);
  if (UNIV_UNLIKELY(!block))
  {
  create_block:
    block= add_block();
    if (UNIV_UNLIKELY(!block))
      return true;
    block->page.used_records= 1;
    block->page.free_offset=
      ut_calc_align<uint16_t>(static_cast<uint16_t>(size), ALIGNMENT);
    static_assert(ut_is_2pow(ALIGNMENT), "ALIGNMENT must be a power of 2");
    UT_LIST_ADD_FIRST(blocks, block);
    MEM_MAKE_ADDRESSABLE(block->page.frame, size);
    MEM_NOACCESS(block->page.frame + size, srv_page_size - size);
    buf= block->page.frame;
  }
  else
  {
    size_t free_offset= block->page.free_offset;
    ut_ad(!ut_2pow_remainder(free_offset, ALIGNMENT));
    if (UNIV_UNLIKELY(!free_offset))
    {
      ut_ad(srv_page_size == 65536);
      goto create_block;
    }
    ut_ad(free_offset <= srv_page_size);
    free_offset+= size;

    if (free_offset > srv_page_size)
      goto create_block;

    block->page.used_records++;
    block->page.free_offset=
      ut_calc_align<uint16_t>(static_cast<uint16_t>(free_offset), ALIGNMENT);
    MEM_MAKE_ADDRESSABLE(block->page.frame + free_offset - size, size);
    buf= block->page.frame + free_offset - size;
  }

  recs.log.append(new (my_assume_aligned<ALIGNMENT>(buf))
                  log_phys_t{start_lsn, lsn, l, len});
  return false;
}

/** Store/remove the freed pages in fil_name_t of recv_spaces.
@param[in]	page_id		freed or init page_id
@param[in]	freed		TRUE if page is freed */
static void store_freed_or_init_rec(page_id_t page_id, bool freed)
{
  uint32_t space_id= page_id.space();
  uint32_t page_no= page_id.page_no();
  if (!freed && page_no == 0 && first_page_init)
    first_page_init(space_id);
  if (is_predefined_tablespace(space_id))
  {
    if (!srv_immediate_scrub_data_uncompressed)
      return;
    fil_space_t *space;
    if (space_id == TRX_SYS_SPACE)
      space= fil_system.sys_space;
    else
      space= fil_space_get(space_id);

    space->free_page(page_no, freed);
    return;
  }

  recv_spaces_t::iterator i= recv_spaces.lower_bound(space_id);
  if (i != recv_spaces.end() && i->first == space_id)
  {
    if (freed)
      i->second.add_freed_page(page_no);
    else
      i->second.remove_freed_page(page_no);
  }
}

ATTRIBUTE_COLD
void recv_sys_t::rewind(const byte *end, const byte *begin) noexcept
{
  ut_ad(srv_operation != SRV_OPERATION_BACKUP);
  mysql_mutex_assert_owner(&mutex);

  uint32_t rlen;
  for (const byte *l= begin; !(l == end); l+= rlen)
  {
    const byte b= *l++;
    ut_ad(UNIV_LIKELY((b & 0x70) != RESERVED) || srv_force_recovery);

    rlen= b & 0xf;
    if (!rlen)
    {
      if (!b)
        continue;
      const uint32_t lenlen= mlog_decode_varint_length(*l);
      const uint32_t addlen= mlog_decode_varint(l);
      ut_ad(addlen != MLOG_DECODE_ERROR);
      rlen= addlen + 15 - lenlen;
      l+= lenlen;
    }
    ut_ad(l + rlen <= end);
    if (b & 0x80)
      continue;

    uint32_t idlen= mlog_decode_varint_length(*l);
    if (UNIV_UNLIKELY(idlen > 5 || idlen >= rlen))
      continue;
    const uint32_t space_id= mlog_decode_varint(l);
    if (UNIV_UNLIKELY(space_id == MLOG_DECODE_ERROR))
      continue;
    l+= idlen;
    rlen-= idlen;
    idlen= mlog_decode_varint_length(*l);
    if (UNIV_UNLIKELY(idlen > 5 || idlen > rlen))
      continue;
    const uint32_t page_no= mlog_decode_varint(l);
    if (UNIV_UNLIKELY(page_no == MLOG_DECODE_ERROR))
      continue;
    const page_id_t id{space_id, page_no};
    if (pages_it == pages.end() || pages_it->first != id)
    {
      pages_it= pages.find(id);
      if (pages_it == pages.end())
        continue;
    }

    ut_ad(!pages_it->second.being_processed);
    const log_phys_t *head=
      static_cast<log_phys_t*>(*pages_it->second.log.begin());
    if (!head || head->start_lsn == recovered_lsn)
    {
      erase(pages_it);
      pages_it= pages.end();
    }
    else
      pages_it->second.log.rewind(recovered_lsn);
  }

  pages_it= pages.end();
}

/** Parse and register mini-transactions in log_t::FORMAT_10_5.
@param checkpoint_lsn  the log sequence number of the latest checkpoint
@param store           whether to store the records
@param apply           whether to apply file-level log records
@return whether FILE_CHECKPOINT record was seen the first time,
or corruption was noticed */
bool recv_sys_t::parse(lsn_t checkpoint_lsn, store_t *store, bool apply)
{
restart:
  mysql_mutex_assert_owner(&log_sys.mutex);
  mysql_mutex_assert_owner(&mutex);
  ut_ad(parse_start_lsn);
  ut_ad(log_sys.is_physical());

  const byte *const end= buf + len;
loop:
  const byte *const log= buf + recovered_offset;
  const lsn_t start_lsn= recovered_lsn;

  /* Check that the entire mini-transaction is included within the buffer */
  const byte *l;
  uint32_t rlen;
  for (l= log; l < end; l+= rlen)
  {
    if (!*l)
      goto eom_found;
    if (UNIV_LIKELY((*l & 0x70) != RESERVED));
    else if (srv_force_recovery)
      ib::warn() << "Ignoring unknown log record at LSN " << recovered_lsn;
    else
    {
malformed:
      ib::error() << "Malformed log record;"
                     " set innodb_force_recovery=1 to ignore.";
corrupted:
      const size_t trailing_bytes= std::min<size_t>(100, size_t(end - l));
      ib::info() << "Dump from the start of the mini-transaction (LSN="
                 << start_lsn << ") to "
                 << trailing_bytes << " bytes after the record:";
      ut_print_buf(stderr, log, l - log + trailing_bytes);
      putc('\n', stderr);
      found_corrupt_log= true;
      return true;
    }
    rlen= *l++ & 0xf;
    if (l + (rlen ? rlen : 16) >= end)
      break;
    if (!rlen)
    {
      rlen= mlog_decode_varint_length(*l);
      if (l + rlen >= end)
        break;
      const uint32_t addlen= mlog_decode_varint(l);
      if (UNIV_UNLIKELY(addlen == MLOG_DECODE_ERROR))
      {
        ib::error() << "Corrupted record length";
        goto corrupted;
      }
      rlen= addlen + 15;
    }
  }

  /* Not the entire mini-transaction was present. */
  return false;

eom_found:
  ut_ad(!*l);
  ut_d(const byte *const el= l + 1);

  const lsn_t end_lsn= recv_calc_lsn_on_data_add(start_lsn, l + 1 - log);
  if (UNIV_UNLIKELY(end_lsn > scanned_lsn))
    /* The log record filled a log block, and we require that also the
    next log block should have been scanned in */
    return false;

  ut_d(std::set<page_id_t> freed);
#if 0 && defined UNIV_DEBUG /* MDEV-21727 FIXME: enable this */
  /* Pages that have been modified in this mini-transaction.
  If a mini-transaction writes INIT_PAGE for a page, it should not have
  written any log records for the page. Unfortunately, this does not
  hold for ROW_FORMAT=COMPRESSED pages, because page_zip_compress()
  can be invoked in a pessimistic operation, even after log has
  been written for other pages. */
  ut_d(std::set<page_id_t> modified);
#endif

  uint32_t space_id= 0, page_no= 0, last_offset= 0;
  bool got_page_op= false;
  for (l= log; l < end; l+= rlen)
  {
    const byte *const recs= l;
    const byte b= *l++;

    if (!b)
      break;
    ut_ad(UNIV_LIKELY(b & 0x70) != RESERVED || srv_force_recovery);
    rlen= b & 0xf;
    ut_ad(l + rlen < end);
    ut_ad(rlen || l + 16 < end);
    if (!rlen)
    {
      const uint32_t lenlen= mlog_decode_varint_length(*l);
      ut_ad(l + lenlen < end);
      const uint32_t addlen= mlog_decode_varint(l);
      ut_ad(addlen != MLOG_DECODE_ERROR);
      rlen= addlen + 15 - lenlen;
      l+= lenlen;
    }
    ut_ad(l + rlen < end);
    uint32_t idlen;
    if ((b & 0x80) && got_page_op)
    {
      /* This record is for the same page as the previous one. */
      if (UNIV_UNLIKELY((b & 0x70) <= INIT_PAGE))
      {
record_corrupted:
        /* FREE_PAGE,INIT_PAGE cannot be with same_page flag */
        if (!srv_force_recovery)
          goto malformed;
        ib::warn() << "Ignoring malformed log record at LSN " << recovered_lsn;
        last_offset= 1; /* the next record must not be same_page  */
        continue;
      }
      goto same_page;
    }
    last_offset= 0;
    idlen= mlog_decode_varint_length(*l);
    if (UNIV_UNLIKELY(idlen > 5 || idlen >= rlen))
    {
page_id_corrupted:
      if (!srv_force_recovery)
      {
        ib::error() << "Corrupted page identifier at " << recovered_lsn
                    << "; set innodb_force_recovery=1 to ignore the record.";
        goto corrupted;
      }
      ib::warn() << "Ignoring corrupted page identifier at LSN "
                 << recovered_lsn;
      continue;
    }
    space_id= mlog_decode_varint(l);
    if (UNIV_UNLIKELY(space_id == MLOG_DECODE_ERROR))
      goto page_id_corrupted;
    l+= idlen;
    rlen-= idlen;
    idlen= mlog_decode_varint_length(*l);
    if (UNIV_UNLIKELY(idlen > 5 || idlen > rlen))
      goto page_id_corrupted;
    page_no= mlog_decode_varint(l);
    if (UNIV_UNLIKELY(page_no == MLOG_DECODE_ERROR))
      goto page_id_corrupted;
    l+= idlen;
    rlen-= idlen;
    got_page_op = !(b & 0x80);
    if (got_page_op && apply && !is_predefined_tablespace(space_id))
    {
      recv_spaces_t::iterator i= recv_spaces.lower_bound(space_id);
      if (i != recv_spaces.end() && i->first == space_id);
      else if (recovered_lsn < mlog_checkpoint_lsn)
        /* We have not seen all records between the checkpoint and
        FILE_CHECKPOINT. There should be a FILE_DELETE for this
        tablespace later. */
        recv_spaces.emplace_hint(i, space_id, file_name_t("", false));
      else
      {
        const page_id_t id(space_id, page_no);
        if (!srv_force_recovery)
        {
          ib::error() << "Missing FILE_DELETE or FILE_MODIFY for " << id
                      << " at " << recovered_lsn
                      << "; set innodb_force_recovery=1 to ignore the record.";
          goto corrupted;
        }
        ib::warn() << "Ignoring record for " << id << " at " << recovered_lsn;
        continue;
      }
    }
same_page:
    DBUG_PRINT("ib_log",
               ("scan " LSN_PF ": rec %x len %zu page %u:%u",
                recovered_lsn, b, static_cast<size_t>(l + rlen - recs),
                space_id, page_no));

    if (got_page_op)
    {
      const page_id_t id(space_id, page_no);
      ut_d(if ((b & 0x70) == INIT_PAGE || (b & 0x70) == OPTION)
             freed.erase(id));
      ut_ad(freed.find(id) == freed.end());
      switch (b & 0x70) {
      case FREE_PAGE:
        ut_ad(freed.emplace(id).second);
        last_offset= 1; /* the next record must not be same_page  */
        goto free_or_init_page;
      case INIT_PAGE:
        last_offset= FIL_PAGE_TYPE;
      free_or_init_page:
        store_freed_or_init_rec(id, (b & 0x70) == FREE_PAGE);
        if (UNIV_UNLIKELY(rlen != 0))
          goto record_corrupted;
        break;
      case EXTENDED:
        if (UNIV_UNLIKELY(!rlen))
          goto record_corrupted;
        if (rlen == 1 && *l == TRIM_PAGES)
        {
#if 0 /* For now, we can only truncate an undo log tablespace */
          if (UNIV_UNLIKELY(!space_id || !page_no))
            goto record_corrupted;
#else
          if (!srv_is_undo_tablespace(space_id) ||
              page_no != SRV_UNDO_TABLESPACE_SIZE_IN_PAGES)
            goto record_corrupted;
          static_assert(UT_ARR_SIZE(truncated_undo_spaces) ==
                        TRX_SYS_MAX_UNDO_SPACES, "compatibility");
          /* The entire undo tablespace will be reinitialized by
          innodb_undo_log_truncate=ON. Discard old log for all pages. */
          trim({space_id, 0}, start_lsn);
          truncated_undo_spaces[space_id - srv_undo_space_id_start]=
            { start_lsn, page_no };
          if (undo_space_trunc)
            undo_space_trunc(space_id);
#endif
          last_offset= 1; /* the next record must not be same_page  */
          continue;
        }
        last_offset= FIL_PAGE_TYPE;
        break;
      case OPTION:
        if (rlen == 5 && *l == OPT_PAGE_CHECKSUM)
          break;
        /* fall through */
      case RESERVED:
        continue;
      case WRITE:
      case MEMMOVE:
      case MEMSET:
        if (UNIV_UNLIKELY(rlen == 0 || last_offset == 1))
          goto record_corrupted;
        const uint32_t olen= mlog_decode_varint_length(*l);
        if (UNIV_UNLIKELY(olen >= rlen) || UNIV_UNLIKELY(olen > 3))
          goto record_corrupted;
        const uint32_t offset= mlog_decode_varint(l);
        ut_ad(offset != MLOG_DECODE_ERROR);
        static_assert(FIL_PAGE_OFFSET == 4, "compatibility");
        if (UNIV_UNLIKELY(offset >= srv_page_size))
          goto record_corrupted;
        last_offset+= offset;
        if (UNIV_UNLIKELY(last_offset < 8 || last_offset >= srv_page_size))
          goto record_corrupted;
        l+= olen;
        rlen-= olen;
        if ((b & 0x70) == WRITE)
        {
          if (UNIV_UNLIKELY(rlen + last_offset > srv_page_size))
            goto record_corrupted;
          if (UNIV_UNLIKELY(!page_no) && apply)
          {
            const bool has_size= last_offset <= FSP_HEADER_OFFSET + FSP_SIZE &&
              last_offset + rlen >= FSP_HEADER_OFFSET + FSP_SIZE + 4;
            const bool has_flags= last_offset <=
              FSP_HEADER_OFFSET + FSP_SPACE_FLAGS &&
              last_offset + rlen >= FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + 4;
            if (has_size || has_flags)
            {
              recv_spaces_t::iterator it= recv_spaces.find(space_id);
              const uint32_t size= has_size
                ? mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE + l -
                                   last_offset)
                : 0;
              const uint32_t flags= has_flags
                ? mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + l -
                                   last_offset)
                : file_name_t::initial_flags;
              if (it == recv_spaces.end())
                ut_ad(!mlog_checkpoint_lsn || space_id == TRX_SYS_SPACE ||
                      srv_is_undo_tablespace(space_id));
              else if (!it->second.space)
              {
                if (has_size)
                  it->second.size= size;
                if (has_flags)
                  it->second.flags= flags;
              }
              fil_space_set_recv_size_and_flags(space_id, size, flags);
            }
          }
          last_offset+= rlen;
          break;
        }
        uint32_t llen= mlog_decode_varint_length(*l);
        if (UNIV_UNLIKELY(llen > rlen || llen > 3))
          goto record_corrupted;
        const uint32_t len= mlog_decode_varint(l);
        ut_ad(len != MLOG_DECODE_ERROR);
        if (UNIV_UNLIKELY(last_offset + len > srv_page_size))
          goto record_corrupted;
        l+= llen;
        rlen-= llen;
        llen= len;
        if ((b & 0x70) == MEMSET)
        {
          if (UNIV_UNLIKELY(rlen > llen))
            goto record_corrupted;
          last_offset+= llen;
          break;
        }
        const uint32_t slen= mlog_decode_varint_length(*l);
        if (UNIV_UNLIKELY(slen != rlen || slen > 3))
          goto record_corrupted;
        uint32_t s= mlog_decode_varint(l);
        ut_ad(slen != MLOG_DECODE_ERROR);
        if (s & 1)
          s= last_offset - (s >> 1) - 1;
        else
          s= last_offset + (s >> 1) + 1;
        if (UNIV_UNLIKELY(s < 8 || s + llen > srv_page_size))
          goto record_corrupted;
        last_offset+= llen;
        break;
      }
#if 0 && defined UNIV_DEBUG
      switch (b & 0x70) {
      case RESERVED:
        ut_ad(0); /* we did "continue" earlier */
        break;
      case OPTION:
      case FREE_PAGE:
        break;
      default:
        ut_ad(modified.emplace(id).second || (b & 0x70) != INIT_PAGE);
      }
#endif
      switch (*store) {
      case STORE_IF_EXISTS:
        if (fil_space_t *space= fil_space_t::get(space_id))
        {
          const auto size= space->get_size();
          space->release();
          if (!size)
            continue;
        }
        else if (!deferred_spaces.find(space_id))
          continue;
        /* fall through */
      case STORE_YES:
        if (mlog_init.will_avoid_read(id, start_lsn))
          continue;
        if (pages_it == pages.end() || pages_it->first != id)
          pages_it= pages.emplace(id, page_recv_t{}).first;
        if (UNIV_UNLIKELY(add(pages_it, start_lsn, end_lsn, recs,
                              l - recs + rlen)))
        {
          recovered_lsn= start_lsn;
          recovered_offset= log - buf;
          rewind(l + rlen, log);
          if (*store == STORE_IF_EXISTS)
          {
            if (log_sys.get_lsn() < start_lsn)
            {
              log_sys.set_lsn(start_lsn);
              log_sys.set_flushed_lsn(start_lsn);
            }
            mysql_mutex_unlock(&mutex);
            this->apply(false);
            mysql_mutex_lock(&mutex);
            if (is_corrupt_fs())
              return true;
          }
          else
          {
            last_stored_lsn= start_lsn;
            sql_print_information("InnoDB: Multi-batch recovery needed at LSN "
                                  LSN_PF, start_lsn);
            *store= STORE_NO;
          }
          goto restart;
        }
        continue;
      case STORE_NO:
        if ((b & 0x70) > INIT_PAGE)
          continue;
        mlog_init.add(id, start_lsn);
        if (pages_it == pages.end() || pages_it->first != id)
        {
          pages_it= pages.find(id);
          if (pages_it == pages.end())
            continue;
        }
        map::iterator r= pages_it++;
        erase(r);
      }
    }
    else if (rlen)
    {
      switch (b & 0xf0) {
      case FILE_CHECKPOINT:
        if (space_id == 0 && page_no == 0 && rlen == 8)
        {
          const lsn_t lsn= mach_read_from_8(l);

          if (UNIV_UNLIKELY(srv_print_verbose_log == 2))
            fprintf(stderr, "FILE_CHECKPOINT(" LSN_PF ") %s at " LSN_PF "\n",
                    lsn, lsn != checkpoint_lsn
                    ? "ignored"
                    : mlog_checkpoint_lsn ? "reread" : "read",
                    recovered_lsn);

          DBUG_PRINT("ib_log", ("FILE_CHECKPOINT(" LSN_PF ") %s at " LSN_PF,
                                lsn, lsn != checkpoint_lsn
                                ? "ignored"
                                : mlog_checkpoint_lsn ? "reread" : "read",
                                recovered_lsn));

          if (lsn == checkpoint_lsn)
          {
            /* There can be multiple FILE_CHECKPOINT for the same LSN. */
            if (mlog_checkpoint_lsn)
              continue;
            mlog_checkpoint_lsn= recovered_lsn;
            l+= 8;
            recovered_offset= l - buf;
            return true;
          }
          continue;
        }
        /* fall through */
      default:
        if (!srv_force_recovery)
          goto malformed;
        ib::warn() << "Ignoring malformed log record at LSN " << recovered_lsn;
        continue;
      case FILE_DELETE:
      case FILE_MODIFY:
      case FILE_RENAME:
        if (UNIV_UNLIKELY(page_no != 0))
        {
        file_rec_error:
          if (!srv_force_recovery)
          {
            ib::error() << "Corrupted file-level record;"
                           " set innodb_force_recovery=1 to ignore.";
            goto corrupted;
          }

          ib::warn() << "Ignoring corrupted file-level record at LSN "
                     << recovered_lsn;
          continue;
        }
        /* fall through */
      case FILE_CREATE:
        if (UNIV_UNLIKELY(!space_id || page_no))
          goto file_rec_error;
        /* There is no terminating NUL character. Names must end in .ibd.
        For FILE_RENAME, there is a NUL between the two file names. */
        const char * const fn= reinterpret_cast<const char*>(l);
        const char *fn2= static_cast<const char*>(memchr(fn, 0, rlen));

        if (UNIV_UNLIKELY((fn2 == nullptr) == ((b & 0xf0) == FILE_RENAME)))
          goto file_rec_error;

        const char * const fnend= fn2 ? fn2 : fn + rlen;
        const char * const fn2end= fn2 ? fn + rlen : nullptr;

        if (fn2)
        {
          fn2++;
          if (memchr(fn2, 0, fn2end - fn2))
            goto file_rec_error;
          if (fn2end - fn2 < 4 || memcmp(fn2end - 4, DOT_IBD, 4))
            goto file_rec_error;
        }

        if (is_predefined_tablespace(space_id))
          goto file_rec_error;
        if (fnend - fn < 4 || memcmp(fnend - 4, DOT_IBD, 4))
          goto file_rec_error;

        fil_name_process(fn, fnend - fn, space_id,
                         (b & 0xf0) == FILE_DELETE ? FILE_DELETE : FILE_MODIFY,
                         start_lsn, *store);

        if ((b & 0xf0) < FILE_CHECKPOINT && log_file_op)
          log_file_op(space_id, b & 0xf0,
                      l, static_cast<ulint>(fnend - fn),
                      reinterpret_cast<const byte*>(fn2),
                      fn2 ? static_cast<ulint>(fn2end - fn2) : 0);

        if (fn2)
        {
          fil_name_process(fn2, fn2end - fn2, space_id,
                           FILE_RENAME, start_lsn, *store);
          if (apply)
          {
            const size_t len= fn2end - fn2;
            auto r= renamed_spaces.emplace(space_id, std::string{fn2, len});
            if (!r.second)
              r.first->second= std::string{fn2, len};
          }
        }

        if (is_corrupt_fs())
          return true;
      }
    }
    else
      goto malformed;
  }

  ut_ad(l == el);
  recovered_offset= l - buf;
  recovered_lsn= end_lsn;
  goto loop;
}

/** Apply the hashed log records to the page, if the page lsn is less than the
lsn of a log record.
@param[in,out]	block		buffer pool page
@param[in,out]	mtr		mini-transaction
@param[in,out]	recs		log records to apply
@param[in,out]	space		tablespace, or NULL if not looked up yet
@param[in,out]	init		page initialization operation, or NULL
@return the recovered page
@retval nullptr on failure */
static buf_block_t *recv_recover_page(buf_block_t *block, mtr_t &mtr,
                                      page_recv_t &recs,
                                      fil_space_t *space,
                                      recv_init *init)
{
	mysql_mutex_assert_not_owner(&recv_sys.mutex);
	ut_ad(recv_sys.apply_log_recs);
	ut_ad(recv_needed_recovery);
	ut_ad(!init || init->created);
	ut_ad(!init || init->lsn);
	ut_ad(recs.being_processed == 1);
	ut_ad(!space || space->id == block->page.id().space());
	ut_ad(log_sys.is_physical());

	if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
		ib::info() << "Applying log to page " << block->page.id();
	}

	DBUG_PRINT("ib_log", ("Applying log to page %u:%u",
			      block->page.id().space(),
			      block->page.id().page_no()));

	byte *frame = UNIV_LIKELY_NULL(block->page.zip.data)
		? block->page.zip.data
		: block->page.frame;
	const lsn_t page_lsn = init
		? 0
		: mach_read_from_8(frame + FIL_PAGE_LSN);
	bool free_page = false;
	lsn_t start_lsn = 0, end_lsn = 0;
	ut_d(lsn_t recv_start_lsn = 0);
	const lsn_t init_lsn = init ? init->lsn : 0;

	bool skipped_after_init = false;

	for (const log_rec_t* recv : recs.log) {
		const log_phys_t* l = static_cast<const log_phys_t*>(recv);
		ut_ad(l->lsn);
		ut_ad(end_lsn <= l->lsn);
		ut_ad(l->lsn <= log_sys.log.scanned_lsn);

		ut_ad(l->start_lsn);
		ut_ad(recv_start_lsn <= l->start_lsn);
		ut_d(recv_start_lsn = l->start_lsn);

		if (l->start_lsn < page_lsn) {
			/* This record has already been applied. */
			DBUG_PRINT("ib_log", ("apply skip %u:%u LSN " LSN_PF
					      " < " LSN_PF,
					      block->page.id().space(),
					      block->page.id().page_no(),
					      l->start_lsn, page_lsn));
			skipped_after_init = true;
			end_lsn = l->lsn;
			continue;
		}

		if (l->start_lsn < init_lsn) {
			DBUG_PRINT("ib_log", ("init skip %u:%u LSN " LSN_PF
					      " < " LSN_PF,
					      block->page.id().space(),
					      block->page.id().page_no(),
					      l->start_lsn, init_lsn));
			skipped_after_init = false;
			end_lsn = l->lsn;
			continue;
		}

		/* There is no need to check LSN for just initialized pages. */
		if (skipped_after_init) {
			skipped_after_init = false;
			ut_ad(end_lsn == page_lsn);
			if (end_lsn != page_lsn)
				ib::warn()
					<< "The last skipped log record LSN "
					<< end_lsn
					<< " is not equal to page LSN "
					<< page_lsn;
		}

		end_lsn = l->lsn;

		if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
			ib::info() << "apply " << l->start_lsn
				   << ": " << block->page.id();
		}

		DBUG_PRINT("ib_log", ("apply " LSN_PF ": %u:%u",
				      l->start_lsn,
				      block->page.id().space(),
				      block->page.id().page_no()));

		log_phys_t::apply_status a= l->apply(*block, recs.last_offset);

		switch (a) {
		case log_phys_t::APPLIED_NO:
			ut_ad(!mtr.has_modifications());
			free_page = true;
			start_lsn = 0;
			continue;
		case log_phys_t::APPLIED_YES:
		case log_phys_t::APPLIED_CORRUPTED:
			goto set_start_lsn;
		case log_phys_t::APPLIED_TO_FSP_HEADER:
		case log_phys_t::APPLIED_TO_ENCRYPTION:
			break;
		}

		if (fil_space_t* s = space
		    ? space
		    : fil_space_t::get(block->page.id().space())) {
			switch (a) {
			case log_phys_t::APPLIED_TO_FSP_HEADER:
				s->flags = mach_read_from_4(
					FSP_HEADER_OFFSET
					+ FSP_SPACE_FLAGS + frame);
				s->size_in_header = mach_read_from_4(
					FSP_HEADER_OFFSET + FSP_SIZE
					+ frame);
				s->free_limit = mach_read_from_4(
					FSP_HEADER_OFFSET
					+ FSP_FREE_LIMIT + frame);
				s->free_len = mach_read_from_4(
					FSP_HEADER_OFFSET + FSP_FREE
					+ FLST_LEN + frame);
				break;
			default:
				byte* b= frame
					+ fsp_header_get_encryption_offset(
						block->zip_size())
					+ FSP_HEADER_OFFSET;
				if (memcmp(b, CRYPT_MAGIC, MAGIC_SZ)) {
					break;
				}
				b += MAGIC_SZ;
				if (*b != CRYPT_SCHEME_UNENCRYPTED
				    && *b != CRYPT_SCHEME_1) {
					break;
				}
				if (b[1] != MY_AES_BLOCK_SIZE) {
					break;
				}
				if (b[2 + MY_AES_BLOCK_SIZE + 4 + 4]
				    > FIL_ENCRYPTION_OFF) {
					break;
				}
				fil_crypt_parse(s, b);
			}

			if (!space) {
				s->release();
			}
		}

set_start_lsn:
		if ((a == log_phys_t::APPLIED_CORRUPTED
		     || recv_sys.is_corrupt_log()) && !srv_force_recovery) {
			if (init) {
				init->created = false;
			}

			mtr.discard_modifications();
			mtr.commit();

			fil_space_t* s = space
				? space
				: fil_space_t::get(block->page.id().space());

			buf_pool.corrupted_evict(&block->page,
						 block->page.state() &
						 buf_page_t::LRU_MASK);
			if (!space) {
				s->release();
			}

			return nullptr;
		}

		if (!start_lsn) {
			start_lsn = l->start_lsn;
		}
	}

	if (start_lsn) {
		ut_ad(end_lsn >= start_lsn);
		mach_write_to_8(FIL_PAGE_LSN + frame, end_lsn);
		if (UNIV_LIKELY(frame == block->page.frame)) {
			mach_write_to_8(srv_page_size
					- FIL_PAGE_END_LSN_OLD_CHKSUM
					+ frame, end_lsn);
		} else {
			buf_zip_decompress(block, false);
		}

		buf_block_modify_clock_inc(block);
		mysql_mutex_lock(&log_sys.flush_order_mutex);
		buf_flush_note_modification(block, start_lsn, end_lsn);
		mysql_mutex_unlock(&log_sys.flush_order_mutex);
	} else if (free_page && init) {
		/* There have been no operations that modify the page.
		Any buffered changes must not be merged. A subsequent
		buf_page_create() from a user thread should discard
		any buffered changes. */
		init->created = false;
		ut_ad(!mtr.has_modifications());
		block->page.set_freed(block->page.state());
	}

	/* Make sure that committing mtr does not change the modification
	lsn values of page */

	mtr.discard_modifications();
	mtr.commit();

	return block;
}

/** Remove records for a corrupted page.
@param page_id  corrupted page identifier
@param node     file for which an error is to be reported
@return whether an error message was reported */
ATTRIBUTE_COLD
bool recv_sys_t::free_corrupted_page(page_id_t page_id,
                                     const fil_node_t &node) noexcept
{
  if (!recovery_on)
    return false;

  mysql_mutex_lock(&mutex);
  map::iterator p= pages.find(page_id);
  if (p == pages.end())
  {
    mysql_mutex_unlock(&mutex);
    return false;
  }

  p->second.being_processed= -1;
  if (!srv_force_recovery)
    set_corrupt_fs();
  mysql_mutex_unlock(&mutex);

  (srv_force_recovery ? sql_print_warning : sql_print_error)
    ("InnoDB: Unable to apply log to corrupted page " UINT32PF
     " in file %s", page_id.page_no(), node.name);
  return true;
}

ATTRIBUTE_COLD void recv_sys_t::set_corrupt_log() noexcept
{
  mysql_mutex_lock(&mutex);
  found_corrupt_log= true;
  mysql_mutex_unlock(&mutex);
}

ATTRIBUTE_COLD void recv_sys_t::set_corrupt_fs() noexcept
{
  mysql_mutex_assert_owner(&mutex);
  if (!srv_force_recovery)
    sql_print_information("InnoDB: Set innodb_force_recovery=1"
                          " to ignore corrupted pages.");
  found_corrupt_fs= true;
}

/** Apply any buffered redo log to a page.
@param space     tablespace
@param bpage     buffer pool page
@return whether the page was recovered correctly */
bool recv_recover_page(fil_space_t* space, buf_page_t* bpage)
{
  mtr_t mtr;
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);

  ut_ad(bpage->frame);
  /* Move the ownership of the x-latch on the page to this OS thread,
  so that we can acquire a second x-latch on it. This is needed for
  the operations to the page to pass the debug checks. */
  bpage->lock.claim_ownership();
  bpage->lock.x_lock_recursive();
  bpage->fix_on_recovery();
  mtr.memo_push(reinterpret_cast<buf_block_t*>(bpage), MTR_MEMO_PAGE_X_FIX);

  buf_block_t *success= reinterpret_cast<buf_block_t*>(bpage);

  mysql_mutex_lock(&recv_sys.mutex);
  if (recv_sys.apply_log_recs)
  {
    const page_id_t id{bpage->id()};
    recv_sys_t::map::iterator p= recv_sys.pages.find(id);
    if (p == recv_sys.pages.end());
    else if (p->second.being_processed < 0)
    {
      recv_sys.pages_it_invalidate(p);
      recv_sys.erase(p);
    }
    else
    {
      p->second.being_processed= 1;
      recv_sys_t::init *init= nullptr;
      if (p->second.skip_read)
        (init= &mlog_init.last(id))->created= true;
      mysql_mutex_unlock(&recv_sys.mutex);
      success= recv_recover_page(success, mtr, p->second, space, init);
      p->second.being_processed= -1;
      goto func_exit;
    }
  }

  mysql_mutex_unlock(&recv_sys.mutex);
  mtr.commit();
func_exit:
  ut_ad(mtr.has_committed());
  return success;
}

void IORequest::fake_read_complete(os_offset_t offset) const noexcept
{
  ut_ad(node);
  ut_ad(is_read());
  ut_ad(bpage);
  ut_ad(bpage->frame);
  ut_ad(recv_recovery_is_on());
  ut_ad(offset);

  mtr_t mtr;
  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);

  ut_ad(bpage->frame);
  /* Move the ownership of the x-latch on the page to this OS thread,
  so that we can acquire a second x-latch on it. This is needed for
  the operations to the page to pass the debug checks. */
  bpage->lock.claim_ownership();
  bpage->lock.x_lock_recursive();
  bpage->fix_on_recovery();
  mtr.memo_push(reinterpret_cast<buf_block_t*>(bpage), MTR_MEMO_PAGE_X_FIX);

  page_recv_t &recs= *reinterpret_cast<page_recv_t*>(slot);
  ut_ad(recs.being_processed == 1);
  recv_init &init= *reinterpret_cast<recv_init*>(offset);
  ut_ad(init.lsn > 1);
  init.created= true;

  if (recv_recover_page(reinterpret_cast<buf_block_t*>(bpage),
                        mtr, recs, node->space, &init))
  {
    ut_ad(bpage->oldest_modification() || bpage->is_freed());
    bpage->lock.x_unlock(true);
  }
  recs.being_processed= -1;
  ut_ad(mtr.has_committed());

  node->space->release();
}

/** @return whether a page has been freed */
inline bool fil_space_t::is_freed(uint32_t page) noexcept
{
  std::lock_guard<std::mutex> freed_lock(freed_range_mutex);
  return freed_ranges.contains(page);
}

bool recv_sys_t::report(time_t time)
{
  if (time - progress_time < 15)
    return false;
  progress_time= time;
  return true;
}

ATTRIBUTE_COLD
void recv_sys_t::report_progress() const
{
  mysql_mutex_assert_owner(&mutex);
  const size_t n{pages.size()};
  if (recv_sys.scanned_lsn == recv_sys.recovered_lsn)
  {
    sql_print_information("InnoDB: To recover: %zu pages", n);
    service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                   "To recover: %zu pages", n);
  }
  else
  {
    sql_print_information("InnoDB: To recover: LSN " LSN_PF
                          "/" LSN_PF "; %zu pages",
                          recv_sys.recovered_lsn, recv_sys.scanned_lsn, n);
    service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                   "To recover: LSN " LSN_PF
                                   "/" LSN_PF "; %zu pages",
                                   recv_sys.recovered_lsn,
                                   recv_sys.scanned_lsn, n);
  }
}

/** Apply a recovery batch.
@param space_id       current tablespace identifier
@param space          current tablespace
@param free_block     spare buffer block
@param last_batch     whether it is possible to write more redo log
@return whether the caller must provide a new free_block */
bool recv_sys_t::apply_batch(uint32_t space_id, fil_space_t *&space,
                             buf_block_t *&free_block, bool last_batch)
{
  mysql_mutex_assert_owner(&mutex);
  ut_ad(pages_it != pages.end());
  ut_ad(!pages_it->second.log.empty());

  mysql_mutex_lock(&buf_pool.mutex);
  size_t n= 0, max_n= std::min<size_t>(BUF_LRU_MIN_LEN,
                                       UT_LIST_GET_LEN(buf_pool.LRU) +
                                       UT_LIST_GET_LEN(buf_pool.free));
  mysql_mutex_unlock(&buf_pool.mutex);

  map::iterator begin= pages.end();
  page_id_t begin_id{~0ULL};

  while (pages_it != pages.end() && n < max_n)
  {
    ut_ad(!buf_dblwr.is_inside(pages_it->first));
    if (!pages_it->second.being_processed)
    {
      if (space_id != pages_it->first.space())
      {
        space_id= pages_it->first.space();
        if (space)
          space->release();
        space= fil_space_t::get(space_id);
        if (!space)
        {
          auto d= deferred_spaces.defers.find(space_id);
          if (d == deferred_spaces.defers.end() || d->second.deleted)
            /* For deleted files we preserve the deferred_spaces entry */;
          else if (!free_block)
            return true;
          else
          {
            space= recover_deferred(pages_it, d->second.file_name, free_block);
            deferred_spaces.defers.erase(d);
            if (!space && !srv_force_recovery)
            {
              set_corrupt_fs();
              return false;
            }
          }
        }
      }
      if (!space || space->is_freed(pages_it->first.page_no()))
        pages_it->second.being_processed= -1;
      else if (!n++)
      {
        begin= pages_it;
        begin_id= pages_it->first;
      }
    }
    pages_it++;
  }

  if (!last_batch)
    mysql_mutex_unlock(&log_sys.mutex);

  mysql_mutex_assert_not_owner(&log_sys.mutex);

  pages_it= begin;

  if (report(time(nullptr)))
    report_progress();

  if (!n)
    goto wait;

  mysql_mutex_lock(&buf_pool.mutex);

  if (UNIV_UNLIKELY(UT_LIST_GET_LEN(buf_pool.free) < n))
  {
    mysql_mutex_unlock(&buf_pool.mutex);
  wait:
    wait_for_pool(n);
    if (n);
    else if (!last_batch)
      goto unlock_relock;
    else
      goto get_last;
    pages_it= pages.lower_bound(begin_id);
    ut_ad(pages_it != pages.end());
  }
  else
    mysql_mutex_unlock(&buf_pool.mutex);

  while (pages_it != pages.end())
  {
    ut_ad(!buf_dblwr.is_inside(pages_it->first));
    if (!pages_it->second.being_processed)
    {
      const page_id_t id{pages_it->first};

      if (space_id != id.space())
      {
        space_id= id.space();
        if (space)
          space->release();
        space= fil_space_t::get(space_id);
      }
      if (!space)
      {
	const auto it= deferred_spaces.defers.find(space_id);
	if (it != deferred_spaces.defers.end() && !it->second.deleted)
          /* The records must be processed after recover_deferred(). */
	  goto next;
        goto space_not_found;
      }
      else if (space->is_freed(id.page_no()))
      {
      space_not_found:
        pages_it->second.being_processed= -1;
        goto next;
      }
      else
      {
        page_recv_t &recs= pages_it->second;
        ut_ad(!recs.log.empty());
        recs.being_processed= 1;
        init *init= recs.skip_read ? &mlog_init.last(id) : nullptr;
        mysql_mutex_unlock(&mutex);
        buf_read_recover(space, id, recs, init);
      }

      if (!--n)
      {
        if (last_batch)
          goto relock_last;
        goto relock;
      }
      mysql_mutex_lock(&mutex);
      pages_it= pages.lower_bound(id);
    }
    else
    next:
      pages_it++;
  }

  if (!last_batch)
  {
  unlock_relock:
    mysql_mutex_unlock(&mutex);
  relock:
    mysql_mutex_lock(&log_sys.mutex);
  relock_last:
    mysql_mutex_lock(&mutex);
  get_last:
    pages_it= pages.lower_bound(begin_id);
  }

  return false;
}

/** Attempt to initialize a page based on redo log records.
@param p        iterator
@param mtr      mini-transaction
@param b        pre-allocated buffer pool block
@param init     page initialization
@return the recovered block
@retval nullptr if the page cannot be initialized based on log records
@retval -1      if the page cannot be recovered due to corruption */
inline buf_block_t *recv_sys_t::recover_low(const map::iterator &p, mtr_t &mtr,
                                            buf_block_t *b, init &init)
{
  mysql_mutex_assert_not_owner(&mutex);
  page_recv_t &recs= p->second;
  ut_ad(recs.skip_read);
  ut_ad(recs.being_processed == 1);
  buf_block_t* block= nullptr;
  const lsn_t end_lsn= recs.log.last()->lsn;
  if (end_lsn < init.lsn)
    DBUG_LOG("ib_log", "skip log for page " << p->first
             << " LSN " << end_lsn << " < " << init.lsn);
  fil_space_t *space= fil_space_t::get(p->first.space());

  mtr.start();
  mtr.set_log_mode(MTR_LOG_NO_REDO);

  ulint zip_size= space ? space->zip_size() : 0;

  if (!space)
  {
    if (p->first.page_no() != 0)
    {
    nothing_recoverable:
      mtr.commit();
      return nullptr;
    }
    auto it= recv_spaces.find(p->first.space());
    ut_ad(it != recv_spaces.end());
    uint32_t flags= it->second.flags;
    zip_size= fil_space_t::zip_size(flags);
    block= buf_page_create_deferred(p->first.space(), zip_size, &mtr, b);
    ut_ad(block == b);
    block->page.lock.x_lock_recursive();
  }
  else
  {
    block= buf_page_create(space, p->first.page_no(), zip_size, &mtr, b);

    if (UNIV_UNLIKELY(block != b))
    {
      /* The page happened to exist in the buffer pool, or it
      was just being read in. Before the exclusive page latch was acquired by
      buf_page_create(), all changes to the page must have been applied. */
      ut_d(mysql_mutex_lock(&mutex));
      ut_ad(pages.find(p->first) == pages.end());
      ut_d(mysql_mutex_unlock(&mutex));
      space->release();
      goto nothing_recoverable;
    }
  }

  ut_d(mysql_mutex_lock(&mutex));
  ut_ad(&recs == &pages.find(p->first)->second);
  ut_d(mysql_mutex_unlock(&mutex));
  init.created= true;
  block= recv_recover_page(block, mtr, recs, space, &init);
  ut_ad(mtr.has_committed());

  if (space)
    space->release();

  return block ? block : reinterpret_cast<buf_block_t*>(-1);
}

/** Attempt to initialize a page based on redo log records.
@param page_id  page identifier
@return recovered block
@retval nullptr if the page cannot be initialized based on log records */
ATTRIBUTE_COLD buf_block_t *recv_sys_t::recover_low(const page_id_t page_id)
{
  mysql_mutex_lock(&mutex);
  map::iterator p= pages.find(page_id);

  if (p != pages.end() && !p->second.being_processed && p->second.skip_read)
  {
    p->second.being_processed= 1;
    init &init= mlog_init.last(page_id);
    mysql_mutex_unlock(&mutex);
    buf_block_t *free_block= buf_LRU_get_free_block(false);
    mtr_t mtr;
    buf_block_t *block= recover_low(p, mtr, free_block, init);
    p->second.being_processed= -1;
    ut_ad(!block || block == reinterpret_cast<buf_block_t*>(-1) ||
          block == free_block);
    if (UNIV_UNLIKELY(!block))
      buf_pool.free_block(free_block);
    return block;
  }

  mysql_mutex_unlock(&mutex);
  return nullptr;
}

inline fil_space_t *fil_system_t::find(const char *path) const noexcept
{
  mysql_mutex_assert_owner(&mutex);
  for (fil_space_t &space : fil_system.space_list)
    if (space.chain.start && !strcmp(space.chain.start->name, path))
      return &space;
  return nullptr;
}

/** Thread-safe function which sorts flush_list by oldest_modification */
static void log_sort_flush_list() noexcept
{
  /* Ensure that oldest_modification() cannot change during std::sort() */
  {
    const double pct_lwm= srv_max_dirty_pages_pct_lwm;
    /* Disable "idle" flushing in order to minimize the wait time below. */
    srv_max_dirty_pages_pct_lwm= 0.0;

    for (;;)
    {
      os_aio_wait_until_no_pending_writes(false);
      mysql_mutex_lock(&buf_pool.flush_list_mutex);
      if (buf_pool.page_cleaner_active())
        my_cond_wait(&buf_pool.done_flush_list,
                     &buf_pool.flush_list_mutex.m_mutex);
      else if (!os_aio_pending_writes())
        break;
      mysql_mutex_unlock(&buf_pool.flush_list_mutex);
    }

    srv_max_dirty_pages_pct_lwm= pct_lwm;
  }

  const size_t size= UT_LIST_GET_LEN(buf_pool.flush_list);
  std::unique_ptr<buf_page_t *[]> list(new buf_page_t *[size]);

  /* Copy the dirty blocks from buf_pool.flush_list to an array for sorting. */
  size_t idx= 0;
  for (buf_page_t *p= UT_LIST_GET_FIRST(buf_pool.flush_list); p; )
  {
    const lsn_t lsn{p->oldest_modification()};
    ut_ad(lsn > 2 || lsn == 1);
    buf_page_t *n= UT_LIST_GET_NEXT(list, p);
    if (lsn > 1)
      list.get()[idx++]= p;
    else
      buf_pool.delete_from_flush_list(p);
    p= n;
  }

  std::sort(list.get(), list.get() + idx,
            [](const buf_page_t *lhs, const buf_page_t *rhs) {
              const lsn_t l{lhs->oldest_modification()};
              const lsn_t r{rhs->oldest_modification()};
              DBUG_ASSERT(l == 1 || l > 2); DBUG_ASSERT(r == 1 || r > 2);
              return r < l;
            });

  UT_LIST_INIT(buf_pool.flush_list, &buf_page_t::list);

  for (size_t i= 0; i < idx; i++)
  {
    buf_page_t *b= list[i];
    const lsn_t lsn{b->oldest_modification()};
    if (lsn == 1)
      continue;
    DBUG_ASSERT(lsn > 2);
    UT_LIST_ADD_LAST(buf_pool.flush_list, b);
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
}

/** Apply buffered log to persistent data pages.
@param last_batch     whether it is possible to write more redo log */
void recv_sys_t::apply(bool last_batch)
{
  ut_ad(srv_operation <= SRV_OPERATION_EXPORT_RESTORED ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_EXPORT);

#ifdef SAFE_MUTEX
  DBUG_ASSERT(!last_batch == mysql_mutex_is_owner(&log_sys.mutex));
#endif /* SAFE_MUTEX */
  mysql_mutex_lock(&mutex);

  garbage_collect();

  for (auto id= srv_undo_tablespaces_open; id--;)
  {
    const trunc& t= truncated_undo_spaces[id];
    if (t.lsn)
    {
      /* The entire undo tablespace will be reinitialized by
      innodb_undo_log_truncate=ON. Discard old log for all pages.
      Even though we recv_sys_t::parse() already invoked trim(),
      this will be needed in case recovery consists of multiple batches
      (there was an invocation with !last_batch). */
      trim({id + srv_undo_space_id_start, 0}, t.lsn);
      if (fil_space_t *space = fil_space_get(id + srv_undo_space_id_start))
      {
        ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
        ut_ad(space->recv_size >= t.pages);
        fil_node_t *file= UT_LIST_GET_FIRST(space->chain);
        ut_ad(file->is_open());
        os_file_truncate(file->name, file->handle,
                         os_offset_t{space->recv_size} <<
                         srv_page_size_shift, true);
      }
    }
  }

  if (!pages.empty())
  {
    recv_no_ibuf_operations = !last_batch ||
      srv_operation == SRV_OPERATION_RESTORE ||
      srv_operation == SRV_OPERATION_RESTORE_EXPORT;
    progress_time= time(nullptr);
    report_progress();

    apply_log_recs= true;

    fil_system.extend_to_recv_size();

    fil_space_t *space= nullptr;
    uint32_t space_id= ~0;
    buf_block_t *free_block= nullptr;

    for (pages_it= pages.begin(); pages_it != pages.end();
         pages_it= pages.begin())
    {
      if (!free_block)
      {
        if (!last_batch)
          mysql_mutex_unlock(&log_sys.mutex);
        wait_for_pool(1);
        pages_it= pages.begin();
        mysql_mutex_unlock(&mutex);
        /* We must release log_sys.mutex and recv_sys.mutex before
        invoking buf_LRU_get_free_block(). Allocating a block may initiate
        a redo log write and therefore acquire log_sys.mutex. To avoid
        deadlocks, log_sys.mutex must not be acquired while holding
        recv_sys.mutex. */
        free_block= buf_LRU_get_free_block(false);
        if (!last_batch)
          mysql_mutex_lock(&log_sys.mutex);
        mysql_mutex_lock(&mutex);
        pages_it= pages.begin();
      }

      while (pages_it != pages.end())
      {
        if (is_corrupt_fs() || is_corrupt_log())
        {
          if (space)
            space->release();
          mysql_mutex_unlock(&mutex);
          if (free_block)
          {
            mysql_mutex_lock(&buf_pool.mutex);
            buf_LRU_block_free_non_file_page(free_block);
            mysql_mutex_unlock(&buf_pool.mutex);
          }
          return;
        }
        if (apply_batch(space_id, space, free_block, last_batch))
          break;
      }
    }

    if (space)
      space->release();

    if (free_block)
    {
      mysql_mutex_lock(&buf_pool.mutex);
      buf_LRU_block_free_non_file_page(free_block);
      mysql_mutex_unlock(&buf_pool.mutex);
    }
  }

  if (last_batch)
  {
    if (!recv_no_ibuf_operations)
      /* We skipped this in buf_page_create(). */
      mlog_init.mark_ibuf_exist();
    mlog_init.clear();
    dblwr.pages.clear();
  }
  else
  {
    mlog_init.reset();
    mysql_mutex_unlock(&log_sys.mutex);
  }

  mysql_mutex_assert_not_owner(&log_sys.mutex);
  mysql_mutex_unlock(&mutex);

  if (!last_batch)
  {
    buf_flush_sync_batch(recovered_lsn);
    buf_pool_invalidate();
    mysql_mutex_lock(&log_sys.mutex);
  }
  else if (srv_operation == SRV_OPERATION_RESTORE ||
           srv_operation == SRV_OPERATION_RESTORE_EXPORT)
    buf_flush_sync_batch(recovered_lsn);
  else
    /* Instead of flushing, last_batch could sort the buf_pool.flush_list
    in ascending order of buf_page_t::oldest_modification() */
    log_sort_flush_list();

  mysql_mutex_lock(&mutex);

  ut_d(after_apply= true);
  clear();
  mysql_mutex_unlock(&mutex);
}

/** Adds data from a new log block to the parsing buffer of recv_sys if
recv_sys.parse_start_lsn is non-zero.
@param[in]	log_block	log block to add
@param[in]	scanned_lsn	lsn of how far we were able to find
				data in this log block
@return true if more data added */
bool recv_sys_add_to_parsing_buf(const byte* log_block, lsn_t scanned_lsn)
{
	ulint	more_len;
	ulint	data_len;
	ulint	start_offset;
	ulint	end_offset;

	ut_ad(scanned_lsn >= recv_sys.scanned_lsn);

	if (!recv_sys.parse_start_lsn) {
		/* Cannot start parsing yet because no start point for
		it found */
		return(false);
	}

	data_len = log_block_get_data_len(log_block);

	if (recv_sys.parse_start_lsn >= scanned_lsn) {

		return(false);

	} else if (recv_sys.scanned_lsn >= scanned_lsn) {

		return(false);

	} else if (recv_sys.parse_start_lsn > recv_sys.scanned_lsn) {
		more_len = (ulint) (scanned_lsn - recv_sys.parse_start_lsn);
	} else {
		more_len = (ulint) (scanned_lsn - recv_sys.scanned_lsn);
	}

	if (more_len == 0) {
		return(false);
	}

	ut_ad(data_len >= more_len);

	start_offset = data_len - more_len;

	if (start_offset < LOG_BLOCK_HDR_SIZE) {
		start_offset = LOG_BLOCK_HDR_SIZE;
	}

	end_offset = std::min<ulint>(data_len, log_sys.trailer_offset());

	ut_ad(start_offset <= end_offset);

	if (start_offset < end_offset) {
		memcpy(recv_sys.buf + recv_sys.len,
		       log_block + start_offset, end_offset - start_offset);

		recv_sys.len += end_offset - start_offset;

		ut_a(recv_sys.len <= RECV_PARSING_BUF_SIZE);
	}

	return(true);
}

/** Moves the parsing buffer data left to the buffer start. */
void recv_sys_justify_left_parsing_buf()
{
	memmove(recv_sys.buf, recv_sys.buf + recv_sys.recovered_offset,
		recv_sys.len - recv_sys.recovered_offset);

	recv_sys.len -= recv_sys.recovered_offset;

	recv_sys.recovered_offset = 0;
}

/** Scan redo log from a buffer and stores new log data to the parsing buffer.
Parse and hash the log records if new data found.
Apply log records automatically when the hash table becomes full.
@param[in,out]	store			whether the records should be
					stored into recv_sys.pages; this is
					reset if just debug checking is
					needed, or when the num_max_blocks in
					recv_sys runs out
@param[in]	log_block		log segment
@param[in]	checkpoint_lsn		latest checkpoint LSN
@param[in]	start_lsn		buffer start LSN
@param[in]	end_lsn			buffer end LSN
@param[in,out]	contiguous_lsn		it is known that all groups contain
					contiguous log data upto this lsn
@param[out]	group_scanned_lsn	scanning succeeded upto this lsn
@return true if not able to scan any more in this log group */
static bool recv_scan_log_recs(
	store_t*	store,
	const byte*	log_block,
	lsn_t		checkpoint_lsn,
	lsn_t		start_lsn,
	lsn_t		end_lsn,
	lsn_t*		contiguous_lsn,
	lsn_t*		group_scanned_lsn)
{
	lsn_t		scanned_lsn	= start_lsn;
	bool		finished	= false;
	ulint		data_len;
	bool		more_data	= false;
	bool		apply		= recv_sys.mlog_checkpoint_lsn != 0;
	ulint		recv_parsing_buf_size = RECV_PARSING_BUF_SIZE;
	const store_t	old_store = *store;
	ut_ad(start_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(end_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(end_lsn >= start_lsn + OS_FILE_LOG_BLOCK_SIZE);
	ut_ad(log_sys.is_physical());

	const byte* const	log_end = log_block
		+ ulint(end_lsn - start_lsn);
	constexpr ulint sizeof_checkpoint= SIZE_OF_FILE_CHECKPOINT;

	do {
		ut_ad(!finished);

		if (log_block_get_flush_bit(log_block)) {
			/* This block was a start of a log flush operation:
			we know that the previous flush operation must have
			been completed for all log groups before this block
			can have been flushed to any of the groups. Therefore,
			we know that log data is contiguous up to scanned_lsn
			in all non-corrupt log groups. */

			if (scanned_lsn > *contiguous_lsn) {
				*contiguous_lsn = scanned_lsn;
			}
		}

		data_len = log_block_get_data_len(log_block);

		if (scanned_lsn + data_len > recv_sys.scanned_lsn
		    && log_block_get_checkpoint_no(log_block)
		    < recv_sys.scanned_checkpoint_no
		    && (recv_sys.scanned_checkpoint_no
			- log_block_get_checkpoint_no(log_block)
			> 0x80000000UL)) {

			/* Garbage from a log buffer flush which was made
			before the most recent database recovery */
			finished = true;
			break;
		}

		if (!recv_sys.parse_start_lsn
		    && (log_block_get_first_rec_group(log_block) > 0)) {

			/* We found a point from which to start the parsing
			of log records */

			recv_sys.parse_start_lsn = scanned_lsn
				+ log_block_get_first_rec_group(log_block);
			recv_sys.scanned_lsn = recv_sys.parse_start_lsn;
			recv_sys.recovered_lsn = recv_sys.parse_start_lsn;
		}

		scanned_lsn += data_len;

		if (data_len == LOG_BLOCK_HDR_SIZE + sizeof_checkpoint
		    && scanned_lsn == checkpoint_lsn + sizeof_checkpoint
		    && log_block[LOG_BLOCK_HDR_SIZE]
		    == (FILE_CHECKPOINT | (SIZE_OF_FILE_CHECKPOINT - 2))
		    && checkpoint_lsn == mach_read_from_8(
			    (LOG_BLOCK_HDR_SIZE + 1 + 2)
			    + log_block)) {
			/* The redo log is logically empty. */
			ut_ad(recv_sys.mlog_checkpoint_lsn == 0
			      || recv_sys.mlog_checkpoint_lsn
			      == checkpoint_lsn);
			recv_sys.mlog_checkpoint_lsn = checkpoint_lsn;
			DBUG_PRINT("ib_log", ("found empty log; LSN=" LSN_PF,
					      scanned_lsn));
			finished = true;
			break;
		}

		if (scanned_lsn > recv_sys.scanned_lsn) {
			ut_ad(!srv_log_file_created);
			if (!recv_needed_recovery) {
				recv_needed_recovery = true;

				if (srv_read_only_mode) {
					ib::warn() << "innodb_read_only"
						" prevents crash recovery";
					return(true);
				}

				ib::info() << "Starting crash recovery from"
					" checkpoint LSN=" << checkpoint_lsn
					   << "," << recv_sys.scanned_lsn;
			}

			/* We were able to find more log data: add it to the
			parsing buffer if parse_start_lsn is already
			non-zero */

			DBUG_EXECUTE_IF(
				"reduce_recv_parsing_buf",
				recv_parsing_buf_size = RECV_SCAN_SIZE * 2;
				);

			if (recv_sys.len + 4 * OS_FILE_LOG_BLOCK_SIZE
			    >= recv_parsing_buf_size) {
				ib::error() << "Log parsing buffer overflow."
					" Recovery may have failed!";

				recv_sys.set_corrupt_log();

				if (!srv_force_recovery) {
					ib::error()
						<< "Set innodb_force_recovery"
						" to ignore this error.";
					return(true);
				}
			} else if (!recv_sys.is_corrupt_log()) {
				more_data = recv_sys_add_to_parsing_buf(
					log_block, scanned_lsn);
			}

			recv_sys.scanned_lsn = scanned_lsn;
			recv_sys.scanned_checkpoint_no
				= log_block_get_checkpoint_no(log_block);
		}

		/* During last phase of scanning, there can be redo logs
		left in recv_sys.buf to parse & store it in recv_sys.pages */
		if (old_store == STORE_IF_EXISTS
		    && recv_sys.recovered_lsn < recv_sys.scanned_lsn) {
			more_data = true;
		}

		if (data_len < OS_FILE_LOG_BLOCK_SIZE) {
			/* Log data for this group ends here */
			finished = true;
			break;
		} else {
			log_block += OS_FILE_LOG_BLOCK_SIZE;
		}
	} while (log_block < log_end);

	*group_scanned_lsn = scanned_lsn;

	mysql_mutex_lock(&recv_sys.mutex);

	if (more_data && !recv_sys.is_corrupt_log()) {
		/* Try to parse more log records */
		if (recv_sys.parse(checkpoint_lsn, store, apply)) {
			finished = true;
			ut_ad(recv_sys.is_corrupt_log()
			      || recv_sys.is_corrupt_fs()
			      || recv_sys.mlog_checkpoint_lsn
			      == recv_sys.recovered_lsn);
		} else if (recv_sys.recovered_offset
			   > recv_parsing_buf_size / 4
			   || (recv_sys.recovered_offset
			       && recv_sys.len
			       >= recv_parsing_buf_size - RECV_SCAN_SIZE)) {
			/* Move parsing buffer data to the buffer start */
			recv_sys_justify_left_parsing_buf();
		}
	}

	mysql_mutex_unlock(&recv_sys.mutex);
	return(finished);
}

/** Scans log from a buffer and stores new log data to the parsing buffer.
Parses and hashes the log records if new data found.
@param[in]	checkpoint_lsn		latest checkpoint log sequence number
@param[in,out]	contiguous_lsn		log sequence number
until which all redo log has been scanned
@param[in]	last_phase		whether changes
can be applied to the tablespaces
@return whether rescan is needed (not everything was stored) */
static
bool
recv_group_scan_log_recs(
	lsn_t		checkpoint_lsn,
	lsn_t*		contiguous_lsn,
	bool		last_phase)
{
	DBUG_ENTER("recv_group_scan_log_recs");
	DBUG_ASSERT(!last_phase || recv_sys.mlog_checkpoint_lsn > 0);

	mysql_mutex_lock(&recv_sys.mutex);
	recv_sys.len = 0;
	recv_sys.recovered_offset = 0;
	recv_sys.clear();
	recv_sys.parse_start_lsn = *contiguous_lsn;
	recv_sys.scanned_lsn = *contiguous_lsn;
	recv_sys.recovered_lsn = *contiguous_lsn;
	recv_sys.scanned_checkpoint_no = 0;
	mysql_mutex_unlock(&recv_sys.mutex);

	lsn_t	start_lsn;
	lsn_t	end_lsn;
	store_t	store	= recv_sys.mlog_checkpoint_lsn == 0
		? STORE_NO : (last_phase ? STORE_IF_EXISTS : STORE_YES);

	log_sys.log.scanned_lsn = end_lsn = *contiguous_lsn =
		ut_uint64_align_down(*contiguous_lsn, OS_FILE_LOG_BLOCK_SIZE);

	do {
		start_lsn = ut_uint64_align_down(end_lsn,
						 OS_FILE_LOG_BLOCK_SIZE);
		end_lsn = start_lsn;
		log_sys.log.read_log_seg(&end_lsn, start_lsn + RECV_SCAN_SIZE);
	} while (end_lsn != start_lsn
		 && !recv_scan_log_recs(&store, log_sys.buf, checkpoint_lsn,
					start_lsn, end_lsn, contiguous_lsn,
					&log_sys.log.scanned_lsn));

	if (recv_sys.is_corrupt_log() || recv_sys.is_corrupt_fs()) {
		DBUG_RETURN(false);
	}

	ut_d(recv_sys.after_apply = last_phase);
	DBUG_PRINT("ib_log", ("%s " LSN_PF " completed",
			      last_phase ? "rescan" : "scan",
			      log_sys.log.scanned_lsn));

	DBUG_RETURN(store == STORE_NO);
}

/** Report a missing tablespace for which page-redo log exists.
@param[in]	err	previous error code
@param[in]	i	tablespace descriptor
@return new error code */
static
dberr_t
recv_init_missing_space(dberr_t err, const recv_spaces_t::const_iterator& i)
{
	switch (srv_operation) {
	default:
		break;
	case SRV_OPERATION_RESTORE:
	case SRV_OPERATION_RESTORE_EXPORT:
		if (i->second.name.find("/#sql") != std::string::npos) {
			ib::warn() << "Tablespace " << i->first << " was not"
				" found at " << i->second.name << " when"
				" restoring a (partial?) backup. All redo log"
				" for this file will be ignored!";
		}
		return(err);
	}

	if (srv_force_recovery == 0) {
		ib::error() << "Tablespace " << i->first << " was not"
			" found at " << i->second.name << ".";

		if (err == DB_SUCCESS) {
			ib::error() << "Set innodb_force_recovery=1 to"
				" ignore this and to permanently lose"
				" all changes to the tablespace.";
			err = DB_TABLESPACE_NOT_FOUND;
		}
	} else {
		ib::warn() << "Tablespace " << i->first << " was not"
			" found at " << i->second.name << ", and"
			" innodb_force_recovery was set. All redo log"
			" for this tablespace will be ignored!";
	}

	return(err);
}

/** Report the missing tablespace and discard the redo logs for the deleted
tablespace.
@param[in]	rescan			rescan of redo logs is needed
					if hash table ran out of memory
@param[out]	missing_tablespace	missing tablespace exists or not
@return error code or DB_SUCCESS. */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
recv_validate_tablespace(bool rescan, bool& missing_tablespace)
{
	dberr_t err = DB_SUCCESS;

	mysql_mutex_lock(&recv_sys.mutex);

	for (recv_sys_t::map::iterator p = recv_sys.pages.begin();
	     p != recv_sys.pages.end();) {
		ut_ad(!p->second.log.empty());
		const uint32_t space = p->first.space();
		if (is_predefined_tablespace(space)) {
next:
			p++;
			continue;
		}

		recv_spaces_t::iterator i = recv_spaces.find(space);
		ut_ad(i != recv_spaces.end());

		if (deferred_spaces.find(space)) {
			/* Skip redo logs belonging to
			incomplete tablespaces */
			goto next;
		}

		switch (i->second.status) {
		case file_name_t::NORMAL:
			goto next;
		case file_name_t::MISSING:
			err = recv_init_missing_space(err, i);
			i->second.status = file_name_t::DELETED;
			/* fall through */
		case file_name_t::DELETED:
			recv_sys_t::map::iterator r = p++;
			recv_sys.pages_it_invalidate(r);
			recv_sys.erase(r);
			continue;
		}
		ut_ad(0);
	}

	if (err != DB_SUCCESS) {
func_exit:
		mysql_mutex_unlock(&recv_sys.mutex);
		return(err);
	}

	/* When rescan is not needed, recv_sys.pages will contain the
	entire redo log. If rescan is needed or innodb_force_recovery
	is set, we can ignore missing tablespaces. */
	for (const recv_spaces_t::value_type& rs : recv_spaces) {
		if (UNIV_LIKELY(rs.second.status != file_name_t::MISSING)) {
			continue;
		}

		if (deferred_spaces.find(static_cast<uint32_t>(rs.first))) {
			continue;
		}

		if (srv_force_recovery > 0) {
			ib::warn() << "Tablespace " << rs.first
				<<" was not found at " << rs.second.name
				<<", and innodb_force_recovery was set."
				<<" All redo log for this tablespace"
				<<" will be ignored!";
			continue;
		}

		if (!rescan) {
			ib::info() << "Tablespace " << rs.first
				<< " was not found at '"
				<< rs.second.name << "', but there"
				<<" were no modifications either.";
		} else {
			missing_tablespace = true;
		}
	}

	goto func_exit;
}

/** Check if all tablespaces were found for crash recovery.
@param[in]	rescan			rescan of redo logs is needed
@param[out]	missing_tablespace	missing table exists
@return error code or DB_SUCCESS */
static MY_ATTRIBUTE((warn_unused_result))
dberr_t
recv_init_crash_recovery_spaces(bool rescan, bool& missing_tablespace)
{
	bool		flag_deleted	= false;

	ut_ad(!srv_read_only_mode);
	ut_ad(recv_needed_recovery);

	for (recv_spaces_t::value_type& rs : recv_spaces) {
		ut_ad(!is_predefined_tablespace(rs.first));
		ut_ad(rs.second.status != file_name_t::DELETED
		      || !rs.second.space);

		if (rs.second.status == file_name_t::DELETED) {
			/* The tablespace was deleted,
			so we can ignore any redo log for it. */
			flag_deleted = true;
		} else if (rs.second.space != NULL) {
			/* The tablespace was found, and there
			are some redo log records for it. */
			fil_names_dirty(rs.second.space);

			/* Add the freed page ranges in the respective
			tablespace */
			if (!rs.second.freed_ranges.empty()
			    && (srv_immediate_scrub_data_uncompressed
				|| rs.second.space->is_compressed())) {

				rs.second.space->add_free_ranges(
					std::move(rs.second.freed_ranges));
			}
		} else if (rs.second.name == "") {
			ib::error() << "Missing FILE_CREATE, FILE_DELETE"
				" or FILE_MODIFY before FILE_CHECKPOINT"
				" for tablespace " << rs.first;
			recv_sys.set_corrupt_log();
			return(DB_CORRUPTION);
		} else {
			rs.second.status = file_name_t::MISSING;
			flag_deleted = true;
		}

		ut_ad(rs.second.status == file_name_t::DELETED
		      || rs.second.name != "");
	}

	if (flag_deleted) {
		return recv_validate_tablespace(rescan, missing_tablespace);
	}

	return DB_SUCCESS;
}

/** Apply any FILE_RENAME records */
static dberr_t recv_rename_files()
{
  mysql_mutex_assert_owner(&recv_sys.mutex);
  mysql_mutex_assert_owner(&log_sys.mutex);

  dberr_t err= DB_SUCCESS;

  for (auto i= renamed_spaces.begin(); i != renamed_spaces.end(); )
  {
    const auto &r= *i;
    const uint32_t id= r.first;
    fil_space_t *space= fil_space_t::get(id);
    if (!space)
    {
      i++;
      continue;
    }
    ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
    char *old= space->chain.start->name;
    if (r.second != old)
    {
      bool exists;
      os_file_type_t ftype;
      const char *new_name= r.second.c_str();
      mysql_mutex_lock(&fil_system.mutex);
      const fil_space_t *other= nullptr;
      if (!space->chain.start->is_open() && space->chain.start->deferred &&
          (other= fil_system.find(new_name)) &&
          (other->chain.start->is_open() || !other->chain.start->deferred))
        other= nullptr;

      if (other)
      {
        /* Multiple tablespaces use the same file name. This should
        only be possible if the recovery of both files was deferred
        (no valid page 0 is contained in either file). We shall not
        rename the file, just rename the metadata. */
        ib::info() << "Renaming tablespace metadata " << id
                   << " from '" << old << "' to '" << r.second
                   << "' that is also associated with tablespace "
                   << other->id;
        space->chain.start->name= mem_strdup(new_name);
        ut_free(old);
      }
      else if (!os_file_status(new_name, &exists, &ftype) || exists)
      {
        ib::error() << "Cannot replay rename of tablespace " << id
                    << " from '" << old << "' to '" << r.second <<
                    (exists ? "' because the target file exists" : "'");
        err= DB_TABLESPACE_EXISTS;
      }
      else
      {
        mysql_mutex_unlock(&fil_system.mutex);
        err= space->rename(new_name, false);
        if (err != DB_SUCCESS)
          ib::error() << "Cannot replay rename of tablespace " << id
                      << " to '" << r.second << "': " << err;
        goto done;
      }
      mysql_mutex_unlock(&fil_system.mutex);
    }
done:
    space->release();
    if (err != DB_SUCCESS)
    {
      recv_sys.set_corrupt_fs();
      break;
    }
    renamed_spaces.erase(i++);
  }
  return err;
}

dberr_t recv_recovery_read_max_checkpoint()
{
  ut_ad(srv_operation <= SRV_OPERATION_EXPORT_RESTORED ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_EXPORT);
  ut_d(mysql_mutex_lock(&buf_pool.mutex));
  ut_ad(UT_LIST_GET_LEN(buf_pool.LRU) == 0);
  ut_ad(UT_LIST_GET_LEN(buf_pool.unzip_LRU) == 0);
  ut_d(mysql_mutex_unlock(&buf_pool.mutex));

  if (srv_force_recovery >= SRV_FORCE_NO_LOG_REDO)
  {
    sql_print_information("InnoDB: innodb_force_recovery=6 skips redo log apply");
    return DB_SUCCESS;
  }

  mysql_mutex_lock(&log_sys.mutex);
  ulint max_cp;
  dberr_t err= recv_find_max_checkpoint(&max_cp);

  if (err != DB_SUCCESS)
    recv_sys.recovered_lsn= log_sys.get_lsn();
  else
  {
    byte* buf= log_sys.checkpoint_buf;
    err= log_sys.log.read(max_cp, {buf, OS_FILE_LOG_BLOCK_SIZE});
    if (err == DB_SUCCESS)
    {
      log_sys.next_checkpoint_no=
        mach_read_from_8(buf + LOG_CHECKPOINT_NO);
      log_sys.next_checkpoint_lsn=
        mach_read_from_8(buf + LOG_CHECKPOINT_LSN);
      recv_sys.mlog_checkpoint_lsn=
        mach_read_from_8(buf + LOG_CHECKPOINT_END_LSN);
    }
  }
  mysql_mutex_unlock(&log_sys.mutex);
  return err;
}

inline
bool recv_sys_t::validate_checkpoint(lsn_t start_lsn, lsn_t end_lsn) const
{
  if (recovered_lsn >= start_lsn && recovered_lsn >= end_lsn)
    return false;
  sql_print_error("InnoDB: The log was only scanned up to "
                  LSN_PF ", while the current LSN at the "
                  "time of the latest checkpoint " LSN_PF
                  " was " LSN_PF "!",
                  recovered_lsn, start_lsn, end_lsn);
  return true;
}

/** Start recovering from a redo log checkpoint.
@param[in]	flush_lsn	FIL_PAGE_FILE_FLUSH_LSN
of first system tablespace page
@return error code or DB_SUCCESS */
dberr_t
recv_recovery_from_checkpoint_start(lsn_t flush_lsn)
{
	bool		rescan = false;
	lsn_t		contiguous_lsn;
	dberr_t		err = DB_SUCCESS;

	ut_ad(srv_operation <= SRV_OPERATION_EXPORT_RESTORED
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);
	ut_d(mysql_mutex_lock(&buf_pool.flush_list_mutex));
	ut_ad(UT_LIST_GET_LEN(buf_pool.LRU) == 0);
	ut_ad(UT_LIST_GET_LEN(buf_pool.unzip_LRU) == 0);
	ut_d(mysql_mutex_unlock(&buf_pool.flush_list_mutex));

	if (srv_force_recovery >= SRV_FORCE_NO_LOG_REDO) {
		sql_print_information("InnoDB: innodb_force_recovery=6"
				      " skips redo log apply");
		return err;
	}

	mysql_mutex_lock(&log_sys.mutex);
	uint64_t checkpoint_no= log_sys.next_checkpoint_no;
	lsn_t checkpoint_lsn= log_sys.next_checkpoint_lsn;
	const lsn_t end_lsn= recv_sys.mlog_checkpoint_lsn;
	recv_sys.recovery_on = true;

	/* Start reading the log from the checkpoint lsn. The variable
	contiguous_lsn contains an lsn up to which the log is known to
	be contiguously written. */
	recv_sys.mlog_checkpoint_lsn = 0;

	ut_ad(RECV_SCAN_SIZE <= srv_log_buffer_size);

	ut_ad(recv_sys.pages.empty());
	contiguous_lsn = checkpoint_lsn;
	switch (log_sys.log.format) {
	default:
		if (end_lsn == 0) {
			break;
		}
		if (end_lsn >= checkpoint_lsn) {
			contiguous_lsn = end_lsn;
			break;
		}
		recv_sys.set_corrupt_log();
	err_exit:
		err = DB_ERROR;
		/* fall through */
	func_exit:
	case 0:
		mysql_mutex_unlock(&log_sys.mutex);
		return err;
	}

	size_t sizeof_checkpoint;

	if (!log_sys.is_physical()) {
		sizeof_checkpoint = 9/* size of MLOG_CHECKPOINT */;
		goto completed;
	}

	/* Look for FILE_CHECKPOINT. */
	recv_group_scan_log_recs(checkpoint_lsn, &contiguous_lsn, false);
	/* The first scan should not have stored or applied any records. */
	ut_ad(recv_sys.pages.empty());
	ut_ad(!recv_sys.is_corrupt_fs() || !srv_force_recovery);

	if (srv_read_only_mode && recv_needed_recovery) {
	read_only:
		err = DB_READ_ONLY;
		goto func_exit;
	}

	if (recv_sys.is_corrupt_log() && !srv_force_recovery) {
		sql_print_warning("InnoDB: Log scan aborted at LSN " LSN_PF,
				  contiguous_lsn);
		goto err_exit;
	}

	/* If we fail to open a tablespace while looking for FILE_CHECKPOINT, we
	set the corruption flag. Specifically, if encryption key is missing, we
	would not be able to open an encrypted tablespace and the flag could be
	set. */
	if (recv_sys.is_corrupt_fs()) {
		goto err_exit;
	}

	if (recv_sys.mlog_checkpoint_lsn == 0) {
		lsn_t scan_lsn = log_sys.log.scanned_lsn;
		if (!srv_read_only_mode && scan_lsn != checkpoint_lsn) {
			ib::error err;
			err << "Missing FILE_CHECKPOINT";
			if (end_lsn) {
				err << " at " << end_lsn;
			}
			err << " between the checkpoint " << checkpoint_lsn
			    << " and the end " << scan_lsn << ".";
			goto err_exit;
		}

		log_sys.log.scanned_lsn = checkpoint_lsn;
	} else {
		contiguous_lsn = checkpoint_lsn;
		rescan = recv_group_scan_log_recs(
			checkpoint_lsn, &contiguous_lsn, false);

		if ((recv_sys.is_corrupt_log() && !srv_force_recovery)
		    || recv_sys.is_corrupt_fs()) {
			goto err_exit;
		}
	}

	/* NOTE: we always do a 'recovery' at startup, but only if
	there is something wrong we will print a message to the
	user about recovery: */
	sizeof_checkpoint= SIZE_OF_FILE_CHECKPOINT;

completed:
	if (flush_lsn == checkpoint_lsn + sizeof_checkpoint
	    && recv_sys.mlog_checkpoint_lsn == checkpoint_lsn) {
		/* The redo log is logically empty. */
	} else if (checkpoint_lsn != flush_lsn) {
		ut_ad(!srv_log_file_created);

		if (checkpoint_lsn + sizeof_checkpoint
		    + log_sys.framing_size() < flush_lsn) {
			ib::warn()
				<< "Are you sure you are using the right "
				<< LOG_FILE_NAME
				<< " to start up the database? Log sequence "
				   "number in the "
				<< LOG_FILE_NAME << " is " << checkpoint_lsn
				<< ", less than the log sequence number in "
				   "the first system tablespace file header, "
				<< flush_lsn << ".";
		}

		if (!recv_needed_recovery) {
			sql_print_information(
				"InnoDB: The log sequence number " LSN_PF
				" in the system tablespace does not match"
				" the log sequence number " LSN_PF
				" in the ib_logfile0!",
				flush_lsn, checkpoint_lsn);

			if (srv_read_only_mode) {
				sql_print_error("InnoDB: innodb_read_only"
						" prevents crash recovery");
				goto read_only;
			}

			recv_needed_recovery = true;
		}
	}

	log_sys.set_lsn(recv_sys.recovered_lsn);
	if (UNIV_LIKELY(log_sys.get_flushed_lsn() < recv_sys.recovered_lsn)) {
		/* This may already have been set by create_log_file()
		if no logs existed when the server started up. */
		log_sys.set_flushed_lsn(recv_sys.recovered_lsn);
	}

	if (recv_needed_recovery) {
		bool missing_tablespace = false;

		err = recv_init_crash_recovery_spaces(
			rescan, missing_tablespace);

		if (err != DB_SUCCESS) {
			goto func_exit;
		}

		/* If there is any missing tablespace and rescan is needed
		then there is a possiblity that hash table will not contain
		all space ids redo logs. Rescan the remaining unstored
		redo logs for the validation of missing tablespace. */
		ut_ad(rescan || !missing_tablespace);

		while (missing_tablespace) {
			DBUG_PRINT("ib_log", ("Rescan of redo log to validate "
					      "the missing tablespace. Scan "
					      "from last stored LSN " LSN_PF,
					      recv_sys.last_stored_lsn));

			lsn_t recent_stored_lsn = recv_sys.last_stored_lsn;
			rescan = recv_group_scan_log_recs(
				checkpoint_lsn, &recent_stored_lsn, false);

			ut_ad(!recv_sys.is_corrupt_fs());

			missing_tablespace = false;

			err = recv_sys.is_corrupt_log()
				? DB_ERROR
				: recv_validate_tablespace(
					rescan, missing_tablespace);

			if (err != DB_SUCCESS) {
				goto func_exit;
			}

			rescan = true;
		}

		ut_ad(log_sys.get_lsn() >= recv_sys.scanned_lsn
		      || log_sys.get_lsn() >= recv_sys.recovered_lsn);

		recv_sys.parse_start_lsn = checkpoint_lsn;

		if (srv_operation <= SRV_OPERATION_EXPORT_RESTORED) {
			mysql_mutex_lock(&recv_sys.mutex);
			deferred_spaces.deferred_dblwr(log_sys.get_lsn());
			buf_dblwr.recover();
			mysql_mutex_unlock(&recv_sys.mutex);
		}

		ut_ad(srv_force_recovery <= SRV_FORCE_NO_UNDO_LOG_SCAN);

		if (rescan) {
			contiguous_lsn = checkpoint_lsn;

			recv_group_scan_log_recs(
				checkpoint_lsn, &contiguous_lsn, true);

			if ((recv_sys.is_corrupt_log()
			     && !srv_force_recovery)
			    || recv_sys.is_corrupt_fs()) {
				goto err_exit;
			}

			ut_ad(contiguous_lsn <= recv_sys.recovered_lsn);

			ut_ad(log_sys.get_lsn() >= recv_sys.recovered_lsn);
			ut_ad(log_sys.get_flushed_lsn()
			      >= recv_sys.recovered_lsn);

			/* In case of multi-batch recovery,
			redo log for the last batch is not
			applied yet. */
			ut_d(recv_sys.after_apply = false);
		}
	} else {
		ut_ad(!rescan || recv_sys.pages.empty());
	}

	if (!log_sys.is_physical()) {
	} else if (recv_sys.validate_checkpoint(checkpoint_lsn, end_lsn)) {
		goto err_exit;
	}

	log_sys.next_checkpoint_lsn = checkpoint_lsn;
	log_sys.next_checkpoint_no = checkpoint_no + 1;

	recv_synchronize_groups();

	ut_ad(recv_needed_recovery
	      || checkpoint_lsn == recv_sys.recovered_lsn);

	log_sys.write_lsn = log_sys.get_lsn();
	log_sys.buf_free = log_sys.write_lsn % OS_FILE_LOG_BLOCK_SIZE;
	log_sys.buf_next_to_write = log_sys.buf_free;

	log_sys.last_checkpoint_lsn = checkpoint_lsn;

	if (!srv_read_only_mode
            && srv_operation <= SRV_OPERATION_EXPORT_RESTORED
	    && (~log_t::FORMAT_ENCRYPTED & log_sys.log.format)
	    == log_t::FORMAT_10_5
	    && recv_sys.recovered_lsn - log_sys.last_checkpoint_lsn
	    < log_sys.log_capacity) {
		/* Write a FILE_CHECKPOINT marker as the first thing,
		before generating any other redo log. This ensures
		that subsequent crash recovery will be possible even
		if the server were killed soon after this. */
		fil_names_clear(log_sys.last_checkpoint_lsn, true);
	}

	log_sys.next_checkpoint_no = ++checkpoint_no;

	DBUG_EXECUTE_IF("before_final_redo_apply", goto err_exit;);
	mysql_mutex_lock(&recv_sys.mutex);
	recv_sys.apply_log_recs = true;
	recv_no_ibuf_operations = false;
	ut_d(recv_no_log_write = srv_operation == SRV_OPERATION_RESTORE
	     || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	if (srv_operation == SRV_OPERATION_NORMAL) {
		err = recv_rename_files();
	}

	mysql_mutex_unlock(&recv_sys.mutex);

	/* The database is now ready to start almost normal processing of user
	transactions: transaction rollbacks and the application of the log
	records in the hash table can be run in background. */
	if (err == DB_SUCCESS && deferred_spaces.reinit_all()
	    && !srv_force_recovery) {
		err = DB_CORRUPTION;
	}
	goto func_exit;
}

bool recv_dblwr_t::validate_page(const page_id_t page_id, lsn_t max_lsn,
                                 const fil_space_t *space,
                                 const byte *page, byte *tmp_buf)
  const noexcept
{
  mysql_mutex_assert_owner(&recv_sys.mutex);
  ulint flags;

  if (page_id.page_no() == 0)
  {
    flags= fsp_header_get_flags(page);
    if (!fil_space_t::is_valid_flags(flags, page_id.space()))
    {
      ulint cflags= fsp_flags_convert_from_101(flags);
      if (cflags == ULINT_UNDEFINED)
      {
        ib::warn() << "Ignoring a doublewrite copy of page " << page_id
                   << "due to invalid flags " << ib::hex(flags);
        return false;
      }

      flags= cflags;
    }

    /* Page 0 is never page_compressed or encrypted. */
    goto check_if_corrupted;
  }

  flags= space->flags;

  if (space->full_crc32())
  check_if_corrupted:
    return !buf_page_is_corrupted(max_lsn < LSN_MAX, page, flags);

  ut_ad(tmp_buf);
  byte *tmp_frame= tmp_buf;
  byte *tmp_page= tmp_buf + srv_page_size;
  const uint16_t page_type= mach_read_from_2(page + FIL_PAGE_TYPE);
  const bool expect_encrypted= space->crypt_data &&
    space->crypt_data->type != CRYPT_SCHEME_UNENCRYPTED;

  if (expect_encrypted &&
      mach_read_from_4(page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION))
  {
    if (!fil_space_verify_crypt_checksum(page, space->zip_size()))
      return false;
    if (page_type != FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED)
      return true;
    if (space->zip_size())
      return false;
    memcpy(tmp_page, page, space->physical_size());
    if (!fil_space_decrypt(space, tmp_frame, tmp_page))
      return false;
  }

  switch (page_type) {
  case FIL_PAGE_PAGE_COMPRESSED:
    memcpy(tmp_page, page, space->physical_size());
    /* fall through */
  case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
    if (space->zip_size())
      return false; /* ROW_FORMAT=COMPRESSED cannot be page_compressed */
    ulint decomp= fil_page_decompress(tmp_frame, tmp_page, space->flags);
    if (!decomp)
      return false; /* decompression failed */
    if (decomp == srv_page_size)
      return false; /* the page was not compressed (invalid page type) */
    page= tmp_page;
  }

  goto check_if_corrupted;
}

ATTRIBUTE_COLD
byte *recv_dblwr_t::find_deferred_page(const fil_node_t &node,
                                       uint32_t page_no,
                                       byte *buf) noexcept
{
  ut_ad(node.space->full_crc32());
  mysql_mutex_lock(&recv_sys.mutex);
  byte *result_page= nullptr;
  bool is_encrypted= node.space->crypt_data &&
                     node.space->crypt_data->is_encrypted();
  for (list::iterator page_it= pages.begin(); page_it != pages.end();
       page_it++)
  {
    if (page_get_page_no(*page_it) != page_no ||
        buf_page_is_corrupted(true, *page_it, node.space->flags))
      continue;

    if (is_encrypted &&
        !mach_read_from_4(*page_it + FIL_PAGE_FCRC32_KEY_VERSION))
      continue;

    memcpy(buf, *page_it, node.space->physical_size());
    buf_tmp_buffer_t *slot= buf_pool.io_buf_reserve(false);
    ut_a(slot);
    slot->allocate();

    bool invalidate= false;
    if (is_encrypted)
    {
      invalidate= !fil_space_decrypt(node.space, slot->crypt_buf, buf);
      if (!invalidate && node.space->is_compressed())
        goto decompress;
    }
    else
decompress:
      invalidate= !fil_page_decompress(slot->crypt_buf, buf,
                                       node.space->flags);
    slot->release();

    if (invalidate ||
        mach_read_from_4(buf + FIL_PAGE_SPACE_ID) != node.space->id)
      continue;

    result_page= *page_it;
    pages.erase(page_it);
    break;
  }
  mysql_mutex_unlock(&recv_sys.mutex);
  if (result_page)
    sql_print_information("InnoDB: Recovered page [page id: space="
                          UINT32PF ", page number=" UINT32PF "] "
                          "to '%s' from the doublewrite buffer.",
                          uint32_t(node.space->id), page_no,
                          node.name);
  return result_page;
}

const byte *recv_dblwr_t::find_page(const page_id_t page_id, lsn_t max_lsn,
                                    const fil_space_t *space, byte *tmp_buf)
  const noexcept
{
  mysql_mutex_assert_owner(&recv_sys.mutex);
  ut_ad(recv_sys.recovered_lsn <= max_lsn);

  for (byte *page : pages)
  {
    if (page_get_page_no(page) != page_id.page_no() ||
        page_get_space_id(page) != page_id.space())
      continue;
    const lsn_t lsn= mach_read_from_8(page + FIL_PAGE_LSN);
    if (page_id.page_no() == 0)
    {
      if (!lsn)
        continue;
      uint32_t flags= mach_read_from_4(
        FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page);
      if (!fil_space_t::is_valid_flags(flags, page_id.space()))
        continue;
    }

    if (lsn > max_lsn || lsn < recv_sys.parse_start_lsn ||
        !validate_page(page_id, max_lsn, space, page, tmp_buf))
    {
      /* Mark processed for subsequent iterations in buf_dblwr_t::recover() */
      memset_aligned<8>(page + FIL_PAGE_LSN, 0, 8);
      continue;
    }

    return page;
  }

  return nullptr;
}
