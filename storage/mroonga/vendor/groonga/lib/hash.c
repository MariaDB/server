/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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
#include "grn_hash.h"
#include "grn_output.h"
#include <string.h>
#include <limits.h>

#include "grn_store.h"
#include "grn_normalizer.h"

/* grn_tiny_array */

/* Requirements: id != GRN_ID_NIL. */
inline static int
grn_tiny_array_get_block_id(grn_id id)
{
  int most_significant_one_bit_offset;
  GRN_BIT_SCAN_REV(id, most_significant_one_bit_offset);
  return most_significant_one_bit_offset >> GRN_TINY_ARRAY_FACTOR;
}

/* Requirements: id != GRN_ID_NIL. */
inline static void *
grn_tiny_array_get(grn_tiny_array *array, grn_id id) {
  const int block_id = grn_tiny_array_get_block_id(id);
  uint8_t * const block = (uint8_t *)array->blocks[block_id];
  if (block) {
    const size_t offset = GRN_TINY_ARRAY_GET_OFFSET(block_id);
    return block + (id - offset) * array->element_size;
  }
  return NULL;
}

/* Requirements: id != GRN_ID_NIL. */
inline static void *
grn_tiny_array_put(grn_tiny_array *array, grn_id id) {
  const int block_id = grn_tiny_array_get_block_id(id);
  void ** const block = &array->blocks[block_id];
  const size_t offset = GRN_TINY_ARRAY_GET_OFFSET(block_id);
  if (!*block) {
    grn_ctx * const ctx = array->ctx;
    if (array->flags & GRN_TINY_ARRAY_THREADSAFE) {
      CRITICAL_SECTION_ENTER(array->lock);
    }
    if (!*block) {
      const size_t block_size =
          GRN_TINY_ARRAY_GET_BLOCK_SIZE(block_id) * array->element_size;
      if (array->flags & GRN_TINY_ARRAY_USE_MALLOC) {
        if (array->flags & GRN_TINY_ARRAY_CLEAR) {
          *block = GRN_CALLOC(block_size);
        } else {
          *block = GRN_MALLOC(block_size);
        }
      } else {
        *block = GRN_CTX_ALLOC(ctx, block_size);
      }
    }
    if (array->flags & GRN_TINY_ARRAY_THREADSAFE) {
      CRITICAL_SECTION_LEAVE(array->lock);
    }
    if (!*block) {
      return NULL;
    }
  }
  if (id > array->max) {
    array->max = id;
  }
  return (uint8_t *)*block + (id - offset) * array->element_size;
}

inline static void *
grn_tiny_array_at_inline(grn_tiny_array *array, grn_id id)
{
  return id ? grn_tiny_array_put(array, id) : NULL;
}

void
grn_tiny_array_init(grn_ctx *ctx, grn_tiny_array *array,
                    uint16_t element_size, uint16_t flags)
{
  array->ctx = ctx;
  array->max = 0;
  array->element_size = element_size;
  array->flags = flags;
  memset(array->blocks, 0, sizeof(array->blocks));
  if (flags & GRN_TINY_ARRAY_THREADSAFE) {
    CRITICAL_SECTION_INIT(array->lock);
  }
}

void
grn_tiny_array_fin(grn_tiny_array *array)
{
  int block_id;
  grn_ctx * const ctx = array->ctx;
  for (block_id = 0; block_id < GRN_TINY_ARRAY_NUM_BLOCKS; block_id++) {
    if (array->blocks[block_id]) {
      if (array->flags & GRN_TINY_ARRAY_USE_MALLOC) {
        GRN_FREE(array->blocks[block_id]);
      } else {
        GRN_CTX_FREE(ctx, array->blocks[block_id]);
      }
      array->blocks[block_id] = NULL;
    }
  }
}

void *
grn_tiny_array_at(grn_tiny_array *array, grn_id id)
{
  return grn_tiny_array_at_inline(array, id);
}

grn_id
grn_tiny_array_id(grn_tiny_array *array, const void *element_address)
{
  const uint8_t * const ptr = (const uint8_t *)element_address;
  uint32_t block_id, offset = 1;
  for (block_id = 0; block_id < GRN_TINY_ARRAY_NUM_BLOCKS; block_id++) {
    const uint32_t block_size = GRN_TINY_ARRAY_GET_BLOCK_SIZE(block_id);
    const uint8_t * const block = (const uint8_t *)array->blocks[block_id];
    if (block) {
      if (block <= ptr && ptr < (block + block_size * array->element_size)) {
        return offset + ((ptr - block) / array->element_size);
      }
    }
    offset += block_size;
  }
  return GRN_ID_NIL;
}

/* grn_tiny_bitmap */

static void
grn_tiny_bitmap_init(grn_ctx *ctx, grn_tiny_bitmap *bitmap)
{
  bitmap->ctx = ctx;
  memset(bitmap->blocks, 0, sizeof(bitmap->blocks));
}

static void
grn_tiny_bitmap_fin(grn_tiny_bitmap *bitmap)
{
  int block_id;
  grn_ctx * const ctx = bitmap->ctx;
  for (block_id = 0; block_id < GRN_TINY_ARRAY_NUM_BLOCKS; block_id++) {
    if (bitmap->blocks[block_id]) {
      GRN_CTX_FREE(ctx, bitmap->blocks[block_id]);
      bitmap->blocks[block_id] = NULL;
    }
  }
}

/* Requirements: bit_id != GRN_ID_NIL. */
inline static uint8_t *
grn_tiny_bitmap_get_byte(grn_tiny_bitmap *bitmap, grn_id bit_id) {
  const uint32_t byte_id = (bit_id >> 3) + 1;
  const int block_id = grn_tiny_array_get_block_id(byte_id);
  uint8_t * const block = (uint8_t *)bitmap->blocks[block_id];
  if (block) {
    const size_t offset = GRN_TINY_ARRAY_GET_OFFSET(block_id);
    return block + byte_id - offset;
  }
  return NULL;
}

/* Requirements: bit_id != GRN_ID_NIL. */
inline static uint8_t *
grn_tiny_bitmap_put_byte(grn_tiny_bitmap *bitmap, grn_id bit_id) {
  const uint32_t byte_id = (bit_id >> 3) + 1;
  const int block_id = grn_tiny_array_get_block_id(byte_id);
  void ** const block = &bitmap->blocks[block_id];
  const size_t offset = GRN_TINY_ARRAY_GET_OFFSET(block_id);
  if (!*block) {
    grn_ctx * const ctx = bitmap->ctx;
    *block = GRN_CTX_ALLOC(ctx, GRN_TINY_ARRAY_GET_BLOCK_SIZE(block_id));
    if (!*block) {
      return NULL;
    }
  }
  return (uint8_t *)*block + byte_id - offset;
}

/* Requirements: bit_id != GRN_ID_NIL. */
/* Return value: 1/0 on success, -1 on failure. */
/* Note: A bitmap is extended if needed. */
inline static int
grn_tiny_bitmap_put(grn_tiny_bitmap *bitmap, grn_id bit_id)
{
  uint8_t * const ptr = grn_tiny_bitmap_put_byte(bitmap, bit_id);
  return ptr ? ((*ptr >> (bit_id & 7)) & 1) : -1;
}

/* Requirements: bit_id != GRN_ID_NIL. */
inline static uint8_t *
grn_tiny_bitmap_get_and_set(grn_tiny_bitmap *bitmap, grn_id bit_id,
                            grn_bool bit)
{
  uint8_t * const ptr = grn_tiny_bitmap_get_byte(bitmap, bit_id);
  if (ptr) {
    /* This branch will be removed because the given `bit' is constant. */
    if (bit) {
      *ptr |= 1 << (bit_id & 7);
    } else {
      *ptr &= ~(1 << (bit_id & 7));
    }
  }
  return ptr;
}

/* Requirements: bit_id != GRN_ID_NIL. */
/* Note: A bitmap is extended if needed. */
inline static uint8_t *
grn_tiny_bitmap_put_and_set(grn_tiny_bitmap *bitmap, grn_id bit_id,
                            grn_bool bit)
{
  uint8_t * const ptr = grn_tiny_bitmap_put_byte(bitmap, bit_id);
  if (ptr) {
    /* This branch will be removed because the given `bit' is constant. */
    if (bit) {
      *ptr |= 1 << (bit_id & 7);
    } else {
      *ptr &= ~(1 << (bit_id & 7));
    }
  }
  return ptr;
}

/* grn_io_array */

#define GRN_ARRAY_MAX (GRN_ID_MAX - 8)

inline static void *
grn_io_array_at_inline(grn_ctx *ctx, grn_io *io, uint32_t segment_id,
                       uint64_t offset, int flags)
{
  void *ptr;
  GRN_IO_ARRAY_AT(io, segment_id, offset, &flags, ptr);
  return ptr;
}

/*
 * grn_io_array_bit_at() returns 1/0 on success, -1 on failure.
 */
inline static int
grn_io_array_bit_at(grn_ctx *ctx, grn_io *io,
                    uint32_t segment_id, uint32_t offset)
{
  uint8_t * const ptr = (uint8_t *)grn_io_array_at_inline(
      ctx, io, segment_id, (offset >> 3) + 1, 0);
  return ptr ? ((*ptr >> (offset & 7)) & 1) : -1;
}

/*
 * The following functions, grn_io_array_bit_*(), return a non-NULL pointer on
 * success, a NULL pointer on failure.
 */
inline static void *
grn_io_array_bit_on(grn_ctx *ctx, grn_io *io,
                    uint32_t segment_id, uint32_t offset)
{
  uint8_t * const ptr = (uint8_t *)grn_io_array_at_inline(
      ctx, io, segment_id, (offset >> 3) + 1, GRN_TABLE_ADD);
  if (ptr) {
    *ptr |= 1 << (offset & 7);
  }
  return ptr;
}

inline static void *
grn_io_array_bit_off(grn_ctx *ctx, grn_io *io,
                     uint32_t segment_id, uint32_t offset)
{
  uint8_t * const ptr = (uint8_t *)grn_io_array_at_inline(
      ctx, io, segment_id, (offset >> 3) + 1, GRN_TABLE_ADD);
  if (ptr) {
    *ptr &= ~(1 << (offset & 7));
  }
  return ptr;
}

/* grn_table_queue */

static void
grn_table_queue_lock_init(grn_ctx *ctx, grn_table_queue *queue)
{
  MUTEX_INIT_SHARED(queue->mutex);
  COND_INIT_SHARED(queue->cond);
}

static void
grn_table_queue_init(grn_ctx *ctx, grn_table_queue *queue)
{
  queue->head = 0;
  queue->tail = 0;
  queue->cap = GRN_ARRAY_MAX;
  queue->unblock_requested = GRN_FALSE;
  grn_table_queue_lock_init(ctx, queue);
}

uint32_t
grn_table_queue_size(grn_table_queue *queue)
{
  return (queue->head < queue->tail)
    ? 2 * queue->cap + queue->head - queue->tail
    : queue->head - queue->tail;
}

void
grn_table_queue_head_increment(grn_table_queue *queue)
{
  if (queue->head == 2 * queue->cap) {
    queue->head = 1;
  } else {
    queue->head++;
  }
}

void
grn_table_queue_tail_increment(grn_table_queue *queue)
{
  if (queue->tail == 2 * queue->cap) {
    queue->tail = 1;
  } else {
    queue->tail++;
  }
}

grn_id
grn_table_queue_head(grn_table_queue *queue)
{
  return queue->head > queue->cap
    ? queue->head - queue->cap
    : queue->head;
}

grn_id
grn_table_queue_tail(grn_table_queue *queue)
{
  return queue->tail > queue->cap
    ? queue->tail - queue->cap
    : queue->tail;
}

/* grn_array */

#define GRN_ARRAY_SEGMENT_SIZE 0x400000

/* Header of grn_io-based grn_array. */
struct grn_array_header {
  uint32_t flags;
  uint32_t curr_rec;
  uint32_t value_size;
  uint32_t n_entries;
  uint32_t n_garbages;
  grn_id garbages;
  uint32_t lock;
  uint32_t truncated;
  uint32_t reserved[8];
  grn_table_queue queue;
};

/*
 * A grn_io-based grn_array consists of the following 2 segments.
 * GRN_ARRAY_VALUE_SEGMENT: stores values.
 * GRN_ARRAY_BITMAP_SEGMENT: stores whether entries are valid or not.
 */
enum {
  GRN_ARRAY_VALUE_SEGMENT = 0,
  GRN_ARRAY_BITMAP_SEGMENT = 1
};

inline static grn_bool
grn_array_is_io_array(grn_array *array)
{
  return array->io != NULL;
}

inline static void *
grn_array_io_entry_at(grn_ctx *ctx, grn_array *array, grn_id id, int flags)
{
  return grn_io_array_at_inline(ctx, array->io, GRN_ARRAY_VALUE_SEGMENT, id, flags);
}

inline static void *
grn_array_entry_at(grn_ctx *ctx, grn_array *array, grn_id id, int flags)
{
  if (grn_array_is_io_array(array)) {
    return grn_array_io_entry_at(ctx, array, id, flags);
  } else {
    return grn_tiny_array_at_inline(&array->array, id);
  }
}

/* grn_array_bitmap_at() returns 1/0 on success, -1 on failure. */
inline static int
grn_array_bitmap_at(grn_ctx *ctx, grn_array *array, grn_id id)
{
  if (grn_array_is_io_array(array)) {
    return grn_io_array_bit_at(ctx, array->io, GRN_ARRAY_BITMAP_SEGMENT, id);
  } else {
    return grn_tiny_bitmap_put(&array->bitmap, id);
  }
}

static grn_rc
grn_array_init_tiny_array(grn_ctx *ctx, grn_array *array, const char *path,
                          uint32_t value_size, uint32_t flags)
{
  if (path) {
    ERR(GRN_INVALID_ARGUMENT, "failed to create tiny array");
    return ctx->rc;
  }
  array->obj.header.flags = flags;
  array->ctx = ctx;
  array->value_size = value_size;
  array->n_keys = 0;
  array->keys = NULL;
  array->n_garbages = &array->n_garbages_buf;
  array->n_entries = &array->n_entries_buf;
  array->n_garbages_buf = 0;
  array->n_entries_buf = 0;
  array->io = NULL;
  array->header = NULL;
  array->garbages = GRN_ID_NIL;
  grn_tiny_array_init(ctx, &array->array, value_size, GRN_TINY_ARRAY_CLEAR);
  grn_tiny_bitmap_init(ctx, &array->bitmap);
  return GRN_SUCCESS;
}

static grn_io *
grn_array_create_io_array(grn_ctx *ctx, const char *path, uint32_t value_size)
{
  uint32_t w_of_element = 0;
  grn_io_array_spec array_spec[2];

  while ((1U << w_of_element) < value_size) {
    w_of_element++;
  }

  array_spec[GRN_ARRAY_VALUE_SEGMENT].w_of_element = w_of_element;
  array_spec[GRN_ARRAY_VALUE_SEGMENT].max_n_segments =
      1U << (30 - (22 - w_of_element));
  array_spec[GRN_ARRAY_BITMAP_SEGMENT].w_of_element = 0;
  array_spec[GRN_ARRAY_BITMAP_SEGMENT].max_n_segments = 1U << (30 - (22 + 3));
  return grn_io_create_with_array(ctx, path, sizeof(struct grn_array_header),
                                  GRN_ARRAY_SEGMENT_SIZE, grn_io_auto,
                                  2, array_spec);
}

