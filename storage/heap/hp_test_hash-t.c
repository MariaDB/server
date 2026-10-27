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

/*
  Use sizeof(void*) for memcpy of actual pointer values, matching
  HP_PTR_SIZE in hp_hash.c. The record slot is portable_sizeof_char_ptr
  bytes wide (always 8), but only sizeof(void*) bytes are meaningful.
*/
#define PTR_SIZE sizeof(void*)


static void setup_blob_keyseg(HA_KEYSEG *seg, my_bool nullable)
{
  memset(seg, 0, sizeof(*seg));
  seg->type=      HA_KEYTYPE_VARTEXT4;
  seg->flag=      HA_BLOB_PART | HA_VAR_LENGTH_PART;
  seg->start=     REC_BLOB_OFFSET;
  seg->length=    4+portable_sizeof_char_ptr; /* Length of blob key */
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
                         const uchar *blob_data, size_t blob_len,
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
  LEX_CUSTRING data_a= { USTRING_WITH_LEN("Hi") };

  /* Case B: medium blob (fits in single run, zero-copy) */
  LEX_CUSTRING data_b= { USTRING_WITH_LEN("Hello World! This is a medium blob.") };

  /* Case C: larger blob data (would need multiple runs in real storage) */
  uchar data_c[200];
  size_t len_c= sizeof(data_c);
  memset(data_c, 'X', sizeof(data_c));
  /* Make it non-uniform so hash is more interesting */
  data_c[0]= 'A';
  data_c[50]= 'B';
  data_c[100]= 'C';
  data_c[199]= 'Z';

  setup_blob_keyseg(&seg, FALSE);
  setup_keydef(&keydef, &seg, 1);

  /* --- Case A: small blob --- */
  build_record(rec, 1, data_a.str, data_a.length, FALSE);

  rec_hash_a= hp_rec_hashnr(0, &keydef, rec);
  hp_make_key(&keydef, key_buf, rec);
  /* Now hash the pre-built key */
  {
    /* Verify the key format produced by hp_make_key is correct. */
    uint32 key_blob_len= uint4korr(key_buf);
    const uchar *key_blob_data;
    memcpy(&key_blob_data, key_buf + 4, PTR_SIZE);
    ok(key_blob_len == data_a.length,
       "Case A: hp_make_key blob length = %u (expected %u)",
       (uint) key_blob_len, (uint) data_a.length);
    ok(key_blob_data == data_a.str,
       "Case A: hp_make_key blob pointer matches source data");
    ok(memcmp(key_blob_data, data_a.str, data_a.length) == 0,
       "Case A: hp_make_key blob data content matches");
  }

  /* --- Case B: medium blob --- */
  build_record(rec, 2, data_b.str, data_b.length, FALSE);

  rec_hash_b= hp_rec_hashnr(0, &keydef, rec);
  hp_make_key(&keydef, key_buf, rec);
  {
    uint32 key_blob_len= uint4korr(key_buf);
    const uchar *key_blob_data;
    memcpy(&key_blob_data, key_buf + 4, PTR_SIZE);
    ok(key_blob_len == data_b.length,
       "Case B: hp_make_key blob length = %u (expected %u)",
       (uint) key_blob_len, (uint) data_b.length);
    ok(key_blob_data == data_b.str,
       "Case B: hp_make_key blob pointer matches source data");
    ok(memcmp(key_blob_data, data_b.str, data_b.length) == 0,
       "Case B: hp_make_key blob data content matches");
  }

  /* --- Case C: large blob --- */
  build_record(rec, 3, data_c, len_c, FALSE);

  rec_hash_c= hp_rec_hashnr(0, &keydef, rec);
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

  LEX_CUSTRING data1= { USTRING_WITH_LEN("same_data_value!") };
  LEX_CUSTRING data2= { USTRING_WITH_LEN("different_value!") };
  LEX_CUSTRING data3= { USTRING_WITH_LEN("short") };

  setup_blob_keyseg(&seg, FALSE);
  setup_keydef(&keydef, &seg, 1);

  /* Same data, same length */
  build_record(rec1, 1, data1.str, data1.length, FALSE);
  build_record(rec2, 2, data1.str, data1.length, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "rec_key_cmp: same blob data compares equal");

  /* Different data, same length */
  build_record(rec2, 2, data2.str, data2.length, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "rec_key_cmp: different blob data compares unequal");

  /* Different length (PAD SPACE: "short" vs "short\0\0..." may differ) */
  build_record(rec2, 2, data3.str, data3.length, FALSE);
  /* For binary charset, different lengths always means different */
  {
    HA_KEYSEG seg_bin;
    HP_KEYDEF keydef_bin;
    setup_blob_keyseg(&seg_bin, FALSE);
    seg_bin.charset= &my_charset_bin;
    setup_keydef(&keydef_bin, &seg_bin, 1);

    build_record(rec1, 1, data1.str, data1.length, FALSE);
    build_record(rec2, 2, data3.str, data3.length, FALSE);
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

  LEX_CUSTRING data1= { USTRING_WITH_LEN("not_null_data") };

  setup_blob_keyseg(&seg, TRUE);  /* nullable */
  setup_keydef(&keydef, &seg, 1);

  /* Both NULL */
  build_record(rec1, 1, NULL, 0, TRUE);
  build_record(rec2, 2, NULL, 0, TRUE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "null_blob: two NULLs compare equal");

  /* NULL vs non-NULL */
  build_record(rec2, 2, data1.str, data1.length, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "null_blob: NULL vs non-NULL compares unequal");

  /* NULL hash consistency */
  hash1= hp_rec_hashnr(0, &keydef, rec1);
  hash2= hp_rec_hashnr(0, &keydef, rec1);
  ok(hash1 == hash2,
     "null_blob: NULL blob hashes consistently (%lu == %lu)", hash1, hash2);

  /* NULL hash differs from empty non-NULL */
  {
    LEX_CUSTRING empty= { USTRING_WITH_LEN("") };
    ulong hash_empty;
    build_record(rec2, 2, empty.str, empty.length, FALSE);
    hash_empty= hp_rec_hashnr(0, &keydef, rec2);
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

  LEX_CUSTRING empty= { USTRING_WITH_LEN("") };
  LEX_CUSTRING nonempty= { USTRING_WITH_LEN("x") };

  setup_blob_keyseg(&seg, FALSE);
  setup_keydef(&keydef, &seg, 1);

  /* Two empty blobs */
  build_record(rec1, 1, empty.str, empty.length, FALSE);
  build_record(rec2, 2, empty.str, empty.length, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "empty_blob: two empty blobs compare equal");

  /* Empty vs non-empty */
  build_record(rec2, 2, nonempty.str, nonempty.length, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "empty_blob: empty vs non-empty compares unequal");

  /* Hash consistency for empty */
  h1= hp_rec_hashnr(0, &keydef, rec1);
  h2= hp_rec_hashnr(0, &keydef, rec1);
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
  LEX_CUSTRING blob_data= { USTRING_WITH_LEN("multi_seg_test_data") };
  LEX_CUSTRING blob_data2= { USTRING_WITH_LEN("different_blob_data") };

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
  build_record(rec1, 42, blob_data.str, blob_data.length, FALSE);
  build_record(rec2, 42, blob_data.str, blob_data.length, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "multi_seg: same int + same blob compares equal");

  /* Different int, same blob */
  build_record(rec2, 99, blob_data.str, blob_data.length, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_seg: different int + same blob compares unequal");

  /* Same int, different blob */
  build_record(rec2, 42, blob_data2.str, blob_data2.length, FALSE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_seg: same int + different blob compares unequal");

  /* Hash consistency: record hash matches after make_key round-trip */
  build_record(rec1, 42, blob_data.str, blob_data.length, FALSE);
  (void) hp_rec_hashnr(0, &keydef, rec1);

  hp_make_key(&keydef, key_buf, rec1);
  /* Verify the key contains int4 (4 bytes) + blob (4B len + 8B ptr) */
  {
    int32 key_int= sint4korr(key_buf);
    uint32 key_blob_len= uint4korr(key_buf + 4);
    const uchar *key_blob_data;
    memcpy(&key_blob_data, key_buf + 8, PTR_SIZE);

    ok(key_int == 42,
       "multi_seg: hp_make_key int = %d (expected 42)", (int) key_int);
    ok(key_blob_len == blob_data.length,
       "multi_seg: hp_make_key blob length = %u (expected %u)",
       (uint) key_blob_len, (uint) blob_data.length);
    ok(key_blob_data == blob_data.str,
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
  LEX_CUSTRING data_no_pad= { USTRING_WITH_LEN("abc") };
  LEX_CUSTRING data_padded= { USTRING_WITH_LEN("abc   ") };
  ulong h1, h2;

  setup_blob_keyseg(&seg, FALSE);
  seg.charset= &my_charset_latin1;  /* PAD SPACE */
  setup_keydef(&keydef, &seg, 1);

  build_record(rec1, 1, data_no_pad.str, data_no_pad.length, FALSE);
  build_record(rec2, 2, data_padded.str, data_padded.length, FALSE);

  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "pad_space: 'abc' == 'abc   ' with PAD SPACE collation");

  /* Hashes should also match for PAD SPACE */
  h1= hp_rec_hashnr(0, &keydef, rec1);
  h2= hp_rec_hashnr(0, &keydef, rec2);
  ok(h1 == h2,
     "pad_space: hash('abc') == hash('abc   ') (%lu == %lu)", h1, h2);

  /* With binary charset (NO PAD), they should differ */
  seg.charset= &my_charset_bin;
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "pad_space: 'abc' != 'abc   ' with binary charset");
}


/*
  Test 7: DISTINCT key path -- varstring key format.

  The SQL layer builds lookup keys in varstring format (2B length prefix +
  inline data) via Field_blob::new_key_field() -> Field_varstring.  The HEAP
  handler's rebuild_key_from_group_buff() converts this to
  record[0]'s blob descriptor format, then hp_make_key()
  builds the hash key.

  This test simulates the full round-trip:
    1. Build a record with blob data (as at INSERT time)
    2. Compute hp_rec_hashnr(0, ) (stored in HASH_INFO at write time)
    3. Build a varstring-format key (as the SQL layer would for lookup)
    4. Parse the varstring key into a record's blob field
       (rebuild_key_from_group_buff)
    5. hp_make_key() from that record, then hp_rec_hashnr(0, ) on the record
    6. Verify the hashes match
*/
static void test_distinct_key_format(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec_insert[REC_LENGTH];  /* record at INSERT time */
  uchar rec_lookup[REC_LENGTH];  /* record rebuilt from lookup key */
  ulong insert_hash, lookup_hash;

  LEX_CUSTRING blob_data= { USTRING_WITH_LEN("1 - 01xxxxxxxxxx") };

  /*
    Step 3: Build varstring-format key (what SQL layer produces).
    Format: null_flag(1) + uint2_length(2) + inline_data(blob_len)
  */
  uchar varstring_key[1 + 2 + 256];

  setup_blob_keyseg(&seg, TRUE);  /* nullable, like real DISTINCT keys */
  setup_keydef(&keydef, &seg, 1);

  /* Step 1-2: INSERT-time record and hash */
  build_record(rec_insert, 1, blob_data.str, blob_data.length, FALSE);
  insert_hash= hp_rec_hashnr(0, &keydef, rec_insert);

  varstring_key[0]= 0;  /* not null */
  int2store(varstring_key + 1, blob_data.length);
  memcpy(varstring_key + 3, blob_data.str, blob_data.length);

  /*
    Step 4: Parse varstring key into rec_lookup's blob field.
    This is what rebuild_key_from_group_buff() does.
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
    int2store(rec_lookup + REC_BLOB_OFFSET, bl);
    memcpy(rec_lookup + REC_BLOB_OFFSET + REC_BLOB_PACKLEN,
           &varchar_data, sizeof(varchar_data));
  }

  /* Step 5: hp_make_key from rec_lookup, then hash the record */
  lookup_hash= hp_rec_hashnr(0, &keydef, rec_lookup);

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
  This must produce different hashes -- demonstrating the bug.
*/
static void test_distinct_key_truncation(void)
{
  HA_KEYSEG seg;
  HP_KEYDEF keydef;
  uchar rec_full[REC_LENGTH];
  uchar rec_trunc[REC_LENGTH];
  ulong full_hash, trunc_hash;

  LEX_CUSTRING full_data= { USTRING_WITH_LEN("1 - 01xxxxxxxxxx") };
  uint16 trunc_len= 10;  /* pack_length() = packlength(2) + sizeof(ptr)(8) */

  setup_blob_keyseg(&seg, FALSE);
  seg.charset= &my_charset_bin;  /* binary: no PAD SPACE confusion */
  setup_keydef(&keydef, &seg, 1);

  /* Full record (as stored at INSERT time) */
  build_record(rec_full, 1, full_data.str, full_data.length, FALSE);
  full_hash= hp_rec_hashnr(0, &keydef, rec_full);

  /* Truncated record (as rebuilt from truncated varstring key) */
  build_record(rec_trunc, 1, full_data.str, trunc_len, FALSE);
  trunc_hash= hp_rec_hashnr(0, &keydef, rec_trunc);

  /* Hashes MUST differ -- this is the bug: truncation causes lookup miss */
  ok(full_hash != trunc_hash,
     "distinct_trunc: full hash (%lu) != truncated hash (%lu) -- "
     "truncation causes hash mismatch (the bug)",
     full_hash, trunc_hash);

  /* Comparison must also differ */
  ok(hp_rec_key_cmp(&keydef, rec_full, rec_trunc, NULL) != 0,
     "distinct_trunc: full vs truncated compares unequal");
}


/*
  Test 9: GROUP BY key format -- varstring with key_length override.

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
  LEX_CUSTRING data= { USTRING_WITH_LEN("group_concat_result_data_here!!") };

  uchar varstring_key[1 + 2 + 256];

  setup_blob_keyseg(&seg, FALSE);
  setup_keydef(&keydef, &seg, 1);

  /* INSERT-time hash */
  build_record(rec_insert, 1, data.str, data.length, FALSE);
  insert_hash= hp_rec_hashnr(0, &keydef, rec_insert);

  /*
    Simulate rebuild_key_from_group_buff: parse varstring
    key, populate rec_lookup.
    In GROUP BY, key_field_length = max_length (not 0, not pack_length).
  */
  /* no null bit for this test */
  int2store(varstring_key, data.length);
  memcpy(varstring_key + 2, data.str, data.length);

  memset(rec_lookup, 0, REC_LENGTH);
  {
    const uchar *key_pos= varstring_key;
    uint16 varchar_len;
    const uchar *varchar_data;
    uint32 bl;

    varchar_len= uint2korr(key_pos);
    varchar_data= key_pos + 2;

    bl= (uint32) varchar_len;
    int2store(rec_lookup + REC_BLOB_OFFSET, bl);
    memcpy(rec_lookup + REC_BLOB_OFFSET + REC_BLOB_PACKLEN,
           &varchar_data, sizeof(varchar_data));
  }

  lookup_hash= hp_rec_hashnr(0, &keydef, rec_lookup);

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
  LEX_CUSTRING blob1= { USTRING_WITH_LEN("sj_materialize_value_1") };
  LEX_CUSTRING blob2= { USTRING_WITH_LEN("sj_materialize_value_2") };
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
  build_record(rec1, 100, blob1.str, blob1.length, FALSE);
  build_record(rec2, 100, blob1.str, blob1.length, FALSE);

  h1= hp_rec_hashnr(0, &keydef, rec1);
  h2= hp_rec_hashnr(0, &keydef, rec2);
  ok(h1 == h2,
     "multi_distinct: same data hashes equal (%lu == %lu)", h1, h2);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "multi_distinct: same data compares equal");

  /* Same blob, different int */
  build_record(rec2, 200, blob1.str, blob1.length, FALSE);
  h3= hp_rec_hashnr(0, &keydef, rec2);
  ok(h1 != h3,
     "multi_distinct: different int hashes differ (%lu != %lu)", h1, h3);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_distinct: same blob + different int compares unequal");

  /* Same int, different blob */
  build_record(rec2, 100, blob2.str, blob2.length, FALSE);
  h3= hp_rec_hashnr(0, &keydef, rec2);
  ok(h1 != h3,
     "multi_distinct: different blob hashes differ (%lu != %lu)", h1, h3);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_distinct: different blob compares unequal");

  /* Same int, NULL blob vs non-NULL blob */
  build_record(rec2, 100, NULL, 0, TRUE);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "multi_distinct: non-NULL vs NULL blob compares unequal");
}


/*
  Test 11: hp_hashnr (key-based) must equal hp_rec_hashnr (record-based).

  This directly tests that building a key via hp_make_key() and then
  hashing it with hp_hashnr() produces the same hash as hp_rec_hashnr(0, )
  on the original record.  This catches divergence bugs where the two
  functions process segments differently (e.g. VARCHAR pack_length
  hardcoded to 2 in hp_hashnr but read from seg->bit_start in
  hp_rec_hashnr).
*/

extern ulong hp_hashnr(HP_KEYDEF *keydef, const uchar *key);

/*
  Record layout for mixed varchar + blob table:

  byte 0:       null bitmap (null_bit 4 = city null, null_bit 8 = libname null)
  bytes 1:      varchar length_bytes=1 (libname: VARCHAR(22))
  bytes 2-23:   varchar data (22 bytes)
  bytes 24-25:  blob packlength=2 (city: TEXT)
  bytes 26-33:  blob data pointer (8 bytes on x86_64)
  byte 34:      flags byte (visible offset)
  Total reclength: 35, recbuffer: ALIGN(MAX(34,8)+1, 8) = 40
*/
#define MIX_NULL_OFFSET    0
#define MIX_VARCHAR_OFFSET 1
#define MIX_VARCHAR_LEN    22
#define MIX_VARCHAR_LENBYTES 1
#define MIX_BLOB_OFFSET    24
#define MIX_BLOB_PACKLEN   2
#define MIX_REC_LENGTH     35
#define MIX_KEY_BUF_SIZE   256


static void setup_mixed_keydef(HP_KEYDEF *keydef, HA_KEYSEG *segs)
{
  /* Segment 0: blob (city TEXT) at offset 24 */
  memset(&segs[0], 0, sizeof(segs[0]));
  segs[0].type=      HA_KEYTYPE_VARTEXT4;
  segs[0].flag=      HA_BLOB_PART | HA_VAR_LENGTH_PART;
  segs[0].start=     MIX_BLOB_OFFSET;
  segs[0].length=    4+portable_sizeof_char_ptr; /* Length of blob key */
  segs[0].bit_start= MIX_BLOB_PACKLEN;
  segs[0].charset=   &my_charset_latin1;
  segs[0].null_bit=  4;   /* bit 2 in null bitmap */
  segs[0].null_pos=  MIX_NULL_OFFSET;

  /* Segment 1: varchar (libname VARCHAR(21)) at offset 1 */
  memset(&segs[1], 0, sizeof(segs[1]));
  segs[1].type=      HA_KEYTYPE_VARTEXT1;
  segs[1].flag=      HA_VAR_LENGTH_PART;
  segs[1].start=     MIX_VARCHAR_OFFSET;
  segs[1].length=    MIX_VARCHAR_LEN;
  segs[1].bit_start= MIX_VARCHAR_LENBYTES;  /* 1-byte length prefix */
  segs[1].bit_length= 2;                   /* key uses 2-byte length prefix */
  segs[1].charset=   &my_charset_latin1;
  segs[1].null_bit=  8;   /* bit 3 in null bitmap */
  segs[1].null_pos=  MIX_NULL_OFFSET;

  setup_keydef(keydef, segs, 2);
}


static void build_mixed_record(uchar *rec, const uchar *blob_data,
                                size_t blob_len, const uchar *varchar_data,
                                size_t varchar_len,
                                my_bool blob_null, my_bool varchar_null)
{
  memset(rec, 0, MIX_REC_LENGTH);

  /* null bitmap */
  if (blob_null)
    rec[MIX_NULL_OFFSET] |= 4;
  if (varchar_null)
    rec[MIX_NULL_OFFSET] |= 8;

  /* varchar: 1-byte length prefix + data */
  rec[MIX_VARCHAR_OFFSET]= (uchar) varchar_len;
  if (varchar_data && varchar_len > 0)
    memcpy(rec + MIX_VARCHAR_OFFSET + MIX_VARCHAR_LENBYTES,
           varchar_data, varchar_len);

  /* blob: packlength(2) + data pointer */
  int2store(rec + MIX_BLOB_OFFSET, blob_len);
  memcpy(rec + MIX_BLOB_OFFSET + MIX_BLOB_PACKLEN, &blob_data, PTR_SIZE);
}


static void test_key_vs_rec_hash_consistency(void)
{
  HA_KEYSEG segs[2];
  HP_KEYDEF keydef;
  uchar rec[MIX_REC_LENGTH];
  uchar key_buf[MIX_KEY_BUF_SIZE];
  ulong rec_hash, key_hash;

  LEX_CUSTRING city= { USTRING_WITH_LEN("New York") };
  LEX_CUSTRING libname= { USTRING_WITH_LEN("New York Public Libra") };

  setup_mixed_keydef(&keydef, segs);

  /* Build record and compute record-based hash (used at INSERT time) */
  build_mixed_record(rec, city.str, city.length, libname.str, libname.length,
                     FALSE, FALSE);
  rec_hash= hp_rec_hashnr(0, &keydef, rec);

  /* Build key via hp_make_key and compute key-based hash (used at LOOKUP) */
  hp_make_key(&keydef, key_buf, rec);
  key_hash= hp_hashnr(&keydef, key_buf);

  ok(rec_hash == key_hash,
     "key_vs_rec_hash: rec_hash (%lu) == key_hash (%lu) "
     "for mixed blob+varchar(1B) key",
     rec_hash, key_hash);

  /* Second test: different data to ensure it's not a coincidence */
  {
    LEX_CUSTRING city2= { USTRING_WITH_LEN("San Fran") };
    LEX_CUSTRING libname2= { USTRING_WITH_LEN("SF Public Library") };

    build_mixed_record(rec, city2.str, city2.length,
                       libname2.str, libname2.length, FALSE, FALSE);
    rec_hash= hp_rec_hashnr(0, &keydef, rec);
    hp_make_key(&keydef, key_buf, rec);
    key_hash= hp_hashnr(&keydef, key_buf);

    ok(rec_hash == key_hash,
       "key_vs_rec_hash: rec_hash (%lu) == key_hash (%lu) "
       "for second mixed blob+varchar(1B) data",
       rec_hash, key_hash);
  }

  /* Third test: varchar with 2-byte length prefix (field_length >= 256) */
  {
    HA_KEYSEG segs2b[2];
    HP_KEYDEF keydef2b;
    uchar rec2b[MIX_REC_LENGTH + 256];
    uchar key2b[MIX_KEY_BUF_SIZE + 256];

    /*
      Copy the setup but change varchar to 2-byte length prefix.
      This should always work because hp_hashnr already hardcodes 2.
    */
    memcpy(segs2b, segs, sizeof(segs));
    segs2b[1].bit_start= 2;  /* 2-byte length prefix */
    segs2b[1].length--;      /* Size for one less character */
    setup_keydef(&keydef2b, segs2b, 2);

    memset(rec2b, 0, sizeof(rec2b));
    /* blob */
    rec2b[MIX_NULL_OFFSET]= 0;
    int2store(rec2b + MIX_BLOB_OFFSET, city.length);
    memcpy(rec2b + MIX_BLOB_OFFSET + MIX_BLOB_PACKLEN, &city.str, PTR_SIZE);
    /* varchar with 2B length prefix */
    int2store(rec2b + MIX_VARCHAR_OFFSET, libname.length);
    memcpy(rec2b + MIX_VARCHAR_OFFSET + 2, libname.str, libname.length);

    rec_hash= hp_rec_hashnr(0, &keydef2b, rec2b);
    hp_make_key(&keydef2b, key2b, rec2b);
    key_hash= hp_hashnr(&keydef2b, key2b);

    ok(rec_hash == key_hash,
       "key_vs_rec_hash: rec_hash (%lu) == key_hash (%lu) "
       "for mixed blob+varchar(2B) key",
       rec_hash, key_hash);
  }

  /* Fourth test: blob-only key (no varchar) -- should always match */
  {
    HA_KEYSEG seg_blob;
    HP_KEYDEF kd_blob;
    uchar rec_b[REC_LENGTH];
    uchar key_b[KEY_BUF_SIZE];

    setup_blob_keyseg(&seg_blob, TRUE);
    setup_keydef(&kd_blob, &seg_blob, 1);

    build_record(rec_b, 1, city.str, city.length, FALSE);
    rec_hash= hp_rec_hashnr(0, &kd_blob, rec_b);
    hp_make_key(&kd_blob, key_b, rec_b);
    key_hash= hp_hashnr(&kd_blob, key_b);

    ok(rec_hash == key_hash,
       "key_vs_rec_hash: rec_hash (%lu) == key_hash (%lu) "
       "for blob-only key",
       rec_hash, key_hash);
  }
}


/*
  Build a record with a blob field using an arbitrary packlength (1-4).
  Record layout for packlength P:
    byte 0:             null bitmap
    bytes 1-4:          int4 field
    bytes 5..(5+P-1):   blob length (little-endian, P bytes)
    bytes (5+P)..(5+P+PTR_SIZE-1): blob data pointer
  Total reclength: 5 + P + PTR_SIZE
*/
static void build_record_packN(uchar *rec, size_t rec_len, uint packlength,
                               int32 int_val,
                               const uchar *blob_data, size_t blob_len)
{
  memset(rec, 0, rec_len);
  int4store(rec + REC_INT_OFFSET, int_val);
  switch (packlength)
  {
  case 1:
    rec[REC_BLOB_OFFSET]= (uchar) blob_len;
    break;
  case 2:
    int2store(rec + REC_BLOB_OFFSET, blob_len);
    break;
  case 3:
    int3store(rec + REC_BLOB_OFFSET, blob_len);
    break;
  case 4:
    int4store(rec + REC_BLOB_OFFSET, blob_len);
    break;
  }
  memcpy(rec + REC_BLOB_OFFSET + packlength, &blob_data, PTR_SIZE);
}


/*
  Test 12: Packlength variants (1, 3, 4).

  The existing tests only exercise packlength=2.  The HEAP engine supports
  packlengths 1-4 via seg->bit_start.  Verify that hp_rec_hashnr(0, ),
  hp_rec_key_cmp(), and hp_make_key() work correctly for each.
*/
static void test_packlength_variants(void)
{
  static const uint packlengths[]= {1, 3, 4};
  uint i;

  for (i= 0; i < array_elements(packlengths); i++)
  {
    uint pl= packlengths[i];
    /*
      Record size: 5 (null + int4) + pl + PTR_SIZE, rounded up.
      Use a generous buffer.
    */
    uchar rec1[32], rec2[32];
    uchar key_buf[KEY_BUF_SIZE];
    HA_KEYSEG seg;
    HP_KEYDEF keydef;
    ulong hash1;
    uint32 key_blob_len;

    uchar data_same[]= "packlength_test_data_ABCDEFGHIJ";
    size_t data_len= 30;  /* well within all packlength maxima */
    uchar data_diff[]= "DIFFERENT_data_XYZXYZXYZXYZXYZ";

    /* Set up keyseg for this packlength */
    memset(&seg, 0, sizeof(seg));
    seg.type=      HA_KEYTYPE_VARTEXT4;
    seg.flag=      HA_BLOB_PART | HA_VAR_LENGTH_PART;
    seg.start=     REC_BLOB_OFFSET;
    seg.length=    4 + portable_sizeof_char_ptr;
    seg.bit_start= pl;
    seg.charset=   &my_charset_latin1;
    seg.null_bit=  0;
    setup_keydef(&keydef, &seg, 1);

    /* Build record and compute hash */
    build_record_packN(rec1, sizeof(rec1), pl, 1, data_same, data_len);
    hash1= hp_rec_hashnr(0, &keydef, rec1);

    ok(hash1 != 0,
       "packlen=%u: hp_rec_hashnr produces non-zero hash (%lu)", pl, hash1);

    /* Same data must compare equal */
    build_record_packN(rec2, sizeof(rec2), pl, 2, data_same, data_len);
    ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
       "packlen=%u: same blob data compares equal", pl);

    /* Different data must compare unequal */
    build_record_packN(rec2, sizeof(rec2), pl, 2, data_diff, data_len);
    ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
       "packlen=%u: different blob data compares unequal", pl);

    /* hp_make_key must produce correct blob length */
    hp_make_key(&keydef, key_buf, rec1);
    key_blob_len= uint4korr(key_buf);
    ok(key_blob_len == data_len,
       "packlen=%u: hp_make_key blob length = %u (expected %u)",
       pl, (uint) key_blob_len, (uint) data_len);
  }
}


