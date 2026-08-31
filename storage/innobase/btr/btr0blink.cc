#include "btr0blink.h"

#include "btr0btr.h"
#include "btr0sea.h"
#include "dict0dict.h"
#include "mtr0mtr.h"

bool blink_stamp_empty_tree(dict_index_t *index)
{
  ut_ad(dict_sys.locked());
  if (!blink_index_shape_ok(index) || index->type & DICT_BLINK ||
      !index->is_committed())
    return false;

  mtr_t mtr{nullptr};
  mtr.start();
  mtr_x_lock_index(index, &mtr);
  dberr_t err= DB_SUCCESS;
  buf_block_t *root= btr_root_block_get(index, RW_X_LATCH, &mtr, &err);
  if (!root || btr_page_get_level(root->page.frame)) {
    mtr.commit();
    return false;
  }

#ifdef BTR_CUR_HASH_ADAPT
  btr_search_drop_page_hash_index(root, nullptr);
  index->search_info.set_enabled_fixed_mask(
    dict_index_t::ahi::AHI_INDEX_FORCE_DISABLED,
    false, false, false, 0, 0, false);
#endif
  index->type|= DICT_BLINK;
  const bool success= dict_index_persist_type(index);
  if (!success)
    index->type&= ~DICT_BLINK;
  mtr.commit();
  return success;
}
