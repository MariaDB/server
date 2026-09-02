#include "btr0blink_alloc.h"

#include "btr0blink.h"
#include "btr0btr.h"
#include "dict0mem.h"
#include "mtr0mtr.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

static constexpr size_t blink_low_watermark[2]{64, 8};
static constexpr size_t blink_high_watermark[2]{256, 32};

struct blink_page_pool_t
{
  std::mutex mutex;
  std::deque<uint32_t> pages[2];
  bool refilling[2]{true, true};
};

struct blink_pool_entry_t
{
  explicit blink_pool_entry_t(dict_index_t *owner) : index(owner) {}
  dict_index_t *index;
  blink_page_pool_t pool;
  std::atomic<uint32_t> users{0};
  std::atomic<bool> shutdown{false};
  std::mutex reclaim_mutex;
  std::condition_variable reclaim_cv;
  bool reclaimed{false};
};

static std::mutex blink_registry_mutex;
static std::condition_variable blink_registry_cv;
static std::unordered_map<dict_index_t*, std::unique_ptr<blink_pool_entry_t>>
  blink_registry;
static std::deque<blink_pool_entry_t*> blink_reclaim_queue;
static std::thread blink_allocator_thread;
static std::atomic<bool> blink_allocator_shutdown{false};
static bool blink_allocator_running;
static size_t blink_registry_cursor;

static size_t blink_kind_index(blink_page_kind kind) noexcept
{
  return kind == blink_page_kind::LEAF ? 0 : 1;
}

blink_pool_entry_t *blink_page_pool_pin(dict_index_t *index) noexcept
{
  std::lock_guard<std::mutex> guard(blink_registry_mutex);
  auto it= blink_registry.find(index);
  if (it == blink_registry.end() || it->second->shutdown.load())
    return nullptr;
  it->second->users.fetch_add(1);
  return it->second.get();
}

void blink_page_pool_unpin(blink_pool_entry_t *entry) noexcept
{
  if (entry->users.fetch_sub(1) == 1)
    blink_registry_cv.notify_all();
}

bool blink_page_pool_register(dict_index_t *index) noexcept
{
  ut_ad(index);
  {
    std::lock_guard<std::mutex> guard(blink_registry_mutex);
    auto existing= blink_registry.find(index);
    if (existing != blink_registry.end()) {
      index->blink_page_pool= &existing->second->pool;
      return true;
    }
    std::unique_ptr<blink_pool_entry_t> entry{
      new (std::nothrow) blink_pool_entry_t(index)};
    if (!entry)
      return false;
    index->blink_page_pool= &entry->pool;
    blink_registry.emplace(index, std::move(entry));
  }
  blink_pending_split_enqueue(index, FIL_NULL);
  blink_registry_cv.notify_one();
  return true;
}

bool blink_page_pool_try_pop(dict_index_t *index, blink_page_kind kind,
                             uint32_t *page_no) noexcept
{
  ut_ad(page_no);
  blink_pool_entry_t *entry= blink_page_pool_pin(index);
  if (!entry)
    return false;
  const size_t slot= blink_kind_index(kind);
  bool found= false;
  {
    std::lock_guard<std::mutex> guard(entry->pool.mutex);
    auto &pages= entry->pool.pages[slot];
    if (!pages.empty()) {
      *page_no= pages.front();
      pages.pop_front();
      if (pages.size() < blink_low_watermark[slot])
        entry->pool.refilling[slot]= true;
      found= true;
    }
  }
  blink_page_pool_unpin(entry);
  blink_registry_cv.notify_one();
  return found;
}

void blink_page_pool_push(dict_index_t *index, blink_page_kind kind,
                          uint32_t page_no) noexcept
{
  blink_pool_entry_t *entry= blink_page_pool_pin(index);
  ut_a(entry);
  const size_t slot= blink_kind_index(kind);
  {
    std::lock_guard<std::mutex> guard(entry->pool.mutex);
    entry->pool.pages[slot].push_back(page_no);
  }
  blink_page_pool_unpin(entry);
}

