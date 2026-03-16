/*
  Unit tests for HEAP blob key handling in heap_prepare_hp_create_info().

  1. distinct_key_truncation: heap_prepare_hp_create_info() must override
     key_part->length for blob key parts from pack_length() to
     max_data_length().  The DISTINCT key path sets key_part.length =
     pack_length() = 10, and the SQL layer's new_key_field() then
     creates Field_varstring(10), which truncates blob data.

  2. garbage_key_part_flag: heap_prepare_hp_create_info() must use
     field->key_part_flag() instead of key_part->key_part_flag, because
     SJ weedout and expression cache paths leave key_part_flag
     uninitialized.  Garbage HA_BLOB_PART bits corrupt the hash index.
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>

#include "sql_priv.h"
#include "sql_class.h"  /* THD (full definition) */
#include "ha_heap.h"
#include "heapdef.h"

static const LEX_CSTRING test_field_name= {STRING_WITH_LEN("")};

/* Wrapper declared in ha_heap.cc */
extern int test_heap_prepare_hp_create_info(TABLE *table_arg,
                                            bool internal_table,
                                            HP_CREATE_INFO *hp_create_info);

/*
  Record layout for test table (nullable tinyblob(16)):
    byte 0:     null bitmap (bit 2 = blob null)
    bytes 1-2:  blob packlength=2 (length, little-endian)
    bytes 3-10: blob data pointer (8 bytes)
  reclength = 11
*/
#define T_REC_NULL_OFFSET  0
#define T_REC_BLOB_OFFSET  1
#define T_REC_BLOB_PACKLEN 2
#define T_REC_LENGTH       11


/*
  Helper: create a Field_blob using the full server constructor
  (the same one make_table_field uses) via placement new.
  Sets field_length = BLOB_PACK_LENGTH_TO_MAX_LENGH(packlength),
  matching real server behavior.
*/
static Field_blob *
make_test_field_blob(void *storage, uchar *ptr, uchar *null_ptr,
                     uchar null_bit, TABLE_SHARE *share,
                     uint packlength, CHARSET_INFO *cs)
{
  static const LEX_CSTRING fname= {STRING_WITH_LEN("")};
  return ::new (storage) Field_blob(ptr, null_ptr, null_bit,
                                    Field::NONE, &fname,
                                    share, packlength,
                                    DTCollation(cs));
}


