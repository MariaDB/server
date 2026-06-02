/**
 *
 * Copyright (C) TidesDB
 *
 * Original Author: Alex Gaetano Padula
 *
 * Licensed under the Mozilla Public License, v. 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.mozilla.org/en-US/MPL/2.0/
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "block_manager.h"

#include "xxhash.h"

#define BM_UNLIKELY(x) TDB_UNLIKELY(x)
#define BM_LIKELY(x)   TDB_LIKELY(x)

/* thread-local reusable pread buffer to avoid page faults on every block read. */
#define BM_READ_BUF_INITIAL_SIZE (128 * 1024)

/* payload bytes fetched together with the 8-byte block header in the first pread.
 * a block whose payload fits within this hint is read in a single syscall; larger
 * blocks pay one extra pread for the remainder. sized to cover the common data /
 * index / footer block without over-reading huge bloom blocks. */
#define BM_READ_HINT_BYTES (4u * 1024u)

/* a block at or below this size is read without consulting the memory budget --
 * covers every data block and the common small footer block, so the hot read
 * path is just integer compares. blocks larger than this (e.g. a multi-hundred-
 * MB bloom filter on a huge bottom-level sstable) are rare and only there do we
 * test the budget. the test itself is a relaxed atomic load, never a syscall. */
#define BM_LARGE_BLOCK_BUDGET_CHECK_THRESHOLD (256u * 1024u * 1024u)

/* memory-safety budget for a single block read, in bytes, pushed down from the
 * tidesdb layer (resolved_memory_limit-derived) via
 * block_manager_set_max_safe_block_bytes and refreshed by the reaper. 0 means
 * "no budget configured" -- the size-vs-EOF check still applies, but no
 * memory-based refusal happens (e.g. block_manager unit tests with no db). */
static _Atomic(uint64_t) bm_max_safe_block_bytes = 0;

void block_manager_set_max_safe_block_bytes(uint64_t bytes)
{
    atomic_store_explicit(&bm_max_safe_block_bytes, bytes, memory_order_relaxed);
}

static pthread_key_t bm_tls_key;
static pthread_once_t bm_tls_once = PTHREAD_ONCE_INIT;

/**
 *
 *  * * * * * * * * * *
 * FILE FORMAT        *
 *  * * * * * * * * * *
 *
 *  * * * * * * * * * *
 * HEADER             *
 *  * * * * * * * * * *
 * magic (3 bytes) 0x544442 "TDB" -- see BLOCK_MANAGER_MAGIC
 * version (1 byte) -- see BLOCK_MANAGER_VERSION
 * padding (4 bytes) reserved
 *
 *  * * * * * * * * * *
 * BLOCKS             *
 *  * * * * * * * * * *
 * block_size (4 bytes) -- size of data (uint32_t, supports up to 4GB)
 * checksum (4 bytes) -- xxHash32 of data
 * data (variable size) -- actual block data
 * footer_size (4 bytes) -- duplicate of block_size for validation
 * footer_magic (4 bytes) -- 0x42445442 "BTDB" for fast validation
 *
 *  * * * * * * * * * *
 * CONCURRENCY MODEL *
 *  * * * * * * * * * *
 * single file descriptor shared by all operations
 * pread/pwrite for lock-free reads (readers don't block readers or writers)
 * atomic offset allocation for lock-free writes
 * writers don't block writers, concurrent writes to different offsets
 * readers never block, they can read while writes happen
 *
 *  * * * * * * * * * *
 * REFERENCE COUNTING *
 *  * * * * * * * * * *
 * blocks use atomic reference counting for safe concurrent access
 * blocks start with ref_count=1 when created
 * callers must call block_manager_block_release when done
 * blocks are freed when ref_count reaches 0
 * block_manager_block_acquire/release provide thread-safe ref management
 * global block cache in tidesdb.c uses these functions for safe sharing
 */

typedef struct
{
    uint8_t *buf;
    size_t capacity;
} bm_tls_read_buf_t;

static void bm_tls_destructor(void *ptr)
{
    if (ptr)
    {
        bm_tls_read_buf_t *tls = (bm_tls_read_buf_t *)ptr;
        free(tls->buf);
        free(tls);
    }
}

static void bm_tls_init_key(void)
{
    pthread_key_create(&bm_tls_key, bm_tls_destructor);
}

static uint8_t *bm_get_read_buf(const size_t needed)
{
    pthread_once(&bm_tls_once, bm_tls_init_key);

    bm_tls_read_buf_t *tls = (bm_tls_read_buf_t *)pthread_getspecific(bm_tls_key);
    if (!tls)
    {
        tls = (bm_tls_read_buf_t *)calloc(1, sizeof(bm_tls_read_buf_t));
        if (!tls) return NULL;
        pthread_setspecific(bm_tls_key, tls);
    }

    if (BM_LIKELY(needed <= tls->capacity)) return tls->buf;

    size_t new_size = tls->capacity ? tls->capacity : BM_READ_BUF_INITIAL_SIZE;
    while (new_size < needed) new_size *= 2;

    uint8_t *new_buf = (uint8_t *)realloc(tls->buf, new_size);
    if (!new_buf) return NULL;

    tls->buf = new_buf;
    tls->capacity = new_size;
    return new_buf;
}

/**
 * odsync_available
 * check if O_DSYNC is available on the specific platform
 * @return 1 if O_DSYNC is available, 0 otherwise
 */
static inline int odsync_available(void)
{
    return O_DSYNC != 0;
}

/**
 * is_sync_full
 * is a block manager in sync full mode?
 * @param bm
 * @return 1 if sync mode is full, 0 otherwise
 */
static inline int is_sync_full(const block_manager_t *bm)
{
    return bm->sync_full_cached;
}

/**
 * compute_checksum
 * compute xxHash32 checksum
 * @param data the data to compute the checksum for
 * @param size the size of the data
 * @return the 32-bit checksum
 */
static inline uint32_t compute_checksum(const void *data, const size_t size)
{
    return XXH32(data, size, 0);
}

/**
 * verify_checksum
 * verify xxHash32 checksum
 * @param data the data to verify the checksum for
 * @param size the size of the data
 * @param expected_checksum the expected checksum
 * @return 0 if the checksum matches, -1 otherwise
 */
static inline int verify_checksum(const void *data, const size_t size,
                                  const uint32_t expected_checksum)
{
    return (compute_checksum(data, size) == expected_checksum) ? 0 : -1;
}

/**
 * is_trailing_zero
 * check whether the file region [start, end) consists entirely of zero bytes.
 * used to distinguish preallocation tail (legitimate trailing zeros that should
 * be tolerated by validation) from mid-write corruption (non-zero garbage).
 * reads in chunks and stops early on the first non-zero byte.
 * @param fd the file descriptor
 * @param start start offset (inclusive)
 * @param end   end offset (exclusive)
 * @return 1 if all bytes in [start, end) are zero, 0 if any non-zero byte found, -1 on I/O error
 */
static int is_trailing_zero(const int fd, const uint64_t start, const uint64_t end)
{
    if (start >= end) return 1;

    /* small on-stack chunk -- the loop re-reads, so a big buffer buys nothing and
     * 64 KB on the stack is risky on platforms with small thread stacks */
    enum
    {
        SCAN_CHUNK = 8 * 1024
    };
    unsigned char buf[SCAN_CHUNK];

    uint64_t pos = start;
    while (pos < end)
    {
        size_t want = SCAN_CHUNK;
        if ((uint64_t)want > end - pos) want = (size_t)(end - pos);

        const ssize_t got = pread(fd, buf, want, (off_t)pos);
        if (got <= 0) return -1;

        for (ssize_t i = 0; i < got; i++)
        {
            if (buf[i] != 0) return 0;
        }
        pos += (uint64_t)got;
    }
    return 1;
}

/**
 * maybe_extend_allocation
 * extends the on-disk preallocation when a new reservation gets within LOWWATER of
 * the current preallocated extent. multiple writers may race here; the loop is
 * lock-free and at worst causes a redundant fallocate (idempotent on overlapping
 * ranges). on platforms without preallocation support, the first failure stamps
 * preallocated_size with UINT64_MAX so the slow path is never retaken.
 * @param bm the block manager
 * @param reservation_end one past the last byte just reserved by the caller
 */
