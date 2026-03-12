/*
  Unit tests for HEAP blob key handling in heap_prepare_hp_create_info().

  Covers blob segment normalization (seg->length = 4+ptr, bit_start =
  packlength), non-blob key types (VARCHAR, INT, ENUM, mixed), promoted
  blob keys (varchar→blob), derived table ref access key setup, LONGBLOB
  keys with length=0, and geometry GROUP BY keys.
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>

#include "sql_priv.h"
#include "sql_class.h"  /* THD (full definition) */
#include "ha_heap.h"
#include "heapdef.h"

/*
  RAII guard for the fake THD used by unit tests.
  Allocates a zero-initialized THD (without calling the constructor),
  sets max_heap_table_size, installs it as current_thd, and tears it
  down on destruction.  This is technically UB (no C++ construction),
  but works because heap_prepare_hp_create_info only reads
  thd->variables.max_heap_table_size from the zeroed memory.
*/
class Fake_thd_guard
{
  char *m_buf;
public:
  Fake_thd_guard(ulonglong max_heap_size= 1024*1024)
  {
    m_buf= (char*) calloc(1, sizeof(THD));
    THD *thd= (THD*) m_buf;
    thd->variables.max_heap_table_size= max_heap_size;
    set_current_thd(thd);
  }
  ~Fake_thd_guard()
  {
    set_current_thd(NULL);
    free(m_buf);
  }
};

static const LEX_CSTRING test_field_name= {STRING_WITH_LEN("")};

extern int heap_prepare_hp_create_info(TABLE *table_arg,
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
  distinct_key_truncation: heap_prepare_hp_create_info must normalize
  blob key segments to the HEAP-native format (seg->length = 4+ptr,
  seg->bit_start = packlength, seg->flag has HA_BLOB_PART).

  The DISTINCT key path sets key_part.length = pack_length() = 10.
  Regardless of the SQL-layer key_part dimensions, the HEAP segment
  must be set to the fixed blob key format.
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

  KEY local_sql_key= {};
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

  Fake_thd_guard thd_guard;

  HP_CREATE_INFO hp_ci;
  memset(&hp_ci, 0, sizeof(hp_ci));
  hp_ci.max_table_size= 1024*1024;
  hp_ci.keys= 1;
  hp_ci.reclength= T_REC_LENGTH;

  int err= heap_prepare_hp_create_info(&test_table, TRUE, &hp_ci);

  ok(err == 0,
     "distinct_key_truncation: heap_prepare succeeded (err=%d)", err);

  HP_KEYDEF *kd= &hp_ci.keydef[0];
  ok(kd->seg[0].length == 4 + portable_sizeof_char_ptr,
     "distinct_key_truncation: seg->length = %u (expected %u = 4+ptr)",
     (uint) kd->seg[0].length, (uint)(4 + portable_sizeof_char_ptr));
  ok(kd->seg[0].bit_start == bf.length_size(),
     "distinct_key_truncation: seg->bit_start = %u (expected %u = packlength)",
     (uint) kd->seg[0].bit_start, (uint) bf.length_size());
  ok(kd->seg[0].flag & HA_BLOB_PART,
     "distinct_key_truncation: seg->flag (0x%x) has HA_BLOB_PART",
     (uint) kd->seg[0].flag);
  ok(kd->seg[0].type == HA_KEYTYPE_VARBINARY4 ||
     kd->seg[0].type == HA_KEYTYPE_VARTEXT4,
     "distinct_key_truncation: seg->type = %u (expected VARTEXT4/VARBINARY4)",
     (uint) kd->seg[0].type);

  my_free(hp_ci.keydef);
  my_free(hp_ci.blob_descs);
  bf.~Field_blob();
}


/*
  Test: heap_prepare_hp_create_info for various non-blob key types.

  Verifies that seg->flag does not contain HA_BLOB_PART for:
    - VARCHAR-only keys (Field_varstring, length_bytes=1)
    - Fixed-length keys (Field_long = INT)
    - ENUM keys (Field_enum)
    - Mixed VARCHAR + INT keys

  Also verifies seg->length, seg->type, seg->bit_start are correct.
*/

/* Helper: set up a single-field TABLE + KEY for heap_prepare testing */
struct Hp_test_single_key
{
  TABLE_SHARE share;
  TABLE test_table;
  KEY_PART_INFO kpi;
  KEY sql_key;
  Field *field_array[2];
  uchar rec_buf[64];
  uint blob_offsets[1];

  void init(Field *field, uint offset, uint rec_length)
  {
    memset(rec_buf, 0, sizeof(rec_buf));
    memset(static_cast<void*>(&share), 0, sizeof(share));
    share.fields= 1;
    share.keys= 1;
    share.reclength= rec_length;
    share.rec_buff_length= rec_length;
    share.db_record_offset= 1;
    share.blob_fields= 0;
    blob_offsets[0]= 0;
    share.blob_field= blob_offsets;

    field_array[0]= field;
    field_array[1]= NULL;

    memset(&kpi, 0, sizeof(kpi));
    kpi.field= field;
    kpi.offset= offset;
    kpi.length= (uint16) field->key_length();
    kpi.key_part_flag= field->key_part_flag();
    kpi.type= field->key_type();
    kpi.store_length= kpi.length;
    if (field->real_maybe_null())
      kpi.store_length+= HA_KEY_NULL_LENGTH;
    if (field->key_part_flag() & HA_VAR_LENGTH_PART)
      kpi.store_length+= field->key_part_length_bytes();

    sql_key= {};
    sql_key.user_defined_key_parts= 1;
    sql_key.usable_key_parts= 1;
    sql_key.key_part= &kpi;
    sql_key.algorithm= HA_KEY_ALG_HASH;
    sql_key.key_length= kpi.store_length;

    memset(static_cast<void*>(&test_table), 0, sizeof(test_table));
    test_table.record[0]= rec_buf;
    test_table.s= &share;
    test_table.field= field_array;
    test_table.key_info= &sql_key;
    share.key_info= &sql_key;

    field->table= &test_table;
  }

