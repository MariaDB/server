/*****************************************************************************

Copyright (c) 1994, 2016, Oracle and/or its affiliates. All Rights Reserved.
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

/**************************************************//**
@file include/page0types.h
Index page routines

Created 2/2/1994 Heikki Tuuri
*******************************************************/

#ifndef page0types_h
#define page0types_h

#include "dict0types.h"
#include "mtr0types.h"
#include "rem0types.h"
#include "ut0new.h"

#include <map>

/** Eliminates a name collision on HP-UX */
#define page_t	   ib_page_t
/** Type of the index page */
typedef	byte		page_t;
#ifndef UNIV_INNOCHECKSUM
/** Index page cursor */
struct page_cur_t;
/** Buffer pool block */
struct buf_block_t;

/** Compressed index page */
typedef byte		page_zip_t;

/* The following definitions would better belong to page0zip.h,
but we cannot include page0zip.h from rem0rec.ic, because
page0*.h includes rem0rec.h and may include rem0rec.ic. */

/** Number of bits needed for representing different compressed page sizes */
#define PAGE_ZIP_SSIZE_BITS 3

/** Maximum compressed page shift size */
#define PAGE_ZIP_SSIZE_MAX	\
	(UNIV_ZIP_SIZE_SHIFT_MAX - UNIV_ZIP_SIZE_SHIFT_MIN + 1)

/* Make sure there are enough bits available to store the maximum zip
ssize, which is the number of shifts from 512. */
#if PAGE_ZIP_SSIZE_MAX >= (1 << PAGE_ZIP_SSIZE_BITS)
# error "PAGE_ZIP_SSIZE_MAX >= (1 << PAGE_ZIP_SSIZE_BITS)"
#endif

/* Page cursor search modes; the values must be in this order! */
enum page_cur_mode_t {
	PAGE_CUR_UNSUPP	= 0,
	PAGE_CUR_G	= 1,
	PAGE_CUR_GE	= 2,
	PAGE_CUR_L	= 3,
	PAGE_CUR_LE	= 4,

/* These search mode is for search R-tree index. */
	PAGE_CUR_CONTAIN		= 7,
	PAGE_CUR_INTERSECT		= 8,
	PAGE_CUR_WITHIN			= 9,
	PAGE_CUR_DISJOINT		= 10,
	PAGE_CUR_MBR_EQUAL		= 11,
	PAGE_CUR_RTREE_INSERT		= 12,
	PAGE_CUR_RTREE_LOCATE		= 13,
	PAGE_CUR_RTREE_GET_FATHER	= 14
};

class buf_pool_t;
class buf_page_t;

/** Compressed page descriptor */
struct page_zip_des_t
{
  /** compressed page data */
  page_zip_t *data;

  /** end offset of modification log */
  uint16_t m_end;

#ifdef UNIV_DEBUG
  /** start offset of the modification log */
  uint16_t m_start;
#endif /* UNIV_DEBUG */

private:
  /** width of the packed state field, in bits */
  static constexpr unsigned STATE_BITS= 16;
  /** ROW_FORMAT=COMPRESSED page size (0=not compressed) */
  static constexpr unsigned SSIZE_BITS= PAGE_ZIP_SSIZE_BITS;
  /** state flag: whether the modification log is empty */
  static constexpr unsigned NONEMPTY= SSIZE_BITS;
  /** state flag: whether an access qualified the block for promotion to
  the "recently used" end of buf_pool.LRU; see
  buf_page_t::make_young_if_needed() */
  static constexpr unsigned PROMOTE= SSIZE_BITS + 1;
  /** state flag: whether the block is part of buf_pool.LRU_old */
  static constexpr unsigned OLD= SSIZE_BITS + 2;
  /** state component: n_blobs(), in the bits from N_BLOBS_SHIFT up to
  the top of state */
  static constexpr unsigned N_BLOBS_SHIFT= SSIZE_BITS + 3;

  static constexpr unsigned MAX_SSIZE= (1U << SSIZE_BITS) - 1;
  static constexpr unsigned MAX_N_BLOBS=
    (1U << (STATE_BITS - N_BLOBS_SHIFT)) - 1;

  static_assert(N_BLOBS_SHIFT == OLD + 1,
                "n_blobs must start right above OLD, so that it shares"
                " no bit with a flag and leaves no bit unused");
  static_assert(MAX_N_BLOBS >= 744,
                "n_blobs must hold the 744-column maximum on a 16 KiB page");

  /** @return the single-bit mask for the given bit position */
  template<unsigned bit> static constexpr uint16_t mask() noexcept
  { return uint16_t(1U << bit); }

  /** @return whether the given bit is set in a state snapshot */
  template<unsigned bit> static bool get_bit(uint16_t state) noexcept
  { return state & mask<bit>(); }