static inline void maybe_extend_allocation(block_manager_t *bm, const uint64_t reservation_end)
{
    for (;;)
    {
        const uint64_t prealloc =
            atomic_load_explicit(&bm->preallocated_size, memory_order_acquire);
        if (BM_LIKELY(reservation_end + BLOCK_MANAGER_PREALLOC_LOWWATER <= prealloc)) return;

        /* we round up to the next CHUNK boundary so successive extends stay aligned */
        const uint64_t target =
            ((reservation_end + BLOCK_MANAGER_PREALLOC_CHUNK - 1) / BLOCK_MANAGER_PREALLOC_CHUNK) *
            BLOCK_MANAGER_PREALLOC_CHUNK;
        if (target <= prealloc) return; /* another writer already extended past us */

        if (tdb_preallocate_extent(bm->fd, (off_t)prealloc, (off_t)(target - prealloc)) != 0)
        {
            /** unsupported on this fs/platform, disable further attempts.
             *  subsequent pwrites simply take the (slower) extending-write path. */
            atomic_store_explicit(&bm->preallocated_size, UINT64_MAX, memory_order_release);
            return;
        }

        uint64_t expected = prealloc;
        if (atomic_compare_exchange_strong_explicit(&bm->preallocated_size, &expected, target,
                                                    memory_order_release, memory_order_acquire))
        {
            return;
        }
        /* lost the CAS race; another writer also extended -- reload and re-check */
    }
}

/**
 * write_header
 * write file header using pwrite
 * @param fd the file descriptor to write to
 * @return 0 if successful, -1 otherwise
 */
static int write_header(const int fd)
{
    unsigned char header[BLOCK_MANAGER_HEADER_SIZE];
    const uint32_t padding = 0;

    /* header format
     * [3-byte magic][1-byte version][4-byte padding] = 8 bytes */
    encode_uint32_le_compat(header, BLOCK_MANAGER_MAGIC);
    header[BLOCK_MANAGER_MAGIC_SIZE] = BLOCK_MANAGER_VERSION;
    encode_uint32_le_compat(header + BLOCK_MANAGER_MAGIC_SIZE + BLOCK_MANAGER_VERSION_SIZE,
                            padding);

    const ssize_t written = pwrite(fd, header, BLOCK_MANAGER_HEADER_SIZE, 0);
    return (written == BLOCK_MANAGER_HEADER_SIZE) ? 0 : -1;
}

/**
 * read_header
 * read and validate file header using pread
 * @param fd the file descriptor to read from
 * @return 0 if successful, -1 otherwise
 */
static int read_header(const int fd)
{
    unsigned char header[BLOCK_MANAGER_HEADER_SIZE];

    const ssize_t nread = pread(fd, header, BLOCK_MANAGER_HEADER_SIZE, 0);
    if (nread != BLOCK_MANAGER_HEADER_SIZE) return -1;

    /* we decode magic using little-endian conversion for cross-platform compatibility */
    uint32_t magic = decode_uint32_le_compat(header);
    magic &= BLOCK_MANAGER_MAGIC_MASK;

    if (magic != BLOCK_MANAGER_MAGIC) return -1;

    uint8_t version;
    memcpy(&version, header + BLOCK_MANAGER_MAGIC_SIZE, BLOCK_MANAGER_VERSION_SIZE);
    if (version != BLOCK_MANAGER_VERSION) return -1;

    return 0;
}

/**
 * get_file_size
 * get file size using fstat
 * @param fd the file descriptor to get the size of
 * @param size the size to store the result in
 * @return 0 if successful, -1 otherwise
 */
static int get_file_size(const int fd, uint64_t *size)
{
    struct STAT_STRUCT st;
    if (FSTAT_FUNC(fd, &st) != 0) return -1;
    *size = (uint64_t)st.st_size;
    return 0;
}

/**
 * reopen_fd
 * closes and reopens the block manager file descriptor with the same flags.
 * NOT safe against concurrent readers: a reader that already captured bm->fd will
 * pread on a closed (possibly recycled) descriptor. callers (truncate, permissive
 * validation) must hold the bm exclusively / quiesce readers first.
 * @param bm the block manager
 * @return 0 if successful, -1 if not
 */
static int reopen_fd(block_manager_t *bm)
{
    close(bm->fd);

    int flags = O_RDWR | O_CREAT;
    if (is_sync_full(bm) && odsync_available())
    {
        flags |= O_DSYNC;
    }

    bm->fd = open(bm->file_path, flags, BLOCK_MANAGER_FILE_MODE);
    if (bm->fd == -1) return -1;

    return 0;
}

/**
 * truncate_to_header
 * truncates a block manager file to just the header and syncs
 * @param bm the block manager
 * @return 0 if successful, -1 if not
 */
static int truncate_to_header(block_manager_t *bm)
{
    if (ftruncate(bm->fd, (off_t)BLOCK_MANAGER_HEADER_SIZE) == -1) return -1;

    /* ftruncate is not covered by O_DSYNC, we always sync truncation */
    if (is_sync_full(bm))
    {
        fdatasync(bm->fd);
    }

    atomic_store(&bm->current_file_size, BLOCK_MANAGER_HEADER_SIZE);
    /** preallocation is invalidated by ftruncate; we reset to current size so the next
     *  write triggers a fresh extend */
    atomic_store(&bm->preallocated_size, BLOCK_MANAGER_HEADER_SIZE);
    return 0;
}

/**
 * block_manager_open_internal
 * opens a block manager (no cache)
 * @param bm the block manager to open
 * @param file_path the path of the file
 * @param sync_mode the sync mode (TDB_SYNC_NONE, TDB_SYNC_FULL)
 * @return 0 if successful, -1 if not
 */
static int block_manager_open_internal(block_manager_t **bm, const char *file_path,
                                       const block_manager_sync_mode_t sync_mode)
{
    block_manager_t *new_bm = malloc(sizeof(block_manager_t));
    if (!new_bm)
    {
        *bm = NULL;
        return -1;
    }

    /* we initialize atomic variable to prevent reading uninitialized memory */
    atomic_init(&new_bm->current_file_size, 0);
    atomic_init(&new_bm->preallocated_size, 0);
    atomic_init(&new_bm->group_durable_size, 0);
    atomic_init(&new_bm->group_sync_active, 0);

    new_bm->sync_mode = sync_mode;
    new_bm->sync_full_cached = (sync_mode == BLOCK_MANAGER_SYNC_FULL);

    const int file_exists = access(file_path, F_OK) == 0;

    int flags = O_RDWR | O_CREAT;

    /* we use O_DSYNC for synchronous data writes in SYNC_FULL mode
     * this ensures each pwrite is durable before returning, eliminating
     * the need for per-write fdatasync() calls on platforms that support it.
     * this is also faster, less syscalls, for example
     */
    if (is_sync_full(new_bm) && odsync_available())
    {
        flags |= O_DSYNC;
    }

    const mode_t mode = BLOCK_MANAGER_FILE_MODE;

    new_bm->fd = open(file_path, flags, mode);
    if (new_bm->fd == -1)
    {
        /* preserve the open() errno across free() so the caller can report the real cause
         * (EMFILE/ENFILE = fd exhaustion, ENOSPC = disk full, EACCES, ...) */
        const int open_errno = errno;
        free(new_bm);
        *bm = NULL;
        errno = open_errno;
        return -1;
    }

    strncpy(new_bm->file_path, file_path, MAX_FILE_PATH_LENGTH - 1);
    new_bm->file_path[MAX_FILE_PATH_LENGTH - 1] = '\0';

    if (file_exists)
    {
        if (read_header(new_bm->fd) != 0)
        {
            const int hdr_errno = errno;
            close(new_bm->fd);
            free(new_bm);
            *bm = NULL;
            errno = hdr_errno;
            return -1;
        }
    }
    else
    {
        if (write_header(new_bm->fd) != 0)
        {
            const int hdr_errno = errno;
            close(new_bm->fd);
            free(new_bm);
            *bm = NULL;
            errno = hdr_errno;
            return -1;
        }
        /* if O_DSYNC is available, pwrite already synced the header
         * otherwise fall back to explicit fdatasync */
        if (is_sync_full(new_bm) && !odsync_available())
        {
            if (fdatasync(new_bm->fd) != 0)
            {
                const int sync_errno = errno;
                close(new_bm->fd);
                free(new_bm);
                *bm = NULL;
                errno = sync_errno;
                return -1;
            }
        }
    }

    /* we set current_file_size if not already set by validation */
    if (atomic_load(&new_bm->current_file_size) == 0)
    {
        uint64_t file_size = 0;
        if (get_file_size(new_bm->fd, &file_size) == 0)
        {
            atomic_store(&new_bm->current_file_size, file_size);
        }
        else
        {
            /* if we can't get size, use lseek to get current position (end of file) */
            const off_t pos = lseek(new_bm->fd, 0, SEEK_END);
            atomic_store(&new_bm->current_file_size, (pos >= 0) ? (uint64_t)pos : 0);
        }
    }

    /* preallocated extent starts at the current file size; first write will extend it */
    atomic_store(&new_bm->preallocated_size, atomic_load(&new_bm->current_file_size));

    *bm = new_bm;
    return 0;
}