  int run_hp_create(HP_CREATE_INFO *hp_ci)
  {
    Fake_thd_guard thd_guard;

    memset(hp_ci, 0, sizeof(*hp_ci));
    hp_ci->max_table_size= 1024*1024;
    hp_ci->keys= 1;
    hp_ci->reclength= share.reclength;

    return heap_prepare_hp_create_info(&test_table, TRUE, hp_ci);
  }
};


static void test_varchar_only_key()
{
  /* VARCHAR(28) NOT NULL, length_bytes=1 */
  static const LEX_CSTRING fname= {STRING_WITH_LEN("v1")};
  TABLE_SHARE dummy_share;
  memset(static_cast<void*>(&dummy_share), 0, sizeof(dummy_share));
  alignas(Field_varstring) char vs_storage[sizeof(Field_varstring)];
  Field_varstring *vs= ::new (vs_storage) Field_varstring(
      (uchar*) NULL + 1, 28, 1, (uchar*) 0, 0,
      Field::NONE, &fname, &dummy_share,
      DTCollation(&my_charset_latin1));
  vs->field_index= 0;

  Hp_test_single_key ctx;
  ctx.init(vs, 1, 30);

  HP_CREATE_INFO hp_ci;
  int err= ctx.run_hp_create(&hp_ci);
  ok(err == 0, "varchar_only: heap_prepare succeeded (err=%d)", err);

  HA_KEYSEG *seg= hp_ci.keydef[0].seg;
  ok(seg->length == 28,
     "varchar_only: seg->length = %u (expected 28)", (uint) seg->length);
  ok(seg->type == HA_KEYTYPE_VARTEXT1,
     "varchar_only: seg->type = %d (expected VARTEXT1=%d)",
     (int) seg->type, (int) HA_KEYTYPE_VARTEXT1);
  /*
    bit_start for varchar is set by hp_create(), not
    heap_prepare_hp_create_info().  After prepare it's 0.
  */
  ok(seg->bit_start == 0,
     "varchar_only: seg->bit_start = %u (expected 0 -- set later by hp_create)",
     (uint) seg->bit_start);
  ok(!(seg->flag & HA_BLOB_PART),
     "varchar_only: seg->flag (0x%x) has NO HA_BLOB_PART",
     (uint) seg->flag);
  ok((seg->flag & HA_VAR_LENGTH_PART),
     "varchar_only: seg->flag (0x%x) has HA_VAR_LENGTH_PART",
     (uint) seg->flag);

  my_free(hp_ci.keydef);
  vs->~Field_varstring();
}


static void test_int_only_key()
{
  /* INT NOT NULL */
  static const LEX_CSTRING fname= {STRING_WITH_LEN("i1")};
  TABLE_SHARE dummy_share;
  memset(static_cast<void*>(&dummy_share), 0, sizeof(dummy_share));
  alignas(Field_long) char fl_storage[sizeof(Field_long)];
  Field_long *fl= ::new (fl_storage) Field_long(
      (uchar*) NULL + 1, 11, (uchar*) 0, 0,
      Field::NONE, &fname, false, false);
  fl->field_index= 0;

  Hp_test_single_key ctx;
  ctx.init(fl, 1, 5);

  HP_CREATE_INFO hp_ci;
  int err= ctx.run_hp_create(&hp_ci);
  ok(err == 0, "int_only: heap_prepare succeeded (err=%d)", err);

  HA_KEYSEG *seg= hp_ci.keydef[0].seg;
  ok(seg->length == 4,
     "int_only: seg->length = %u (expected 4)", (uint) seg->length);
  ok(seg->type == HA_KEYTYPE_BINARY,
     "int_only: seg->type = %d (expected BINARY=%d)",
     (int) seg->type, (int) HA_KEYTYPE_BINARY);
  ok(!(seg->flag & HA_BLOB_PART),
     "int_only: seg->flag (0x%x) has NO HA_BLOB_PART",
     (uint) seg->flag);
  ok(!(seg->flag & HA_VAR_LENGTH_PART),
     "int_only: seg->flag (0x%x) has NO HA_VAR_LENGTH_PART",
     (uint) seg->flag);

  my_free(hp_ci.keydef);
  fl->~Field_long();
}