static grn_rc
grn_array_init_io_array(grn_ctx *ctx, grn_array *array, const char *path,
                        uint32_t value_size, uint32_t flags)
{
  grn_io *io;
  struct grn_array_header *header;

  io = grn_array_create_io_array(ctx, path, value_size);
  if (!io) {
    return ctx->rc;
  }
  grn_io_set_type(io, GRN_TABLE_NO_KEY);

  header = grn_io_header(io);
  header->flags = flags;
  header->curr_rec = 0;
  header->lock = 0;
  header->value_size = value_size;
  header->n_entries = 0;
  header->n_garbages = 0;
  header->garbages = GRN_ID_NIL;
  header->truncated = GRN_FALSE;
  grn_table_queue_init(ctx, &header->queue);
  array->obj.header.flags = flags;
  array->ctx = ctx;
  array->value_size = value_size;
  array->n_keys = 0;
  array->keys = NULL;
  array->n_garbages = &header->n_garbages;
  array->n_entries = &header->n_entries;
  array->io = io;
  array->header = header;
  array->lock = &header->lock;
  return GRN_SUCCESS;
}

void
grn_array_queue_lock_clear(grn_ctx *ctx, grn_array *array)
{
  struct grn_array_header *header;
  header = grn_io_header(array->io);
  grn_table_queue_lock_init(ctx, &header->queue);
}

grn_table_queue *
grn_array_queue(grn_ctx *ctx, grn_array *array)
{
  if (grn_array_is_io_array(array)) {
    struct grn_array_header *header;
    header = grn_io_header(array->io);
    return &header->queue;
  } else {
    return NULL;
  }
}

static grn_rc
grn_array_init(grn_ctx *ctx, grn_array *array,
               const char *path, uint32_t value_size, uint32_t flags)
{
  if (flags & GRN_ARRAY_TINY) {
    return grn_array_init_tiny_array(ctx, array, path, value_size, flags);
  } else {
    return grn_array_init_io_array(ctx, array, path, value_size, flags);
  }
}

grn_array *
grn_array_create(grn_ctx *ctx, const char *path, uint32_t value_size, uint32_t flags)
{
  if (ctx) {
    grn_array * const array = (grn_array *)GRN_CALLOC(sizeof(grn_array));
    if (array) {
      GRN_DB_OBJ_SET_TYPE(array, GRN_TABLE_NO_KEY);
      if (!grn_array_init(ctx, array, path, value_size, flags)) {
        return array;
      }
      GRN_FREE(array);
    }
  }
  return NULL;
}

grn_array *
grn_array_open(grn_ctx *ctx, const char *path)
{
  if (ctx) {
    grn_io * const io = grn_io_open(ctx, path, grn_io_auto);
    if (io) {
      struct grn_array_header * const header = grn_io_header(io);
      uint32_t io_type = grn_io_get_type(io);
      if (io_type == GRN_TABLE_NO_KEY) {
        grn_array * const array = (grn_array *)GRN_MALLOC(sizeof(grn_array));
        if (array) {
          if (!(header->flags & GRN_ARRAY_TINY)) {
            GRN_DB_OBJ_SET_TYPE(array, GRN_TABLE_NO_KEY);
            array->obj.header.flags = header->flags;
            array->ctx = ctx;
            array->value_size = header->value_size;
            array->n_keys = 0;
            array->keys = NULL;
            array->n_garbages = &header->n_garbages;
            array->n_entries = &header->n_entries;
            array->io = io;
            array->header = header;
            array->lock = &header->lock;
            return array;
          } else {
            GRN_LOG(ctx, GRN_LOG_NOTICE, "invalid array flags. (%x)", header->flags);
          }
          GRN_FREE(array);
        }
      } else {
        ERR(GRN_INVALID_FORMAT,
            "[table][array] file type must be %#04x: <%#04x>",
            GRN_TABLE_NO_KEY, io_type);
      }
      grn_io_close(ctx, io);
    }
  }
  return NULL;
}

/*
 * grn_array_error_if_truncated() logs an error and returns its error code if
 * an array is truncated by another process.
 * Otherwise, this function returns GRN_SUCCESS.
 * Note that `ctx` and `array` must be valid.
 *
 * FIXME: An array should be reopened if possible.
 */
