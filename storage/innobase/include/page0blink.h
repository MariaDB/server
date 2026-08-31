#ifndef page0blink_h
#define page0blink_h

#include "page0page.h"

struct buf_block_t;
struct dict_index_t;
struct dtuple_t;
struct mem_block_info_t;
typedef mem_block_info_t mem_heap_t;
struct mtr_t;

inline bool page_has_incomplete_split(const page_t *page) noexcept
{
  return page_get_direction(page) == PAGE_INCOMPLETE_SPLIT;
}

void page_set_incomplete_split(buf_block_t *block, mtr_t *mtr);
void page_clear_incomplete_split(buf_block_t *block, mtr_t *mtr);

bool rec_is_high_key_structural(const page_t *page, const rec_t *rec,
                                const dict_index_t *index);
dtuple_t *blink_read_high_key_record(const page_t *page,
                                      const dict_index_t *index,
                                      mem_heap_t *heap);
bool blink_write_high_key_record(buf_block_t *block, const dtuple_t *key,
                                 dict_index_t *index, mtr_t *mtr);
void blink_delete_high_key_record(buf_block_t *block, dict_index_t *index,
                                  mtr_t *mtr);

#endif
