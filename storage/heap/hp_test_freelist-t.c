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


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_freelist");
  plan(81);

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

  my_end(0);
  return exit_status();
}