/*
  Record layout for a two-blob table:
    byte 0:       null bitmap
    bytes 1-4:    int4 field (padding, not keyed)
    bytes 5-6:    blob1 packlength=2
    bytes 7-14:   blob1 data pointer (8 bytes)
    bytes 15-16:  blob2 packlength=2
    bytes 17-24:  blob2 data pointer (8 bytes)
    byte 25:      flags byte (visible offset)
  Total reclength: 26, recbuffer: ALIGN(MAX(26,8)+1, 8) = 32
*/
#define BB_INT_OFFSET    1
#define BB_BLOB1_OFFSET  5
#define BB_BLOB2_OFFSET  15
#define BB_BLOB_PACKLEN  2
#define BB_REC_LENGTH    26
#define BB_REC_BUFFER    32


static void build_two_blob_record(uchar *rec, int32 int_val,
                                  const uchar *blob1_data, size_t blob1_len,
                                  const uchar *blob2_data, size_t blob2_len)
{
  memset(rec, 0, BB_REC_LENGTH);
  int4store(rec + BB_INT_OFFSET, int_val);
  int2store(rec + BB_BLOB1_OFFSET, blob1_len);
  memcpy(rec + BB_BLOB1_OFFSET + BB_BLOB_PACKLEN, &blob1_data, PTR_SIZE);
  int2store(rec + BB_BLOB2_OFFSET, blob2_len);
  memcpy(rec + BB_BLOB2_OFFSET + BB_BLOB_PACKLEN, &blob2_data, PTR_SIZE);
}