int block_manager_close(block_manager_t *bm)
{
    if (!bm) return -1;

    /* preallocation advances logical EOF past actual data; trim back so next-open
     * validation sees the real tail block instead of trailing zeros. crash recovery
     * still has to tolerate trailing zeros (size_field == 0 marks the boundary). */
    const uint64_t valid_size = atomic_load(&bm->current_file_size);
    const uint64_t prealloc = atomic_load(&bm->preallocated_size);
    if (prealloc != UINT64_MAX && prealloc > valid_size && bm->fd >= 0)
    {
        /* best-effort -- if it fails, next-open validate_last_block tolerates the
         * trailing-zero preallocation tail. (void) cast doesn't suppress glibc's
         * warn_unused_result, hence the explicit if. */
        if (ftruncate(bm->fd, (off_t)valid_size) != 0)
        {
            /* swallow */
        }
    }

    /* final sync on close -- we really only needed if O_DSYNC wasnt used
     * with O_DSYNC, all writes are already durable */
    if (is_sync_full(bm) && !odsync_available())
    {
        (void)fdatasync(bm->fd);
    }

    int close_result = 0;
    if (bm->fd >= 0 && close(bm->fd) != 0)
    {
        close_result = -1;
    }

    free(bm);

    return close_result;
}

block_manager_block_t *block_manager_block_create(const uint64_t size, const void *data)
{
    if (size > UINT32_MAX)
    {
        return NULL;
    }

    block_manager_block_t *block = malloc(sizeof(block_manager_block_t));
    if (!block) return NULL;

    block->size = size;
    atomic_init(&block->ref_count, 1);
    block->inline_data = 0;

    block->data = malloc(size);
    if (!block->data)
    {
        free(block);
        return NULL;
    }

    /* we only copy if size > 0 and data is not NULL */
    if (size > 0 && data != NULL)
    {
        memcpy(block->data, data, size);
    }
    return block;
}

block_manager_block_t *block_manager_block_create_from_buffer(const uint64_t size, void *data)
{
    if (size > UINT32_MAX)
    {
        return NULL;
    }

    block_manager_block_t *block = malloc(sizeof(block_manager_block_t));
    if (!block) return NULL;

    block->size = size;
    block->data = data;
    atomic_init(&block->ref_count, 1);
    block->inline_data = 0;
    return block;
}

/**
 * bm_append_block
 * append one framed block [size][checksum][data][size][magic] at the atomically
 * reserved tail offset via a single pwritev. shared by block_write and write_raw
 * so the on-disk encoding lives in one place. data must be non-NULL and size
 * non-zero -- the caller validates (a zero size_field reads back as EOF).
 * @return the offset written at, or -1 on failure
 */
static int64_t bm_append_block(block_manager_t *bm, const void *data, const uint32_t size)
{
    const size_t total_size =
        BLOCK_MANAGER_BLOCK_HEADER_SIZE + (size_t)size + BLOCK_MANAGER_FOOTER_SIZE;
    const uint32_t checksum = compute_checksum(data, size);

    /* atomically reserve space, then extend preallocation so the pwrite stays in-place */
    const int64_t offset = (int64_t)atomic_fetch_add(&bm->current_file_size, total_size);
    (void)maybe_extend_allocation(bm, (uint64_t)offset + total_size);

    unsigned char header[BLOCK_MANAGER_BLOCK_HEADER_SIZE];
    encode_uint32_le_compat(header, size);
    encode_uint32_le_compat(header + BLOCK_MANAGER_SIZE_FIELD_SIZE, checksum);

    unsigned char footer[BLOCK_MANAGER_FOOTER_SIZE];
    encode_uint32_le_compat(footer, size);
    encode_uint32_le_compat(footer + BLOCK_MANAGER_CHECKSUM_LENGTH, BLOCK_MANAGER_FOOTER_MAGIC);

    /* header + data + footer in a single pwritev -- zero copy from data */
    struct iovec iov[BLOCK_MANAGER_IOVECS_PER_BLOCK];
    iov[0].iov_base = header;
    iov[0].iov_len = BLOCK_MANAGER_BLOCK_HEADER_SIZE;
    iov[1].iov_base = (void *)data;
    iov[1].iov_len = size;
    iov[2].iov_base = footer;
    iov[2].iov_len = BLOCK_MANAGER_FOOTER_SIZE;

    if (BM_UNLIKELY(tdb_pwritev_safe(bm->fd, iov, BLOCK_MANAGER_IOVECS_PER_BLOCK, (off_t)offset) !=
                    (ssize_t)total_size))
        return -1;

    /* with O_DSYNC the pwrite already synced; otherwise fall back to fdatasync */
    if (is_sync_full(bm) && !odsync_available())
    {
        if (fdatasync(bm->fd) != 0) return -1;
    }

    return offset;
}

int64_t block_manager_block_write(block_manager_t *bm, block_manager_block_t *block)
{
    if (BM_UNLIKELY(!bm || !block)) return -1;

    /* block size is stored as uint32_t, thus enforced 4GB limit */
    if (BM_UNLIKELY(block->size > UINT32_MAX)) return -1;

    /* a zero-size block encodes size_field == 0, which every reader treats as EOF;
     * reject it so it can never truncate iteration (matches write_raw) */
    if (BM_UNLIKELY(block->size == 0)) return -1;

    /* guard size_t overflow of the framed total on 32-bit platforms */
    if (block->size > SIZE_MAX - BLOCK_MANAGER_BLOCK_HEADER_SIZE - BLOCK_MANAGER_FOOTER_SIZE)
        return -1;

    return bm_append_block(bm, block->data, (uint32_t)block->size);
}

int64_t block_manager_write_raw(block_manager_t *bm, const void *data, const uint32_t size)
{
    if (BM_UNLIKELY(!bm || !data || size == 0)) return -1;
    return bm_append_block(bm, data, size);
}

/* maximum iovecs per pwritev call, POSIX minimum is 16, Linux uses 1024 */
#ifndef BM_IOV_MAX
#define BM_IOV_MAX 1024
#endif

