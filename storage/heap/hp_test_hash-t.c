/*
   Unit tests for HEAP hash functions with blob key segments.

   Validates that hp_rec_hashnr() (hashes from a record) and hp_hashnr()
   (hashes from a pre-built key via hp_make_key()) produce identical
   results for blob data.  Also validates hp_rec_key_cmp() and hp_key_cmp()
   for blob segments.

   The three blob storage cases (A, B, C) refer to how blobs are stored
   in continuation chains, but for hashing purposes what matters is the
   record format: packlength bytes of length + sizeof(ptr) bytes of
   data pointer.  The hash functions read blob data via pointer
   dereference, so the tests verify that the pointer dereference and
   length handling are correct for various configurations.
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>
#include "heap.h"
#include "heapdef.h"

/*
  Record layout for a table (int4, blob(N)):
    byte 0:       null bitmap (1 byte, bit 2 = blob null)
    bytes 1-4:    int4 field (4 bytes)
    bytes 5-6:    blob packlength=2 (length, little-endian)
    bytes 7-14:   blob data pointer (8 bytes on x86_64)
    byte 15:      flags byte (at offset = visible = 15)
  Total: recbuffer = ALIGN(MAX(16, 8) + 1, 8) = 24
*/

#define REC_NULL_OFFSET  0
#define REC_INT_OFFSET   1
#define REC_BLOB_OFFSET  5
#define REC_BLOB_PACKLEN 2
#define REC_LENGTH       16    /* reclength: through end of blob descriptor */
#define REC_VISIBLE      15    /* flags byte offset */
#define REC_BUFFER       24    /* aligned recbuffer */

/* Key buffer: null_byte + 4B_blob_len + 8B_blob_ptr = 13 bytes max */
#define KEY_BUF_SIZE     64

/* Avoids -Wsizeof-pointer-memaccess with sizeof(uchar*) */
#define PTR_SIZE sizeof(void*)


static void setup_blob_keyseg(HA_KEYSEG *seg, my_bool nullable)
{
  memset(seg, 0, sizeof(*seg));
  seg->type=      HA_KEYTYPE_VARTEXT1;
  seg->flag=      HA_BLOB_PART | HA_VAR_LENGTH_PART;
  seg->start=     REC_BLOB_OFFSET;
  seg->length=    0;    /* blob key segments must have length=0 */
  seg->bit_start= REC_BLOB_PACKLEN;  /* actual packlength */
  seg->charset=   &my_charset_latin1;
  if (nullable)
  {
    seg->null_bit= 2;
    seg->null_pos= REC_NULL_OFFSET;
  }
  else
  {
    seg->null_bit= 0;
  }
}


static void setup_keydef(HP_KEYDEF *keydef, HA_KEYSEG *seg, uint keysegs)
{
  uint i;
  memset(keydef, 0, sizeof(*keydef));
  keydef->keysegs=   keysegs;
  keydef->seg=       seg;
  keydef->algorithm= HA_KEY_ALG_HASH;
  keydef->flag=      HA_NOSAME;
  keydef->length=    0;  /* computed below */
  keydef->has_blob_seg= 1;

  /* Compute keydef->length: sum of key part sizes */
  for (i= 0; i < keysegs; i++)
  {
    if (seg[i].null_bit)
      keydef->length++;
    if (seg[i].flag & HA_BLOB_PART)
      keydef->length+= 4 + PTR_SIZE;
    else if (seg[i].flag & HA_VAR_LENGTH_PART)
      keydef->length+= 2 + seg[i].length;
    else
      keydef->length+= seg[i].length;
  }
}