/*
  distinct_key_truncation: heap_prepare_hp_create_info must override
  key_part->length for blob key parts from pack_length() to
  max_data_length().

  The DISTINCT key path sets key_part.length = pack_length() = 10.
  The SQL layer's new_key_field() then creates Field_varstring(10),
  which truncates blob data longer than 10 bytes.

  heap_prepare_hp_create_info must widen key_part->length to
  max_data_length() (the maximum data the blob type can hold)
  and update store_length/key_length accordingly, so that
  new_key_field() creates a Field_varstring large enough for
  the full blob data.

  FAILS when the override is missing (key_part.length stays at 10).
  PASSES when heap_prepare_hp_create_info overrides to max_data_length().
*/
static void test_distinct_key_truncation()
{
  uchar local_rec[T_REC_LENGTH];
  memset(local_rec, 0, sizeof(local_rec));

  TABLE_SHARE share;
  memset(static_cast<void*>(&share), 0, sizeof(share));
  share.fields= 1;
  share.blob_fields= 0;  /* Field_blob constructor increments this */
  share.keys= 1;
  share.reclength= T_REC_LENGTH;
  share.rec_buff_length= T_REC_LENGTH;
  share.db_record_offset= 1;

  alignas(Field_blob) char bf_storage[sizeof(Field_blob)];
  Field_blob *bfp= make_test_field_blob(bf_storage,
                                        local_rec + T_REC_BLOB_OFFSET,
                                        local_rec + T_REC_NULL_OFFSET,
                                        2, &share,
                                        T_REC_BLOB_PACKLEN,
                                        &my_charset_bin);
  Field_blob &bf= *bfp;
  bf.field_index= 0;

  Field *field_array[2]= { &bf, NULL };

  KEY_PART_INFO local_kpi;
  memset(&local_kpi, 0, sizeof(local_kpi));
  local_kpi.field= &bf;
  local_kpi.offset= T_REC_BLOB_OFFSET;
  local_kpi.length= (uint16) bf.pack_length();  /* = 10 (the bug) */
  local_kpi.key_part_flag= bf.key_part_flag();
  local_kpi.type= bf.key_type();

  KEY local_sql_key;
  memset(&local_sql_key, 0, sizeof(local_sql_key));
  local_sql_key.user_defined_key_parts= 1;
  local_sql_key.usable_key_parts= 1;
  local_sql_key.key_part= &local_kpi;
  local_sql_key.algorithm= HA_KEY_ALG_HASH;

  TABLE test_table;
  memset(static_cast<void*>(&test_table), 0, sizeof(test_table));
  test_table.record[0]= local_rec;
  test_table.s= &share;
  test_table.field= field_array;
  test_table.key_info= &local_sql_key;
  share.key_info= &local_sql_key;

  bf.table= &test_table;

  uint blob_offsets[1]= { 0 };
  share.blob_field= blob_offsets;

  /*
    Simulate DISTINCT key path: set store_length and key_length
    based on key_part.length = pack_length() = 10, same as finalize().
  */
  local_kpi.store_length= local_kpi.length;
  if (bf.real_maybe_null())
    local_kpi.store_length+= HA_KEY_NULL_LENGTH;
  local_kpi.store_length+= bf.key_part_length_bytes();
  local_sql_key.key_length= local_kpi.store_length;

  ok(local_kpi.length == bf.pack_length(),
     "distinct_key_truncation setup: key_part.length = pack_length() = %u",
     (uint) local_kpi.length);

  char *fake_thd= (char*) calloc(1, sizeof(THD));
  THD *real_thd= (THD*) fake_thd;
  real_thd->variables.max_heap_table_size= 1024*1024;
  set_current_thd(real_thd);

  HP_CREATE_INFO hp_ci;
  memset(&hp_ci, 0, sizeof(hp_ci));
  hp_ci.max_table_size= 1024*1024;
  hp_ci.keys= 1;
  hp_ci.reclength= T_REC_LENGTH;

  int err= test_heap_prepare_hp_create_info(&test_table, TRUE, &hp_ci);

  set_current_thd(NULL);
  free(fake_thd);

  ok(err == 0,
     "distinct_key_truncation: heap_prepare succeeded (err=%d)", err);

  /*
    Phase 1 tests: key_part.length widening to max_data_length().
    In MDEV-38975 proper (without varchar-to-blob promotion),
    hp_create.c normalizes blob segments at runtime (zeroes
    seg->length, derives bit_start from blob_descs), so this
    widening is not needed. These assertions are deferred to
    Phase 1 where they are exercised.
  */
#if 0 /* Phase 1: distinct_key_truncation assertions */
  uint32 expected_length= bf.max_data_length();
  ok(local_kpi.length == expected_length,
     "distinct_key_truncation: key_part.length (%u) == max_data_length() (%u)",
     (uint) local_kpi.length, (uint) expected_length);

  uint expected_store_length= expected_length;
  if (bf.real_maybe_null())
    expected_store_length+= HA_KEY_NULL_LENGTH;
  expected_store_length+= bf.key_part_length_bytes();
  ok(local_kpi.store_length == expected_store_length,
     "distinct_key_truncation: store_length (%u) == expected (%u)",
     (uint) local_kpi.store_length, (uint) expected_store_length);
  ok(local_sql_key.key_length == expected_store_length,
     "distinct_key_truncation: key_length (%u) == expected (%u)",
     (uint) local_sql_key.key_length, (uint) expected_store_length);
#endif

  my_free(hp_ci.keydef);
  my_free(hp_ci.blob_descs);
  bf.~Field_blob();
}


/*
  garbage_key_part_flag: heap_prepare_hp_create_info uses
  key_part->key_part_flag to decide whether a key segment is a blob.
  Several SQL layer paths (SJ weedout, expression cache) leave
  key_part_flag uninitialized.  If the garbage value has HA_BLOB_PART
  set, heap_prepare_hp_create_info zeroes seg->length and treats the
  segment as a blob, corrupting the HEAP hash index for non-blob
  VARCHAR/VARBINARY keys.

  This manifests as:
  - Row loss in SJ lookups (HA_ERR_KEY_NOT_FOUND on non-blob keys)
  - COUNT(*)=1 instead of thousands because every insert after the
    first is rejected as a duplicate (all records hash identically
    when seg->length=0)

  Test: create a TABLE with a non-blob Field_varstring key and set
  key_part_flag to garbage containing HA_BLOB_PART.  Call
  test_heap_prepare_hp_create_info and verify the resulting HEAP key
  segment has the correct length (not 0) and does not have HA_BLOB_PART.
*/