int block_manager_block_write_batch(block_manager_t *bm, block_manager_block_t **blocks,
                                    const size_t count, int64_t *offsets)
{
    if (BM_UNLIKELY(!bm || !blocks || count == 0 || !offsets)) return -1;

    /* we calculate total size needed and count valid blocks */
    size_t total_batch_size = 0;
    size_t valid_count = 0;
    for (size_t i = 0; i < count; i++)
    {
        if (!blocks[i])
        {
            offsets[i] = -1;
            continue;
        }
        if (blocks[i]->size > UINT32_MAX) return -1;

        total_batch_size +=
            BLOCK_MANAGER_BLOCK_HEADER_SIZE + blocks[i]->size + BLOCK_MANAGER_FOOTER_SIZE;
        valid_count++;
    }

    if (total_batch_size == 0) return 0;

    /* we atomically allocate space for all blocks at once */
    const int64_t base_offset = (int64_t)atomic_fetch_add(&bm->current_file_size, total_batch_size);

    (void)maybe_extend_allocation(bm, (uint64_t)base_offset + total_batch_size);

    const size_t meta_size =
        valid_count * (BLOCK_MANAGER_BLOCK_HEADER_SIZE + BLOCK_MANAGER_FOOTER_SIZE);
    const size_t iov_count = valid_count * BLOCK_MANAGER_IOVECS_PER_BLOCK;
    unsigned char *alloc = malloc(meta_size + iov_count * sizeof(struct iovec));
    if (!alloc) return -1;

    unsigned char *meta_buf = alloc;
    struct iovec *iov = (struct iovec *)(alloc + meta_size);

    /* we build iovecs, header and footer go into meta_buf, data points directly to block->data */
    int64_t current_offset = base_offset;
    size_t iov_idx = 0;
    size_t meta_idx = 0;

    for (size_t i = 0; i < count; i++)
    {
        if (!blocks[i]) continue;

        block_manager_block_t *block = blocks[i];
        const size_t block_total =
            BLOCK_MANAGER_BLOCK_HEADER_SIZE + block->size + BLOCK_MANAGER_FOOTER_SIZE;

        offsets[i] = current_offset;

        /* we encode header and footer into contiguous metadata buffer */
        unsigned char *hdr =
            meta_buf + meta_idx * (BLOCK_MANAGER_BLOCK_HEADER_SIZE + BLOCK_MANAGER_FOOTER_SIZE);
        unsigned char *ftr = hdr + BLOCK_MANAGER_BLOCK_HEADER_SIZE;

        const uint32_t checksum = compute_checksum(block->data, block->size);
        encode_uint32_le_compat(hdr, (uint32_t)block->size);
        encode_uint32_le_compat(hdr + BLOCK_MANAGER_SIZE_FIELD_SIZE, checksum);
        encode_uint32_le_compat(ftr, (uint32_t)block->size);
        encode_uint32_le_compat(ftr + BLOCK_MANAGER_CHECKSUM_LENGTH, BLOCK_MANAGER_FOOTER_MAGIC);

        iov[iov_idx].iov_base = hdr;
        iov[iov_idx].iov_len = BLOCK_MANAGER_BLOCK_HEADER_SIZE;
        iov[iov_idx + 1].iov_base = block->data;
        iov[iov_idx + 1].iov_len = block->size;
        iov[iov_idx + 2].iov_base = ftr;
        iov[iov_idx + 2].iov_len = BLOCK_MANAGER_FOOTER_SIZE;

        iov_idx += BLOCK_MANAGER_IOVECS_PER_BLOCK;
        meta_idx++;
        current_offset += (int64_t)block_total;
    }

    /* we write in BM_IOV_MAX-sized chunks for batches that exceed the iovec limit */
    size_t iov_done = 0;
    off_t write_offset = (off_t)base_offset;

    while (iov_done < iov_idx)
    {
        int chunk = (int)(iov_idx - iov_done);
        if (chunk > BM_IOV_MAX) chunk = BM_IOV_MAX;

        ssize_t expected = 0;
        for (int j = 0; j < chunk; j++) expected += (ssize_t)iov[iov_done + j].iov_len;

        const ssize_t written = tdb_pwritev_safe(bm->fd, iov + iov_done, chunk, write_offset);
        if (written != expected)
        {
            free(alloc);
            for (size_t i = 0; i < count; i++) offsets[i] = -1;
            return -1;
        }

        write_offset += written;
        iov_done += (size_t)chunk;
    }

    free(alloc);

    /* we sync if needed */
    if (is_sync_full(bm) && !odsync_available())
    {
        if (fdatasync(bm->fd) != 0)
        {
            return -1;
        }
    }

    return (int)valid_count;
}

int block_manager_write_at(block_manager_t *bm, const int64_t offset, const uint8_t *data,
                           const size_t size)
{
    if (!bm || !data || size == 0 || offset < 0) return -1;

    /* this only patches existing data -- a write past the tracked extent would
     * grow the file without advancing current_file_size, desyncing the two */
    if ((uint64_t)offset + size > atomic_load(&bm->current_file_size)) return -1;

    const ssize_t written = pwrite(bm->fd, data, size, offset);
    if (written != (ssize_t)size)
    {
        return -1;
    }

    if (is_sync_full(bm) && !odsync_available())
    {
        if (fdatasync(bm->fd) != 0)
        {
            return -1;
        }
    }

    return 0;
}

int block_manager_update_checksum(block_manager_t *bm, const int64_t block_offset)
{
    if (!bm || block_offset < 0) return -1;

    /* we read block size from header */
    unsigned char size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
    if (pread(bm->fd, size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE, block_offset) !=
        BLOCK_MANAGER_SIZE_FIELD_SIZE)
    {
        return -1;
    }

    const uint32_t block_size = decode_uint32_le_compat(size_buf);
    if (block_size == 0) return -1;

    /* we use thread-local buffer to avoid page faults from fresh malloc pages */
    uint8_t *data = bm_get_read_buf(block_size);
    if (!data) return -1;

    const off_t data_offset = block_offset + BLOCK_MANAGER_BLOCK_HEADER_SIZE;
    if (pread(bm->fd, data, block_size, data_offset) != (ssize_t)block_size)
    {
        return -1;
    }

    const uint32_t new_checksum = compute_checksum(data, block_size);

    unsigned char checksum_buf[BLOCK_MANAGER_CHECKSUM_LENGTH];
    encode_uint32_le_compat(checksum_buf, new_checksum);

    const off_t checksum_offset = block_offset + BLOCK_MANAGER_SIZE_FIELD_SIZE;
    if (pwrite(bm->fd, checksum_buf, BLOCK_MANAGER_CHECKSUM_LENGTH, checksum_offset) !=
        BLOCK_MANAGER_CHECKSUM_LENGTH)
    {
        return -1;
    }

    if (is_sync_full(bm) && !odsync_available())
    {
        if (fdatasync(bm->fd) != 0)
        {
            return -1;
        }
    }

    return 0;
}

void block_manager_block_free(block_manager_block_t *block)
{
    if (!block) return;

    if (!block->inline_data && block->data) free(block->data);
    free(block);
}

int block_manager_block_acquire(block_manager_block_t *block)
{
    if (!block) return 0;

    uint32_t old_ref = atomic_load_explicit(&block->ref_count, memory_order_relaxed);
    do
    {
        if (old_ref == 0) return 0; /* block is being freed */
    } while (!atomic_compare_exchange_weak_explicit(&block->ref_count, &old_ref, old_ref + 1,
                                                    memory_order_acquire, memory_order_relaxed));
    return 1;
}

void block_manager_block_release(block_manager_block_t *block)
{
    if (!block) return;

    const uint32_t old_ref = atomic_fetch_sub_explicit(&block->ref_count, 1, memory_order_release);
    if (old_ref == 1)
    {
        /* we were the last reference, free the block */
        atomic_thread_fence(memory_order_acquire);
        block_manager_block_free(block);
    }
}

int block_manager_cursor_init_stack(block_manager_cursor_t *cursor, block_manager_t *bm)
{
    if (!cursor || !bm) return -1;

    cursor->bm = bm;

    /* we initialize to position before first block */
    cursor->current_pos = BLOCK_MANAGER_HEADER_SIZE;
    cursor->current_block_size = 0;
    cursor->block_index = -1; /* -1 means before first block */
    cursor->block_size_valid = 0;

    /* we position at first block so cursor_read works immediately */
    block_manager_cursor_goto_first(cursor);

    return 0;
}

int block_manager_cursor_init(block_manager_cursor_t **cursor, block_manager_t *bm)
{
    if (!bm) return -1;

    (*cursor) = malloc(sizeof(block_manager_cursor_t));
    if (!(*cursor)) return -1;

    const int rc = block_manager_cursor_init_stack(*cursor, bm);
    if (rc == 0)
    {
        /* heap-allocated cursors are used for sequential iteration
         * we hint to OS for read-ahead optimization */
        set_file_sequential_hint(bm->fd);
    }
    return rc;
}