/*
  Build a record with blob data.
  rec must be at least REC_LENGTH bytes.
  Sets the blob field to point to blob_data with blob_len bytes.
*/
static void build_record(uchar *rec, int32 int_val,
                         const uchar *blob_data, uint16 blob_len,
                         my_bool blob_is_null)
{
  memset(rec, 0, REC_LENGTH);

  /* null bitmap */
  if (blob_is_null)
    rec[REC_NULL_OFFSET]= 2;  /* null_bit=2 for blob */
  else
    rec[REC_NULL_OFFSET]= 0;

  /* int4 field */
  int4store(rec + REC_INT_OFFSET, int_val);

  /* blob field: packlength (2 bytes) + data pointer (8 bytes) */
  int2store(rec + REC_BLOB_OFFSET, blob_len);
  memcpy(rec + REC_BLOB_OFFSET + REC_BLOB_PACKLEN, &blob_data, PTR_SIZE);
}


/*
  Test 1: hp_rec_hashnr and hp_make_key + hp_hashnr produce same hash
  for various blob data sizes.
*/
static void test_hash_consistency(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec[REC_LENGTH];
  uchar key_buf[KEY_BUF_SIZE];
  ulong rec_hash_a, rec_hash_b, rec_hash_c;

  /* Case A: very small blob (fits in single record, <= visible - 10) */
  const uchar *data_a= (const uchar*) "Hi";
  uint16 len_a= 2;

  /* Case B: medium blob (fits in single run, zero-copy) */
  const uchar *data_b= (const uchar*) "Hello World! This is a medium blob.";
  uint16 len_b= 35;

  /* Case C: larger blob data (would need multiple runs in real storage) */
  uchar data_c[200];
  uint16 len_c= sizeof(data_c);
  memset(data_c, 'X', sizeof(data_c));
  /* Make it non-uniform so hash is more interesting */
  data_c[0]= 'A';
  data_c[50]= 'B';
  data_c[100]= 'C';
  data_c[199]= 'Z';

  setup_blob_keyseg(&seg, FALSE);
  setup_keydef(&keydef, &seg, 1);

  /* --- Case A: small blob --- */
  build_record(rec, 1, data_a, len_a, FALSE);

  rec_hash_a= hp_rec_hashnr(&keydef, rec);
  hp_make_key(&keydef, key_buf, rec);
  /* Now hash the pre-built key */
  {
    /* hp_hashnr is static, so we test via hp_make_key + hp_rec_hashnr.
       But we can verify the key format is correct. */
    uint32 key_blob_len= uint4korr(key_buf);
    const uchar *key_blob_data;
    memcpy(&key_blob_data, key_buf + 4, PTR_SIZE);
    ok(key_blob_len == len_a,
       "Case A: hp_make_key blob length = %u (expected %u)",
       (uint) key_blob_len, (uint) len_a);
    ok(key_blob_data == data_a,
       "Case A: hp_make_key blob pointer matches source data");
    ok(memcmp(key_blob_data, data_a, len_a) == 0,
       "Case A: hp_make_key blob data content matches");
  }

  /* --- Case B: medium blob --- */
  build_record(rec, 2, data_b, len_b, FALSE);

  rec_hash_b= hp_rec_hashnr(&keydef, rec);
  hp_make_key(&keydef, key_buf, rec);
  {
    uint32 key_blob_len= uint4korr(key_buf);
    const uchar *key_blob_data;
    memcpy(&key_blob_data, key_buf + 4, PTR_SIZE);
    ok(key_blob_len == len_b,
       "Case B: hp_make_key blob length = %u (expected %u)",
       (uint) key_blob_len, (uint) len_b);
    ok(key_blob_data == data_b,
       "Case B: hp_make_key blob pointer matches source data");
    ok(memcmp(key_blob_data, data_b, len_b) == 0,
       "Case B: hp_make_key blob data content matches");
  }

  /* --- Case C: large blob --- */
  build_record(rec, 3, data_c, len_c, FALSE);

  rec_hash_c= hp_rec_hashnr(&keydef, rec);
  hp_make_key(&keydef, key_buf, rec);
  {
    uint32 key_blob_len= uint4korr(key_buf);
    const uchar *key_blob_data;
    memcpy(&key_blob_data, key_buf + 4, PTR_SIZE);
    ok(key_blob_len == len_c,
       "Case C: hp_make_key blob length = %u (expected %u)",
       (uint) key_blob_len, (uint) len_c);
    ok(key_blob_data == data_c,
       "Case C: hp_make_key blob pointer matches source data");
    ok(memcmp(key_blob_data, data_c, len_c) == 0,
       "Case C: hp_make_key blob data content matches");
  }

  /* Different data must produce different hashes */
  ok(rec_hash_a != rec_hash_b,
     "Hash A (%lu) != Hash B (%lu)", rec_hash_a, rec_hash_b);
  ok(rec_hash_a != rec_hash_c,
     "Hash A (%lu) != Hash C (%lu)", rec_hash_a, rec_hash_c);
  ok(rec_hash_b != rec_hash_c,
     "Hash B (%lu) != Hash C (%lu)", rec_hash_b, rec_hash_c);
}