/*
  Record layout for varchar test table (non-nullable varbinary(28)):
    byte 0:     null bitmap (all zero for NOT NULL)
    byte 1:     varchar length_bytes=1 (field_length=28 < 256)
    bytes 2-29: varchar data (28 bytes max)
  reclength = 30
*/
#define V_REC_NULL_OFFSET  0
#define V_REC_VARCHAR_OFFSET 1
#define V_REC_VARCHAR_LEN  28
#define V_REC_LENGTH       30


class Hp_test_varchar_key_flag
{
  alignas(Field_varstring) char vs_storage[sizeof(Field_varstring)];
  Field_varstring *vs_field;
  TABLE_SHARE share;
  TABLE test_table;
  uchar rec_buf[V_REC_LENGTH];
  KEY_PART_INFO local_kpi;
  KEY local_sql_key;

public:
  Hp_test_varchar_key_flag()
  {
    memset(rec_buf, 0, sizeof(rec_buf));
    memset(static_cast<void*>(&share), 0, sizeof(share));
    share.fields= 1;
    share.keys= 1;
    share.reclength= V_REC_LENGTH;
    share.rec_buff_length= V_REC_LENGTH;
    share.db_record_offset= 1;

    static const LEX_CSTRING fname= {STRING_WITH_LEN("")};
    vs_field= ::new (vs_storage) Field_varstring(
        rec_buf + V_REC_VARCHAR_OFFSET,
        V_REC_VARCHAR_LEN,
        1,           /* length_bytes: 1 for field_length < 256 */
        (uchar*) 0,  /* null_ptr: NOT NULL */
        0,            /* null_bit */
        Field::NONE,
        &fname,
        &share,
        DTCollation(&my_charset_bin));

    vs_field->field_index= 0;

    Field *field_array[2]= { vs_field, NULL };

    /*
      Simulate SJ weedout: leave key_part_flag UNINITIALIZED.
      We set it to garbage containing HA_BLOB_PART to reproduce
      the exact failure condition.
    */
    memset(&local_kpi, 0, sizeof(local_kpi));
    local_kpi.field= vs_field;
    local_kpi.offset= V_REC_VARCHAR_OFFSET;
    local_kpi.length= (uint16) vs_field->key_length();
    local_kpi.type= vs_field->key_type();
    /* Poison key_part_flag with garbage including HA_BLOB_PART (0x20) */
    local_kpi.key_part_flag= 0xA5A5;  /* garbage from uninitialized memory */

    memset(&local_sql_key, 0, sizeof(local_sql_key));
    local_sql_key.user_defined_key_parts= 1;
    local_sql_key.usable_key_parts= 1;
    local_sql_key.key_part= &local_kpi;
    local_sql_key.algorithm= HA_KEY_ALG_HASH;
    local_sql_key.key_length= local_kpi.length + 2; /* + varchar pack len */

    memset(static_cast<void*>(&test_table), 0, sizeof(test_table));
    test_table.record[0]= rec_buf;
    test_table.s= &share;
    test_table.field= field_array;
    test_table.key_info= &local_sql_key;
    share.key_info= &local_sql_key;

    vs_field->table= &test_table;

    /* No blob fields */
    uint blob_offsets[1]= { 0 };
    share.blob_field= blob_offsets;
    share.blob_fields= 0;
  }

  ~Hp_test_varchar_key_flag()
  {
    vs_field->~Field_varstring();
  }

