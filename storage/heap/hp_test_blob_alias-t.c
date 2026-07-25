/*
   Unit tests for writing a record whose blob data still lives in a chain
   parked for deferred free.

   heap_update() and heap_delete() park the old blob chain instead of freeing
   it, so the record buffer the caller read into keeps a valid zero-copy
   pointer for the rest of the statement.  heap_write() redeems that parking.

   The SQL layer depends on both halves at once.  A system-versioned UPDATE
   calls ha_update_row() and then writes the history row from the same
   pre-update record buffer, so heap_write() sees a record whose blob source
   is the chain the update just parked -- while that same buffer stays live as
   record[1], which an AFTER UPDATE trigger reads OLD.<blob> from.

   These tests pin that redeeming the parking preserves the data reachable
   through both record buffers: the one being written and the one still held
   by the caller.

   They also pin how that is done.  A record whose blob still lives in a
   parked chain adopts the chain instead of allocating a second copy of bytes
   that are already in place, so the write costs one record slot -- its own --
   and no chain.  At max_heap_table_size that is the difference between the
   write succeeding and failing, so it is checked directly here, through the
   adopted chain pointer and the number of slots the table handed out, rather
   than through a table filled to capacity where free space is a function of
   block granularity and pointer width.
*/

#include "hp_test_helpers.h"

#define MAX_PAYLOAD 256


/*
  Non-uniform payload, so that an overlapping or recycled copy shows up as
  wrong data rather than accidentally-correct repeated bytes.
*/

static void fill_pattern(uchar *buf, uint16 len)
{
  uint16 i;
  for (i= 0; i < len; i++)
    buf[i]= (uchar) ('a' + (i % 26));
}


static const uchar *blob_data_ptr(const uchar *rec)
{
  const uchar *ptr;
  memcpy(&ptr, rec + BLOB_OFFSET + BLOB_PACKLEN, sizeof(ptr));
  return ptr;
}


static uint16 blob_data_len(const uchar *rec)
{
  return uint2korr(rec + BLOB_OFFSET);
}


static my_bool blob_matches(const uchar *rec, const uchar *expected,
                            uint16 expected_len)
{
  return blob_data_len(rec) == expected_len &&
         memcmp(blob_data_ptr(rec), expected, expected_len) == 0;
}


/*
  Leave the free list holding one run that is too short for the payload that
  follows, so its chain has to be assembled from that run plus the tail.
  hp_read_blobs() cannot hand out a zero-copy pointer into a multi-run chain
  and reassembles it into info->blob_buff instead.
*/

static void fragment_free_list(HP_INFO *info)
{
  uchar rec[REC_LENGTH];
  uchar filler[100];
  uchar key[4];

  memset(filler, 'F', sizeof(filler));

  build_record(rec, 100, filler, sizeof(filler));
  (void) heap_write(info, rec);
  build_record(rec, 101, filler, 1);            /* guard, keeps the run apart
                                                   from what follows it */
  (void) heap_write(info, rec);

  int4store(key, 100);
  if (heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0)
    (void) heap_delete(info, rec);
  heap_reset(info);                             /* redeem the parking; the run
                                                   is now genuinely free */
}


/*
  Test: the system-versioned UPDATE sequence.

    1. Insert a row, read it back.  The record buffer now holds the blob the
       way hp_read_blobs() chose to present it -- this models record[1].
    2. heap_update() to a shorter blob.  The old chain is parked, not freed,
       precisely so that buffer stays readable.
    3. heap_write() a record copied verbatim from that buffer, differing only
       in the key.  This models vers_insert_history_row(), which does
       restore_record(table, record[1]) and changes only row_end.

  Both records must still carry the pre-update blob afterwards: the row that
  was written (checked by reading it back) and the caller's buffer (checked
  in place, before any further read, since a read may reuse info->blob_buff).

  @param expect_alloc_growth  Record slots the history write may hand out, or
                              -1 where the free list makes it indeterminate.
*/