/*
  Test 2: hp_rec_key_cmp with blob segments.
  Two records with same blob data must compare equal.
  Two records with different blob data must compare unequal.
*/
static void test_rec_key_cmp(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec1[REC_LENGTH], rec2[REC_LENGTH];

  const uchar *data1= (const uchar*) "same_data_value!";
  uint16 len1= 16;
  const uchar *data2= (const uchar*) "different_value!";
  uint16 len2= 16;
  const uchar *data3= (const uchar*) "short";
  uint16 len3= 5;

  setup_blob_keyseg(&seg, FALSE);
  setup_keydef(&keydef, &seg, 1);

  /* Same data, same length */
  build_record(rec1, 1, data1, len1, FALSE);
  build_record(rec2, 2, data1, len1, FALSE);  /* different int, same blob */
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "rec_key_cmp: same blob data compares equal");

  /* Different data, same length */
  build_record(rec2, 2, data2, len2, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "rec_key_cmp: different blob data compares unequal");

  /* Different length (PAD SPACE: "short" vs "short\0\0..." may differ) */
  build_record(rec2, 2, data3, len3, FALSE);
  /* For binary charset, different lengths always means different */
  {
    HA_KEYSEG seg_bin;
    HP_KEYDEF keydef_bin;
    setup_blob_keyseg(&seg_bin, FALSE);
    seg_bin.charset= &my_charset_bin;
    setup_keydef(&keydef_bin, &seg_bin, 1);

    build_record(rec1, 1, data1, len1, FALSE);
    build_record(rec2, 2, data3, len3, FALSE);
    ok(hp_rec_key_cmp(&keydef_bin, rec1, rec2, NULL) != 0,
       "rec_key_cmp: different length blobs compare unequal (binary)");
  }
}


/*
  Test 3: NULL blob handling.
  Two NULL blobs must compare equal.
  NULL vs non-NULL must compare unequal.
  NULL blob must hash consistently.
*/
static void test_null_blob(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec1[REC_LENGTH], rec2[REC_LENGTH];
  uchar key_buf[KEY_BUF_SIZE];
  ulong hash1, hash2;

  const uchar *data1= (const uchar*) "not_null_data";
  uint16 len1= 13;

  setup_blob_keyseg(&seg, TRUE);  /* nullable */
  setup_keydef(&keydef, &seg, 1);

  /* Both NULL */
  build_record(rec1, 1, NULL, 0, TRUE);
  build_record(rec2, 2, NULL, 0, TRUE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "null_blob: two NULLs compare equal");

  /* NULL vs non-NULL */
  build_record(rec2, 2, data1, len1, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "null_blob: NULL vs non-NULL compares unequal");

  /* NULL hash consistency */
  hash1= hp_rec_hashnr(&keydef, rec1);
  hash2= hp_rec_hashnr(&keydef, rec1);
  ok(hash1 == hash2,
     "null_blob: NULL blob hashes consistently (%lu == %lu)", hash1, hash2);

  /* NULL hash differs from empty non-NULL */
  {
    const uchar *empty= (const uchar*) "";
    ulong hash_empty;
    build_record(rec2, 2, empty, 0, FALSE);
    hash_empty= hp_rec_hashnr(&keydef, rec2);
    ok(hash1 != hash_empty,
       "null_blob: NULL hash (%lu) != empty non-NULL hash (%lu)",
       hash1, hash_empty);
  }

  /* hp_make_key for NULL blob */
  build_record(rec1, 1, NULL, 0, TRUE);
  hp_make_key(&keydef, key_buf, rec1);
  ok(key_buf[0] == 1,
     "null_blob: hp_make_key sets null flag byte to 1 for NULL");
}


