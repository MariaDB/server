#include "tap.h"
#include "ut0rbt.h"
#include "ut0new.h"

const size_t alloc_max_retries= 0;
void os_thread_sleep(ulint) { abort(); }
void ut_dbg_assertion_failed(const char *, const char *, unsigned)
{ abort(); }
namespace ib { fatal_or_error::~fatal_or_error() { IF_WIN(__debugbreak(),abort()); } }
#ifdef UNIV_PFS_MEMORY
PSI_memory_key mem_key_other, mem_key_std;
PSI_memory_key ut_new_get_key_by_file(uint32_t) { return mem_key_std; }
#endif

static const uint64_t doc_ids[]=
{
  103571,     104018,     106821,     108647,     109352,     109379,
  110325,     122868,     210682130,  231275441,  234172769,  366236849,
  526467159,  1675241735, 1675243405, 1947751899, 1949940363, 2033691953,
  2148227299, 2256289791, 2294223591, 2367501260, 2792700091, 2792701220,
  2817121627, 2820680352, 2821165664, 3253312130, 3404918378, 3532599429,
  3538712078, 3539373037, 3546479309, 3566641838, 3580209634, 3580871267,
  3693930556, 3693932734, 3693932983, 3781949558, 3839877411, 3930968983
};

static int fts_doc_id_cmp(const void *p1, const void *p2)
{
  uint64_t a= *static_cast<const uint64_t*>(p1),
    b= *static_cast<const uint64_t*>(p2);
  return b > a ? -1 : a > b;
}


static int fts_doc_id_buggy_cmp(const void *p1, const void *p2)
{
  return int(*static_cast<const uint64_t*>(p1) -
             *static_cast<const uint64_t*>(p2));
}

typedef int (*comparator) (const void*, const void*);

static void rbt_populate(ib_rbt_t *rbt)
{
  ib_rbt_bound_t parent;
  for (const uint64_t &doc_id : doc_ids)
  {
    if (rbt_search(rbt, &parent, &doc_id))
      rbt_add_node(rbt, &parent, &doc_id);
  }
}

static void rbt_populate2(ib_rbt_t *rbt)
{
  for (const uint64_t &doc_id : doc_ids)
    rbt_insert(rbt, &doc_id, &doc_id);
}

static bool rbt_search_all(ib_rbt_t *rbt)
{
  ib_rbt_bound_t parent;
  for (const uint64_t &doc_id : doc_ids)
    if (rbt_search(rbt, &parent, &doc_id))
      return false;
  return true;
}

static void rbt_test(comparator cmp, bool buggy)
{
  ib_rbt_t *rbt= rbt_create(sizeof(uint64_t), cmp);
  rbt_populate(rbt);
  ok(rbt_search_all(rbt) != buggy, "search after populate");
  rbt_free(rbt);
  rbt= rbt_create(sizeof(uint64_t), cmp);
  rbt_populate2(rbt);
  ok(rbt_search_all(rbt) != buggy, "search after populate2");
  rbt_free(rbt);
}

int main ()
{
  rbt_test(fts_doc_id_buggy_cmp, true);
  rbt_test(fts_doc_id_cmp, false);
}
