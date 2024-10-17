/*****************************************************************************

Copyright (c) 1997, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2023, MariaDB Corporation.

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
Protected by log_sys.latch. */
bool	recv_no_log_write = false;
#endif /* UNIV_DEBUG */

/** TRUE if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially FALSE, and set by
recv_recovery_from_checkpoint_start(). */
bool	recv_lsn_checks_on;

/** The maximum lsn we see for a page during the recovery process. If this
is bigger than the lsn we are able to scan up to, that is an indication that
the recovery failed and the database may be corrupt. */
static lsn_t	recv_max_page_lsn;

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
	std::less<uint32_t>,
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
      if (const char *name= strrchr(filename, '/'))
      {
        while (--name > filename && *name != '/');
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
    log_sys.latch.wr_unlock();
    fil_space_t *space= fil_system.sys_space;
    buf_block_t *free_block= buf_LRU_get_free_block(have_no_mutex);
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
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
    fil_space_t *space= fil_space_t::create(it->first, flags,
                                            FIL_TYPE_TABLESPACE, crypt_data);
    ut_ad(space);
    const char *filename= name.c_str();
    if (srv_operation == SRV_OPERATION_RESTORE)
    {
      if (const char *tbl_name= strrchr(filename, '/'))
      {
        while (--tbl_name > filename && *tbl_name != '/');
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
                             OS_FILE_CREATE_SILENT,
                             OS_FILE_AIO, OS_DATA_FILE, false, &success);
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
  void deferred_dblwr()
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
      const byte *page= recv_sys.dblwr.find_page(page_id);
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
		    const byte* name, ulint len,
		    const byte* new_name, ulint new_len);

void (*undo_space_trunc)(uint32_t space_id);

void (*first_page_init)(uint32_t space_id);

/** Information about initializing page contents during redo log processing.
FIXME: Rely on recv_sys.pages! */
class mlog_init_t
{
  using map= std::map<const page_id_t, lsn_t,
                      std::less<const page_id_t>,
                      ut_allocator<std::pair<const page_id_t, lsn_t>>>;
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
    std::pair<map::iterator, bool> p=
      inits.emplace(map::value_type{page_id, lsn});
    if (p.second) return true;
    if (p.first->second >= lsn) return false;
    p.first->second= lsn;
    i= p.first;
    return true;
  }

  /** Get the last initialization lsn of a page.
  @param page_id    page identifier
  @return the latest page initialization;
  not valid after releasing recv_sys.mutex. */
  lsn_t last(page_id_t page_id)
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
      return i->second > lsn;
    i= inits.lower_bound(page_id);
    return i != inits.end() && i->first == page_id && i->second > lsn;
  }

  /** Clear the data structure */
  void clear() { inits.clear(); i= inits.end(); }
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
    lsn_t init_lsn= mlog_init.last(p->first);
    mysql_mutex_unlock(&mutex);
    buf_block_t *block= recover_low(p, mtr, free_block, init_lsn);
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

inline dberr_t recv_sys_t::read(os_offset_t total_offset, span<byte> buf)
{
  size_t file_idx= static_cast<size_t>(total_offset / log_sys.file_size);
  os_offset_t offset= total_offset % log_sys.file_size;
  return file_idx
    ? recv_sys.files[file_idx].read(offset, buf)
    : log_sys.log.read(offset, buf);
}

inline size_t recv_sys_t::files_size()
{
  ut_ad(!files.empty());
  return files.size();
}

/** Process a file name from a FILE_* record.
@param[in]	name		file name
@param[in]	len		length of the file name
@param[in]	space_id	the tablespace ID
@param[in]	ftype		FILE_MODIFY, FILE_DELETE, or FILE_RENAME
@param[in]	lsn		lsn of the redo log
@param[in]	if_exists	whether to check if the tablespace exists */
static void fil_name_process(const char *name, ulint len, uint32_t space_id,
                             mfile_type_t ftype, lsn_t lsn, bool if_exists)
{
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
				sql_print_error("InnoDB: Tablespace " UINT32PF
						" has been found"
						" in two places:"
						" '%.*s' and '%.*s'."
						" You must delete"
						" one of them.",
						space_id,
						int(f.name.size()),
						f.name.data(),
						int(fname.name.size()),
						fname.name.data());
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

				sql_print_information(
					"InnoDB: At LSN: " LSN_PF
					": unable to open file %.*s"
					" for tablespace " UINT32PF,
					recv_sys.lsn,
					int(fname.name.size()),
					fname.name.data(), space_id);
			}
			break;

		case FIL_LOAD_DEFER:
			if (d && ftype == FILE_RENAME
			    && srv_operation == SRV_OPERATION_RESTORE) {
				goto rename;
			}
			/* Skip the deferred spaces
			when lsn is already processed */
			if (!if_exists) {
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

void recv_sys_t::close_files()
{
  for (auto &file : files)
    if (file.is_opened())
      file.close();
  files.clear();
  files.shrink_to_fit();
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

    scanned_lsn= 0;
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

	len = 0;
	offset = 0;
	lsn = 0;
	scanned_lsn = 1;
	found_corrupt_log = false;
	found_corrupt_fs = false;
	file_checkpoint = 0;

	progress_time = time(NULL);
	ut_ad(pages.empty());
	pages_it = pages.end();
	recv_max_page_lsn = 0;

	memset(truncated_undo_spaces, 0, sizeof truncated_undo_spaces);
	truncated_sys_space= {0, 0};
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

  mysql_mutex_unlock(&mutex);
  log_sys.clear_mmap();
}


/** Free a redo log snippet.
@param data buffer allocated in add() */
inline void recv_sys_t::free(const void *data)
{
  ut_ad(!ut_align_offset(data, ALIGNMENT));
  data= page_align(data);
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
    ut_ad(block->page.frame == data);
    ut_ad(block->page.state() == buf_page_t::MEMORY);
    ut_ad(static_cast<uint16_t>(block->page.access_time - 1) <
          srv_page_size);
    unsigned a= block->page.access_time;
    ut_ad(a >= 1U << 16);
    a-= 1U << 16;
    block->page.access_time= a;
    if (!(a >> 16))
    {
      UT_LIST_REMOVE(blocks, block);
      MEM_MAKE_ADDRESSABLE(block->page.frame, srv_page_size);
      buf_block_free(block);
    }
    return;
  }
  ut_ad(0);
}