static uint32_t blink_alloc_page(dict_index_t *index,
                                 blink_page_kind kind) noexcept
{
  fil_space_t *space= index->table->space;
  if (index->page == FIL_NULL || !space || !index->is_committed())
    return FIL_NULL;

  uint32_t reserved= 0;
  mtr_t reserve_mtr{nullptr};
  reserve_mtr.start();
  index->set_modified(reserve_mtr);
  dberr_t err= fsp_reserve_free_extents(&reserved, space, 1,
                                        FSP_NORMAL, &reserve_mtr);
  reserve_mtr.commit();
  if (err != DB_SUCCESS)
    return FIL_NULL;

  mtr_t mtr{nullptr};
  mtr.start();
  index->set_modified(mtr);
  if (!index->lock.s_lock_try()) {
    mtr.commit();
    space->release_free_extents(reserved);
    return FIL_NULL;
  }
  mtr.memo_push(&index->lock, MTR_MEMO_S_LOCK);
  if (index->page == FIL_NULL || !index->table->space) {
    mtr.commit();
    space->release_free_extents(reserved);
    return FIL_NULL;
  }
  const ulint level= kind == blink_page_kind::LEAF ? 0 : 1;
  buf_block_t *block= btr_page_alloc(index, 0, FSP_NO_DIR, level,
                                     &mtr, &mtr, &err);
  uint32_t page_no= FIL_NULL;
  if (block) {
    btr_page_create(block, nullptr, index, level, &mtr);
    page_no= block->page.id().page_no();
  }
  mtr.commit();
  space->release_free_extents(reserved);
  return page_no;
}

static void blink_free_page(dict_index_t *index, uint32_t page_no) noexcept
{
  if (index->page == FIL_NULL || !index->table->space)
    return;
  mtr_t mtr{nullptr};
  mtr.start();
  index->set_modified(mtr);
  mtr_s_lock_index(index, &mtr);
  dberr_t err= DB_SUCCESS;
  if (!btr_root_block_get(index, RW_X_LATCH, &mtr, &err)) {
    mtr.commit();
    return;
  }
  buf_block_t *block= btr_block_get(*index, page_no, RW_X_LATCH, &mtr, &err);
  if (block)
    static_cast<void>(btr_page_free(index, block, &mtr));
  mtr.commit();
}

static bool blink_refill_one(blink_pool_entry_t *entry) noexcept
{
  for (size_t slot= 0; slot < 2; ++slot) {
    {
      std::lock_guard<std::mutex> guard(entry->pool.mutex);
      const size_t size= entry->pool.pages[slot].size();
      if (!entry->pool.refilling[slot] && size < blink_low_watermark[slot])
        entry->pool.refilling[slot]= true;
      if (!entry->pool.refilling[slot])
        continue;
      if (size >= blink_high_watermark[slot]) {
        entry->pool.refilling[slot]= false;
        continue;
      }
    }
    const auto kind= slot ? blink_page_kind::INTERNAL : blink_page_kind::LEAF;
    const uint32_t page_no= blink_alloc_page(entry->index, kind);
    if (page_no == FIL_NULL)
      return false;
    {
      std::lock_guard<std::mutex> guard(entry->pool.mutex);
      entry->pool.pages[slot].push_back(page_no);
      if (entry->pool.pages[slot].size() >= blink_high_watermark[slot])
        entry->pool.refilling[slot]= false;
    }
    return true;
  }
  return false;
}

static void blink_reclaim(blink_pool_entry_t *entry) noexcept
{
  std::deque<uint32_t> pages[2];
  {
    std::lock_guard<std::mutex> guard(entry->pool.mutex);
    pages[0].swap(entry->pool.pages[0]);
    pages[1].swap(entry->pool.pages[1]);
  }
  for (auto &queue : pages)
    while (!queue.empty()) {
      blink_free_page(entry->index, queue.front());
      queue.pop_front();
    }
  {
    std::lock_guard<std::mutex> guard(entry->reclaim_mutex);
    entry->reclaimed= true;
  }
  entry->reclaim_cv.notify_all();
}