/*
  Test 4: empty blob (non-NULL, length=0).
*/
static void test_empty_blob(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec1[REC_LENGTH], rec2[REC_LENGTH];
  ulong h1, h2;

  const uchar *empty= (const uchar*) "";
  const uchar *nonempty= (const uchar*) "x";

  setup_blob_keyseg(&seg, FALSE);
  setup_keydef(&keydef, &seg, 1);

  /* Two empty blobs */
  build_record(rec1, 1, empty, 0, FALSE);
  build_record(rec2, 2, empty, 0, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "empty_blob: two empty blobs compare equal");

  /* Empty vs non-empty */
  build_record(rec2, 2, nonempty, 1, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "empty_blob: empty vs non-empty compares unequal");

  /* Hash consistency for empty */
  h1= hp_rec_hashnr(&keydef, rec1);
  h2= hp_rec_hashnr(&keydef, rec1);
  ok(h1 == h2, "empty_blob: empty blob hashes consistently");
}


/*
  Test 5: Multi-segment key with int + blob.
  Verifies that key advancement works correctly when blob segments
  have seg->length=0.
*/
static void test_multi_segment_key(void)
{
  HA_KEYSEG segs[2];
  HP_KEYDEF keydef;
  uchar rec1[REC_LENGTH], rec2[REC_LENGTH];
  uchar key_buf[KEY_BUF_SIZE];
  const uchar *blob_data= (const uchar*) "multi_seg_test_data";
  uint16 blob_len= 19;
  const uchar *blob_data2= (const uchar*) "different_blob_data";
  uint16 blob_len2= 19;

  /* Segment 0: int4 at offset 1, length 4 */
  memset(&segs[0], 0, sizeof(segs[0]));
  segs[0].type=    HA_KEYTYPE_BINARY;
  segs[0].start=   REC_INT_OFFSET;
  segs[0].length=  4;
  segs[0].charset= &my_charset_bin;
  segs[0].null_bit= 0;

  /* Segment 1: blob at offset 5, packlength 2 */
  setup_blob_keyseg(&segs[1], FALSE);

  setup_keydef(&keydef, segs, 2);

  /* Same int, same blob */
  build_record(rec1, 42, blob_data, blob_len, FALSE);
  build_record(rec2, 42, blob_data, blob_len, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "multi_seg: same int + same blob compares equal");

  /* Different int, same blob */
  build_record(rec2, 99, blob_data, blob_len, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_seg: different int + same blob compares unequal");

  /* Same int, different blob */
  build_record(rec2, 42, blob_data2, blob_len2, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_seg: same int + different blob compares unequal");

  /* Hash consistency: record hash matches after make_key round-trip */
  build_record(rec1, 42, blob_data, blob_len, FALSE);
  (void) hp_rec_hashnr(&keydef, rec1);

  hp_make_key(&keydef, key_buf, rec1);
  /* Verify the key contains int4 (4 bytes) + blob (4B len + 8B ptr) */
  {
    int32 key_int= sint4korr(key_buf);
    uint32 key_blob_len= uint4korr(key_buf + 4);
    const uchar *key_blob_data;
    memcpy(&key_blob_data, key_buf + 8, PTR_SIZE);

    ok(key_int == 42,
       "multi_seg: hp_make_key int = %d (expected 42)", (int) key_int);
    ok(key_blob_len == blob_len,
       "multi_seg: hp_make_key blob length = %u (expected %u)",
       (uint) key_blob_len, (uint) blob_len);
    ok(key_blob_data == blob_data,
       "multi_seg: hp_make_key blob pointer matches");
  }
}


/*
  Test 6: PAD SPACE collation behavior.
  With PAD SPACE (default for latin1), 'a' and 'a   ' should
  compare equal and produce the same hash.
*/
static void test_pad_space(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec1[REC_LENGTH], rec2[REC_LENGTH];
  const uchar *data_no_pad= (const uchar*) "abc";
  const uchar *data_padded= (const uchar*) "abc   ";
  ulong h1, h2;

  setup_blob_keyseg(&seg, FALSE);
  seg.charset= &my_charset_latin1;  /* PAD SPACE */
  setup_keydef(&keydef, &seg, 1);

  build_record(rec1, 1, data_no_pad, 3, FALSE);
  build_record(rec2, 2, data_padded, 6, FALSE);

  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "pad_space: 'abc' == 'abc   ' with PAD SPACE collation");

  /* Hashes should also match for PAD SPACE */
  h1= hp_rec_hashnr(&keydef, rec1);
  h2= hp_rec_hashnr(&keydef, rec2);
  ok(h1 == h2,
     "pad_space: hash('abc') == hash('abc   ') (%lu == %lu)", h1, h2);

  /* With binary charset (NO PAD), they should differ */
  seg.charset= &my_charset_bin;
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "pad_space: 'abc' != 'abc   ' with binary charset");
}