/** @return whether a log_t::FORMAT_10_5 log block checksum matches */
static bool recv_check_log_block(const byte *buf)
{
  return mach_read_from_4(my_assume_aligned<4>(508 + buf)) ==
    my_crc32c(0, buf, 508);
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

  ut_ad(log_sys.format == 0);

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

  lsn_t source_offset= 0;
  const lsn_t log_size{(log_sys.file_size - 2048) * recv_sys.files_size()};
  for (size_t field= 512; field < 2048; field+= 1024)
  {
    const byte *buf= log_sys.buf + field;

    if (static_cast<uint32_t>(ut_fold_binary(buf, CHECKSUM_1)) !=
        mach_read_from_4(buf + CHECKSUM_1) ||
        static_cast<uint32_t>(ut_fold_binary(buf + CHECKPOINT_LSN,
                                             CHECKSUM_2 - CHECKPOINT_LSN)) !=
        mach_read_from_4(buf + CHECKSUM_2))
    {
      DBUG_PRINT("ib_log", ("invalid pre-10.2.2 checkpoint %zu", field));
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

    if (checkpoint_no < max_no)
      continue;

    const lsn_t o= lsn_t{mach_read_from_4(buf + OFFS_HI)} << 32 |
      mach_read_from_4(buf + OFFS_LO);
    if (o >= 0x80c && (o & ~511) + 512 < log_size)
    {
      max_no= checkpoint_no;
      log_sys.next_checkpoint_lsn= mach_read_from_8(buf + CHECKPOINT_LSN);
      source_offset= o;
    }
  }

  const char *uag= srv_operation == SRV_OPERATION_NORMAL
    ? "InnoDB: Upgrade after a crash is not supported."
    : "mariadb-backup --prepare is not possible.";

  if (!log_sys.next_checkpoint_lsn)
  {
    sql_print_error("%s"
                    " This redo log was created before MariaDB 10.2.2,"
                    " and we did not find a valid checkpoint."
                    " Please follow the instructions at"
                    " https://mariadb.com/kb/en/library/upgrading/", uag);
    return DB_ERROR;
  }

  static const char pre_10_2[]=
    " This redo log was created before MariaDB 10.2.2";

  byte *buf= const_cast<byte*>(field_ref_zero);

  if (source_offset < (log_sys.is_mmap() ? log_sys.file_size : 4096))
    memcpy_aligned<512>(buf, &log_sys.buf[source_offset & ~511], 512);
  else
    if (dberr_t err= recv_sys.read(source_offset & ~511, {buf, 512}))
      return err;

  if (log_block_calc_checksum_format_0(buf) !=
      mach_read_from_4(my_assume_aligned<4>(buf + 508)) &&
      !log_crypt_101_read_block(buf, log_sys.next_checkpoint_lsn))
  {
    sql_print_error("%s%s, and it appears corrupted.", uag, pre_10_2);
    return DB_CORRUPTION;
  }

  if (mach_read_from_2(buf + 4) == (source_offset & 511))
    return DB_SUCCESS;

  if (buf[20 + 32 * 9] == 2)
    sql_print_error("InnoDB: Cannot decrypt log for upgrading."
                    " The encrypted log was created before MariaDB 10.2.2.");
  else
    sql_print_error("%s%s. You must start up and shut down"
                    " MariaDB 10.1 or MySQL 5.6 or earlier"
                    " on the data directory.",
                    uag, pre_10_2);

  return DB_ERROR;
}

/** Determine if a redo log from MariaDB 10.2.2, 10.3, 10.4, or 10.5 is clean.
@param lsn_offset  checkpoint LSN offset
@return	error code
@retval	DB_SUCCESS	if the redo log is clean
@retval	DB_CORRUPTION	if the redo log is corrupted
@retval	DB_ERROR	if the redo log is not empty */
static dberr_t recv_log_recover_10_5(lsn_t lsn_offset)
{
  byte *buf= const_cast<byte*>(field_ref_zero);

  if (lsn_offset < (log_sys.is_mmap() ? log_sys.file_size : 4096))
    memcpy_aligned<512>(buf, &log_sys.buf[lsn_offset & ~511], 512);
  else
  {
    if (dberr_t err= recv_sys.read(lsn_offset & ~lsn_t{4095}, {buf, 4096}))
      return err;
    buf+= lsn_offset & 0xe00;
  }

  if (!recv_check_log_block(buf))
  {
    sql_print_error("InnoDB: Invalid log header checksum");
    return DB_CORRUPTION;
  }

  if (log_sys.is_encrypted() &&
      !log_decrypt(buf, log_sys.next_checkpoint_lsn & ~511, 512))
    return DB_ERROR;

  /* On a clean shutdown, the redo log will be logically empty
  after the checkpoint lsn. */

  if (mach_read_from_2(my_assume_aligned<2>(buf + 4)) != (lsn_offset & 511))
    return DB_ERROR;

  return DB_SUCCESS;
}

dberr_t recv_sys_t::find_checkpoint()
{
  bool wrong_size= false;
  byte *buf;

  ut_ad(pages.empty());
  pages_it= pages.end();

  if (files.empty())
  {
    file_checkpoint= 0;
    std::string path{get_log_file_path()};
    bool success;
    os_file_t file{os_file_create_func(path.c_str(),
                                       OS_FILE_OPEN,
                                       OS_FILE_NORMAL, OS_LOG_FILE,
                                       srv_read_only_mode, &success)};
    if (file == OS_FILE_CLOSED)
      return DB_ERROR;
    const os_offset_t size{os_file_get_size(file)};
    if (!size)
    {
      if (srv_operation != SRV_OPERATION_NORMAL)
        goto too_small;
    }
    else if (size < log_t::START_OFFSET + SIZE_OF_FILE_CHECKPOINT)
    {
    too_small:
      sql_print_error("InnoDB: File %.*s is too small",
                      int(path.size()), path.data());
    err_exit:
      os_file_close(file);
      return DB_ERROR;
    }
    else if (!log_sys.attach(file, size))
      goto err_exit;
    else
      file= OS_FILE_CLOSED;

    recv_sys.files.emplace_back(file);
    for (int i= 1; i < 101; i++)
    {
      path= get_log_file_path(LOG_FILE_NAME_PREFIX).append(std::to_string(i));
      file= os_file_create_func(path.c_str(),
                                OS_FILE_OPEN_SILENT,
                                OS_FILE_NORMAL, OS_LOG_FILE, true, &success);
      if (file == OS_FILE_CLOSED)
        break;
      const os_offset_t sz{os_file_get_size(file)};
      if (size != sz)
      {
        sql_print_error("InnoDB: Log file %.*s is of different size " UINT64PF
                        " bytes than other log files " UINT64PF " bytes!",
                        int(path.size()), path.data(), sz, size);
        wrong_size= true;
      }
      recv_sys.files.emplace_back(file);
    }

    if (!size)
    {
      if (wrong_size)
        return DB_CORRUPTION;
      lsn= log_sys.next_checkpoint_lsn;
      log_sys.format= log_t::FORMAT_3_23;
      goto upgrade;
    }
  }
  else
    ut_ad(srv_operation == SRV_OPERATION_BACKUP);
  log_sys.next_checkpoint_lsn= 0;
  lsn= 0;
  buf= my_assume_aligned<4096>(log_sys.buf);
  if (!log_sys.is_mmap())
    if (dberr_t err= log_sys.log.read(0, {buf, log_sys.START_OFFSET}))
      return err;
  /* Check the header page checksum. There was no
  checksum in the first redo log format (version 0). */
  log_sys.format= mach_read_from_4(buf + LOG_HEADER_FORMAT);
  if (log_sys.format == log_t::FORMAT_3_23)
  {
    if (wrong_size)
      return DB_CORRUPTION;
    if (dberr_t err= recv_log_recover_pre_10_2())
      return err;
  upgrade:
    memset_aligned<4096>(const_cast<byte*>(field_ref_zero), 0, 4096);
    /* Mark the redo log for upgrading. */
    log_sys.last_checkpoint_lsn= log_sys.next_checkpoint_lsn;
    log_sys.set_recovered_lsn(log_sys.next_checkpoint_lsn);
    lsn= file_checkpoint= log_sys.next_checkpoint_lsn;
    log_sys.next_checkpoint_no= 0;
    return DB_SUCCESS;
  }

  if (!recv_check_log_block(buf))
  {
    sql_print_error("InnoDB: Invalid log header checksum");
    return DB_CORRUPTION;
  }

  const lsn_t first_lsn{mach_read_from_8(buf + LOG_HEADER_START_LSN)};
  log_sys.set_first_lsn(first_lsn);
  char creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR + 1];
  memcpy(creator, buf + LOG_HEADER_CREATOR, sizeof creator);
  /* Ensure that the string is NUL-terminated. */
  creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR]= 0;

  lsn_t lsn_offset= 0;

  switch (log_sys.format) {
  default:
    sql_print_error("InnoDB: Unsupported redo log format."
                    " The redo log was created with %s.", creator);
    return DB_ERROR;
  case log_t::FORMAT_10_8:
    if (files.size() != 1)
    {
      sql_print_error("InnoDB: Expecting only ib_logfile0");
      return DB_CORRUPTION;
    }

    if (*reinterpret_cast<const uint32_t*>(buf + LOG_HEADER_FORMAT + 4) ||
        first_lsn < log_t::FIRST_LSN)
    {
      sql_print_error("InnoDB: Invalid ib_logfile0 header block;"
                      " the log was created with %s.", creator);
      return DB_CORRUPTION;
    }

    if (!mach_read_from_4(buf + LOG_HEADER_CREATOR_END));
    else if (!log_crypt_read_header(buf + LOG_HEADER_CREATOR_END))
    {
      sql_print_error("InnoDB: Reading log encryption info failed;"
                      " the log was created with %s.", creator);
      return DB_ERROR;
    }
    else
      log_sys.format= log_t::FORMAT_ENC_10_8;

    for (size_t field= log_t::CHECKPOINT_1; field <= log_t::CHECKPOINT_2;
         field+= log_t::CHECKPOINT_2 - log_t::CHECKPOINT_1)
    {
      buf= log_sys.buf + field;
      const lsn_t checkpoint_lsn{mach_read_from_8(buf)};
      const lsn_t end_lsn{mach_read_from_8(buf + 8)};
      if (checkpoint_lsn < first_lsn || end_lsn < checkpoint_lsn ||
          memcmp(buf + 16, field_ref_zero, 60 - 16) ||
          my_crc32c(0, buf, 60) != mach_read_from_4(buf + 60))
      {
        DBUG_PRINT("ib_log", ("invalid checkpoint at %zu", field));
        continue;
      }

      if (checkpoint_lsn >= log_sys.next_checkpoint_lsn)
      {
        log_sys.next_checkpoint_lsn= checkpoint_lsn;
        log_sys.next_checkpoint_no= field == log_t::CHECKPOINT_1;
        lsn= end_lsn;
      }
    }
    if (!log_sys.next_checkpoint_lsn)
      goto got_no_checkpoint;
    if (!memcmp(creator, "Backup ", 7))
      srv_start_after_restore= true;
    return DB_SUCCESS;
  case log_t::FORMAT_10_5:
  case log_t::FORMAT_10_5 | log_t::FORMAT_ENCRYPTED:
    if (files.size() != 1)
    {
      sql_print_error("InnoDB: Expecting only ib_logfile0");
      return DB_CORRUPTION;
    }
    /* fall through */
  case log_t::FORMAT_10_2:
  case log_t::FORMAT_10_2 | log_t::FORMAT_ENCRYPTED:
  case log_t::FORMAT_10_3:
  case log_t::FORMAT_10_3 | log_t::FORMAT_ENCRYPTED:
  case log_t::FORMAT_10_4:
  case log_t::FORMAT_10_4 | log_t::FORMAT_ENCRYPTED:
    uint64_t max_no= 0;
    const lsn_t log_size{(log_sys.file_size - 2048) * files.size()};
    for (size_t field= 512; field < 2048; field += 1024)
    {
      const byte *b = buf + field;

      if (!recv_check_log_block(b))
      {
        DBUG_PRINT("ib_log", ("invalid checkpoint checksum at %zu", field));
        continue;
      }

      if (log_sys.is_encrypted() && !log_crypt_read_checkpoint_buf(b))
      {
        sql_print_error("InnoDB: Reading checkpoint encryption info failed.");
        continue;
      }

      const uint64_t checkpoint_no= mach_read_from_8(b);
      const lsn_t checkpoint_lsn= mach_read_from_8(b + 8);
      DBUG_PRINT("ib_log", ("checkpoint " UINT64PF " at " LSN_PF " found",
                            checkpoint_no, checkpoint_lsn));
      const lsn_t o{mach_read_from_8(b + 16)};
      if (checkpoint_no >= max_no && o >= 0x80c && (o & ~511) + 512 < log_size)
      {
        max_no= checkpoint_no;
        log_sys.next_checkpoint_lsn= checkpoint_lsn;
        log_sys.next_checkpoint_no= field == 512;
        lsn_offset= mach_read_from_8(b + 16);
      }
    }
  }

  if (!log_sys.next_checkpoint_lsn)
  {
  got_no_checkpoint:
    sql_print_error("InnoDB: No valid checkpoint was found;"
                    " the log was created with %s.", creator);
    return DB_ERROR;
  }

  if (wrong_size)
    return DB_CORRUPTION;

  if (dberr_t err= recv_log_recover_10_5(lsn_offset))
  {
    const char *msg1, *msg2, *msg3;
    msg1= srv_operation == SRV_OPERATION_NORMAL
      ? "InnoDB: Upgrade after a crash is not supported."
      : "mariadb-backup --prepare is not possible.";

    if (err == DB_ERROR)
    {
      msg2= srv_operation == SRV_OPERATION_NORMAL
        ? ". You must start up and shut down MariaDB "
        : ". You must use mariadb-backup ";
      msg3= (log_sys.format & ~log_t::FORMAT_ENCRYPTED) == log_t::FORMAT_10_5
        ? "10.7 or earlier." : "10.4 or earlier.";
    }
    else
      msg2= ", and it appears corrupted.", msg3= "";

    sql_print_error("%s The redo log was created with %s%s%s",
                    msg1, creator, msg2, msg3);
    return err;
  }

  goto upgrade;
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
      buf_block_t *block= buf_LRU_get_free_block(have_mutex);
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
    buf_flush_sync_batch(lsn);
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
    const size_t used= static_cast<uint16_t>(block->page.access_time - 1) + 1;
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
    block->page.access_time= (block->page.access_time & ~0U << 16) |
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
    block->page.access_time= 1U << 16 |
      ut_calc_align<uint16_t>(static_cast<uint16_t>(size), ALIGNMENT);
    static_assert(ut_is_2pow(ALIGNMENT), "ALIGNMENT must be a power of 2");
    UT_LIST_ADD_FIRST(blocks, block);
    MEM_MAKE_ADDRESSABLE(block->page.frame, size);
    MEM_NOACCESS(block->page.frame + size, srv_page_size - size);
    buf= block->page.frame;
  }
  else
  {
    size_t free_offset= static_cast<uint16_t>(block->page.access_time);
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

    block->page.access_time= ((block->page.access_time >> 16) + 1) << 16 |
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

/** Wrapper for log_sys.buf[] between recv_sys.offset and recv_sys.len */
struct recv_buf
{
  bool is_mmap() const noexcept { return log_sys.is_mmap(); }

  const byte *ptr;

  constexpr recv_buf(const byte *ptr) : ptr(ptr) {}
  constexpr bool operator==(const recv_buf other) const
  { return ptr == other.ptr; }

  static const byte *end() { return &log_sys.buf[recv_sys.len]; }

  const char *get_filename(byte*, size_t) const noexcept
  { return reinterpret_cast<const char*>(ptr); }

  bool is_eof(size_t len= 0) const noexcept { return ptr + len >= end(); }

  byte operator*() const noexcept
  {
    ut_ad(ptr >= log_sys.buf);
    ut_ad(ptr < end());
    return *ptr;
  }
  byte operator[](size_t size) const noexcept { return *(*this + size); }
  recv_buf operator+(size_t len) const noexcept
  { recv_buf r{*this}; return r+= len; }
  recv_buf &operator++() noexcept { return *this+= 1; }
  recv_buf &operator+=(size_t len) noexcept { ptr+= len; return *this; }

  size_t operator-(const recv_buf start) const noexcept
  {
    ut_ad(ptr >= start.ptr);
    return size_t(ptr - start.ptr);
  }

  uint32_t crc32c(const recv_buf start) const noexcept
  {
    return my_crc32c(0, start.ptr, ptr - start.ptr);
  }

  void *memcpy(void *buf, size_t size) const noexcept
  {
    ut_ad(size);
    ut_ad(!is_eof(size - 1));
    return ::memcpy(buf, ptr, size);
  }

  bool is_zero(size_t size) const noexcept
  {
    ut_ad(!is_eof(size));
    return !memcmp(ptr, field_ref_zero, size);
  }

  uint64_t read8() const noexcept
  { ut_ad(!is_eof(7)); return mach_read_from_8(ptr); }
  uint32_t read4() const noexcept
  { ut_ad(!is_eof(3)); return mach_read_from_4(ptr); }

  /** Update the pointer if the new pointer is within the buffer. */
  bool set_if_contains(const byte *pos) noexcept
  {
    if (pos > end() || pos < ptr)
      return false;
    ptr= pos;
    return true;
  }

  /** Get the contiguous, unencrypted buffer.
  @param buf         return value of copy_if_needed()
  @param start       start of the mini-transaction
  @param decrypt_buf possibly, a copy of the mini-transaction
  @return contiguous, non-encrypted buffer */
  const byte *get_buf(const byte *buf, const recv_buf start,
                      const byte *decrypt_buf) const noexcept
  { return ptr == buf ? start.ptr : decrypt_buf; }

  /** Copy and decrypt a log record if needed.
  @param iv    initialization vector
  @param tmp   buffer for the decrypted log record
  @param start un-encrypted start of the log record
  @param len   length of the possibly encrypted part, in bytes */
  const byte *copy_if_needed(const byte *iv, byte *tmp, recv_buf start,
                             size_t len)
  {
    ut_ad(*this - start + len <= srv_page_size);
    if (!len || !log_sys.is_encrypted())
      return ptr;
    const size_t s(*this - start);
    start.memcpy(tmp, s);
    return log_decrypt_buf(iv, tmp + s, ptr, static_cast<uint>(len));
  }
};

#ifdef HAVE_INNODB_MMAP
/** Ring buffer wrapper for log_sys.buf[]; recv_sys.len == log_sys.file_size */
struct recv_ring : public recv_buf
{
  static constexpr bool is_mmap() { return true; }

  constexpr recv_ring(const byte *ptr) : recv_buf(ptr) {}

  constexpr static bool is_eof() { return false; }
  constexpr static bool is_eof(size_t) { return false; }

  byte operator*() const noexcept
  {
    ut_ad(ptr >= &log_sys.buf[log_sys.START_OFFSET]);
    ut_ad(ptr < end());
    return *ptr;
  }
  byte operator[](size_t size) const noexcept { return *(*this + size); }
  recv_ring operator+(size_t len) const noexcept
  { recv_ring r{*this}; return r+= len; }
  recv_ring &operator++() noexcept { return *this+= 1; }
  recv_ring &operator+=(size_t len) noexcept
  {
    ut_ad(ptr < end());
    ut_ad(ptr >= &log_sys.buf[log_sys.START_OFFSET]);
    ut_ad(len < recv_sys.MTR_SIZE_MAX * 2);
    ptr+= len;
    if (ptr >= end())
    {
      ptr-= recv_sys.len - log_sys.START_OFFSET;
      ut_ad(ptr >= &log_sys.buf[log_sys.START_OFFSET]);
      ut_ad(ptr < end());
    }
    return *this;
  }
  size_t operator-(const recv_ring start) const noexcept
  {
    auto s= ptr - start.ptr;
    return s >= 0
      ? size_t(s)
      : size_t(s + recv_sys.len - log_sys.START_OFFSET);
  }

  uint32_t crc32c(const recv_ring start) const noexcept
  {
    return ptr >= start.ptr
      ? my_crc32c(0, start.ptr, ptr - start.ptr)
      : my_crc32c(my_crc32c(0, start.ptr, end() - start.ptr),
                  &log_sys.buf[log_sys.START_OFFSET],
                  ptr - &log_sys.buf[log_sys.START_OFFSET]);
  }

  void *memcpy(void *buf, size_t size) const noexcept
  {
    ut_ad(size);
    ut_ad(size < srv_page_size);

    auto s= ptr + size - end();
    if (s <= 0)
      return ::memcpy(buf, ptr, size);
    ::memcpy(buf, ptr, size - s);
    ::memcpy(static_cast<byte*>(buf) + size - s,
             &log_sys.buf[log_sys.START_OFFSET], s);
    return buf;
  }

  bool is_zero(size_t size) const noexcept
  {
    auto s= ptr + size - end();
    if (s <= 0)
      return !memcmp(ptr, field_ref_zero, size);
    return !memcmp(ptr, field_ref_zero, size - s) &&
      !memcmp(&log_sys.buf[log_sys.START_OFFSET], field_ref_zero, s);
  }

  uint64_t read8() const noexcept
  {
    if (UNIV_LIKELY(ptr + 8 <= end()))
      return mach_read_from_8(ptr);
    byte b[8];
    return mach_read_from_8(static_cast<const byte*>(memcpy(b, 8)));
  }
  uint32_t read4() const noexcept
  {
    if (UNIV_LIKELY(ptr + 4 <= end()))
      return mach_read_from_4(ptr);
    byte b[4];
    return mach_read_from_4(static_cast<const byte*>(memcpy(b, 4)));
  }

  /** Get the contiguous, unencrypted buffer.
  @param buf         return value of copy_if_needed()
  @param start       start of the mini-transaction
  @param decrypt_buf possibly, a copy of the mini-transaction
  @return contiguous, non-encrypted buffer */
  const byte *get_buf(const byte *buf, const recv_ring start,
                      const byte *decrypt_buf) const noexcept
  { return ptr == buf && start.ptr < ptr ? start.ptr : decrypt_buf; }

  const char *get_filename(byte* buf, size_t rlen) const noexcept
  {
    return UNIV_LIKELY(ptr + rlen <= end())
      ? reinterpret_cast<const char*>(ptr)
      : static_cast<const char*>(memcpy(buf, rlen));
  }

  /** Copy and decrypt a log record if needed.
  @param iv    initialization vector
  @param tmp   buffer for the decrypted log record
  @param start un-encrypted start of the log record
  @param len   length of the possibly encrypted part, in bytes */
  const byte *copy_if_needed(const byte *iv, byte *tmp, recv_ring start,
                             size_t len)
  {
    const size_t s(*this - start);
    ut_ad(s + len <= srv_page_size);
    if (!len || !log_sys.is_encrypted())
    {
      if (start.ptr + s == ptr && ptr + len <= end())
        return ptr;
      start.memcpy(tmp, s + len);
      return tmp + s;
    }

    start.memcpy(tmp, s);

    const byte *b= ptr;
    if (ptr + len > end())
      b= static_cast<byte*>(memcpy(alloca(len), len));
    return log_decrypt_buf(iv, tmp + s, b, static_cast<uint>(len));
  }
};
#endif

template<typename source>
void recv_sys_t::rewind(source &l, source &begin) noexcept
{
  ut_ad(srv_operation != SRV_OPERATION_BACKUP);
  mysql_mutex_assert_owner(&mutex);

  const source end= l;
  uint32_t rlen;
  for (l= begin; !(l == end); l+= rlen)
  {
    const source recs{l};
    ++l;
    const byte b= *recs;

    ut_ad(b > 1);
    ut_ad(UNIV_LIKELY((b & 0x70) != RESERVED) || srv_force_recovery);

    rlen= b & 0xf;
    if (!rlen)
    {
      const uint32_t lenlen= mlog_decode_varint_length(*l);
      const uint32_t addlen= mlog_decode_varint(l);
      ut_ad(addlen != MLOG_DECODE_ERROR);
      rlen= addlen + 15 - lenlen;
      l+= lenlen;
    }
    ut_ad(!l.is_eof(rlen));
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
    if (!head || head->start_lsn == lsn)
    {
      erase(pages_it);
      pages_it= pages.end();
    }
    else
      pages_it->second.log.rewind(lsn);
  }

  l= begin;
  pages_it= pages.end();
}

/** Parse and register one log_t::FORMAT_10_8 mini-transaction.
@tparam store     whether to store the records
@param  l         log data source
@param  if_exists if store: whether to check if the tablespace exists */
template<typename source,bool store>
inline
recv_sys_t::parse_mtr_result recv_sys_t::parse(source &l, bool if_exists)
  noexcept
{
restart:
  ut_ad(log_sys.latch_have_wr() ||
        srv_operation == SRV_OPERATION_BACKUP ||
        srv_operation == SRV_OPERATION_BACKUP_NO_DEFER);
  mysql_mutex_assert_owner(&mutex);
  ut_ad(log_sys.next_checkpoint_lsn);
  ut_ad(log_sys.is_latest());
  ut_ad(store || !if_exists);
  ut_ad(store ||
        srv_operation != SRV_OPERATION_BACKUP ||
        srv_operation != SRV_OPERATION_BACKUP_NO_DEFER);

  alignas(8) byte iv[MY_AES_BLOCK_SIZE];
  byte *decrypt_buf= static_cast<byte*>(alloca(srv_page_size));

  const lsn_t start_lsn{lsn};

  /* Check that the entire mini-transaction is included within the buffer */
  if (l.is_eof(0))
    return PREMATURE_EOF;

  if (*l <= 1)
    return GOT_EOF; /* We should never write an empty mini-transaction. */

  source begin{l};
  uint32_t rlen;
  for (uint32_t total_len= 0; !l.is_eof(); l+= rlen, total_len+= rlen)
  {
    if (total_len >= MTR_SIZE_MAX)
      return GOT_EOF;
    if (*l <= 1)
      goto eom_found;
    rlen= *l & 0xf;
    ++l;
    if (!rlen)
    {
      if (l.is_eof(0))
        break;
      rlen= mlog_decode_varint_length(*l);
      if (l.is_eof(rlen))
        break;
      const uint32_t addlen= mlog_decode_varint(l);
      if (UNIV_UNLIKELY(addlen >= MTR_SIZE_MAX))
        return GOT_EOF;
      rlen= addlen + 15;
    }
  }

  /* Not the entire mini-transaction was present. */
  return PREMATURE_EOF;

 eom_found:
  if (*l != log_sys.get_sequence_bit((l - begin) + lsn))
    return GOT_EOF;

  if (l.is_eof(4))
    return PREMATURE_EOF;

  uint32_t crc{l.crc32c(begin)};

  if (log_sys.is_encrypted())
  {
    if (l.is_eof(8 + 4))
      return PREMATURE_EOF;
    (l + 1).memcpy(iv, 8);
    l+= 8;
    crc= my_crc32c(crc, iv, 8);
  }

  DBUG_EXECUTE_IF("log_intermittent_checksum_mismatch",
                  {
                    static int c;
                    if (!c++)
                    {
                      sql_print_information("Invalid log block checksum");
                      return GOT_EOF;
                    }
                  });

  if (crc != (l + 1).read4())
    return GOT_EOF;

  l+= 5;
  ut_d(const source el{l});
  lsn+= l - begin;
  offset= l.ptr - log_sys.buf;
  if (!l.is_mmap());
  else if (offset == log_sys.file_size)
    offset= log_sys.START_OFFSET;
  else
    ut_ad(offset < log_sys.file_size);

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

  for (l= begin;; l+= rlen)
  {
    const source recs{l};
    ++l;
    const byte b= *recs;

    if (b <= 1)
      break;

    if (UNIV_LIKELY((b & 0x70) != RESERVED));
    else if (srv_force_recovery)
      sql_print_warning("InnoDB: Ignoring unknown log record at LSN " LSN_PF,
                        lsn);
    else
    {
      sql_print_error("InnoDB: Unknown log record at LSN " LSN_PF, lsn);
    corrupted:
      found_corrupt_log= true;
      return GOT_EOF;
    }

    rlen= b & 0xf;
    if (!rlen)
    {
      const uint32_t lenlen= mlog_decode_varint_length(*l);
      const uint32_t addlen= mlog_decode_varint(l);
      ut_ad(addlen != MLOG_DECODE_ERROR);
      rlen= addlen + 15 - lenlen;
      l+= lenlen;
    }
    ut_ad(!l.is_eof(rlen));

    uint32_t idlen;
    if ((b & 0x80) && got_page_op)
    {
      /* This record is for the same page as the previous one. */
      if (UNIV_UNLIKELY((b & 0x70) <= INIT_PAGE))
      {
      record_corrupted:
        /* FREE_PAGE,INIT_PAGE cannot be with same_page flag */
        if (!srv_force_recovery)
        {
        malformed:
          sql_print_error("InnoDB: Malformed log record at LSN " LSN_PF
                          "; set innodb_force_recovery=1 to ignore.", lsn);
          goto corrupted;
        }
        sql_print_warning("InnoDB: Ignoring malformed log record at LSN "
                          LSN_PF, lsn);
        last_offset= 1; /* the next record must not be same_page  */
        continue;
      }
      if (srv_operation == SRV_OPERATION_BACKUP)
        continue;
      DBUG_PRINT("ib_log",
                 ("scan " LSN_PF ": rec %x len %zu page %u:%u",
                  lsn, b, l - recs + rlen, space_id, page_no));
      goto same_page;
    }
    last_offset= 0;
    idlen= mlog_decode_varint_length(*l);
    if (UNIV_UNLIKELY(idlen > 5 || idlen >= rlen))
    {
      if (!*l && b == FILE_CHECKPOINT + 1)
        continue;
    page_id_corrupted:
      if (!srv_force_recovery)
      {
        sql_print_error("InnoDB: Corrupted page identifier at " LSN_PF
                        "; set innodb_force_recovery=1 to ignore the record.",
                        lsn);
        goto corrupted;
      }
      sql_print_warning("InnoDB: Ignoring corrupted page identifier at LSN "
                        LSN_PF, lsn);
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
    mach_write_to_4(iv + 8, space_id);
    mach_write_to_4(iv + 12, page_no);
    got_page_op= !(b & 0x80);
    if (!got_page_op);
    else if (!store && srv_operation == SRV_OPERATION_BACKUP)
    {
      if (page_no == 0 && first_page_init && (b & 0x10))
        first_page_init(space_id);
      continue;
    }
    else if (store && file_checkpoint && !is_predefined_tablespace(space_id))
    {
      recv_spaces_t::iterator i= recv_spaces.lower_bound(space_id);
      if (i != recv_spaces.end() && i->first == space_id);
      else if (lsn < file_checkpoint)
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
                      << " at " << lsn
                      << "; set innodb_force_recovery=1 to ignore the record.";
          goto corrupted;
        }
        ib::warn() << "Ignoring record for " << id << " at " << lsn;
        continue;
      }
    }
    DBUG_PRINT("ib_log",
               ("scan " LSN_PF ": rec %x len %zu page %u:%u",
                lsn, b, l - recs + rlen, space_id, page_no));
    if (got_page_op)
    {
    same_page:
      const byte *cl= l.ptr;
      if (!rlen);
      else if (UNIV_UNLIKELY(l - recs + rlen > srv_page_size))
        goto record_corrupted;
      const page_id_t id{space_id, page_no};
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
      copy_if_needed:
        cl= l.copy_if_needed(iv, decrypt_buf, recs, rlen);
        break;
      case EXTENDED:
        if (UNIV_UNLIKELY(!rlen))
          goto record_corrupted;
        cl= l.copy_if_needed(iv, decrypt_buf, recs, rlen);
        if (rlen == 1 && *cl == TRIM_PAGES)
        {
          if (srv_is_undo_tablespace(space_id))
	  {
	    if (page_no != SRV_UNDO_TABLESPACE_SIZE_IN_PAGES)
              goto record_corrupted;
            /* The entire undo tablespace will be reinitialized by
            innodb_undo_log_truncate=ON. Discard old log for all
	    pages. */
	    trim({space_id, 0}, start_lsn);
	    truncated_undo_spaces[space_id - srv_undo_space_id_start]=
              { start_lsn, page_no};
	  }
	  else if (space_id != 0) goto record_corrupted;
	  else
	  {
	    /* Shrink the system tablespace */
            trim({space_id, page_no}, start_lsn);
            truncated_sys_space= {start_lsn, page_no};
	  }
          static_assert(UT_ARR_SIZE(truncated_undo_spaces) ==
                        TRX_SYS_MAX_UNDO_SPACES, "compatibility");
          if (!store && undo_space_trunc && space_id)
            undo_space_trunc(space_id);
          last_offset= 1; /* the next record must not be same_page  */
          continue;
        }
        last_offset= FIL_PAGE_TYPE;
        break;
      case OPTION:
        if (rlen == 5 && *l == OPT_PAGE_CHECKSUM)
          goto copy_if_needed;
        /* fall through */
      case RESERVED:
        continue;
      case WRITE:
      case MEMMOVE:
      case MEMSET:
        if (UNIV_UNLIKELY(rlen == 0 || last_offset == 1))
          goto record_corrupted;
        ut_d(const source payload{l});
        cl= l.copy_if_needed(iv, decrypt_buf, recs, rlen);
        const uint32_t olen= mlog_decode_varint_length(*cl);
        if (UNIV_UNLIKELY(olen >= rlen) || UNIV_UNLIKELY(olen > 3))
          goto record_corrupted;
        const uint32_t offset= mlog_decode_varint(cl);
        ut_ad(offset != MLOG_DECODE_ERROR);
        static_assert(FIL_PAGE_OFFSET == 4, "compatibility");
        if (UNIV_UNLIKELY(offset >= srv_page_size))
          goto record_corrupted;
        last_offset+= offset;
        if (UNIV_UNLIKELY(last_offset < 8 || last_offset >= srv_page_size))
          goto record_corrupted;
        cl+= olen;
        rlen-= olen;
        if ((b & 0x70) == WRITE)
        {
          if (UNIV_UNLIKELY(rlen + last_offset > srv_page_size))
            goto record_corrupted;
          if (UNIV_UNLIKELY(!page_no) && file_checkpoint)
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
                ? mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE + cl -
                                   last_offset)
                : 0;
              const uint32_t flags= has_flags
                ? mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + cl -
                                   last_offset)
                : file_name_t::initial_flags;
              if (it == recv_spaces.end())
                ut_ad(!store || space_id == TRX_SYS_SPACE ||
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
        parsed_ok:
          last_offset+= rlen;
          ut_ad(l == payload);
          if (!l.set_if_contains(cl))
            (l= recs)+= cl - decrypt_buf;
          break;
        }
        uint32_t llen= mlog_decode_varint_length(*cl);
        if (UNIV_UNLIKELY(llen > rlen || llen > 3))
          goto record_corrupted;
        const uint32_t len= mlog_decode_varint(cl);
        ut_ad(len != MLOG_DECODE_ERROR);
        if (UNIV_UNLIKELY(last_offset + len > srv_page_size))
          goto record_corrupted;
        cl+= llen;
        rlen-= llen;
        llen= len;
        if ((b & 0x70) == MEMSET)
        {
          if (UNIV_UNLIKELY(rlen > llen))
            goto record_corrupted;
          goto parsed_ok;
        }
        const uint32_t slen= mlog_decode_varint_length(*cl);
        if (UNIV_UNLIKELY(slen != rlen || slen > 3))
          goto record_corrupted;
        uint32_t s= mlog_decode_varint(cl);
        ut_ad(slen != MLOG_DECODE_ERROR);
        if (s & 1)
          s= last_offset - (s >> 1) - 1;
        else
          s= last_offset + (s >> 1) + 1;
        if (UNIV_UNLIKELY(s < 8 || s + llen > srv_page_size))
          goto record_corrupted;
        goto parsed_ok;
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
      if (store)
      {
        if (if_exists)
        {
          if (fil_space_t *space= fil_space_t::get(space_id))
          {
            const auto size= space->get_size();
            space->release();
            if (!size)
              continue;
          }
          else if (!deferred_spaces.find(space_id))
            continue;
        }
        if (!mlog_init.will_avoid_read(id, start_lsn))
        {
          if (pages_it == pages.end() || pages_it->first != id)
            pages_it= pages.emplace(id, page_recv_t{}).first;
          if (UNIV_UNLIKELY(add(pages_it, start_lsn, lsn,
                                l.get_buf(cl, recs, decrypt_buf),
                                l - recs + rlen)))
          {
            lsn= start_lsn;
            log_sys.set_recovered_lsn(start_lsn);
            l+= rlen;
            offset= begin.ptr - log_sys.buf;
            rewind(l, begin);
            if (if_exists)
            {
              apply(false);
              if (is_corrupt_fs())
                return GOT_EOF;
              goto restart;
            }
            sql_print_information("InnoDB: Multi-batch recovery needed at LSN "
                                  LSN_PF, lsn);
            return GOT_OOM;
          }
        }
      }
      else if ((b & 0x70) <= INIT_PAGE)
      {
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
        if (space_id || page_no || l[rlen] > 1);
        else if (rlen != 8)
        {
          if (rlen < UNIV_PAGE_SIZE_MAX && !l.is_zero(rlen))
            continue;
        }
        else if (store)
        {
          ut_ad(file_checkpoint);
          continue;
        }
        else if (const lsn_t c= l.read8())
        {
          if (UNIV_UNLIKELY(srv_print_verbose_log == 2))
            fprintf(stderr, "FILE_CHECKPOINT(" LSN_PF ") %s at " LSN_PF "\n",
                    c, c != log_sys.next_checkpoint_lsn
                    ? "ignored" : file_checkpoint ? "reread" : "read", lsn);

          DBUG_PRINT("ib_log",
                     ("FILE_CHECKPOINT(" LSN_PF ") %s at " LSN_PF,
                      c, c != log_sys.next_checkpoint_lsn
                      ? "ignored" : file_checkpoint ? "reread" : "read", lsn));

          if (c == log_sys.next_checkpoint_lsn)
          {
            /* There can be multiple FILE_CHECKPOINT for the same LSN. */
            if (file_checkpoint)
              continue;
            file_checkpoint= lsn;
            return GOT_EOF;
          }
          continue;
        }
        else
          continue;
        /* fall through */
      default:
        if (!srv_force_recovery)
          goto malformed;
        sql_print_warning("InnoDB: Ignoring malformed log record at LSN "
                          LSN_PF, lsn);
        continue;
      case FILE_DELETE:
      case FILE_MODIFY:
      case FILE_RENAME:
        if (UNIV_UNLIKELY(page_no != 0))
        {
        file_rec_error:
          if (!srv_force_recovery)
          {
            sql_print_error("InnoDB: Corrupted file-level record;"
                            " set innodb_force_recovery=1 to ignore.");
            goto corrupted;
          }

          sql_print_warning("InnoDB: Ignoring corrupted file-level record"
                            " at LSN " LSN_PF, lsn);
          continue;
        }
        /* fall through */
      case FILE_CREATE:
        if (UNIV_UNLIKELY(!space_id || page_no))
          goto file_rec_error;
        /* There is no terminating NUL character. Names must end in .ibd.
        For FILE_RENAME, there is a NUL between the two file names. */

        const char * const fn= l.get_filename(decrypt_buf, rlen);
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

        if (UNIV_UNLIKELY(!recv_needed_recovery && srv_read_only_mode))
          continue;

        if (!store &&
            (srv_operation == SRV_OPERATION_BACKUP ||
             srv_operation == SRV_OPERATION_BACKUP_NO_DEFER))
        {
          if ((b & 0xf0) < FILE_CHECKPOINT && log_file_op)
            log_file_op(space_id, b & 0xf0,
                        reinterpret_cast<const byte*>(fn),
                        static_cast<ulint>(fnend - fn),
                        reinterpret_cast<const byte*>(fn2),
                        fn2 ? static_cast<ulint>(fn2end - fn2) : 0);
          continue;
        }

        fil_name_process(fn, fnend - fn, space_id,
                         (b & 0xf0) == FILE_DELETE ? FILE_DELETE : FILE_MODIFY,
                         start_lsn, if_exists);

        if (fn2)
        {
          fil_name_process(fn2, fn2end - fn2, space_id,
                           FILE_RENAME, start_lsn, if_exists);
          if (file_checkpoint)
          {
            const size_t len= fn2end - fn2;
            auto r= renamed_spaces.emplace(space_id, std::string{fn2, len});
            if (!r.second)
              r.first->second= std::string{fn2, len};
          }
        }

        if (is_corrupt_fs())
          return GOT_EOF;
      }
    }
    else if (b == FILE_CHECKPOINT + 2 && !space_id && !page_no);
    else
      goto malformed;
  }

  l+= log_sys.is_encrypted() ? 4U + 8U : 4U;
  ut_ad(l == el);
  return OK;
}