static void test_enum_key()
{
  /* ENUM('a','','b') NULLABLE */
  static const LEX_CSTRING fname= {STRING_WITH_LEN("e1")};
  static const char *enum_names[]= { "a", "", "b", NULL };
  static unsigned int enum_lengths[]= { 1, 0, 1 };
  TYPELIB enum_typelib= { 3, "", enum_names, enum_lengths, NULL };
  Type_typelib_attributes enum_typelib_attr(enum_typelib);
  TABLE_SHARE dummy_share;
  memset(static_cast<void*>(&dummy_share), 0, sizeof(dummy_share));
  alignas(Field_enum) char fe_storage[sizeof(Field_enum)];
  /*
    Field_enum(ptr, len, null_ptr, null_bit, unireg, name,
               packlength, typelib, collation)
  */
  Field_enum *fe= ::new (fe_storage) Field_enum(
      (uchar*) NULL + 1, 1, (uchar*) NULL, 2,
      Field::NONE, &fname, 1, &enum_typelib_attr,
      &my_charset_latin1);
  fe->field_index= 0;

  Hp_test_single_key ctx;
  ctx.init(fe, 1, 3);

  HP_CREATE_INFO hp_ci;
  int err= ctx.run_hp_create(&hp_ci);
  ok(err == 0, "enum: heap_prepare succeeded (err=%d)", err);

  HA_KEYSEG *seg= hp_ci.keydef[0].seg;
  ok(seg->length == 1,
     "enum: seg->length = %u (expected 1 = packlength)", (uint) seg->length);
  ok(seg->type == HA_KEYTYPE_BINARY,
     "enum: seg->type = %d (expected BINARY=%d)",
     (int) seg->type, (int) HA_KEYTYPE_BINARY);
  ok(!(seg->flag & HA_BLOB_PART),
     "enum: seg->flag (0x%x) has NO HA_BLOB_PART", (uint) seg->flag);

  my_free(hp_ci.keydef);
  fe->~Field_enum();
}


static void test_mixed_int_varchar_key()
{
  /*
    Two-part key: INT(4 bytes) + VARCHAR(20), simulating the
    main.having GROUP BY (bigint, varchar(20)).
  */
  static const LEX_CSTRING fname_i= {STRING_WITH_LEN("id")};
  static const LEX_CSTRING fname_v= {STRING_WITH_LEN("description")};
  TABLE_SHARE dummy_share;
  memset(static_cast<void*>(&dummy_share), 0, sizeof(dummy_share));
  dummy_share.fields= 2;
  dummy_share.keys= 1;
  dummy_share.reclength= 26; /* 1 null + 4 int + 1 len + 20 varchar */
  dummy_share.rec_buff_length= 26;
  dummy_share.db_record_offset= 1;
  dummy_share.blob_fields= 0;
  uint blob_offsets[1]= { 0 };
  dummy_share.blob_field= blob_offsets;

  alignas(Field_long) char fl_storage[sizeof(Field_long)];
  Field_long *fl= ::new (fl_storage) Field_long(
      (uchar*) NULL + 1, 11, (uchar*) 0, 0,
      Field::NONE, &fname_i, false, false);
  fl->field_index= 0;

  alignas(Field_varstring) char vs_storage[sizeof(Field_varstring)];
  Field_varstring *vs= ::new (vs_storage) Field_varstring(
      (uchar*) NULL + 5, 20, 1, (uchar*) 0, 0,
      Field::NONE, &fname_v, &dummy_share,
      DTCollation(&my_charset_latin1));
  vs->field_index= 1;

  Field *field_array[3]= { fl, vs, NULL };

  KEY_PART_INFO kpis[2];
  memset(kpis, 0, sizeof(kpis));
  kpis[0].field= fl;
  kpis[0].offset= 1;
  kpis[0].length= 4;
  kpis[0].key_part_flag= fl->key_part_flag();
  kpis[0].type= fl->key_type();
  kpis[0].store_length= 4;

  kpis[1].field= vs;
  kpis[1].offset= 5;
  kpis[1].length= 20;
  kpis[1].key_part_flag= vs->key_part_flag();
  kpis[1].type= vs->key_type();
  kpis[1].store_length= 20 + 2; /* + key_part_length_bytes */

  KEY sql_key= {};
  sql_key.user_defined_key_parts= 2;
  sql_key.usable_key_parts= 2;
  sql_key.key_part= kpis;
  sql_key.algorithm= HA_KEY_ALG_HASH;
  sql_key.key_length= 4 + 20 + 2;

  TABLE test_table;
  uchar rec_buf[26];
  memset(rec_buf, 0, sizeof(rec_buf));
  memset(static_cast<void*>(&test_table), 0, sizeof(test_table));
  test_table.record[0]= rec_buf;
  test_table.s= &dummy_share;
  test_table.field= field_array;
  test_table.key_info= &sql_key;
  dummy_share.key_info= &sql_key;

  fl->table= &test_table;
  vs->table= &test_table;

  Fake_thd_guard thd_guard;

  HP_CREATE_INFO hp_ci;
  memset(&hp_ci, 0, sizeof(hp_ci));
  hp_ci.max_table_size= 1024*1024;
  hp_ci.keys= 1;
  hp_ci.reclength= 26;

  int err= heap_prepare_hp_create_info(&test_table, TRUE, &hp_ci);

  ok(err == 0, "int_varchar: heap_prepare succeeded (err=%d)", err);

  HP_KEYDEF *kd= &hp_ci.keydef[0];
  ok(kd->keysegs == 2,
     "int_varchar: keysegs = %u (expected 2)", kd->keysegs);
  {
    my_bool any_blob= FALSE;
    uint j;
    for (j= 0; j < kd->keysegs; j++)
      if (kd->seg[j].flag & HA_BLOB_PART)
        any_blob= TRUE;
    ok(!any_blob,
       "int_varchar: no keydef seg has HA_BLOB_PART");
  }

  HA_KEYSEG *seg0= &kd->seg[0];
  ok(seg0->length == 4,
     "int_varchar: seg[0].length = %u (expected 4)", (uint) seg0->length);
  ok(!(seg0->flag & HA_BLOB_PART),
     "int_varchar: seg[0] has NO HA_BLOB_PART");

  HA_KEYSEG *seg1= &kd->seg[1];
  ok(seg1->length == 20,
     "int_varchar: seg[1].length = %u (expected 20)", (uint) seg1->length);
  ok(!(seg1->flag & HA_BLOB_PART),
     "int_varchar: seg[1] has NO HA_BLOB_PART");
  ok((seg1->flag & HA_VAR_LENGTH_PART),
     "int_varchar: seg[1] has HA_VAR_LENGTH_PART");

  my_free(hp_ci.keydef);
  vs->~Field_varstring();
  fl->~Field_long();
}