  template<unsigned bit> bool test_and_reset() noexcept
  {
    /* Skip the read-modify-write once the bit is already clear, mirroring
    set()'s skip-if-already-set. Only instantiated for PROMOTE, which
    set<PROMOTE>() already treats as racy/best-effort, so a concurrent
    set() flipping it back on right after this load is harmless. */
    if (!get_bit<bit>(get_state()))
      return false;

    /* Starting with GCC 7 and clang 15, on x86 and x86-64 this translates
    to the straightforward 80386 instructions LOCK BTR and SETB.

    Luckily, GCC 7 is currently the oldest in currently supported GNU/Linux
    distributions. Also, FreeBSD 14 ships with clang 16.

    MSVC has _interlockedbittestandreset() only for a 32-bit word, which
    would also cover the adjacent fix field. On MSVC, this may be a loop
    around LOCK CMPXCHG, but it runs only in the LRU eviction and flush
    sweeps. */
    return get_bit<bit>(
      state.fetch_and(uint16_t(~mask<bit>()), std::memory_order_relaxed));
  }
  template<unsigned bit> void set() noexcept
  {
    /* Skip the read-modify-write once the bit is already set, to avoid a
    LOCK OR that dirties the cache line. Two callers rely on this: PROMOTE
    is set outside buf_pool.mutex, so a concurrent test_and_reset() may
    clear it right after this load; harmless, since PROMOTE is a
    best-effort heuristic. OLD is set only under buf_pool.mutex, so no
    writer can race the load and the check is always exact. */
    if (get_bit<bit>(get_state()))
      return;

    /* On 80386, this translates into LOCK OR or LOCK BTS */
    state.fetch_or(mask<bit>(), std::memory_order_relaxed);
  }
  template<unsigned bit> void reset() noexcept
  {
    /* Skip the read-modify-write once the bit is already clear. Only
    instantiated for OLD, which set<OLD>() already establishes is written
    only under buf_pool.mutex, so this load can't race a concurrent writer
    and is always exact, not just best-effort. */
    if (!get_bit<bit>(get_state()))
      return;

    /* On 80386, this translates into LOCK AND or LOCK BTR */
    state.fetch_and(uint16_t(~mask<bit>()), std::memory_order_relaxed);
  }

public:
  uint16_t get_state() const noexcept
  { return state.load(std::memory_order_relaxed); }

  /** @return whether the modification log is empty */
  static bool is_nonempty(uint16_t state) noexcept
  { return get_bit<NONEMPTY>(state); }
  bool is_nonempty() const noexcept { return is_nonempty(get_state()); }
  void set_nonempty() noexcept { set<NONEMPTY>(); }

  /** @return whether the block is pending promotion in buf_pool.LRU */
  static bool is_promote(uint16_t state) noexcept
  { return get_bit<PROMOTE>(state); }
  bool is_promote() const noexcept { return is_promote(get_state()); }
  void set_promote() noexcept { set<PROMOTE>(); }
  /** @return the previous value of is_promote(), clearing the flag */
  bool take_promote() noexcept { return test_and_reset<PROMOTE>(); }

  /** @return whether the block is part of buf_pool.LRU_old */
  static bool old(uint16_t state) noexcept { return get_bit<OLD>(state); }
  bool old() const noexcept { return old(get_state()); }
  template<bool old> void set_old() noexcept
  { if (old) set<OLD>(); else reset<OLD>(); }

  /** number of externally stored columns; the maximum is 744 in a 16 KiB
  page */
  static uint16_t n_blobs(uint16_t state) noexcept
  { return uint16_t(state >> N_BLOBS_SHIFT); }
  uint16_t n_blobs() const noexcept { return n_blobs(get_state()); }
  /** Increment n_blobs() by n. */
  void add_n_blobs(ulint n) noexcept
  {
    ut_ad(n <= MAX_N_BLOBS);
    ut_d(uint16_t s=)
    state.fetch_add(uint16_t(n << N_BLOBS_SHIFT), std::memory_order_relaxed);
    ut_ad(n + n_blobs(s) <= MAX_N_BLOBS);
  }
  /** Decrement n_blobs() by n. */
  void sub_n_blobs(ulint n) noexcept
  {
    ut_ad(n <= MAX_N_BLOBS);
    ut_d(uint16_t s=)
    state.fetch_sub(uint16_t(n << N_BLOBS_SHIFT), std::memory_order_relaxed);
    ut_ad(n_blobs(s) >= n);
  }

  /** Set n_blobs() and clear is_nonempty() */
  inline void set_n_blobs_and_empty(ulint n_blobs) noexcept;

  /** @return ROW_FORMAT=COMPRESSED page shift size
  @retval 0 if the page is not in ROW_FORMAT=COMPRESSED */
  static uint16_t ssize(uint16_t state) noexcept
  { return state & MAX_SSIZE; }
  uint16_t ssize() const noexcept { return ssize(get_state()); }