static grn_rc
grn_array_error_if_truncated(grn_ctx *ctx, grn_array *array)
{
  if (array->header && array->header->truncated) {
    ERR(GRN_FILE_CORRUPT,
        "array is truncated, please unmap or reopen the database");
    return GRN_FILE_CORRUPT;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_array_close(grn_ctx *ctx, grn_array *array)
{
  grn_rc rc = GRN_SUCCESS;
  if (!ctx || !array) { return GRN_INVALID_ARGUMENT; }
  if (array->keys) { GRN_FREE(array->keys); }
  if (grn_array_is_io_array(array)) {
    rc = grn_io_close(ctx, array->io);
  } else {
    GRN_ASSERT(ctx == array->ctx);
    grn_tiny_array_fin(&array->array);
    grn_tiny_bitmap_fin(&array->bitmap);
  }
  GRN_FREE(array);
  return rc;
}

grn_rc
grn_array_remove(grn_ctx *ctx, const char *path)
{
  if (!ctx || !path) { return GRN_INVALID_ARGUMENT; }
  return grn_io_remove(ctx, path);
}

uint32_t
grn_array_size(grn_ctx *ctx, grn_array *array)
{
  if (grn_array_error_if_truncated(ctx, array) != GRN_SUCCESS) {
    return 0;
  }
  return *array->n_entries;
}

uint32_t
grn_array_get_flags(grn_ctx *ctx, grn_array *array)
{
  return array->header->flags;
}

grn_rc
grn_array_truncate(grn_ctx *ctx, grn_array *array)
{
  grn_rc rc;
  char *path = NULL;
  uint32_t value_size, flags;

  if (!ctx || !array) { return GRN_INVALID_ARGUMENT; }
  rc = grn_array_error_if_truncated(ctx, array);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (grn_array_is_io_array(array)) {
    const char * const io_path = grn_io_path(array->io);
    if (io_path && *io_path) {
      path = GRN_STRDUP(io_path);
      if (!path) {
        ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_path);
        return GRN_NO_MEMORY_AVAILABLE;
      }
    }
  }
  value_size = array->value_size;
  flags = array->obj.header.flags;

  if (grn_array_is_io_array(array)) {
    if (path) {
      /* Only an I/O array with a valid path uses the `truncated` flag. */
      array->header->truncated = GRN_TRUE;
    }
    rc = grn_io_close(ctx, array->io);
    if (!rc) {
      array->io = NULL;
      if (path) {
        rc = grn_io_remove(ctx, path);
      }
    }
  }
  if (!rc) {
    rc = grn_array_init(ctx, array, path, value_size, flags);
  }
  if (path) { GRN_FREE(path); }
  return rc;
}

inline static grn_id
grn_array_get_max_id(grn_array *array)
{
  return grn_array_is_io_array(array) ? array->header->curr_rec : array->array.max;
}

inline static void *
grn_array_get_value_inline(grn_ctx *ctx, grn_array *array, grn_id id)
{
  if (!ctx || !array) {
    return NULL;
  }
  if (grn_array_error_if_truncated(ctx, array) != GRN_SUCCESS) {
    return NULL;
  }
  if (*array->n_garbages) {
    /*
     * grn_array_bitmap_at() is a time-consuming function, so it is called only
     * when there are garbages in the array.
     */
    if (grn_array_bitmap_at(ctx, array, id) != 1) {
      return NULL;
    }
  } else if (id == 0 || id > grn_array_get_max_id(array)) {
    return NULL;
  }
  return grn_array_entry_at(ctx, array, id, 0);
}

int
grn_array_get_value(grn_ctx *ctx, grn_array *array, grn_id id, void *valuebuf)
{
  void * const value = grn_array_get_value_inline(ctx, array, id);
  if (value) {
    if (valuebuf) {
      grn_memcpy(valuebuf, value, array->value_size);
    }
    return array->value_size;
  }
  return 0;
}

void *
_grn_array_get_value(grn_ctx *ctx, grn_array *array, grn_id id)
{
  return grn_array_get_value_inline(ctx, array, id);
}

inline static grn_rc
grn_array_set_value_inline(grn_ctx *ctx, grn_array *array, grn_id id,
                           const void *value, int flags)
{
  void *entry;
  entry = grn_array_entry_at(ctx, array, id, 0);
  if (!entry) {
    return GRN_NO_MEMORY_AVAILABLE;
  }

  switch ((flags & GRN_OBJ_SET_MASK)) {
  case GRN_OBJ_SET :
    grn_memcpy(entry, value, array->value_size);
    return GRN_SUCCESS;
  case GRN_OBJ_INCR :
    switch (array->value_size) {
    case sizeof(int32_t) :
      *((int32_t *)entry) += *((int32_t *)value);
      return GRN_SUCCESS;
    case sizeof(int64_t) :
      *((int64_t *)entry) += *((int64_t *)value);
      return GRN_SUCCESS;
    default :
      return GRN_INVALID_ARGUMENT;
    }
    break;
  case GRN_OBJ_DECR :
    switch (array->value_size) {
    case sizeof(int32_t) :
      *((int32_t *)entry) -= *((int32_t *)value);
      return GRN_SUCCESS;
    case sizeof(int64_t) :
      *((int64_t *)entry) -= *((int64_t *)value);
      return GRN_SUCCESS;
    default :
      return GRN_INVALID_ARGUMENT;
    }
    break;
  default :
    /* todo : support other types. */
    return GRN_INVALID_ARGUMENT;
  }
}

grn_rc
grn_array_set_value(grn_ctx *ctx, grn_array *array, grn_id id,
                    const void *value, int flags)
{
  grn_rc rc;

  if (!ctx || !array || !value) {
    return GRN_INVALID_ARGUMENT;
  }

  rc = grn_array_error_if_truncated(ctx, array);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (*array->n_garbages) {
    /*
     * grn_array_bitmap_at() is a time-consuming function, so it is called only
     * when there are garbages in the array.
     */
    if (grn_array_bitmap_at(ctx, array, id) != 1) {
      return GRN_INVALID_ARGUMENT;
    }
  } else if (id == 0 || id > grn_array_get_max_id(array)) {
    return GRN_INVALID_ARGUMENT;
  }
  return grn_array_set_value_inline(ctx, array, id, value, flags);
}

grn_rc
grn_array_delete_by_id(grn_ctx *ctx, grn_array *array, grn_id id,
                       grn_table_delete_optarg *optarg)
{
  grn_rc rc;
  if (!ctx || !array) {
    return GRN_INVALID_ARGUMENT;
  }
  rc = grn_array_error_if_truncated(ctx, array);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (grn_array_bitmap_at(ctx, array, id) != 1) {
    return GRN_INVALID_ARGUMENT;
  }

  {
    rc = GRN_SUCCESS;
    /* lock */
    if (grn_array_is_io_array(array)) {
      if (array->value_size >= sizeof(grn_id)) {
        struct grn_array_header * const header = array->header;
        void * const entry = grn_array_io_entry_at(ctx, array, id, 0);
        if (!entry) {
          rc = GRN_INVALID_ARGUMENT;
        } else {
          *((grn_id *)entry) = header->garbages;
          header->garbages = id;
        }
      }
      if (!rc) {
        (*array->n_entries)--;
        (*array->n_garbages)++;
        /*
         * The following grn_io_array_bit_off() fails iff a problem has
         * occurred after the above grn_array_bitmap_at(). That is to say,
         * an unexpected case.
         */
        grn_io_array_bit_off(ctx, array->io, GRN_ARRAY_BITMAP_SEGMENT, id);
      }
    } else {
      if (array->value_size >= sizeof(grn_id)) {
        void * const entry = grn_tiny_array_get(&array->array, id);
        if (!entry) {
          rc = GRN_INVALID_ARGUMENT;
        } else {
          *((grn_id *)entry) = array->garbages;
          array->garbages = id;
        }
      }
      if (!rc) {
        (*array->n_entries)--;
        (*array->n_garbages)++;
        /*
         * The following grn_io_array_bit_off() fails iff a problem has
         * occurred after the above grn_array_bitmap_at(). That is to say,
         * an unexpected case.
         */
        grn_tiny_bitmap_get_and_set(&array->bitmap, id, 0);
      }
    }
    /* unlock */
    return rc;
  }
}

grn_id
grn_array_at(grn_ctx *ctx, grn_array *array, grn_id id)
{
  if (grn_array_error_if_truncated(ctx, array) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  if (*array->n_garbages) {
    /*
     * grn_array_bitmap_at() is a time-consuming function, so it is called only
     * when there are garbages in the array.
     */
    if (grn_array_bitmap_at(ctx, array, id) != 1) {
      return GRN_ID_NIL;
    }
  } else if (id > grn_array_get_max_id(array)) {
    return GRN_ID_NIL;
  }
  return id;
}

grn_rc
grn_array_copy_sort_key(grn_ctx *ctx, grn_array *array,
                        grn_table_sort_key *keys, int n_keys)
{
  array->keys = (grn_table_sort_key *)GRN_MALLOCN(grn_table_sort_key, n_keys);
  if (!array->keys) {
    return ctx->rc;
  }
  grn_memcpy(array->keys, keys, sizeof(grn_table_sort_key) * n_keys);
  array->n_keys = n_keys;
  return GRN_SUCCESS;
}

void
grn_array_cursor_close(grn_ctx *ctx, grn_array_cursor *cursor)
{
  GRN_ASSERT(cursor->ctx == ctx);
  GRN_FREE(cursor);
}

grn_array_cursor *
grn_array_cursor_open(grn_ctx *ctx, grn_array *array, grn_id min, grn_id max,
                      int offset, int limit, int flags)
{
  grn_array_cursor *cursor;
  if (!array || !ctx) { return NULL; }
  if (grn_array_error_if_truncated(ctx, array) != GRN_SUCCESS) {
    return NULL;
  }

  cursor = (grn_array_cursor *)GRN_MALLOCN(grn_array_cursor, 1);
  if (!cursor) { return NULL; }

  GRN_DB_OBJ_SET_TYPE(cursor, GRN_CURSOR_TABLE_NO_KEY);
  cursor->array = array;
  cursor->ctx = ctx;
  cursor->obj.header.flags = flags;
  cursor->obj.header.domain = GRN_ID_NIL;

  if (flags & GRN_CURSOR_DESCENDING) {
    cursor->dir = -1;
    if (max) {
      cursor->curr_rec = max;
      if (!(flags & GRN_CURSOR_LT)) { cursor->curr_rec++; }
    } else {
      cursor->curr_rec = grn_array_get_max_id(array) + 1;
    }
    if (min) {
      cursor->tail = min;
      if ((flags & GRN_CURSOR_GT)) { cursor->tail++; }
    } else {
      cursor->tail = GRN_ID_NIL + 1;
    }
    if (cursor->curr_rec < cursor->tail) { cursor->tail = cursor->curr_rec; }
  } else {
    cursor->dir = 1;
    if (min) {
      cursor->curr_rec = min;
      if (!(flags & GRN_CURSOR_GT)) { cursor->curr_rec--; }
    } else {
      cursor->curr_rec = GRN_ID_NIL;
    }
    if (max) {
      cursor->tail = max;
      if ((flags & GRN_CURSOR_LT)) { cursor->tail--; }
    } else {
      cursor->tail = grn_array_get_max_id(array);
    }
    if (cursor->tail < cursor->curr_rec) { cursor->tail = cursor->curr_rec; }
  }

  if (*array->n_garbages) {
    while (offset && cursor->curr_rec != cursor->tail) {
      cursor->curr_rec += cursor->dir;
      if (grn_array_bitmap_at(ctx, cursor->array, cursor->curr_rec) == 1) {
        offset--;
      }
    }
  } else {
    cursor->curr_rec += cursor->dir * offset;
  }
  cursor->rest = (limit < 0) ? GRN_ARRAY_MAX : limit;
  return cursor;
}

grn_id
grn_array_cursor_next(grn_ctx *ctx, grn_array_cursor *cursor)
{
  if (cursor && cursor->rest) {
    while (cursor->curr_rec != cursor->tail) {
      cursor->curr_rec += cursor->dir;
      if (*cursor->array->n_garbages) {
        if (grn_array_bitmap_at(ctx, cursor->array, cursor->curr_rec) != 1) {
          continue;
        }
      }
      cursor->rest--;
      return cursor->curr_rec;
    }
  }
  return GRN_ID_NIL;
}

grn_id
grn_array_next(grn_ctx *ctx, grn_array *array, grn_id id)
{
  grn_id max_id;
  if (grn_array_error_if_truncated(ctx, array) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  max_id = grn_array_get_max_id(array);
  while (++id <= max_id) {
    if (!*array->n_garbages ||
        grn_array_bitmap_at(ctx, array, id) == 1) {
      return id;
    }
  }
  return GRN_ID_NIL;
}

int
grn_array_cursor_get_value(grn_ctx *ctx, grn_array_cursor *cursor, void **value)
{
  if (cursor && value) {
    void * const entry = grn_array_entry_at(ctx, cursor->array, cursor->curr_rec, 0);
    if (entry) {
      *value = entry;
      return cursor->array->value_size;
    }
  }
  return 0;
}

grn_rc
grn_array_cursor_set_value(grn_ctx *ctx, grn_array_cursor *cursor,
                           const void *value, int flags)
{
  return grn_array_set_value_inline(ctx, cursor->array, cursor->curr_rec,
                                    value, flags);
}

grn_rc
grn_array_cursor_delete(grn_ctx *ctx, grn_array_cursor *cursor,
                        grn_table_delete_optarg *optarg)
{
  return grn_array_delete_by_id(ctx, cursor->array, cursor->curr_rec, optarg);
}

inline static grn_id
grn_array_add_to_tiny_array(grn_ctx *ctx, grn_array *array, void **value)
{
  grn_id id = array->garbages;
  void *entry;
  if (id) {
    /* These operations fail iff the array is broken. */
    entry = grn_tiny_array_get(&array->array, id);
    if (!entry) {
      return GRN_ID_NIL;
    }
    array->garbages = *(grn_id *)entry;
    memset(entry, 0, array->value_size);
    (*array->n_garbages)--;
    if (!grn_tiny_bitmap_get_and_set(&array->bitmap, id, 1)) {
      /* Actually, it is difficult to recover from this error. */
      *(grn_id *)entry = array->garbages;
      array->garbages = id;
      (*array->n_garbages)++;
      return GRN_ID_NIL;
    }
  } else {
    id = array->array.max + 1;
    if (!grn_tiny_bitmap_put_and_set(&array->bitmap, id, 1)) {
      return GRN_ID_NIL;
    }
    entry = grn_tiny_array_put(&array->array, id);
    if (!entry) {
      grn_tiny_bitmap_get_and_set(&array->bitmap, id, 0);
      return GRN_ID_NIL;
    }
    array->array.max = id;
  }
  (*array->n_entries)++;
  if (value) { *value = entry; }
  return id;
}

inline static grn_id
grn_array_add_to_io_array(grn_ctx *ctx, grn_array *array, void **value)
{
  grn_id id;
  void *entry;
  struct grn_array_header *header;
  if (grn_array_error_if_truncated(ctx, array) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  header = array->header;
  id = header->garbages;
  if (id) {
    /* These operations fail iff the array is broken. */
    entry = grn_array_io_entry_at(ctx, array, id, GRN_TABLE_ADD);
    if (!entry) {
      return GRN_ID_NIL;
    }
    header->garbages = *(grn_id *)entry;
    memset(entry, 0, header->value_size);
    (*array->n_garbages)--;
    if (!grn_io_array_bit_on(ctx, array->io, GRN_ARRAY_BITMAP_SEGMENT, id)) {
      /* Actually, it is difficult to recover from this error. */
      *(grn_id *)entry = array->garbages;
      array->garbages = id;
      (*array->n_garbages)++;
      return GRN_ID_NIL;
    }
  } else {
    if (header->curr_rec >= GRN_ARRAY_MAX) { return GRN_ID_NIL; }
    id = header->curr_rec + 1;
    if (!grn_io_array_bit_on(ctx, array->io, GRN_ARRAY_BITMAP_SEGMENT, id)) {
      return GRN_ID_NIL;
    }
    entry = grn_array_io_entry_at(ctx, array, id, GRN_TABLE_ADD);
    if (!entry) {
      grn_io_array_bit_off(ctx, array->io, GRN_ARRAY_BITMAP_SEGMENT, id);
      return GRN_ID_NIL;
    }
    header->curr_rec = id;
  }
  (*array->n_entries)++;
  if (value) { *value = entry; }
  return id;
}

void
grn_array_clear_curr_rec(grn_ctx *ctx, grn_array *array)
{
  struct grn_array_header * const header = array->header;
  header->curr_rec = GRN_ID_NIL;
}

grn_id
grn_array_add(grn_ctx *ctx, grn_array *array, void **value)
{
  if (ctx && array) {
    if (grn_array_is_io_array(array)) {
      return grn_array_add_to_io_array(ctx, array, value);
    } else {
      return grn_array_add_to_tiny_array(ctx, array, value);
    }
  }
  return GRN_ID_NIL;
}

grn_id
grn_array_push(grn_ctx *ctx, grn_array *array,
               void (*func)(grn_ctx *, grn_array *, grn_id, void *),
               void *func_arg)
{
  grn_id id = GRN_ID_NIL;
  grn_table_queue *queue = grn_array_queue(ctx, array);
  if (queue) {
    MUTEX_LOCK(queue->mutex);
    if (grn_table_queue_head(queue) == queue->cap) {
      grn_array_clear_curr_rec(ctx, array);
    }
    id = grn_array_add(ctx, array, NULL);
    if (func) {
      func(ctx, array, id, func_arg);
    }
    if (grn_table_queue_size(queue) == queue->cap) {
      grn_table_queue_tail_increment(queue);
    }
    grn_table_queue_head_increment(queue);
    COND_SIGNAL(queue->cond);
    MUTEX_UNLOCK(queue->mutex);
  } else {
    ERR(GRN_OPERATION_NOT_SUPPORTED, "only persistent arrays support push");
  }
  return id;
}

grn_id
grn_array_pull(grn_ctx *ctx, grn_array *array, grn_bool blockp,
               void (*func)(grn_ctx *, grn_array *, grn_id, void *),
               void *func_arg)
{
  grn_id id = GRN_ID_NIL;
  grn_table_queue *queue = grn_array_queue(ctx, array);
  if (queue) {
    MUTEX_LOCK(queue->mutex);
    queue->unblock_requested = GRN_FALSE;
    while (grn_table_queue_size(queue) == 0) {
      if (!blockp || queue->unblock_requested) {
        MUTEX_UNLOCK(queue->mutex);
        GRN_OUTPUT_BOOL(0);
        return id;
      }
      COND_WAIT(queue->cond, queue->mutex);
    }
    grn_table_queue_tail_increment(queue);
    id = grn_table_queue_tail(queue);
    if (func) {
      func(ctx, array, id, func_arg);
    }
    MUTEX_UNLOCK(queue->mutex);
  } else {
    ERR(GRN_OPERATION_NOT_SUPPORTED, "only persistent arrays support pull");
  }
  return id;
}

void
grn_array_unblock(grn_ctx *ctx, grn_array *array)
{
  grn_table_queue *queue = grn_array_queue(ctx, array);
  if (!queue) {
    return;
  }

  queue->unblock_requested = GRN_TRUE;
  COND_BROADCAST(queue->cond);
}

/* grn_hash : hash table */

#define GRN_HASH_MAX_SEGMENT  0x400
#define GRN_HASH_HEADER_SIZE_NORMAL 0x9000
#define GRN_HASH_HEADER_SIZE_LARGE\
  (GRN_HASH_HEADER_SIZE_NORMAL +\
   (sizeof(grn_id) *\
    (GRN_HASH_MAX_KEY_SIZE_LARGE - GRN_HASH_MAX_KEY_SIZE_NORMAL)))
#define GRN_HASH_SEGMENT_SIZE 0x400000
#define GRN_HASH_KEY_MAX_N_SEGMENTS_NORMAL 0x400
#define GRN_HASH_KEY_MAX_N_SEGMENTS_LARGE 0x40000
#define W_OF_KEY_IN_A_SEGMENT 22
#define GRN_HASH_KEY_MAX_TOTAL_SIZE_NORMAL\
  (((uint64_t)(1) << W_OF_KEY_IN_A_SEGMENT) *\
   GRN_HASH_KEY_MAX_N_SEGMENTS_NORMAL - 1)
#define GRN_HASH_KEY_MAX_TOTAL_SIZE_LARGE\
  (((uint64_t)(1) << W_OF_KEY_IN_A_SEGMENT) *\
   GRN_HASH_KEY_MAX_N_SEGMENTS_LARGE - 1)
#define IDX_MASK_IN_A_SEGMENT 0xfffff

typedef struct {
  uint8_t key[4];
  uint8_t value[1];
} grn_plain_hash_entry;

typedef struct {
  uint32_t hash_value;
  uint8_t key_and_value[1];
} grn_rich_hash_entry;

typedef struct {
  uint32_t hash_value;
  uint16_t flag;
  uint16_t key_size;
  union {
    uint8_t buf[sizeof(uint32_t)];
    uint32_t offset;
  } key;
  uint8_t value[1];
} grn_io_hash_entry_normal;

typedef struct {
  uint32_t hash_value;
  uint16_t flag;
  uint16_t key_size;
  union {
    uint8_t buf[sizeof(uint64_t)];
    uint64_t offset;
  } key;
  uint8_t value[1];
} grn_io_hash_entry_large;

typedef struct {
  uint32_t hash_value;
  uint16_t flag;
  uint16_t key_size;
  union {
    uint8_t buf[sizeof(void *)];
    void *ptr;
  } key;
  uint8_t value[1];
} grn_tiny_hash_entry;

/*
 * hash_value is valid even if the entry is grn_plain_hash_entry. In this case,
 * its hash_value equals its key.
 * flag, key_size and key.buf are valid if the entry has a variable length key.
 */
typedef struct {
  uint32_t hash_value;
  uint16_t flag;
  uint16_t key_size;
} grn_hash_entry_header;

typedef union {
  uint32_t hash_value;
  grn_hash_entry_header header;
  grn_plain_hash_entry plain_entry;
  grn_rich_hash_entry rich_entry;
  grn_io_hash_entry_normal io_entry_normal;
  grn_io_hash_entry_large io_entry_large;
  grn_tiny_hash_entry tiny_entry;
} grn_hash_entry;

typedef struct {
  uint32_t key;
  uint8_t dummy[1];
} entry;

typedef struct {
  uint32_t key;
  uint16_t flag;
  uint16_t size;
  uint32_t str;
  uint8_t dummy[1];
} entry_str;

typedef struct {
  uint32_t key;
  uint16_t flag;
  uint16_t size;
  char *str;
  uint8_t dummy[1];
} entry_astr;

enum {
  GRN_HASH_KEY_SEGMENT    = 0,
  GRN_HASH_ENTRY_SEGMENT  = 1,
  GRN_HASH_INDEX_SEGMENT  = 2,
  GRN_HASH_BITMAP_SEGMENT = 3
};

inline static int
grn_hash_name(grn_ctx *ctx, grn_hash *hash, char *buffer, int buffer_size)
{
  int name_size;

  if (DB_OBJ(hash)->id == GRN_ID_NIL) {
    grn_strcpy(buffer, buffer_size, "(anonymous)");
    name_size = strlen(buffer);
  } else {
    name_size = grn_obj_name(ctx, (grn_obj *)hash, buffer, buffer_size);
  }

  return name_size;
}

inline static grn_bool
grn_hash_is_io_hash(grn_hash *hash)
{
  return hash->io != NULL;
}

inline static void *
grn_io_hash_entry_at(grn_ctx *ctx, grn_hash *hash, grn_id id, int flags)
{
  return grn_io_array_at_inline(ctx, hash->io, GRN_HASH_ENTRY_SEGMENT, id, flags);
}

/* todo : error handling */
inline static void *
grn_hash_entry_at(grn_ctx *ctx, grn_hash *hash, grn_id id, int flags)
{
  if (grn_hash_is_io_hash(hash)) {
    return grn_io_hash_entry_at(ctx, hash, id, flags);
  } else {
    return grn_tiny_array_at_inline(&hash->a, id);
  }
}

inline static grn_bool
grn_hash_bitmap_at(grn_ctx *ctx, grn_hash *hash, grn_id id)
{
  if (grn_hash_is_io_hash(hash)) {
    return grn_io_array_bit_at(ctx, hash->io, GRN_HASH_BITMAP_SEGMENT, id) == 1;
  } else {
    return grn_tiny_bitmap_put(&hash->bitmap, id) == 1;
  }
}

inline static grn_id *
grn_io_hash_idx_at(grn_ctx *ctx, grn_hash *hash, grn_id id)
{
  return grn_io_array_at_inline(ctx, hash->io, GRN_HASH_INDEX_SEGMENT,
                                id, GRN_TABLE_ADD);
}

inline static grn_id *
grn_hash_idx_at(grn_ctx *ctx, grn_hash *hash, grn_id id)
{
  if (grn_hash_is_io_hash(hash)) {
    id = (id & *hash->max_offset) + hash->header.common->idx_offset;
    return grn_io_hash_idx_at(ctx, hash, id);
  } else {
    return hash->index + (id & *hash->max_offset);
  }
}

inline static void *
grn_io_hash_key_at(grn_ctx *ctx, grn_hash *hash, uint64_t pos)
{
  return grn_io_array_at_inline(ctx, hash->io, GRN_HASH_KEY_SEGMENT,
                                pos, GRN_TABLE_ADD);
}

#define HASH_IMMEDIATE 1

#define MAX_INDEX_SIZE ((GRN_HASH_MAX_SEGMENT * (IDX_MASK_IN_A_SEGMENT + 1)) >> 1)

inline static uint16_t
grn_hash_entry_get_key_size(grn_hash *hash, grn_hash_entry *entry)
{
  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    return entry->header.key_size;
  } else {
    return hash->key_size;
  }
}

inline static char *
grn_hash_entry_get_key(grn_ctx *ctx, grn_hash *hash, grn_hash_entry *entry)
{
  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    if (grn_hash_is_io_hash(hash)) {
      if (grn_hash_is_large_total_key_size(ctx, hash)) {
        if (entry->io_entry_large.flag & HASH_IMMEDIATE) {
          return (char *)entry->io_entry_large.key.buf;
        } else {
          return (char *)grn_io_hash_key_at(ctx, hash,
                                            entry->io_entry_large.key.offset);
        }
      } else {
        if (entry->io_entry_normal.flag & HASH_IMMEDIATE) {
          return (char *)entry->io_entry_normal.key.buf;
        } else {
          return (char *)grn_io_hash_key_at(ctx, hash,
                                            entry->io_entry_normal.key.offset);
        }
      }
    } else {
      if (entry->tiny_entry.flag & HASH_IMMEDIATE) {
        return (char *)entry->tiny_entry.key.buf;
      } else {
        return entry->tiny_entry.key.ptr;
      }
    }
  } else {
    if (hash->key_size == sizeof(uint32_t)) {
      return (char *)entry->plain_entry.key;
    } else {
      return (char *)entry->rich_entry.key_and_value;
    }
  }
}

inline static void *
grn_hash_entry_get_value(grn_ctx *ctx, grn_hash *hash, grn_hash_entry *entry)
{
  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    if (grn_hash_is_io_hash(hash)) {
      if (grn_hash_is_large_total_key_size(ctx, hash)) {
        return entry->io_entry_large.value;
      } else {
        return entry->io_entry_normal.value;
      }
    } else {
      return entry->tiny_entry.value;
    }
  } else {
    if (hash->key_size == sizeof(uint32_t)) {
      return entry->plain_entry.value;
    } else {
      return entry->rich_entry.key_and_value + hash->key_size;
    }
  }
}

inline static grn_rc
grn_io_hash_entry_put_key(grn_ctx *ctx, grn_hash *hash,
                          grn_hash_entry *entry,
                          const void *key, unsigned int key_size)
{
  grn_bool is_large_mode;
  grn_bool key_exist;
  uint64_t key_offset;
  grn_io_hash_entry_normal *io_entry_normal = &(entry->io_entry_normal);
  grn_io_hash_entry_large *io_entry_large = &(entry->io_entry_large);

  is_large_mode = grn_hash_is_large_total_key_size(ctx, hash);

  if (is_large_mode) {
    key_exist = (io_entry_large->key_size > 0);
  } else {
    key_exist = (io_entry_normal->key_size > 0);
  }

  if (key_exist > 0) {
    if (is_large_mode) {
      key_offset = io_entry_large->key.offset;
    } else {
      key_offset = io_entry_normal->key.offset;
    }
  } else {
    uint64_t segment_id;
    grn_hash_header_common *header;
    uint64_t curr_key;
    uint64_t max_total_size;

    header = hash->header.common;
    if (key_size >= GRN_HASH_SEGMENT_SIZE) {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;
      name_size = grn_hash_name(ctx, hash, name, GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_INVALID_ARGUMENT,
          "[hash][key][put] too long key: <%.*s>: max=%u: key size=%u",
          name_size, name,
          GRN_HASH_SEGMENT_SIZE,
          key_size);
      return ctx->rc;
    }

    if (is_large_mode) {
      curr_key = header->curr_key_large;
      max_total_size = GRN_HASH_KEY_MAX_TOTAL_SIZE_LARGE;
    } else {
      curr_key = header->curr_key_normal;
      max_total_size = GRN_HASH_KEY_MAX_TOTAL_SIZE_NORMAL;
    }

    if (key_size > (max_total_size - curr_key)) {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;
      name_size = grn_hash_name(ctx, hash, name, GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_NOT_ENOUGH_SPACE,
          "[hash][key][put] total key size is over: <%.*s>: "
          "max=%" GRN_FMT_INT64U ": "
          "current=%" GRN_FMT_INT64U ": "
          "new key size=%u",
          name_size, name,
          max_total_size,
          curr_key,
          key_size);
      return ctx->rc;
    }
    key_offset = curr_key;
    segment_id = (key_offset + key_size) >> W_OF_KEY_IN_A_SEGMENT;
    if ((key_offset >> W_OF_KEY_IN_A_SEGMENT) != segment_id) {
      key_offset = segment_id << W_OF_KEY_IN_A_SEGMENT;
      if (is_large_mode) {
        header->curr_key_large = key_offset;
      } else {
        header->curr_key_normal = key_offset;
      }
    }
    if (is_large_mode) {
      header->curr_key_large += key_size;
      io_entry_large->key.offset = key_offset;
    } else {
      header->curr_key_normal += key_size;
      io_entry_normal->key.offset = key_offset;
    }
  }

  {
    void * const key_ptr = grn_io_hash_key_at(ctx, hash, key_offset);
    if (!key_ptr) {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;
      name_size = grn_hash_name(ctx, hash, name, GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[hash][key][put] failed to allocate for new key: <%.*s>: "
          "new offset:%" GRN_FMT_INT64U " "
          "key size:%u",
          name_size, name,
          key_offset,
          key_size);
      return ctx->rc;
    }
    grn_memcpy(key_ptr, key, key_size);
  }
  return GRN_SUCCESS;
}

inline static grn_rc
grn_hash_entry_put_key(grn_ctx *ctx, grn_hash *hash,
                       grn_hash_entry *entry, uint32_t hash_value,
                       const void *key, unsigned int key_size)
{
  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    if (grn_hash_is_io_hash(hash)) {
      grn_bool is_large_mode;
      uint8_t *buffer;
      size_t buffer_size;
      uint16_t flag;

      is_large_mode = grn_hash_is_large_total_key_size(ctx, hash);
      if (is_large_mode) {
        buffer = entry->io_entry_large.key.buf;
        buffer_size = sizeof(entry->io_entry_large.key.buf);
      } else {
        buffer = entry->io_entry_normal.key.buf;
        buffer_size = sizeof(entry->io_entry_normal.key.buf);
      }

      if (key_size <= buffer_size) {
        grn_memcpy(buffer, key, key_size);
        flag = HASH_IMMEDIATE;
      } else {
        const grn_rc rc =
          grn_io_hash_entry_put_key(ctx, hash, entry, key, key_size);
        if (rc) {
          return rc;
        }
        flag = 0;
      }

      if (is_large_mode) {
        entry->io_entry_large.flag = flag;
        entry->io_entry_large.hash_value = hash_value;
        entry->io_entry_large.key_size = key_size;
      } else {
        entry->io_entry_normal.flag = flag;
        entry->io_entry_normal.hash_value = hash_value;
        entry->io_entry_normal.key_size = key_size;
      }
    } else {
      if (key_size <= sizeof(entry->tiny_entry.key.buf)) {
        grn_memcpy(entry->tiny_entry.key.buf, key, key_size);
        entry->tiny_entry.flag = HASH_IMMEDIATE;
      } else {
        grn_ctx * const ctx = hash->ctx;
        entry->tiny_entry.key.ptr = GRN_CTX_ALLOC(ctx, key_size);
        if (!entry->tiny_entry.key.ptr) {
          return GRN_NO_MEMORY_AVAILABLE;
        }
        grn_memcpy(entry->tiny_entry.key.ptr, key, key_size);
        entry->tiny_entry.flag = 0;
      }
      entry->tiny_entry.hash_value = hash_value;
      entry->tiny_entry.key_size = key_size;
    }
  } else {
    if (hash->key_size == sizeof(uint32_t)) {
      *(uint32_t *)entry->plain_entry.key = hash_value;
    } else {
      entry->rich_entry.hash_value = hash_value;
      grn_memcpy(entry->rich_entry.key_and_value, key, key_size);
    }
  }
  return GRN_SUCCESS;
}

