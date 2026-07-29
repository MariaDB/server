/*
   Unit tests for the key recovery of heap_update().

   heap_update() moves every changed key entry to its new key value before it
   updates the record itself, so a failure raised afterwards leaves the index
   describing a record image that is not in the table.  The recovery at the
   err: label moves those entries back for the errors it knows how to recover
   from, and marks the table crashed when it cannot restore a key, so that no
   later statement can read from or write to an index that no longer
   describes the data.

   The failure used here - an rb-tree node allocation failure - is injected
   with DBUG keywords inside tree_insert(), so these tests only run on debug
   builds.  The keyword lists always carry the inert "heap_test_guard"
   keyword: a keyword list that becomes empty while debugging is on matches
   every keyword and would turn on all of the debug output.
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>
#include "heap.h"
#include "heapdef.h"

/*
  Record layout: (null bitmap, int4 a, int4 b)
    byte 0:     null bitmap
    bytes 1-4:  a
    bytes 5-8:  b
*/
#define REC_LENGTH  9
#define A_OFFSET    1
#define B_OFFSET    5

/* Never a valid errkey value, so it shows whether errkey was written to */
#define ERRKEY_UNSET (-2)

/*
  Everything below is driven by the injected failures, so a build without
  DBUG has nothing to call it with.  The tests and the helpers only they use
  are left out of such a build entirely, rather than only left uncalled,
  which would warn about every one of them.
*/
#ifndef DBUG_OFF

static void build_rec(uchar *rec, int32 a, int32 b)
{
  memset(rec, 0, REC_LENGTH);
  int4store(rec + A_OFFSET, a);
  int4store(rec + B_OFFSET, b);
}


/*
  Create a table with 'keys' keys: key 0 over column a and key 1 over
  column b.  Only key 0 can be made unique; the tests that need a
  duplicate use a single key.
*/

static int create_and_open(const char *name, uint keys,
                           enum ha_key_alg alg_a, enum ha_key_alg alg_b,
                           uint flag_a, HP_SHARE **share, HP_INFO **info)
{
  HP_KEYDEF keydef[2];
  HA_KEYSEG keyseg[2];
  HP_CREATE_INFO ci;
  my_bool unused;
  uint i;

  memset(keyseg, 0, sizeof(keyseg));
  memset(keydef, 0, sizeof(keydef));
  for (i= 0; i < 2; i++)
  {
    keyseg[i].type=    HA_KEYTYPE_BINARY;
    keyseg[i].start=   i ? B_OFFSET : A_OFFSET;
    keyseg[i].length=  4;
    keyseg[i].charset= &my_charset_bin;

    keydef[i].keysegs= 1;
    keydef[i].seg=     &keyseg[i];
    keydef[i].length=  4;
  }
  keydef[0].algorithm= alg_a;
  keydef[0].flag=      flag_a;
  keydef[1].algorithm= alg_b;

  memset(&ci, 0, sizeof(ci));
  ci.keys=           keys;
  ci.keydef=         keydef;
  ci.reclength=      REC_LENGTH;
  ci.max_records=    1000;
  ci.min_records=    10;
  ci.max_table_size= 1024 * 1024;

  if (heap_create(name, &ci, share, &unused))
    return 1;
  if (!(*info= heap_open(name, 2)))
    return 1;
  heap_extra(*info, HA_EXTRA_NO_READCHECK);
  return 0;
}


static int position_on(HP_INFO *info, int keynr, int32 value, uchar *rec)
{
  uchar key[4];
  int4store(key, value);
  return heap_rkey(info, rec, keynr, key, (key_part_map) 1, HA_READ_KEY_EXACT);
}


static my_bool row_found(HP_INFO *info, int keynr, int32 value)
{
  uchar rec[REC_LENGTH];
  return position_on(info, keynr, value, rec) == 0;
}


/*
  Insert (1, 10) and (2, 20) and position on the first one through key 0.
*/

static int fill_and_position(HP_INFO *info, uchar *rec)
{
  build_rec(rec, 1, 10);
  if (heap_write(info, rec))
    return 1;
  build_rec(rec, 2, 20);
  if (heap_write(info, rec))
    return 1;
  build_rec(rec, 1, 10);
  return position_on(info, 0, 1, rec) != 0;
}


/*
  Test 1: a failed rb-tree node allocation is reported as such.

  The key is not unique, so a duplicate cannot even occur, yet before the
  fix the failure was reported as HA_ERR_FOUND_DUPP_KEY - with a fabricated
  duplicate value and a key that has no uniqueness to violate.  The failure
  is injected only once, so restoring the old key succeeds and the table
  stays usable.
*/

static void test_rb_alloc_failure_is_oom(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH], new_rec[REC_LENGTH];
  ulonglong index_length_before;
  int error;

  if (create_and_open("test_upd_oom", 1, HA_KEY_ALG_BTREE, HA_KEY_ALG_BTREE,
                      0, &share, &info) ||
      fill_and_position(info, rec))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }

  index_length_before= share->index_length;
  build_rec(new_rec, 11, 10);
  info->errkey= ERRKEY_UNSET;

  DBUG_PUSH("d,once_simulate_tree_insert_oom,heap_test_guard");
  error= heap_update(info, rec, new_rec);
  DBUG_POP();

  ok(error == HA_ERR_OUT_OF_MEM,
     "allocation failure reported as out of memory, not as a duplicate "
     "(got %d)", error);
  ok(info->errkey == -1,
     "errkey says no key error for a non-duplicate failure (got %d)",
     info->errkey);
  ok(share->index_length == index_length_before,
     "index size unchanged after the recovery");
  ok(!heap_is_crashed(share), "table not crashed: the recovery succeeded");
  ok(heap_check_heap(info, 0) == 0, "index consistent after the recovery");
  ok(row_found(info, 0, 1), "row still found through its old key value");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 2: a real duplicate is still reported as a duplicate.

  Guards the classification above against reporting every rb-tree insert
  failure as an allocation failure.
