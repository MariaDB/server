/*
  Shared helpers for HEAP blob unit tests.

  Record layout: (int4, blob(packlength=2))
    byte 0:       null bitmap (1 byte)
    bytes 1-4:    int4 field (4 bytes)
    bytes 5-6:    blob packlength=2 (length, little-endian)
    bytes 7-14:   blob data pointer (portable_sizeof_char_ptr = 8 bytes always)
  reclength = 15
  visible_offset = MAX(15, HP_DEL_METADATA_SIZE=11) = 15
  recbuffer = ALIGN(15 + 1, 8) = 16

  The pointer slot is always portable_sizeof_char_ptr (8) bytes in the record
  (same on 32-bit and 64-bit), but only sizeof(void*) bytes are meaningful.
  The rest is zero-padded by memset.
*/

#ifndef HP_TEST_HELPERS_H
#define HP_TEST_HELPERS_H

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>
#include "heap.h"
#include "heapdef.h"

#define REC_LENGTH    15
#define INT_OFFSET    1
#define BLOB_OFFSET   5
#define BLOB_PACKLEN  2


static void build_record(uchar *rec, int32 int_val,
                         const uchar *blob_data, uint16 blob_len)
{
  memset(rec, 0, REC_LENGTH);
  int4store(rec + INT_OFFSET, int_val);
  int2store(rec + BLOB_OFFSET, blob_len);
  memcpy(rec + BLOB_OFFSET + BLOB_PACKLEN, &blob_data, sizeof(blob_data));
}


static int create_and_open(const char *name, HP_SHARE **share, HP_INFO **info)
{
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  HP_CREATE_INFO ci;
  HP_BLOB_DESC blob_desc;
  my_bool unused;

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

#endif /* HP_TEST_HELPERS_H */