/*
 * grn_hash_entry_compare_key() returns GRN_TRUE if the entry key equals the
 * specified key, or GRN_FALSE otherwise.
 */
inline static grn_bool
grn_hash_entry_compare_key(grn_ctx *ctx, grn_hash *hash,
                           grn_hash_entry *entry, uint32_t hash_value,
                           const void *key, unsigned int key_size)
{
  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    if (entry->hash_value != hash_value ||
        entry->header.key_size != key_size) {
      return GRN_FALSE;
    }
    if (grn_hash_is_io_hash(hash)) {
      if (grn_hash_is_large_total_key_size(ctx, hash)) {
        if (entry->io_entry_large.flag & HASH_IMMEDIATE) {
          return !memcmp(key, entry->io_entry_large.key.buf, key_size);
        } else {
          const void * const entry_key_ptr =
              grn_io_hash_key_at(ctx, hash, entry->io_entry_large.key.offset);
          return !memcmp(key, entry_key_ptr, key_size);
        }
      } else {
        if (entry->io_entry_normal.flag & HASH_IMMEDIATE) {
          return !memcmp(key, entry->io_entry_normal.key.buf, key_size);
        } else {
          const void * const entry_key_ptr =
              grn_io_hash_key_at(ctx, hash, entry->io_entry_normal.key.offset);
          return !memcmp(key, entry_key_ptr, key_size);
        }
      }
    } else {
      if (entry->tiny_entry.flag & HASH_IMMEDIATE) {
        return !memcmp(key, entry->tiny_entry.key.buf, key_size);
      } else {
        return !memcmp(key, entry->tiny_entry.key.ptr, key_size);
      }
    }
  } else {
    if (entry->hash_value != hash_value) {
      return GRN_FALSE;
    }
    if (key_size == sizeof(uint32_t)) {
      return GRN_TRUE;
    } else {
      return !memcmp(key, entry->rich_entry.key_and_value, key_size);
    }
  }
}

inline static char *
get_key(grn_ctx *ctx, grn_hash *hash, entry_str *n)
{
  return grn_hash_entry_get_key(ctx, hash, (grn_hash_entry *)n);
}

inline static void *
get_value(grn_ctx *ctx, grn_hash *hash, entry_str *n)
{
  return grn_hash_entry_get_value(ctx, hash, (grn_hash_entry *)n);
}

inline static int
match_key(grn_ctx *ctx, grn_hash *hash, entry_str *ee, uint32_t h,
          const char *key, unsigned int len)
{
  return grn_hash_entry_compare_key(ctx, hash, (grn_hash_entry *)ee,
                                    h, key, len);
}

#define GARBAGE (0xffffffff)

inline static uint32_t
grn_io_hash_calculate_entry_size(uint32_t key_size, uint32_t value_size,
                                 uint32_t flags)
{
  if (flags & GRN_OBJ_KEY_VAR_SIZE) {
    if (flags & GRN_OBJ_KEY_LARGE) {
      return (uintptr_t)((grn_io_hash_entry_large *)0)->value + value_size;
    } else {
      return (uintptr_t)((grn_io_hash_entry_normal *)0)->value + value_size;
    }
  } else {
    if (key_size == sizeof(uint32_t)) {
      return (uintptr_t)((grn_plain_hash_entry *)0)->value + value_size;
    } else {
      return (uintptr_t)((grn_rich_hash_entry *)0)->key_and_value
          + key_size + value_size;
    }
  }
}

static grn_io *
grn_io_hash_create_io(grn_ctx *ctx, const char *path,
                      uint32_t header_size, uint32_t entry_size,
                      uint32_t flags)
{
  uint32_t w_of_element = 0;
  grn_io_array_spec array_spec[4];

  while ((1U << w_of_element) < entry_size) {
    w_of_element++;
  }

  array_spec[GRN_HASH_KEY_SEGMENT].w_of_element = 0;
  if (flags & GRN_OBJ_KEY_LARGE) {
    array_spec[GRN_HASH_KEY_SEGMENT].max_n_segments =
      GRN_HASH_KEY_MAX_N_SEGMENTS_LARGE;
  } else {
    array_spec[GRN_HASH_KEY_SEGMENT].max_n_segments =
      GRN_HASH_KEY_MAX_N_SEGMENTS_NORMAL;
  }
  array_spec[GRN_HASH_ENTRY_SEGMENT].w_of_element = w_of_element;
  array_spec[GRN_HASH_ENTRY_SEGMENT].max_n_segments =
      1U << (30 - (22 - w_of_element));
  array_spec[GRN_HASH_INDEX_SEGMENT].w_of_element = 2;
  array_spec[GRN_HASH_INDEX_SEGMENT].max_n_segments = 1U << (30 - (22 - 2));
  array_spec[GRN_HASH_BITMAP_SEGMENT].w_of_element = 0;
  array_spec[GRN_HASH_BITMAP_SEGMENT].max_n_segments = 1U << (30 - (22 + 3));
  return grn_io_create_with_array(ctx, path, header_size,
                                  GRN_HASH_SEGMENT_SIZE,
                                  grn_io_auto, 4, array_spec);
}

static grn_rc
grn_io_hash_init(grn_ctx *ctx, grn_hash *hash, const char *path,
                 uint32_t key_size, uint32_t value_size, uint32_t flags,
                 grn_encoding encoding, uint32_t init_size)
{
  grn_io *io;
  grn_hash_header_common *header;
  uint32_t header_size, entry_size, max_offset;

  if (key_size <= GRN_HASH_MAX_KEY_SIZE_NORMAL) {
    header_size = GRN_HASH_HEADER_SIZE_NORMAL;
  } else {
    header_size = GRN_HASH_HEADER_SIZE_LARGE;
  }
  entry_size = grn_io_hash_calculate_entry_size(key_size, value_size, flags);

  io = grn_io_hash_create_io(ctx, path, header_size, entry_size, flags);
  if (!io) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  grn_io_set_type(io, GRN_TABLE_HASH_KEY);

  max_offset = IDX_MASK_IN_A_SEGMENT + 1;
  while (max_offset < init_size * 2) {
    max_offset *= 2;
  }
  max_offset--;

  if (encoding == GRN_ENC_DEFAULT) {
    encoding = ctx->encoding;
  }

  hash->key_size = key_size;

  header = grn_io_header(io);
  header->flags = flags;
  header->encoding = encoding;
  header->key_size = key_size;
  header->curr_rec = 0;
  header->curr_key_normal = 0;
  header->curr_key_large = 0;
  header->lock = 0;
  header->idx_offset = 0;
  header->value_size = value_size;
  header->entry_size = entry_size;
  header->max_offset = max_offset;
  header->n_entries = 0;
  header->n_garbages = 0;
  header->tokenizer = GRN_ID_NIL;
  if (header->flags & GRN_OBJ_KEY_NORMALIZE) {
    header->flags &= ~GRN_OBJ_KEY_NORMALIZE;
    hash->normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
    header->normalizer = grn_obj_id(ctx, hash->normalizer);
  } else {
    hash->normalizer = NULL;
    header->normalizer = GRN_ID_NIL;
  }
  header->truncated = GRN_FALSE;
  GRN_PTR_INIT(&(hash->token_filters), GRN_OBJ_VECTOR, GRN_ID_NIL);
  {
    grn_table_queue *queue;
    if (GRN_HASH_IS_LARGE_KEY(hash)) {
      queue = &(((grn_hash_header_large *)(header))->queue);
    } else {
      queue = &(((grn_hash_header_normal *)(header))->queue);
    }
    grn_table_queue_init(ctx, queue);
  }

  hash->obj.header.flags = (header->flags & GRN_OBJ_FLAGS_MASK);
  hash->ctx = ctx;
  hash->encoding = encoding;
  hash->value_size = value_size;
  hash->entry_size = entry_size;
  hash->n_garbages = &header->n_garbages;
  hash->n_entries = &header->n_entries;
  hash->max_offset = &header->max_offset;
  hash->io = io;
  hash->header.common = header;
  hash->lock = &header->lock;
  hash->tokenizer = NULL;
  return GRN_SUCCESS;
}

#define INITIAL_INDEX_SIZE 256U

static uint32_t
grn_tiny_hash_calculate_entry_size(uint32_t key_size, uint32_t value_size,
                                   uint32_t flags)
{
  uint32_t entry_size;
  if (flags & GRN_OBJ_KEY_VAR_SIZE) {
    entry_size = (uintptr_t)((grn_tiny_hash_entry *)0)->value + value_size;
  } else {
    if (key_size == sizeof(uint32_t)) {
      entry_size = (uintptr_t)((grn_plain_hash_entry *)0)->value + value_size;
    } else {
      entry_size = (uintptr_t)((grn_rich_hash_entry *)0)->key_and_value
          + key_size + value_size;
    }
  }
  if (entry_size != sizeof(uint32_t)) {
    entry_size += sizeof(uintptr_t) - 1;
    entry_size &= ~(sizeof(uintptr_t) - 1);
  }
  return entry_size;
}

static grn_rc
grn_tiny_hash_init(grn_ctx *ctx, grn_hash *hash, const char *path,
                   uint32_t key_size, uint32_t value_size, uint32_t flags,
                   grn_encoding encoding)
{
  uint32_t entry_size;

  if (path) {
    return GRN_INVALID_ARGUMENT;
  }
  hash->index = GRN_CTX_ALLOC(ctx, INITIAL_INDEX_SIZE * sizeof(grn_id));
  if (!hash->index) {
    return GRN_NO_MEMORY_AVAILABLE;
  }

  entry_size = grn_tiny_hash_calculate_entry_size(key_size, value_size, flags);
  hash->obj.header.flags = flags;
  hash->ctx = ctx;
  hash->key_size = key_size;
  hash->encoding = encoding;
  hash->value_size = value_size;
  hash->entry_size = entry_size;
  hash->n_garbages = &hash->n_garbages_;
  hash->n_entries = &hash->n_entries_;
  hash->max_offset = &hash->max_offset_;
  hash->max_offset_ = INITIAL_INDEX_SIZE - 1;
  hash->io = NULL;
  hash->header.common = NULL;
  hash->n_garbages_ = 0;
  hash->n_entries_ = 0;
  hash->garbages = GRN_ID_NIL;
  hash->tokenizer = NULL;
  hash->normalizer = NULL;
  GRN_PTR_INIT(&(hash->token_filters), GRN_OBJ_VECTOR, GRN_ID_NIL);
  grn_tiny_array_init(ctx, &hash->a, entry_size, GRN_TINY_ARRAY_CLEAR);
  grn_tiny_bitmap_init(ctx, &hash->bitmap);
  return GRN_SUCCESS;
}

