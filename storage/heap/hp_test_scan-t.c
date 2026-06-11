/*
   Unit tests for heap_scan() internal continuation record skipping.

   Verifies that heap_scan() skips blob continuation records internally
   (via goto retry) rather than returning HA_ERR_RECORD_DELETED to the
   caller.  Uses real HEAP tables with blob columns.
*/

#include "hp_test_helpers.h"


/*
  Test 1: scan with continuation records never returns HA_ERR_RECORD_DELETED.

  Inserts rows with blobs large enough to create continuation chains
  (recbuffer=16, so >5 bytes needs continuations), then scans and
  verifies that heap_scan returns only 0 or HA_ERR_END_OF_FILE.
*/
static void test_scan_skips_continuations(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar scan_buf[REC_LENGTH];
  int error;
  uint row_count= 0;
  my_bool got_record_deleted= FALSE;

  uchar blob1[50], blob2[80];
  memset(blob1, 'A', sizeof(blob1));
  memset(blob2, 'B', sizeof(blob2));
  blob1[0]= '1'; blob1[49]= 'Z';
  blob2[0]= '2'; blob2[79]= 'Z';

  if (create_and_open("test_scan_cont", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }

  build_record(rec, 1, blob1, sizeof(blob1));
  ok(heap_write(info, rec) == 0, "insert row 1 (50-byte blob)");

  build_record(rec, 2, blob2, sizeof(blob2));
  ok(heap_write(info, rec) == 0, "insert row 2 (80-byte blob)");

  ok(share->records == 2,
     "records == 2 (got %lu)", (ulong) share->records);
  ok(share->total_records > share->records,
     "total_records (%lu) > records (%lu)",
     (ulong) share->total_records, (ulong) share->records);

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after inserts with continuations");

  heap_scan_init(info);
  while ((error= heap_scan(info, scan_buf)) != HA_ERR_END_OF_FILE)
  {
    if (error == HA_ERR_RECORD_DELETED)
      got_record_deleted= TRUE;
    else if (error == 0)
      row_count++;
    else
      break;
  }

  ok(!got_record_deleted,
     "heap_scan never returned HA_ERR_RECORD_DELETED for continuations");
  ok(row_count == 2,
     "scan returned 2 rows (got %u)", row_count);

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 2: scan with deleted rows AND continuation records.

  Inserts 3 rows with blobs, deletes the middle one, then scans.
  Deleted rows should still return HA_ERR_RECORD_DELETED (existing
  behavior), but continuation records must be skipped internally.
*/
static void test_scan_deleted_plus_continuations(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar scan_buf[REC_LENGTH];
  int error;
  uint row_count= 0;
  uint deleted_count= 0;

  uchar blob1[40], blob2[60], blob3[45];
  memset(blob1, 'X', sizeof(blob1));
  memset(blob2, 'Y', sizeof(blob2));
  memset(blob3, 'Z', sizeof(blob3));

  if (create_and_open("test_scan_del", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }

  build_record(rec, 10, blob1, sizeof(blob1));
  ok(heap_write(info, rec) == 0, "insert row 10");

  build_record(rec, 20, blob2, sizeof(blob2));
  ok(heap_write(info, rec) == 0, "insert row 20");

  build_record(rec, 30, blob3, sizeof(blob3));
  ok(heap_write(info, rec) == 0, "insert row 30");

  /* Delete row 20 via key lookup */
  {
    uchar key[4];
    int4store(key, 20);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 20 for deletion");
    ok(heap_delete(info, rec) == 0, "deleted row 20");
  }

  ok(share->records == 2,
     "records == 2 after delete (got %lu)", (ulong) share->records);

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after delete with continuations");

  heap_scan_init(info);
  while ((error= heap_scan(info, scan_buf)) != HA_ERR_END_OF_FILE)
  {
    if (error == HA_ERR_RECORD_DELETED)
      deleted_count++;
    else if (error == 0)
      row_count++;
    else
      break;
  }

  ok(row_count == 2, "scan returned 2 live rows (got %u)", row_count);
  ok(deleted_count > 0,
     "scan returned HA_ERR_RECORD_DELETED for deleted slots (%u times)",
     deleted_count);

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 3: scan a non-blob table is unaffected.

  Inserts rows without blobs, scans, verifies existing behavior is
  unchanged (deleted records still return HA_ERR_RECORD_DELETED).
*/
static void test_scan_no_blobs(void)
{
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  HP_CREATE_INFO ci;
  HP_SHARE *share;
  HP_INFO *info;
  my_bool unused;
  uchar rec[REC_LENGTH];
  uchar scan_buf[REC_LENGTH];
  int error;
  uint row_count= 0;
  uint deleted_count= 0;

  memset(&keyseg, 0, sizeof(keyseg));
  keyseg.type=    HA_KEYTYPE_BINARY;
  keyseg.start=   INT_OFFSET;
  keyseg.length=  4;
  keyseg.charset= &my_charset_bin;

  memset(&keydef, 0, sizeof(keydef));
  keydef.keysegs=   1;
  keydef.seg=       &keyseg;
  keydef.algorithm= HA_KEY_ALG_HASH;
  keydef.flag=      HA_NOSAME;
  keydef.length=    4;

  memset(&ci, 0, sizeof(ci));
  ci.keys=           1;
  ci.keydef=         &keydef;
  ci.reclength=      REC_LENGTH;
  ci.max_records=    1000;
  ci.min_records=    10;
  ci.max_table_size= 1024 * 1024;

  if (heap_create("test_scan_noblob", &ci, &share, &unused))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(4, "setup failed");
    return;
  }
  info= heap_open("test_scan_noblob", 2);
  if (!info)
  {
    ok(0, "open failed: %d", my_errno);
    skip(4, "open failed");
    return;
  }

  /* Insert 3 rows (no blob data, just int) */
  memset(rec, 0, REC_LENGTH);
  int4store(rec + INT_OFFSET, 100);
  ok(heap_write(info, rec) == 0, "insert row 100");

  int4store(rec + INT_OFFSET, 200);
  ok(heap_write(info, rec) == 0, "insert row 200");

  int4store(rec + INT_OFFSET, 300);
  ok(heap_write(info, rec) == 0, "insert row 300");

  /* Delete middle row */
  {
    uchar key[4];
    int4store(key, 200);
    heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT);
    heap_delete(info, rec);
  }

  heap_scan_init(info);
  while ((error= heap_scan(info, scan_buf)) != HA_ERR_END_OF_FILE)
  {
    if (error == HA_ERR_RECORD_DELETED)
      deleted_count++;
    else if (error == 0)
      row_count++;
    else
      break;
  }

  ok(row_count == 2, "no-blob scan returned 2 live rows (got %u)", row_count);
  ok(deleted_count > 0,
     "no-blob scan returned HA_ERR_RECORD_DELETED for deleted slots (%u)",
     deleted_count);

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates non-blob table");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 4: heap_check_heap with Case A single-record continuations.

  Inserts rows with small blobs (<= 5 bytes for recbuffer=16) that use
  Case A layout (HP_ROW_SINGLE_REC: no header, data at offset 0), and
  verifies that heap_check_heap correctly counts them.  Also inserts
  a larger blob (Case B) to exercise both continuation paths.
*/
static void test_check_heap_with_case_a(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];

  uchar small_blob[3]= {'a', 'b', 'c'};
  uchar tiny_blob[5]=  {'1', '2', '3', '4', '5'};
  uchar medium_blob[50];
  memset(medium_blob, 'M', sizeof(medium_blob));

  if (create_and_open("test_check_case_a", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(8, "setup failed");
    return;
  }

  build_record(rec, 1, small_blob, sizeof(small_blob));
  ok(heap_write(info, rec) == 0, "insert row 1 (3-byte blob, Case A)");

  build_record(rec, 2, tiny_blob, sizeof(tiny_blob));
  ok(heap_write(info, rec) == 0, "insert row 2 (5-byte blob, Case A)");

  build_record(rec, 3, medium_blob, sizeof(medium_blob));
  ok(heap_write(info, rec) == 0, "insert row 3 (50-byte blob, Case B)");

  ok(share->records == 3, "records == 3 (got %lu)", (ulong) share->records);
  ok(share->total_records > share->records,
     "total_records (%lu) > records (%lu) due to continuations",
     (ulong) share->total_records, (ulong) share->records);

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates Case A + Case B continuations");

  /* Delete a Case A row and verify check still passes */
  {
    uchar key[4];
    int4store(key, 1);
    ok(heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0,
       "found row 1 (Case A) for deletion");
    ok(heap_delete(info, rec) == 0, "deleted row 1");
  }

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates after deleting Case A row");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Create and open a blob-keyed table: hash key on the blob column.

  Record layout same as hp_test_helpers.h, but the hash key is on the
  blob column instead of the int column.  This exercises check_one_key()
  blob hash materialization, because the stored record's blob pointers
  point to continuation chain heads (not raw data).
*/
static int create_and_open_blob_key(const char *name,
                                    HP_SHARE **share, HP_INFO **info)
{
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  HP_CREATE_INFO ci;
  HP_BLOB_DESC blob_desc;
  my_bool unused;

  memset(&keyseg, 0, sizeof(keyseg));
  keyseg.type=    HA_KEYTYPE_VARTEXT4;
  keyseg.start=   BLOB_OFFSET;
  keyseg.length=  4 + portable_sizeof_char_ptr;
  keyseg.bit_start= BLOB_PACKLEN;
  keyseg.charset= &my_charset_bin;
  keyseg.flag=    HA_BLOB_PART;

  memset(&keydef, 0, sizeof(keydef));
  keydef.keysegs=   1;
  keydef.seg=       &keyseg;
  keydef.algorithm= HA_KEY_ALG_HASH;
  keydef.flag=      0;
  keydef.length=    keyseg.length;

  blob_desc.offset=     BLOB_OFFSET;
  blob_desc.packlength= BLOB_PACKLEN;

  memset(&ci, 0, sizeof(ci));
  ci.keys=           1;
  ci.keydef=         &keydef;
  ci.reclength=      REC_LENGTH;
  ci.max_records=    1000;
  ci.min_records=    10;
  ci.max_table_size= 1024 * 1024;
  ci.blob_descs=     &blob_desc;
  ci.blob_count=     1;

  if (heap_create(name, &ci, share, &unused))
    return 1;
  *info= heap_open(name, 2);
  if (!*info)
    return 1;
  heap_extra(*info, HA_EXTRA_NO_READCHECK);
  return 0;
}


/*
  Test 5: heap_check_heap with blob hash key.

  Creates a table with a hash key on the blob column, inserts rows
  with Case A, B, and C blobs, and calls heap_check_heap().  This
  exercises check_one_key() blob hash materialization: the stored
  record's blob pointers point to chain heads (with headers for
  Case B/C), not raw data, so the hash must be recomputed via
  hp_materialize_one_blob().
*/
static void test_check_heap_blob_key(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];

  uchar blob_a[3]= {'X', 'Y', 'Z'};
  uchar blob_b[60];
  uchar blob_c[200];
  memset(blob_b, 'B', sizeof(blob_b));
  memset(blob_c, 'C', sizeof(blob_c));
  blob_b[0]= '<'; blob_b[59]= '>';
  blob_c[0]= '{'; blob_c[199]= '}';

  if (create_and_open_blob_key("test_blob_key", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(9, "setup failed");
    return;
  }

  /* Case A: blob <= 5 bytes */
  build_record(rec, 1, blob_a, sizeof(blob_a));
  ok(heap_write(info, rec) == 0, "insert row 1 (3-byte blob, Case A key)");

  /* Case B: blob fits in single run, zero-copy layout */
  build_record(rec, 2, blob_b, sizeof(blob_b));
  ok(heap_write(info, rec) == 0, "insert row 2 (60-byte blob, Case B key)");

  /* Case C: blob large enough for multi-run */
  build_record(rec, 3, blob_c, sizeof(blob_c));
  ok(heap_write(info, rec) == 0, "insert row 3 (200-byte blob, Case C key)");

  ok(share->records == 3, "records == 3 (got %lu)", (ulong) share->records);

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates blob hash key (Case A + B + C)");

  /* Delete Case B row and verify check still passes */
  {
    uchar read_buf[REC_LENGTH];
    heap_scan_init(info);
    while (heap_scan(info, read_buf) == 0)
    {
      uint16 len= uint2korr(read_buf + BLOB_OFFSET);
      if (len == sizeof(blob_b))
      {
        ok(heap_delete(info, read_buf) == 0, "deleted Case B row");
        break;
      }
    }
  }

  ok(share->records == 2, "records == 2 after delete (got %lu)",
     (ulong) share->records);

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates blob key after delete");

  /* Insert new row to verify reuse + check */
  {
    uchar blob_new[40];
    memset(blob_new, 'N', sizeof(blob_new));
    build_record(rec, 4, blob_new, sizeof(blob_new));
    ok(heap_write(info, rec) == 0,
       "insert row 4 (40-byte blob, reuse after delete)");
  }

  ok(heap_check_heap(info, 0) == 0,
     "heap_check_heap validates blob key after reinsert");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 6: hp_key_cmp() blob mismatch via hash collision.

  Two 2-byte blobs \x00\x69 and \x01\x07 produce identical
  my_hash_sort_bin() output (hash 66889, initial state nr=1 nr2=4).
  Inserting both into a blob hash key table puts them in the same
  hash chain.  Looking up the first-inserted value walks through the
  second entry first (LIFO), calling hp_key_cmp() which returns 1
  (mismatch on the strnncollsp comparison), then finds the match.
*/
static void test_key_cmp_blob_collision(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  uchar rec[REC_LENGTH];
  uchar read_buf[REC_LENGTH];
  uchar key_buf[4 + sizeof(uchar*)];

  /*
    Pre-computed hash collision pair for my_hash_sort_bin()
    with initial state nr=1, nr2=4.
  */
  uchar blob_val1[2]= {0x00, 0x69};
  uchar blob_val2[2]= {0x01, 0x07};

  if (create_and_open_blob_key("test_collision", &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }

  build_record(rec, 1, blob_val1, sizeof(blob_val1));
  ok(heap_write(info, rec) == 0, "insert blob \\x00\\x69");

  build_record(rec, 2, blob_val2, sizeof(blob_val2));
  ok(heap_write(info, rec) == 0, "insert blob \\x01\\x07 (same hash)");

  /*
    Look up the first-inserted record.  hp_search() walks the chain
    starting from the most-recently-inserted entry (blob_val2).
    hp_key_cmp() compares blob_val2 against the search key (blob_val1)
    and returns 1 (mismatch), exercising hp_hash.c line 652.
  */
  build_record(rec, 1, blob_val1, sizeof(blob_val1));
  hp_make_key(share->keydef, key_buf, rec);
  ok(heap_rkey(info, read_buf, 0, key_buf, 1, HA_READ_KEY_EXACT) == 0,
     "lookup blob \\x00\\x69 through collision chain");

  /* Verify we got the right record with correct blob content */
  ok(sint4korr(read_buf + INT_OFFSET) == 1,
     "found record has int_val=1 (got %d)", sint4korr(read_buf + INT_OFFSET));
  {
    uint16 got_len= uint2korr(read_buf + BLOB_OFFSET);
    const uchar *got_data;
    memcpy(&got_data, read_buf + BLOB_OFFSET + BLOB_PACKLEN, sizeof(got_data));
    ok(got_len == 2 && got_data[0] == 0x00 && got_data[1] == 0x69,
       "returned blob is \\x00\\x69 (got len=%u [%02x,%02x])",
       got_len, got_data[0], got_data[1]);
  }

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap after collision test");

  heap_drop_table(info);
  heap_close(info);
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_scan");
  plan(47);

  diag("Test 1: scan skips continuation records internally");
  test_scan_skips_continuations();

  diag("Test 2: deleted rows + continuations");
  test_scan_deleted_plus_continuations();

  diag("Test 3: non-blob table scan unchanged");
  test_scan_no_blobs();

  diag("Test 4: heap_check_heap with Case A single-record continuations");
  test_check_heap_with_case_a();

  diag("Test 5: heap_check_heap with blob hash key (materialization)");
  test_check_heap_blob_key();

  diag("Test 6: hp_key_cmp blob mismatch via pre-computed hash collision");
  test_key_cmp_blob_collision();

  my_end(0);
  return exit_status();
}