/*
  Test: varchar→blob promotion in tmp table (main.having scenario).

  Simulates the case where:
    1. The SQL layer sets up KEY_PART_INFO with length=20 (varchar-sized)
    2. create_tmp_field promotes the field to Field_blob in the tmp table
    3. heap_prepare_hp_create_info is called with this mismatch

  The key_part has HA_BLOB_PART flag matching the promoted Field_blob.
  heap_prepare_hp_create_info must normalize the blob segment:
  seg->length to 4+ptr, seg->bit_start to packlength.
*/
static void test_varchar_promoted_to_blob()
{
  static const LEX_CSTRING fname_i= {STRING_WITH_LEN("id")};

  /*
    Record layout (mimics the tmp table after promotion):
      byte 0:     null bitmap
      bytes 1-8:  bigint (8 bytes)
      bytes 9-10: blob packlength=2
      bytes 11-18: blob data pointer
    reclength = 19
  */
  TABLE_SHARE share;
  memset(static_cast<void*>(&share), 0, sizeof(share));
  share.fields= 2;
  share.keys= 1;
  share.reclength= 19;
  share.rec_buff_length= 19;
  share.db_record_offset= 1;
  share.blob_fields= 0; /* Field_blob ctor increments */

  uchar rec_buf[24];
  memset(rec_buf, 0, sizeof(rec_buf));

  /* Field 0: bigint at offset 1 */
  alignas(Field_long) char fl_storage[sizeof(Field_long)];
  Field_long *fl= ::new (fl_storage) Field_long(
      rec_buf + 1, 11, (uchar*) 0, 0,
      Field::NONE, &fname_i, false, false);
  fl->field_index= 0;

  /* Field 1: Field_blob at offset 9 (promoted from varchar(20)) */
  alignas(Field_blob) char bf_storage[sizeof(Field_blob)];
  Field_blob *bf= make_test_field_blob(bf_storage,
                                       rec_buf + 9,
                                       (uchar*) 0, 0,
                                       &share, 2,
                                       &my_charset_latin1);
  bf->field_index= 1;

  Field *field_array[3]= { fl, bf, NULL };

  uint blob_offsets[1]= { 1 };
  share.blob_field= blob_offsets;

  /*
    KEY_PART_INFO: set up with correct HA_BLOB_PART flag matching Field_blob.
    key_part[1].length = 20 (varchar-like, non-zero).
    heap_prepare must normalize seg->length to 4+ptr for blob segments.
  */
  KEY_PART_INFO kpis[2];
  memset(kpis, 0, sizeof(kpis));

  kpis[0].field= fl;
  kpis[0].offset= 1;
  kpis[0].length= 8;
  kpis[0].key_part_flag= 0;
  kpis[0].type= fl->key_type();
  kpis[0].store_length= 8;

  kpis[1].field= bf;          /* promoted blob */
  kpis[1].offset= 9;
  kpis[1].length= 20;         /* varchar-like, non-zero */
  kpis[1].key_part_flag= HA_BLOB_PART;
  kpis[1].type= HA_KEYTYPE_VARTEXT2;
  kpis[1].store_length= 20 + 2;  /* varchar store_length */

  KEY sql_key= {};
  sql_key.user_defined_key_parts= 2;
  sql_key.usable_key_parts= 2;
  sql_key.key_part= kpis;
  sql_key.algorithm= HA_KEY_ALG_HASH;
  sql_key.key_length= 8 + 20 + 2;

  TABLE test_table;
  memset(static_cast<void*>(&test_table), 0, sizeof(test_table));
  test_table.record[0]= rec_buf;
  test_table.s= &share;
  test_table.field= field_array;
  test_table.key_info= &sql_key;
  share.key_info= &sql_key;

  fl->table= &test_table;
  bf->table= &test_table;

  Fake_thd_guard thd_guard;

  HP_CREATE_INFO hp_ci;
  memset(&hp_ci, 0, sizeof(hp_ci));
  hp_ci.max_table_size= 1024*1024;
  hp_ci.keys= 1;
  hp_ci.reclength= 19;

  int err= heap_prepare_hp_create_info(&test_table, TRUE, &hp_ci);

  ok(err == 0,
     "promoted_blob: heap_prepare succeeded (err=%d)", err);

  HP_KEYDEF *kd= &hp_ci.keydef[0];
  ok(kd->keysegs == 2,
     "promoted_blob: keysegs = %u (expected 2)", kd->keysegs);

  /* seg[0]: bigint — should be untouched */
  ok(kd->seg[0].length == 8,
     "promoted_blob: seg[0].length = %u (expected 8)", (uint) kd->seg[0].length);
  ok(!(kd->seg[0].flag & HA_BLOB_PART),
     "promoted_blob: seg[0] has NO HA_BLOB_PART");

  /*
    seg[1]: the promoted blob.
    heap_prepare_hp_create_info must:
      - preserve seg->flag with HA_BLOB_PART
      - set seg->length to 4+ptr (blob key format)
      - set seg->bit_start to length_size()
  */
  ok(kd->seg[1].flag & HA_BLOB_PART,
     "promoted_blob: seg[1].flag (0x%x) has HA_BLOB_PART",
     (uint) kd->seg[1].flag);
  ok(kd->seg[1].length == 4 + portable_sizeof_char_ptr,
     "promoted_blob: seg[1].length = %u (expected %u = 4+ptr)",
     (uint) kd->seg[1].length, (uint)(4 + portable_sizeof_char_ptr));
  ok(kd->seg[1].bit_start == bf->length_size(),
     "promoted_blob: seg[1].bit_start = %u (expected %u = packlength)",
     (uint) kd->seg[1].bit_start, (uint) bf->length_size());

  my_free(hp_ci.keydef);
  my_free(hp_ci.blob_descs);
  bf->~Field_blob();
  fl->~Field_long();
}