static grn_rc
grn_hash_init(grn_ctx *ctx, grn_hash *hash, const char *path,
              uint32_t key_size, uint32_t value_size, uint32_t flags)
{
  if (flags & GRN_HASH_TINY) {
    return grn_tiny_hash_init(ctx, hash, path, key_size, value_size,
                              flags, ctx->encoding);
  } else {
    return grn_io_hash_init(ctx, hash, path, key_size, value_size,
                            flags, ctx->encoding, 0);
  }
}

grn_hash *
grn_hash_create(grn_ctx *ctx, const char *path, uint32_t key_size, uint32_t value_size,
                uint32_t flags)
{
  grn_hash *hash;
  if (!ctx) {
    return NULL;
  }
  if (key_size > GRN_HASH_MAX_KEY_SIZE_LARGE) {
    return NULL;
  }
  hash = (grn_hash *)GRN_CALLOC(sizeof(grn_hash));
  if (!hash) {
    return NULL;
  }
  GRN_DB_OBJ_SET_TYPE(hash, GRN_TABLE_HASH_KEY);
  if (grn_hash_init(ctx, hash, path, key_size, value_size, flags)) {
    GRN_FREE(hash);
    return NULL;
  }
  return hash;
}

grn_hash *
grn_hash_open(grn_ctx *ctx, const char *path)
{
  if (ctx) {
    grn_io * const io = grn_io_open(ctx, path, grn_io_auto);
    if (io) {
      grn_hash_header_common * const header = grn_io_header(io);
      uint32_t io_type = grn_io_get_type(io);
      if (io_type == GRN_TABLE_HASH_KEY) {
        grn_hash * const hash = (grn_hash *)GRN_MALLOC(sizeof(grn_hash));
        if (hash) {
          if (!(header->flags & GRN_HASH_TINY)) {
            GRN_DB_OBJ_SET_TYPE(hash, GRN_TABLE_HASH_KEY);
            hash->ctx = ctx;
            hash->key_size = header->key_size;
            hash->encoding = header->encoding;
            hash->value_size = header->value_size;
            hash->entry_size = header->entry_size;
            hash->n_garbages = &header->n_garbages;
            hash->n_entries = &header->n_entries;
            hash->max_offset = &header->max_offset;
            hash->io = io;
            hash->header.common = header;
            hash->lock = &header->lock;
            hash->tokenizer = grn_ctx_at(ctx, header->tokenizer);
            if (header->flags & GRN_OBJ_KEY_NORMALIZE) {
              header->flags &= ~GRN_OBJ_KEY_NORMALIZE;
              hash->normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
              header->normalizer = grn_obj_id(ctx, hash->normalizer);
            } else {
              hash->normalizer = grn_ctx_at(ctx, header->normalizer);
            }
            GRN_PTR_INIT(&(hash->token_filters), GRN_OBJ_VECTOR, GRN_ID_NIL);
            hash->obj.header.flags = header->flags;
            return hash;
          } else {
            GRN_LOG(ctx, GRN_LOG_NOTICE,
                    "invalid hash flag. (%x)", header->flags);
          }
          GRN_FREE(hash);
        }
      } else {
        ERR(GRN_INVALID_FORMAT,
            "[table][hash] file type must be %#04x: <%#04x>",
            GRN_TABLE_HASH_KEY, io_type);
      }
      grn_io_close(ctx, io);
    }
  }
  return NULL;
}

/*
 * grn_hash_error_if_truncated() logs an error and returns its error code if
 * a hash is truncated by another process.
 * Otherwise, this function returns GRN_SUCCESS.
 * Note that `ctx` and `hash` must be valid.
 *
 * FIXME: A hash should be reopened if possible.
 */
static grn_rc
grn_hash_error_if_truncated(grn_ctx *ctx, grn_hash *hash)
{
  if (hash->header.common && hash->header.common->truncated) {
    ERR(GRN_FILE_CORRUPT,
        "hash is truncated, please unmap or reopen the database");
    return GRN_FILE_CORRUPT;
  }
  return GRN_SUCCESS;
}

static grn_rc
grn_tiny_hash_fin(grn_ctx *ctx, grn_hash *hash)
{
  if (!hash->index) {
    return GRN_INVALID_ARGUMENT;
  }

  GRN_OBJ_FIN(ctx, &(hash->token_filters));

  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    uint32_t num_remaining_entries = *hash->n_entries;
    grn_id *hash_ptr;
    for (hash_ptr = hash->index; num_remaining_entries; hash_ptr++) {
      const grn_id id = *hash_ptr;
      if (id && id != GARBAGE) {
        grn_tiny_hash_entry * const entry =
            (grn_tiny_hash_entry *)grn_tiny_array_get(&hash->a, id);
        GRN_ASSERT(entry);
        num_remaining_entries--;
        if (entry && !(entry->flag & HASH_IMMEDIATE)) {
          GRN_CTX_FREE(ctx, entry->key.ptr);
        }
      }
    }
  }
  grn_tiny_array_fin(&hash->a);
  grn_tiny_bitmap_fin(&hash->bitmap);
  GRN_CTX_FREE(ctx, hash->index);
  return GRN_SUCCESS;
}

grn_rc
grn_hash_close(grn_ctx *ctx, grn_hash *hash)
{
  grn_rc rc;
  if (!ctx || !hash) { return GRN_INVALID_ARGUMENT; }
  if (grn_hash_is_io_hash(hash)) {
    rc = grn_io_close(ctx, hash->io);
    GRN_OBJ_FIN(ctx, &(hash->token_filters));
  } else {
    GRN_ASSERT(ctx == hash->ctx);
    rc = grn_tiny_hash_fin(ctx, hash);
  }
  GRN_FREE(hash);
  return rc;
}

grn_rc
grn_hash_remove(grn_ctx *ctx, const char *path)
{
  if (!ctx || !path) { return GRN_INVALID_ARGUMENT; }
  return grn_io_remove(ctx, path);
}

grn_rc
grn_hash_truncate(grn_ctx *ctx, grn_hash *hash)
{
  grn_rc rc;
  char *path = NULL;
  uint32_t key_size, value_size, flags;

  if (!ctx || !hash) {
    return GRN_INVALID_ARGUMENT;
  }
  rc = grn_hash_error_if_truncated(ctx, hash);
  if (rc != GRN_SUCCESS) {
    return rc;
  }

  if (grn_hash_is_io_hash(hash)) {
    const char * const io_path = grn_io_path(hash->io);
    if (io_path && *io_path) {
      path = GRN_STRDUP(io_path);
      if (!path) {
        ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_path);
        return GRN_NO_MEMORY_AVAILABLE;
      }
    }
  }
  key_size = hash->key_size;
  value_size = hash->value_size;
  flags = hash->obj.header.flags;

  if (grn_hash_is_io_hash(hash)) {
    if (path) {
      /* Only an I/O hash with a valid path uses the `truncated` flag. */
      hash->header.common->truncated = GRN_TRUE;
    }
    rc = grn_io_close(ctx, hash->io);
    if (!rc) {
      hash->io = NULL;
      if (path) {
        rc = grn_io_remove(ctx, path);
      }
    }
    GRN_OBJ_FIN(ctx, &(hash->token_filters));
  }
  if (!rc) {
    rc = grn_hash_init(ctx, hash, path, key_size, value_size, flags);
  }
  if (path) {
    GRN_FREE(path);
  }
  return rc;
}

inline static uint32_t
grn_hash_calculate_hash_value(const void *ptr, uint32_t size)
{
  uint32_t i;
  uint32_t hash_value = 0;
  for (i = 0; i < size; i++) {
    hash_value = (hash_value * 1021) + ((const uint8_t *)ptr)[i];
  }
  return hash_value;
}

inline static uint32_t
grn_hash_calculate_step(uint32_t hash_value)
{
  return (hash_value >> 2) | 0x1010101;
}

static grn_rc
grn_hash_reset(grn_ctx *ctx, grn_hash *hash, uint32_t expected_n_entries)
{
  grn_id *new_index = NULL;
  uint32_t new_index_size = INITIAL_INDEX_SIZE;
  grn_id *src_ptr = NULL, *dest_ptr = NULL;
  uint32_t src_offset = 0, dest_offset = 0;
  const uint32_t n_entries = *hash->n_entries;
  const uint32_t max_offset = *hash->max_offset;

  if (!expected_n_entries) {
    expected_n_entries = n_entries * 2;
  }
  if (expected_n_entries > INT_MAX) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  while (new_index_size <= expected_n_entries) {
    new_index_size *= 2;
  }

  if (grn_hash_is_io_hash(hash)) {
    uint32_t i;
    src_offset = hash->header.common->idx_offset;
    dest_offset = MAX_INDEX_SIZE - src_offset;
    for (i = 0; i < new_index_size; i += (IDX_MASK_IN_A_SEGMENT + 1)) {
      /*
       * The following grn_io_hash_idx_at() allocates memory for a new segment
       * and returns a pointer to the new segment. It's actually bad manners
       * but faster than calling grn_io_hash_idx_at() for each element.
       */
      dest_ptr = grn_io_hash_idx_at(ctx, hash, i + dest_offset);
      if (!dest_ptr) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      memset(dest_ptr, 0, GRN_HASH_SEGMENT_SIZE);
    }
  } else {
    GRN_ASSERT(ctx == hash->ctx);
    new_index = GRN_CTX_ALLOC(ctx, new_index_size * sizeof(grn_id));
    if (!new_index) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    src_ptr = hash->index;
  }

  {
    uint32_t src_pos, count;
    const uint32_t new_max_offset = new_index_size - 1;
    for (count = 0, src_pos = 0; count < n_entries && src_pos <= max_offset;
         src_pos++, src_ptr++) {
      uint32_t i, step;
      grn_id entry_id;
      grn_hash_entry *entry;
      if (grn_hash_is_io_hash(hash) && !(src_pos & IDX_MASK_IN_A_SEGMENT)) {
        src_ptr = grn_io_hash_idx_at(ctx, hash, src_pos + src_offset);
        if (!src_ptr) {
          return GRN_NO_MEMORY_AVAILABLE;
        }
      }
      entry_id = *src_ptr;
      if (!entry_id || (entry_id == GARBAGE)) {
        continue;
      }
      entry = grn_hash_entry_at(ctx, hash, entry_id, GRN_TABLE_ADD);
      if (!entry) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      step = grn_hash_calculate_step(entry->hash_value);
      for (i = entry->hash_value; ; i += step) {
        i &= new_max_offset;
        if (grn_hash_is_io_hash(hash)) {
          dest_ptr = grn_io_hash_idx_at(ctx, hash, i + dest_offset);
          if (!dest_ptr) {
            return GRN_NO_MEMORY_AVAILABLE;
          }
        } else {
          dest_ptr = new_index + i;
        }
        if (!*dest_ptr) {
          break;
        }
      }
      *dest_ptr = entry_id;
      count++;
    }
    *hash->max_offset = new_max_offset;
    *hash->n_garbages = 0;
  }

  if (grn_hash_is_io_hash(hash)) {
    hash->header.common->idx_offset = dest_offset;
  } else {
    grn_id * const old_index = hash->index;
    hash->index = new_index;
    GRN_CTX_FREE(ctx, old_index);
  }

  return GRN_SUCCESS;
}

grn_rc
grn_hash_lock(grn_ctx *ctx, grn_hash *hash, int timeout)
{
  static int _ncalls = 0, _ncolls = 0;
  uint32_t count;
  _ncalls++;
  for (count = 0;; count++) {
    uint32_t lock;
    GRN_ATOMIC_ADD_EX(hash->lock, 1, lock);
    if (lock) {
      GRN_ATOMIC_ADD_EX(hash->lock, -1, lock);
      if (!timeout || (timeout > 0 && (uint32_t) timeout == count)) { break; }
      if (!(++_ncolls % 1000000) && (_ncolls > _ncalls)) {
        if (_ncolls < 0 || _ncalls < 0) {
          _ncolls = 0; _ncalls = 0;
        } else {
          GRN_LOG(ctx, GRN_LOG_NOTICE, "hash(%p) collisions(%d/%d)", hash, _ncolls, _ncalls);
        }
      }
      grn_nanosleep(GRN_LOCK_WAIT_TIME_NANOSECOND);
      continue;
    }
    return GRN_SUCCESS;
  }
  ERR(GRN_RESOURCE_DEADLOCK_AVOIDED, "grn_hash_lock");
  return ctx->rc;
}

grn_rc
grn_hash_unlock(grn_ctx *ctx, grn_hash *hash)
{
  uint32_t lock;
  GRN_ATOMIC_ADD_EX(hash->lock, -1, lock);
  return GRN_SUCCESS;
}

grn_rc
grn_hash_clear_lock(grn_ctx *ctx, grn_hash *hash)
{
  *hash->lock = 0;
  return GRN_SUCCESS;
}

uint32_t
grn_hash_size(grn_ctx *ctx, grn_hash *hash)
{
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return 0;
  }
  return *hash->n_entries;
}

inline static grn_id
grn_io_hash_add(grn_ctx *ctx, grn_hash *hash, uint32_t hash_value,
                const void *key, unsigned int key_size, void **value)
{
  grn_id entry_id;
  grn_hash_entry *entry;
  grn_hash_header_common * const header = hash->header.common;
  grn_id *garbages;

  if (GRN_HASH_IS_LARGE_KEY(hash)) {
    garbages = hash->header.large->garbages;
  } else {
    garbages = hash->header.normal->garbages;
  }

  entry_id = garbages[key_size - 1];
  if (entry_id) {
    entry = grn_io_hash_entry_at(ctx, hash, entry_id, GRN_TABLE_ADD);
    if (!entry) {
      return GRN_ID_NIL;
    }
    garbages[key_size - 1] = *(grn_id *)entry;
    if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
      /* keep entry->io_entry's hash_value, flag, key_size and key. */
      if (grn_hash_is_large_total_key_size(ctx, hash)) {
        memset(entry->io_entry_large.value, 0, header->value_size);
      } else {
        memset(entry->io_entry_normal.value, 0, header->value_size);
      }
    } else {
      memset(entry, 0, header->entry_size);
    }
  } else {
    entry_id = header->curr_rec + 1;
    entry = grn_hash_entry_at(ctx, hash, entry_id, GRN_TABLE_ADD);
    if (!entry) {
      return GRN_ID_NIL;
    }
    header->curr_rec = entry_id;
  }

  if (!grn_io_array_bit_on(ctx, hash->io, GRN_HASH_BITMAP_SEGMENT, entry_id)) {
    /* TODO: error handling. */
  }

  if (grn_hash_entry_put_key(ctx, hash, entry, hash_value, key, key_size)) {
    grn_hash_delete_by_id(ctx, hash, entry_id, NULL);
    return GRN_ID_NIL;
  }

  if (value) {
    *value = grn_hash_entry_get_value(ctx, hash, entry);
  }
  return entry_id;
}

inline static grn_id
grn_tiny_hash_add(grn_ctx *ctx, grn_hash *hash, uint32_t hash_value,
                  const void *key, unsigned int key_size, void **value)
{
  grn_id entry_id;
  grn_hash_entry *entry;
  if (hash->garbages) {
    entry_id = hash->garbages;
    entry = (grn_hash_entry *)grn_tiny_array_get(&hash->a, entry_id);
    hash->garbages = *(grn_id *)entry;
    memset(entry, 0, hash->entry_size);
  } else {
    entry_id = hash->a.max + 1;
    entry = (grn_hash_entry *)grn_tiny_array_put(&hash->a, entry_id);
    if (!entry) {
      return GRN_ID_NIL;
    }
  }

  if (!grn_tiny_bitmap_put_and_set(&hash->bitmap, entry_id, 1)) {
    /* TODO: error handling. */
  }

  if (grn_hash_entry_put_key(ctx, hash, entry, hash_value, key, key_size)) {
    /* TODO: error handling. */
  }

  if (value) {
    *value = grn_hash_entry_get_value(ctx, hash, entry);
  }
  return entry_id;
}

