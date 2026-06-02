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
#ifndef __BLOCK_MANAGER_H__
#define __BLOCK_MANAGER_H__
#include "compat.h"

/* max file path length for block manager file(s) */
#define MAX_FILE_PATH_LENGTH (1024 * 4)

/* TDB in hex */
#define BLOCK_MANAGER_MAGIC 0x544442
/* 3-byte mask for magic number validation */
#define BLOCK_MANAGER_MAGIC_MASK 0xFFFFFF
#define BLOCK_MANAGER_VERSION    7

/* header field sizes */
/* magic number size in bytes */
#define BLOCK_MANAGER_MAGIC_SIZE 3
/* version field size in bytes */
#define BLOCK_MANAGER_VERSION_SIZE 1
#define BLOCK_MANAGER_HEADER_SIZE  8

/* block field sizes */
/* block size field (uint32_t) -- supports blocks up to 4GB, though try to keep it under! */
#define BLOCK_MANAGER_SIZE_FIELD_SIZE 4
/* xxHash32 = 4 bytes (sufficient for block-level checksums) */
#define BLOCK_MANAGER_CHECKSUM_LENGTH 4

/* block header is now just size + checksum */
#define BLOCK_MANAGER_BLOCK_HEADER_SIZE \
    (BLOCK_MANAGER_SIZE_FIELD_SIZE + BLOCK_MANAGER_CHECKSUM_LENGTH)

/* block footer for fast validation -- size + magic */
#define BLOCK_MANAGER_FOOTER_MAGIC 0x42445442 /* "BTDB" reversed */
#define BLOCK_MANAGER_FOOTER_SIZE  8          /* 4-byte size + 4-byte magic */

/* number of iovecs we emit per block in pwritev ( header, payload, footer ) */
#define BLOCK_MANAGER_IOVECS_PER_BLOCK 3

/* default file permissions (rw-r--r--) */
#define BLOCK_MANAGER_FILE_MODE 0644

/* preallocation tunables -- controls how aggressively we extend on-disk allocation
 * ahead of writes to avoid the kernel's file-extending lock on every pwrite.
 * extending writes serialize on the per-inode lock (e.g., ext4 i_rwsem) regardless
 * of disjoint offsets, so we preallocate in chunks and let pwrites land in-place. */
#define BLOCK_MANAGER_PREALLOC_CHUNK    (64ull * 1024 * 1024) /* extend by 64 MB at a time */
#define BLOCK_MANAGER_PREALLOC_LOWWATER (4ull * 1024 * 1024)  /* trigger extend when 4 MB left */

typedef enum
{
    BLOCK_MANAGER_SYNC_NONE,
    BLOCK_MANAGER_SYNC_FULL,
} block_manager_sync_mode_t;

typedef enum
{
    BLOCK_MANAGER_PERMISSIVE_BLOCK_VALIDATION =
        0, /* no error on validation, we truncate to last valid block */
    BLOCK_MANAGER_STRICT_BLOCK_VALIDATION = 1 /* error on validation */
} tidesdb_block_validation_mode_t;

/**
 * block_manager_t
 * block manager struct
 * used for block managers in TidesDB
 * @param fd the file descriptor the block manager is managing
 * @param file_path the path of the file
 * @param sync_mode sync mode for this block manager
 * @param sync_full_cached cached result of (sync_mode == BLOCK_MANAGER_SYNC_FULL)
 * @param current_file_size track file size in memory to avoid syscalls
 * @param preallocated_size on-disk allocation high water mark; pwrites within
 *                          [HEADER_SIZE, preallocated_size) avoid extending the file
 *                          and skip the kernel's per-inode write lock fast path.
 *                          set to UINT64_MAX if preallocation is unsupported on this
 *                          platform/fs to disable further attempts.
 * @param group_durable_size bytes of the file confirmed fdatasync'd, used by group-commit
 *                           callers to tell whether their write is already durable
 * @param group_sync_active set while a group-commit leader is mid-fdatasync on this file
 */