*/

static void test_rb_duplicate_still_reported(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH], new_rec[REC_LENGTH];
  int error;

  if (create_and_open("test_upd_dupp", 1, HA_KEY_ALG_BTREE, HA_KEY_ALG_BTREE,
                      HA_NOSAME, &share, &info) ||
      fill_and_position(info, rec))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(3, "setup failed");
    return;
  }

  /* a=2 already exists on the second row */
  build_rec(new_rec, 2, 10);
  info->errkey= ERRKEY_UNSET;

  error= heap_update(info, rec, new_rec);

  ok(error == HA_ERR_FOUND_DUPP_KEY,
     "duplicate key still reported as a duplicate (got %d)", error);
  ok(info->errkey == 0, "errkey names the duplicate key (got %d)",
     info->errkey);
  ok(heap_check_heap(info, 0) == 0, "index consistent after the recovery");
  ok(row_found(info, 0, 1), "row still found through its old key value");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 3: a key that was already moved is moved back.

  Key 0 (hash) is moved to its new value before key 1 (rb-tree) fails, so
  the recovery has to move key 0 back to the value the record still holds.
*/

static void test_earlier_key_is_moved_back(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH], new_rec[REC_LENGTH];
  int error;

  if (create_and_open("test_upd_earlier", 2, HA_KEY_ALG_HASH,
                      HA_KEY_ALG_BTREE, 0, &share, &info) ||
      fill_and_position(info, rec))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(4, "setup failed");
    return;
  }

  build_rec(new_rec, 11, 110);

  DBUG_PUSH("d,once_simulate_tree_insert_oom,heap_test_guard");
  error= heap_update(info, rec, new_rec);
  DBUG_POP();

  ok(error == HA_ERR_OUT_OF_MEM, "update failed with out of memory (got %d)",
     error);
  ok(!heap_is_crashed(share), "table not crashed: the recovery succeeded");
  ok(heap_check_heap(info, 0) == 0, "both indexes consistent after recovery");
  ok(row_found(info, 0, 1), "hash key moved back to the old value");
  ok(row_found(info, 1, 10), "rb-tree key holds the old value");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 4: a failed recovery marks the table crashed and shuts it down.

  The allocation failure is left switched on, so restoring the old key 1
  (rb-tree) entry fails as well.  The already moved key 0 (hash) then no
  longer describes the record, so the table is marked crashed: every read
  and write on it fails with HA_ERR_CRASHED and heap_check_heap() reports
  it as damaged, until the table is emptied, which rebuilds the indexes
  from nothing.
*/

static void test_failed_recovery_marks_crashed(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH], new_rec[REC_LENGTH];
  int error;

  if (create_and_open("test_upd_crashed", 2, HA_KEY_ALG_HASH,
                      HA_KEY_ALG_BTREE, 0, &share, &info) ||
      fill_and_position(info, rec))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  build_rec(new_rec, 11, 110);

  DBUG_PUSH("d,simulate_tree_insert_oom,heap_test_guard");
  error= heap_update(info, rec, new_rec);
  DBUG_POP();

  ok(error == HA_ERR_OUT_OF_MEM, "update failed with out of memory (got %d)",
     error);
  ok(heap_is_crashed(share),
     "table marked crashed: one key could not be restored");
  ok(heap_check_heap(info, 0) != 0, "heap_check_heap() reports the damage");

  build_rec(rec, 1, 10);
  build_rec(new_rec, 3, 30);
  ok(heap_update(info, rec, new_rec) == HA_ERR_CRASHED,
     "update on a crashed table is refused");
  ok(!row_found(info, 0, 1) && my_errno == HA_ERR_CRASHED,
     "key read on a crashed table is refused");
  build_rec(rec, 4, 40);
  ok(heap_write(info, rec) == HA_ERR_CRASHED,
     "write on a crashed table is refused");
  ok(heap_scan_init(info) == HA_ERR_CRASHED,
     "scan on a crashed table is refused");

  heap_clear(info);
  ok(!heap_is_crashed(share), "emptying the table clears the crashed state");
  build_rec(rec, 5, 50);
  ok(heap_write(info, rec) == 0 && row_found(info, 0, 5),
     "the emptied table accepts and finds rows again");

  heap_drop_table(info);
  heap_close(info);
}

#endif /* !DBUG_OFF */


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_update");

#ifdef DBUG_OFF
  skip_all("the failures under test are injected with DBUG keywords");
#else
  plan(24);

  diag("Test 1: rb-tree allocation failure is not a duplicate key");
  test_rb_alloc_failure_is_oom();

  diag("Test 2: a real duplicate is still reported as a duplicate");
  test_rb_duplicate_still_reported();

  diag("Test 3: an already moved key is moved back");
  test_earlier_key_is_moved_back();

  diag("Test 4: a failed recovery marks the table crashed");
  test_failed_recovery_marks_crashed();
#endif

  my_end(0);
  return exit_status();
}