static void test_write_from_parked_chain(uint16 payload_len,
                                         my_bool fragment,
                                         my_bool want_reassembled,
                                         int expect_alloc_growth,
                                         const char *case_name)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH], old_rec[REC_LENGTH], new_rec[REC_LENGTH];
  uchar hist_rec[REC_LENGTH], read_rec[REC_LENGTH];
  uchar payload[MAX_PAYLOAD];
  const uchar *short_blob= (const uchar*) "s";
  const uchar *parked_chain, *stored_chain;
  ulong alloc_before, alloc_after;
  uchar key[4];

  fill_pattern(payload, payload_len);

  if (create_and_open("test_blob_alias", &share, &info))
  {
    ok(0, "%s: setup failed: %d", case_name, my_errno);
    skip(12, "setup failed");
    return;
  }

  if (fragment)
    fragment_free_list(info);

  build_record(rec, 1, payload, payload_len);
  ok(heap_write(info, rec) == 0, "%s: inserted base row", case_name);

  /* Read the row back the way the SQL layer fills record[1] */
  int4store(key, 1);
  ok(heap_rkey(info, old_rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
     "%s: read base row into record[1]", case_name);
  ok(blob_matches(old_rec, payload, payload_len),
     "%s: record[1] blob reads correctly before the update", case_name);

  /*
    Which layout hp_read_blobs() handed out decides whether the buffer can
    alias a chain at all: a zero-copy pointer aims into HP_BLOCK, while a
    reassembled multi-run blob lives in info->blob_buff and never can.  Pin
    it, so that a layout change cannot silently drop the case being covered.
  */
  ok((blob_data_ptr(old_rec) == info->blob_buff) == want_reassembled,
     "%s: record[1] blob is %s as intended", case_name,
     want_reassembled ? "reassembled into info->blob_buff"
                      : "zero-copy into HP_BLOCK");

  /* ha_update_row(): the row shrinks, so the old chain is parked */
  memcpy(new_rec, old_rec, REC_LENGTH);
  int2store(new_rec + BLOB_OFFSET, 1);
  memcpy(new_rec + BLOB_OFFSET + BLOB_PACKLEN, &short_blob,
         sizeof(short_blob));
  ok(heap_update(info, old_rec, new_rec) == 0,
     "%s: updated base row to a shorter blob", case_name);

  /*
    The chain just parked is the one the history row below will be asked to
    source its blob from.  Remember it, and how many record slots the table
    has handed out, so the write can be checked for taking the chain over
    rather than allocating a second copy of it.
  */
  parked_chain= info->pending_blob_chains[0];
  ok(parked_chain != NULL, "%s: the update parked the old chain", case_name);
  alloc_before= (ulong) share->block.last_allocated;

  /*
    vers_insert_history_row(): the history row is record[1] verbatim, blob
    pointer included, with only row_end (here the key) changed.
  */
  memcpy(hist_rec, old_rec, REC_LENGTH);
  int4store(hist_rec + INT_OFFSET, 2);
  ok(heap_write(info, hist_rec) == 0, "%s: wrote history row", case_name);
  alloc_after= (ulong) share->block.last_allocated;

  /*
    record[1] stays live for the rest of the statement -- an AFTER UPDATE
    trigger reads OLD.<blob> from it, and the row-based binlog image is built
    from it -- so redeeming the parked chain must not cost it its data.
    Checked before any further read, which could reuse info->blob_buff.
  */
  ok(blob_matches(old_rec, payload, payload_len),
     "%s: record[1] blob still reads correctly after the history write",
     case_name);

  /* The stored history row must carry the pre-update blob */
  int4store(key, 2);
  ok(heap_rkey(info, read_rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
     "%s: read the history row back", case_name);
  ok(blob_matches(read_rec, payload, payload_len),
     "%s: stored history row keeps the pre-update blob", case_name);

  /*
    The stored row's blob field holds the head of the chain it owns, so
    comparing it with the chain the update parked says outright whether the
    write adopted that chain or built its own.  A reassembled multi-run blob
    lives in info->blob_buff and sources nothing from the chain, so there it
    must allocate.
  */
  stored_chain= blob_data_ptr(info->current_ptr);
  ok((stored_chain == parked_chain) == !want_reassembled,
     "%s: history row %s the parked chain", case_name,
     want_reassembled ? "allocated its own chain rather than adopting"
                      : "adopted");

  if (expect_alloc_growth >= 0)
    ok(alloc_after - alloc_before == (ulong) expect_alloc_growth,
       "%s: history write handed out %d record slot(s), no chain "
       "(before=%lu, after=%lu)",
       case_name, expect_alloc_growth, alloc_before, alloc_after);
  else
    skip(1, "%s: free list makes the slot count indeterminate here",
         case_name);

  ok(heap_check_heap(info, 0) == 0, "%s: heap consistent", case_name);

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test: the same hazard reached through heap_delete().

  heap_delete() parks the chain for the same reason heap_update() does, so a
  write issued while that parking is outstanding must preserve the deleted
  row's buffer just as well.
*/

static void test_write_from_parked_chain_after_delete(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH], old_rec[REC_LENGTH], hist_rec[REC_LENGTH];
  uchar read_rec[REC_LENGTH];
  uchar payload[MAX_PAYLOAD];
  const uchar *parked_chain, *stored_chain;
  ulong alloc_before, alloc_after;
  uchar key[4];
  const uint16 payload_len= 60;

  fill_pattern(payload, payload_len);

  if (create_and_open("test_blob_alias_del", &share, &info))
  {
    ok(0, "delete: setup failed: %d", my_errno);
    skip(9, "setup failed");
    return;
  }

  build_record(rec, 1, payload, payload_len);
  ok(heap_write(info, rec) == 0, "delete: inserted base row");

  int4store(key, 1);
  ok(heap_rkey(info, old_rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
     "delete: read base row into record[1]");
  ok(heap_delete(info, old_rec) == 0, "delete: deleted base row");

  parked_chain= info->pending_blob_chains[0];
  ok(parked_chain != NULL, "delete: the delete parked the old chain");
  alloc_before= (ulong) share->block.last_allocated;

  memcpy(hist_rec, old_rec, REC_LENGTH);
  int4store(hist_rec + INT_OFFSET, 2);
  ok(heap_write(info, hist_rec) == 0, "delete: wrote history row");
  alloc_after= (ulong) share->block.last_allocated;

  ok(blob_matches(old_rec, payload, payload_len),
     "delete: record[1] blob still reads correctly after the history write");

  int4store(key, 2);
  if (heap_rkey(info, read_rec, 0, key, 4, HA_READ_KEY_EXACT) == 0)
    ok(blob_matches(read_rec, payload, payload_len),
       "delete: stored history row keeps the pre-delete blob");
  else
    ok(0, "delete: could not read the history row back");

  stored_chain= blob_data_ptr(info->current_ptr);
  ok(stored_chain == parked_chain, "delete: history row adopted the "
     "parked chain");

  /*
    The delete put its base record on the free list and parked its chain, so
    the write reuses that slot and takes the chain over: nothing new at all.
  */
  ok(alloc_after == alloc_before,
     "delete: history write handed out no new record slots "
     "(before=%lu, after=%lu)", alloc_before, alloc_after);

  ok(heap_check_heap(info, 0) == 0, "delete: heap consistent");

  heap_drop_table(info);
  heap_close(info);
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_blob_alias");
  plan(62);

  /*
    Nothing is on the free list in the first three cases, so the history write
    can only take its one base record from the tail: a growth of exactly 1
    says it adopted the chain, and anything more says it allocated one.
  */
  diag("Test 1: history write, 3-byte blob (data at offset 0 of the chain)");
  test_write_from_parked_chain(3, FALSE, FALSE, 1, "3-byte");

  diag("Test 2: history write, 60-byte blob (zero-copy single run)");
  test_write_from_parked_chain(60, FALSE, FALSE, 1, "60-byte");

  diag("Test 3: history write, 200-byte blob (zero-copy single run)");
  test_write_from_parked_chain(200, FALSE, FALSE, 1, "200-byte");

  diag("Test 4: history write, 200-byte blob reassembled from a multi-run "
       "chain -- the layout that cannot alias");
  test_write_from_parked_chain(200, TRUE, TRUE, -1, "multi-run");

  diag("Test 5: history write while a delete's chain is parked");
  test_write_from_parked_chain_after_delete();

  my_end(0);
  return exit_status();
}
