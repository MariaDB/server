/*
   Unit tests for free-list contiguity detection in hp_write_one_blob().

   Verifies that Step 1 (free-list peek) correctly identifies contiguous
   groups of 3+ records, not just pairs.
*/

#include "hp_test_helpers.h"


/*
  Test: free-list contiguity detection finds groups > 2 records.

  Scenario:
    1. Insert a row with a 100-byte blob (needs 8 continuation records
       in Case B format: 1 header + 7 data records, recbuffer=16).
    2. Delete the row.  The continuation chain's 8 records form a
       contiguous group on the free list (pushed in ascending address
       order by hp_free_run_chain, so LIFO yields descending addresses).
       The primary record is pushed on top.
    3. Insert a new row with the same blob size.  The primary record
       reuses the old primary from the free list head.  The blob
       allocation (Step 1) should detect the remaining 8 contiguous
       continuation records as a single group and reuse them.
    4. Assert that block.last_allocated did NOT grow: all records
       came from the free list, nothing from the tail.

  With the prev_pos bug (prev_pos not updated in the contiguity loop),
  Step 1 only detects 2-record groups.  For a 100-byte blob:
    total_records_needed = 7
    min_run_records = min(7, max(ceil(128/16), 2)) = 7
  A 2-record group < 7 causes Step 1 to give up, falling to tail
  allocation, which grows last_allocated.
*/