  /** @return the ROW_FORMAT=COMPRESSED page size for a given ssize
  @retval 0 if ssize is 0 (the page is not in ROW_FORMAT=COMPRESSED) */
  static unsigned zip_size(unsigned ssize) noexcept
  { return ssize ? (UNIV_ZIP_SIZE_MIN >> 1) << ssize : 0; }

  /** @return the ROW_FORMAT=COMPRESSED page size
  @retval 0 if the page is not in ROW_FORMAT=COMPRESSED */
  unsigned get_size() const noexcept { return zip_size(ssize()); }

  /** @return the ssize for a given ROW_FORMAT=COMPRESSED page size
  @param zip_size  page size in bytes (0, or a power of two) */
  static uint16_t calc_ssize(ulint zip_size) noexcept
  {
    constexpr unsigned BASE= 512U;
    ut_ad(zip_size <= (BASE << MAX_SSIZE));
    uint16_t ssize= 0;
    if (zip_size)
      for (ssize= 1; zip_size > (BASE << ssize); ++ssize);
    ut_ad(ssize <= MAX_SSIZE);
    ut_ad(zip_size == page_zip_des_t::zip_size(ssize));
    return ssize;
  }

  /** Reinitialize an existing descriptor for a new page.
  @param fix_state the initial fix state()
  @param ssize     the ROW_FORMAT=COMPRESSED page shift size, or 0 */
  void clear(uint32_t fix_state, uint16_t ssize) noexcept
  {
    m_end= 0;
    ut_d(m_start= 0);
    ut_ad(ssize <= MAX_SSIZE);
    state.store(ssize, std::memory_order_relaxed);
    /* This synchronizes with the acquire load in buf_pool_t::page_guess(). */
    fix.store(fix_state, std::memory_order_release);
  }

  /** Reset to an all-zero, unused descriptor. Unlike the other overload,
  this also clears data and leaves fix untouched. */
  void clear() noexcept
  {
    data= nullptr;
    m_end= 0;
    ut_d(m_start= 0);
    state.store(0, std::memory_order_relaxed);
  }

  page_zip_des_t() = default;
  page_zip_des_t(const page_zip_des_t&) = default;

  /** Copy everything except the member "fix". */
  page_zip_des_t(const page_zip_des_t& old, bool) noexcept :
    data(old.data), m_end(old.m_end),
#ifdef UNIV_DEBUG
    m_start(old.m_start),
#endif
    state(old.state.load(std::memory_order_relaxed)) {}

private:
  friend buf_pool_t;
  friend buf_page_t;
  /** page descriptor state: ssize, the NONEMPTY/PROMOTE/OLD flags, and the
  n_blobs count, all packed into one atomic word of STATE_BITS bits */
  Atomic_relaxed<uint16_t> state;
  static_assert(sizeof state * 8 == STATE_BITS,
                "STATE_BITS must match the width of state");
  /** fix count and state used in buf_page_t */
  Atomic_relaxed<uint32_t> fix;
};

/** Compression statistics for a given page size */
struct page_zip_stat_t {
	/** Number of page compressions */
	ulint		compressed;
	/** Number of successful page compressions */
	ulint		compressed_ok;
	/** Number of page decompressions */
	ulint		decompressed;
	/** Duration of page compressions in microseconds */
	ib_uint64_t	compressed_usec;
	/** Duration of page decompressions in microseconds */
	ib_uint64_t	decompressed_usec;
	page_zip_stat_t() :
		/* Initialize members to 0 so that when we do
		stlmap[key].compressed++ and element with "key" does not
		exist it gets inserted with zeroed members. */
		compressed(0),
		compressed_ok(0),
		decompressed(0),
		compressed_usec(0),
		decompressed_usec(0)
	{ }
};

/** Compression statistics types */
typedef std::map<
	index_id_t,
	page_zip_stat_t,
	std::less<index_id_t>,
	ut_allocator<std::pair<const index_id_t, page_zip_stat_t> > >
	page_zip_stat_per_index_t;

/** Statistics on compression, indexed by page_zip_des_t::ssize - 1 */
extern page_zip_stat_t			page_zip_stat[PAGE_ZIP_SSIZE_MAX];
/** Statistics on compression, indexed by dict_index_t::id */
extern page_zip_stat_per_index_t	page_zip_stat_per_index;

/**********************************************************************//**
Write the "owned" flag of a record on a compressed page.  The n_owned field
must already have been written on the uncompressed page. */
void
page_zip_rec_set_owned(
/*===================*/
	buf_block_t*	block,	/*!< in/out: ROW_FORMAT=COMPRESSED page */
	const byte*	rec,	/*!< in: record on the uncompressed page */
	ulint		flag,	/*!< in: the owned flag (nonzero=TRUE) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
	MY_ATTRIBUTE((nonnull));
#endif /* !UNIV_INNOCHECKSUM */
#endif
