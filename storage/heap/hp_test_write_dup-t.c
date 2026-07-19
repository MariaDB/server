/*
   Unit tests for unique hash key duplicate rejection cost and rollback
   correctness in the HEAP engine.

   A hash index insert must hash the key value exactly once: the hash
   selects the bucket, so it is computed unconditionally for every
   insert.  Historically a *rejected duplicate* paid the full-value
   hash twice: hp_write_key() hashed the key to insert it, and after
   the duplicate was detected the caller invoked hp_delete_key() to
   remove the just-inserted entry, which hashed the same key again to
   locate the bucket.  For long key values (blob keys) the second scan
   of the value dominates duplicate-heavy workloads such as
   SELECT DISTINCT over blobs.

   These tests wrap the key charset's hash_sort collation callback in
   a counting shim so every full-value hash of key data is observable,
   and assert:
   - a successful insert costs exactly one full-value hash (the
     duplicate probe must not add hashing);
   - a rejected duplicate insert costs exactly one full-value hash;
   - deleting or re-keying a row positioned via the same hash index
     reuses the hash cached in the index entry instead of re-hashing;
   plus rollback behavior: rejected inserts and updates leave the
   table exactly as before, including multi-key rollback, NULL key
   parts and non-unique keys.

   Record layout (same as hp_test_helpers.h, plus a nullable variant):
     byte 0:       null bitmap (1 byte, bit 2 = blob null)
     bytes 1-4:    int4 field
     bytes 5-6:    blob packlength=2 (length, little-endian)
     bytes 7-14:   blob data pointer (portable_sizeof_char_ptr bytes)
   reclength = 15
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>
#include "heap.h"
#include "heapdef.h"

#define REC_NULL_OFFSET 0
#define INT_OFFSET      1
#define BLOB_OFFSET     5
#define BLOB_PACKLEN    2
#define REC_LENGTH      15

#define BLOB_KEY_LEN    (4 + portable_sizeof_char_ptr)

/*
  In builds where DBUG_ASSERT evaluates its expression (debug builds,
  and DBUG_OFF builds compiled with DBUG_ASSERT_AS_PRINTF),
  hp_delete_key() cross-checks the cached hash against a recomputed
  one, which adds one counted hash_sort call per delete that takes the
  cached-hash path.  The cached-path count assertions are therefore
  only distinguishing when DBUG_ASSERT compiles to nothing; the
  insert-path assertions distinguish in all builds.
*/
#if defined(DBUG_OFF) && !defined(DBUG_ASSERT_AS_PRINTF)
#define DELETE_HASH_CHECK 0
#else
#define DELETE_HASH_CHECK 1
#endif

/*
  Counting charset: a copy of latin1 whose collation handler routes
  hash_sort through a wrapper that increments hash_calls.  Comparisons
  (strnncollsp) are left untouched, so the counter isolates hashing.
*/
static struct charset_info_st counting_cs;
static struct my_collation_handler_st counting_coll;
static ulong hash_calls;
static void (*real_hash_sort)(my_hasher_st *hasher, CHARSET_INFO *cs,
                              const uchar *key, size_t len);

static void counting_hash_sort(my_hasher_st *hasher, CHARSET_INFO *cs,
                               const uchar *key, size_t len)
{
  hash_calls++;
  real_hash_sort(hasher, cs, key, len);
}

static void init_counting_charset(void)
{
  counting_cs= my_charset_latin1;
  counting_coll= *my_charset_latin1.coll;
  real_hash_sort= counting_coll.hash_sort;
  counting_coll.hash_sort= counting_hash_sort;
  counting_cs.coll= &counting_coll;
}