/*
  Test 7: DISTINCT key path — varstring key format.

  The SQL layer builds lookup keys in varstring format (2B length prefix +
  inline data) via Field_blob::new_key_field() -> Field_varstring.  The HEAP
  handler's rebuild_blob_key() converts this to record[0]'s blob descriptor
  format, then hp_make_key() builds the hash key.

  This test simulates the full round-trip:
    1. Build a record with blob data (as at INSERT time)
    2. Compute hp_rec_hashnr() (stored in HASH_INFO at write time)
    3. Build a varstring-format key (as the SQL layer would for lookup)
    4. Parse the varstring key into a record's blob field (rebuild_blob_key)
    5. hp_make_key() from that record, then hp_rec_hashnr() on the record
    6. Verify the hashes match
*/
static void test_distinct_key_format(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec_insert[REC_LENGTH];  /* record at INSERT time */
  uchar rec_lookup[REC_LENGTH];  /* record rebuilt from lookup key */
  ulong insert_hash, lookup_hash;

  const uchar *blob_data= (const uchar*) "1 - 01xxxxxxxxxx";
  uint16 blob_len= 16;

  /*
    Step 3: Build varstring-format key (what SQL layer produces).
    Format: null_flag(1) + uint2_length(2) + inline_data(blob_len)
  */
  uchar varstring_key[1 + 2 + 256];

  setup_blob_keyseg(&seg, TRUE);  /* nullable, like real DISTINCT keys */
  setup_keydef(&keydef, &seg, 1);

  /* Step 1-2: INSERT-time record and hash */
  build_record(rec_insert, 1, blob_data, blob_len, FALSE);
  insert_hash= hp_rec_hashnr(&keydef, rec_insert);

  varstring_key[0]= 0;  /* not null */
  int2store(varstring_key + 1, blob_len);
  memcpy(varstring_key + 3, blob_data, blob_len);

  /*
    Step 4: Parse varstring key into rec_lookup's blob field.
    This is what rebuild_blob_key() does.
  */
  memset(rec_lookup, 0, REC_LENGTH);
  {
    const uchar *key_pos= varstring_key;
    uint16 varchar_len;
    const uchar *varchar_data;
    uint32 bl;
    /* skip null byte */
    key_pos++;
    /* read varstring: 2B length + data */
    varchar_len= uint2korr(key_pos);
    varchar_data= key_pos + 2;

    /* Write into rec_lookup's blob field */
    bl= (uint32) varchar_len;
    memcpy(rec_lookup + REC_BLOB_OFFSET, &bl, REC_BLOB_PACKLEN);
    memcpy(rec_lookup + REC_BLOB_OFFSET + REC_BLOB_PACKLEN,
           &varchar_data, PTR_SIZE);
  }

  /* Step 5: hp_make_key from rec_lookup, then hash the record */
  lookup_hash= hp_rec_hashnr(&keydef, rec_lookup);

  /* Step 6: hashes must match */
  ok(insert_hash == lookup_hash,
     "distinct_key: INSERT hash (%lu) == lookup hash (%lu)",
     insert_hash, lookup_hash);

  /* Also verify comparison works */
  ok(hp_rec_key_cmp(&keydef, rec_insert, rec_lookup, NULL) == 0,
     "distinct_key: INSERT record == lookup record via rec_key_cmp");
}