template<bool store>
recv_sys_t::parse_mtr_result recv_sys_t::parse_mtr(bool if_exists) noexcept
{
  recv_buf s{&log_sys.buf[recv_sys.offset]};
  return recv_sys.parse<recv_buf,store>(s, if_exists);
}

/** for mariadb-backup; @see xtrabackup_copy_logfile() */
template
recv_sys_t::parse_mtr_result recv_sys_t::parse_mtr<false>(bool) noexcept;

#ifdef HAVE_INNODB_MMAP
template<bool store>
recv_sys_t::parse_mtr_result recv_sys_t::parse_mmap(bool if_exists) noexcept
{
  recv_sys_t::parse_mtr_result r{parse_mtr<store>(if_exists)};
  if (UNIV_LIKELY(r != PREMATURE_EOF) || !log_sys.is_mmap())
    return r;
  ut_ad(recv_sys.len == log_sys.file_size);
  ut_ad(recv_sys.offset >= log_sys.START_OFFSET);
  ut_ad(recv_sys.offset <= recv_sys.len);
  recv_ring s
    {recv_sys.offset == recv_sys.len
     ? &log_sys.buf[log_sys.START_OFFSET]
     : &log_sys.buf[recv_sys.offset]};
  return recv_sys.parse<recv_ring,store>(s, if_exists);
}