int block_manager_cursor_next(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    uint32_t block_size;

    /* we use cached block size if valid, otherwise read from disk */
    if (cursor->block_size_valid && cursor->current_block_size > 0)
    {
        block_size = (uint32_t)cursor->current_block_size;
    }
    else
    {
        unsigned char size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
        const ssize_t nread = pread(cursor->bm->fd, size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE,
                                    (off_t)cursor->current_pos);
        if (nread != BLOCK_MANAGER_SIZE_FIELD_SIZE)
        {
            if (nread == 0) return 1; /* EOF */
            return -1;
        }
        block_size = decode_uint32_le_compat(size_buf);
        if (block_size == 0) return -1; /* invalid block */
    }

    /* next block starts after, [size][checksum][data][footer_size][footer_magic] */
    cursor->current_pos +=
        BLOCK_MANAGER_BLOCK_HEADER_SIZE + (uint64_t)block_size + BLOCK_MANAGER_FOOTER_SIZE;
    cursor->current_block_size = 0;
    cursor->block_size_valid = 0; /* we invalidate cache after moving */
    cursor->block_index++;

    return 0;
}

int block_manager_cursor_has_next(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    const uint64_t file_size = atomic_load(&cursor->bm->current_file_size);
    if (cursor->current_pos >= file_size) return 0; /* at or past EOF */

    /** we use cached block size if valid */
    if (cursor->block_size_valid && cursor->current_block_size > 0)
    {
        return 1;
    }

    /* we read current block size to check if current block is valid */
    unsigned char size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
    const ssize_t nread =
        pread(cursor->bm->fd, size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE, (off_t)cursor->current_pos);
    if (nread != BLOCK_MANAGER_SIZE_FIELD_SIZE)
    {
        if (nread == 0) return 0; /* EOF */
        return -1;
    }

    const uint32_t block_size = decode_uint32_le_compat(size_buf);
    if (block_size == 0) return -1; /* invalid block */

    /* we cache the block size for subsequent cursor_next call */
    cursor->current_block_size = block_size;
    cursor->block_size_valid = 1;

    /* has_next returns 1 if cursor_next would succeed (can read current block and move forward) */
    return 1;
}

int block_manager_cursor_has_prev(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    return (cursor->current_pos > BLOCK_MANAGER_HEADER_SIZE) ? 1 : 0;
}

int block_manager_cursor_skip_corrupt(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    /* we read the size field from the current position */
    unsigned char size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
    if (pread(cursor->bm->fd, size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE,
              (off_t)cursor->current_pos) != BLOCK_MANAGER_SIZE_FIELD_SIZE)
    {
        return -1;
    }

    const uint32_t block_size = decode_uint32_le_compat(size_buf);
    if (block_size == 0) return -1; /* zero-filled hole extent unknown, cannot advance */

    /* read footer magic to distinguish partial write from genuine corruption.
     * footer layout [footer_size(4)][footer_magic(4)]; footer_magic sits at
     * (current_pos + BLOCK_HEADER_SIZE + block_size + SIZE_FIELD_SIZE) */
    const off_t footer_magic_offset = (off_t)cursor->current_pos + BLOCK_MANAGER_BLOCK_HEADER_SIZE +
                                      (off_t)block_size + BLOCK_MANAGER_SIZE_FIELD_SIZE;
    unsigned char magic_buf[BLOCK_MANAGER_CHECKSUM_LENGTH];
    const ssize_t nread =
        pread(cursor->bm->fd, magic_buf, BLOCK_MANAGER_CHECKSUM_LENGTH, footer_magic_offset);
    if (nread != BLOCK_MANAGER_CHECKSUM_LENGTH)
    {
        /* footer not present so file truncated mid-block; treat as partial write */
        cursor->current_pos +=
            BLOCK_MANAGER_BLOCK_HEADER_SIZE + (uint64_t)block_size + BLOCK_MANAGER_FOOTER_SIZE;
        cursor->current_block_size = 0;
        cursor->block_size_valid = 0;
        cursor->block_index++;
        return 0;
    }

    const uint32_t footer_magic = decode_uint32_le_compat(magic_buf);
    if (footer_magic == BLOCK_MANAGER_FOOTER_MAGIC)
    {
        return -1;
    }

    cursor->current_pos +=
        BLOCK_MANAGER_BLOCK_HEADER_SIZE + (uint64_t)block_size + BLOCK_MANAGER_FOOTER_SIZE;
    cursor->current_block_size = 0;
    cursor->block_size_valid = 0;
    cursor->block_index++;
    return 0;
}

/**
 * bm_read_block_tls
 * reads a full block (header + payload) at `offset` into the thread-local buffer.
 * the first pread grabs the header plus BM_READ_HINT_BYTES of payload, so a block
 * within the hint costs a single syscall; a larger block pays one more pread for
 * the remainder. the checksum is verified before returning.
 * @param fd the file descriptor
 * @param offset the file offset of the block (start of header)
 * @param extent_limit if non-zero, reject a block whose frame extends past this
 *                     byte offset (guards against garbage sizes); 0 skips the check
 * @param check_budget if non-zero, refuse a payload larger than the memory budget
 * @param out_size set to the payload size on success
 * @return pointer to the verified payload inside the TLS buffer, or NULL on failure
 */
static uint8_t *bm_read_block_tls(const int fd, const uint64_t offset, const uint64_t extent_limit,
                                  const int check_budget, uint32_t *out_size)
{
    /* first pread -- header + a hint of payload in one syscall */
    uint8_t *buf = bm_get_read_buf(BLOCK_MANAGER_BLOCK_HEADER_SIZE + BM_READ_HINT_BYTES);
    if (BM_UNLIKELY(!buf)) return NULL;

    const ssize_t got =
        pread(fd, buf, BLOCK_MANAGER_BLOCK_HEADER_SIZE + BM_READ_HINT_BYTES, (off_t)offset);
    if (BM_UNLIKELY(got < (ssize_t)BLOCK_MANAGER_BLOCK_HEADER_SIZE)) return NULL;

    const uint32_t size = decode_uint32_le_compat(buf);
    if (BM_UNLIKELY(size == 0)) return NULL;
    const uint32_t checksum = decode_uint32_le_compat(buf + BLOCK_MANAGER_SIZE_FIELD_SIZE);

    /* a block claiming to extend past the data extent is garbage (off-boundary
     * read, torn write, corruption) -- reject before reading/allocating trash */
    if (extent_limit)
    {
        const uint64_t frame_end =
            offset + BLOCK_MANAGER_BLOCK_HEADER_SIZE + (uint64_t)size + BLOCK_MANAGER_FOOTER_SIZE;
        if (BM_UNLIKELY(frame_end > extent_limit)) return NULL;
    }

    /* only large blocks consult the budget (relaxed atomic load, no syscall); a
     * block over budget is skipped so the caller degrades instead of OOMing */
    if (check_budget && BM_UNLIKELY(size > BM_LARGE_BLOCK_BUDGET_CHECK_THRESHOLD))
    {
        const uint64_t budget =
            atomic_load_explicit(&bm_max_safe_block_bytes, memory_order_relaxed);
        if (budget > 0 && (uint64_t)size > budget) return NULL;
    }

    /* payload bytes already in buf (the first read may also have pulled the footer
     * and into the next block -- clamp to the real payload length) */
    uint32_t have = (uint32_t)got - BLOCK_MANAGER_BLOCK_HEADER_SIZE;
    if (have > size) have = size;

    if (size > have)
    {
        /* grow the TLS buffer if needed -- realloc preserves the bytes already read */
        buf = bm_get_read_buf(BLOCK_MANAGER_BLOCK_HEADER_SIZE + size);
        if (BM_UNLIKELY(!buf)) return NULL;

        const off_t rem_offset = (off_t)offset + BLOCK_MANAGER_BLOCK_HEADER_SIZE + have;
        if (BM_UNLIKELY(pread(fd, buf + BLOCK_MANAGER_BLOCK_HEADER_SIZE + have, size - have,
                              rem_offset) != (ssize_t)(size - have)))
            return NULL;
    }

    uint8_t *payload = buf + BLOCK_MANAGER_BLOCK_HEADER_SIZE;
    if (BM_UNLIKELY(verify_checksum(payload, size, checksum) != 0)) return NULL;

    *out_size = size;
    return payload;
}