typedef struct
{
    int fd;
    char file_path[MAX_FILE_PATH_LENGTH];
    block_manager_sync_mode_t sync_mode;
    int sync_full_cached; /* cached result of (sync_mode == BLOCK_MANAGER_SYNC_FULL) */
    /* explicit alignment for atomic uint64_t to avoid ABI issues on 32-bit platforms */
    ATOMIC_ALIGN(8) _Atomic uint64_t current_file_size;
    ATOMIC_ALIGN(8) _Atomic uint64_t preallocated_size;
    ATOMIC_ALIGN(8) _Atomic uint64_t group_durable_size;
    /* atomic so concurrent group-commit leaders don't race on this flag */
    _Atomic int group_sync_active;
} block_manager_t;

/**
 * block_t
 * block struct
 * used for blocks in TidesDB
 * @param size the size of the data in the block
 * @param data the data in the block
 * @param ref_count atomic reference count for safe concurrent access
 * @param inline_data 1 if data is allocated inline with this struct (single allocation)
 */
typedef struct
{
    uint64_t size;
    void *data;
    _Atomic(uint32_t) ref_count;
    uint8_t inline_data;
} block_manager_block_t;

/**
 * block_cursor_t
 * block cursor struct
 * used for block cursors in TidesDB
 * @param bm the block manager
 * @param current_pos the current position of the cursor
 * @param current_block_size the size of the current block
 * @param block_index current index in shared position cache (-1 if before first block)
 * @param block_size_valid 1 if current_block_size is cached and valid, 0 otherwise
 */
typedef struct
{
    block_manager_t *bm;
    uint64_t current_pos;
    uint64_t current_block_size;
    int block_index;
    int block_size_valid;
} block_manager_cursor_t;

/**
 * block_manager_open
 * opens a block manager
 * @param bm the block manager to open
 * @param file_path the path of the file
 * @param sync_mode the sync mode (BLOCK_MANAGER_SYNC_NONE, BLOCK_MANAGER_SYNC_FULL)
 * @return 0 if successful, -1 if not
 */
int block_manager_open(block_manager_t **bm, const char *file_path, int sync_mode);

/**
 * block_manager_close
 * closes a block manager gracefully
 * @param bm the block manager to close
 * @return 0 if successful, -1 if not
 */
int block_manager_close(block_manager_t *bm);

/**
 * block_manager_block_create
 * creates a new block
 * @param size the size of the data in block
 * @param data the data to be placed in block
 * @return a new block
 */
block_manager_block_t *block_manager_block_create(uint64_t size, const void *data);

/**
 * block_manager_block_create_from_buffer
 * creates a new block taking ownership of buffer (no copy)
 * @param size the size of the data in block
 * @param data the data buffer (will be freed with block)
 * @return a new block
 */
block_manager_block_t *block_manager_block_create_from_buffer(uint64_t size, void *data);

/**
 * block_manager_block_write
 * @param bm the block manager to write the block to
 * @param block the block to write
 * @return block offset if successful, -1 if not
 */
int64_t block_manager_block_write(block_manager_t *bm, block_manager_block_t *block);

/**
 * block_manager_write_raw
 * write raw data directly to the block manager without allocating a block_manager_block_t.
 * avoids the malloc/memcpy/free cycle of block_create + block_write + block_release.
 * the data pointer only needs to be valid during this call.
 * @param bm the block manager
 * @param data pointer to the data to write
 * @param size size of the data in bytes
 * @return the offset where the block was written, or -1 on failure
 */
int64_t block_manager_write_raw(block_manager_t *bm, const void *data, uint32_t size);

/**
 * block_manager_block_write_batch
 * writes multiple blocks in a single I/O operation for better performance
 * @param bm the block manager to write the blocks to
 * @param blocks array of blocks to write
 * @param count number of blocks
 * @param offsets output array for block offsets (must be pre-allocated with count elements)
 * @return number of successfully written blocks, -1 on critical failure
 */