static void build_record(uchar *rec, int32 int_val,
                         const uchar *blob_data, uint16 blob_len,
                         my_bool blob_is_null)
{
  memset(rec, 0, REC_LENGTH);
  rec[REC_NULL_OFFSET]= blob_is_null ? 2 : 0;
  int4store(rec + INT_OFFSET, int_val);
  if (!blob_is_null)
  {
    int2store(rec + BLOB_OFFSET, blob_len);
    memcpy(rec + BLOB_OFFSET + BLOB_PACKLEN, &blob_data, sizeof(blob_data));
  }
}


/* Packed key for heap_rkey on the blob key: 4-byte length + pointer */
static void build_blob_key(uchar *key, const uchar *data, uint32 len)
{
  memset(key, 0, BLOB_KEY_LEN);
  int4store(key, len);
  memcpy(key + 4, &data, sizeof(data));
}


static void init_blob_keyseg(HA_KEYSEG *seg, my_bool nullable)
{
  memset(seg, 0, sizeof(*seg));
  seg->type=      HA_KEYTYPE_VARTEXT4;
  seg->flag=      HA_BLOB_PART | HA_VAR_LENGTH_PART;
  seg->start=     BLOB_OFFSET;
  seg->length=    BLOB_KEY_LEN;
  seg->bit_start= BLOB_PACKLEN;
  seg->charset=   &counting_cs;
  if (nullable)
  {
    seg->null_bit= 2;
    seg->null_pos= REC_NULL_OFFSET;
  }
}


static void init_int_keyseg(HA_KEYSEG *seg)
{
  memset(seg, 0, sizeof(*seg));
  seg->type=    HA_KEYTYPE_BINARY;
  seg->start=   INT_OFFSET;
  seg->length=  4;
  seg->charset= &my_charset_bin;
}


static void init_keydef(HP_KEYDEF *keydef, HA_KEYSEG *seg, uint length,
                        uint8 flag)
{
  memset(keydef, 0, sizeof(*keydef));
  keydef->keysegs=   1;
  keydef->seg=       seg;
  keydef->algorithm= HA_KEY_ALG_HASH;
  keydef->flag=      flag;
  keydef->length=    length;
}


static int create_and_open(const char *name, uint keys, HP_KEYDEF *keydef,
                           HP_SHARE **share, HP_INFO **info)
{
  HP_CREATE_INFO ci;
  HP_BLOB_DESC blob_desc;
  my_bool unused;

  blob_desc.offset=     BLOB_OFFSET;
  blob_desc.packlength= BLOB_PACKLEN;

  memset(&ci, 0, sizeof(ci));
  ci.keys=           keys;
  ci.keydef=         keydef;
  ci.reclength=      REC_LENGTH;
  ci.max_records=    0;
  ci.min_records=    10;
  ci.max_table_size= 64 * 1024 * 1024;
  ci.blob_descs=     &blob_desc;
  ci.blob_count=     1;

  if (heap_create(name, &ci, share, &unused))
    return 1;
  if (!(*info= heap_open(name, 2)))
    return 1;
  heap_extra(*info, HA_EXTRA_NO_READCHECK);
  return 0;
}


/*
  Test 1: hashing cost of unique inserts and rejected duplicates.

  Every insert must cost exactly one full-value hash: one for a
  successful insert (probe + insert share the hash) and one for a
  rejected duplicate (the undo delete that re-hashed the value must
  not exist).  A rejected duplicate must leave the table unchanged
  and usable.
*/

