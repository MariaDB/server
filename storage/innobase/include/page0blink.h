#ifndef page0blink_h
#define page0blink_h

#include "dict0mem.h"
#include "page0page.h"

struct buf_block_t;
struct dict_index_t;
struct dtuple_t;
struct mem_block_info_t;
typedef mem_block_info_t mem_heap_t;
struct mtr_t;

bool rec_is_high_key_structural(const page_t *page, const rec_t *rec,
                                const dict_index_t *index);

inline bool page_has_incomplete_split(const page_t *page) noexcept
{
  return page_get_direction(page) == PAGE_INCOMPLETE_SPLIT;
}

inline bool rec_is_high_key(const page_t *page, const rec_t *rec,
                            const dict_index_t *index) noexcept
{
  return index && rec_is_high_key_structural(page, rec, index);
}

inline const rec_t *page_rec_get_next_user(const page_t *page,
                                            const rec_t *rec,
                                            const dict_index_t *index) noexcept
{
  const rec_t *next= page_rec_get_next_const(rec);
  return rec_is_high_key(page, next, index) ? page_get_supremum_rec(page) : next;
}

inline const rec_t *page_rec_get_prev_user(const page_t *page,
                                            const rec_t *rec,
                                            const dict_index_t *index) noexcept
{
  const rec_t *previous= page_rec_get_prev_const(rec);
  return rec_is_high_key(page, previous, index)
    ? page_rec_get_prev_const(previous) : previous;
}

inline ulint page_get_n_user_recs(const page_t *page,
                                  const dict_index_t *index) noexcept
{
  const ulint records= page_get_n_recs(page);
  const rec_t *last= page_rec_get_prev_const(page_get_supremum_rec(page));
  return records - ulint{rec_is_high_key(page, last, index)};
}

void page_set_incomplete_split(buf_block_t *block, mtr_t *mtr);
void page_clear_incomplete_split(buf_block_t *block, mtr_t *mtr);

dtuple_t *blink_read_high_key_record(const page_t *page,
                                      const dict_index_t *index,
                                      mem_heap_t *heap);
bool blink_write_high_key_record(buf_block_t *block, const dtuple_t *key,
                                 dict_index_t *index, mtr_t *mtr);
void blink_delete_high_key_record(buf_block_t *block, dict_index_t *index,
                                  mtr_t *mtr);

#endif