/** for mariadb-backup; @see xtrabackup_copy_mmap_logfile() */
template
recv_sys_t::parse_mtr_result recv_sys_t::parse_mmap<false>(bool) noexcept;
#endif

/** Apply the hashed log records to the page, if the page lsn is less than the
lsn of a log record.
@param[in,out]	block		buffer pool page
@param[in,out]	mtr		mini-transaction
@param[in,out]	recs		log records to apply
@param[in,out]	space		tablespace, or NULL if not looked up yet
@param[in,out]	init_lsn	page initialization LSN, or 0
@return the recovered page
@retval nullptr on failure */
static buf_block_t *recv_recover_page(buf_block_t *block, mtr_t &mtr,
                                      page_recv_t &recs,
                                      fil_space_t *space, lsn_t init_lsn = 0)
{
	mysql_mutex_assert_not_owner(&recv_sys.mutex);
	ut_ad(recv_sys.apply_log_recs);
	ut_ad(recv_needed_recovery);
	ut_ad(recs.being_processed == 1);
	ut_ad(!space || space->id == block->page.id().space());
	ut_ad(log_sys.is_latest());

	if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
		ib::info() << "Applying log to page " << block->page.id();
	}

	DBUG_PRINT("ib_log", ("Applying log to page %u:%u",
			      block->page.id().space(),
			      block->page.id().page_no()));

	byte *frame = UNIV_LIKELY_NULL(block->page.zip.data)
		? block->page.zip.data
		: block->page.frame;
	const lsn_t page_lsn = init_lsn
		? 0
		: mach_read_from_8(frame + FIL_PAGE_LSN);
	bool free_page = false;
	lsn_t start_lsn = 0, end_lsn = 0;
	ut_d(lsn_t recv_start_lsn = 0);

	bool skipped_after_init = false;

	for (const log_rec_t* recv : recs.log) {
		const log_phys_t* l = static_cast<const log_phys_t*>(recv);
		ut_ad(l->lsn);
		ut_ad(end_lsn <= l->lsn);
		ut_ad(l->lsn <= recv_sys.lsn);

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
			if (end_lsn != page_lsn) {
				sql_print_information(
					"InnoDB: The last skipped log record"
					" LSN " LSN_PF
					" is not equal to page LSN " LSN_PF,
					end_lsn, page_lsn);
			}
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
			mtr.discard_modifications();
			mtr.commit();

			buf_pool.corrupted_evict(&block->page,
						 block->page.state() &
						 buf_page_t::LRU_MASK);
			block = nullptr;
			goto done;
		}

		if (!start_lsn) {
			start_lsn = l->start_lsn;
		}
	}

	if (start_lsn) {
		ut_ad(end_lsn >= start_lsn);
		ut_ad(!block->page.oldest_modification());
		mach_write_to_8(FIL_PAGE_LSN + frame, end_lsn);
		if (UNIV_LIKELY(!block->page.zip.data)) {
			mach_write_to_8(srv_page_size
					- FIL_PAGE_END_LSN_OLD_CHKSUM
					+ frame, end_lsn);
		} else {
			buf_zip_decompress(block, false);
		}
		/* The following is adapted from
		buf_pool_t::insert_into_flush_list() */
		mysql_mutex_lock(&buf_pool.flush_list_mutex);
		buf_pool.flush_list_bytes+= block->physical_size();
		block->page.set_oldest_modification(start_lsn);
		UT_LIST_ADD_FIRST(buf_pool.flush_list, &block->page);
		buf_pool.page_cleaner_wakeup();
		mysql_mutex_unlock(&buf_pool.flush_list_mutex);
	} else if (free_page && init_lsn) {
		/* There have been no operations that modify the page.
		Any buffered changes will be merged in ibuf_upgrade(). */
		ut_ad(!mtr.has_modifications());
		block->page.set_freed(block->page.state());
	}

	/* Make sure that committing mtr does not change the modification
	lsn values of page */

	mtr.discard_modifications();
	mtr.commit();