static void test_dup_insert_hash_cost(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  uchar rec[REC_LENGTH];
  uchar blob_a[400], blob_b[400];
  ulong i, dup_errors;

  memset(blob_a, 'a', sizeof(blob_a));
  memset(blob_b, 'b', sizeof(blob_b));

  init_blob_keyseg(&keyseg, FALSE);
  init_keydef(&keydef, &keyseg, BLOB_KEY_LEN, HA_NOSAME);

  if (create_and_open("t_dup_cost", 1, &keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(12, "setup failed");
    return;
  }
  ok(1, "created table with unique blob hash key");

  build_record(rec, 1, blob_a, sizeof(blob_a), FALSE);
  hash_calls= 0;
  ok(heap_write(info, rec) == 0, "insert first row");
  ok(hash_calls == 1, "unique insert costs exactly one full-value hash "
     "(got %lu)", hash_calls);

  build_record(rec, 2, blob_a, sizeof(blob_a), FALSE);
  hash_calls= 0;
  ok(heap_write(info, rec) == HA_ERR_FOUND_DUPP_KEY,
     "duplicate blob value rejected");
  ok(hash_calls == 1, "rejected duplicate costs exactly one full-value hash "
     "(got %lu)", hash_calls);
  ok(info->errkey == 0, "errkey is the failing key (got %d)", info->errkey);
  ok(share->records == 1, "table unchanged after rejection (records=%lu)",
     (ulong) share->records);

  build_record(rec, 3, blob_b, sizeof(blob_b), FALSE);
  hash_calls= 0;
  ok(heap_write(info, rec) == 0, "different value inserted after rejection");
  ok(hash_calls == 1, "insert after rejection costs one hash (got %lu)",
     hash_calls);
  ok(share->records == 2, "records=2 after second insert");

  dup_errors= 0;
  hash_calls= 0;
  for (i= 0; i < 50; i++)
  {
    build_record(rec, (int32) (100 + i), blob_a, sizeof(blob_a), FALSE);
    if (heap_write(info, rec) == HA_ERR_FOUND_DUPP_KEY)
      dup_errors++;
  }
  ok(dup_errors == 50, "all 50 repeated duplicates rejected (got %lu)",
     dup_errors);
  ok(hash_calls == 50, "50 rejections cost exactly 50 hashes (got %lu)",
     hash_calls);

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 2: delete of a row positioned via the hash index must reuse the
  hash cached in the index entry (heap_rkey already hashed the key to
  find the row); delete of a row positioned via a table scan must fall
  back to computing the hash once.
*/

static void test_delete_hash_cost(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  uchar rec[REC_LENGTH];
  uchar key[BLOB_KEY_LEN];
  uchar blob_a[400], blob_b[400];

  memset(blob_a, 'a', sizeof(blob_a));
  memset(blob_b, 'b', sizeof(blob_b));

  init_blob_keyseg(&keyseg, FALSE);
  init_keydef(&keydef, &keyseg, BLOB_KEY_LEN, HA_NOSAME);

  if (create_and_open("t_del_cost", 1, &keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(11, "setup failed");
    return;
  }
  ok(1, "created table");

  build_record(rec, 1, blob_a, sizeof(blob_a), FALSE);
  ok(heap_write(info, rec) == 0, "insert row a");
  build_record(rec, 2, blob_b, sizeof(blob_b), FALSE);
  ok(heap_write(info, rec) == 0, "insert row b");

  /* Position row b via the hash index, then delete it */
  build_blob_key(key, blob_b, sizeof(blob_b));
  hash_calls= 0;
  ok(heap_rkey(info, rec, 0, key, 1, HA_READ_KEY_EXACT) == 0,
     "positioned row b via hash key");
  ok(heap_delete(info, rec) == 0, "deleted row b");
  ok(hash_calls == 1 + DELETE_HASH_CHECK,
     "index-read + delete hash the key exactly once (got %lu)", hash_calls);
  ok(share->records == 1, "one row left");

  /* Position the remaining row via a scan: delete must still work */
  heap_scan_init(info);
  {
    int found= 0;
    while (heap_scan(info, rec) == 0)
    {
      found= 1;
      break;
    }
    ok(found, "scan found remaining row");
  }
  hash_calls= 0;
  ok(heap_delete(info, rec) == 0, "deleted scan-positioned row");
  ok(hash_calls == 1,
     "scan-positioned delete computes the hash once (got %lu)", hash_calls);
  ok(share->records == 0, "table empty");

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 3: update re-keying a row positioned via the hash index, and an
  update rejected as duplicate.

  A successful re-key must hash only the new key value (the old key's
  hash is cached in the index entry).  A rejected update must restore
  the old key and leave both rows intact.
*/

static void test_update_rekey_and_dup(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  uchar oldrec[REC_LENGTH], newrec[REC_LENGTH], rec[REC_LENGTH];
  uchar key[BLOB_KEY_LEN];
  uchar blob_x[300], blob_y[300], blob_z[300];

  memset(blob_x, 'x', sizeof(blob_x));
  memset(blob_y, 'y', sizeof(blob_y));
  memset(blob_z, 'z', sizeof(blob_z));

  init_blob_keyseg(&keyseg, FALSE);
  init_keydef(&keydef, &keyseg, BLOB_KEY_LEN, HA_NOSAME);

  if (create_and_open("t_upd_dup", 1, &keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(15, "setup failed");
    return;
  }
  ok(1, "created table");

  build_record(rec, 1, blob_x, sizeof(blob_x), FALSE);
  ok(heap_write(info, rec) == 0, "insert row 1 (x)");
  build_record(rec, 2, blob_y, sizeof(blob_y), FALSE);
  ok(heap_write(info, rec) == 0, "insert row 2 (y)");

  /* Re-key row 2 from y to z, positioned via the index */
  build_blob_key(key, blob_y, sizeof(blob_y));
  ok(heap_rkey(info, oldrec, 0, key, 1, HA_READ_KEY_EXACT) == 0,
     "positioned row 2 via hash key");
  build_record(newrec, 2, blob_z, sizeof(blob_z), FALSE);
  hash_calls= 0;
  ok(heap_update(info, oldrec, newrec) == 0, "re-keyed row 2 from y to z");
  ok(hash_calls == 1 + DELETE_HASH_CHECK,
     "re-key hashes only the new key value (got %lu)", hash_calls);

  build_blob_key(key, blob_z, sizeof(blob_z));
  ok(heap_rkey(info, rec, 0, key, 1, HA_READ_KEY_EXACT) == 0,
     "new key z finds the row");
  ok(sint4korr(rec + INT_OFFSET) == 2, "row content preserved");
  build_blob_key(key, blob_y, sizeof(blob_y));
  ok(heap_rkey(info, rec, 0, key, 1, HA_READ_KEY_EXACT) != 0,
     "old key y no longer finds a row");

  /* Update row 2 to x: duplicate of row 1, must be rejected + rolled back */
  build_blob_key(key, blob_z, sizeof(blob_z));
  ok(heap_rkey(info, oldrec, 0, key, 1, HA_READ_KEY_EXACT) == 0,
     "re-positioned row 2 via key z");
  build_record(newrec, 2, blob_x, sizeof(blob_x), FALSE);
  ok(heap_update(info, oldrec, newrec) == HA_ERR_FOUND_DUPP_KEY,
     "update to duplicate value rejected");
  ok(info->errkey == 0, "errkey is the failing key (got %d)", info->errkey);
  ok(share->records == 2, "both rows still present");

  ok(heap_rkey(info, rec, 0, key, 1, HA_READ_KEY_EXACT) == 0,
     "row 2 still reachable via key z after rejected update");
  build_blob_key(key, blob_x, sizeof(blob_x));
  ok(heap_rkey(info, rec, 0, key, 1, HA_READ_KEY_EXACT) == 0 &&
     sint4korr(rec + INT_OFFSET) == 1,
     "row 1 still reachable via key x");

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 4: multi-key rollback.  When a later key detects a duplicate,
  keys already written for the row must be removed again, and when the
  first key detects the duplicate, later keys must never be touched.
*/

static void test_multikey_rollback(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef[2];
  HA_KEYSEG int_seg, blob_seg;
  uchar rec[REC_LENGTH];
  uchar int_key[4];
  uchar key[BLOB_KEY_LEN];
  uchar blob_x[200], blob_y[200], blob_z[200], blob_w[200];

  memset(blob_x, 'x', sizeof(blob_x));
  memset(blob_y, 'y', sizeof(blob_y));
  memset(blob_z, 'z', sizeof(blob_z));
  memset(blob_w, 'w', sizeof(blob_w));

  init_int_keyseg(&int_seg);
  init_keydef(&keydef[0], &int_seg, 4, HA_NOSAME);
  init_blob_keyseg(&blob_seg, FALSE);
  init_keydef(&keydef[1], &blob_seg, BLOB_KEY_LEN, HA_NOSAME);

  if (create_and_open("t_multikey", 2, keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(13, "setup failed");
    return;
  }
  ok(1, "created table with unique int + unique blob hash keys");

  build_record(rec, 1, blob_x, sizeof(blob_x), FALSE);
  ok(heap_write(info, rec) == 0, "insert (1, x)");
  build_record(rec, 2, blob_y, sizeof(blob_y), FALSE);
  ok(heap_write(info, rec) == 0, "insert (2, y)");

  /* key 0 accepts 3, key 1 rejects y: key 0 must be rolled back */
  build_record(rec, 3, blob_y, sizeof(blob_y), FALSE);
  ok(heap_write(info, rec) == HA_ERR_FOUND_DUPP_KEY,
     "(3, y) rejected on blob key");
  ok(info->errkey == 1, "errkey is the blob key (got %d)", info->errkey);
  int4store(int_key, 3);
  ok(heap_rkey(info, rec, 0, int_key, 1, HA_READ_KEY_EXACT) != 0,
     "int key 3 rolled back");
  build_record(rec, 3, blob_z, sizeof(blob_z), FALSE);
  ok(heap_write(info, rec) == 0, "(3, z) inserted after rollback");
  ok(share->records == 3, "records=3");

  /* key 0 rejects 2 immediately: key 1 must never see w */
  build_record(rec, 2, blob_w, sizeof(blob_w), FALSE);
  ok(heap_write(info, rec) == HA_ERR_FOUND_DUPP_KEY,
     "(2, w) rejected on int key");
  ok(info->errkey == 0, "errkey is the int key (got %d)", info->errkey);
  build_blob_key(key, blob_w, sizeof(blob_w));
  ok(heap_rkey(info, rec, 1, key, 1, HA_READ_KEY_EXACT) != 0,
     "blob key w was never inserted");
  build_record(rec, 4, blob_w, sizeof(blob_w), FALSE);
  ok(heap_write(info, rec) == 0, "(4, w) inserted");
  ok(share->records == 4, "records=4");

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 5: multi-key update rollback.  An update that changes two
  unique hash keys where the second key hits a duplicate must restore
  both original key values: the failing key was never inserted (only
  its old value is re-inserted), and the first key's new value must be
  removed and its old value re-inserted.
*/

static void test_multikey_update_rollback(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef[2];
  HA_KEYSEG int_seg, blob_seg;
  uchar oldrec[REC_LENGTH], newrec[REC_LENGTH], rec[REC_LENGTH];
  uchar int_key[4];
  uchar key[BLOB_KEY_LEN];
  uchar blob_x[200], blob_y[200];

  memset(blob_x, 'x', sizeof(blob_x));
  memset(blob_y, 'y', sizeof(blob_y));

  init_int_keyseg(&int_seg);
  init_keydef(&keydef[0], &int_seg, 4, HA_NOSAME);
  init_blob_keyseg(&blob_seg, FALSE);
  init_keydef(&keydef[1], &blob_seg, BLOB_KEY_LEN, HA_NOSAME);

  if (create_and_open("t_multikey_upd", 2, keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(11, "setup failed");
    return;
  }
  ok(1, "created table with unique int + unique blob hash keys");

  build_record(rec, 1, blob_x, sizeof(blob_x), FALSE);
  ok(heap_write(info, rec) == 0, "insert (1, x)");
  build_record(rec, 2, blob_y, sizeof(blob_y), FALSE);
  ok(heap_write(info, rec) == 0, "insert (2, y)");

  /* Update (1, x) to (3, y): int key 3 is written, blob key y dups */
  int4store(int_key, 1);
  ok(heap_rkey(info, oldrec, 0, int_key, 1, HA_READ_KEY_EXACT) == 0,
     "positioned row (1, x) via int key");
  build_record(newrec, 3, blob_y, sizeof(blob_y), FALSE);
  ok(heap_update(info, oldrec, newrec) == HA_ERR_FOUND_DUPP_KEY,
     "update (1, x) -> (3, y) rejected on blob key");
  ok(info->errkey == 1, "errkey is the blob key (got %d)", info->errkey);

  int4store(int_key, 1);
  ok(heap_rkey(info, rec, 0, int_key, 1, HA_READ_KEY_EXACT) == 0 &&
     sint4korr(rec + INT_OFFSET) == 1,
     "int key 1 restored after rollback");
  int4store(int_key, 3);
  ok(heap_rkey(info, rec, 0, int_key, 1, HA_READ_KEY_EXACT) != 0,
     "int key 3 rolled back");
  build_blob_key(key, blob_x, sizeof(blob_x));
  ok(heap_rkey(info, rec, 1, key, 1, HA_READ_KEY_EXACT) == 0 &&
     sint4korr(rec + INT_OFFSET) == 1,
     "blob key x restored after rollback");
  build_blob_key(key, blob_y, sizeof(blob_y));
  ok(heap_rkey(info, rec, 1, key, 1, HA_READ_KEY_EXACT) == 0 &&
     sint4korr(rec + INT_OFFSET) == 2,
     "blob key y still finds row 2");

  ok(share->records == 2, "records=2");

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 6: delete after a rejected ordered read on a hash index.

  heap_rfirst()/heap_rlast() retarget lastinx to the requested index
  before discovering it is a hash index and failing with
  HA_ERR_WRONG_COMMAND.  They must not leave current_hash_ptr pointing
  into another key's hash block: hp_delete_key() trusts
  current_hash_ptr's cached hash when the row was positioned via the
  index it is deleting from, so a stale pointer from a different key
  would send the bucket lookup to the wrong chain and fail with
  HA_ERR_CRASHED.
*/

static void test_delete_after_rejected_ordered_read(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef[2];
  HA_KEYSEG int_seg, blob_seg;
  uchar rec[REC_LENGTH], readbuf[REC_LENGTH];
  uchar int_key[4];
  uchar key[BLOB_KEY_LEN];
  uchar blob_x[200], blob_y[200];

  memset(blob_x, 'x', sizeof(blob_x));
  memset(blob_y, 'y', sizeof(blob_y));

  init_int_keyseg(&int_seg);
  init_keydef(&keydef[0], &int_seg, 4, HA_NOSAME);
  init_blob_keyseg(&blob_seg, FALSE);
  init_keydef(&keydef[1], &blob_seg, BLOB_KEY_LEN, HA_NOSAME);

  if (create_and_open("t_stale_cursor", 2, keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(13, "setup failed");
    return;
  }
  ok(1, "created table with unique int + unique blob hash keys");

  build_record(rec, 1, blob_x, sizeof(blob_x), FALSE);
  ok(heap_write(info, rec) == 0, "insert (1, x)");
  build_record(rec, 2, blob_y, sizeof(blob_y), FALSE);
  ok(heap_write(info, rec) == 0, "insert (2, y)");

  /* Position row 1 via the blob key, then poison lastinx via rfirst */
  build_blob_key(key, blob_x, sizeof(blob_x));
  ok(heap_rkey(info, rec, 1, key, 1, HA_READ_KEY_EXACT) == 0,
     "positioned row 1 via blob key");
  ok(heap_rfirst(info, readbuf, 0) == HA_ERR_WRONG_COMMAND,
     "rfirst on hash int key rejected");
  ok(heap_delete(info, rec) == 0,
     "delete succeeds after rejected rfirst retargeted lastinx");
  ok(share->records == 1, "one row left");
  build_blob_key(key, blob_x, sizeof(blob_x));
  ok(heap_rkey(info, rec, 1, key, 1, HA_READ_KEY_EXACT) != 0,
     "deleted row unreachable via blob key");
  int4store(int_key, 2);
  ok(heap_rkey(info, rec, 0, int_key, 1, HA_READ_KEY_EXACT) == 0,
     "row 2 still reachable via int key");

  /* Same shape with heap_rlast */
  build_blob_key(key, blob_y, sizeof(blob_y));
  ok(heap_rkey(info, rec, 1, key, 1, HA_READ_KEY_EXACT) == 0,
     "positioned row 2 via blob key");
  ok(heap_rlast(info, readbuf, 0) == HA_ERR_WRONG_COMMAND,
     "rlast on hash int key rejected");
  ok(heap_delete(info, rec) == 0,
     "delete succeeds after rejected rlast retargeted lastinx");
  ok(share->records == 0, "table empty");

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 7: NULL key parts never conflict on a unique key; non-NULL
  duplicates in the same table are still rejected.
*/

static void test_null_parts(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  uchar rec[REC_LENGTH];
  uchar blob_x[200];

  memset(blob_x, 'x', sizeof(blob_x));

  init_blob_keyseg(&keyseg, TRUE);
  init_keydef(&keydef, &keyseg, 1 + BLOB_KEY_LEN, HA_NOSAME);

  if (create_and_open("t_null_parts", 1, &keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(7, "setup failed");
    return;
  }
  ok(1, "created table with nullable unique blob hash key");

  build_record(rec, 1, NULL, 0, TRUE);
  ok(heap_write(info, rec) == 0, "insert first NULL key");
  hash_calls= 0;
  build_record(rec, 2, NULL, 0, TRUE);
  ok(heap_write(info, rec) == 0, "insert second NULL key (NULLs never dup)");
  ok(hash_calls == 0, "NULL key value is never hashed (got %lu)", hash_calls);
  ok(share->records == 2, "records=2");

  build_record(rec, 3, blob_x, sizeof(blob_x), FALSE);
  ok(heap_write(info, rec) == 0, "insert non-NULL value");
  build_record(rec, 4, blob_x, sizeof(blob_x), FALSE);
  ok(heap_write(info, rec) == HA_ERR_FOUND_DUPP_KEY,
     "non-NULL duplicate still rejected");

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 8: non-unique hash keys accept duplicates and never pay a
  duplicate probe beyond the single insert hash.
*/

static void test_non_unique_dups(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  uchar rec[REC_LENGTH];
  uchar blob_x[200];

  memset(blob_x, 'x', sizeof(blob_x));

  init_blob_keyseg(&keyseg, FALSE);
  init_keydef(&keydef, &keyseg, BLOB_KEY_LEN, 0);

  if (create_and_open("t_non_unique", 1, &keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(5, "setup failed");
    return;
  }
  ok(1, "created table with non-unique blob hash key");

  hash_calls= 0;
  build_record(rec, 1, blob_x, sizeof(blob_x), FALSE);
  ok(heap_write(info, rec) == 0, "insert value");
  build_record(rec, 2, blob_x, sizeof(blob_x), FALSE);
  ok(heap_write(info, rec) == 0, "insert same value again");
  ok(hash_calls == 2, "two inserts cost two hashes (got %lu)", hash_calls);
  ok(share->records == 2, "records=2");

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK");

  heap_drop_table(info);
  heap_close(info);
}


/*
  Test 9: duplicate-heavy insert stream across many hash table splits,
  then delete everything via index reads.

  500 inserts of 173 distinct values: exactly 173 succeed, 327 are
  rejected, and the total hashing cost is exactly 500 (one hash per
  insert attempt, successful or not).  The table stays consistent
  through the linear-hash splits interleaved with rejections.
*/

static void test_mixed_stress(void)
{
  HP_SHARE *share;
  HP_INFO *info;
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  uchar rec[REC_LENGTH];
  uchar key[BLOB_KEY_LEN];
  uchar blob_val[24];
  ulong i, successes= 0, dups= 0, delete_failures= 0;
  const ulong attempts= 500, distinct= 173;

  init_blob_keyseg(&keyseg, FALSE);
  init_keydef(&keydef, &keyseg, BLOB_KEY_LEN, HA_NOSAME);

  if (create_and_open("t_stress", 1, &keydef, &share, &info))
  {
    ok(0, "setup failed: %d", my_errno);
    skip(9, "setup failed");
    return;
  }
  ok(1, "created table");

  hash_calls= 0;
  for (i= 0; i < attempts; i++)
  {
    int res;
    memset(blob_val, 'v', sizeof(blob_val));
    my_snprintf((char*) blob_val, sizeof(blob_val), "value-%03lu",
                i % distinct);
    build_record(rec, (int32) i, blob_val, sizeof(blob_val), FALSE);
    res= heap_write(info, rec);
    if (res == 0)
      successes++;
    else if (res == HA_ERR_FOUND_DUPP_KEY)
      dups++;
  }
  ok(successes == distinct, "%lu distinct values inserted (got %lu)",
     distinct, successes);
  ok(dups == attempts - distinct, "%lu duplicates rejected (got %lu)",
     attempts - distinct, dups);
  ok(hash_calls == attempts,
     "%lu insert attempts cost exactly %lu hashes (got %lu)",
     attempts, attempts, hash_calls);
  ok(share->records == distinct, "records=%lu", (ulong) share->records);
  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK after inserts");

  hash_calls= 0;
  for (i= 0; i < distinct; i++)
  {
    memset(blob_val, 'v', sizeof(blob_val));
    my_snprintf((char*) blob_val, sizeof(blob_val), "value-%03lu", i);
    build_blob_key(key, blob_val, sizeof(blob_val));
    if (heap_rkey(info, rec, 0, key, 1, HA_READ_KEY_EXACT) ||
        heap_delete(info, rec))
      delete_failures++;
  }
  ok(delete_failures == 0, "all %lu rows deleted via index reads (%lu failed)",
     distinct, delete_failures);
  ok(hash_calls == distinct * (1 + DELETE_HASH_CHECK),
     "each index-read + delete hashes the key exactly once (got %lu)",
     hash_calls);
  ok(share->records == 0, "table empty");

  ok(heap_check_heap(info, 0) == 0, "heap_check_heap OK after deletes");

  heap_drop_table(info);
  heap_close(info);
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_write_dup");
  plan(105);

  init_counting_charset();

  diag("Test 1: hashing cost of unique inserts and rejected duplicates");
  test_dup_insert_hash_cost();

  diag("Test 2: hashing cost of deletes (index- and scan-positioned)");
  test_delete_hash_cost();

  diag("Test 3: update re-key cost and update-to-duplicate rollback");
  test_update_rekey_and_dup();

  diag("Test 4: multi-key rollback on duplicate");
  test_multikey_rollback();

  diag("Test 5: multi-key update rollback on duplicate");
  test_multikey_update_rollback();

  diag("Test 6: delete after rejected rfirst/rlast on a hash index");
  test_delete_after_rejected_ordered_read();

  diag("Test 7: NULL key parts never conflict");
  test_null_parts();

  diag("Test 8: non-unique hash keys accept duplicates");
  test_non_unique_dups();

  diag("Test 9: duplicate-heavy stress across hash splits");
  test_mixed_stress();

  my_end(0);
  return exit_status();
}