grn_id
grn_hash_add(grn_ctx *ctx, grn_hash *hash, const void *key,
             unsigned int key_size, void **value, int *added)
{
  uint32_t hash_value;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  if (!key || !key_size) {
    return GRN_ID_NIL;
  }
  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    if (key_size > hash->key_size) {
      ERR(GRN_INVALID_ARGUMENT, "too long key");
      return GRN_ID_NIL;
    }
    hash_value = grn_hash_calculate_hash_value(key, key_size);
  } else {
    if (key_size != hash->key_size) {
      ERR(GRN_INVALID_ARGUMENT, "key size unmatch");
      return GRN_ID_NIL;
    }
    if (key_size == sizeof(uint32_t)) {
      hash_value = *((uint32_t *)key);
    } else {
      hash_value = grn_hash_calculate_hash_value(key, key_size);
    }
  }

  {
    uint32_t i;
    const uint32_t step = grn_hash_calculate_step(hash_value);
    grn_id id, *index, *garbage_index = NULL;
    grn_hash_entry *entry;

    /* lock */
    if ((*hash->n_entries + *hash->n_garbages) * 2 > *hash->max_offset) {
      if (*hash->max_offset > (1 << 29)) {
        ERR(GRN_TOO_LARGE_OFFSET, "hash table size limit");
        return GRN_ID_NIL;
      }
      grn_hash_reset(ctx, hash, 0);
    }

    for (i = hash_value; ; i += step) {
      index = grn_hash_idx_at(ctx, hash, i);
      if (!index) {
        return GRN_ID_NIL;
      }
      id = *index;
      if (!id) {
        break;
      }
      if (id == GARBAGE) {
        if (!garbage_index) {
          garbage_index = index;
        }
        continue;
      }

      entry = grn_hash_entry_at(ctx, hash, id, GRN_TABLE_ADD);
      if (!entry) {
        return GRN_ID_NIL;
      }
      if (grn_hash_entry_compare_key(ctx, hash, entry, hash_value,
                                     key, key_size)) {
        if (value) {
          *value = grn_hash_entry_get_value(ctx, hash, entry);
        }
        if (added) {
          *added = 0;
        }
        return id;
      }
    }

    if (grn_hash_is_io_hash(hash)) {
      id = grn_io_hash_add(ctx, hash, hash_value, key, key_size, value);
    } else {
      id = grn_tiny_hash_add(ctx, hash, hash_value, key, key_size, value);
    }
    if (!id) {
      return GRN_ID_NIL;
    }
    if (garbage_index) {
      (*hash->n_garbages)--;
      index = garbage_index;
    }
    *index = id;
    (*hash->n_entries)++;
    /* unlock */

    if (added) {
      *added = 1;
    }
    return id;
  }
}

grn_id
grn_hash_get(grn_ctx *ctx, grn_hash *hash, const void *key,
             unsigned int key_size, void **value)
{
  uint32_t hash_value;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    if (key_size > hash->key_size) {
      return GRN_ID_NIL;
    }
    hash_value = grn_hash_calculate_hash_value(key, key_size);
  } else {
    if (key_size != hash->key_size) {
      return GRN_ID_NIL;
    }
    if (key_size == sizeof(uint32_t)) {
      hash_value = *((uint32_t *)key);
    } else {
      hash_value = grn_hash_calculate_hash_value(key, key_size);
    }
  }

  {
    uint32_t i;
    const uint32_t step = grn_hash_calculate_step(hash_value);
    for (i = hash_value; ; i += step) {
      grn_id id;
      grn_id * const index = grn_hash_idx_at(ctx, hash, i);
      if (!index) {
        return GRN_ID_NIL;
      }
      id = *index;
      if (!id) {
        return GRN_ID_NIL;
      }
      if (id != GARBAGE) {
        grn_hash_entry * const entry = grn_hash_entry_at(ctx, hash, id, 0);
        if (entry) {
          if (grn_hash_entry_compare_key(ctx, hash, entry, hash_value,
                                         key, key_size)) {
            if (value) {
              *value = grn_hash_entry_get_value(ctx, hash, entry);
            }
            return id;
          }
        }
      }
    }
  }
}

inline static grn_hash_entry *
grn_hash_get_entry(grn_ctx *ctx, grn_hash *hash, grn_id id)
{
  if (!grn_hash_bitmap_at(ctx, hash, id)) {
    return NULL;
  }
  return grn_hash_entry_at(ctx, hash, id, 0);
}

const char *
_grn_hash_key(grn_ctx *ctx, grn_hash *hash, grn_id id, uint32_t *key_size)
{
  grn_hash_entry * const entry = grn_hash_get_entry(ctx, hash, id);
  if (!entry) {
    *key_size = 0;
    return NULL;
  }
  *key_size = grn_hash_entry_get_key_size(hash, entry);
  return grn_hash_entry_get_key(ctx, hash, entry);
}

int
grn_hash_get_key(grn_ctx *ctx, grn_hash *hash, grn_id id, void *keybuf, int bufsize)
{
  int key_size;
  grn_hash_entry *entry;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return 0;
  }
  entry = grn_hash_get_entry(ctx, hash, id);
  if (!entry) {
    return 0;
  }
  key_size = grn_hash_entry_get_key_size(hash, entry);
  if (bufsize >= key_size) {
    grn_memcpy(keybuf, grn_hash_entry_get_key(ctx, hash, entry), key_size);
  }
  return key_size;
}

int
grn_hash_get_key2(grn_ctx *ctx, grn_hash *hash, grn_id id, grn_obj *bulk)
{
  int key_size;
  char *key;
  grn_hash_entry *entry;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return 0;
  }
  entry = grn_hash_get_entry(ctx, hash, id);
  if (!entry) {
    return 0;
  }
  key_size = grn_hash_entry_get_key_size(hash, entry);
  key = grn_hash_entry_get_key(ctx, hash, entry);
  if (bulk->header.impl_flags & GRN_OBJ_REFER) {
    bulk->u.b.head = key;
    bulk->u.b.curr = key + key_size;
  } else {
    grn_bulk_write(ctx, bulk, key, key_size);
  }
  return key_size;
}

int
grn_hash_get_value(grn_ctx *ctx, grn_hash *hash, grn_id id, void *valuebuf)
{
  void *value;
  grn_hash_entry *entry;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return 0;
  }
  entry = grn_hash_get_entry(ctx, hash, id);
  if (!entry) {
    return 0;
  }
  value = grn_hash_entry_get_value(ctx, hash, entry);
  if (!value) {
    return 0;
  }
  if (valuebuf) {
    grn_memcpy(valuebuf, value, hash->value_size);
  }
  return hash->value_size;
}

const char *
grn_hash_get_value_(grn_ctx *ctx, grn_hash *hash, grn_id id, uint32_t *size)
{
  const void *value;
  grn_hash_entry *entry;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return NULL;
  }
  entry = grn_hash_get_entry(ctx, hash, id);
  if (!entry) {
    return NULL;
  }
  value = grn_hash_entry_get_value(ctx, hash, entry);
  if (!value) {
    return NULL;
  }
  if (size) {
    *size = hash->value_size;
  }
  return (const char *)value;
}

int
grn_hash_get_key_value(grn_ctx *ctx, grn_hash *hash, grn_id id,
                       void *keybuf, int bufsize, void *valuebuf)
{
  void *value;
  int key_size;
  grn_hash_entry *entry;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return 0;
  }
  entry = grn_hash_get_entry(ctx, hash, id);
  if (!entry) {
    return 0;
  }
  key_size = grn_hash_entry_get_key_size(hash, entry);
  if (bufsize >= key_size) {
    grn_memcpy(keybuf, grn_hash_entry_get_key(ctx, hash, entry), key_size);
  }
  value = grn_hash_entry_get_value(ctx, hash, entry);
  if (!value) {
    return 0;
  }
  if (valuebuf) {
    grn_memcpy(valuebuf, value, hash->value_size);
  }
  return key_size;
}

int
_grn_hash_get_key_value(grn_ctx *ctx, grn_hash *hash, grn_id id,
                        void **key, void **value)
{
  int key_size;
  grn_hash_entry *entry;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return 0;
  }
  entry = grn_hash_get_entry(ctx, hash, id);
  if (!entry) {
    return 0;
  }
  key_size = grn_hash_entry_get_key_size(hash, entry);
  *key = grn_hash_entry_get_key(ctx, hash, entry);
  *value = grn_hash_entry_get_value(ctx, hash, entry);
  return *value ? key_size : 0;
}

grn_rc
grn_hash_set_value(grn_ctx *ctx, grn_hash *hash, grn_id id,
                   const void *value, int flags)
{
  void *entry_value;
  grn_hash_entry *entry;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  if (!value) {
    return GRN_INVALID_ARGUMENT;
  }
  entry = grn_hash_get_entry(ctx, hash, id);
  if (!entry) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  entry_value = grn_hash_entry_get_value(ctx, hash, entry);
  if (!entry_value) {
    return GRN_NO_MEMORY_AVAILABLE;
  }

  switch (flags & GRN_OBJ_SET_MASK) {
  case GRN_OBJ_SET :
    grn_memcpy(entry_value, value, hash->value_size);
    return GRN_SUCCESS;
  case GRN_OBJ_INCR :
    switch (hash->value_size) {
    case sizeof(int32_t) :
      *((int32_t *)entry_value) += *((int32_t *)value);
      return GRN_SUCCESS;
    case sizeof(int64_t) :
      *((int64_t *)entry_value) += *((int64_t *)value);
      return GRN_SUCCESS;
    default :
      return GRN_INVALID_ARGUMENT;
    }
    break;
  case GRN_OBJ_DECR :
    switch (hash->value_size) {
    case sizeof(int32_t) :
      *((int32_t *)entry_value) -= *((int32_t *)value);
      return GRN_SUCCESS;
    case sizeof(int64_t) :
      *((int64_t *)entry_value) -= *((int64_t *)value);
      return GRN_SUCCESS;
    default :
      return GRN_INVALID_ARGUMENT;
    }
    break;
  default :
    ERR(GRN_INVALID_ARGUMENT, "flags = %d", flags);
    return ctx->rc;
  }
}

#define DELETE_IT do {\
  *ep = GARBAGE;\
  if (grn_hash_is_io_hash(hash)) {\
    uint32_t size = key_size - 1;\
    grn_id *garbages;\
    if (GRN_HASH_IS_LARGE_KEY(hash)) {\
      garbages = hash->header.large->garbages;\
    } else {\
      garbages = hash->header.normal->garbages;\
    }\
    ee->key = garbages[size];\
    garbages[size] = e;\
    grn_io_array_bit_off(ctx, hash->io, GRN_HASH_BITMAP_SEGMENT, e);\
  } else {\
    ee->key = hash->garbages;\
    hash->garbages = e;\
    if ((hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) && !(ee->flag & HASH_IMMEDIATE)) {\
      grn_ctx *ctx = hash->ctx;\
      GRN_CTX_FREE(ctx, ((entry_astr *)ee)->str);\
    }\
    grn_tiny_bitmap_get_and_set(&hash->bitmap, e, 0);\
  }\
  (*hash->n_entries)--;\
  (*hash->n_garbages)++;\
  rc = GRN_SUCCESS;\
} while (0)

grn_rc
grn_hash_delete_by_id(grn_ctx *ctx, grn_hash *hash, grn_id id,
                      grn_table_delete_optarg *optarg)
{
  entry_str *ee;
  grn_rc rc;
  if (!hash || !id) { return GRN_INVALID_ARGUMENT; }
  rc = grn_hash_error_if_truncated(ctx, hash);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = GRN_INVALID_ARGUMENT;
  /* lock */
  ee = grn_hash_entry_at(ctx, hash, id, 0);
  if (ee) {
    grn_id e, *ep;
    uint32_t i, key_size, h = ee->key, s = grn_hash_calculate_step(h);
    key_size = (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) ? ee->size : hash->key_size;
    for (i = h; ; i += s) {
      if (!(ep = grn_hash_idx_at(ctx, hash, i))) { return GRN_NO_MEMORY_AVAILABLE; }
      if (!(e = *ep)) { break; }
      if (e == id) {
        DELETE_IT;
        break;
      }
    }
  }
  /* unlock */
  return rc;
}

grn_rc
grn_hash_delete(grn_ctx *ctx, grn_hash *hash, const void *key, uint32_t key_size,
                grn_table_delete_optarg *optarg)
{
  uint32_t h, i, m, s;
  grn_rc rc = grn_hash_error_if_truncated(ctx, hash);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  rc = GRN_INVALID_ARGUMENT;
  if (hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) {
    if (key_size > hash->key_size) { return GRN_INVALID_ARGUMENT; }
    h = grn_hash_calculate_hash_value(key, key_size);
  } else {
    if (key_size != hash->key_size) { return GRN_INVALID_ARGUMENT; }
    if (key_size == sizeof(uint32_t)) {
      h = *((uint32_t *)key);
    } else {
      h = grn_hash_calculate_hash_value(key, key_size);
    }
  }
  s = grn_hash_calculate_step(h);
  {
    grn_id e, *ep;
    /* lock */
    m = *hash->max_offset;
    for (i = h; ; i += s) {
      if (!(ep = grn_hash_idx_at(ctx, hash, i))) { return GRN_NO_MEMORY_AVAILABLE; }
      if (!(e = *ep)) { break; }
      if (e == GARBAGE) { continue; }
      {
        entry_str * const ee = grn_hash_entry_at(ctx, hash, e, 0);
        if (ee && match_key(ctx, hash, ee, h, key, key_size)) {
          DELETE_IT;
          break;
        }
      }
    }
    /* unlock */
    return rc;
  }
}

/* only valid for hash tables, GRN_OBJ_KEY_VAR_SIZE && GRN_HASH_TINY */
const char *
_grn_hash_strkey_by_val(void *v, uint16_t *size)
{
  entry_astr *n = (entry_astr *)((uintptr_t)v -
                                 (uintptr_t)&((entry_astr *)0)->dummy);
  *size = n->size;
  return (n->flag & HASH_IMMEDIATE) ? (char *)&n->str : n->str;
}

void
grn_hash_cursor_close(grn_ctx *ctx, grn_hash_cursor *c)
{
  GRN_ASSERT(c->ctx == ctx);
  GRN_FREE(c);
}

#define HASH_CURR_MAX(hash) \
  ((grn_hash_is_io_hash(hash)) ? (hash)->header.common->curr_rec : (hash)->a.max)

grn_hash_cursor *
grn_hash_cursor_open(grn_ctx *ctx, grn_hash *hash,
                     const void *min, uint32_t min_size,
                     const void *max, uint32_t max_size,
                     int offset, int limit, int flags)
{
  grn_hash_cursor *c;
  if (!hash || !ctx) { return NULL; }
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return NULL;
  }
  if (!(c = GRN_MALLOCN(grn_hash_cursor, 1))) { return NULL; }
  GRN_DB_OBJ_SET_TYPE(c, GRN_CURSOR_TABLE_HASH_KEY);
  c->hash = hash;
  c->ctx = ctx;
  c->obj.header.flags = flags;
  c->obj.header.domain = GRN_ID_NIL;
  if (flags & GRN_CURSOR_DESCENDING) {
    c->dir = -1;
    if (max) {
      if (!(c->curr_rec = grn_hash_get(ctx, hash, max, max_size, NULL))) {
        c->tail = GRN_ID_NIL;
        goto exit;
      }
      if (!(flags & GRN_CURSOR_LT)) { c->curr_rec++; }
    } else {
      c->curr_rec = HASH_CURR_MAX(hash) + 1;
    }
    if (min) {
      if (!(c->tail = grn_hash_get(ctx, hash, min, min_size, NULL))) {
        c->curr_rec = GRN_ID_NIL;
        goto exit;
      }
      if ((flags & GRN_CURSOR_GT)) { c->tail++; }
    } else {
      c->tail = GRN_ID_NIL + 1;
    }
    if (c->curr_rec < c->tail) { c->tail = c->curr_rec; }
  } else {
    c->dir = 1;
    if (min) {
      if (!(c->curr_rec = grn_hash_get(ctx, hash, min, min_size, NULL))) {
        c->tail = GRN_ID_NIL;
        goto exit;
      }
      if (!(flags & GRN_CURSOR_GT)) { c->curr_rec--; }
    } else {
      c->curr_rec = GRN_ID_NIL;
    }
    if (max) {
      if (!(c->tail = grn_hash_get(ctx, hash, max, max_size, NULL))) {
        c->curr_rec = GRN_ID_NIL;
        goto exit;
      }
      if ((flags & GRN_CURSOR_LT)) { c->tail--; }
    } else {
      c->tail = HASH_CURR_MAX(hash);
    }
    if (c->tail < c->curr_rec) { c->tail = c->curr_rec; }
  }
  if (*hash->n_entries != HASH_CURR_MAX(hash)) {
    while (offset && c->curr_rec != c->tail) {
      c->curr_rec += c->dir;
      if (grn_hash_bitmap_at(ctx, c->hash, c->curr_rec)) { offset--; }
    }
  } else {
    c->curr_rec += c->dir * offset;
  }