/*
  derived_with_keys_no_widening: when create_key_part_by_field() sets
  key_part->length = field_length for a promoted BLOB, heap_prepare
  must NOT modify key_part->length.

  The derived_with_keys path (ref access on materialized derived tables)
  sets key_part->length = field_length via create_key_part_by_field().
  heap_prepare normalizes the HEAP segment (seg->length = 4+ptr) but
  must leave key_part->length unchanged, since the SQL layer uses it
  for Field_varstring sizing in new_key_field().
*/
static void test_derived_blob_key_no_widening()
{
  static const LEX_CSTRING fname_id= {STRING_WITH_LEN("id")};

  /*
    Simulate a derived table with two fields:
      - id: INT (4 bytes) at offset 1
      - TABLE_SCHEMA: BLOB promoted from VARCHAR(64) utf8 at offset 5
        packlength=4, field_length=192 (64*3)
    Record layout:
      byte 0:    null bitmap
      bytes 1-4: INT
      bytes 5-8: blob packlength=4 (length, little-endian)
      bytes 9-16: blob data pointer (8 bytes)
    reclength = 17
  */
#define DWK_REC_LENGTH 17
#define DWK_INT_OFFSET 1
#define DWK_BLOB_OFFSET 5
#define DWK_BLOB_PACKLEN 4
#define DWK_BLOB_FIELD_LENGTH 192  /* VARCHAR(64) * 3 (utf8 mbmaxlen) */

  uchar rec_buf[DWK_REC_LENGTH + 8];
  memset(rec_buf, 0, sizeof(rec_buf));

  TABLE_SHARE share;
  memset(static_cast<void*>(&share), 0, sizeof(share));
  share.fields= 2;
  share.keys= 1;
  share.reclength= DWK_REC_LENGTH;
  share.rec_buff_length= DWK_REC_LENGTH;
  share.db_record_offset= 1;
  share.blob_fields= 0;

  /* Field 0: INT at offset 1 */
  alignas(Field_long) char fl_storage[sizeof(Field_long)];
  Field_long *fl= ::new (fl_storage) Field_long(
      rec_buf + DWK_INT_OFFSET, 11, (uchar*) 0, 0,
      Field::NONE, &fname_id, false, false);
  fl->field_index= 0;

  /* Field 1: BLOB promoted from VARCHAR(64) utf8 */
  alignas(Field_blob) char bf_storage[sizeof(Field_blob)];
  Field_blob *bf= make_test_field_blob(bf_storage,
                                       rec_buf + DWK_BLOB_OFFSET,
                                       (uchar*) 0, 0,
                                       &share, DWK_BLOB_PACKLEN,
                                       &my_charset_utf8mb3_general_ci);
  bf->field_index= 1;
  /* Simulate Phase 1 promotion: set field_length to original VARCHAR size */
  bf->field_length= DWK_BLOB_FIELD_LENGTH;

  Field *field_array[3]= { fl, bf, NULL };

  uint blob_offsets[1]= { 1 };
  share.blob_field= blob_offsets;

  /*
    KEY_PART_INFO: set up as create_key_part_by_field() does for BLOB.
    key_part->length = field_length (192), NOT pack_length.
    key_part->key_part_flag = HA_BLOB_PART (from Field_blob::key_part_flag()).
  */
  KEY_PART_INFO kpis[2];
  memset(kpis, 0, sizeof(kpis));

  kpis[0].field= fl;
  kpis[0].offset= DWK_INT_OFFSET;
  kpis[0].length= 4;
  kpis[0].key_part_flag= 0;
  kpis[0].type= fl->key_type();
  kpis[0].store_length= 4;

  kpis[1].field= bf;
  kpis[1].offset= DWK_BLOB_OFFSET;
  kpis[1].length= DWK_BLOB_FIELD_LENGTH;  /* = field_length, NOT pack_length */
  kpis[1].key_part_flag= bf->key_part_flag();  /* HA_BLOB_PART */
  kpis[1].type= bf->key_type();
  kpis[1].store_length= DWK_BLOB_FIELD_LENGTH + bf->key_part_length_bytes();

  KEY sql_key= {};
  sql_key.user_defined_key_parts= 2;
  sql_key.usable_key_parts= 2;
  sql_key.key_part= kpis;
  sql_key.algorithm= HA_KEY_ALG_HASH;
  sql_key.key_length= kpis[0].store_length + kpis[1].store_length;

  TABLE test_table;
  memset(static_cast<void*>(&test_table), 0, sizeof(test_table));
  test_table.record[0]= rec_buf;
  test_table.s= &share;
  test_table.field= field_array;
  test_table.key_info= &sql_key;
  share.key_info= &sql_key;

  fl->table= &test_table;
  bf->table= &test_table;

  Fake_thd_guard thd_guard;

  HP_CREATE_INFO hp_ci;
  memset(&hp_ci, 0, sizeof(hp_ci));
  hp_ci.max_table_size= 1024*1024;
  hp_ci.keys= 1;
  hp_ci.reclength= DWK_REC_LENGTH;

  int err= heap_prepare_hp_create_info(&test_table, TRUE, &hp_ci);

  ok(err == 0,
     "derived_no_widening: heap_prepare succeeded (err=%d)", err);

  HP_KEYDEF *kd= &hp_ci.keydef[0];

  /* seg[0]: INT — unchanged */
  ok(kd->seg[0].length == 4,
     "derived_no_widening: seg[0].length = %u (expected 4)",
     (uint) kd->seg[0].length);

  /* seg[1]: promoted blob — seg->length = 4+ptr (blob key format) */
  ok(kd->seg[1].flag & HA_BLOB_PART,
     "derived_no_widening: seg[1] has HA_BLOB_PART");
  ok(kd->seg[1].length == 4 + portable_sizeof_char_ptr,
     "derived_no_widening: seg[1].length = %u (expected %u = 4+ptr)",
     (uint) kd->seg[1].length, (uint)(4 + portable_sizeof_char_ptr));

  /* key_part->length must NOT be widened to max_data_length() */
  ok(kpis[1].length == DWK_BLOB_FIELD_LENGTH,
     "derived_no_widening: key_part.length = %u (expected %u, NOT %u)",
     (uint) kpis[1].length, DWK_BLOB_FIELD_LENGTH,
     (uint) bf->max_data_length());

  /* store_length and key_length should also be unchanged */
  uint expected_store= DWK_BLOB_FIELD_LENGTH + bf->key_part_length_bytes();
  ok(kpis[1].store_length == expected_store,
     "derived_no_widening: store_length = %u (expected %u)",
     (uint) kpis[1].store_length, expected_store);

  my_free(hp_ci.keydef);
  my_free(hp_ci.blob_descs);
  bf->~Field_blob();
  fl->~Field_long();

#undef DWK_REC_LENGTH
#undef DWK_INT_OFFSET
#undef DWK_BLOB_OFFSET
#undef DWK_BLOB_PACKLEN
#undef DWK_BLOB_FIELD_LENGTH
}