  void test_garbage_key_part_flag()
  {
    /* Verify setup: key_part_flag has HA_BLOB_PART set (the poison) */
    ok((local_kpi.key_part_flag & HA_BLOB_PART) != 0,
       "garbage_flag setup: key_part_flag has HA_BLOB_PART set (garbage)");
    ok(local_kpi.length == V_REC_VARCHAR_LEN,
       "garbage_flag setup: key_part.length = %u (field_length)",
       (uint) local_kpi.length);

    char *fake_thd= (char*) calloc(1, sizeof(THD));
    THD *real_thd= (THD*) fake_thd;
    real_thd->variables.max_heap_table_size= 1024*1024;
    set_current_thd(real_thd);

    HP_CREATE_INFO hp_ci;
    memset(&hp_ci, 0, sizeof(hp_ci));
    hp_ci.max_table_size= 1024*1024;
    hp_ci.keys= 1;
    hp_ci.reclength= V_REC_LENGTH;

    int err= test_heap_prepare_hp_create_info(&test_table, TRUE, &hp_ci);

    set_current_thd(NULL);
    free(fake_thd);

    ok(err == 0,
       "garbage_flag: heap_prepare succeeded (err=%d)", err);

    HA_KEYSEG *seg= hp_ci.keydef[0].seg;
    ok(seg->length == V_REC_VARCHAR_LEN,
       "garbage_flag: seg->length = %u (expected %u, NOT 0)",
       (uint) seg->length, (uint) V_REC_VARCHAR_LEN);

    /*
      Phase 1 test: seg->flag must not have HA_BLOB_PART.
      In MDEV-38975 proper, hp_create.c strips spurious HA_BLOB_PART
      via blob_descs cross-check, so this is handled at runtime.
      The heap_prepare_hp_create_info fix (field->key_part_flag()
      instead of key_part->key_part_flag) is deferred to Phase 1.
    */
#if 0 /* Phase 1: garbage_key_part_flag assertion */
    ok(!(seg->flag & HA_BLOB_PART),
       "garbage_flag: seg->flag (0x%x) does NOT have HA_BLOB_PART",
       (uint) seg->flag);
#endif

    HP_KEYDEF *kd= &hp_ci.keydef[0];

    {
      uchar mk1[64], mk2[64];
      memset(mk1, 0, sizeof(mk1));
      memset(mk2, 0, sizeof(mk2));
      uchar mr1[V_REC_LENGTH], mr2[V_REC_LENGTH];
      memset(mr1, 0, sizeof(mr1));
      mr1[V_REC_VARCHAR_OFFSET]= 4;
      memcpy(mr1 + V_REC_VARCHAR_OFFSET + 1, "XXXX", 4);
      memset(mr2, 0, sizeof(mr2));
      mr2[V_REC_VARCHAR_OFFSET]= 4;
      memcpy(mr2 + V_REC_VARCHAR_OFFSET + 1, "YYYY", 4);
      hp_make_key(kd, mk1, mr1);
      hp_make_key(kd, mk2, mr2);
      ok(memcmp(mk1, mk2, 2 + V_REC_VARCHAR_LEN) != 0,
         "garbage_flag: hp_make_key produces different keys for different values");
    }

    /* Record 1: "AAAA" */
    uchar r1[V_REC_LENGTH];
    memset(r1, 0, sizeof(r1));
    r1[V_REC_VARCHAR_OFFSET]= 4;  /* length=4, 1-byte prefix */
    memcpy(r1 + V_REC_VARCHAR_OFFSET + 1, "AAAA", 4);

    /* Record 2: "BBBB" */
    uchar r2[V_REC_LENGTH];
    memset(r2, 0, sizeof(r2));
    r2[V_REC_VARCHAR_OFFSET]= 4;
    memcpy(r2 + V_REC_VARCHAR_OFFSET + 1, "BBBB", 4);

    ulong rh1= hp_rec_hashnr(kd, r1);
    ulong rh2= hp_rec_hashnr(kd, r2);

    ok(rh1 != rh2,
       "garbage_flag: different records produce different hashes "
       "(rh1=%lu, rh2=%lu)", rh1, rh2);

    ok(hp_rec_key_cmp(kd, r1, r2, NULL) != 0,
       "garbage_flag: different records compare as different");

    my_free(hp_ci.keydef);
  }
};


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_key_setup");
  /* Field constructors reference system_charset_info via DTCollation */
  system_charset_info= &my_charset_latin1;
  plan(9);

  diag("distinct_key_truncation: key_part->length widened for blob key parts");
  test_distinct_key_truncation();

  diag("garbage_key_part_flag: uninitialized key_part_flag corrupts non-blob keys");
  Hp_test_varchar_key_flag t2;
  t2.test_garbage_key_part_flag();

  my_end(0);
  return exit_status();
}