done:
	/* FIXME: do this in page read, protected with recv_sys.mutex! */
	if (recv_max_page_lsn < page_lsn) {
		recv_max_page_lsn = page_lsn;
	}

	return block;
}

/** Remove records for a corrupted page.
This function should only be called when innodb_force_recovery is set.
@param page_id  corrupted page identifier */
ATTRIBUTE_COLD void recv_sys_t::free_corrupted_page(page_id_t page_id)
{
  if (!recovery_on)
    return;

  mysql_mutex_lock(&mutex);
  map::iterator p= pages.find(page_id);
  if (p == pages.end())
  {
    mysql_mutex_unlock(&mutex);
    return;
  }

  p->second.being_processed= -1;
  if (!srv_force_recovery)
    set_corrupt_fs();
  mysql_mutex_unlock(&mutex);

  ib::error_or_warn(!srv_force_recovery)
    << "Unable to apply log to corrupted page " << page_id;
}

ATTRIBUTE_COLD void recv_sys_t::set_corrupt_log()
{
  mysql_mutex_lock(&mutex);
  found_corrupt_log= true;
  mysql_mutex_unlock(&mutex);
}

ATTRIBUTE_COLD void recv_sys_t::set_corrupt_fs()
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
      const lsn_t init_lsn{p->second.skip_read ? mlog_init.last(id) : 0};
      mysql_mutex_unlock(&recv_sys.mutex);
      success= recv_recover_page(success, mtr, p->second, space, init_lsn);
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