exit :
  c->rest = (limit < 0) ? GRN_ARRAY_MAX : limit;
  return c;
}

grn_id
grn_hash_cursor_next(grn_ctx *ctx, grn_hash_cursor *c)
{
  if (c && c->rest) {
    while (c->curr_rec != c->tail) {
      c->curr_rec += c->dir;
      if (*c->hash->n_entries != HASH_CURR_MAX(c->hash)) {
        if (!grn_hash_bitmap_at(ctx, c->hash, c->curr_rec)) { continue; }
      }
      c->rest--;
      return c->curr_rec;
    }
  }
  return GRN_ID_NIL;
}

grn_id
grn_hash_next(grn_ctx *ctx, grn_hash *hash, grn_id id)
{
  grn_id max = HASH_CURR_MAX(hash);
  while (++id <= max) {
    if (grn_hash_bitmap_at(ctx, hash, id)) { return id; }
  }
  return GRN_ID_NIL;
}

grn_id
grn_hash_at(grn_ctx *ctx, grn_hash *hash, grn_id id)
{
  return grn_hash_bitmap_at(ctx, hash, id) ? id : GRN_ID_NIL;
}

int
grn_hash_cursor_get_key(grn_ctx *ctx, grn_hash_cursor *c, void **key)
{
  int key_size;
  entry_str *ee;
  if (!c) { return 0; }
  ee = grn_hash_entry_at(ctx, c->hash, c->curr_rec, 0);
  if (!ee) { return 0; }
  key_size = (c->hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) ? ee->size : c->hash->key_size;
  *key = get_key(ctx, c->hash, ee);
  return key_size;
}

int
grn_hash_cursor_get_value(grn_ctx *ctx, grn_hash_cursor *c, void **value)
{
  void *v;
  entry_str *ee;
  if (!c) { return 0; }
  ee = grn_hash_entry_at(ctx, c->hash, c->curr_rec, 0);
  if (ee && (v = get_value(ctx, c->hash, ee))) {
    *value = v;
    return c->hash->value_size;
  }
  return 0;
}

int
grn_hash_cursor_get_key_value(grn_ctx *ctx, grn_hash_cursor *c,
                              void **key, uint32_t *key_size, void **value)
{
  entry_str *ee;
  if (!c) { return GRN_INVALID_ARGUMENT; }
  ee = grn_hash_entry_at(ctx, c->hash, c->curr_rec, 0);
  if (!ee) { return GRN_INVALID_ARGUMENT; }
  if (key_size) {
    *key_size = (c->hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) ? ee->size : c->hash->key_size;
  }
  if (key) { *key = get_key(ctx, c->hash, ee); }
  if (value) { *value = get_value(ctx, c->hash, ee); }
  return c->hash->value_size;
}

grn_rc
grn_hash_cursor_set_value(grn_ctx *ctx, grn_hash_cursor *c,
                          const void *value, int flags)
{
  if (!c) { return GRN_INVALID_ARGUMENT; }
  return grn_hash_set_value(ctx, c->hash, c->curr_rec, value, flags);
}

grn_rc
grn_hash_cursor_delete(grn_ctx *ctx, grn_hash_cursor *c,
                       grn_table_delete_optarg *optarg)
{
  if (!c) { return GRN_INVALID_ARGUMENT; }
  return grn_hash_delete_by_id(ctx, c->hash, c->curr_rec, optarg);
}

/* sort */

#define PREPARE_VAL(e,ep,es) do {\
  if ((arg->flags & GRN_TABLE_SORT_BY_VALUE)) {\
    ep = ((const uint8_t *)(get_value(ctx, hash, (entry_str *)(e))));\
    es = hash->value_size;\
  } else {\
    ep = ((const uint8_t *)(get_key(ctx, hash, (entry_str *)(e))));\
    es = ((hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE)\
          ? ((entry_str *)(e))->size : hash->key_size); \
  }\
  ep += arg->offset;\
  es -= arg->offset;\
} while (0)

#define COMPARE_VAL_(ap,as,bp,bs)\
  (arg->compar\
   ? arg->compar(ctx,\
                 (grn_obj *)hash, (void *)(ap), as,\
                 (grn_obj *)hash, (void *)(bp), bs, arg->compar_arg)\
   : ((arg->flags & GRN_TABLE_SORT_AS_NUMBER)\
      ? ((arg->flags & GRN_TABLE_SORT_AS_UNSIGNED)\
         ? ((arg->flags & GRN_TABLE_SORT_AS_INT64)\
            ? *((uint64_t *)(ap)) > *((uint64_t *)(bp))\
            : *((uint32_t *)(ap)) > *((uint32_t *)(bp)))\
         : ((arg->flags & GRN_TABLE_SORT_AS_INT64)\
            ? *((int64_t *)(ap)) > *((int64_t *)(bp))\
            : *((int32_t *)(ap)) > *((int32_t *)(bp))))\
      : grn_str_greater(ap, as, bp, bs)))

#define COMPARE_VAL(ap,as,bp,bs)\
  ((dir) ? COMPARE_VAL_((bp),(bs),(ap),(as)) : COMPARE_VAL_((ap),(as),(bp),(bs)))

inline static entry **
pack(grn_ctx *ctx, grn_hash *hash, entry **res, grn_table_sort_optarg *arg, int dir)
{
  uint32_t n;
  uint32_t cs, es;
  const uint8_t *cp, *ep;
  entry **head, **tail, *e, *c;
  grn_id id, m = HASH_CURR_MAX(hash);
  for (id = m >> 1;;id = (id == m) ? 1 : id + 1) {
    if (grn_hash_bitmap_at(ctx, hash, id)) { break; }
  }
  c = grn_hash_entry_at(ctx, hash, id, 0);
  if (!c) { return NULL; }
  PREPARE_VAL(c, cp, cs);
  head = res;
  n = *hash->n_entries - 1;
  tail = res + n;
  while (n--) {
    do {
      id = (id == m) ? 1 : id + 1;
    } while (!grn_hash_bitmap_at(ctx, hash, id));
    e = grn_hash_entry_at(ctx, hash, id, 0);
    if (!e) { return NULL; }
    PREPARE_VAL(e, ep, es);
    if (COMPARE_VAL(cp, cs, ep, es)) {
      *head++ = e;
    } else {
      *tail-- = e;
    }
  }
  *head = c;
  return *hash->n_entries > 2 ? head : NULL;
}

inline static void
swap(entry **a, entry **b)
{
  entry *c_ = *a;
  *a = *b;
  *b = c_;
}

#define SWAP(a,ap,as,b,bp,bs) do {\
  const uint8_t *cp_ = ap;\
  uint32_t cs_ = as;\
  ap = bp; bp = cp_;\
  as = bs; bs = cs_;\
  swap(a,b);\
} while (0)

inline static entry **
part(grn_ctx *ctx, entry **b, entry **e, grn_table_sort_optarg *arg, grn_hash *hash, int dir)
{
  entry **c;
  const uint8_t *bp, *cp, *ep;
  uint32_t bs, cs, es;
  intptr_t d = e - b;
  PREPARE_VAL(*b, bp, bs);
  PREPARE_VAL(*e, ep, es);
  if (COMPARE_VAL(bp, bs, ep, es)) {
    SWAP(b, bp, bs, e, ep, es);
  }
  if (d < 2) { return NULL; }
  c = b + (d >> 1);
  PREPARE_VAL(*c, cp, cs);
  if (COMPARE_VAL(bp, bs, cp, cs)) {
    SWAP(b, bp, bs, c, cp, cs);
  } else {
    if (COMPARE_VAL(cp, cs, ep, es)) {
      SWAP(c, cp, cs, e, ep, es);
    }
  }
  if (d < 3) { return NULL; }
  b++;
  swap(b, c);
  c = b;
  PREPARE_VAL(*c, cp, cs);
  for (;;) {
    do {
      b++;
      PREPARE_VAL(*b, bp, bs);
    } while (COMPARE_VAL(cp, cs, bp, bs));
    do {
      e--;
      PREPARE_VAL(*e, ep, es);
    } while (COMPARE_VAL(ep, es, cp, cs));
    if (b >= e) { break; }
    SWAP(b, bp, bs, e, ep, es);
  }
  SWAP(c, cp, cs, e, ep, es);
  return e;
}

static void
_sort(grn_ctx *ctx, entry **head, entry **tail, int limit,
      grn_table_sort_optarg *arg, grn_hash *hash, int dir)
{
  entry **c;
  if (head < tail && (c = part(ctx, head, tail, arg, hash, dir))) {
    intptr_t rest = limit - 1 - (c - head);
    _sort(ctx, head, c - 1, limit, arg, hash, dir);
    if (rest > 0) { _sort(ctx, c + 1, tail, (int)rest, arg, hash, dir); }
  }
}

static void
sort(grn_ctx *ctx,
     grn_hash *hash, entry **res, int limit, grn_table_sort_optarg *arg, int dir)
{
  entry **c = pack(ctx, hash, res, arg, dir);
  if (c) {
    intptr_t rest = limit - 1 - (c - res);
    _sort(ctx, res, c - 1, limit, arg, hash, dir);
    if (rest > 0 ) {
      _sort(ctx, c + 1, res + *hash->n_entries - 1, (int)rest, arg, hash, dir);
    }
  }
}

typedef struct {
  grn_id id;
  int32_t v;
} val32;

#define PREPARE_VAL32(id,e,ep) do {\
  (ep)->id = id;\
  (ep)->v = (arg->flags & GRN_TABLE_SORT_BY_ID)\
    ? (int32_t) id\
    : (*((int32_t *)((byte *)((arg->flags & GRN_TABLE_SORT_BY_VALUE)\
                              ? get_value(ctx, hash, (e))\
                              : get_key(ctx, hash, (e))) + arg->offset)));\
} while (0)

#define COMPARE_VAL32_(ap,bp) \
  (arg->compar\
   ? arg->compar(ctx,\
                 (grn_obj *)hash, (void *)&(ap)->v, sizeof(uint32_t),\
                 (grn_obj *)hash, (void *)&(bp)->v, sizeof(uint32_t),\
                 arg->compar_arg)\
   : ((arg->flags & GRN_TABLE_SORT_AS_NUMBER)\
      ? ((arg->flags & GRN_TABLE_SORT_AS_UNSIGNED)\
         ? *((uint32_t *)&(ap)->v) > *((uint32_t *)&(bp)->v)\
         : *((int32_t *)&(ap)->v) > *((int32_t *)&(bp)->v))\
      : memcmp(&(ap)->v, &(bp)->v, sizeof(uint32_t)) > 0))

#define COMPARE_VAL32(ap,bp)\
  ((dir) ? COMPARE_VAL32_((bp),(ap)) : COMPARE_VAL32_((ap),(bp)))

inline static val32 *
pack_val32(grn_ctx *ctx, grn_hash *hash, val32 *res, grn_table_sort_optarg *arg, int dir)
{
  uint32_t n;
  entry_str *e, *c;
  val32 *head, *tail, cr, er;
  grn_id id, m = HASH_CURR_MAX(hash);
  for (id = m >> 1;;id = (id == m) ? 1 : id + 1) {
    if (grn_hash_bitmap_at(ctx, hash, id)) { break; }
  }
  c = grn_hash_entry_at(ctx, hash, id, 0);
  if (!c) { return NULL; }
  PREPARE_VAL32(id, c, &cr);
  head = res;
  n = *hash->n_entries - 1;
  tail = res + n;
  while (n--) {
    do {
      id = (id == m) ? 1 : id + 1;
    } while (!grn_hash_bitmap_at(ctx, hash, id));
    e = grn_hash_entry_at(ctx, hash, id, 0);
    if (!e) { return NULL; }
    PREPARE_VAL32(id, e, &er);
    if (COMPARE_VAL32(&cr, &er)) {
      *head++ = er;
    } else {
      *tail-- = er;
    }
  }
  *head = cr;
  return *hash->n_entries > 2 ? head : NULL;
}

#define SWAP_VAL32(ap,bp) do {\
  val32 cr_ = *ap;\
  *ap = *bp;\
  *bp = cr_;\
} while (0)

inline static val32 *
part_val32(grn_ctx *ctx,
           val32 *b, val32 *e, grn_table_sort_optarg *arg, grn_hash *hash, int dir)
{
  val32 *c;
  intptr_t d = e - b;
  if (COMPARE_VAL32(b, e)) { SWAP_VAL32(b, e); }
  if (d < 2) { return NULL; }
  c = b + (d >> 1);
  if (COMPARE_VAL32(b, c)) {
    SWAP_VAL32(b, c);
  } else {
    if (COMPARE_VAL32(c, e)) { SWAP_VAL32(c, e); }
  }
  if (d < 3) { return NULL; }
  b++;
  SWAP_VAL32(b, c);
  c = b;
  for (;;) {
    do { b++; } while (COMPARE_VAL32(c, b));
    do { e--; } while (COMPARE_VAL32(e, c));
    if (b >= e) { break; }
    SWAP_VAL32(b, e);
  }
  SWAP_VAL32(c, e);
  return e;
}

static void
_sort_val32(grn_ctx *ctx, val32 *head, val32 *tail, int limit,
      grn_table_sort_optarg *arg, grn_hash *hash, int dir)
{
  val32 *c;
  if (head < tail && (c = part_val32(ctx, head, tail, arg, hash, dir))) {
    intptr_t rest = limit - 1 - (c - head);
    _sort_val32(ctx, head, c - 1, limit, arg, hash, dir);
    if (rest > 0) { _sort_val32(ctx, c + 1, tail, (int)rest, arg, hash, dir); }
  }
}

static void
sort_val32(grn_ctx *ctx,
           grn_hash *hash, val32 *res, int limit, grn_table_sort_optarg *arg, int dir)
{
  val32 *c = pack_val32(ctx, hash, res, arg, dir);
  if (c) {
    intptr_t rest = limit - 1 - (c - res);
    _sort_val32(ctx, res, c - 1, limit, arg, hash, dir);
    if (rest > 0 ) {
      _sort_val32(ctx, c + 1, res + *hash->n_entries - 1, (int)rest, arg, hash, dir);
    }
  }
}

inline static grn_id
entry2id(grn_ctx *ctx, grn_hash *hash, entry *e)
{
  entry *e2;
  grn_id id, *ep;
  uint32_t i, h = e->key, s = grn_hash_calculate_step(h);
  for (i = h; ; i += s) {
    if (!(ep = grn_hash_idx_at(ctx, hash, i))) { return GRN_ID_NIL; }
    if (!(id = *ep)) { break; }
    if (id != GARBAGE) {
      e2 = grn_hash_entry_at(ctx, hash, id, 0);
      if (!e2) { return GRN_ID_NIL; }
      if (e2 == e) { break; }
    }
  }
  return id;
}

