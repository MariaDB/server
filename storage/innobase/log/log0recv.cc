/*****************************************************************************

Copyright (c) 1997, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.
Copyright (c) 2013, 2020, MariaDB Corporation.

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

/** Read-ahead area in applying log records to file pages */
#define RECV_READ_AHEAD_AREA	32U

/** The recovery system */
recv_sys_t	recv_sys;
/** TRUE when recv_init_crash_recovery() has been called. */
bool	recv_needed_recovery;
#ifdef UNIV_DEBUG
/** TRUE if writing to the redo log (mtr_commit) is forbidden.
Protected by log_sys.mutex. */
bool	recv_no_log_write = false;
#endif /* UNIV_DEBUG */

/** TRUE if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially FALSE, and set by
recv_recovery_from_checkpoint_start(). */
bool	recv_lsn_checks_on;

/** If the following is TRUE, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this becomes TRUE if
the log record hash table becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

true means that recovery is running and no operations on the log file
are allowed yet: the variable name is misleading. */
bool	recv_no_ibuf_operations;

/** The maximum lsn we see for a page during the recovery process. If this
is bigger than the lsn we are able to scan up to, that is an indication that
the recovery failed and the database may be corrupt. */
static lsn_t	recv_max_page_lsn;

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t	recv_writer_thread_key;
#endif /* UNIV_PFS_THREAD */

/** Is recv_writer_thread active? */
bool	recv_writer_thread_active;

/** Stored physical log record with logical LSN (@see log_t::FORMAT_10_5) */
struct log_phys_t : public log_rec_t
{
  /** start LSN of the mini-transaction (not necessarily of this record) */
  const lsn_t start_lsn;
private:
  /** length  of the record, in bytes */
  uint16_t len;

  /** @return start of the log records */
  byte *begin() { return reinterpret_cast<byte*>(&len + 1); }
  /** @return end of the log records */
  byte *end() { byte *e= begin() + len; ut_ad(!*e); return e; }
public:
  /** @return start of the log records */
  const byte *begin() const { return const_cast<log_phys_t*>(this)->begin(); }
  /** @return end of the log records */
  const byte *end() const { return const_cast<log_phys_t*>(this)->end(); }

  /** Determine the allocated size of the object.
  @param len  length of recs, excluding terminating NUL byte
  @return the total allocation size */
  static size_t alloc_size(size_t len)
  {
    return len + 1 +
      reinterpret_cast<size_t>(reinterpret_cast<log_phys_t*>(0)->begin());
  }

  /** Constructor.
  @param start_lsn start LSN of the mini-transaction
  @param lsn  mtr_t::commit_lsn() of the mini-transaction
  @param recs the first log record for the page in the mini-transaction
  @param size length of recs, in bytes, excluding terminating NUL byte */
  log_phys_t(lsn_t start_lsn, lsn_t lsn, const byte *recs, size_t size) :
    log_rec_t(lsn), start_lsn(start_lsn), len(static_cast<uint16_t>(size))
  {
    ut_ad(start_lsn);
    ut_ad(start_lsn < lsn);
    ut_ad(len == size);
    reinterpret_cast<byte*>(memcpy(begin(), recs, size))[size]= 0;
  }

  /** Append a record to the log.
  @param recs  log to append
  @param size  size of the log, in bytes */
  void append(const byte *recs, size_t size)
  {
    ut_ad(start_lsn < lsn);
    reinterpret_cast<byte*>(memcpy(end(), recs, size))[size]= 0;
    len= static_cast<uint16_t>(len + size);
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
      (TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE + block.frame);
    const uint16_t free= mach_read_from_2(free_p);
    if (UNIV_UNLIKELY(free < TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE ||
                      free + len + 6 >= srv_page_size - FIL_PAGE_DATA_END))
    {
      ib::error() << "Not applying UNDO_APPEND due to corruption on "
                  << block.page.id();
      return true;
    }

    byte *p= block.frame + free;
    mach_write_to_2(free_p, free + 4 + len);
    memcpy(p, free_p, 2);
    p+= 2;
    memcpy(p, data, len);
    p+= len;
    mach_write_to_2(p, free);
    return false;
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
    APPLIED_TO_FSP_HEADER
  };

  /** Apply log to a page frame.
  @param[in,out] block         buffer block
  @param[in,out] last_offset   last byte offset, for same_page records
  @return whether any log was applied to the page */
  apply_status apply(const buf_block_t &block, uint16_t &last_offset) const
  {
    const byte * const recs= begin();
    byte *const frame= block.page.zip.ssize
      ? block.page.zip.data : block.frame;
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
            recv_sys.found_corrupt_log= true;
            return applied;
          }
      next_not_same_page:
          last_offset= 1; /* the next record must not be same_page  */
        }
      next:
        l+= rlen;
        continue;
      }

      ut_ad(mach_read_from_4(frame + FIL_PAGE_OFFSET) ==
            block.page.id().page_no());
      ut_ad(mach_read_from_4(frame + FIL_PAGE_SPACE_ID) ==
            block.page.id().space());
      ut_ad(last_offset <= 1 || last_offset > 8);
      ut_ad(last_offset <= size);

      switch (b & 0x70) {
      case OPTION:
        goto next;
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
          {
page_corrupted:
            ib::error() << "Set innodb_force_recovery=1 to ignore corruption.";
            recv_sys.found_corrupt_log= true;
            return applied;
          }
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
            if (UNIV_UNLIKELY(ll > 3 || ll >= rlen))
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
            if (UNIV_UNLIKELY(ll > 2 || ll >= rlen))
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
          ut_ad(rlen < llen);
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


/** Tablespace item during recovery */
struct file_name_t {
	/** Tablespace file name (FILE_MODIFY) */
	std::string	name;
	/** Tablespace object (NULL if not valid or not found) */
	fil_space_t*	space;

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
	ulint		size;

	/** Freed pages of tablespace */
	range_set	freed_ranges;

	/** Constructor */
	file_name_t(std::string name_, bool deleted)
		: name(std::move(name_)), space(NULL),
		status(deleted ? DELETED: NORMAL),
		size(0) {}

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
	ulint,
	file_name_t,
	std::less<ulint>,
	ut_allocator<std::pair<const ulint, file_name_t> > >	recv_spaces_t;

static recv_spaces_t	recv_spaces;

/** Report an operation to create, delete, or rename a file during backup.
@param[in]	space_id	tablespace identifier
@param[in]	create		whether the file is being created
@param[in]	name		file name (not NUL-terminated)
@param[in]	len		length of name, in bytes
@param[in]	new_name	new file name (NULL if not rename)
@param[in]	new_len		length of new_name, in bytes (0 if NULL) */
void (*log_file_op)(ulint space_id, bool create,
		    const byte* name, ulint len,
		    const byte* new_name, ulint new_len);

/** Information about initializing page contents during redo log processing.
FIXME: Rely on recv_sys.pages! */
class mlog_init_t
{
public:
	/** A page initialization operation that was parsed from
	the redo log */
	struct init {
		/** log sequence number of the page initialization */
		lsn_t lsn;
		/** Whether btr_page_create() avoided a read of the page.

		At the end of the last recovery batch, mark_ibuf_exist()
		will mark pages for which this flag is set. */
		bool created;
	};

private:
	typedef std::map<const page_id_t, init,
			 std::less<const page_id_t>,
			 ut_allocator<std::pair<const page_id_t, init> > >
		map;
	/** Map of page initialization operations.
	FIXME: Merge this to recv_sys.pages! */
	map inits;
public:
	/** Record that a page will be initialized by the redo log.
	@param[in]	page_id		page identifier
	@param[in]	lsn		log sequence number
	@return whether the state was changed */
	bool add(const page_id_t page_id, lsn_t lsn)
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		const init init = { lsn, false };
		std::pair<map::iterator, bool> p = inits.insert(
			map::value_type(page_id, init));
		ut_ad(!p.first->second.created);
		if (p.second) return true;
		if (p.first->second.lsn >= init.lsn) return false;
		p.first->second = init;
		return true;
	}

	/** Get the last stored lsn of the page id and its respective
	init/load operation.
	@param[in]	page_id	page id
	@param[in,out]	init	initialize log or load log
	@return the latest page initialization;
	not valid after releasing recv_sys.mutex. */
	init& last(page_id_t page_id)
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		return inits.find(page_id)->second;
	}

	/** Determine if a page will be initialized or freed after a time.
	@param page_id      page identifier
	@param lsn          log sequence number
	@return whether page_id will be freed or initialized after lsn */
	bool will_avoid_read(page_id_t page_id, lsn_t lsn) const
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		auto i= inits.find(page_id);
		return i != inits.end() && i->second.lsn > lsn;
	}

	/** At the end of each recovery batch, reset the 'created' flags. */
	void reset()
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		ut_ad(recv_no_ibuf_operations);
		for (map::value_type& i : inits) {
			i.second.created = false;
		}
	}

	/** On the last recovery batch, mark whether there exist
	buffered changes for the pages that were initialized
	by buf_page_create() and still reside in the buffer pool.
	@param[in,out]	mtr	dummy mini-transaction */
	void mark_ibuf_exist(mtr_t& mtr)
	{
		ut_ad(mutex_own(&recv_sys.mutex));
		mtr.start();

		for (const map::value_type& i : inits) {
			if (!i.second.created) {
				continue;
			}
			if (buf_block_t* block = buf_page_get_low(
				    i.first, 0, RW_X_LATCH, nullptr,
				    BUF_GET_IF_IN_POOL, __FILE__, __LINE__,
				    &mtr, nullptr, false)) {
				if (UNIV_LIKELY_NULL(block->page.zip.data)) {
					switch (fil_page_get_type(
							block->page.zip.data)) {
					case FIL_PAGE_INDEX:
					case FIL_PAGE_RTREE:
						if (page_zip_decompress(
							    &block->page.zip,
							    block->frame,
							    true)) {
							break;
						}
						ib::error() << "corrupted "
							    << block->page.id();
					}
				}
				if (recv_no_ibuf_operations) {
					mtr.commit();
					mtr.start();
					continue;
				}
				mutex_exit(&recv_sys.mutex);
				block->page.ibuf_exist = ibuf_page_exists(
					block->page.id(), block->zip_size());
				mtr.commit();
				mtr.start();
				mutex_enter(&recv_sys.mutex);
			}
		}

		mtr.commit();
	}

	/** Clear the data structure */
	void clear() { inits.clear(); }
};