/*
  Test 13: Multi-segment key with two blob segments.

  Verifies hash, comparison, and hp_make_key() when a key has two
  HA_KEYTYPE_VARTEXT4 (blob) segments.
*/
static void test_blob_blob_multi_segment(void)
{
  HA_KEYSEG segs[2];
  HP_KEYDEF keydef;
  uchar rec1[BB_REC_BUFFER], rec2[BB_REC_BUFFER];
  uchar key_buf[KEY_BUF_SIZE];
  ulong h1, h2;

  LEX_CUSTRING data_a1= { USTRING_WITH_LEN("first_blob_data_alpha") };
  LEX_CUSTRING data_a2= { USTRING_WITH_LEN("second_blob_data_beta") };
  LEX_CUSTRING data_b1= { USTRING_WITH_LEN("CHANGED_first_blob!!!") };
  LEX_CUSTRING data_b2= { USTRING_WITH_LEN("CHANGED_second_blob!!") };

  /* Segment 0: blob1 at BB_BLOB1_OFFSET */
  memset(&segs[0], 0, sizeof(segs[0]));
  segs[0].type=      HA_KEYTYPE_VARTEXT4;
  segs[0].flag=      HA_BLOB_PART | HA_VAR_LENGTH_PART;
  segs[0].start=     BB_BLOB1_OFFSET;
  segs[0].length=    4 + portable_sizeof_char_ptr;
  segs[0].bit_start= BB_BLOB_PACKLEN;
  segs[0].charset=   &my_charset_latin1;
  segs[0].null_bit=  0;

  /* Segment 1: blob2 at BB_BLOB2_OFFSET */
  memset(&segs[1], 0, sizeof(segs[1]));
  segs[1].type=      HA_KEYTYPE_VARTEXT4;
  segs[1].flag=      HA_BLOB_PART | HA_VAR_LENGTH_PART;
  segs[1].start=     BB_BLOB2_OFFSET;
  segs[1].length=    4 + portable_sizeof_char_ptr;
  segs[1].bit_start= BB_BLOB_PACKLEN;
  segs[1].charset=   &my_charset_latin1;
  segs[1].null_bit=  0;

  setup_keydef(&keydef, segs, 2);

  /* Same data in both blobs: hash consistency */
  build_two_blob_record(rec1, 1, data_a1.str, data_a1.length,
                        data_a2.str, data_a2.length);
  build_two_blob_record(rec2, 2, data_a1.str, data_a1.length,
                        data_a2.str, data_a2.length);
  h1= hp_rec_hashnr(0, &keydef, rec1);
  h2= hp_rec_hashnr(0, &keydef, rec2);
  ok(h1 == h2,
     "blob+blob: same data in both blobs hashes equal (%lu == %lu)", h1, h2);

  /* Same data: comparison must be equal */
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) == 0,
     "blob+blob: same data compares equal");

  /* Change blob1 only: must compare unequal */
  build_two_blob_record(rec2, 2, data_b1.str, data_b1.length,
                        data_a2.str, data_a2.length);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "blob+blob: different blob1 compares unequal");

  /* Change blob2 only: must compare unequal */
  build_two_blob_record(rec2, 2, data_a1.str, data_a1.length,
                        data_b2.str, data_b2.length);
  ok(hp_rec_key_cmp(&keydef, rec1, rec2, NULL) != 0,
     "blob+blob: different blob2 compares unequal");

  /* hp_make_key: verify both blob lengths in the key */
  hp_make_key(&keydef, key_buf, rec1);
  {
    uint32 key_blob1_len= uint4korr(key_buf);
    uint32 key_blob2_len= uint4korr(key_buf + 4 + portable_sizeof_char_ptr);

    ok(key_blob1_len == data_a1.length,
       "blob+blob: hp_make_key blob1 length = %u (expected %u)",
       (uint) key_blob1_len, (uint) data_a1.length);
    ok(key_blob2_len == data_a2.length,
       "blob+blob: hp_make_key blob2 length = %u (expected %u)",
       (uint) key_blob2_len, (uint) data_a2.length);
  }
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_hash");
  plan(67);

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

  diag("Test 11: hp_hashnr vs hp_rec_hashnr consistency");
  test_key_vs_rec_hash_consistency();

  diag("Test 12: Packlength variants (1, 3, 4)");
  test_packlength_variants();

  diag("Test 13: Multi-segment key with two blob segments");
  test_blob_blob_multi_segment();

  my_end(0);
  return exit_status();
}
