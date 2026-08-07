/*
   Unit tests for HP_BLOCK allocation sizing in init_block().

   A HEAP table's max_records is normally derived from the memory
   ceiling (max_heap_table_size / tmp_memory_table_size), not from any
   estimate of how many rows the table will hold.  The first (and every)
   block used to be sized as max_records/heap_allocation_parts records,
   so a large ceiling produced a multi-hundred-MB allocation on the
   first row write even for a table holding a handful of rows.  Such
   allocations are served by mmap and unmapped on free, causing
   per-statement mmap/munmap churn and mmap_lock contention for
   create-and-drop temporary tables.

   These tests verify:
   - the ceiling-derived block size is capped so it stays in the range
     that memory allocators recycle without returning pages to the OS;
   - an explicit min_records pre-sizing hint (CREATE TABLE ... MIN_ROWS)
     still overrides the cap;
   - an explicit min_records below the cap does not defeat the cap;
   - an explicit min_records above max_records is unreachable and is
     clamped to the ceiling instead of pre-sizing past it;
   - small tables are sized exactly as before;
   - max_records=0 keeps meaning "no row limit" even though block
     sizing needs a concrete row count;
   - a capped table remains fully functional past the first block.
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>
#include "heap.h"
#include "heapdef.h"

#define REC_LENGTH   100
#define INT_OFFSET   1

/* Largest block allocation init_block() may produce from a
   ceiling-derived max_records (heap_max_allocation_block) */
#define BLOCK_SIZE_CAP (4*1024*1024)

static void build_record(uchar *rec, int32 int_val)
{
  memset(rec, 0, REC_LENGTH);
  int4store(rec + INT_OFFSET, int_val);
}


static int create_table(const char *name, uint keys, uint reclength,
                        ulong min_records, ulong max_records,
                        HP_KEYDEF *keydef, HP_SHARE **share)
{
  HP_CREATE_INFO ci;
  my_bool unused;

  memset(&ci, 0, sizeof(ci));
  ci.keys=           keys;
  ci.keydef=         keydef;
  ci.reclength=      reclength;
  ci.max_records=    max_records;
  ci.min_records=    min_records;
  ci.max_table_size= 17179869184ULL;            /* 16G ceiling */

  return heap_create(name, &ci, share, &unused);
}


static void init_int_keydef(HP_KEYDEF *keydef, HA_KEYSEG *keyseg)
{
  memset(keyseg, 0, sizeof(*keyseg));
  keyseg->type=    HA_KEYTYPE_BINARY;
  keyseg->start=   INT_OFFSET;
  keyseg->length=  4;
  keyseg->charset= &my_charset_bin;

  memset(keydef, 0, sizeof(*keydef));
  keydef->keysegs=   1;
  keydef->seg=       keyseg;
  keydef->algorithm= HA_KEY_ALG_HASH;
  keydef->flag=      HA_NOSAME;
  keydef->length=    4;
}


/*
  Test: ceiling-derived max_records must not inflate block allocations.

  max_records = 100M simulates a multi-GB memory ceiling divided by the
  per-row size.  Without the cap the record block came out at
  max_records/16 * recbuffer ~ 650MB (rounded up to 1GB) and the hash
  key block at max_records/16 * sizeof(HASH_INFO) ~ 200MB+.  Both must
  stay within BLOCK_SIZE_CAP.

  The table must also remain fully functional across the first-block
  boundary: with recbuffer = 104 a capped block holds ~40K records, so
  writing 45K rows exercises hp_get_new_block() beyond the first block
  at the capped geometry.
*/