int
grn_hash_sort(grn_ctx *ctx, grn_hash *hash,
              int limit, grn_array *result, grn_table_sort_optarg *optarg)
{
  entry **res;
  if (!result || !*hash->n_entries) { return 0; }
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return 0;
  }
  if (!(res = GRN_MALLOC(sizeof(entry *) * *hash->n_entries))) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "allocation of entries failed on grn_hash_sort !");
    return 0;
  }
  if (limit < 0) {
    limit += *hash->n_entries + 1;
    if (limit < 0) {
      GRN_LOG(ctx, GRN_LOG_ALERT, "limit is too small in grn_hash_sort !");
      return 0;
    }
  }
  if ((uint) limit > (uint) *hash->n_entries) { limit = *hash->n_entries; }
  /*  hash->limit = limit; */
  if (optarg) {
    int dir = (optarg->flags & GRN_TABLE_SORT_DESC);
    if ((optarg->flags & GRN_TABLE_SORT_BY_ID) ||
        (optarg->flags & GRN_TABLE_SORT_BY_VALUE)
        ? ((hash->value_size - optarg->offset) == sizeof(uint32_t))
        : (!(hash->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE)
           && hash->key_size == sizeof(uint32_t))) {
      if (sizeof(entry *) != sizeof(val32)) {
        GRN_FREE(res);
        if (!(res = GRN_MALLOC(sizeof(val32) * *hash->n_entries))) {
          GRN_LOG(ctx, GRN_LOG_ALERT, "allocation of entries failed on grn_hash_sort !");
          return 0;
        }
      }
      sort_val32(ctx, hash, (val32 *)res, limit, optarg, dir);
      {
        int i;
        grn_id *v;
        val32 *rp = (val32 *)res;
        for (i = 0; i < limit; i++, rp++) {
          if (!grn_array_add(ctx, result, (void **)&v)) { break; }
          if (!(*v = rp->id)) { break; }
        }
        GRN_FREE(res);
        return i;
      }
    } else {
      sort(ctx, hash, res, limit, optarg, dir);
    }
  } else {
    grn_table_sort_optarg opt = {0, NULL, NULL, NULL, 0};
    sort(ctx, hash, res, limit, &opt, 0);
  }
  {
    int i;
    grn_id *v;
    entry **rp = res;
    for (i = 0; i < limit; i++, rp++) {
      if (!grn_array_add(ctx, result, (void **)&v)) { break; }
      if (!(*v = entry2id(ctx, hash, *rp))) { break; }
    }
    GRN_FREE(res);
    return i;
  }
}

void
grn_hash_check(grn_ctx *ctx, grn_hash *hash)
{
  char buf[8];
  grn_hash_header_common *h = hash->header.common;
  if (grn_hash_error_if_truncated(ctx, hash) != GRN_SUCCESS) {
    return;
  }
  GRN_OUTPUT_ARRAY_OPEN("RESULT", 1);
  GRN_OUTPUT_MAP_OPEN("SUMMARY", 26);
  GRN_OUTPUT_CSTR("flags");
  grn_itoh(h->flags, buf, 8);
  GRN_OUTPUT_STR(buf, 8);
  GRN_OUTPUT_CSTR("key_size");
  GRN_OUTPUT_INT64(hash->key_size);
  GRN_OUTPUT_CSTR("value_size");
  GRN_OUTPUT_INT64(hash->value_size);
  GRN_OUTPUT_CSTR("tokenizer");
  GRN_OUTPUT_INT64(h->tokenizer);
  GRN_OUTPUT_CSTR("normalizer");
  GRN_OUTPUT_INT64(h->normalizer);
  GRN_OUTPUT_CSTR("curr_rec");
  GRN_OUTPUT_INT64(h->curr_rec);
  GRN_OUTPUT_CSTR("curr_key_normal");
  GRN_OUTPUT_UINT64(h->curr_key_normal);
  GRN_OUTPUT_CSTR("curr_key_large");
  GRN_OUTPUT_UINT64(h->curr_key_large);
  GRN_OUTPUT_CSTR("idx_offset");
  GRN_OUTPUT_INT64(h->idx_offset);
  GRN_OUTPUT_CSTR("entry_size");
  GRN_OUTPUT_INT64(hash->entry_size);
  GRN_OUTPUT_CSTR("max_offset");
  GRN_OUTPUT_INT64(*hash->max_offset);
  GRN_OUTPUT_CSTR("n_entries");
  GRN_OUTPUT_INT64(*hash->n_entries);
  GRN_OUTPUT_CSTR("n_garbages");
  GRN_OUTPUT_INT64(*hash->n_garbages);
  GRN_OUTPUT_CSTR("lock");
  GRN_OUTPUT_INT64(h->lock);
  GRN_OUTPUT_MAP_CLOSE();
  GRN_OUTPUT_ARRAY_CLOSE();
}

/* rhash : grn_hash with subrecs */

#ifdef USE_GRN_INDEX2

static uint32_t default_flags = GRN_HASH_TINY;

grn_rc
grn_rhash_init(grn_ctx *ctx, grn_hash *hash, grn_rec_unit record_unit, int record_size,
               grn_rec_unit subrec_unit, int subrec_size, unsigned int max_n_subrecs)
{
  grn_rc rc;
  record_size = grn_rec_unit_size(record_unit, record_size);
  subrec_size = grn_rec_unit_size(subrec_unit, subrec_size);
  if (record_unit != grn_rec_userdef && subrec_unit != grn_rec_userdef) {
    subrec_size -= record_size;
  }
  if (!hash) { return GRN_INVALID_ARGUMENT; }
  if (record_size < 0) { return GRN_INVALID_ARGUMENT; }
  if ((default_flags & GRN_HASH_TINY)) {
    rc = grn_tiny_hash_init(ctx, hash, NULL, record_size,
                            max_n_subrecs * (GRN_RSET_SCORE_SIZE + subrec_size),
                            default_flags, GRN_ENC_NONE);
  } else {
    rc = grn_io_hash_init(ctx, hash, NULL, record_size,
                          max_n_subrecs * (GRN_RSET_SCORE_SIZE + subrec_size),
                          default_flags, GRN_ENC_NONE, 0);
  }
  if (rc) { return rc; }
  hash->record_unit = record_unit;
  hash->subrec_unit = subrec_unit;
  hash->subrec_size = subrec_size;
  hash->max_n_subrecs = max_n_subrecs;
  return rc;
}

grn_rc
grn_rhash_fin(grn_ctx *ctx, grn_hash *hash)
{
  grn_rc rc;
  if (grn_hash_is_io_hash(hash)) {
    rc = grn_io_close(ctx, hash->io);
  } else {
    GRN_ASSERT(ctx == hash->ctx);
    rc = grn_tiny_hash_fin(ctx, hash);
  }
  return rc;
}

inline static void
subrecs_push(byte *subrecs, int size, int n_subrecs, int score, void *body, int dir)
{
  byte *v;
  int *c2;
  int n = n_subrecs - 1, n2;
  while (n) {
    n2 = (n - 1) >> 1;
    c2 = GRN_RSET_SUBRECS_NTH(subrecs,size,n2);
    if (GRN_RSET_SUBRECS_CMP(score, *c2, dir) > 0) { break; }
    GRN_RSET_SUBRECS_COPY(subrecs,size,n,c2);
    n = n2;
  }
  v = subrecs + n * (size + GRN_RSET_SCORE_SIZE);
  *((int *)v) = score;
  grn_memcpy(v + GRN_RSET_SCORE_SIZE, body, size);
}

inline static void
subrecs_replace_min(byte *subrecs, int size, int n_subrecs, int score, void *body, int dir)
{
  byte *v;
  int n = 0, n1, n2, *c1, *c2;
  for (;;) {
    n1 = n * 2 + 1;
    n2 = n1 + 1;
    c1 = n1 < n_subrecs ? GRN_RSET_SUBRECS_NTH(subrecs,size,n1) : NULL;
    c2 = n2 < n_subrecs ? GRN_RSET_SUBRECS_NTH(subrecs,size,n2) : NULL;
    if (c1 && GRN_RSET_SUBRECS_CMP(score, *c1, dir) > 0) {
      if (c2 &&
          GRN_RSET_SUBRECS_CMP(score, *c2, dir) > 0 &&
          GRN_RSET_SUBRECS_CMP(*c1, *c2, dir) > 0) {
        GRN_RSET_SUBRECS_COPY(subrecs,size,n,c2);
        n = n2;
      } else {
        GRN_RSET_SUBRECS_COPY(subrecs,size,n,c1);
        n = n1;
      }
    } else {
      if (c2 && GRN_RSET_SUBRECS_CMP(score, *c2, dir) > 0) {
        GRN_RSET_SUBRECS_COPY(subrecs,size,n,c2);
        n = n2;
      } else {
        break;
      }
    }
  }
  v = subrecs + n * (size + GRN_RSET_SCORE_SIZE);
  grn_memcpy(v, &score, GRN_RSET_SCORE_SIZE);
  grn_memcpy(v + GRN_RSET_SCORE_SIZE, body, size);
}

void
grn_rhash_add_subrec(grn_hash *s, grn_rset_recinfo *ri, int score, void *body, int dir)
{
  int limit = s->max_n_subrecs;
  ri->score += score;
  ri->n_subrecs += 1;
  if (limit) {
    int ssize = s->subrec_size;
    int n_subrecs = GRN_RSET_N_SUBRECS(ri);
    if (limit < n_subrecs) {
      if (GRN_RSET_SUBRECS_CMP(score, *ri->subrecs, dir) > 0) {
        subrecs_replace_min(ri->subrecs, ssize, limit, score, body, dir);
      }
    } else {
      subrecs_push(ri->subrecs, ssize, n_subrecs, score, body, dir);
    }
  }
}

grn_hash *
grn_rhash_group(grn_hash *s, int limit, grn_group_optarg *optarg)
{
  grn_ctx *ctx = s->ctx;
  grn_hash *g, h;
  grn_rset_recinfo *ri;
  grn_rec_unit unit;
  grn_hash_cursor *c;
  grn_id rh;
  byte *key, *ekey, *gkey = NULL;
  int funcp, dir;
  unsigned int rsize;
  if (!s || !s->index) { return NULL; }
  if (optarg) {
    unit = grn_rec_userdef;
    rsize = optarg->key_size;
    funcp = optarg->func ? 1 : 0;
    dir = (optarg->mode == grn_sort_ascending) ? -1 : 1;
  } else {
    unit = grn_rec_document;
    rsize = grn_rec_unit_size(unit, sizeof(grn_id));
    funcp = 0;
    dir = 1;
  }
  if (funcp) {
    gkey = GRN_MALLOC(rsize ? rsize : 8192);
    if (!gkey) {
      GRN_LOG(ctx, GRN_LOG_ALERT, "allocation for gkey failed !");
      return NULL;
    }
  } else {
    if (s->key_size <= rsize) { return NULL; }
  }
  if (!(c = grn_hash_cursor_open(s->ctx, s, NULL, 0, NULL, -1, 0))) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_hash_cursor_open on grn_hash_group failed !");
    if (gkey) { GRN_FREE(gkey); }
    return NULL;
  }
  grn_memcpy(&h, s, sizeof(grn_hash));
  g = s;
  s = &h;
  if (grn_rhash_init(ctx, g, unit, rsize, s->record_unit, s->key_size, limit)) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_rhash_init in grn_hash_group failed !");
    grn_hash_cursor_close(s->ctx, c);
    if (gkey) { GRN_FREE(gkey); }
    return NULL;
  }
  while ((rh = grn_hash_cursor_next(ctx, c))) {
    grn_hash_cursor_get_key_value(ctx, c, (void **)&key, NULL, (void **)&ri);
    if (funcp) {
      if (optarg->func((grn_records *)s,
                       (grn_recordh *)(intptr_t)rh, gkey, optarg->func_arg)) { continue; }
      ekey = key;
    } else {
      gkey = key;
      ekey = key + rsize;
    }
    {
      grn_rset_recinfo *gri;
      if (grn_hash_add(ctx, g, gkey, rsize, (void **)&gri, NULL)) {
        grn_rhash_add_subrec(g, gri, ri->score, ekey, dir);
      }
    }
  }
  grn_hash_cursor_close(s->ctx, c);
  grn_rhash_fin(s->ctx, s);
  if (funcp) { GRN_FREE(gkey); }
  return g;
}

grn_rc
grn_rhash_subrec_info(grn_ctx *ctx, grn_hash *s, grn_id rh, int index,
                      grn_id *rid, int *section, int *pos, int *score, void **subrec)
{
  grn_rset_posinfo *pi;
  grn_rset_recinfo *ri;
  int *p, unit_size = GRN_RSET_SCORE_SIZE + s->subrec_size;
  if (!s || !rh || index < 0) { return GRN_INVALID_ARGUMENT; }
  if ((unsigned int)index >= s->max_n_subrecs) { return GRN_INVALID_ARGUMENT; }
  {
    entry_str *ee;
    if (!grn_hash_bitmap_at(ctx, s, rh)) { return GRN_INVALID_ARGUMENT; }
    ee = grn_hash_entry_at(ctx, s, rh, 0);
    if (!ee) { return GRN_INVALID_ARGUMENT; }
    pi = (grn_rset_posinfo *)get_key(ctx, s, ee);
    ri = get_value(ctx, s, ee);
    if (!pi || !ri) { return GRN_INVALID_ARGUMENT; }
  }
  if (index >= ri->n_subrecs) { return GRN_INVALID_ARGUMENT; }
  p = (int *)(ri->subrecs + index * unit_size);
  if (score) { *score = p[0]; }
  if (subrec) { *subrec = &p[1]; }
  switch (s->record_unit) {
  case grn_rec_document :
    if (rid) { *rid = pi->rid; }
    if (section) { *section = (s->subrec_unit != grn_rec_userdef) ? p[1] : 0; }
    if (pos) { *pos = (s->subrec_unit == grn_rec_position) ? p[2] : 0; }
    break;
  case grn_rec_section :
    if (rid) { *rid = pi->rid; }
    if (section) { *section = pi->sid; }
    if (pos) { *pos = (s->subrec_unit == grn_rec_position) ? p[1] : 0; }
    break;
  default :
    pi = (grn_rset_posinfo *)&p[1];
    switch (s->subrec_unit) {
    case grn_rec_document :
      if (rid) { *rid = pi->rid; }
      if (section) { *section = 0; }
      if (pos) { *pos = 0; }
      break;
    case grn_rec_section :
      if (rid) { *rid = pi->rid; }
      if (section) { *section = pi->sid; }
      if (pos) { *pos = 0; }
      break;
    case grn_rec_position :
      if (rid) { *rid = pi->rid; }
      if (section) { *section = pi->sid; }
      if (pos) { *pos = pi->pos; }
      break;
    default :
      if (rid) { *rid = 0; }
      if (section) { *section = 0; }
      if (pos) { *pos = 0; }
      break;
    }
    break;
  }
  return GRN_SUCCESS;
}
#endif /* USE_GRN_INDEX2 */

grn_bool
grn_hash_is_large_total_key_size(grn_ctx *ctx, grn_hash *hash)
{
  return (hash->header.common->flags & GRN_OBJ_KEY_LARGE) == GRN_OBJ_KEY_LARGE;
}

uint64_t
grn_hash_total_key_size(grn_ctx *ctx, grn_hash *hash)
{
  if (grn_hash_is_large_total_key_size(ctx, hash)) {
    return hash->header.common->curr_key_large;
  } else {
    return hash->header.common->curr_key_normal;
  }
}

uint64_t
grn_hash_max_total_key_size(grn_ctx *ctx, grn_hash *hash)
{
  if (grn_hash_is_large_total_key_size(ctx, hash)) {
    return GRN_HASH_KEY_MAX_TOTAL_SIZE_LARGE;
  } else {
    return GRN_HASH_KEY_MAX_TOTAL_SIZE_NORMAL;
  }
}