void IORequest::fake_read_complete(os_offset_t offset) const
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
  const lsn_t init_lsn{offset};
  ut_ad(init_lsn > 1);

  if (recv_recover_page(reinterpret_cast<buf_block_t*>(bpage),
                        mtr, recs, node->space, init_lsn))
  {
    ut_ad(bpage->oldest_modification() || bpage->is_freed());
    bpage->lock.x_unlock(true);
  }
  recs.being_processed= -1;
  ut_ad(mtr.has_committed());

  node->space->release();
}

/** @return whether a page has been freed */
inline bool fil_space_t::is_freed(uint32_t page)
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
  if (recv_sys.scanned_lsn == recv_sys.lsn)
  {
    sql_print_information("InnoDB: To recover: %zu pages", n);
    service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                   "To recover: %zu pages", n);
  }
  else
  {
    sql_print_information("InnoDB: To recover: LSN " LSN_PF
                          "/" LSN_PF "; %zu pages",
                          recv_sys.lsn, recv_sys.scanned_lsn, n);
    service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                   "To recover: LSN " LSN_PF
                                   "/" LSN_PF "; %zu pages",
                                   recv_sys.lsn, recv_sys.scanned_lsn, n);
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
    log_sys.latch.wr_unlock();

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
        const lsn_t init_lsn{recs.skip_read ? mlog_init.last(id) : 0};
        mysql_mutex_unlock(&mutex);
        buf_read_recover(space, id, recs, init_lsn);
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
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
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
                                            buf_block_t *b, lsn_t init_lsn)
{
  mysql_mutex_assert_not_owner(&mutex);
  page_recv_t &recs= p->second;
  ut_ad(recs.skip_read);
  ut_ad(recs.being_processed == 1);
  buf_block_t* block= nullptr;
  const lsn_t end_lsn= recs.log.last()->lsn;
  if (end_lsn < init_lsn)
    DBUG_LOG("ib_log", "skip log for page " << p->first
             << " LSN " << end_lsn << " < " << init_lsn);
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
  block= recv_recover_page(block, mtr, recs, space, init_lsn);
  ut_ad(mtr.has_committed());

  if (space)
    space->release();

  return block ? block : reinterpret_cast<buf_block_t*>(-1);
}

/** Read a page or recover it based on redo log records.
@param page_id  page identifier
@param mtr      mini-transaction
@param err      error code
@return the requested block
@retval nullptr if the page cannot be accessed due to corruption */
ATTRIBUTE_COLD
buf_block_t *
recv_sys_t::recover(const page_id_t page_id, mtr_t *mtr, dberr_t *err)
{
  if (!recovery_on)
  must_read:
    return buf_page_get_gen(page_id, 0, RW_NO_LATCH, nullptr, BUF_GET_RECOVER,
                            mtr, err);

  mysql_mutex_lock(&mutex);
  map::iterator p= pages.find(page_id);

  if (p == pages.end() || p->second.being_processed || !p->second.skip_read)
  {
    mysql_mutex_unlock(&mutex);
    goto must_read;
  }

  p->second.being_processed= 1;
  const lsn_t init_lsn{mlog_init.last(page_id)};
  mysql_mutex_unlock(&mutex);
  buf_block_t *free_block= buf_LRU_get_free_block(have_no_mutex);
  buf_block_t *block;
  {
    mtr_t local_mtr;
    block= recover_low(p, local_mtr, free_block, init_lsn);
  }
  p->second.being_processed= -1;
  if (UNIV_UNLIKELY(!block))
  {
    buf_pool.free_block(free_block);
    goto must_read;
  }
  else if (block == reinterpret_cast<buf_block_t*>(-1))
  {
  corrupted:
    if (err)
      *err= DB_CORRUPTION;
    return nullptr;
  }

  ut_ad(block == free_block);
  auto s= block->page.fix();
  ut_ad(s >= buf_page_t::FREED);
  /* The block may be write-fixed at this point because we are not
  holding a latch, but it must not be read-fixed. */
  ut_ad(s < buf_page_t::READ_FIX || s >= buf_page_t::WRITE_FIX);
  if (s < buf_page_t::UNFIXED)
  {
    mysql_mutex_lock(&buf_pool.mutex);
    block->page.unfix();
    buf_LRU_free_page(&block->page, true);
    mysql_mutex_unlock(&buf_pool.mutex);
    goto corrupted;
  }

  mtr->page_lock(block, RW_NO_LATCH);
  return block;
}

inline fil_space_t *fil_system_t::find(const char *path) const
{
  mysql_mutex_assert_owner(&mutex);
  for (fil_space_t &space : fil_system.space_list)
    if (space.chain.start && !strcmp(space.chain.start->name, path))
      return &space;
  return nullptr;
}