static mlog_init_t mlog_init;

/** Process a record that indicates that a tablespace is
being shrunk in size.
@param page_id	first page identifier that is not in the file
@param lsn	log sequence number of the shrink operation */
inline void recv_sys_t::trim(const page_id_t page_id, lsn_t lsn)
{
	DBUG_ENTER("recv_sys_t::trim");
	DBUG_LOG("ib_log",
		 "discarding log beyond end of tablespace "
		 << page_id << " before LSN " << lsn);
	ut_ad(mutex_own(&mutex));
	for (recv_sys_t::map::iterator p = pages.lower_bound(page_id);
	     p != pages.end() && p->first.space() == page_id.space();) {
		recv_sys_t::map::iterator r = p++;
		if (r->second.trim(lsn)) {
			pages.erase(r);
		}
	}
	if (fil_space_t* space = fil_space_get(page_id.space())) {
		ut_ad(UT_LIST_GET_LEN(space->chain) == 1);
		fil_node_t* file = UT_LIST_GET_FIRST(space->chain);
		ut_ad(file->is_open());
		os_file_truncate(file->name, file->handle,
				 os_offset_t{page_id.page_no()}
				 << srv_page_size_shift, true);
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

void recv_sys_t::read(os_offset_t total_offset, span<byte> buf)
{
  open_log_files_if_needed();

  size_t file_idx= static_cast<size_t>(total_offset / log_sys.log.file_size);
  os_offset_t offset= total_offset % log_sys.log.file_size;
  dberr_t err= recv_sys.files[file_idx].read(offset, buf);
  ut_a(err == DB_SUCCESS);
}

inline size_t recv_sys_t::files_size()
{
  open_log_files_if_needed();
  return files.size();
}

/** Process a file name from a FILE_* record.
@param[in,out]	name		file name
@param[in]	len		length of the file name
@param[in]	space_id	the tablespace ID
@param[in]	deleted		whether this is a FILE_DELETE record */
static
void
fil_name_process(char* name, ulint len, ulint space_id, bool deleted)
{
	if (srv_operation == SRV_OPERATION_BACKUP) {
		return;
	}

	ut_ad(srv_operation == SRV_OPERATION_NORMAL
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	/* We will also insert space=NULL into the map, so that
	further checks can ensure that a FILE_MODIFY record was
	scanned before applying any page records for the space_id. */

	os_normalize_path(name);
	const file_name_t fname(std::string(name, len), deleted);
	std::pair<recv_spaces_t::iterator,bool> p = recv_spaces.emplace(
		space_id, fname);
	ut_ad(p.first->first == space_id);

	file_name_t&	f = p.first->second;

	if (deleted) {
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
		fil_space_t*	space;

		/* Check if the tablespace file exists and contains
		the space_id. If not, ignore the file after displaying
		a note. Abort if there are multiple files with the
		same space_id. */
		switch (fil_ibd_load(space_id, name, space)) {
		case FIL_LOAD_OK:
			ut_ad(space != NULL);

			if (f.space == NULL || f.space == space) {

				if (f.size && f.space == NULL) {
					fil_space_set_recv_size(space->id, f.size);
				}

				f.name = fname.name;
				f.space = space;
				f.status = file_name_t::NORMAL;
			} else {
				ib::error() << "Tablespace " << space_id
					<< " has been found in two places: '"
					<< f.name << "' and '" << name << "'."
					" You must delete one of them.";
				recv_sys.found_corrupt_fs = true;
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

			if (srv_force_recovery) {
				/* Without innodb_force_recovery,
				missing tablespaces will only be
				reported in
				recv_init_crash_recovery_spaces().
				Enable some more diagnostics when
				forcing recovery. */

				ib::info()
					<< "At LSN: " << recv_sys.recovered_lsn
					<< ": unable to open file " << name
					<< " for tablespace " << space_id;
			}
			break;

		case FIL_LOAD_INVALID:
			ut_ad(space == NULL);
			if (srv_force_recovery == 0) {
				ib::warn() << "We do not continue the crash"
					" recovery, because the table may"
					" become corrupt if we cannot apply"
					" the log records in the InnoDB log to"
					" it. To fix the problem and start"
					" mysqld:";
				ib::info() << "1) If there is a permission"
					" problem in the file and mysqld"
					" cannot open the file, you should"
					" modify the permissions.";
				ib::info() << "2) If the tablespace is not"
					" needed, or you can restore an older"
					" version from a backup, then you can"
					" remove the .ibd file, and use"
					" --innodb_force_recovery=1 to force"
					" startup without this file.";
				ib::info() << "3) If the file system or the"
					" disk is broken, and you cannot"
					" remove the .ibd file, you can set"
					" --innodb_force_recovery.";
				recv_sys.found_corrupt_fs = true;
				break;
			}

			ib::info() << "innodb_force_recovery was set to "
				<< srv_force_recovery << ". Continuing crash"
				" recovery even though we cannot access the"
				" files for tablespace " << space_id << ".";
			break;
		}
	}
}

/** Clean up after recv_sys_t::create() */
void recv_sys_t::close()
{
	ut_ad(this == &recv_sys);
	ut_ad(!recv_writer_thread_active);

	if (is_initialised()) {
		dblwr.pages.clear();
		ut_d(mutex_enter(&mutex));
		clear();
		ut_d(mutex_exit(&mutex));

		if (flush_start) {
			os_event_destroy(flush_start);
		}

		if (flush_end) {
			os_event_destroy(flush_end);
		}

		if (buf) {
			ut_free_dodump(buf, RECV_PARSING_BUF_SIZE);
			buf = NULL;
		}

		last_stored_lsn = 0;
		mutex_free(&writer_mutex);
		mutex_free(&mutex);
	}

	recv_spaces.clear();
	mlog_init.clear();

	files.clear();
}

/******************************************************************//**
recv_writer thread tasked with flushing dirty pages from the buffer
pools.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(recv_writer_thread)(
/*===============================*/
	void*	arg MY_ATTRIBUTE((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	my_thread_init();
	ut_ad(!srv_read_only_mode);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(recv_writer_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib::info() << "recv_writer thread running, id "
		<< os_thread_pf(os_thread_get_curr_id());
#endif /* UNIV_DEBUG_THREAD_CREATION */

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {

		/* Wait till we get a signal to clean the LRU list.
		Bounded by max wait time of 100ms. */
		int64_t      sig_count = os_event_reset(buf_flush_event);
		os_event_wait_time_low(buf_flush_event, 100000, sig_count);

		mutex_enter(&recv_sys.writer_mutex);

		if (!recv_recovery_is_on()) {
			mutex_exit(&recv_sys.writer_mutex);
			break;
		}

		/* Flush pages from end of LRU if required */
		os_event_reset(recv_sys.flush_end);
		recv_sys.flush_lru = true;
		os_event_set(recv_sys.flush_start);
		os_event_wait(recv_sys.flush_end);

		mutex_exit(&recv_sys.writer_mutex);
	}

	recv_writer_thread_active = false;

	my_thread_end();
	/* We count the number of threads in os_thread_exit().
	A created thread should always use that to exit and not
	use return() to exit. */
	os_thread_exit();

	OS_THREAD_DUMMY_RETURN;
}

/** Initialize the redo log recovery subsystem. */
void recv_sys_t::create()
{
	ut_ad(this == &recv_sys);
	ut_ad(!is_initialised());
	ut_ad(!flush_start);
	ut_ad(!flush_end);
	mutex_create(LATCH_ID_RECV_SYS, &mutex);
	mutex_create(LATCH_ID_RECV_WRITER, &writer_mutex);

	if (!srv_read_only_mode) {
		flush_start = os_event_create(0);
		flush_end = os_event_create(0);
	}

	flush_lru = true;
	apply_log_recs = false;
	apply_batch_on = false;

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
	recv_max_page_lsn = 0;

	memset(truncated_undo_spaces, 0, sizeof truncated_undo_spaces);
	last_stored_lsn = 1;
	UT_LIST_INIT(blocks, &buf_block_t::unzip_LRU);
}

/** Clear a fully processed set of stored redo log records. */
inline void recv_sys_t::clear()
{
  ut_ad(mutex_own(&mutex));
  apply_log_recs= false;
  apply_batch_on= false;
  ut_ad(!after_apply || !UT_LIST_GET_LAST(blocks));
  pages.clear();

  for (buf_block_t *block= UT_LIST_GET_LAST(blocks); block; )
  {
    buf_block_t *prev_block= UT_LIST_GET_PREV(unzip_LRU, block);
    ut_ad(block->page.state() == BUF_BLOCK_MEMORY);
    UT_LIST_REMOVE(blocks, block);
    MEM_MAKE_ADDRESSABLE(block->frame, srv_page_size);
    buf_block_free(block);
    block= prev_block;
  }
}

/** Free most recovery data structures. */
void recv_sys_t::debug_free()
{
	ut_ad(this == &recv_sys);
	ut_ad(is_initialised());
	mutex_enter(&mutex);

	pages.clear();
	ut_free_dodump(buf, RECV_PARSING_BUF_SIZE);

	buf = NULL;

	/* wake page cleaner up to progress */
	if (!srv_read_only_mode) {
		ut_ad(!recv_recovery_is_on());
		ut_ad(!recv_writer_thread_active);
		os_event_reset(buf_flush_event);
		os_event_set(flush_start);
	}

	mutex_exit(&mutex);
}

inline void *recv_sys_t::alloc(size_t len)
{
  ut_ad(mutex_own(&mutex));
  ut_ad(len);
  ut_ad(len <= srv_page_size);

  buf_block_t *block= UT_LIST_GET_FIRST(blocks);
  if (UNIV_UNLIKELY(!block))
  {
create_block:
    block= buf_block_alloc();
    block->page.access_time= 1U << 16 |
      ut_calc_align<uint16_t>(static_cast<uint16_t>(len), ALIGNMENT);
    static_assert(ut_is_2pow(ALIGNMENT), "ALIGNMENT must be a power of 2");
    UT_LIST_ADD_FIRST(blocks, block);
    MEM_MAKE_ADDRESSABLE(block->frame, len);
    MEM_NOACCESS(block->frame + len, srv_page_size - len);
    return my_assume_aligned<ALIGNMENT>(block->frame);
  }

  size_t free_offset= static_cast<uint16_t>(block->page.access_time);
  ut_ad(!ut_2pow_remainder(free_offset, ALIGNMENT));
  if (UNIV_UNLIKELY(!free_offset))
  {
    ut_ad(srv_page_size == 65536);
    goto create_block;
  }
  ut_ad(free_offset <= srv_page_size);
  free_offset+= len;

  if (free_offset > srv_page_size)
    goto create_block;

  block->page.access_time= ((block->page.access_time >> 16) + 1) << 16 |
    ut_calc_align<uint16_t>(static_cast<uint16_t>(free_offset), ALIGNMENT);
  MEM_MAKE_ADDRESSABLE(block->frame + free_offset - len, len);
  return my_assume_aligned<ALIGNMENT>(block->frame + free_offset - len);
}


/** Free a redo log snippet.
@param data buffer returned by alloc() */
inline void recv_sys_t::free(const void *data)
{
  ut_ad(!ut_align_offset(data, ALIGNMENT));
  data= page_align(data);
  ut_ad(mutex_own(&mutex));

  /* MDEV-14481 FIXME: To prevent race condition with buf_pool.resize(),
  we must acquire and hold the buffer pool mutex here. */
  ut_ad(!buf_pool.resize_in_progress());

  auto *chunk= buf_pool.chunks;
  for (auto i= buf_pool.n_chunks; i--; chunk++)
  {
    if (data < chunk->blocks->frame)
      continue;
    const size_t offs= (reinterpret_cast<const byte*>(data) -
                        chunk->blocks->frame) >> srv_page_size_shift;
    if (offs >= chunk->size)
      continue;
    buf_block_t *block= &chunk->blocks[offs];
    ut_ad(block->frame == data);
    ut_ad(block->page.state() == BUF_BLOCK_MEMORY);
    ut_ad(static_cast<uint16_t>(block->page.access_time - 1) <
          srv_page_size);
    ut_ad(block->page.access_time >= 1U << 16);
    if (!((block->page.access_time -= 1U << 16) >> 16))
    {
      UT_LIST_REMOVE(blocks, block);
      MEM_MAKE_ADDRESSABLE(block->frame, srv_page_size);
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
	ut_ad(log_sys.mutex.is_owned());
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

	recv_sys.read(source_offset, {buf, len});

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

		if (UNIV_UNLIKELY(crc != cksum)) {
			ib::error() << "Invalid log block checksum."
				    << " block: " << block_number
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
			recv_sys.found_corrupt_log = true;
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
		log_mutex_enter();
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
    log_sys.log.read(field, {buf, OS_FILE_LOG_BLOCK_SIZE});

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
      ib::error() << "Decrypting checkpoint failed";
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
    ib::error() << "Upgrade after a crash is not supported."
            " This redo log was created before MariaDB 10.2.2,"
            " and we did not find a valid checkpoint."
            " Please follow the instructions at"
            " https://mariadb.com/kb/en/library/upgrading/";
    return DB_ERROR;
  }

  log_sys.set_lsn(lsn);
  log_sys.set_flushed_lsn(lsn);
  const lsn_t source_offset= log_sys.log.calc_lsn_offset_old(lsn);

  static constexpr char NO_UPGRADE_RECOVERY_MSG[]=
    "Upgrade after a crash is not supported."
    " This redo log was created before MariaDB 10.2.2";

  recv_sys.read(source_offset & ~511, {buf, 512});

  if (log_block_calc_checksum_format_0(buf) != log_block_get_checksum(buf) &&
      !log_crypt_101_read_block(buf, lsn))
  {
    ib::error() << NO_UPGRADE_RECOVERY_MSG << ", and it appears corrupted.";
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
    ib::error() << "Cannot decrypt log for upgrading."
                   " The encrypted log was created before MariaDB 10.2.2.";
  else
    ib::error() << NO_UPGRADE_RECOVERY_MSG << ".";

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

	recv_sys.read(source_offset & ~(OS_FILE_LOG_BLOCK_SIZE - 1),
		      {buf, OS_FILE_LOG_BLOCK_SIZE});

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

	log_sys.log.read(0, {buf, OS_FILE_LOG_BLOCK_SIZE});
	/* Check the header page checksum. There was no
	checksum in the first redo log format (version 0). */
	log_sys.log.format = mach_read_from_4(buf + LOG_HEADER_FORMAT);
	log_sys.log.subformat = log_sys.log.format != log_t::FORMAT_3_23
		? mach_read_from_4(buf + LOG_HEADER_SUBFORMAT)
		: 0;
	if (log_sys.log.format != log_t::FORMAT_3_23
	    && !recv_check_log_header_checksum(buf)) {
		ib::error() << "Invalid redo log header checksum.";
		return(DB_CORRUPTION);
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
		ib::error() << "Unsupported redo log format."
			" The redo log was created with " << creator << ".";
		return(DB_ERROR);
	}

	for (field = LOG_CHECKPOINT_1; field <= LOG_CHECKPOINT_2;
	     field += LOG_CHECKPOINT_2 - LOG_CHECKPOINT_1) {
		log_sys.log.read(field, {buf, OS_FILE_LOG_BLOCK_SIZE});

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
			ib::error() << "Reading checkpoint"
				" encryption info failed.";
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
		ib::error() << "No valid checkpoint found"
			" (corrupted redo log)."
			" You can try --innodb-force-recovery=6"
			" as a last resort.";
		return(DB_ERROR);
	}

	switch (log_sys.log.format) {
	case log_t::FORMAT_10_5:
	case log_t::FORMAT_10_5 | log_t::FORMAT_ENCRYPTED:
		break;
	default:
		if (dberr_t err = recv_log_recover_10_4()) {
			ib::error()
				<< "Upgrade after a crash is not supported."
				" The redo log was created with " << creator
				<< (err == DB_ERROR
				    ? "." : ", and it appears corrupted.");
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
    if (log.head->lsn >= start_lsn) return false;
    last_offset= 1; /* the next record must not be same_page */
    log_rec_t *next= log.head->next;
    recv_sys.free(log.head);
    log.head= next;
  }
  log.tail= nullptr;
  return true;
}


inline void page_recv_t::recs_t::clear()
{
  ut_ad(mutex_own(&recv_sys.mutex));
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
  ut_ad(state == RECV_NOT_PROCESSED || state == RECV_WILL_NOT_READ);
  state= RECV_WILL_NOT_READ;
  log.clear();
}


/** Register a redo log snippet for a page.
@param page_id  page identifier
@param start_lsn start LSN of the mini-transaction
@param lsn      @see mtr_t::commit_lsn()
@param recs     redo log snippet @see log_t::FORMAT_10_5
@param len      length of l, in bytes */
inline void recv_sys_t::add(const page_id_t page_id,
                            lsn_t start_lsn, lsn_t lsn, const byte *l,
                            size_t len)
{
  ut_ad(mutex_own(&mutex));
  std::pair<map::iterator, bool> p= pages.emplace(map::value_type
                                                  (page_id, page_recv_t()));
  page_recv_t& recs= p.first->second;
  ut_ad(p.second == recs.log.empty());

  switch (*l & 0x70) {
  case FREE_PAGE: case INIT_PAGE:
    recs.will_not_read();
    mlog_init.add(page_id, start_lsn); /* FIXME: remove this! */
    /* fall through */
  default:
    log_phys_t *tail= static_cast<log_phys_t*>(recs.log.last());
    if (!tail)
      break;
    if (tail->start_lsn != start_lsn)
      break;
    ut_ad(tail->lsn == lsn);
    buf_block_t *block= UT_LIST_GET_LAST(blocks);
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
      return;
    }
    if (end <= &block->frame[used - ALIGNMENT] || &block->frame[used] >= end)
      break; /* Not the last allocated record in the page */
    const size_t new_used= static_cast<size_t>(end - block->frame + len + 1);
    ut_ad(new_used > used);
    if (new_used > srv_page_size)
      break;
    block->page.access_time= (block->page.access_time & ~0U << 16) |
      ut_calc_align<uint16_t>(static_cast<uint16_t>(new_used), ALIGNMENT);
    goto append;
  }
  recs.log.append(new (alloc(log_phys_t::alloc_size(len)))
                  log_phys_t(start_lsn, lsn, l, len));
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

/** Parse and register one mini-transaction in log_t::FORMAT_10_5.
@param checkpoint_lsn  the log sequence number of the latest checkpoint
@param store           whether to store the records
@param apply           whether to apply file-level log records
@return whether FILE_CHECKPOINT record was seen the first time,
or corruption was noticed */
bool recv_sys_t::parse(lsn_t checkpoint_lsn, store_t *store, bool apply)
{
  ut_ad(log_mutex_own());
  ut_ad(mutex_own(&mutex));
  ut_ad(parse_start_lsn);
  ut_ad(log_sys.is_physical());

  bool last_phase= (*store == STORE_IF_EXISTS);
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
#if 1 /* MDEV-14425 FIXME: remove this */
  bool got_page_op= false;
#endif
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
      ut_d(if ((b & 0x70) == INIT_PAGE) freed.erase(id));
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
          truncated_undo_spaces[space_id - srv_undo_space_id_start]=
            { recovered_lsn, page_no };
#endif
          last_offset= 1; /* the next record must not be same_page  */
          continue;
        }
        last_offset= FIL_PAGE_TYPE;
        break;
      case RESERVED:
      case OPTION:
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
          if (UNIV_UNLIKELY(page_no == 0) && apply &&
              last_offset <= FSP_HEADER_OFFSET + FSP_SIZE &&
              last_offset + rlen >= FSP_HEADER_OFFSET + FSP_SIZE + 4)
          {
            recv_spaces_t::iterator it= recv_spaces.find(space_id);
            const uint32_t size= mach_read_from_4(FSP_HEADER_OFFSET + FSP_SIZE
                                                  + l - last_offset);
            if (it == recv_spaces.end())
              ut_ad(!mlog_checkpoint_lsn || space_id == TRX_SYS_SPACE ||
                    srv_is_undo_tablespace(space_id));
            else if (!it->second.space)
              it->second.size= size;
            fil_space_set_recv_size(space_id, size);
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
      case OPTION:
        ut_ad(0); /* we did "continue" earlier */
        break;
      case FREE_PAGE:
        break;
      default:
        ut_ad(modified.emplace(id).second || (b & 0x70) != INIT_PAGE);
      }
#endif
      const bool is_init= (b & 0x70) <= INIT_PAGE;
      switch (*store) {
      case STORE_IF_EXISTS:
        if (!fil_space_get_size(space_id))
          continue;
        /* fall through */
      case STORE_YES:
        if (!mlog_init.will_avoid_read(id, start_lsn))
          add(id, start_lsn, end_lsn, recs,
              static_cast<size_t>(l + rlen - recs));
        continue;
      case STORE_NO:
        if (!is_init)
          continue;
        mlog_init.add(id, start_lsn);
        map::iterator i= pages.find(id);
        if (i == pages.end())
          continue;
        i->second.log.clear();
        pages.erase(i);
      }
    }
#if 1 /* MDEV-14425 FIXME: this must be in the checkpoint file only! */
    else if (rlen)
    {
      switch (b & 0xf0) {
# if 1 /* MDEV-14425 FIXME: Remove this! */
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
            ut_ad(mlog_checkpoint_lsn <= recovered_lsn);
            if (mlog_checkpoint_lsn)
              continue;
            mlog_checkpoint_lsn= recovered_lsn;
            l+= 8;
            recovered_offset= l - buf;
            return true;
          }
          continue;
        }
# endif
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

        const char saved_end= fn[rlen];
        const_cast<char&>(fn[rlen])= '\0';
        fil_name_process(const_cast<char*>(fn), fnend - fn, space_id,
                         (b & 0xf0) == FILE_DELETE);
        if (fn2)
          fil_name_process(const_cast<char*>(fn2), fn2end - fn2, space_id,
                           false);
        if ((b & 0xf0) < FILE_MODIFY && log_file_op)
          log_file_op(space_id, (b & 0xf0) == FILE_CREATE,
                      l, static_cast<ulint>(fnend - fn),
                      reinterpret_cast<const byte*>(fn2),
                      fn2 ? static_cast<ulint>(fn2end - fn2) : 0);

        if (!fn2 || !apply);
        else if (!fil_op_replay_rename(space_id, fn, fn2))
          found_corrupt_fs= true;
        const_cast<char&>(fn[rlen])= saved_end;
        if (UNIV_UNLIKELY(found_corrupt_fs))
          return true;
      }
    }
#endif
    else
      goto malformed;
  }

  ut_ad(l == el);
  recovered_offset= l - buf;
  recovered_lsn= end_lsn;
  if (is_memory_exhausted(store) && last_phase)
    return false;
  goto loop;
}

/** Apply the hashed log records to the page, if the page lsn is less than the
lsn of a log record.
@param[in,out]	block		buffer pool page
@param[in,out]	mtr		mini-transaction
@param[in,out]	p		recovery address
@param[in,out]	space		tablespace, or NULL if not looked up yet
@param[in,out]	init		page initialization operation, or NULL */
static void recv_recover_page(buf_block_t* block, mtr_t& mtr,
			      const recv_sys_t::map::iterator& p,
			      fil_space_t* space = NULL,
			      mlog_init_t::init* init = NULL)
{
	ut_ad(mutex_own(&recv_sys.mutex));
	ut_ad(recv_sys.apply_log_recs);
	ut_ad(recv_needed_recovery);
	ut_ad(!init || init->created);
	ut_ad(!init || init->lsn);
	ut_ad(block->page.id() == p->first);
	ut_ad(!p->second.is_being_processed());
	ut_ad(!space || space->id == block->page.id().space());
	ut_ad(log_sys.is_physical());

	if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
		ib::info() << "Applying log to page " << block->page.id();
	}

	DBUG_PRINT("ib_log", ("Applying log to page %u:%u",
			      block->page.id().space(),
			      block->page.id().page_no()));

	p->second.state = page_recv_t::RECV_BEING_PROCESSED;

	mutex_exit(&recv_sys.mutex);

	byte *frame = UNIV_LIKELY_NULL(block->page.zip.data)
		? block->page.zip.data
		: block->frame;
	const lsn_t page_lsn = init
		? 0
		: mach_read_from_8(frame + FIL_PAGE_LSN);
	bool free_page = false;
	lsn_t start_lsn = 0, end_lsn = 0;
	ut_d(lsn_t recv_start_lsn = 0);
	const lsn_t init_lsn = init ? init->lsn : 0;

	for (const log_rec_t* recv : p->second.log) {
		const log_phys_t* l = static_cast<const log_phys_t*>(recv);
		ut_ad(l->lsn);
		ut_ad(end_lsn <= l->lsn);
		end_lsn = l->lsn;
		ut_ad(end_lsn <= log_sys.log.scanned_lsn);

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
			continue;
		}

		if (l->start_lsn < init_lsn) {
			DBUG_PRINT("ib_log", ("init skip %u:%u LSN " LSN_PF
					      " < " LSN_PF,
					      block->page.id().space(),
					      block->page.id().page_no(),
					      l->start_lsn, init_lsn));
			continue;
		}


		if (UNIV_UNLIKELY(srv_print_verbose_log == 2)) {
			ib::info() << "apply " << l->start_lsn
				   << ": " << block->page.id();
		}

		DBUG_PRINT("ib_log", ("apply " LSN_PF ": %u:%u",
				      l->start_lsn,
				      block->page.id().space(),
				      block->page.id().page_no()));

		log_phys_t::apply_status a= l->apply(*block,
						     p->second.last_offset);

		switch (a) {
		case log_phys_t::APPLIED_NO:
			ut_ad(!mtr.has_modifications());
			free_page = true;
			start_lsn = 0;
			continue;
		case log_phys_t::APPLIED_YES:
			goto set_start_lsn;
		case log_phys_t::APPLIED_TO_FSP_HEADER:
		case log_phys_t::APPLIED_TO_ENCRYPTION:
			break;
		}

		if (fil_space_t* s = space
		    ? space
		    : fil_space_acquire(block->page.id().space())) {
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

			if (s != space) {
				s->release();
			}
		}

set_start_lsn:
		if (recv_sys.found_corrupt_log && !srv_force_recovery) {
			break;
		}

		if (!start_lsn) {
			start_lsn = l->start_lsn;
		}
	}

	if (start_lsn) {
		ut_ad(end_lsn >= start_lsn);
		mach_write_to_8(FIL_PAGE_LSN + frame, end_lsn);
		if (UNIV_LIKELY(frame == block->frame)) {
			mach_write_to_8(srv_page_size
					- FIL_PAGE_END_LSN_OLD_CHKSUM
					+ frame, end_lsn);
		} else {
			buf_zip_decompress(block, false);
		}

		buf_block_modify_clock_inc(block);
		log_flush_order_mutex_enter();
		buf_flush_note_modification(block, start_lsn, end_lsn);
		log_flush_order_mutex_exit();
	} else if (free_page && init) {
		/* There have been no operations that modify the page.
		Any buffered changes must not be merged. A subsequent
		buf_page_create() from a user thread should discard
		any buffered changes. */
		init->created = false;
		ut_ad(!mtr.has_modifications());
	}

	/* Make sure that committing mtr does not change the modification
	lsn values of page */

	mtr.discard_modifications();
	mtr.commit();

	time_t now = time(NULL);

	mutex_enter(&recv_sys.mutex);

	if (recv_max_page_lsn < page_lsn) {
		recv_max_page_lsn = page_lsn;
	}

	ut_ad(p->second.is_being_processed());
	ut_ad(!recv_sys.pages.empty());

	if (recv_sys.report(now)) {
		const ulint n = recv_sys.pages.size();
		ib::info() << "To recover: " << n << " pages from log";
		service_manager_extend_timeout(
			INNODB_EXTEND_TIMEOUT_INTERVAL, "To recover: " ULINTPF " pages from log", n);
	}
}

/** Remove records for a corrupted page.
This function should only be called when innodb_force_recovery is set.
@param page_id  corrupted page identifier */
ATTRIBUTE_COLD void recv_sys_t::free_corrupted_page(page_id_t page_id)
{
  mutex_enter(&mutex);
  map::iterator p= pages.find(page_id);
  if (p != pages.end())
  {
    p->second.log.clear();
    pages.erase(p);
  }
  mutex_exit(&mutex);
}

/** Apply any buffered redo log to a page that was just read from a data file.
@param[in,out]	space	tablespace
@param[in,out]	bpage	buffer pool page */
void recv_recover_page(fil_space_t* space, buf_page_t* bpage)
{
	mtr_t mtr;
	mtr.start();
	mtr.set_log_mode(MTR_LOG_NONE);

	ut_ad(bpage->state() == BUF_BLOCK_FILE_PAGE);
	buf_block_t* block = reinterpret_cast<buf_block_t*>(bpage);

	/* Move the ownership of the x-latch on the page to
	this OS thread, so that we can acquire a second
	x-latch on it.  This is needed for the operations to
	the page to pass the debug checks. */
	rw_lock_x_lock_move_ownership(&block->lock);
	buf_block_buf_fix_inc(block, __FILE__, __LINE__);
	rw_lock_x_lock(&block->lock);
	mtr.memo_push(block, MTR_MEMO_PAGE_X_FIX);

	mutex_enter(&recv_sys.mutex);
	if (recv_sys.apply_log_recs) {
		recv_sys_t::map::iterator p = recv_sys.pages.find(bpage->id());
		if (p != recv_sys.pages.end()
		    && !p->second.is_being_processed()) {
			recv_recover_page(block, mtr, p, space);
			p->second.log.clear();
			recv_sys.pages.erase(p);
			goto func_exit;
		}
	}

	mtr.commit();
func_exit:
	mutex_exit(&recv_sys.mutex);
	ut_ad(mtr.has_committed());
}

/** Reads in pages which have hashed log records, from an area around a given
page number.
@param[in]	page_id	page id */
static void recv_read_in_area(page_id_t page_id)
{
	ulint	page_nos[RECV_READ_AHEAD_AREA];
	compile_time_assert(ut_is_2pow(RECV_READ_AHEAD_AREA));
	page_id.set_page_no(ut_2pow_round(page_id.page_no(),
					  RECV_READ_AHEAD_AREA));
	const ulint up_limit = page_id.page_no() + RECV_READ_AHEAD_AREA;
	ulint*	p = page_nos;

	for (recv_sys_t::map::iterator i= recv_sys.pages.lower_bound(page_id);
	     i != recv_sys.pages.end()
	     && i->first.space() == page_id.space()
	     && i->first.page_no() < up_limit; i++) {
		if (i->second.state == page_recv_t::RECV_NOT_PROCESSED
		    && !buf_pool.page_hash_contains(i->first)) {
			i->second.state = page_recv_t::RECV_BEING_READ;
			*p++ = i->first.page_no();
		}
	}

	if (p != page_nos) {
		mutex_exit(&recv_sys.mutex);
		buf_read_recv_pages(FALSE, page_id.space(), page_nos,
				    ulint(p - page_nos));
		mutex_enter(&recv_sys.mutex);
	}
}

/** Attempt to initialize a page based on redo log records.
@param page_id  page identifier
@param p        iterator pointing to page_id
@param mtr      mini-transaction
@return whether the page was successfully initialized */
inline buf_block_t *recv_sys_t::recover_low(const page_id_t page_id,
                                            map::iterator &p, mtr_t &mtr)
{
  ut_ad(mutex_own(&mutex));
  ut_ad(p->first == page_id);
  page_recv_t &recs= p->second;
  ut_ad(recs.state == page_recv_t::RECV_WILL_NOT_READ);
  buf_block_t* block= nullptr;
  mlog_init_t::init &i= mlog_init.last(page_id);
  const lsn_t end_lsn = recs.log.last()->lsn;
  if (end_lsn < i.lsn)
    DBUG_LOG("ib_log", "skip log for page " << page_id
             << " LSN " << end_lsn << " < " << i.lsn);
  else if (fil_space_t *space= fil_space_acquire_for_io(page_id.space()))
  {
    mtr.start();
    mtr.set_log_mode(MTR_LOG_NONE);
    block= buf_page_create(space, page_id.page_no(), space->zip_size(), &mtr);
    p= recv_sys.pages.find(page_id);
    if (p == recv_sys.pages.end())
    {
      /* The page happened to exist in the buffer pool, or it was just
      being read in. Before buf_page_get_with_no_latch() returned to
      buf_page_create(), all changes must have been applied to the
      page already. */
      mtr.commit();
      block= nullptr;
    }
    else
    {
      ut_ad(&recs == &p->second);
      i.created= true;
      buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);
      mtr.x_latch_at_savepoint(0, block);
      recv_recover_page(block, mtr, p, space, &i);
      ut_ad(mtr.has_committed());
      recs.log.clear();
      map::iterator r= p++;
      recv_sys.pages.erase(r);
    }
    space->release_for_io();
  }

  return block;
}

/** Attempt to initialize a page based on redo log records.
@param page_id  page identifier
@return whether the page was successfully initialized */
buf_block_t *recv_sys_t::recover_low(const page_id_t page_id)
{
  buf_block_t *block= nullptr;

  mutex_enter(&mutex);
  map::iterator p= pages.find(page_id);

  if (p != pages.end() && p->second.state == page_recv_t::RECV_WILL_NOT_READ)
  {
    mtr_t mtr;
    block= recover_low(page_id, p, mtr);
  }

  mutex_exit(&mutex);
  return block;
}

/** Apply buffered log to persistent data pages.
@param last_batch     whether it is possible to write more redo log */
void recv_sys_t::apply(bool last_batch)
{
  ut_ad(srv_operation == SRV_OPERATION_NORMAL ||
        srv_operation == SRV_OPERATION_RESTORE ||
        srv_operation == SRV_OPERATION_RESTORE_EXPORT);

  mutex_enter(&mutex);

  while (apply_batch_on)
  {
    bool abort= found_corrupt_log;
    mutex_exit(&mutex);

    if (abort)
      return;

    os_thread_sleep(500000);
    mutex_enter(&mutex);
  }

  ut_ad(!last_batch == log_mutex_own());

  recv_no_ibuf_operations = !last_batch ||
    srv_operation == SRV_OPERATION_RESTORE ||
    srv_operation == SRV_OPERATION_RESTORE_EXPORT;

  ut_d(recv_no_log_write = recv_no_ibuf_operations);

  mtr_t mtr;

  if (!pages.empty())
  {
    const char *msg= last_batch
      ? "Starting final batch to recover "
      : "Starting a batch to recover ";
    const ulint n= pages.size();
    ib::info() << msg << n << " pages from redo log.";
    sd_notifyf(0, "STATUS=%s" ULINTPF " pages from redo log", msg, n);

    apply_log_recs= true;
    apply_batch_on= true;

    for (auto id= srv_undo_tablespaces_open; id--;)
    {
      const trunc& t= truncated_undo_spaces[id];
      if (t.lsn)
        trim(page_id_t(id + srv_undo_space_id_start, t.pages), t.lsn);
    }

    for (map::iterator p= pages.begin(); p != pages.end(); )
    {
      const page_id_t page_id= p->first;
      page_recv_t &recs= p->second;
      ut_ad(!recs.log.empty());

      switch (recs.state) {
      case page_recv_t::RECV_BEING_READ:
      case page_recv_t::RECV_BEING_PROCESSED:
        p++;
        continue;
      case page_recv_t::RECV_WILL_NOT_READ:
        recover_low(page_id, p, mtr);
        continue;
      case page_recv_t::RECV_NOT_PROCESSED:
        mtr.start();
        mtr.set_log_mode(MTR_LOG_NONE);
        if (buf_block_t *block= buf_page_get_low(page_id, 0, RW_X_LATCH,
                                                 nullptr, BUF_GET_IF_IN_POOL,
                                                 __FILE__, __LINE__,
                                                 &mtr, nullptr, false))
        {
          buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);
          recv_recover_page(block, mtr, p);
          ut_ad(mtr.has_committed());
        }
        else
        {
          mtr.commit();
          recv_read_in_area(page_id);
          break;
        }
        map::iterator r= p++;
        r->second.log.clear();
        pages.erase(r);
        continue;
      }

      p= pages.lower_bound(page_id);
    }

    /* Wait until all the pages have been processed */
    while (!pages.empty())
    {
      const bool abort= found_corrupt_log || found_corrupt_fs;

      if (found_corrupt_fs && !srv_force_recovery)
        ib::info() << "Set innodb_force_recovery=1 to ignore corrupted pages.";

      mutex_exit(&mutex);

      if (abort)
        return;
      os_thread_sleep(500000);
      mutex_enter(&mutex);
    }
  }

  if (!last_batch)
  {
    /* Flush all the file pages to disk and invalidate them in buf_pool */
    mutex_exit(&mutex);
    log_mutex_exit();

    /* Stop the recv_writer thread from issuing any LRU flush batches. */
    mutex_enter(&writer_mutex);

    /* Wait for any currently run batch to end. */
    buf_flush_wait_LRU_batch_end();

    os_event_reset(flush_end);
    flush_lru= false;
    os_event_set(flush_start);
    os_event_wait(flush_end);

    buf_pool_invalidate();

    /* Allow batches from recv_writer thread. */
    mutex_exit(&writer_mutex);

    log_mutex_enter();
    mutex_enter(&mutex);
    mlog_init.reset();
  }
  else
    /* We skipped this in buf_page_create(). */
    mlog_init.mark_ibuf_exist(mtr);

  ut_d(after_apply= true);
  clear();
  mutex_exit(&mutex);
}

/** Check whether the number of read redo log blocks exceeds the maximum.
Store last_stored_lsn if the recovery is not in the last phase.
@param[in,out] store    whether to store page operations
@return whether the memory is exhausted */
inline bool recv_sys_t::is_memory_exhausted(store_t *store)
{
  if (*store == STORE_NO ||
      UT_LIST_GET_LEN(blocks) * 3 < buf_pool.get_n_pages())
    return false;
  if (*store == STORE_YES)
    last_stored_lsn= recovered_lsn;
  *store= STORE_NO;
  DBUG_PRINT("ib_log",("Ran out of memory and last stored lsn " LSN_PF
                       " last stored offset " ULINTPF "\n",
                       recovered_lsn, recovered_offset));
  return true;
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
	const bool	last_phase = (*store == STORE_IF_EXISTS);
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
					" checkpoint LSN="
					<< recv_sys.scanned_lsn;
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

				recv_sys.found_corrupt_log = true;

				if (!srv_force_recovery) {
					ib::error()
						<< "Set innodb_force_recovery"
						" to ignore this error.";
					return(true);
				}
			} else if (!recv_sys.found_corrupt_log) {
				more_data = recv_sys_add_to_parsing_buf(
					log_block, scanned_lsn);
			}

			recv_sys.scanned_lsn = scanned_lsn;
			recv_sys.scanned_checkpoint_no
				= log_block_get_checkpoint_no(log_block);
		}

		/* During last phase of scanning, there can be redo logs
		left in recv_sys.buf to parse & store it in recv_sys.heap */
		if (last_phase
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

	mutex_enter(&recv_sys.mutex);

	if (more_data && !recv_sys.found_corrupt_log) {
		/* Try to parse more log records */
		if (recv_sys.parse(checkpoint_lsn, store, apply)) {
			ut_ad(recv_sys.found_corrupt_log
			      || recv_sys.found_corrupt_fs
			      || recv_sys.mlog_checkpoint_lsn
			      == recv_sys.recovered_lsn);
			finished = true;
			goto func_exit;
		}

		recv_sys.is_memory_exhausted(store);

		if (recv_sys.recovered_offset > recv_parsing_buf_size / 4
		    || (recv_sys.recovered_offset
			&& recv_sys.len
			>= recv_parsing_buf_size - RECV_SCAN_SIZE)) {
			/* Move parsing buffer data to the buffer start */
			recv_sys_justify_left_parsing_buf();
		}

		/* Need to re-parse the redo log which're stored
		in recv_sys.buf */
		if (last_phase && *store == STORE_NO) {
			finished = false;
		}
	}

func_exit:
	mutex_exit(&recv_sys.mutex);
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

	mutex_enter(&recv_sys.mutex);
	recv_sys.len = 0;
	recv_sys.recovered_offset = 0;
	recv_sys.clear();
	recv_sys.parse_start_lsn = *contiguous_lsn;
	recv_sys.scanned_lsn = *contiguous_lsn;
	recv_sys.recovered_lsn = *contiguous_lsn;
	recv_sys.scanned_checkpoint_no = 0;
	ut_ad(recv_max_page_lsn == 0);
	ut_ad(last_phase || !recv_writer_thread_active);
	mutex_exit(&recv_sys.mutex);

	lsn_t	start_lsn;
	lsn_t	end_lsn;
	store_t	store	= recv_sys.mlog_checkpoint_lsn == 0
		? STORE_NO : (last_phase ? STORE_IF_EXISTS : STORE_YES);

	log_sys.log.scanned_lsn = end_lsn = *contiguous_lsn =
		ut_uint64_align_down(*contiguous_lsn, OS_FILE_LOG_BLOCK_SIZE);
	ut_d(recv_sys.after_apply = last_phase);

	do {
		if (last_phase && store == STORE_NO) {
			store = STORE_IF_EXISTS;
			recv_sys.apply(false);
			/* Rescan the redo logs from last stored lsn */
			end_lsn = recv_sys.recovered_lsn;
		}

		start_lsn = ut_uint64_align_down(end_lsn,
						 OS_FILE_LOG_BLOCK_SIZE);
		end_lsn = start_lsn;
		log_sys.log.read_log_seg(&end_lsn, start_lsn + RECV_SCAN_SIZE);
	} while (end_lsn != start_lsn
		 && !recv_scan_log_recs(&store, log_sys.buf, checkpoint_lsn,
					start_lsn, end_lsn, contiguous_lsn,
					&log_sys.log.scanned_lsn));

	if (recv_sys.found_corrupt_log || recv_sys.found_corrupt_fs) {
		DBUG_RETURN(false);
	}

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
	if (srv_operation == SRV_OPERATION_RESTORE
	    || srv_operation == SRV_OPERATION_RESTORE_EXPORT) {
		if (i->second.name.find(TEMP_TABLE_PATH_PREFIX)
		    != std::string::npos) {
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

	mutex_enter(&recv_sys.mutex);

	for (recv_sys_t::map::iterator p = recv_sys.pages.begin();
	     p != recv_sys.pages.end();) {
		ut_ad(!p->second.log.empty());
		const ulint space = p->first.space();
		if (is_predefined_tablespace(space)) {
next:
			p++;
			continue;
		}

		recv_spaces_t::iterator i = recv_spaces.find(space);
		ut_ad(i != recv_spaces.end());

		switch (i->second.status) {
		case file_name_t::NORMAL:
			goto next;
		case file_name_t::MISSING:
			err = recv_init_missing_space(err, i);
			i->second.status = file_name_t::DELETED;
			/* fall through */
		case file_name_t::DELETED:
			recv_sys_t::map::iterator r = p++;
			r->second.log.clear();
			recv_sys.pages.erase(r);
			continue;
		}
		ut_ad(0);
	}

	if (err != DB_SUCCESS) {
func_exit:
		mutex_exit(&recv_sys.mutex);
		return(err);
	}

	/* When rescan is not needed, recv_sys.pages will contain the
	entire redo log. If rescan is needed or innodb_force_recovery
	is set, we can ignore missing tablespaces. */
	for (const recv_spaces_t::value_type& rs : recv_spaces) {
		if (UNIV_LIKELY(rs.second.status != file_name_t::MISSING)) {
			continue;
		}

		missing_tablespace = true;

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
		}
	}

	if (!rescan || srv_force_recovery > 0) {
		missing_tablespace = false;
	}

	err = DB_SUCCESS;
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
			recv_sys.found_corrupt_log = true;
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

/** Start recovering from a redo log checkpoint.
@see recv_recovery_from_checkpoint_finish
@param[in]	flush_lsn	FIL_PAGE_FILE_FLUSH_LSN
of first system tablespace page
@return error code or DB_SUCCESS */
dberr_t
recv_recovery_from_checkpoint_start(lsn_t flush_lsn)
{
	ulint		max_cp_field;
	lsn_t		checkpoint_lsn;
	bool		rescan = false;
	ib_uint64_t	checkpoint_no;
	lsn_t		contiguous_lsn;
	byte*		buf;
	dberr_t		err = DB_SUCCESS;

	ut_ad(srv_operation == SRV_OPERATION_NORMAL
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);
	ut_d(mutex_enter(&buf_pool.flush_list_mutex));
	ut_ad(UT_LIST_GET_LEN(buf_pool.LRU) == 0);
	ut_ad(UT_LIST_GET_LEN(buf_pool.unzip_LRU) == 0);
	ut_d(mutex_exit(&buf_pool.flush_list_mutex));

	/* Initialize red-black tree for fast insertions into the
	flush_list during recovery process. */
	buf_flush_init_flush_rbt();

	if (srv_force_recovery >= SRV_FORCE_NO_LOG_REDO) {

		ib::info() << "innodb_force_recovery=6 skips redo log apply";

		return(DB_SUCCESS);
	}

	recv_sys.recovery_on = true;

	log_mutex_enter();

	err = recv_find_max_checkpoint(&max_cp_field);

	if (err != DB_SUCCESS) {

		recv_sys.recovered_lsn = log_sys.get_lsn();
		log_mutex_exit();
		return(err);
	}

	buf = log_sys.checkpoint_buf;
	log_sys.log.read(max_cp_field, {buf, OS_FILE_LOG_BLOCK_SIZE});

	checkpoint_lsn = mach_read_from_8(buf + LOG_CHECKPOINT_LSN);
	checkpoint_no = mach_read_from_8(buf + LOG_CHECKPOINT_NO);

	/* Start reading the log from the checkpoint lsn. The variable
	contiguous_lsn contains an lsn up to which the log is known to
	be contiguously written. */
	recv_sys.mlog_checkpoint_lsn = 0;

	ut_ad(RECV_SCAN_SIZE <= srv_log_buffer_size);

	const lsn_t	end_lsn = mach_read_from_8(
		buf + LOG_CHECKPOINT_END_LSN);

	ut_ad(recv_sys.pages.empty());
	contiguous_lsn = checkpoint_lsn;
	switch (log_sys.log.format) {
	case 0:
		log_mutex_exit();
		return DB_SUCCESS;
	default:
		if (end_lsn == 0) {
			break;
		}
		if (end_lsn >= checkpoint_lsn) {
			contiguous_lsn = end_lsn;
			break;
		}
		recv_sys.found_corrupt_log = true;
		log_mutex_exit();
		return(DB_ERROR);
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
	ut_ad(!recv_sys.found_corrupt_fs);

	if (srv_read_only_mode && recv_needed_recovery) {
		log_mutex_exit();
		return(DB_READ_ONLY);
	}

	if (recv_sys.found_corrupt_log && !srv_force_recovery) {
		log_mutex_exit();
		ib::warn() << "Log scan aborted at LSN " << contiguous_lsn;
		return(DB_ERROR);
	}

	if (recv_sys.mlog_checkpoint_lsn == 0) {
		lsn_t scan_lsn = log_sys.log.scanned_lsn;
		if (!srv_read_only_mode && scan_lsn != checkpoint_lsn) {
			log_mutex_exit();
			ib::error err;
			err << "Missing FILE_CHECKPOINT";
			if (end_lsn) {
				err << " at " << end_lsn;
			}
			err << " between the checkpoint " << checkpoint_lsn
			    << " and the end " << scan_lsn << ".";
			return(DB_ERROR);
		}

		log_sys.log.scanned_lsn = checkpoint_lsn;
	} else {
		contiguous_lsn = checkpoint_lsn;
		rescan = recv_group_scan_log_recs(
			checkpoint_lsn, &contiguous_lsn, false);

		if ((recv_sys.found_corrupt_log && !srv_force_recovery)
		    || recv_sys.found_corrupt_fs) {
			log_mutex_exit();
			return(DB_ERROR);
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

		if (checkpoint_lsn + sizeof_checkpoint < flush_lsn) {
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

			ib::info()
				<< "The log sequence number " << flush_lsn
				<< " in the system tablespace does not match"
				   " the log sequence number "
				<< checkpoint_lsn << " in the "
				<< LOG_FILE_NAME << "!";

			if (srv_read_only_mode) {
				ib::error() << "innodb_read_only"
					" prevents crash recovery";
				log_mutex_exit();
				return(DB_READ_ONLY);
			}

			recv_needed_recovery = true;
		}
	}

	log_sys.set_lsn(recv_sys.recovered_lsn);

	if (recv_needed_recovery) {
		bool missing_tablespace = false;

		err = recv_init_crash_recovery_spaces(
			rescan, missing_tablespace);

		if (err != DB_SUCCESS) {
			log_mutex_exit();
			return(err);
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

			ut_ad(!recv_sys.found_corrupt_fs);

			missing_tablespace = false;

			err = recv_sys.found_corrupt_log
				? DB_ERROR
				: recv_validate_tablespace(
					rescan, missing_tablespace);

			if (err != DB_SUCCESS) {
				log_mutex_exit();
				return err;
			}

			rescan = true;
		}

		if (srv_operation == SRV_OPERATION_NORMAL) {
			buf_dblwr_process();
		}

		ut_ad(srv_force_recovery <= SRV_FORCE_NO_UNDO_LOG_SCAN);

		/* Spawn the background thread to flush dirty pages
		from the buffer pools. */
		recv_writer_thread_active = true;
		os_thread_create(recv_writer_thread, 0, 0);

		if (rescan) {
			contiguous_lsn = checkpoint_lsn;

			recv_group_scan_log_recs(
				checkpoint_lsn, &contiguous_lsn, true);

			if ((recv_sys.found_corrupt_log
			     && !srv_force_recovery)
			    || recv_sys.found_corrupt_fs) {
				log_mutex_exit();
				return(DB_ERROR);
			}
		}
	} else {
		ut_ad(!rescan || recv_sys.pages.empty());
	}

	if (log_sys.is_physical()
	    && (log_sys.log.scanned_lsn < checkpoint_lsn
		|| log_sys.log.scanned_lsn < recv_max_page_lsn)) {

		ib::error() << "We scanned the log up to "
			<< log_sys.log.scanned_lsn
			<< ". A checkpoint was at " << checkpoint_lsn << " and"
			" the maximum LSN on a database page was "
			<< recv_max_page_lsn << ". It is possible that the"
			" database is now corrupt!";
	}

	if (recv_sys.recovered_lsn < checkpoint_lsn) {
		log_mutex_exit();

		ib::error() << "Recovered only to lsn:"
			    << recv_sys.recovered_lsn << " checkpoint_lsn: " << checkpoint_lsn;

		return(DB_ERROR);
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

	if (!srv_read_only_mode && srv_operation == SRV_OPERATION_NORMAL) {
		/* Write a FILE_CHECKPOINT marker as the first thing,
		before generating any other redo log. This ensures
		that subsequent crash recovery will be possible even
		if the server were killed soon after this. */
		fil_names_clear(log_sys.last_checkpoint_lsn, true);
	}

	log_sys.next_checkpoint_no = ++checkpoint_no;

	mutex_enter(&recv_sys.mutex);

	recv_sys.apply_log_recs = true;

	mutex_exit(&recv_sys.mutex);

	log_mutex_exit();

	recv_lsn_checks_on = true;

	/* The database is now ready to start almost normal processing of user
	transactions: transaction rollbacks and the application of the log
	records in the hash table can be run in background. */

	return(DB_SUCCESS);
}

/** Complete recovery from a checkpoint. */
void
recv_recovery_from_checkpoint_finish(void)
{
	/* Make sure that the recv_writer thread is done. This is
	required because it grabs various mutexes and we want to
	ensure that when we enable sync_order_checks there is no
	mutex currently held by any thread. */
	mutex_enter(&recv_sys.writer_mutex);

	/* Free the resources of the recovery system */
	recv_sys.recovery_on = false;

	/* By acquring the mutex we ensure that the recv_writer thread
	won't trigger any more LRU batches. Now wait for currently
	in progress batches to finish. */
	buf_flush_wait_LRU_batch_end();

	mutex_exit(&recv_sys.writer_mutex);

	ulint count = 0;
	while (recv_writer_thread_active) {
		++count;
		os_thread_sleep(100000);
		if (srv_print_verbose_log && count > 600) {
			ib::info() << "Waiting for recv_writer to"
				" finish flushing of buffer pool";
			count = 0;
		}
	}

	recv_sys.debug_free();

	/* Free up the flush_rbt. */
	buf_flush_free_flush_rbt();
	/* Enable innodb_sync_debug checks */
	ut_d(sync_check_enable());
}

/** Find a doublewrite copy of a page.
@param[in]	space_id	tablespace identifier
@param[in]	page_no		page number
@return	page frame
@retval NULL if no page was found */
const byte*
recv_dblwr_t::find_page(ulint space_id, ulint page_no)
{
  const byte *result= NULL;
  lsn_t max_lsn= 0;

  for (const byte *page : pages)
  {
    if (page_get_page_no(page) != page_no ||
        page_get_space_id(page) != space_id)
      continue;
    const lsn_t lsn= mach_read_from_8(page + FIL_PAGE_LSN);
    if (lsn <= max_lsn)
      continue;
    max_lsn= lsn;
    result= page;
  }

  return result;
}
