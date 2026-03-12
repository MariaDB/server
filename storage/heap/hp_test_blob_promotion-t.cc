/*
  Unit tests for HEAP varchar→blob promotion (Phase 1).

  stale_record0_rebuild: rebuild_key_from_group_buff() must overwrite
  record[0] from the GROUP BY key buffer.  When the SQL layer promotes
  a wide VARCHAR to BLOB for HEAP temp tables, GROUP BY / materialization
  lookups pass a key buffer to index_read_map() but do NOT populate
  the temp table's record[0].  Stale data in record[0] after
  copy_funcs() causes hp_make_key() to build an incorrect key,
  leading to hash mismatches and missing/duplicate group rows.
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>

#include "sql_priv.h"
#include "sql_class.h"  /* THD (full definition) */
#include "ha_heap.h"
#include "heapdef.h"

/* Wrapper declared in ha_heap.cc under HEAP_UNIT_TESTS */
extern void test_rebuild_key_from_group_buff(ha_heap *handler, TABLE *tbl,
                                             HP_INFO *fake_file,
                                             HP_KEYDEF *keydef,
                                             const uchar *key, uint key_index,
                                             const uchar **rebuilt_key);

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


static void test_stale_record0_rebuild()
{
  alignas(ha_heap) char h_storage[sizeof(ha_heap)];
  ha_heap *h= reinterpret_cast<ha_heap*>(h_storage);
  memset(h_storage, 0, sizeof(h_storage));

  uchar lastkey_buf[64];
  HP_INFO hp_info;
  memset(&hp_info, 0, sizeof(hp_info));
  hp_info.lastkey= lastkey_buf;

  TABLE_SHARE tbl_share;
  memset(static_cast<void*>(&tbl_share), 0, sizeof(tbl_share));

  /* Set up blob keyseg */
  HA_KEYSEG seg;
  memset(&seg, 0, sizeof(seg));
  seg.type=      HA_KEYTYPE_VARTEXT1;
  seg.flag=      HA_BLOB_PART | HA_VAR_LENGTH_PART;
  seg.start=     T_REC_BLOB_OFFSET;
  seg.length=    0;
  seg.bit_start= T_REC_BLOB_PACKLEN;
  seg.charset=   &my_charset_bin;
  seg.null_bit=  2;
  seg.null_pos=  T_REC_NULL_OFFSET;

  HP_KEYDEF keydef;
  memset(&keydef, 0, sizeof(keydef));
  keydef.keysegs=    1;
  keydef.seg=        &seg;
  keydef.algorithm=  HA_KEY_ALG_HASH;
  keydef.length=     1 + 4 + (uint) sizeof(void*);
  keydef.has_blob_seg= 1;

  uchar rec_buf[T_REC_LENGTH];
  alignas(Field_blob) char blob_storage[sizeof(Field_blob)];
  Field_blob *blob_field= make_test_field_blob(blob_storage,
                                               rec_buf + T_REC_BLOB_OFFSET,
                                               rec_buf + T_REC_NULL_OFFSET,
                                               2, &tbl_share,
                                               T_REC_BLOB_PACKLEN,
                                               &my_charset_bin);

  KEY_PART_INFO kpi;
  memset(&kpi, 0, sizeof(kpi));
  kpi.field= blob_field;
  kpi.offset= T_REC_BLOB_OFFSET;
  kpi.length= (uint16) blob_field->pack_length();
  kpi.key_part_flag= blob_field->key_part_flag();
  kpi.null_bit= 2;  /* matches seg.null_bit — field is nullable */

  KEY sql_key;
  memset(&sql_key, 0, sizeof(sql_key));
  sql_key.user_defined_key_parts= 1;
  sql_key.usable_key_parts= 1;
  sql_key.key_part= &kpi;
  sql_key.algorithm= HA_KEY_ALG_HASH;

  TABLE tbl;
  memset(static_cast<void*>(&tbl), 0, sizeof(tbl));
  tbl.record[0]= rec_buf;
  tbl.key_info= &sql_key;
  tbl.s= &tbl_share;

  blob_field->table= &tbl;

  /* --- test data --- */
  const uchar *correct_data= (const uchar*) "1 - 00xxxxxxxxxx";
  const uint16 correct_len= 16;
  const uchar *stale_data= (const uchar*) "1 - 03xxxxxxxxxx";
  const uint16 stale_len= 16;

  /* Compute reference hashes */
  auto hash_record= [&](const uchar *data, uint16 len) -> ulong {
    uchar rec[T_REC_LENGTH];
    memset(rec, 0, sizeof(rec));
    int2store(rec + T_REC_BLOB_OFFSET, len);
    memcpy(rec + T_REC_BLOB_OFFSET + T_REC_BLOB_PACKLEN,
           &data, sizeof(void*));
    return hp_rec_hashnr(&keydef, rec);
  };

  ulong correct_hash= hash_record(correct_data, correct_len);
  ulong stale_hash= hash_record(stale_data, stale_len);

  ok(correct_hash != stale_hash,
     "stale_record0 setup: correct hash != stale hash");

  /* Pre-populate record[0] with STALE data */
  memset(rec_buf, 0, T_REC_LENGTH);
  int2store(rec_buf + T_REC_BLOB_OFFSET, stale_len);
  memcpy(rec_buf + T_REC_BLOB_OFFSET + T_REC_BLOB_PACKLEN,
         &stale_data, sizeof(void*));

  /* Build varstring key with CORRECT data: [null_flag][2B len][data] */
  uchar vkey[64];
  uchar *p= vkey;
  *p++= 0;                               /* not null */
  int2store(p, correct_len); p+= 2;
  memcpy(p, correct_data, correct_len); p+= correct_len;

  /*
    Set store_length to match the key buffer layout:
    null_flag(1) + varchar_len_prefix(2) + data(correct_len)
  */
  kpi.store_length= 1 + 2 + correct_len;

  /* Call rebuild_key_from_group_buff via test wrapper */
  const uchar *rebuilt;
  test_rebuild_key_from_group_buff(h, &tbl, &hp_info, &keydef,
                                   vkey, 0, &rebuilt);

  /* Hash record[0] after rebuild — must match correct, not stale */
  ulong rebuilt_hash= hp_rec_hashnr(&keydef, rec_buf);

  ok(rebuilt_hash == correct_hash,
     "stale_record0: rebuilt hash (%lu) == correct hash (%lu)",
     rebuilt_hash, correct_hash);
  ok(rebuilt_hash != stale_hash,
     "stale_record0: rebuilt hash (%lu) != stale hash (%lu)",
     rebuilt_hash, stale_hash);

  blob_field->~Field_blob();
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  MY_INIT("hp_test_blob_promotion");
  system_charset_info= &my_charset_latin1;
  plan(3);

  diag("stale_record0_rebuild: promoted blob GROUP BY key overwrites stale record[0]");
  test_stale_record0_rebuild();

  my_end(0);
  return exit_status();
}