/**
 * block_manager_read_block_at_offset
 * reads a block at a specific offset
 * @param bm the block manager
 * @param offset the offset to read from
 * @return the block if successful, NULL otherwise
 */
static block_manager_block_t *block_manager_read_block_at_offset(block_manager_t *bm,
                                                                 const uint64_t offset)
{
    if (BM_UNLIKELY(!bm)) return NULL;

    /* enforce the data extent so a garbage size can't drive a read/alloc past EOF;
     * file_size 0 means "size not yet known" -- skip the check as before */
    const uint64_t file_size = atomic_load_explicit(&bm->current_file_size, memory_order_acquire);

    uint32_t block_size = 0;
    uint8_t *payload = bm_read_block_tls(bm->fd, offset, file_size, 1, &block_size);
    if (BM_UNLIKELY(!payload)) return NULL;

    block_manager_block_t *block = malloc(sizeof(block_manager_block_t) + block_size);
    if (!block) return NULL;

    block->size = block_size;
    block->data = (uint8_t *)(block + 1);
    block->inline_data = 1;
    atomic_init(&block->ref_count, 1);

    memcpy(block->data, payload, block_size);
    return block;
}

block_manager_block_t *block_manager_cursor_read(block_manager_cursor_t *cursor)
{
    if (!cursor) return NULL;

    block_manager_block_t *block =
        block_manager_read_block_at_offset(cursor->bm, cursor->current_pos);
    if (block)
    {
        /* we cache block size so cursor_next skips the pread for size header */
        cursor->current_block_size = block->size;
        cursor->block_size_valid = 1;
    }
    return block;
}

block_manager_block_t *block_manager_cursor_read_partial(block_manager_cursor_t *cursor,
                                                         const size_t max_bytes)
{
    if (!cursor) return NULL;
    if (max_bytes == 0) return block_manager_cursor_read(cursor);

    block_manager_t *bm = cursor->bm;
    const uint64_t offset = cursor->current_pos;

    /* we use cached block size to avoid redundant pread syscall */
    uint32_t block_size;
    if (cursor->block_size_valid && cursor->current_block_size > 0)
    {
        block_size = (uint32_t)cursor->current_block_size;
    }
    else
    {
        unsigned char size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
        if (pread(bm->fd, size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE, (off_t)offset) !=
            BLOCK_MANAGER_SIZE_FIELD_SIZE)
            return NULL;
        block_size = decode_uint32_le_compat(size_buf);
        if (block_size == 0) return NULL;
    }

    /* if block is smaller than max_bytes, we read full block */
    if (block_size <= max_bytes)
    {
        return block_manager_read_block_at_offset(bm, offset);
    }

    block_manager_block_t *block = malloc(sizeof(block_manager_block_t));
    if (!block) return NULL;

    block->size = max_bytes;
    atomic_init(&block->ref_count, 1);
    block->inline_data = 0;
    block->data = malloc(max_bytes);
    if (!block->data)
    {
        free(block);
        return NULL;
    }

    /* we read only first max_bytes of data */
    const off_t data_pos = (off_t)offset + (off_t)BLOCK_MANAGER_BLOCK_HEADER_SIZE;
    if (pread(bm->fd, block->data, max_bytes, data_pos) != (ssize_t)max_bytes)
    {
        free(block->data);
        free(block);
        return NULL;
    }

    /* we don't verify checksum for partial reads since we don't have full data */
    return block;
}

block_manager_block_t *block_manager_cursor_read_and_advance(block_manager_cursor_t *cursor)
{
    if (!cursor) return NULL;

    block_manager_block_t *block =
        block_manager_read_block_at_offset(cursor->bm, cursor->current_pos);
    if (!block) return NULL;

    /* we advance cursor using the block size we just read, avoiding redundant pread */
    cursor->current_pos +=
        BLOCK_MANAGER_BLOCK_HEADER_SIZE + block->size + BLOCK_MANAGER_FOOTER_SIZE;
    cursor->current_block_size = 0;
    cursor->block_size_valid = 0; /* invalidate cache -- we moved to a new position */
    cursor->block_index++;

    return block;
}

void block_manager_cursor_free(block_manager_cursor_t *cursor)
{
    if (cursor)
    {
        free(cursor);
    }
}

int block_manager_cursor_prev(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    /* we are already at first block position, we can't go back */
    if (cursor->current_pos <= BLOCK_MANAGER_HEADER_SIZE) return -1;

    const uint64_t prev_footer_end = cursor->current_pos;
    if (prev_footer_end <
        BLOCK_MANAGER_HEADER_SIZE + BLOCK_MANAGER_BLOCK_HEADER_SIZE + BLOCK_MANAGER_FOOTER_SIZE)
    {
        return -1; /* not enough space for a valid previous block */
    }

    unsigned char footer_buf[BLOCK_MANAGER_FOOTER_SIZE];
    const off_t footer_offset = (off_t)(prev_footer_end - BLOCK_MANAGER_FOOTER_SIZE);
    if (pread(cursor->bm->fd, footer_buf, BLOCK_MANAGER_FOOTER_SIZE, footer_offset) !=
        BLOCK_MANAGER_FOOTER_SIZE)
    {
        return -1;
    }

    const uint32_t prev_block_size = decode_uint32_le_compat(footer_buf);
    const uint32_t footer_magic =
        decode_uint32_le_compat(footer_buf + BLOCK_MANAGER_CHECKSUM_LENGTH);

    /* we validate footer magic */
    if (footer_magic != BLOCK_MANAGER_FOOTER_MAGIC || prev_block_size == 0)
    {
        return -1;
    }

    /* we calculate start of previous block */
    const uint64_t prev_total_size =
        BLOCK_MANAGER_BLOCK_HEADER_SIZE + prev_block_size + BLOCK_MANAGER_FOOTER_SIZE;
    if (cursor->current_pos < prev_total_size)
    {
        return -1; /* invalid -- would underflow */
    }

    const uint64_t prev_block_start = cursor->current_pos - prev_total_size;
    if (prev_block_start < BLOCK_MANAGER_HEADER_SIZE)
    {
        return -1; /* invalid -- before file header */
    }

    cursor->current_pos = prev_block_start;
    cursor->current_block_size = prev_block_size;
    cursor->block_size_valid = 1; /* we know the size from footer */
    cursor->block_index--;

    return 0;
}

int block_manager_cursor_goto_first(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    cursor->current_pos = BLOCK_MANAGER_HEADER_SIZE;
    cursor->current_block_size = 0;
    cursor->block_index = -1;
    cursor->block_size_valid = 0;

    return 0;
}

int block_manager_cursor_goto_last_before(block_manager_cursor_t *cursor, const uint64_t end_offset)
{
    if (!cursor) return -1;

    if (end_offset <= BLOCK_MANAGER_HEADER_SIZE)
    {
        return -1;
    }

    /* we read footer of last block to get its size */
    unsigned char footer_buf[BLOCK_MANAGER_FOOTER_SIZE];
    const off_t footer_offset = (off_t)(end_offset - BLOCK_MANAGER_FOOTER_SIZE);
    const ssize_t n = pread(cursor->bm->fd, footer_buf, BLOCK_MANAGER_FOOTER_SIZE, footer_offset);

    if (n != BLOCK_MANAGER_FOOTER_SIZE)
    {
        return -1;
    }

    const uint32_t block_size = decode_uint32_le_compat(footer_buf);
    const uint32_t footer_magic =
        decode_uint32_le_compat(footer_buf + BLOCK_MANAGER_CHECKSUM_LENGTH);

    /* we verify footer magic */
    if (footer_magic != BLOCK_MANAGER_FOOTER_MAGIC || block_size == 0)
    {
        return -1;
    }

    /* we calculate start position of last block */
    const uint64_t total_block_size =
        BLOCK_MANAGER_BLOCK_HEADER_SIZE + block_size + BLOCK_MANAGER_FOOTER_SIZE;
    if (end_offset < total_block_size)
    {
        return -1;
    }

    cursor->current_pos = end_offset - total_block_size;
    cursor->current_block_size = block_size;
    cursor->block_size_valid = 1; /* we know the size from footer */
    cursor->block_index = -1;     /* unknown index */

    return 0;
}