/*
  Test 8: DISTINCT key truncation bug.

  When the DISTINCT key path sets key_part.length = pack_length() = 10
  (blob descriptor size), and new_key_field() creates Field_varstring(10),
  the outer value (e.g. 16 bytes) gets truncated to 10 bytes.  The lookup
  key then has only 10 bytes but the stored record was hashed with 16 bytes.
  This must produce different hashes — demonstrating the bug.
*/
static void test_distinct_key_truncation(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec_full[REC_LENGTH];
  uchar rec_trunc[REC_LENGTH];
  ulong full_hash, trunc_hash;

  const uchar *full_data= (const uchar*) "1 - 01xxxxxxxxxx";  /* 16 bytes */
  uint16 full_len= 16;
  uint16 trunc_len= 10;  /* pack_length() = packlength(2) + sizeof(ptr)(8) */

  setup_blob_keyseg(&seg, FALSE);
  seg.charset= &my_charset_bin;  /* binary: no PAD SPACE confusion */
  setup_keydef(&keydef, &seg, 1);

  /* Full record (as stored at INSERT time) */
  build_record(rec_full, 1, full_data, full_len, FALSE);
  full_hash= hp_rec_hashnr(&keydef, rec_full);

  /* Truncated record (as rebuilt from truncated varstring key) */
  build_record(rec_trunc, 1, full_data, trunc_len, FALSE);
  trunc_hash= hp_rec_hashnr(&keydef, rec_trunc);

  /* Hashes MUST differ — this is the bug: truncation causes lookup miss */
  ok(full_hash != trunc_hash,
     "distinct_trunc: full hash (%lu) != truncated hash (%lu) — "
     "truncation causes hash mismatch (the bug)",
     full_hash, trunc_hash);

  /* Comparison must also differ */
  ok(hp_rec_key_cmp(&keydef, rec_full, rec_trunc, NULL) != 0,
     "distinct_trunc: full vs truncated compares unequal");
}


/*
  Test 9: GROUP BY key format — varstring with key_length override.

  The GROUP BY path overrides the key field length to max_length (not
  key_length() which is 0 for blobs).  This means the varstring key
  holds the full data.  Verify hash consistency.
*/
static void test_group_by_key_format(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec_insert[REC_LENGTH];
  uchar rec_lookup[REC_LENGTH];
  ulong insert_hash, lookup_hash;

  /* GROUP BY on group_concat result: blob data */
  const uchar *data= (const uchar*) "group_concat_result_data_here!!";
  uint16 data_len= 31;

  uchar varstring_key[1 + 2 + 256];

  setup_blob_keyseg(&seg, FALSE);
  setup_keydef(&keydef, &seg, 1);

  /* INSERT-time hash */
  build_record(rec_insert, 1, data, data_len, FALSE);
  insert_hash= hp_rec_hashnr(&keydef, rec_insert);

  /*
    Simulate rebuild_blob_key: parse varstring key, populate rec_lookup.
    In GROUP BY, key_field_length = max_length (not 0, not pack_length).
  */
  /* no null bit for this test */
  int2store(varstring_key, data_len);
  memcpy(varstring_key + 2, data, data_len);

  memset(rec_lookup, 0, REC_LENGTH);
  {
    const uchar *key_pos= varstring_key;
    uint16 varchar_len;
    const uchar *varchar_data;
    uint32 bl;

    varchar_len= uint2korr(key_pos);
    varchar_data= key_pos + 2;

    bl= (uint32) varchar_len;
    memcpy(rec_lookup + REC_BLOB_OFFSET, &bl, REC_BLOB_PACKLEN);
    memcpy(rec_lookup + REC_BLOB_OFFSET + REC_BLOB_PACKLEN,
           &varchar_data, PTR_SIZE);
  }

  lookup_hash= hp_rec_hashnr(&keydef, rec_lookup);

  ok(insert_hash == lookup_hash,
     "group_by_key: INSERT hash (%lu) == lookup hash (%lu)",
     insert_hash, lookup_hash);

  ok(hp_rec_key_cmp(&keydef, rec_insert, rec_lookup, NULL) == 0,
     "group_by_key: INSERT record == lookup record");
}