static void blink_allocator_main() noexcept
{
  bool immediate= false;
  for (;;) {
    blink_pending_splits_process();
    blink_pool_entry_t *entry= nullptr;
    blink_pool_entry_t *reclaim= nullptr;
    {
      std::unique_lock<std::mutex> lock(blink_registry_mutex);
      if (!immediate)
        blink_registry_cv.wait_for(lock, std::chrono::milliseconds(100));
      immediate= false;
      if (!blink_reclaim_queue.empty()) {
        reclaim= blink_reclaim_queue.front();
        blink_reclaim_queue.pop_front();
      } else if (blink_allocator_shutdown.load())
        break;
      else if (!blink_registry.empty()) {
        blink_registry_cursor%= blink_registry.size();
        auto it= blink_registry.begin();
        std::advance(it, blink_registry_cursor++);
        if (!it->second->shutdown.load()) {
          entry= it->second.get();
          entry->users.fetch_add(1);
        }
      }
    }
    if (reclaim) {
      blink_reclaim(reclaim);
      immediate= true;
    }
    if (entry) {
      immediate= blink_refill_one(entry);
      blink_page_pool_unpin(entry);
    }
    if (blink_allocator_shutdown.load() && !reclaim && !entry)
      break;
  }
}

void blink_page_pool_unregister(dict_index_t *index) noexcept
{
  blink_pending_splits_remove(index);
  std::unique_ptr<blink_pool_entry_t> entry;
  bool async_reclaim= false;
  {
    std::unique_lock<std::mutex> lock(blink_registry_mutex);
    auto it= blink_registry.find(index);
    if (it == blink_registry.end()) {
      index->blink_page_pool= nullptr;
      return;
    }
    it->second->shutdown.store(true);
    entry= std::move(it->second);
    blink_registry.erase(it);
    index->blink_page_pool= nullptr;
    blink_registry_cv.wait(lock, [&entry] { return !entry->users.load(); });
    async_reclaim= blink_allocator_running;
    if (async_reclaim) {
      {
        std::lock_guard<std::mutex> reclaim_guard(entry->reclaim_mutex);
        entry->reclaimed= false;
      }
      blink_reclaim_queue.push_back(entry.get());
    }
  }
  if (async_reclaim) {
    blink_registry_cv.notify_one();
    std::unique_lock<std::mutex> lock(entry->reclaim_mutex);
    entry->reclaim_cv.wait(lock, [&entry] { return entry->reclaimed; });
  } else
    blink_reclaim(entry.get());
}

void blink_page_pool_thread_start() noexcept
{
  std::lock_guard<std::mutex> guard(blink_registry_mutex);
  if (blink_allocator_thread.joinable())
    return;
  blink_allocator_shutdown.store(false);
  blink_allocator_running= true;
  blink_allocator_thread= std::thread(blink_allocator_main);
}

void blink_page_pool_thread_stop() noexcept
{
  {
    std::lock_guard<std::mutex> guard(blink_registry_mutex);
    if (!blink_allocator_thread.joinable())
      return;
    blink_allocator_shutdown.store(true);
  }
  blink_registry_cv.notify_all();
  blink_allocator_thread.join();
  std::deque<blink_pool_entry_t*> entries;
  {
    std::lock_guard<std::mutex> guard(blink_registry_mutex);
    blink_allocator_running= false;
    for (auto &item : blink_registry) {
      item.second->users.fetch_add(1);
      entries.push_back(item.second.get());
    }
  }
  for (blink_pool_entry_t *entry : entries) {
    blink_reclaim(entry);
    blink_page_pool_unpin(entry);
  }
}

void blink_page_pool_query_depth(size_t *leaf, size_t *internal) noexcept
{
  *leaf= *internal= 0;
  std::lock_guard<std::mutex> registry_guard(blink_registry_mutex);
  for (const auto &item : blink_registry) {
    std::lock_guard<std::mutex> pool_guard(item.second->pool.mutex);
    *leaf+= item.second->pool.pages[0].size();
    *internal+= item.second->pool.pages[1].size();
  }
}