int block_manager_block_write_batch(block_manager_t *bm, block_manager_block_t **blocks,
                                    size_t count, int64_t *offsets);

/**
 * block_manager_write_at
 * writes raw bytes at a specific offset (for patching existing data)
 * WARNING: use with care -- this bypasses block checksums
 * @param bm the block manager
 * @param offset the file offset to write at
 * @param data the data to write
 * @param size the size of data to write
 * @return 0 if successful, -1 if not
 */
int block_manager_write_at(block_manager_t *bm, int64_t offset, const uint8_t *data, size_t size);

/**
 * block_manager_update_checksum
 * recalculates and updates the checksum of a block after in-place modification
 * use this after block_manager_write_at to fix the checksum
 * @param bm the block manager
 * @param block_offset the file offset of the block (start of block header)
 * @return 0 if successful, -1 if not
 */
int block_manager_update_checksum(block_manager_t *bm, int64_t block_offset);

/**
 * block_manager_block_free
 * frees a block
 * @param block the block to free
 */
void block_manager_block_free(block_manager_block_t *block);

/**
 * block_manager_block_acquire
 * increments reference count for a block
 * @param block the block to acquire
 * @return 1 if successful, 0 if block is being freed
 */
int block_manager_block_acquire(block_manager_block_t *block);

/**
 * block_manager_block_release
 * decrements reference count and frees block when count reaches 0
 * @param block the block to release
 */
void block_manager_block_release(block_manager_block_t *block);

/**
 * block_manager_cursor_init
 * initializes a block manager cursor (heap allocated)
 * @param cursor the cursor to initialize
 * @param bm the block manager to initialize the cursor on
 * @return 0 if successful, -1 if not
 */
int block_manager_cursor_init(block_manager_cursor_t **cursor, block_manager_t *bm);

/**
 * block_manager_cursor_init_stack
 * initializes a pre-allocated block manager cursor (stack or caller-allocated)
 * avoids heap allocation in hot paths
 * @param cursor pointer to pre-allocated cursor struct
 * @param bm the block manager to initialize the cursor on
 * @return 0 if successful, -1 if not
 */
int block_manager_cursor_init_stack(block_manager_cursor_t *cursor, block_manager_t *bm);

/**
 * cursor_next
 * moves the cursor to the next block
 * @param cursor the cursor to move
 * @return 0 if successful, -1 if not
 */