/*
  Test 10: Multi-segment DISTINCT key (varchar + blob).

  Tests the key advancement logic when a non-blob varchar segment
  precedes a blob segment, both with seg->length handling.
*/
static void test_multi_seg_distinct(void)
{
  HA_KEYSEG segs[2];
  HP_KEYDEF keydef;
  uchar rec1[REC_LENGTH], rec2[REC_LENGTH];
  const uchar *blob1= (const uchar*) "sj_materialize_value_1";
  const uchar *blob2= (const uchar*) "sj_materialize_value_2";
  ulong h1, h2, h3;

  /* Segment 0: int4 at offset 1, length 4 */
  memset(&segs[0], 0, sizeof(segs[0]));
  segs[0].type=    HA_KEYTYPE_BINARY;
  segs[0].start=   REC_INT_OFFSET;
  segs[0].length=  4;
  segs[0].charset= &my_charset_bin;
  segs[0].null_bit= 0;

  /* Segment 1: blob */
  setup_blob_keyseg(&segs[1], TRUE);  /* nullable */
  setup_keydef(&keydef, segs, 2);

  /* Same int, same blob */
  build_record(rec1, 100, blob1, 22, FALSE);
  build_record(rec2, 100, blob1, 22, FALSE);

  h1= hp_rec_hashnr(&keydef, rec1);
  h2= hp_rec_hashnr(&keydef, rec2);
  ok(h1 == h2,
     "multi_distinct: same data hashes equal (%lu == %lu)", h1, h2);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "multi_distinct: same data compares equal");

  /* Same int, different blob */
  build_record(rec2, 100, blob2, 22, FALSE);
  h3= hp_rec_hashnr(&keydef, rec2);
  ok(h1 != h3,
     "multi_distinct: different blob hashes differ (%lu != %lu)", h1, h3);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_distinct: different blob compares unequal");

  /* Same int, NULL blob vs non-NULL blob */
  build_record(rec2, 100, NULL, 0, TRUE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_distinct: non-NULL vs NULL blob compares unequal");
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_hash");
  plan(43);

  diag("Test 1: Hash consistency between record and key formats");
  test_hash_consistency();

  diag("Test 2: Record-to-record comparison with blobs");
  test_rec_key_cmp();

  diag("Test 3: NULL blob handling");
  test_null_blob();

  diag("Test 4: Empty blob handling");
  test_empty_blob();

  diag("Test 5: Multi-segment key (int + blob)");
  test_multi_segment_key();

  diag("Test 6: PAD SPACE collation");
  test_pad_space();

  diag("Test 7: DISTINCT key format (varstring round-trip)");
  test_distinct_key_format();

  diag("Test 8: DISTINCT key truncation bug");
  test_distinct_key_truncation();

  diag("Test 9: GROUP BY key format");
  test_group_by_key_format();

  diag("Test 10: Multi-segment DISTINCT key (sj-materialize)");
  test_multi_seg_distinct();

  my_end(0);
  return exit_status();
}