static void test_freelist_contiguity_multirecord(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  ulong last_alloc_after_first_insert, last_alloc_after_delete;

  uchar blob_data[100];
  memset(blob_data, 'Q', sizeof(blob_data));
  blob_data[0]= '!';
  blob_data[99]= '?';

  if (create_and_open("test_freelist_cont", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(10, "setup failed");
    return;
  }

  /* Insert row with 100-byte blob */
  build_record(rec, 1, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert row 1 (100-byte blob)");

  last_alloc_after_first_insert= (ulong) share->block.last_allocated;
  ok(last_alloc_after_first_insert >= 9,
     "allocated >= 9 records: 1 primary + 8 continuation (got %lu)",
     last_alloc_after_first_insert);

  /* Delete row 1 */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 1 for deletion");
    ok(heap_delete(info, rec) == 0, "deleted row 1");
  }

  last_alloc_after_delete= (ulong) share->block.last_allocated;
  ok(last_alloc_after_delete == last_alloc_after_first_insert,
     "last_allocated unchanged after delete (%lu)",
     last_alloc_after_delete);

  ok(share->deleted == 1,
     "primary on free list, blob chains deferred (deleted=%lu)",
     (ulong) share->deleted);

  /* Insert row 2 with same blob size - should fully reuse free list */
  build_record(rec, 2, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert row 2 (100-byte blob, free-list reuse)");

  /*
    Key assertion: if contiguity detection works for groups > 2,
    the entire continuation chain is recovered from the free list
    without any tail allocation.
  */
  ok(share->block.last_allocated == last_alloc_after_delete,
     "last_allocated unchanged after reinsert: free list fully reused "
     "(before=%lu, after=%lu)",
     last_alloc_after_delete, (ulong) share->block.last_allocated);

  /* Verify data integrity of the reinserted row */
  {
    uchar key[4];
    uchar read_buf[REC_LENGTH];
    uint32 read_len;
    const uchar *read_ptr;

    int4store(key, 2);
    ok(heap_rkey(info, read_buf, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 2 for verification");
    read_len= uint2korr(read_buf + BLOB_OFFSET);
    memcpy(&read_ptr, read_buf + BLOB_OFFSET + BLOB_PACKLEN, sizeof(read_ptr));
    ok(read_len == 100, "blob length == 100 (got %u)", read_len);
    ok(memcmp(read_ptr, blob_data, 100) == 0, "blob data matches after free-list reuse");
  }

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after free-list reuse");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: free-list contiguity across multiple delete-reinsert cycles.

  Performs 3 rounds of insert-delete-reinsert with a 200-byte blob.
  In each round, last_allocated must not grow, proving that contiguity
  detection consistently reuses the freed continuation chain.
*/

static void test_freelist_contiguity_repeated_cycles(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  int round;

  uchar blob_data[200];
  memset(blob_data, 'R', sizeof(blob_data));

  if (create_and_open("test_freelist_cycles", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(12, "setup failed");
    return;
  }

  /* Initial insert to establish baseline */
  build_record(rec, 1, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "initial insert (200-byte blob)");

  for (round= 0; round < 3; round++)
  {
    uchar key[4];
    ulong alloc_before;

    /* Delete current row */
    int4store(key, round + 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "round %d: found row for deletion", round);
    ok(heap_delete(info, rec) == 0,
       "round %d: deleted row", round);
    alloc_before= (ulong) share->block.last_allocated;

    /* Reinsert with new key */
    build_record(rec, round + 2, blob_data, sizeof(blob_data));
    ok(heap_write(info, rec) == 0,
       "round %d: reinserted (free-list reuse)", round);
    ok(share->block.last_allocated == alloc_before,
       "round %d: last_allocated stable (before=%lu, after=%lu)",
       round, alloc_before, (ulong) share->block.last_allocated);
  }

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: Step 3 free-list scavenge fallback when tail is full.

  Fills the entire first HP_BLOCK leaf block with 0-byte-blob rows,
  then deletes every other row to create a heavily fragmented free
  list of non-contiguous individual slots.  Locks out tail allocation
  by setting max_table_size tight.

  Insert a row with a 50-byte blob.  Step 1 gives up (1-slot groups
  < min_run_records=4).  Step 2 fails (tail at block boundary with
  tight max_table_size).  Step 3 scavenges individual free-list
  slots, writing maximally fragmented Case C chains.
*/

static void test_freelist_scavenge_fallback(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  int32 i;
  int32 inserted, deleted_count;
  int32 records_in_block;
  ulong last_alloc_after_fill;

  uchar blob_data[50];
  memset(blob_data, 'F', sizeof(blob_data));
  blob_data[0]= '<';
  blob_data[49]= '>';

  if (create_and_open("test_scavenge", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  records_in_block= (int32) share->block.records_in_block;

  /* Fill the first leaf block with 0-byte blob rows (1 record each) */
  for (i= 0, inserted= 0; i < records_in_block; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    if (heap_write(info, rec) != 0)
      break;
    inserted++;
  }

  last_alloc_after_fill= (ulong) share->block.last_allocated;
  ok(inserted == records_in_block,
     "filled block: %d of %d rows inserted",
     inserted, records_in_block);

  /* Delete every other row -- non-contiguous fragmentation */
  deleted_count= 0;
  for (i= 0; i < inserted; i+= 2)
  {
    uchar key[4];
    int4store(key, i);
    if (heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0 &&
        heap_delete(info, rec) == 0)
      deleted_count++;
  }
  ok(deleted_count == inserted / 2,
     "deleted %d rows (every other)", deleted_count);
  ok(share->deleted == (ulong) deleted_count,
     "share->deleted == %d", deleted_count);

  /* Lock out tail allocation */
  share->max_table_size= share->data_length + share->index_length;

  ok(share->block.last_allocated == last_alloc_after_fill,
     "last_allocated at block boundary (%lu)", last_alloc_after_fill);

  /*
    Insert row with 50-byte blob.
      total_records_needed = 1 + ceil((50 - 5) / 16) = 4
      min_run_records = min(4, max(ceil(128/16), 2)) = 4
    Step 1: free list has non-contiguous singles -> 1 < 4 -> gives up
    Step 2: block boundary + tight max_table_size -> fails
    Step 3: scavenges individual free-list slots -> succeeds
  */
  build_record(rec, 99999, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0,
     "insert with 50-byte blob via Step 3 scavenge fallback");

  ok(share->block.last_allocated == last_alloc_after_fill,
     "last_allocated unchanged: all records from free list (%lu)",
     (ulong) share->block.last_allocated);

  /* Verify data integrity */
  {
    uchar key[4];
    uchar read_buf[REC_LENGTH];
    uint32 read_len;
    const uchar *read_ptr;

    int4store(key, 99999);
    ok(heap_rkey(info, read_buf, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row via key lookup after scavenge insert");
    read_len= uint2korr(read_buf + BLOB_OFFSET);
    memcpy(&read_ptr, read_buf + BLOB_OFFSET + BLOB_PACKLEN, sizeof(read_ptr));
    ok(read_len == 50, "blob length == 50 (got %u)", read_len);
    ok(memcmp(read_ptr, blob_data, 50) == 0, "blob data matches");
  }

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after Step 3 scavenge insert");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: true capacity exhaustion fails correctly.

  Same setup as test_freelist_scavenge_fallback, but insert a blob
  large enough to exhaust BOTH the tail AND the free list.  The
  insert must fail with HA_ERR_RECORD_FILE_FULL.
*/

static void test_true_capacity_exhaustion(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  int32 i;
  int32 inserted, deleted_count;
  int32 records_in_block;

  /*
    Blob large enough to need more records than available free slots.
    With records_in_block=1024, deleting 512 rows gives 512 free slots.
    A blob needing 600+ continuation records exceeds that.
    600 records x 16 bytes/rec ~ 9600 bytes, minus headers.
    Use ~5000 bytes to be safe (needs ~315 records).
    We only delete ~10 rows to create very few free slots.
  */
  uchar blob_data[5000];
  memset(blob_data, 'X', sizeof(blob_data));

  if (create_and_open("test_exhaust", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(4, "setup failed");
    return;
  }

  records_in_block= (int32) share->block.records_in_block;

  /* Fill the block */
  for (i= 0, inserted= 0; i < records_in_block; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    if (heap_write(info, rec) != 0)
      break;
    inserted++;
  }
  ok(inserted == records_in_block, "filled block: %d rows", inserted);

  /* Delete only 10 rows -- not enough free slots for the large blob */
  deleted_count= 0;
  for (i= 0; i < 20 && i < inserted; i+= 2)
  {
    uchar key[4];
    int4store(key, i);
    if (heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0 &&
        heap_delete(info, rec) == 0)
      deleted_count++;
  }
  ok(deleted_count == 10, "deleted 10 rows");

  /* Lock out tail */
  share->max_table_size= share->data_length + share->index_length;

  /* Try to insert a 5000-byte blob -- should fail */
  build_record(rec, 99998, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) != 0,
     "insert with 5000-byte blob correctly fails (not enough free slots)");
  ok(my_errno == HA_ERR_RECORD_FILE_FULL,
     "error is HA_ERR_RECORD_FILE_FULL (got %d)", my_errno);

  /* Verify table is still consistent (no corruption from partial rollback) */
  ok(share->records == (ulong)(inserted - deleted_count),
     "records count consistent after failed insert (%lu)",
     (ulong) share->records);

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: tail reclaim on failed blob allocation (single block).

  Fills a block to within 5 slots of capacity, locks out new block
  allocation, then inserts a blob needing more records than the
  remaining tail.  The blob partially allocates from the tail, then
  fails.  hp_shrink_tail() must reclaim the tail-allocated records.
*/

static void test_tail_reclaim_single_block(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  int32 i;
  int32 rib;
  ulong last_alloc_before;

  uchar blob_data[200];
  memset(blob_data, 'T', sizeof(blob_data));

  if (create_and_open("test_tail_reclaim_sb", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  rib= (int32) share->block.records_in_block;

  /* Fill block to rib - 5 */
  for (i= 0; i < rib - 5; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    if (heap_write(info, rec) != 0)
      break;
  }
  ok(i == rib - 5, "filled block to rib-5 (%d rows)", i);

  last_alloc_before= (ulong) share->block.last_allocated;

  /* Lock out new block allocation */
  share->max_table_size= share->data_length + share->index_length;

  /* Insert blob needing ~14 records -- only 4 tail slots available for blob */
  build_record(rec, 99999, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) != 0,
     "insert with 200-byte blob fails (not enough tail)");
  ok(my_errno == HA_ERR_RECORD_FILE_FULL,
     "error is HA_ERR_RECORD_FILE_FULL (got %d)", my_errno);

  /*
    After failed insert: 4 blob continuation records were tail-allocated
    then reclaimed by hp_shrink_tail().  The primary record was allocated
    (+1 to last_allocated) and freed to the delete list.
  */
  ok(share->block.last_allocated == last_alloc_before + 1,
     "last_allocated: 4 blob recs reclaimed, primary on free list "
     "(expected %lu, got %lu)",
     last_alloc_before + 1, (ulong) share->block.last_allocated);
  ok(share->deleted == 1,
     "deleted == 1 (primary on free list, got %lu)",
     (ulong) share->deleted);
  ok(share->total_records == (ulong)(rib - 5),
     "total_records back to pre-insert (%lu)",
     (ulong) share->total_records);
  ok(share->total_records + share->deleted == share->block.last_allocated,
     "invariant: total_records + deleted == last_allocated");

  /* Verify existing data readable */
  {
    uchar key[4];
    int4store(key, 0);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "existing row 0 still readable");
  }

  /* Insert a small blob -- should succeed using reclaimed tail */
  {
    uchar small_blob[30];
    memset(small_blob, 's', sizeof(small_blob));
    build_record(rec, 88888, small_blob, sizeof(small_blob));
    ok(heap_write(info, rec) == 0,
       "small blob insert succeeds using reclaimed tail");
  }

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: tail reclaim across block boundaries.

  Fills a block to within 5 slots of capacity, allows exactly 2 blocks,
  then inserts a blob large enough to span both blocks and require a third.
  The allocation fails at the third block.  hp_shrink_tail() must reclaim
  all records across both blocks and update last_blocks to the first block.
*/

static void test_tail_reclaim_cross_block(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  int32 i;
  int32 rib;
  ulong last_alloc_before;
  HP_PTRS *first_block;
  uchar *blob_data;
  uint32 blob_len;

  if (create_and_open("test_tail_reclaim_xb", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  rib= (int32) share->block.records_in_block;

  /* Fill block to rib - 5 */
  for (i= 0; i < rib - 5; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    if (heap_write(info, rec) != 0)
      break;
  }
  ok(i == rib - 5, "filled block to rib-5 (%d rows)", i);

  last_alloc_before= (ulong) share->block.last_allocated;
  first_block= share->block.level_info[0].last_blocks;

  /* Allow exactly 2 blocks, fail at 3rd */
  share->max_records= (ulong)(2 * rib - 1);

  /*
    Blob large enough to need more than 4 + rib continuation records.
    Each Case C record holds 16 bytes of payload (inner records) plus
    5 bytes in the first record of each run.  Use (rib + 20) * 16 bytes
    to ensure it exceeds 2 blocks.
  */
  blob_len= (uint32)((rib + 20) * 16);
  blob_data= (uchar*) my_malloc(PSI_NOT_INSTRUMENTED, blob_len, MYF(0));
  memset(blob_data, 'X', blob_len);

  build_record(rec, 99999, blob_data, (uint16) MY_MIN(blob_len, 65535));
  ok(heap_write(info, rec) != 0,
     "insert with %u-byte blob fails (spans 2 blocks, needs 3rd)",
     blob_len);
  ok(my_errno == HA_ERR_RECORD_FILE_FULL,
     "error is HA_ERR_RECORD_FILE_FULL (got %d)", my_errno);

  /*
    All blob continuation records (4 from first block + rib from second)
    were reclaimed by hp_shrink_tail().  Only the primary record remains
    on the free list.
  */
  ok(share->block.last_allocated == last_alloc_before + 1,
     "last_allocated: all blob recs reclaimed across blocks "
     "(expected %lu, got %lu)",
     last_alloc_before + 1, (ulong) share->block.last_allocated);
  ok(share->deleted == 1,
     "deleted == 1 (primary on free list, got %lu)",
     (ulong) share->deleted);
  ok(share->total_records + share->deleted == share->block.last_allocated,
     "invariant: total_records + deleted == last_allocated");

  /* last_blocks must have been restored to the first block */
  ok(share->block.level_info[0].last_blocks == first_block,
     "last_blocks restored to first block after cross-block reclaim");

  /* Verify existing data readable */
  {
    uchar key[4];
    int4store(key, 0);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "existing row 0 still readable");
  }

  /* Insert a small blob -- should succeed using reclaimed tail in first block */
  {
    uchar small_blob[30];
    memset(small_blob, 's', sizeof(small_blob));
    build_record(rec, 88888, small_blob, sizeof(small_blob));
    ok(heap_write(info, rec) == 0,
       "small blob insert succeeds using reclaimed tail in first block");
  }

  my_free(blob_data);
  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: tail reclaim across 3 block boundaries.

  Like test 6 but with a blob large enough to span 3 leaf blocks.
  Verifies hp_find_block() correctly navigates the block tree when
  hp_shrink_tail() crosses two block boundaries sequentially.
*/

static void test_tail_reclaim_three_blocks(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  int32 i;
  int32 rib;
  ulong last_alloc_before;
  HP_PTRS *first_block;
  uchar *blob_data;
  uint32 blob_len;

  if (create_and_open("test_tail_reclaim_3b", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  rib= (int32) share->block.records_in_block;

  /* Fill block to rib - 5 */
  for (i= 0; i < rib - 5; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    if (heap_write(info, rec) != 0)
      break;
  }
  ok(i == rib - 5, "filled block to rib-5 (%d rows)", i);

  last_alloc_before= (ulong) share->block.last_allocated;
  first_block= share->block.level_info[0].last_blocks;

  /* Allow exactly 3 blocks, fail at 4th */
  share->max_records= (ulong)(3 * rib - 1);

  /*
    Blob needs more than 4 + 2*rib continuation records to span
    3 blocks and fail requesting the 4th.
  */
  blob_len= (uint32)((2 * rib + 20) * 16);
  blob_data= (uchar*) my_malloc(PSI_NOT_INSTRUMENTED, blob_len, MYF(0));
  memset(blob_data, 'Z', blob_len);

  build_record(rec, 99999, blob_data, (uint16) MY_MIN(blob_len, 65535));
  ok(heap_write(info, rec) != 0,
     "insert with %u-byte blob fails (spans 3 blocks, needs 4th)",
     blob_len);
  ok(my_errno == HA_ERR_RECORD_FILE_FULL,
     "error is HA_ERR_RECORD_FILE_FULL (got %d)", my_errno);

  /*
    All blob records across 3 blocks reclaimed: 4 from block 1 +
    rib from block 2 + rib from block 3.  Two block boundary crossings.
  */
  ok(share->block.last_allocated == last_alloc_before + 1,
     "last_allocated: all recs reclaimed across 3 blocks "
     "(expected %lu, got %lu)",
     last_alloc_before + 1, (ulong) share->block.last_allocated);
  ok(share->deleted == 1,
     "deleted == 1 (got %lu)", (ulong) share->deleted);
  ok(share->total_records + share->deleted == share->block.last_allocated,
     "invariant: total_records + deleted == last_allocated");
  ok(share->block.level_info[0].last_blocks == first_block,
     "last_blocks restored to first block after 2 boundary crossings");

  /* Verify existing data and insert small blob */
  {
    uchar key[4];
    int4store(key, 0);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "existing row 0 still readable");
  }
  {
    uchar small_blob[30];
    memset(small_blob, 's', sizeof(small_blob));
    build_record(rec, 88888, small_blob, sizeof(small_blob));
    ok(heap_write(info, rec) == 0,
       "small blob insert succeeds after 3-block reclaim");
  }

  my_free(blob_data);
  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: orphaned blocks are reused via high_water_allocated.

  After hp_shrink_tail() empties 2 leaf blocks, fills them back up
  with non-blob rows.  data_length must NOT grow (blocks are reused
  via hp_find_block, not freshly allocated via hp_get_new_block).
*/

static void test_block_reuse_after_reclaim(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  int32 i;
  int32 rib;
  ulong last_alloc_before;
  ulonglong data_len_after_shrink;
  uchar *blob_data;
  uint32 blob_len;

  if (create_and_open("test_block_reuse", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(13, "setup failed");
    return;
  }

  rib= (int32) share->block.records_in_block;

  /* Fill block to rib - 5 */
  for (i= 0; i < rib - 5; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    if (heap_write(info, rec) != 0)
      break;
  }
  ok(i == rib - 5, "filled block to rib-5 (%d rows)", i);

  last_alloc_before= (ulong) share->block.last_allocated;

  /* Allow exactly 3 blocks, fail at 4th */
  share->max_records= (ulong)(3 * rib - 1);

  /* Blob that spans 3 blocks then fails */
  blob_len= (uint32)((2 * rib + 20) * 16);
  blob_data= (uchar*) my_malloc(PSI_NOT_INSTRUMENTED, blob_len, MYF(0));
  memset(blob_data, 'R', blob_len);

  build_record(rec, 99999, blob_data, (uint16) MY_MIN(blob_len, 65535));
  ok(heap_write(info, rec) != 0,
     "blob insert fails as expected");

  ok(share->block.last_allocated == last_alloc_before + 1,
     "tail reclaimed after failure");
  ok(share->block.high_water_allocated == (ulong)(3 * rib),
     "high_water_allocated set to 3*rib (got %lu, expected %lu)",
     (ulong) share->block.high_water_allocated, (ulong)(3 * rib));

  data_len_after_shrink= share->data_length;

  /* Remove max_records limit so we can fill freely */
  share->max_records= 0;

  /*
    Insert 2*rib non-blob rows.  The first reuses the free-list slot
    (primary from failed insert).  The remaining 2*rib-1 extend the
    tail, crossing block boundaries at rib and 2*rib.  At each boundary,
    hp_alloc_from_tail() must REUSE the orphaned block (not allocate new).
    Key check: data_length must NOT grow.
  */
  {
    int32 inserted= 0;
    int32 to_insert= 2 * rib;
    for (i= 0; i < to_insert; i++)
    {
      build_record(rec, rib + i, (const uchar*) "", 0);
      if (heap_write(info, rec) != 0)
        break;
      inserted++;
    }
    ok(inserted == to_insert,
       "inserted %d more rows across 2 reused blocks", inserted);
  }

  ok(share->data_length == data_len_after_shrink,
     "data_length unchanged: blocks reused, not newly allocated "
     "(before=%llu, after=%llu)",
     data_len_after_shrink, share->data_length);

  /*
    last_allocated = (rib - 4) + (2*rib - 1) = 3*rib - 5
    The -4 is the post-shrink starting point (rib-5 data + 1 primary
    on free list); the -1 accounts for the first insert reusing the
    free-list slot instead of extending the tail.
  */
  ok(share->block.last_allocated == (ulong)(3 * rib - 5),
     "last_allocated grew through reused blocks (got %lu, expected %lu)",
     (ulong) share->block.last_allocated, (ulong)(3 * rib - 5));

  ok(share->total_records + share->deleted == share->block.last_allocated,
     "invariant: total_records + deleted == last_allocated");

  /* Verify data integrity: read first and last inserted rows */
  {
    uchar key[4];
    int4store(key, 0);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "first row still readable");
    int4store(key, 3 * rib - 6);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "last inserted row readable");
  }

  /*
    Now delete a row and insert a blob that crosses a block boundary.
    hp_write_one_blob() -> hp_alloc_from_tail() uses the same reuse path.
    high_water_allocated is still 3*rib; current last_allocated is 3*rib-5.
    A blob needing 6+ continuation records will cross into the tail of
    block 3 (5 slots left).  No new block should be allocated.
  */
  {
    uchar key[4];
    ulonglong data_len_before;
    uchar reuse_blob[50];
    memset(reuse_blob, 'B', sizeof(reuse_blob));

    int4store(key, rib);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row for delete before blob reuse test");
    ok(heap_delete(info, rec) == 0, "deleted row");

    data_len_before= share->data_length;
    build_record(rec, 77777, reuse_blob, sizeof(reuse_blob));
    ok(heap_write(info, rec) == 0,
       "blob insert succeeds using reused blocks");
    ok(share->data_length == data_len_before,
       "data_length unchanged after blob insert (blocks reused)");
  }

  my_free(blob_data);
  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: block formation after blob chain free.

  Insert a blob row (100 bytes, 8 continuation records) and a guard
  row.  Delete the blob row.  The continuation chain becomes a block
  on the free list.  Verify block-start/end flags, count, dark record
  zeroing, and heap_check_heap.
*/

static void test_block_formation(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data[100];
  uchar *pos0, *pos8;
  uint visible;
  uint16 j;

  memset(blob_data, 'B', sizeof(blob_data));

  if (create_and_open("test_block_form", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(9, "setup failed");
    return;
  }

  visible= share->visible;

  /* Insert blob row (primary at pos 0, chain at pos 1-8) */
  build_record(rec, 1, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert blob row");

  /* Guard row at pos 9 prevents hp_shrink_tail from reclaiming */
  build_record(rec, 2, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "insert guard row");

  /* Delete the blob row (deferred blob free for non-internal table) */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob row for deletion");
    ok(heap_delete(info, rec) == 0, "deleted blob row");
  }

  /* Flush deferred blob free to materialize the block */
  hp_flush_pending_blob_free(info);

  /*
    Free list: block(pos0-pos8, count=9) -> NULL
    The primary (pos0) coalesced with the chain block (pos1-8)
    via hp_push_free_block_coalesce in hp_free_run_chain.
  */
  pos0= hp_find_block(&share->block, 0);
  pos8= hp_find_block(&share->block, 8);

  ok(hp_is_free_block_start(pos0),
     "pos 0 has HP_DEL_BLOCK_START flag (primary coalesced with chain)");
  ok(hp_free_block_start_count(pos0) == 9,
     "block count == 9 (got %u)", hp_free_block_start_count(pos0));
  ok(hp_is_free_block_end(pos8),
     "pos 8 has HP_DEL_BLOCK_END flag");
  ok(hp_free_block_first(pos8) == pos0,
     "block-end del_link points to block-start (pos0)");

  /* Verify dark records (pos 1-7) have del_flag == 0 and visible == 0 */
  {
    my_bool dark_ok= TRUE;
    for (j= 1; j <= 7; j++)
    {
      uchar *dark= hp_find_block(&share->block, j);
      if (dark[HP_DEL_FLAG_OFFSET] != 0 || dark[visible] != 0)
        dark_ok= FALSE;
    }
    ok(dark_ok, "dark records (pos 1-7) have zeroed metadata");
  }

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates block structure");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: hp_pop_free_record from a block shrinks it.

  After creating a block(8) on the free list, pop records one at a time.
  Verify the block shrinks correctly: block-end moves, count decreases,
  dark records are promoted.
*/

static void test_pop_from_block(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data[100];
  uchar *pos0, *pos7;

  memset(blob_data, 'P', sizeof(blob_data));

  if (create_and_open("test_pop_block", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  /* Insert blob row + guard */
  build_record(rec, 1, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert blob row");
  build_record(rec, 2, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "insert guard row");

  /* Delete blob row */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob row");
    ok(heap_delete(info, rec) == 0, "deleted blob row");
  }
  hp_flush_pending_blob_free(info);
  /* Free list: block(pos0-pos8, count=9) -> NULL (primary coalesced) */

  ok(hp_is_free_block_end(share->del_link),
     "free list head is block-end after flush");

  /* Pop from block head by inserting a non-blob row */
  build_record(rec, 3, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0,
     "insert non-blob row (pops block-end pos8)");

  /* Block should have shrunk: 9 -> 8, new end at pos7 */
  pos0= hp_find_block(&share->block, 0);
  pos7= hp_find_block(&share->block, 7);
  ok(share->del_link == pos7 &&
     hp_is_free_block_end(pos7) &&
     hp_free_block_start_count(pos0) == 8,
     "block shrank to 8 records (end at pos7, count=%u)",
     hp_free_block_start_count(pos0));

  /* Pop again: 8 -> 7 */
  build_record(rec, 4, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0,
     "insert non-blob row (pops from block again)");

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after pops");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: block collapse to single record on pop.

  Create a 3-record block (primary + 2 continuation records from a
  16-byte blob, coalesced via hp_push_free_block_coalesce).  Pop
  records until only one remains and it collapses to a single.
*/

static void test_block_collapse_to_single(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data[16];
  uchar *pos0;

  memset(blob_data, 'C', sizeof(blob_data));

  if (create_and_open("test_block_collapse", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  /* Insert 16-byte blob (2 continuation records: pos 1-2) */
  build_record(rec, 1, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert 16-byte blob row");

  /* Guard row at pos 3 */
  build_record(rec, 2, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "insert guard row");

  /* Delete blob row */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob row");
    ok(heap_delete(info, rec) == 0, "deleted blob row");
  }
  hp_flush_pending_blob_free(info);
  /*
    Free list: block(pos0-pos2, count=3) -> NULL
    Primary coalesced with chain via hp_push_free_block_coalesce.
  */

  /* Pop from 3-record block: takes pos2, block shrinks to 2 */
  build_record(rec, 3, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "pop from 3-record block");

  /* Pop again: takes pos1, collapses pos0 to single */
  build_record(rec, 4, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "pop from 2-record block");

  pos0= hp_find_block(&share->block, 0);
  ok(share->del_link == pos0 && pos0[HP_DEL_FLAG_OFFSET] == 0,
     "remaining record collapsed to single (del_flag=0)");

  /* Pop pos0 (single) */
  build_record(rec, 5, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "pop collapsed single");
  ok(share->del_link == NULL, "free list empty after all pops");

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after collapse");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: partial block take via blob reinsert.

  Create a block(8) on the free list, then insert a new blob that
  needs fewer records.  hp_take_free_block takes a partial block.
  Verify the remaining block is correct.
*/

static void test_partial_block_take(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data_big[100];
  uchar blob_data_small[50];
  uchar *pos0;
  ulong alloc_before;

  memset(blob_data_big, 'G', sizeof(blob_data_big));
  memset(blob_data_small, 'S', sizeof(blob_data_small));

  if (create_and_open("test_partial_take", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(7, "setup failed");
    return;
  }

  /* Insert 100-byte blob (pos 0=primary, pos 1-8=chain) + guard */
  build_record(rec, 1, blob_data_big, sizeof(blob_data_big));
  ok(heap_write(info, rec) == 0, "insert 100-byte blob");
  build_record(rec, 2, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "insert guard row");

  alloc_before= (ulong) share->block.last_allocated;

  /* Delete blob row */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob row");
    ok(heap_delete(info, rec) == 0, "deleted blob row");
  }
  hp_flush_pending_blob_free(info);
  /* Free list: block(pos0-pos8, count=9) -> NULL (primary coalesced) */

  /* Insert 50-byte blob: pops from coalesced block for chain + primary */
  build_record(rec, 3, blob_data_small, sizeof(blob_data_small));
  ok(heap_write(info, rec) == 0, "insert 50-byte blob (partial block take)");

  /* No tail growth: all from free list */
  ok(share->block.last_allocated == alloc_before,
     "last_allocated unchanged (all from free list)");

  /* Remaining block should be smaller */
  pos0= hp_find_block(&share->block, 0);
  ok(hp_is_free_block_start(pos0) || share->del_link == pos0,
     "remaining records still on free list (block or single)");

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after partial take");

  /* Verify blob data integrity */
  {
    uchar key[4];
    uchar read_buf[REC_LENGTH];
    uint32 read_len;
    const uchar *read_ptr;

    int4store(key, 3);
    ok(heap_rkey(info, read_buf, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found reinserted row");
    read_len= uint2korr(read_buf + BLOB_OFFSET);
    memcpy(&read_ptr, read_buf + BLOB_OFFSET + BLOB_PACKLEN, sizeof(read_ptr));
    ok(read_len == 50 && memcmp(read_ptr, blob_data_small, 50) == 0,
       "blob data matches after partial block take");
  }

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: scan batch-skip for deleted blocks.

  Insert active rows, then a blob row, then more active rows.  Delete
  the blob row to create a block in the middle.  Scan should return
  only the active rows, batch-skipping the block.
*/

static void test_scan_batch_skip_block(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data[100];
  int scan_count;
  int err;
  int32 i;

  memset(blob_data, 'K', sizeof(blob_data));

  if (create_and_open("test_scan_skip", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }

  /* Insert 3 active rows (pos 0, 1, 2) */
  for (i= 0; i < 3; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    ok(heap_write(info, rec) == 0, "insert row %d", (int) i);
  }

  /* Insert blob row (primary at pos 3, chain at pos 4-11) */
  build_record(rec, 100, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert blob row");

  /* Insert 2 more active rows (pos 12, 13) */
  for (i= 10; i < 12; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    ok(heap_write(info, rec) == 0, "insert row %d", (int) i);
  }

  ok(share->records == 6, "6 primary rows before delete");

  /* Delete the blob row: chain at pos 4-11 becomes block(8) */
  {
    uchar key[4];
    int4store(key, 100);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob row");
    ok(heap_delete(info, rec) == 0, "deleted blob row");
  }

  ok(share->records == 5, "5 primary rows after delete");

  /* Flush deferred blob free so chain becomes a block */
  hp_flush_pending_blob_free(info);

  /* Scan: should return exactly 5 rows */
  heap_scan_init(info);
  scan_count= 0;
  while ((err= heap_scan(info, rec)) != HA_ERR_END_OF_FILE)
  {
    if (err == 0)
      scan_count++;
  }
  ok(scan_count == 5, "scan returned %d rows (expected 5)", scan_count);

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates with block in middle");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: hp_shrink_tail reclaims entire block at once.

  Insert a blob row at the tail (no guard row).  Delete it.
  hp_shrink_tail should reclaim the entire block in one step,
  not record-by-record.
*/

static void test_shrink_tail_with_block(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data[100];
  ulong alloc_after_insert, alloc_after_delete;

  memset(blob_data, 'H', sizeof(blob_data));

  if (create_and_open("test_shrink_block", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }

  /* Insert blob row: primary at pos 0, chain at pos 1-8 */
  build_record(rec, 1, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert blob row");
  alloc_after_insert= (ulong) share->block.last_allocated;

  /* Delete blob row, then flush to trigger block free + shrink_tail */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob row");
    ok(heap_delete(info, rec) == 0, "deleted blob row");
  }
  hp_flush_pending_blob_free(info);

  alloc_after_delete= (ulong) share->block.last_allocated;
  /*
    hp_flush_pending_blob_free -> hp_free_run_chain (pushes block)
    -> hp_shrink_tail reclaims block + primary, leaving last_allocated=0.
  */
  ok(alloc_after_delete == 0,
     "hp_shrink_tail reclaimed all records (before=%lu, after=%lu)",
     alloc_after_insert, alloc_after_delete);
  ok(share->deleted == 0, "deleted == 0 (all reclaimed)");
  ok(share->del_link == NULL, "free list empty after full reclaim");

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after full reclaim");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: interleaved singles and blocks on free list.

  Create a free list with mixed entries: a single record, then a
  block, then another single.  Verify traversal and heap_check_heap.
*/

static void test_interleaved_singles_and_blocks(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data[100];
  ulong del_count;

  memset(blob_data, 'I', sizeof(blob_data));

  if (create_and_open("test_interleaved", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(6, "setup failed");
    return;
  }

  /*
    Layout after inserts:
      pos 0: non-blob row (key=10)
      pos 1-8: blob chain (key=20, 100-byte blob)
      pos 9: blob primary (key=20)
        Actually, wait -- primary comes first. Let me re-think.

    Actually, heap_write puts the primary at the first free position,
    then blob chains extend from there. So:
      pos 0: primary of row key=10 (non-blob)
      pos 1: primary of row key=20 (blob row)
      pos 2-9: blob chain of row key=20 (8 continuation records)
      pos 10: primary of row key=30 (non-blob)
  */
  build_record(rec, 10, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "insert row 10 (non-blob)");
  build_record(rec, 20, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert row 20 (blob)");
  build_record(rec, 30, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "insert row 30 (non-blob, guard)");

  /* Delete row 10 (non-blob at pos 0) -> single on free list */
  {
    uchar key[4];
    int4store(key, 10);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 10");
    ok(heap_delete(info, rec) == 0, "deleted row 10 (single)");
  }

  /* Delete row 20 (blob at pos 1) -> deferred blob chain free */
  {
    uchar key[4];
    int4store(key, 20);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 20");
    ok(heap_delete(info, rec) == 0, "deleted row 20 (blob -> block)");
  }
  hp_flush_pending_blob_free(info);

  /*
    Free list:
      pos9 (block-end, 8 records at pos2-pos9) ->
      pos1 (single, blob primary) -> pos0 (single) -> NULL
    Total deleted = 10 (8 block + 2 singles)
  */
  del_count= share->deleted;
  ok(del_count == 10,
     "total deleted records == 10 (got %lu)", del_count);

  /* Verify free list traversal counts correctly */
  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates interleaved singles and blocks");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: delete coalescing forms block from sequential ascending deletes.

  Delete rows 0, 1, 2 in ascending order.  First delete pushes a single.
  Second delete finds head adjacent below pos, forms a 2-block.
  Third delete finds block-end adjacent below pos, extends to 3.
*/

static void test_delete_coalesce_ascending(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar *pos0, *pos1, *pos2;
  int32 i;

  if (create_and_open("test_coalesce_asc", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(13, "setup failed");
    return;
  }

  /* Insert 5 non-blob rows: pos 0-4 */
  for (i= 0; i < 5; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    ok(heap_write(info, rec) == 0, "insert row %d", (int) i);
  }

  pos0= hp_find_block(&share->block, 0);
  pos1= hp_find_block(&share->block, 1);
  pos2= hp_find_block(&share->block, 2);

  /* Delete row 0: push single */
  {
    uchar key[4];
    int4store(key, 0);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 0");
    ok(heap_delete(info, rec) == 0, "deleted row 0");
  }
  ok(share->del_link == pos0 && pos0[HP_DEL_FLAG_OFFSET] == 0,
     "row 0 is single on free list");

  /* Delete row 1: adjacent above head, form 2-block */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 1");
    ok(heap_delete(info, rec) == 0, "deleted row 1");
  }
  ok(share->del_link == pos1 && hp_is_free_block_end(pos1),
     "coalesced into 2-block (end at pos1)");
  ok(hp_is_free_block_start(pos0) && hp_free_block_start_count(pos0) == 2,
     "block-start at pos0, count=2");

  /* Delete row 2: adjacent above block-end, extend to 3 */
  {
    uchar key[4];
    int4store(key, 2);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 2");
    ok(heap_delete(info, rec) == 0, "deleted row 2");
  }
  ok(share->del_link == pos2 && hp_is_free_block_end(pos2),
     "extended to 3-block (end at pos2)");
  ok(hp_free_block_start_count(pos0) == 3,
     "block count == 3");
  ok(pos1[HP_DEL_FLAG_OFFSET] == 0,
     "pos1 is dark (del_flag=0)");

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates ascending coalesce");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: delete coalescing forms block from sequential descending deletes.

  Delete rows 4, 3, 2 in descending order.  First delete pushes single.
  Second finds head adjacent above pos, forms 2-block.
  Third extends downward to 3.
*/

static void test_delete_coalesce_descending(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar *pos2, *pos3, *pos4;
  int32 i;

  if (create_and_open("test_coalesce_desc", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(13, "setup failed");
    return;
  }

  for (i= 0; i < 5; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    ok(heap_write(info, rec) == 0, "insert row %d", (int) i);
  }

  pos2= hp_find_block(&share->block, 2);
  pos3= hp_find_block(&share->block, 3);
  pos4= hp_find_block(&share->block, 4);

  /* Delete row 4: push single */
  {
    uchar key[4];
    int4store(key, 4);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 4");
    ok(heap_delete(info, rec) == 0, "deleted row 4");
  }
  ok(share->del_link == pos4 && pos4[HP_DEL_FLAG_OFFSET] == 0,
     "row 4 is single on free list");

  /* Delete row 3: adjacent below head, form 2-block */
  {
    uchar key[4];
    int4store(key, 3);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 3");
    ok(heap_delete(info, rec) == 0, "deleted row 3");
  }
  ok(share->del_link == pos4 && hp_is_free_block_end(pos4),
     "coalesced into 2-block (end at pos4)");
  ok(hp_is_free_block_start(pos3) && hp_free_block_start_count(pos3) == 2,
     "block-start at pos3, count=2");

  /* Delete row 2: adjacent below block-start, extend downward to 3 */
  {
    uchar key[4];
    int4store(key, 2);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 2");
    ok(heap_delete(info, rec) == 0, "deleted row 2");
  }
  ok(share->del_link == pos4 && hp_is_free_block_end(pos4),
     "extended to 3-block (end still at pos4)");
  ok(hp_free_block_start_count(pos2) == 3,
     "block-start moved to pos2, count=3");
  ok(pos3[HP_DEL_FLAG_OFFSET] == 0,
     "pos3 is dark (del_flag=0)");

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates descending coalesce");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: non-adjacent deletes do not coalesce.

  Delete rows 0 and 2 (gap at row 1).  Both should remain singles.
*/

static void test_delete_no_coalesce_gap(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar *pos0, *pos2;
  int32 i;

  if (create_and_open("test_no_coalesce", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(7, "setup failed");
    return;
  }

  for (i= 0; i < 5; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    ok(heap_write(info, rec) == 0, "insert row %d", (int) i);
  }

  pos0= hp_find_block(&share->block, 0);
  pos2= hp_find_block(&share->block, 2);

  {
    uchar key[4];
    int4store(key, 0);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 0");
    ok(heap_delete(info, rec) == 0, "deleted row 0");
  }

  {
    uchar key[4];
    int4store(key, 2);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 2");
    ok(heap_delete(info, rec) == 0, "deleted row 2");
  }

  ok(share->del_link == pos2 && pos2[HP_DEL_FLAG_OFFSET] == 0,
     "pos2 is single (no coalesce)");
  ok(*((uchar**) pos2) == pos0 && pos0[HP_DEL_FLAG_OFFSET] == 0,
     "pos0 is single (no coalesce), linked from pos2");

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates non-adjacent singles");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: coalesced block reused by blob insert.

  Delete 5 adjacent non-blob rows, coalescing into a 5-block.
  Insert a blob row that fits in 4 continuation records.
  The blob allocator should take from the coalesced block.
*/

static void test_coalesced_block_reuse(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data[50];
  ulong alloc_before;
  int32 i;

  memset(blob_data, 'R', sizeof(blob_data));

  if (create_and_open("test_coalesce_reuse", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(10, "setup failed");
    return;
  }

  /* Insert 7 rows: 0-6 */
  for (i= 0; i < 7; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    ok(heap_write(info, rec) == 0, "insert row %d", (int) i);
  }

  alloc_before= (ulong) share->block.last_allocated;

  /* Delete rows 1-5 (ascending) -> coalesced 5-block */
  for (i= 1; i <= 5; i++)
  {
    uchar key[4];
    int4store(key, i);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row %d", (int) i);
    ok(heap_delete(info, rec) == 0, "deleted row %d", (int) i);
  }

  ok(share->deleted == 5, "5 deleted records (coalesced block)");
  ok(hp_is_free_block_end(share->del_link),
     "free list head is block-end");
  ok(hp_free_block_start_count(
       hp_free_block_first(share->del_link)) == 5,
     "coalesced block count == 5");

  /* Insert blob row: should take from the coalesced block */
  build_record(rec, 100, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert blob row (reuse coalesced block)");

  ok(share->block.last_allocated == alloc_before,
     "last_allocated unchanged: blob reused coalesced block");

  /* Verify blob data integrity */
  {
    uchar key[4];
    uchar read_buf[REC_LENGTH];
    uint32 read_len;
    const uchar *read_ptr;

    int4store(key, 100);
    ok(heap_rkey(info, read_buf, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob row");
    read_len= uint2korr(read_buf + BLOB_OFFSET);
    memcpy(&read_ptr, read_buf + BLOB_OFFSET + BLOB_PACKLEN, sizeof(read_ptr));
    ok(read_len == 50 && memcmp(read_ptr, blob_data, 50) == 0,
       "blob data matches after coalesced block reuse");
  }

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after coalesced block reuse");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: scan correctly handles coalesced blocks from delete.

  Insert 8 rows.  Delete the middle 4 (rows 2-5) via ascending deletes
  to form a coalesced block.  Scan should return only the 4 active rows,
  batch-skipping the coalesced block.
*/

static void test_scan_coalesced_block(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  int32 i;
  int scan_count, err;

  if (create_and_open("test_scan_coalesce", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  for (i= 0; i < 8; i++)
  {
    build_record(rec, i, (const uchar*) "", 0);
    ok(heap_write(info, rec) == 0, "insert row %d", (int) i);
  }

  /* Delete rows 2-5 ascending -> coalesced 4-block */
  for (i= 2; i <= 5; i++)
  {
    uchar key[4];
    int4store(key, i);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row %d", (int) i);
    ok(heap_delete(info, rec) == 0, "deleted row %d", (int) i);
  }

  ok(share->records == 4, "4 active rows after deletes");

  heap_scan_init(info);
  scan_count= 0;
  while ((err= heap_scan(info, rec)) != HA_ERR_END_OF_FILE)
  {
    if (err == 0)
      scan_count++;
  }
  ok(scan_count == 4, "scan returned %d rows (expected 4)", scan_count);

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates scan with coalesced block");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: block-to-block coalescing via adjacent blob chain free.

  Insert two adjacent blob rows A (pos0-8) and B (pos9-17).
  Delete both.  When B's chain (pos10-17) is freed, the primary (pos9)
  has already coalesced with A's chain block via record coalescing.
  The block push of B's chain then merges with the existing block
  via hp_push_free_block_coalesce (block-to-block merge).

  Result: a single block spanning pos0-17 (count=18).
*/

static void test_block_to_block_coalesce(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar blob_data[100];
  uchar *pos0, *pos17;

  memset(blob_data, 'M', sizeof(blob_data));

  if (create_and_open("test_blk_blk_coal", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(12, "setup failed");
    return;
  }

  /* Insert blob A (primary pos0, chain pos1-8) */
  build_record(rec, 1, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert blob A");

  /* Insert blob B (primary pos9, chain pos10-17) */
  build_record(rec, 2, blob_data, sizeof(blob_data));
  ok(heap_write(info, rec) == 0, "insert blob B");

  /* Guard row at pos18 */
  build_record(rec, 3, (const uchar*) "", 0);
  ok(heap_write(info, rec) == 0, "insert guard row");

  ok(share->block.last_allocated == 19,
     "19 records allocated (2*9 blob + 1 guard)");

  /* Delete blob A: primary pos0 pushed, chains deferred */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob A");
    ok(heap_delete(info, rec) == 0, "deleted blob A");
  }

  /*
    Delete blob B: first flushes A's chains, then pushes B's primary,
    then defers B's chains.

    Flush of A: hp_free_run_chain pushes pos1-8 as block.
    hp_push_free_block_coalesce: head is pos0 (single), adjacent below
    -> merge into block(pos0-8, count=9).

    Then B's primary pos9 pushed via hp_push_free_record_coalesce:
    head is block-end pos8, pos9 == pos8 + rb -> extend upward to
    block(pos0-9, count=10).

    B's chains deferred.
  */
  {
    uchar key[4];
    int4store(key, 2);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found blob B");
    ok(heap_delete(info, rec) == 0, "deleted blob B");
  }

  /*
    Flush B's chains: hp_free_run_chain pushes pos10-17 (8 records).
    hp_push_free_block_coalesce: head is block-end pos9, new block
    first=pos10, pos9 == pos10 - rb -> block-to-block merge!
    Result: block(pos0-17, count=18).
  */
  hp_flush_pending_blob_free(info);

  pos0= hp_find_block(&share->block, 0);
  pos17= hp_find_block(&share->block, 17);

  ok(share->del_link == pos17 && hp_is_free_block_end(pos17),
     "free list head is block-end at pos17");
  ok(hp_free_block_first(pos17) == pos0,
     "block-end points to pos0 (block-start)");
  ok(hp_free_block_start_count(pos0) == 18,
     "block count == 18 (two blob rows fully coalesced, got %u)",
     hp_free_block_start_count(pos0));

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates block-to-block coalesce");

  heap_drop_table(info);
  heap_close(info);
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_freelist");
  plan(252);

  diag("Test 1: free-list contiguity detects groups > 2 records");
  test_freelist_contiguity_multirecord();

  diag("Test 2: contiguity across repeated delete-reinsert cycles");
  test_freelist_contiguity_repeated_cycles();

  diag("Test 3: Step 3 free-list scavenge fallback");
  test_freelist_scavenge_fallback();

  diag("Test 4: true capacity exhaustion fails correctly");
  test_true_capacity_exhaustion();

  diag("Test 5: tail reclaim on failed blob (single block)");
  test_tail_reclaim_single_block();

  diag("Test 6: tail reclaim across block boundaries");
  test_tail_reclaim_cross_block();

  diag("Test 7: tail reclaim across 3 block boundaries");
  test_tail_reclaim_three_blocks();

  diag("Test 8: orphaned blocks reused after reclaim");
  test_block_reuse_after_reclaim();

  diag("Test 9: block formation after blob chain free");
  test_block_formation();

  diag("Test 10: pop from block shrinks it");
  test_pop_from_block();

  diag("Test 11: block collapse to single on pop");
  test_block_collapse_to_single();

  diag("Test 12: partial block take via blob reinsert");
  test_partial_block_take();

  diag("Test 13: scan batch-skip for deleted blocks");
  test_scan_batch_skip_block();

  diag("Test 14: hp_shrink_tail reclaims entire block at once");
  test_shrink_tail_with_block();

  diag("Test 15: interleaved singles and blocks on free list");
  test_interleaved_singles_and_blocks();

  diag("Test 16: delete coalescing - ascending adjacent deletes");
  test_delete_coalesce_ascending();

  diag("Test 17: delete coalescing - descending adjacent deletes");
  test_delete_coalesce_descending();

  diag("Test 18: delete coalescing - non-adjacent deletes stay separate");
  test_delete_no_coalesce_gap();

  diag("Test 19: coalesced block reused by blob insert");
  test_coalesced_block_reuse();

  diag("Test 20: scan batch-skip with coalesced delete block");
  test_scan_coalesced_block();

  diag("Test 21: block-to-block coalescing via adjacent blob chains");
  test_block_to_block_coalesce();

  my_end(0);
  return exit_status();
}