int block_manager_cursor_goto_last(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    /* O(1) seek to end and work backwards using footer */
    const uint64_t file_size = atomic_load(&cursor->bm->current_file_size);
    return block_manager_cursor_goto_last_before(cursor, file_size);
}

int block_manager_truncate(block_manager_t *bm)
{
    if (!bm) return -1;

    /* we truncate to header-only (preserves valid header, single sync) */
    if (truncate_to_header(bm) != 0) return -1;

    /* reopen the fd so any stale O_APPEND/seek state is reset and the descriptor
     * reflects the freshly truncated file (caller must have quiesced readers) */
    if (reopen_fd(bm) != 0) return -1;

    return 0;
}

int block_manager_cursor_at_first(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;
    return (cursor->current_pos == BLOCK_MANAGER_HEADER_SIZE) ? 1 : 0;
}

int block_manager_cursor_at_second(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    /* if at first block, not at second */
    if (cursor->current_pos == BLOCK_MANAGER_HEADER_SIZE) return 0;

    /* we read first block size */
    unsigned char first_size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
    if (pread(cursor->bm->fd, first_size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE,
              (off_t)BLOCK_MANAGER_HEADER_SIZE) != BLOCK_MANAGER_SIZE_FIELD_SIZE)
        return -1;
    const uint32_t first_block_size = decode_uint32_le_compat(first_size_buf);
    if (first_block_size == 0) return -1;

    /* we calculate second block position, first_block_pos + [size][checksum][data][footer] */
    const uint64_t first_total_size =
        BLOCK_MANAGER_BLOCK_HEADER_SIZE + (uint64_t)first_block_size + BLOCK_MANAGER_FOOTER_SIZE;
    const uint64_t second_block_pos = BLOCK_MANAGER_HEADER_SIZE + first_total_size;

    return (cursor->current_pos == second_block_pos) ? 1 : 0;
}

int block_manager_cursor_at_last(block_manager_cursor_t *cursor)
{
    if (!cursor) return -1;

    /* we use cached block size to avoid pread syscall when possible */
    uint32_t block_size;
    if (cursor->block_size_valid && cursor->current_block_size > 0)
    {
        block_size = (uint32_t)cursor->current_block_size;
    }
    else
    {
        unsigned char size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
        if (pread(cursor->bm->fd, size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE,
                  (off_t)cursor->current_pos) != BLOCK_MANAGER_SIZE_FIELD_SIZE)
            return -1;
        block_size = decode_uint32_le_compat(size_buf);
        if (block_size == 0) return -1;
    }

    /* we calculate position after current block, [size][checksum][data][footer] */
    const uint64_t total_block_size =
        BLOCK_MANAGER_BLOCK_HEADER_SIZE + block_size + BLOCK_MANAGER_FOOTER_SIZE;
    const uint64_t next_block_pos = cursor->current_pos + total_block_size;

    /* we check against cached file size, if there's no room after this block, we're at last */
    const uint64_t file_size = atomic_load(&cursor->bm->current_file_size);
    return (next_block_pos >= file_size) ? 1 : 0;
}

int block_manager_get_size(block_manager_t *bm, uint64_t *size)
{
    if (!bm || !size) return -1;
    *size = atomic_load(&bm->current_file_size);
    return 0;
}

int block_manager_cursor_goto(block_manager_cursor_t *cursor, const uint64_t pos)
{
    if (!cursor) return -1;

    cursor->current_pos = pos;
    cursor->block_size_valid = 0; /* we invalidate cache when jumping to arbitrary position */
    cursor->block_index = -1;     /* index is unknown after an arbitrary jump */
    return 0;
}

int block_manager_escalate_fsync(block_manager_t *bm)
{
    if (!bm) return -1;
    return fdatasync(bm->fd);
}

time_t block_manager_last_modified(block_manager_t *bm)
{
    if (!bm) return -1;

    struct STAT_STRUCT st;
    if (STAT_FUNC(bm->file_path, &st) != 0) return -1;
    return st.st_mtime;
}

int block_manager_count_blocks(block_manager_t *bm)
{
    if (!bm) return -1;

    const uint64_t file_size = atomic_load(&bm->current_file_size);
    if (file_size <= BLOCK_MANAGER_HEADER_SIZE) return 0;

    set_file_sequential_hint(bm->fd);

    /** buffered scan where we read 64KB chunks so thousands of block headers are parsed per
     * syscall. we only need the first 4 bytes of each block (size field) to compute the skip
     * distance. */
    enum
    {
        COUNT_BUF = 64 * 1024
    };
    uint8_t *buf = bm_get_read_buf(COUNT_BUF);
    if (!buf)
    {
        /* fallback to per-block pread via cursor */
        block_manager_cursor_t c;
        int n = 0;
        (void)block_manager_cursor_init_stack(&c, bm);
        while (block_manager_cursor_next(&c) == 0) n++;
        return n;
    }

    int count = 0;
    uint64_t pos = BLOCK_MANAGER_HEADER_SIZE;

    while (pos < file_size)
    {
        size_t want = COUNT_BUF;
        if (pos + want > file_size) want = (size_t)(file_size - pos);

        const ssize_t got = pread(bm->fd, buf, want, (off_t)pos);
        if (got < (ssize_t)BLOCK_MANAGER_SIZE_FIELD_SIZE) break;

        size_t off = 0;
        while (off + BLOCK_MANAGER_SIZE_FIELD_SIZE <= (size_t)got)
        {
            const uint32_t bsz = decode_uint32_le_compat(buf + off);
            if (bsz == 0) return count;

            const size_t total =
                BLOCK_MANAGER_BLOCK_HEADER_SIZE + (size_t)bsz + BLOCK_MANAGER_FOOTER_SIZE;

            if (off + total > (size_t)got)
            {
                /* block straddles buffer edge, we break to re-read from this block's start */
                break;
            }

            off += total;
            count++;
        }

        /** we advance file position by bytes consumed.
         *  if off == 0, one block is larger than the buffer, we read its size and skip. */
        if (off == 0)
        {
            const uint32_t bsz = decode_uint32_le_compat(buf);
            pos += BLOCK_MANAGER_BLOCK_HEADER_SIZE + (uint64_t)bsz + BLOCK_MANAGER_FOOTER_SIZE;
            count++;
        }
        else
        {
            pos += off;
        }
    }

    return count;
}