/*
  derived_blob_key_longblob: LONGBLOB (packlength=4, field_length=UINT32_MAX)
  with key_part->length = 0 and metadata-only store_length.

  This simulates the direct-to-record[0] path where add_tmp_key() leaves
  BLOB key parts with length=0 (from key_length()) and store_length has
  only HA_KEY_BLOB_LENGTH (2 bytes) + HA_KEY_NULL_LENGTH.  The key buffer
  has no space for BLOB data — heap_store_key_blob_ref writes directly
  into record[0]'s Field_blob.

  heap_prepare_hp_create_info must handle this correctly:
    - seg->flag = HA_BLOB_PART
    - seg->length = 4+ptr (blob key format)
    - key_part->length stays at 0 (not widened)
*/
static void test_derived_blob_key_longblob()
{
  static const LEX_CSTRING fname_id= {STRING_WITH_LEN("id")};

  /*
    Record layout:
      byte 0:     null bitmap
      bytes 1-4:  INT
      bytes 5-8:  blob packlength=4 (LONGBLOB)
      bytes 9-16: blob data pointer (8 bytes)
    reclength = 17
  */
#define LB_REC_LENGTH 17
#define LB_INT_OFFSET 1
#define LB_BLOB_OFFSET 5
#define LB_BLOB_PACKLEN 4

  uchar rec_buf[LB_REC_LENGTH + 8];
  memset(rec_buf, 0, sizeof(rec_buf));

  TABLE_SHARE share;
  memset(static_cast<void*>(&share), 0, sizeof(share));
  share.fields= 2;
  share.keys= 1;
  share.reclength= LB_REC_LENGTH;
  share.rec_buff_length= LB_REC_LENGTH;
  share.db_record_offset= 1;
  share.blob_fields= 0;

  /* Field 0: INT at offset 1 */
  alignas(Field_long) char fl_storage[sizeof(Field_long)];
  Field_long *fl= ::new (fl_storage) Field_long(
      rec_buf + LB_INT_OFFSET, 11, (uchar*) 0, 0,
      Field::NONE, &fname_id, false, false);
  fl->field_index= 0;

  /* Field 1: LONGBLOB at offset 5 (packlength=4) */
  alignas(Field_blob) char bf_storage[sizeof(Field_blob)];
  Field_blob *bf= make_test_field_blob(bf_storage,
                                       rec_buf + LB_BLOB_OFFSET,
                                       rec_buf, 2, /* nullable, null_bit=2 */
                                       &share, LB_BLOB_PACKLEN,
                                       &my_charset_latin1);
  bf->field_index= 1;

  Field *field_array[3]= { fl, bf, NULL };

  uint blob_offsets[1]= { 1 };
  share.blob_field= blob_offsets;

  /*
    KEY_PART_INFO: simulates what add_tmp_key() produces with the
    direct-to-record[0] fix — no BLOB length override.
    key_part->length = 0 (from Field_blob::key_length())
    store_length = 0 + HA_KEY_BLOB_LENGTH(2) + HA_KEY_NULL_LENGTH(1) = 3
  */
  KEY_PART_INFO kpis[2];
  memset(kpis, 0, sizeof(kpis));

  kpis[0].field= fl;
  kpis[0].offset= LB_INT_OFFSET;
  kpis[0].length= 4;
  kpis[0].key_part_flag= 0;
  kpis[0].type= fl->key_type();
  kpis[0].store_length= 4;

  kpis[1].field= bf;
  kpis[1].offset= LB_BLOB_OFFSET;
  kpis[1].length= 0;                          /* key_length() = 0 for blobs */
  kpis[1].null_bit= 2;
  kpis[1].null_offset= 0;
  kpis[1].key_part_flag= bf->key_part_flag();  /* HA_BLOB_PART */
  kpis[1].type= bf->key_type();
  /* store_length = HA_KEY_BLOB_LENGTH + HA_KEY_NULL_LENGTH = 3 */
  kpis[1].store_length= HA_KEY_BLOB_LENGTH + HA_KEY_NULL_LENGTH;

  KEY sql_key= {};
  sql_key.user_defined_key_parts= 2;
  sql_key.usable_key_parts= 2;
  sql_key.key_part= kpis;
  sql_key.algorithm= HA_KEY_ALG_HASH;
  sql_key.key_length= kpis[0].store_length + kpis[1].store_length;

  TABLE test_table;
  memset(static_cast<void*>(&test_table), 0, sizeof(test_table));
  test_table.record[0]= rec_buf;
  test_table.s= &share;
  test_table.field= field_array;
  test_table.key_info= &sql_key;
  share.key_info= &sql_key;

  fl->table= &test_table;
  bf->table= &test_table;

  Fake_thd_guard thd_guard;

  HP_CREATE_INFO hp_ci;
  memset(&hp_ci, 0, sizeof(hp_ci));
  hp_ci.max_table_size= 1024*1024;
  hp_ci.keys= 1;
  hp_ci.reclength= LB_REC_LENGTH;

  int err= heap_prepare_hp_create_info(&test_table, TRUE, &hp_ci);

  ok(err == 0,
     "longblob: heap_prepare succeeded (err=%d)", err);

  HP_KEYDEF *kd= &hp_ci.keydef[0];
  ok(kd->keysegs == 2,
     "longblob: keysegs = %u (expected 2)", kd->keysegs);
  /* seg[0]: INT — unchanged */
  ok(kd->seg[0].length == 4,
     "longblob: seg[0].length = %u (expected 4)",
     (uint) kd->seg[0].length);
  ok(!(kd->seg[0].flag & HA_BLOB_PART),
     "longblob: seg[0] has NO HA_BLOB_PART");

  /* seg[1]: LONGBLOB — must have HA_BLOB_PART, length = 4+ptr */
  ok(kd->seg[1].flag & HA_BLOB_PART,
     "longblob: seg[1].flag (0x%x) has HA_BLOB_PART",
     (uint) kd->seg[1].flag);
  ok(kd->seg[1].length == 4 + portable_sizeof_char_ptr,
     "longblob: seg[1].length = %u (expected %u = 4+ptr)",
     (uint) kd->seg[1].length, (uint)(4 + portable_sizeof_char_ptr));
  ok(kd->seg[1].bit_start == bf->length_size(),
     "longblob: seg[1].bit_start = %u (expected %u = packlength)",
     (uint) kd->seg[1].bit_start, (uint) bf->length_size());

  /* key_part->length must NOT be modified */
  ok(kpis[1].length == 0,
     "longblob: key_part.length = %u (expected 0, not widened)",
     (uint) kpis[1].length);

  /* store_length remains metadata-only */
  ok(kpis[1].store_length == HA_KEY_BLOB_LENGTH + HA_KEY_NULL_LENGTH,
     "longblob: store_length = %u (expected %u = metadata only)",
     (uint) kpis[1].store_length,
     (uint)(HA_KEY_BLOB_LENGTH + HA_KEY_NULL_LENGTH));

  my_free(hp_ci.keydef);
  my_free(hp_ci.blob_descs);
  bf->~Field_blob();
  fl->~Field_long();

#undef LB_REC_LENGTH
#undef LB_INT_OFFSET
#undef LB_BLOB_OFFSET
#undef LB_BLOB_PACKLEN
}


