#ifndef btr0blink_alloc_h
#define btr0blink_alloc_h

#include "db0err.h"
#include "univ.i"

struct dict_index_t;
struct trx_t;

struct blink_page_pool_t;
struct blink_pool_entry_t;

enum class blink_page_kind : uint8_t
{
  LEAF,
  INTERNAL
};

bool blink_page_pool_register(dict_index_t *index) noexcept;
void blink_page_pool_unregister(dict_index_t *index) noexcept;
bool blink_page_pool_try_pop(dict_index_t *index, blink_page_kind kind,
                             uint32_t *page_no) noexcept;
void blink_page_pool_push(dict_index_t *index, blink_page_kind kind,
                          uint32_t page_no) noexcept;
void blink_page_pool_thread_start() noexcept;
void blink_page_pool_thread_stop() noexcept;
void blink_page_pool_query_depth(size_t *leaf, size_t *internal) noexcept;
void blink_page_pool_watermarks_changed() noexcept;
blink_pool_entry_t *blink_page_pool_pin(dict_index_t *index) noexcept;
void blink_page_pool_unpin(blink_pool_entry_t *entry) noexcept;

#endif