int block_manager_cursor_next(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_read
 * reads the block at the cursor current position
 * @param cursor the cursor to read from
 * @return the block read from the cursor
 */
block_manager_block_t *block_manager_cursor_read(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_read_partial
 * reads only the first max_bytes of a block at cursor position
 * useful for reading header+key without reading large values
 * @param cursor the cursor to read from
 * @param max_bytes maximum bytes to read (0 = read full block)
 * @return the partial block read from the cursor
 */
block_manager_block_t *block_manager_cursor_read_partial(block_manager_cursor_t *cursor,
                                                         size_t max_bytes);

/**
 * block_manager_cursor_read_and_advance
 * reads the block at cursor position and advances cursor to next block in one operation
 * this is more efficient than separate read + next calls as it avoids redundant pread
 * @param cursor the cursor to read from and advance
 * @return the block read from the cursor, NULL on error or EOF
 */
block_manager_block_t *block_manager_cursor_read_and_advance(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_free
 * frees a cursor
 * @param cursor the cursor to free
 */
void block_manager_cursor_free(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_prev
 * moves the cursor to the previous block
 * @param cursor the cursor to move
 * @return 0 if successful, -1 if not
 */
int block_manager_cursor_prev(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_skip_corrupt
 * advances the cursor past a partially-written block at the current position.
 *
 * distinguishes two failure modes:
 *   partial write (size > 0, footer magic absent); advances cursor, returns 0.
 *   genuine corruption (size > 0, footer magic valid but checksum bad), returns -1.
 *   zero-filled hole (size == 0) -- cannot determine block extent, returns -1.
 *
 * only call after block_manager_cursor_read returns NULL to attempt recovery.
 * @param cursor the cursor positioned at the suspect block
 * @return 0 if cursor was advanced past a partial write, -1 otherwise
 */
int block_manager_cursor_skip_corrupt(block_manager_cursor_t *cursor);

/**
 * block_manager_truncate
 * truncates a block manager to 0 removing all blocks
 * @param bm the block manager to truncate
 * @return 0 if successful, -1 if not
 */
int block_manager_truncate(block_manager_t *bm);

/**
 * block_manager_last_modified
 * gets the last modified time of a block manager file
 * @param bm the block manager to get the last modified time of
 * @return the last modified time of the block manager
 */
time_t block_manager_last_modified(block_manager_t *bm);

/**
 * block_manager_count_blocks
 * counts the number of blocks in a block managed file
 * @param bm the block manager to count the blocks of
 * @return the number of blocks in the block manager
 */
int block_manager_count_blocks(block_manager_t *bm);

/**
 * block_manager_cursor_has_next
 * checks if the cursor has a next block
 * @param cursor the cursor to check
 * @return 1 if the cursor has a next block, 0 if not.  Can return -1 if error
 */
int block_manager_cursor_has_next(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_has_prev
 * checks if the cursor has a previous block
 * @param cursor the cursor to check
 * @return 1 if the cursor has a previous block, 0 if not.  Can return -1 if error
 */
int block_manager_cursor_has_prev(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_goto_last
 * moves the cursor to the last block
 * @param cursor the cursor to move
 * @return 0 if successful, -1 if not
 */
int block_manager_cursor_goto_last(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_goto_last_before
 * moves the cursor to the last block whose footer ends at end_offset, using
 * footer-based O(1) positioning. lets callers seek to the last block of a
 * logical region (e.g. an sstable's data blocks) without walking past
 * trailing blocks appended after that region.
 * @param cursor the cursor to move
 * @param end_offset byte offset immediately after the target block's footer
 * @return 0 if successful, -1 if not
 */
int block_manager_cursor_goto_last_before(block_manager_cursor_t *cursor, uint64_t end_offset);

/**
 * block_manager_cursor_goto
 * moves the cursor to a specific block
 * @param cursor the cursor to move
 * @param pos the position to move the cursor to
 * @return 0 if successful, -1 if not
 */
int block_manager_cursor_goto(block_manager_cursor_t *cursor, uint64_t pos);

/**
 * block_manager_cursor_goto_first
 * moves the cursor to the first block
 * @param cursor the cursor to move
 * @return 0 if successful, -1 if not
 */
int block_manager_cursor_goto_first(block_manager_cursor_t *cursor);

/**
 * block_manager_get_size
 * gets the total size of a block manager file
 * @param bm the block manager to get the size of
 * @param size the size of the block manager
 * @return 0 if successful, -1 if not
 */
int block_manager_get_size(block_manager_t *bm, uint64_t *size);

/**
 * block_manager_escalate_fsync
 * escalates an fsync syscall to the underlying block manager file
 * @param bm the block manager to fsync
 * @return 0 if successful, -1 if not
 */
int block_manager_escalate_fsync(block_manager_t *bm);

/**
 * block_manager_cursor_at_last
 * checks if the cursor is at the last block
 * @param cursor the cursor to check
 * @return 1 if the cursor is at the last block, 0 if not.  can return -1 if error
 */
int block_manager_cursor_at_last(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_at_first
 * checks if the cursor is at the first block
 * @param cursor the cursor to check
 * @return 1 if the cursor is at the first block, 0 if not.  can return -1 if error
 */
int block_manager_cursor_at_first(block_manager_cursor_t *cursor);

/**
 * block_manager_cursor_at_second
 * checks if the cursor is at the second block from start
 * @param cursor the cursor to check
 * @return 1 if the cursor is at the second block, 0 if not.  can return -1 if error
 */
int block_manager_cursor_at_second(block_manager_cursor_t *cursor);

/**
 * block_manager_validate_last_block
 * validates the integrity of the last block in a block manager file
 * @param bm the block manager
 * @param validation the type of validation to apply, either strict or permissive
 * @return 0 if valid or successfully recovered, -1 if validation fails
 *
 * In strict mode -- any corruption returns -1, file is not modified
 * In permissive mode -- truncates to last valid block on corruption
 */
int block_manager_validate_last_block(block_manager_t *bm,
                                      tidesdb_block_validation_mode_t validation);

/**
 * block_manager_set_max_safe_block_bytes
 * sets a process-wide upper bound (bytes) on the size of a single block the
 * reader will allocate. a block whose claimed size exceeds this budget is
 * refused with a warning instead of allocating (graceful degradation, not OOM).
 * pushed down from the tidesdb layer (derived from resolved_memory_limit) so the
 * read path never makes a memory syscall. 0 disables the memory-based refusal.
 * @param bytes the budget in bytes, or 0 to disable
 */
void block_manager_set_max_safe_block_bytes(uint64_t bytes);

/**
 * convert_sync_mode
 * converts TidesDB sync mode enum values to block manager sync mode enum values
 * this method provides compatibility between the public TidesDB API (which uses
 * TDB_SYNC_NONE/TDB_SYNC_FULL) and the internal block manager API (which uses
 * BLOCK_MANAGER_SYNC_NONE/BLOCK_MANAGER_SYNC_FULL)
 * @param tdb_sync_mode the TidesDB sync mode (TDB_SYNC_NONE=0, TDB_SYNC_FULL=1)
 * @return the corresponding block manager sync mode enum value
 */
block_manager_sync_mode_t convert_sync_mode(int tdb_sync_mode);

/**
 * block_manager_set_sync_mode
 * updates the sync mode of an existing block manager
 * @param bm the block manager to update
 * @param sync_mode the new sync mode (TDB_SYNC_NONE=0, TDB_SYNC_FULL=1)
 */
void block_manager_set_sync_mode(block_manager_t *bm, int sync_mode);

/**
 * block_manager_get_block_size_at_offset
 * reads the size of a block at a specific file offset
 * useful for determining allocation size before reading block data
 * @param bm the block manager to read from
 * @param offset the file offset of the block (start of block header)
 * @param size output parameter for block data size (not including header)
 * @return 0 if successful, -1 if not
 */
int block_manager_get_block_size_at_offset(block_manager_t *bm, uint64_t offset, uint32_t *size);

/**
 * block_manager_read_at_offset
 * reads data at a specific file offset (not block-aligned)
 * useful for reading values from vlog where offset points to data within a block
 * @param bm the block manager to read from
 * @param offset the file offset to read from (absolute position in file)
 * @param size the number of bytes to read
 * @param data output buffer (caller must allocate)
 * @return 0 if successful, -1 if not
 */
int block_manager_read_at_offset(block_manager_t *bm, uint64_t offset, size_t size, uint8_t *data);

/**
 * block_manager_read_block_data_at_offset
 * reads a complete block (header + data) at a specific file offset in one I/O operation
 * optimized for vlog reads -- combines size lookup and data read into single pread
 * @param bm the block manager to read from
 * @param offset the file offset of the block (start of block header)
 * @param data output buffer pointer (allocated by function, caller must free)
 * @param data_size output parameter for actual data size (not including header)
 * @return 0 if successful, -1 if not
 */
int block_manager_read_block_data_at_offset(block_manager_t *bm, uint64_t offset, uint8_t **data,
                                            uint32_t *data_size);

#endif /* __BLOCK_MANAGER_H__ */