/*
  Test: geometry GROUP BY key must NOT trigger blob key widening.

  Field_geom::key_length() returns packlength (4), not 0 like Field_blob.
  heap_prepare_hp_create_info must normalize the blob segment
  (seg->length = 4+ptr) without corrupting key_part->store_length
  or key->key_length.

  This test simulates a GROUP BY on a GEOMETRY(POINT) column:
    - key_part->length = 4 (from Field_geom::key_length() = packlength)
    - key_part->store_length = small (from GROUP BY buffer sizing)
  After heap_prepare, key_part->length must still be 4, and
  store_length/key_length must not overflow.
*/
static void test_geometry_group_by_no_widening()
{
  /*
    Record layout: nullable geometry (POINT, packlength=4)
      byte 0:     null bitmap
      bytes 1-4:  blob packlength=4
      bytes 5-12: blob data pointer
    reclength = 13
  */
  uchar rec[16];
  memset(rec, 0, sizeof(rec));

  TABLE_SHARE share;
  memset(static_cast<void*>(&share), 0, sizeof(share));
  share.fields= 1;
  share.blob_fields= 0;
  share.keys= 1;
  share.reclength= 13;
  share.rec_buff_length= 13;
  share.db_record_offset= 1;

  /* GEOMETRY is a LONGBLOB (packlength=4) */
  alignas(Field_blob) char bf_storage[sizeof(Field_blob)];
  Field_blob *bfp= make_test_field_blob(bf_storage,
                                        rec + 1,
                                        rec + 0,
                                        2, &share,
                                        4 /* packlength for LONGBLOB */,
                                        &my_charset_bin);
  bfp->field_index= 0;

  Field *field_array[2]= { bfp, NULL };

  KEY_PART_INFO kpi;
  memset(&kpi, 0, sizeof(kpi));
  kpi.field= bfp;
  kpi.offset= 1;
  /*
    GROUP BY path: Field_geom::key_length() returns packlength = 4.
    finalize() sets m_key_part_info->length = field->key_length() = 4.
  */
  kpi.length= 4;
  kpi.key_part_flag= HA_BLOB_PART;
  kpi.null_bit= 2;
  kpi.null_offset= 0;
  kpi.type= bfp->key_type();
  /*
    GROUP BY store_length: set by finalize() from the group buffer
    Field_varstring.  Use a reasonable value (e.g. 100 + 2 + 1 = 103).
  */
  kpi.store_length= 103;

  KEY sql_key= {};
  sql_key.user_defined_key_parts= 1;
  sql_key.usable_key_parts= 1;
  sql_key.key_part= &kpi;
  sql_key.algorithm= HA_KEY_ALG_HASH;
  sql_key.key_length= kpi.store_length;

  TABLE test_table;
  memset(static_cast<void*>(&test_table), 0, sizeof(test_table));
  test_table.record[0]= rec;
  test_table.s= &share;
  test_table.field= field_array;
  test_table.key_info= &sql_key;
  share.key_info= &sql_key;
  bfp->table= &test_table;

  uint blob_offsets[1]= { 0 };
  share.blob_field= blob_offsets;

  /* Set group to simulate GROUP BY path */
  ORDER group_item= {};
  test_table.group= &group_item;

  Fake_thd_guard thd_guard;

  HP_CREATE_INFO hp_ci;
  memset(&hp_ci, 0, sizeof(hp_ci));
  hp_ci.max_table_size= 1024*1024;
  hp_ci.keys= 1;
  hp_ci.reclength= 13;

  uint16 orig_length= kpi.length;
  uint orig_store_length= kpi.store_length;
  uint orig_key_length= sql_key.key_length;

  int err= heap_prepare_hp_create_info(&test_table, TRUE, &hp_ci);
  ok(err == 0, "geom_group_by: heap_prepare succeeded (err=%d)", err);

  /* key_part->length must NOT be widened -- must stay at packlength (4) */
  ok(kpi.length == orig_length,
     "geom_group_by: key_part.length = %u (expected %u, NOT widened)",
     (uint) kpi.length, (uint) orig_length);

  /* store_length must not overflow */
  ok(kpi.store_length == orig_store_length,
     "geom_group_by: store_length = %u (expected %u, NOT overflowed)",
     (uint) kpi.store_length, (uint) orig_store_length);

  /* key_length must not overflow */
  ok(sql_key.key_length == orig_key_length,
     "geom_group_by: key_length = %u (expected %u, NOT overflowed)",
     (uint) sql_key.key_length, (uint) orig_key_length);

  /* seg->length = 4+ptr (blob key format) */
  ok(hp_ci.keydef[0].seg[0].length == 4 + portable_sizeof_char_ptr,
     "geom_group_by: seg->length = %u (expected %u = 4+ptr)",
     (uint) hp_ci.keydef[0].seg[0].length,
     (uint)(4 + portable_sizeof_char_ptr));


  my_free(hp_ci.keydef);
  my_free(hp_ci.blob_descs);
  bfp->~Field_blob();
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_key_setup");
  /* Field constructors reference system_charset_info via DTCollation */
  system_charset_info= &my_charset_latin1;
  plan(56);

  diag("distinct_key_truncation: blob segment normalization (seg->length, bit_start, flag, type)");
  test_distinct_key_truncation();

  diag("varchar_only: VARCHAR key has no blob flag");
  test_varchar_only_key();

  diag("int_only: INT key has no blob flag");
  test_int_only_key();

  diag("enum: ENUM key has no blob flag");
  test_enum_key();

  diag("int_varchar: mixed INT+VARCHAR key has no blob flag");
  test_mixed_int_varchar_key();

  diag("promoted_blob: varchar promoted to blob in tmp table");
  test_varchar_promoted_to_blob();

  diag("geom_group_by: geometry GROUP BY key must not trigger blob key widening");
  test_geometry_group_by_no_widening();

  diag("derived_blob_key_no_widening: promoted blob key not widened for derived");
  test_derived_blob_key_no_widening();

  diag("derived_blob_key_longblob: LONGBLOB key with length=0");
  test_derived_blob_key_longblob();

  my_end(0);
  return exit_status();
}