int block_manager_validate_last_block(block_manager_t *bm,
                                      const tidesdb_block_validation_mode_t validation)
{
    if (!bm) return -1;

    uint64_t file_size;
    if (get_file_size(bm->fd, &file_size) != 0) return -1;

    atomic_store(&bm->current_file_size, file_size);
    atomic_store(&bm->preallocated_size, file_size);

    /* if file is empty, we write header */
    if (file_size == 0)
    {
        if (write_header(bm->fd) != 0)
        {
            return -1;
        }
        if (is_sync_full(bm) && !odsync_available())
        {
            fdatasync(bm->fd);
        }
        return 0;
    }

    if (file_size == BLOCK_MANAGER_HEADER_SIZE)
    {
        return 0; /* valid empty file with header */
    }

    /* we must ensure we have at least header + minimum block */
    const uint64_t min_block_size = BLOCK_MANAGER_BLOCK_HEADER_SIZE + BLOCK_MANAGER_FOOTER_SIZE;
    if (file_size < BLOCK_MANAGER_HEADER_SIZE + min_block_size)
    {
        if (validation == BLOCK_MANAGER_STRICT_BLOCK_VALIDATION)
        {
            return -1;
        }
        return truncate_to_header(bm);
    }

    /* O(1) validation, we read footer of last block */
    unsigned char footer_buf[BLOCK_MANAGER_FOOTER_SIZE];
    const off_t footer_offset = (off_t)(file_size - BLOCK_MANAGER_FOOTER_SIZE);
    const ssize_t n = pread(bm->fd, footer_buf, BLOCK_MANAGER_FOOTER_SIZE, footer_offset);

    if (n != BLOCK_MANAGER_FOOTER_SIZE)
    {
        if (validation == BLOCK_MANAGER_STRICT_BLOCK_VALIDATION)
        {
            /* strict mode -- can't read footer = corruption */
            return -1;
        }
        /* permissive mode -- truncate to header */
        return truncate_to_header(bm);
    }

    const uint32_t footer_size = decode_uint32_le_compat(footer_buf);
    const uint32_t footer_magic =
        decode_uint32_le_compat(footer_buf + BLOCK_MANAGER_CHECKSUM_LENGTH);

    /* we check if footer is valid */
    if (footer_magic != BLOCK_MANAGER_FOOTER_MAGIC)
    {
        /*** the trailing region might be preallocation tail (zeros from fallocate after
         **  the last valid block) rather than corruption. forward-scan to find the actual
         *   data extent, then check whether the trailing region is all zeros to decide. */
        uint64_t scan_pos = BLOCK_MANAGER_HEADER_SIZE;
        uint64_t valid_size = BLOCK_MANAGER_HEADER_SIZE;
        int hit_corruption = 0; /* 1 = found non-zero garbage or partial block */

        while (scan_pos + min_block_size <= file_size)
        {
            unsigned char size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
            if (pread(bm->fd, size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE, (off_t)scan_pos) !=
                BLOCK_MANAGER_SIZE_FIELD_SIZE)
            {
                hit_corruption = 1;
                break;
            }

            const uint32_t block_size = decode_uint32_le_compat(size_buf);
            if (block_size == 0) break; /* end of data; tail is either prealloc or hole */

            const uint64_t total_block_size =
                BLOCK_MANAGER_BLOCK_HEADER_SIZE + block_size + BLOCK_MANAGER_FOOTER_SIZE;
            if (scan_pos + total_block_size > file_size)
            {
                hit_corruption = 1; /* declared size overruns file */
                break;
            }

            /* we verify footer of this block */
            const off_t block_footer_offset =
                (off_t)(scan_pos + total_block_size - BLOCK_MANAGER_FOOTER_SIZE);
            if (pread(bm->fd, footer_buf, BLOCK_MANAGER_FOOTER_SIZE, block_footer_offset) !=
                BLOCK_MANAGER_FOOTER_SIZE)
            {
                hit_corruption = 1;
                break;
            }

            const uint32_t block_footer_size = decode_uint32_le_compat(footer_buf);
            const uint32_t block_footer_magic =
                decode_uint32_le_compat(footer_buf + BLOCK_MANAGER_CHECKSUM_LENGTH);

            if (block_footer_magic != BLOCK_MANAGER_FOOTER_MAGIC || block_footer_size != block_size)
            {
                hit_corruption = 1;
                break;
            }

            valid_size = scan_pos + total_block_size;
            scan_pos += total_block_size;
        }

        /* if we stopped without explicit corruption, verify the trailing region is
         * all zeros -- that confirms it's preallocation tail, not a partial write. */
        const int trailing_zero =
            hit_corruption ? 0 : is_trailing_zero(bm->fd, valid_size, file_size);

        if (validation == BLOCK_MANAGER_STRICT_BLOCK_VALIDATION)
        {
            if (hit_corruption || trailing_zero != 1) return -1;
            /* preallocation tail is legitimate; don't truncate, just record true extent */
            atomic_store(&bm->current_file_size, valid_size);
            return 0;
        }

        /* permissive mode -- truncate trailing garbage OR preallocation tail so
         * the file is always self-describing on next open */
        if (valid_size != file_size)
        {
            if (ftruncate(bm->fd, (off_t)valid_size) != 0) return -1;

            if (is_sync_full(bm))
            {
                fdatasync(bm->fd);
            }

            if (reopen_fd(bm) != 0) return -1;
            atomic_store(&bm->current_file_size, valid_size);
            atomic_store(&bm->preallocated_size, valid_size);
        }

        return 0;
    }

    /* the footer magic is valid, we verify size matches header */
    const uint64_t min_required =
        (uint64_t)BLOCK_MANAGER_FOOTER_SIZE + footer_size + BLOCK_MANAGER_BLOCK_HEADER_SIZE;
    if (file_size < min_required + BLOCK_MANAGER_HEADER_SIZE)
    {
        if (validation == BLOCK_MANAGER_STRICT_BLOCK_VALIDATION)
        {
            /*** strict mode -- invalid block position = corruption */
            return -1;
        }
        /*** permissive mode -- truncate to header */
        return truncate_to_header(bm);
    }

    const uint64_t block_start = file_size - min_required;

    unsigned char header_size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
    if (pread(bm->fd, header_size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE, (off_t)block_start) !=
        BLOCK_MANAGER_SIZE_FIELD_SIZE)
    {
        /* we cant read block header = I/O error (fail in both modes) */
        return -1;
    }

    const uint32_t header_size = decode_uint32_le_compat(header_size_buf);
    if (header_size != footer_size)
    {
        /* size mismatch = corruption (fail in both modes, this is unrecoverable) */
        return -1;
    }

    /* the last block is valid, no truncation needed */
    return 0;
}

/*
 * convert_sync_mode
 * converts tidesdb sync mode to block manager sync mode
 * @param tdb_sync_mode the tidesdb sync mode
 * @return the corresponding block manager sync mode
 */
block_manager_sync_mode_t convert_sync_mode(const int tdb_sync_mode)
{
    switch (tdb_sync_mode)
    {
        case 0:
            return BLOCK_MANAGER_SYNC_NONE;
        case 1:
            return BLOCK_MANAGER_SYNC_FULL;
        default:
            return BLOCK_MANAGER_SYNC_NONE;
    }
}

void block_manager_set_sync_mode(block_manager_t *bm, const int sync_mode)
{
    if (!bm) return;
    bm->sync_mode = convert_sync_mode(sync_mode);
    bm->sync_full_cached = (bm->sync_mode == BLOCK_MANAGER_SYNC_FULL);
}

int block_manager_get_block_size_at_offset(block_manager_t *bm, const uint64_t offset,
                                           uint32_t *size)
{
    if (!bm || !size) return -1;

    /* we read the size field from block header */
    unsigned char size_buf[BLOCK_MANAGER_SIZE_FIELD_SIZE];
    const ssize_t nread = pread(bm->fd, size_buf, BLOCK_MANAGER_SIZE_FIELD_SIZE, (off_t)offset);
    if (nread != BLOCK_MANAGER_SIZE_FIELD_SIZE)
    {
        return -1;
    }

    *size = decode_uint32_le_compat(size_buf);
    if (*size == 0) return -1; /* invalid block */

    return 0;
}

int block_manager_read_at_offset(block_manager_t *bm, const uint64_t offset, const size_t size,
                                 uint8_t *data)
{
    if (!bm || !data || size == 0) return -1;

    /* we do a simple pread at the specified offset */
    const ssize_t nread = pread(bm->fd, data, size, (off_t)offset);
    if (nread != (ssize_t)size)
    {
        return -1;
    }

    return 0;
}

int block_manager_read_block_data_at_offset(block_manager_t *bm, const uint64_t offset,
                                            uint8_t **data, uint32_t *data_size)
{
    if (!bm || !data || !data_size) return -1;

    /* offset points at a known-good block (vlog lookup), so no extent/budget check;
     * the single optimistic pread + checksum verify happen inside the helper */
    uint32_t block_size = 0;
    uint8_t *payload = bm_read_block_tls(bm->fd, offset, 0, 0, &block_size);
    if (BM_UNLIKELY(!payload)) return -1;

    uint8_t *block_data = malloc(block_size);
    if (BM_UNLIKELY(!block_data)) return -1;

    memcpy(block_data, payload, block_size);
    *data = block_data;
    *data_size = block_size;
    return 0;
}

int block_manager_open(block_manager_t **bm, const char *file_path, const int sync_mode)
{
    if (!bm || !file_path) return -1;
    return block_manager_open_internal(bm, file_path, convert_sync_mode(sync_mode));
}