/** Thread-safe function which sorts flush_list by oldest_modification */
static void log_sort_flush_list()
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
              DBUG_ASSERT(l > 2); DBUG_ASSERT(r > 2);
              return r < l;
            });

  UT_LIST_INIT(buf_pool.flush_list, &buf_page_t::list);

  for (size_t i= 0; i < idx; i++)
  {
    UT_LIST_ADD_LAST(buf_pool.flush_list, list[i]);
    DBUG_ASSERT(list[i]->oldest_modification() > 2);
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

  mysql_mutex_assert_owner(&mutex);

  garbage_collect();

  if (truncated_sys_space.lsn)
  {
    trim({0, truncated_sys_space.pages}, truncated_sys_space.lsn);
    fil_node_t *file= UT_LIST_GET_LAST(fil_system.sys_space->chain);
    ut_ad(file->is_open());

    /* Last file new size after truncation */
    uint32_t new_last_file_size=
      truncated_sys_space.pages -
        (srv_sys_space.get_min_size()
         - srv_sys_space.m_files.at(
             srv_sys_space.m_files.size() - 1). param_size());

    os_file_truncate(
      file->name, file->handle,
      os_offset_t{new_last_file_size} << srv_page_size_shift, true);
    mysql_mutex_lock(&fil_system.mutex);
    fil_system.sys_space->size= truncated_sys_space.pages;
    fil_system.sys_space->chain.end->size= new_last_file_size;
    srv_sys_space.set_last_file_size(new_last_file_size);
    truncated_sys_space={0, 0};
    mysql_mutex_unlock(&fil_system.mutex);
  }

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
    ut_ad(!last_batch || lsn == scanned_lsn);
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
          log_sys.latch.wr_unlock();
        wait_for_pool(1);
        pages_it= pages.begin();
        mysql_mutex_unlock(&mutex);
        /* We must release log_sys.latch and recv_sys.mutex before
        invoking buf_LRU_get_free_block(). Allocating a block may initiate
        a redo log write and therefore acquire log_sys.latch. To avoid
        deadlocks, log_sys.latch must not be acquired while holding
        recv_sys.mutex. */
        free_block= buf_LRU_get_free_block(have_no_mutex);
        if (!last_batch)
          log_sys.latch.wr_lock(SRW_LOCK_CALL);
        mysql_mutex_lock(&mutex);
        pages_it= pages.begin();
      }

      while (pages_it != pages.end())
      {
        if (is_corrupt_fs() || is_corrupt_log())
        {
          if (space)
            space->release();
          if (free_block)
          {
            mysql_mutex_unlock(&mutex);
            mysql_mutex_lock(&buf_pool.mutex);
            buf_LRU_block_free_non_file_page(free_block);
            mysql_mutex_unlock(&buf_pool.mutex);
            mysql_mutex_lock(&mutex);
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

  if (!last_batch)
    log_sys.latch.wr_unlock();

  mysql_mutex_unlock(&mutex);

  if (!last_batch)
  {
    buf_flush_sync_batch(lsn);
    buf_pool_invalidate();
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
  }
  else if (srv_operation == SRV_OPERATION_RESTORE ||
           srv_operation == SRV_OPERATION_RESTORE_EXPORT)
    buf_flush_sync_batch(lsn);
  else
    /* Instead of flushing, last_batch sorts the buf_pool.flush_list
    in ascending order of buf_page_t::oldest_modification. */
    log_sort_flush_list();

#ifdef HAVE_PMEM
  if (last_batch && log_sys.is_mmap() && !log_sys.is_opened())
    mprotect(log_sys.buf, len, PROT_READ | PROT_WRITE);
#endif

  mysql_mutex_lock(&mutex);

  ut_d(after_apply= true);
  clear();
}

/** Scan log_t::FORMAT_10_8 log store records to the parsing buffer.
@param last_phase     whether changes can be applied to the tablespaces
@return whether rescan is needed (not everything was stored) */
static bool recv_scan_log(bool last_phase)
{
  DBUG_ENTER("recv_scan_log");

  ut_ad(log_sys.is_latest());
  const size_t block_size_1{log_sys.write_size - 1};

  mysql_mutex_lock(&recv_sys.mutex);
  if (!last_phase)
    recv_sys.clear();
  else
    ut_ad(recv_sys.file_checkpoint);

  bool store{recv_sys.file_checkpoint != 0};
  size_t buf_size= log_sys.buf_size;
  if (log_sys.is_mmap())
  {
    recv_sys.offset= size_t(log_sys.calc_lsn_offset(recv_sys.lsn));
    buf_size= size_t(log_sys.file_size);
    recv_sys.len= size_t(log_sys.file_size);
  }
  else
  {
    recv_sys.offset= size_t(recv_sys.lsn - log_sys.get_first_lsn()) &
      block_size_1;
    recv_sys.len= 0;
  }

  lsn_t rewound_lsn= 0;
  for (ut_d(lsn_t source_offset= 0);;)
  {
    ut_ad(log_sys.latch_have_wr());
#ifdef UNIV_DEBUG
    const bool wrap{source_offset + recv_sys.len == log_sys.file_size};
#endif
    if (size_t size= buf_size - recv_sys.len)
    {
#ifndef UNIV_DEBUG
      lsn_t
#endif
      source_offset=
        log_sys.calc_lsn_offset(recv_sys.lsn + recv_sys.len - recv_sys.offset);
      ut_ad(!wrap || source_offset == log_t::START_OFFSET);
      source_offset&= ~block_size_1;

      if (source_offset + size > log_sys.file_size)
        size= static_cast<size_t>(log_sys.file_size - source_offset);

      if (dberr_t err= log_sys.log.read(source_offset,
                                        {log_sys.buf + recv_sys.len, size}))
      {
        mysql_mutex_unlock(&recv_sys.mutex);
        ib::error() << "Failed to read log at " << source_offset
                    << ": " << err;
        recv_sys.set_corrupt_log();
        mysql_mutex_lock(&recv_sys.mutex);
      }
      else
        recv_sys.len+= size;
    }

    if (recv_sys.report(time(nullptr)))
    {
      sql_print_information("InnoDB: Read redo log up to LSN=" LSN_PF,
                            recv_sys.lsn);
      service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                     "Read redo log up to LSN=" LSN_PF,
                                     recv_sys.lsn);
    }

    recv_sys_t::parse_mtr_result r;

    if (UNIV_UNLIKELY(!recv_needed_recovery))
    {
      ut_ad(!last_phase);
      ut_ad(recv_sys.lsn >= log_sys.next_checkpoint_lsn);

      if (!store)
      {
        ut_ad(!recv_sys.file_checkpoint);
        for (;;)
        {
          const byte& b{log_sys.buf[recv_sys.offset]};
          r= recv_sys.parse_mmap<false>(false);
          switch (r) {
          case recv_sys_t::PREMATURE_EOF:
            goto read_more;
          default:
            ut_ad(r == recv_sys_t::GOT_EOF);
            break;
          case recv_sys_t::OK:
            if (b == FILE_CHECKPOINT + 2 + 8 || (b & 0xf0) == FILE_MODIFY)
              continue;
          }

          const lsn_t end{recv_sys.file_checkpoint};
          ut_ad(!end || end == recv_sys.lsn);
          bool corrupt_fs= recv_sys.is_corrupt_fs();
          mysql_mutex_unlock(&recv_sys.mutex);

          if (!end && !corrupt_fs)
          {
            recv_sys.set_corrupt_log();
            sql_print_error("InnoDB: Missing FILE_CHECKPOINT(" LSN_PF
                            ") at " LSN_PF, log_sys.next_checkpoint_lsn,
                            recv_sys.lsn);
          }
          DBUG_RETURN(true);
        }
      }
      else
      {
        ut_ad(recv_sys.file_checkpoint != 0);
        switch ((r= recv_sys.parse_mmap<true>(false))) {
        case recv_sys_t::PREMATURE_EOF:
          goto read_more;
        case recv_sys_t::GOT_EOF:
          break;
        default:
          ut_ad(r == recv_sys_t::OK);
          recv_needed_recovery= true;
          if (srv_read_only_mode)
          {
            mysql_mutex_unlock(&recv_sys.mutex);
            DBUG_RETURN(false);
          }
          sql_print_information("InnoDB: Starting crash recovery from"
                                " checkpoint LSN="  LSN_PF,
                                log_sys.next_checkpoint_lsn);
        }
      }
    }

    if (!store)
    skip_the_rest:
      while ((r= recv_sys.parse_mmap<false>(false)) == recv_sys_t::OK);
    else
    {
      uint16_t count= 0;
      while ((r= recv_sys.parse_mmap<true>(last_phase)) == recv_sys_t::OK)
        if (!++count && recv_sys.report(time(nullptr)))
        {
          const size_t n= recv_sys.pages.size();
          sql_print_information("InnoDB: Parsed redo log up to LSN=" LSN_PF
                                "; to recover: %zu pages", recv_sys.lsn, n);
          service_manager_extend_timeout(INNODB_EXTEND_TIMEOUT_INTERVAL,
                                         "Parsed redo log up to LSN=" LSN_PF
                                         "; to recover: %zu pages",
                                         recv_sys.lsn, n);
        }
      if (r == recv_sys_t::GOT_OOM)
      {
        ut_ad(!last_phase);
        rewound_lsn= recv_sys.lsn;
        store= false;
        if (recv_sys.scanned_lsn <= 1)
          goto skip_the_rest;
        ut_ad(recv_sys.file_checkpoint);
        goto func_exit;
      }
    }

    if (r != recv_sys_t::PREMATURE_EOF)
    {
      ut_ad(r == recv_sys_t::GOT_EOF);
    got_eof:
      ut_ad(recv_sys.is_initialised());
      if (recv_sys.scanned_lsn > 1)
      {
        ut_ad(recv_sys.scanned_lsn == recv_sys.lsn);
        break;
      }
      recv_sys.scanned_lsn= recv_sys.lsn;
      sql_print_information("InnoDB: End of log at LSN=" LSN_PF, recv_sys.lsn);
      break;
    }

  read_more:
    if (log_sys.is_mmap())
      break;

    if (recv_sys.is_corrupt_log())
      break;

    if (recv_sys.offset < log_sys.write_size &&
        recv_sys.lsn == recv_sys.scanned_lsn)
      goto got_eof;

    if (recv_sys.offset > buf_size / 4 ||
        (recv_sys.offset > block_size_1 &&
         recv_sys.len >= buf_size - recv_sys.MTR_SIZE_MAX))
    {
      const size_t ofs{recv_sys.offset & ~block_size_1};
      memmove_aligned<64>(log_sys.buf, log_sys.buf + ofs, recv_sys.len - ofs);
      recv_sys.len-= ofs;
      recv_sys.offset&= block_size_1;
    }
  }

  if (last_phase)
  {
    ut_ad(!rewound_lsn);
    ut_ad(recv_sys.lsn >= recv_sys.file_checkpoint);
    log_sys.set_recovered_lsn(recv_sys.lsn);
  }
  else if (rewound_lsn)
  {
    ut_ad(!store);
    ut_ad(recv_sys.file_checkpoint);
    recv_sys.lsn= rewound_lsn;
  }
func_exit:
  ut_d(recv_sys.after_apply= last_phase);
  mysql_mutex_unlock(&recv_sys.mutex);
  DBUG_RETURN(!store);
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
			sql_print_warning("InnoDB: Tablespace " UINT32PF
					  " was not found at %.*s when"
					  " restoring a (partial?) backup."
					  " All redo log"
					  " for this file will be ignored!",
					  i->first, int(i->second.name.size()),
					  i->second.name.data());
		}
		return(err);
	}

	if (srv_force_recovery == 0) {
		sql_print_error("InnoDB: Tablespace " UINT32PF " was not"
				" found at %.*s.", i->first,
				int(i->second.name.size()),
				i->second.name.data());

		if (err == DB_SUCCESS) {
			sql_print_information(
				"InnoDB: Set innodb_force_recovery=1 to"
				" ignore this and to permanently lose"
				" all changes to the tablespace.");
			err = DB_TABLESPACE_NOT_FOUND;
		}
	} else {
		sql_print_warning("InnoDB: Tablespace " UINT32PF
				  " was not found at %.*s"
				  ", and innodb_force_recovery was set."
				  " All redo log for this tablespace"
				  " will be ignored!",
				  i->first, int(i->second.name.size()),
				  i->second.name.data());
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

		if (deferred_spaces.find(static_cast<uint32_t>(space))) {
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

		if (srv_force_recovery) {
			sql_print_warning("InnoDB: Tablespace " UINT32PF
					  " was not found at %.*s,"
					  " and innodb_force_recovery was set."
					  " All redo log for this tablespace"
					  " will be ignored!",
					  rs.first, int(rs.second.name.size()),
					  rs.second.name.data());
			continue;
		}

		if (!rescan) {
			sql_print_information("InnoDB: Tablespace " UINT32PF
					      " was not found at '%.*s',"
					      " but there were"
					      " no modifications either.",
					      rs.first,
					      int(rs.second.name.size()),
					      rs.second.name.data());
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
			sql_print_error("InnoDB: Missing FILE_CREATE,"
					" FILE_DELETE or FILE_MODIFY"
					" before FILE_CHECKPOINT"
					" for tablespace " UINT32PF, rs.first);
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
  ut_ad(log_sys.latch_have_wr());

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
        sql_print_information("InnoDB: Renaming tablespace metadata " UINT32PF
                              " from '%s' to '%s' that is also associated"
                              " with tablespace " UINT32PF,
                              id, old, new_name, other->id);
        space->chain.start->name= mem_strdup(new_name);
        ut_free(old);
      }
      else if (!os_file_status(new_name, &exists, &ftype) || exists)
      {
        sql_print_error("InnoDB: Cannot replay rename of tablespace " UINT32PF
                        " from '%s' to '%s'%s",
                        id, old, new_name, exists ?
                        " because the target file exists" : "");
        err= DB_TABLESPACE_EXISTS;
      }
      else
      {
        mysql_mutex_unlock(&fil_system.mutex);
        err= space->rename(new_name, false);
        if (err != DB_SUCCESS)
          sql_print_error("InnoDB: Cannot replay rename of tablespace "
                          UINT32PF " to '%s: %s", new_name, ut_strerr(err));
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

dberr_t recv_recovery_read_checkpoint()
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
    sql_print_information("InnoDB: innodb_force_recovery=6"
                          " skips redo log apply");
    return DB_SUCCESS;
  }

  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  dberr_t err= recv_sys.find_checkpoint();
  log_sys.latch.wr_unlock();
  return err;
}

inline void log_t::set_recovered() noexcept
{
  ut_ad(get_flushed_lsn() == get_lsn());
  ut_ad(recv_sys.lsn == get_lsn());
  size_t offset{recv_sys.offset};
  if (!is_mmap())
  {
    const size_t bs{log_sys.write_size}, bs_1{bs - 1};
    memmove_aligned<512>(buf, buf + (offset & ~bs_1), bs);
    offset&= bs_1;
  }
#ifndef _WIN32
  else
    mprotect(buf, size_t(file_size), PROT_READ | PROT_WRITE);
#endif
  set_buf_free(offset);
}

/** Start recovering from a redo log checkpoint.
of first system tablespace page
@return error code or DB_SUCCESS */
dberr_t recv_recovery_from_checkpoint_start()
{
	bool rescan = false;
	dberr_t err = DB_SUCCESS;

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

	recv_sys.recovery_on = true;

	log_sys.latch.wr_lock(SRW_LOCK_CALL);
	log_sys.set_capacity();

	/* Start reading the log from the checkpoint lsn. The variable
	contiguous_lsn contains an lsn up to which the log is known to
	be contiguously written. */

	ut_ad(recv_sys.pages.empty());

	if (log_sys.format == log_t::FORMAT_3_23) {
func_exit:
		log_sys.latch.wr_unlock();
		return err;
	}

	if (log_sys.is_latest()) {
		const bool rewind = recv_sys.lsn
			!= log_sys.next_checkpoint_lsn;
		log_sys.last_checkpoint_lsn = log_sys.next_checkpoint_lsn;

		recv_scan_log(false);
		if (recv_needed_recovery) {
read_only_recovery:
			sql_print_warning("InnoDB: innodb_read_only"
					  " prevents crash recovery");
			err = DB_READ_ONLY;
			goto func_exit;
		}
		if (recv_sys.is_corrupt_log()) {
			sql_print_error("InnoDB: Log scan aborted at LSN "
					LSN_PF, recv_sys.lsn);
                        goto err_exit;
		}
		if (recv_sys.is_corrupt_fs()) {
			goto err_exit;
		}
		ut_ad(recv_sys.file_checkpoint);
		if (rewind) {
			recv_sys.lsn = log_sys.next_checkpoint_lsn;
			recv_sys.offset = 0;
			recv_sys.len = 0;
		}
		ut_ad(!recv_max_page_lsn);
		rescan = recv_scan_log(false);

		if (srv_read_only_mode && recv_needed_recovery) {
			goto read_only_recovery;
		}

		if ((recv_sys.is_corrupt_log() && !srv_force_recovery)
		    || recv_sys.is_corrupt_fs()) {
			goto err_exit;
		}
	}

	log_sys.set_recovered_lsn(recv_sys.lsn);

	if (recv_needed_recovery) {
		bool missing_tablespace = false;

		err = recv_init_crash_recovery_spaces(
			rescan, missing_tablespace);

		if (err != DB_SUCCESS) {
			goto func_exit;
		}

		if (missing_tablespace) {
			ut_ad(rescan);
			/* If any tablespaces seem to be missing,
			validate the remaining log records. */

			do {
				rescan = recv_scan_log(false);

				if (recv_sys.is_corrupt_log() ||
				    recv_sys.is_corrupt_fs()) {
					goto err_exit;
				}

				missing_tablespace = false;

				err = recv_validate_tablespace(
					rescan, missing_tablespace);

				if (err != DB_SUCCESS) {
					goto func_exit;
				}
			} while (missing_tablespace);

			rescan = true;
			/* Because in the loop above we overwrote the
			initially stored recv_sys.pages, we must
			restart parsing the log from the very beginning. */

			/* FIXME: Use a separate loop for checking for
			tablespaces (not individual pages), while retaining
			the initial recv_sys.pages. */
			mysql_mutex_lock(&recv_sys.mutex);
			recv_sys.clear();
			recv_sys.lsn = log_sys.next_checkpoint_lsn;
			mysql_mutex_unlock(&recv_sys.mutex);
		}

		if (srv_operation <= SRV_OPERATION_EXPORT_RESTORED) {
			deferred_spaces.deferred_dblwr();
			buf_dblwr.recover();
		}

		ut_ad(srv_force_recovery <= SRV_FORCE_NO_UNDO_LOG_SCAN);

		if (rescan) {
			recv_scan_log(true);
			if ((recv_sys.is_corrupt_log()
			     && !srv_force_recovery)
			    || recv_sys.is_corrupt_fs()) {
				goto err_exit;
			}

			/* In case of multi-batch recovery,
			redo log for the last batch is not
			applied yet. */
			ut_d(recv_sys.after_apply = false);
		}
	} else {
		ut_ad(recv_sys.pages.empty());
	}

	if (log_sys.is_latest()
	    && (recv_sys.lsn < log_sys.next_checkpoint_lsn
		|| recv_sys.lsn < recv_max_page_lsn)) {

		sql_print_error("InnoDB: We scanned the log up to " LSN_PF "."
				" A checkpoint was at " LSN_PF
				" and the maximum LSN on a database page was "
				LSN_PF ". It is possible that the"
				" database is now corrupt!",
				recv_sys.lsn,
				log_sys.next_checkpoint_lsn,
				recv_max_page_lsn);
	}

	if (recv_sys.lsn < log_sys.next_checkpoint_lsn) {
err_exit:
		err = DB_ERROR;
		goto func_exit;
	}

	if (!srv_read_only_mode && log_sys.is_latest()) {
		log_sys.set_recovered();
		if (recv_needed_recovery
		    && srv_operation <= SRV_OPERATION_EXPORT_RESTORED
		    && recv_sys.lsn - log_sys.next_checkpoint_lsn
		    < log_sys.log_capacity) {
			/* Write a FILE_CHECKPOINT marker as the first thing,
			before generating any other redo log. This ensures
			that subsequent crash recovery will be possible even
			if the server were killed soon after this. */
			fil_names_clear(log_sys.next_checkpoint_lsn);
		}
	}

	DBUG_EXECUTE_IF("before_final_redo_apply", goto err_exit;);
	mysql_mutex_lock(&recv_sys.mutex);
	if (UNIV_UNLIKELY(recv_sys.scanned_lsn != recv_sys.lsn)
	    && log_sys.is_latest()) {
		ut_ad("log parsing error" == 0);
		mysql_mutex_unlock(&recv_sys.mutex);
		err = DB_CORRUPTION;
		goto func_exit;
	}
	recv_sys.apply_log_recs = true;
	ut_d(recv_no_log_write = srv_operation == SRV_OPERATION_RESTORE
	     || srv_operation == SRV_OPERATION_RESTORE_EXPORT);
	if (srv_operation == SRV_OPERATION_NORMAL) {
		err = recv_rename_files();
	}

	recv_lsn_checks_on = true;
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

bool recv_dblwr_t::validate_page(const page_id_t page_id,
                                 const byte *page,
                                 const fil_space_t *space,
                                 byte *tmp_buf)
{
  if (page_id.page_no() == 0)
  {
    uint32_t flags= fsp_header_get_flags(page);
    if (!fil_space_t::is_valid_flags(flags, page_id.space()))
    {
      uint32_t cflags= fsp_flags_convert_from_101(flags);
      if (cflags == UINT32_MAX)
      {
        ib::warn() << "Ignoring a doublewrite copy of page " << page_id
                   << "due to invalid flags " << ib::hex(flags);
        return false;
      }

      flags= cflags;
    }

    /* Page 0 is never page_compressed or encrypted. */
    return !buf_page_is_corrupted(true, page, flags);
  }

  ut_ad(tmp_buf);
  byte *tmp_frame= tmp_buf;
  byte *tmp_page= tmp_buf + srv_page_size;
  const uint16_t page_type= mach_read_from_2(page + FIL_PAGE_TYPE);
  const bool expect_encrypted= space->crypt_data &&
    space->crypt_data->type != CRYPT_SCHEME_UNENCRYPTED;

  if (space->full_crc32())
    return !buf_page_is_corrupted(true, page, space->flags);

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
    return !buf_page_is_corrupted(true, tmp_page, space->flags);
  }

  return !buf_page_is_corrupted(true, page, space->flags);
}

byte *recv_dblwr_t::find_page(const page_id_t page_id,
                              const fil_space_t *space, byte *tmp_buf)
{
  byte *result= NULL;
  lsn_t max_lsn= 0;

  for (byte *page : pages)
  {
    if (page_get_page_no(page) != page_id.page_no() ||
        page_get_space_id(page) != page_id.space())
      continue;
    if (page_id.page_no() == 0)
    {
      uint32_t flags= mach_read_from_4(
        FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page);
      if (!fil_space_t::is_valid_flags(flags, page_id.space()))
        continue;
    }

    const lsn_t lsn= mach_read_from_8(page + FIL_PAGE_LSN);
    if (lsn <= max_lsn ||
        !validate_page(page_id, page, space, tmp_buf))
    {
      /* Mark processed for subsequent iterations in buf_dblwr_t::recover() */
      memset(page + FIL_PAGE_LSN, 0, 8);
      continue;
    }

    ut_a(page_get_page_no(page) == page_id.page_no());
    max_lsn= lsn;
    result= page;
  }

  return result;
}

bool recv_dblwr_t::restore_first_page(uint32_t space_id, const char *name,
                                      pfs_os_file_t file)
{
  const page_id_t page_id(space_id, 0);
  const byte* page= find_page(page_id);
  if (!page)
  {
    /* If the first page of the given user tablespace is not there
    in the doublewrite buffer, then the recovery is going to fail
    now. Report error only when doublewrite buffer is not empty */
    if (pages.size())
      ib::error() << "Corrupted page " << page_id << " of datafile '"
                  << name << "' could not be found in the doublewrite buffer.";
    return true;
  }

  ulint physical_size= fil_space_t::physical_size(
    mach_read_from_4(page + FSP_HEADER_OFFSET + FSP_SPACE_FLAGS));
  ib::info() << "Restoring page " << page_id << " of datafile '"
          << name << "' from the doublewrite buffer. Writing "
          << physical_size << " bytes into file '" << name << "'";

  return os_file_write(
           IORequestWrite, name, file, page, 0, physical_size) !=
         DB_SUCCESS;
}

uint32_t recv_dblwr_t::find_first_page(const char *name, pfs_os_file_t file)
{
  os_offset_t file_size= os_file_get_size(file);
  if (file_size != (os_offset_t) -1)
  {
    for (const page_t *page : pages)
    {
      uint32_t space_id= page_get_space_id(page);
      byte *read_page= nullptr;
      if (page_get_page_no(page) > 0 || space_id == 0)
      {
next_page:
        aligned_free(read_page);
        continue;
      }
      uint32_t flags= mach_read_from_4(
        FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page);
      page_id_t page_id(space_id, 0);
      size_t page_size= fil_space_t::physical_size(flags);
      if (file_size < 4 * page_size)
        goto next_page;
      read_page=
        static_cast<byte*>(aligned_malloc(3 * page_size, page_size));
      /* Read 3 pages from the file and match the space id
      with the space id which is stored in
      doublewrite buffer page. */
      if (os_file_read(IORequestRead, file, read_page, page_size,
                       3 * page_size, nullptr) != DB_SUCCESS)
        goto next_page;
      for (ulint j= 0; j <= 2; j++)
      {
        byte *cur_page= read_page + j * page_size;
        if (buf_is_zeroes(span<const byte>(cur_page, page_size)))
        {
          space_id= 0;
          goto early_exit;
        }
        if (mach_read_from_4(cur_page + FIL_PAGE_OFFSET) != j + 1 ||
            memcmp(cur_page + FIL_PAGE_SPACE_ID,
                   page + FIL_PAGE_SPACE_ID, 4) ||
            buf_page_is_corrupted(false, cur_page, flags))
          goto next_page;
      }
      if (!restore_first_page(space_id, name, file))
      {
early_exit:
        aligned_free(read_page);
        return space_id;
      }
      break;
    }
  }
  return 0;
}