static void test_huge_max_records_capped(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  uchar rec[REC_LENGTH];
  ulong i;
  ulong write_failures= 0;
  const ulong rows= 45000;

  init_int_keydef(&keydef, &keyseg);

  if (create_table("test_block_cap", 1, REC_LENGTH, 0, 100000000UL, &keydef,
                   &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }
  ok(1, "created table with max_records=100M");

  ok(share->block.alloc_size <= BLOCK_SIZE_CAP,
     "record block alloc_size capped (got %zu, cap %d)",
     share->block.alloc_size, BLOCK_SIZE_CAP);

  ok(share->keydef[0].block.alloc_size <= BLOCK_SIZE_CAP,
     "hash key block alloc_size capped (got %zu, cap %d)",
     share->keydef[0].block.alloc_size, BLOCK_SIZE_CAP);

  ok(share->block.records_in_block >= 10,
     "capped block still holds >= 10 records (got %lu)",
     share->block.records_in_block);

  if (!(info= heap_open("test_block_cap", 2)))
  {
    ok(0, "heap_open failed: %d", my_errno);
    skip(4, "open failed");
    return;
  }
  heap_extra(info, HA_EXTRA_NO_READCHECK);

  for (i= 0; i < rows; i++)
  {
    build_record(rec, (int32) i);
    if (heap_write(info, rec))
      write_failures++;
  }
  ok(write_failures == 0, "wrote %lu rows without error (%lu failures)",
     rows, write_failures);

  ok(share->block.last_allocated > share->block.records_in_block,
     "writes crossed the first block boundary "
     "(last_allocated %lu, records_in_block %lu)",
     share->block.last_allocated, share->block.records_in_block);

  {
    uchar key[4];
    uchar read_buf[REC_LENGTH];
    int4store(key, 12345);
    ok(heap_rkey(info, read_buf, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "read row 12345 back via hash key");
    ok(sint4korr(read_buf + INT_OFFSET) == 12345,
       "row 12345 content matches");
  }

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates multi-block capped table");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: keyless table (the I_S/SHOW materialization shape) is capped.
*/

static void test_huge_max_records_keyless_capped(void)
{
  HP_SHARE *share;
  HP_INFO *info;

  if (create_table("test_block_cap_nokey", 0, REC_LENGTH, 0, 100000000UL,
                   NULL, &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(1, "setup failed");
    return;
  }
  ok(1, "created keyless table with max_records=100M");

  ok(share->block.alloc_size <= BLOCK_SIZE_CAP,
     "record block alloc_size capped (got %zu, cap %d)",
     share->block.alloc_size, BLOCK_SIZE_CAP);

  if ((info= heap_open("test_block_cap_nokey", 2)))
  {
    heap_drop_table(info);
    heap_close(info);
  }
}


/*
  Test: an explicit min_records hint (CREATE TABLE ... MIN_ROWS) is a
  user request to pre-size the table and must override the cap.
  1M records * recbuffer 104 ~ 104MB, rounded up to 128MB.
*/

static void test_min_records_overrides_cap(void)
{
  HP_SHARE *share;
  HP_INFO *info;

  if (create_table("test_block_minrows", 0, REC_LENGTH, 1000000UL,
                   100000000UL, NULL, &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(1, "setup failed");
    return;
  }
  ok(1, "created table with min_records=1M");

  ok(share->block.alloc_size > BLOCK_SIZE_CAP,
     "min_records pre-sizing overrides the cap (got %zu)",
     share->block.alloc_size);

  if ((info= heap_open("test_block_minrows", 2)))
  {
    heap_drop_table(info);
    heap_close(info);
  }
}


/*
  Test: small tables keep their historical sizing (the cap must be a
  pure upper bound and never change small-table behavior).
  max_records=1000: block memory = max(62 * 104 + extra, 16384) = 16K.
*/

static void test_small_table_sizing_unchanged(void)
{
  HP_SHARE *share;
  HP_INFO *info;

  if (create_table("test_block_small", 0, REC_LENGTH, 0, 1000UL, NULL,
                   &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(1, "setup failed");
    return;
  }
  ok(1, "created table with max_records=1000");

  ok(share->block.alloc_size <= 16384,
     "small table block stays at heap_min_allocation_block (got %zu)",
     share->block.alloc_size);

  if ((info= heap_open("test_block_small", 2)))
  {
    heap_drop_table(info);
    heap_close(info);
  }
}


/*
  Test: wide rows must not defeat the cap through the defaulted
  min_records.

  When the caller passes min_records=0 (every internal temporary
  table), init_block() defaults it to 1000 ("optimize for 1000 rows").
  That heuristic default is not a user pre-sizing request and must not
  override the cap the way an explicit MIN_ROWS does.  Once recbuffer
  exceeds heap_max_allocation_block/1000 (~4KB), cap_records drops
  below 1000 and the defaulted value takes over: 8KB rows give
  1000 * 8KB ~ 8MB, rounded up to a 16MB block.  Capped, the block must
  stay within BLOCK_SIZE_CAP (511 records * 8KB fits exactly).
  max_records = 2M simulates a 16GB ceiling divided by the row size.
*/

static void test_wide_row_capped(void)
{
  HP_SHARE *share;
  HP_INFO *info;

  if (create_table("test_block_wide", 0, 8192, 0, 2097152UL, NULL, &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(1, "setup failed");
    return;
  }
  ok(1, "created keyless table with reclength=8K, max_records=2M");

  ok(share->block.alloc_size <= BLOCK_SIZE_CAP,
     "8K-row block alloc_size capped (got %zu, cap %d)",
     share->block.alloc_size, BLOCK_SIZE_CAP);

  if ((info= heap_open("test_block_wide", 2)))
  {
    heap_drop_table(info);
    heap_close(info);
  }
}


/*
  Test: rows near the 64KB row-size limit (a single max-width table
  materialized into a tmp table) are capped too.  Uncapped, the
  defaulted min_records of 1000 gives 1000 * 64KB ~ 64MB, rounded up
  to a 128MB block.  Capped: 63 records * 64KB fits in 4MB.
  max_records = 262144 simulates a 16GB ceiling / 64KB rows.
*/

static void test_very_wide_row_capped(void)
{
  HP_SHARE *share;
  HP_INFO *info;

  if (create_table("test_block_vwide", 0, 65535, 0, 262144UL, NULL, &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(1, "setup failed");
    return;
  }
  ok(1, "created keyless table with reclength=64K, max_records=256K");

  ok(share->block.alloc_size <= BLOCK_SIZE_CAP,
     "64K-row block alloc_size capped (got %zu, cap %d)",
     share->block.alloc_size, BLOCK_SIZE_CAP);

  if ((info= heap_open("test_block_vwide", 2)))
  {
    heap_drop_table(info);
    heap_close(info);
  }
}


/*
  Test: a row wider than the cap itself cannot honor the cap
  (cap_records = 0); the 10-records-per-block floor takes over and the
  block degrades to 10 * recbuffer, NOT to the defaulted 1000-record
  geometry.  5MB rows: floor gives 10 * 5MB = 50MB rounded up to 64MB;
  the defaulted min_records would give 1000 * 5MB ~ 5GB, clamped to
  INT_MAX32 and rounded to a 2GB block.
  max_records = 3276 simulates a 16GB ceiling / 5MB rows.
*/

static void test_row_wider_than_cap(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  const uint row_5mb= 5*1024*1024;

  if (create_table("test_block_hugerow", 0, row_5mb, 0, 3276UL, NULL,
                   &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(2, "setup failed");
    return;
  }
  ok(1, "created keyless table with reclength=5M, max_records=3276");

  ok(share->block.alloc_size <= 64*1024*1024,
     "block for rows wider than the cap degrades to the 10-record floor "
     "(got %zu)", share->block.alloc_size);

  ok(share->block.records_in_block >= 10,
     "block still holds >= 10 records (got %lu)",
     share->block.records_in_block);

  if ((info= heap_open("test_block_hugerow", 2)))
  {
    heap_drop_table(info);
    heap_close(info);
  }
}


/*
  Test: an explicit min_records equal to the 1000-row default still
  overrides the cap for wide rows.  This pins down the distinction the
  cap must make: explicit MIN_ROWS pre-sizes past the cap, the
  defaulted 1000 does not.  1000 records * 8KB ~ 8MB > cap.
*/

static void test_explicit_min_records_wide_row(void)
{
  HP_SHARE *share;
  HP_INFO *info;

  if (create_table("test_block_wide_minrows", 0, 8192, 1000UL, 2097152UL,
                   NULL, &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(1, "setup failed");
    return;
  }
  ok(1, "created 8K-row table with explicit min_records=1000");

  ok(share->block.alloc_size > BLOCK_SIZE_CAP,
     "explicit min_records pre-sizing overrides the cap for wide rows "
     "(got %zu)", share->block.alloc_size);

  if ((info= heap_open("test_block_wide_minrows", 2)))
  {
    heap_drop_table(info);
    heap_close(info);
  }
}


/*
  Walk the whole min_records / cap interaction at one ceiling-derived
  max_records, so the boundaries between the regimes are pinned down
  together rather than one at a time.

  max_records 1M with 104-byte rows puts max_records/heap_allocation_parts
  (62500 records) above cap_records (40319), so the cap is live for every
  case here and only min_records decides the outcome:

    - no min_records          -> capped (the heuristic must not override)
    - min_records below cap   -> capped (too small to raise the block)
    - cap < min_records < max -> pre-sized to min_records, past the cap
    - min_records >= max      -> unreachable, clamped to max_records
*/

#define BOUNDARY_MAX_RECORDS  1000000UL

static void test_min_records_cap_boundary(void)
{
  static const struct
  {
    const char *name;
    ulong       min_records;
    size_t      expected_alloc;
    const char *what;
  } cases[]=
  {
    { "test_block_bnd_none",   0UL,
      4194272,   "no min_records is capped" },
    { "test_block_bnd_small",  10000UL,
      4194272,   "min_records below the cap is capped" },
    { "test_block_bnd_mid",    200000UL,
      33554400,  "min_records above the cap pre-sizes past it" },
    { "test_block_bnd_over",   100000000UL,
      134217696, "min_records above max_records clamps to the ceiling" }
  };
  uint i;

  for (i= 0; i < array_elements(cases); i++)
  {
    HP_SHARE *share;
    HP_INFO *info;

    if (create_table(cases[i].name, 0, REC_LENGTH, cases[i].min_records,
                     BOUNDARY_MAX_RECORDS, NULL, &share))
    {
      ok(0, "setup failed for %s: %d", cases[i].what, my_errno);
      continue;
    }

    ok(share->block.alloc_size == cases[i].expected_alloc,
       "%s (got %zu, expected %zu)", cases[i].what,
       share->block.alloc_size, cases[i].expected_alloc);

    if ((info= heap_open(cases[i].name, 2)))
    {
      heap_drop_table(info);
      heap_close(info);
    }
  }
}


/*
  Test: the ceiling clamp applies to the hash key block too.  An
  unreachable MIN_ROWS inflates DATA_LENGTH and INDEX_LENGTH alike,
  because both blocks run through init_block().  sizeof(HASH_INFO) is
  24 on LP64 and 12 on ILP32, so the key block has its own recbuffer,
  its own cap_records and its own expected size -- assert both blocks,
  not just the record one.
*/

static void test_min_records_above_max_records_keyed(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;

  init_int_keydef(&keydef, &keyseg);

  if (create_table("test_block_bnd_keyed", 1, REC_LENGTH, 100000000UL,
                   BOUNDARY_MAX_RECORDS, &keydef, &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(2, "setup failed");
    return;
  }

  ok(share->block.alloc_size == 134217696,
     "record block clamped to the ceiling (got %zu)",
     share->block.alloc_size);

  /*
    1M * sizeof(HASH_INFO) + extra, rounded up to the next power of two:
    32MB on LP64, 16MB on ILP32.  The record block above rounds to the
    same size on both, but halving the key record halves memory_needed,
    which lands a whole power of two lower.
  */
  ok(share->keydef[0].block.alloc_size ==
     (SIZEOF_CHARP == 8 ? 33554400 : 16777184),
     "hash key block clamped to the ceiling (got %zu)",
     share->keydef[0].block.alloc_size);

  if ((info= heap_open("test_block_bnd_keyed", 2)))
  {
    heap_drop_table(info);
    heap_close(info);
  }
}


/*
  Test: max_records=0 means "no row limit", not "1000 rows".

  Block sizing needs a concrete row count, so heap_create() derives one
  when the caller passes max_records=0.  That derived value must not
  reach share->max_records: hp_alloc_from_tail() only skips the row
  limit check while max_records is 0, so storing the derived default
  would turn an unlimited table into one that reports
  HA_ERR_RECORD_FILE_FULL a few blocks in.
*/

static void test_no_max_records_is_unlimited(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  ulong i;
  ulong write_failures= 0;
  const ulong rows= 5000;

  if (create_table("test_block_nomax", 0, REC_LENGTH, 0, 0, NULL, &share))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(3, "setup failed");
    return;
  }
  ok(1, "created keyless table with max_records=0");

  ok(share->max_records == 0,
     "max_records stays 0, meaning no row limit (got %lu)",
     share->max_records);

  ok(share->block.alloc_size == 16352,
     "block sized from the derived default (got %zu, expected 16352)",
     share->block.alloc_size);

  if (!(info= heap_open("test_block_nomax", 2)))
  {
    ok(0, "heap_open failed: %d", my_errno);
    skip(1, "open failed");
    return;
  }
  heap_extra(info, HA_EXTRA_NO_READCHECK);

  for (i= 0; i < rows; i++)
  {
    build_record(rec, (int32) i);
    if (heap_write(info, rec))
      write_failures++;
  }
  ok(write_failures == 0,
     "wrote %lu rows, well past the derived default, without error "
     "(%lu failures)", rows, write_failures);

  heap_drop_table(info);
  heap_close(info);
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_block_size");
  plan(34);

  diag("Test 1: ceiling-derived max_records capped (keyed table)");
  test_huge_max_records_capped();

  diag("Test 2: ceiling-derived max_records capped (keyless table)");
  test_huge_max_records_keyless_capped();

  diag("Test 3: min_records pre-sizing hint overrides cap");
  test_min_records_overrides_cap();

  diag("Test 4: small table sizing unchanged");
  test_small_table_sizing_unchanged();

  diag("Test 5: wide rows (8K) capped despite defaulted min_records");
  test_wide_row_capped();

  diag("Test 6: very wide rows (64K) capped despite defaulted min_records");
  test_very_wide_row_capped();

  diag("Test 7: rows wider than the cap degrade to the 10-record floor");
  test_row_wider_than_cap();

  diag("Test 8: explicit min_records still overrides cap for wide rows");
  test_explicit_min_records_wide_row();

  diag("Test 9: min_records / cap boundary walk");
  test_min_records_cap_boundary();

  diag("Test 10: unreachable min_records clamps record and key blocks");
  test_min_records_above_max_records_keyed();

  diag("Test 11: max_records=0 stays unlimited");
  test_no_max_records_is_unlimited();

  my_end(0);
  return exit_status();
}
