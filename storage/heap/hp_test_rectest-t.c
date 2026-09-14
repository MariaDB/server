/* Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/*
  Unit tests for hp_rectest(), the check heap_update() and heap_delete()
  run while READ_CHECK_USED is set.  It answers one question: is the record
  the caller read still what the table holds?

  Every other consumer of the heap C API turns the flag off -- the server
  does it in handler::ha_open(), and the other unit tests do it in their
  setup -- so these tests are the only place the answer is observed.

  A column stored out of line makes the question harder than comparing the
  two buffers.  The record buffer holds a length and then either the value
  itself, for a VARCHAR the engine promoted, or a pointer to wherever the
  read handed the value out, for a blob.  The stored record holds the same
  length and then a pointer to the continuation chain.  So the buffers do
  not agree byte for byte even when nothing has changed, and the payload
  the question is about is not in the stored record at all.

  Record layout, one promoted VARCHAR and one blob:

    byte 0        null bitmap
    bytes 1-4     int4, the key
    bytes 5-6     promoted length prefix
    bytes 7-38    promoted payload, 32 declared bytes
    bytes 39-40   blob length prefix
    bytes 41-48   blob data pointer

  reclength is 49.  Stored, the 32 payload bytes give way to a chain
  pointer, so stored_reclength is 49 - 32 + sizeof(uchar*).
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <tap.h>
#include "heap.h"
#include "heapdef.h"

#define REC_LENGTH    49
#define INT_OFFSET    1
#define PROM_OFFSET   5
#define PROM_PACKLEN  2
#define PROM_LENGTH   32
#define BLOB_OFFSET   39
#define BLOB_PACKLEN  2
#define STORED_LENGTH (REC_LENGTH - PROM_LENGTH + (uint) sizeof(uchar*))

/* Long enough that the chain needs more than one continuation record */
#define LONG_BLOB_LEN  200
#define SHORT_BLOB_LEN 4


static void build_record(uchar *rec, int32 int_val,
                         const uchar *prom_data, uint16 prom_len,
                         const uchar *blob_data, uint16 blob_len)
{
  memset(rec, 0, REC_LENGTH);
  int4store(rec + INT_OFFSET, int_val);
  int2store(rec + PROM_OFFSET, prom_len);
  memcpy(rec + PROM_OFFSET + PROM_PACKLEN, prom_data, prom_len);
  int2store(rec + BLOB_OFFSET, blob_len);
  memcpy(rec + BLOB_OFFSET + BLOB_PACKLEN, &blob_data, sizeof(blob_data));
}


static void fill_pattern(uchar *buf, uint len, uchar seed)
{
  uint i;
  for (i= 0; i < len; i++)
    buf[i]= (uchar) (seed + (i % 251));
}


static int create_and_open_two(const char *name, HP_SHARE **share,
                               HP_INFO **info1, HP_INFO **info2)
{
  HP_KEYDEF keydef;
  HA_KEYSEG keyseg;
  HP_CREATE_INFO ci;
  HP_BLOB_DESC blob_descs[2];
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

  memset(blob_descs, 0, sizeof(blob_descs));
  blob_descs[0].offset=     PROM_OFFSET;
  blob_descs[0].packlength= PROM_PACKLEN;
  blob_descs[0].length=     PROM_LENGTH;
  blob_descs[0].promoted=   TRUE;
  blob_descs[1].offset=     BLOB_OFFSET;
  blob_descs[1].packlength= BLOB_PACKLEN;

  memset(&ci, 0, sizeof(ci));
  ci.keys=             1;
  ci.keydef=           &keydef;
  ci.reclength=        REC_LENGTH;
  ci.stored_reclength= STORED_LENGTH;
  ci.max_records=      1000;
  ci.min_records=      10;
  ci.max_table_size=   1024 * 1024;
  ci.blob_descs=       blob_descs;
  ci.blob_count=       2;

  if (heap_create(name, &ci, share, &unused))
    return 1;
  /*
    info1 is the handle whose read is being checked, and asks for the
    check rather than relying on the one hp_open() sets: that assignment
    is inside #ifndef DBUG_OFF, so a release build opens with the check
    off and hp_update() would accept every stale record.  info2 stands in
    for whoever changed the row underneath info1, and must not check
    anything itself.
  */
  if (!(*info1= heap_open(name, 2)))
    return 1;
  if (!(*info2= heap_open(name, 2)))
    return 1;
  heap_extra(*info1, HA_EXTRA_READCHECK);
  heap_extra(*info2, HA_EXTRA_NO_READCHECK);
  return 0;
}


/*
  Write one row, read it back through info1, and leave info1 positioned on
  it with old_rec holding what the read handed out.
*/

static int seed_row(HP_INFO *info1, uchar *old_rec,
                    const uchar *prom, uint16 prom_len,
                    const uchar *blob, uint16 blob_len)
{
  uchar rec[REC_LENGTH];
  uchar key[4];

  build_record(rec, 1, prom, prom_len, blob, blob_len);
  if (heap_write(info1, rec))
    return 1;
  int4store(key, 1);
  return heap_rkey(info1, old_rec, 0, key, 4, HA_READ_KEY_EXACT) != 0;
}


/* Position info2 on the row and store rec over it */

static int change_row(HP_INFO *info2, const uchar *rec)
{
  uchar cur[REC_LENGTH];
  uchar key[4];

  int4store(key, 1);
  if (heap_rkey(info2, cur, 0, key, 4, HA_READ_KEY_EXACT))
    return 1;
  return heap_update(info2, cur, rec) != 0;
}


typedef void (*mutate_fn)(uchar *rec, const uchar *prom_alt,
                          const uchar *blob_alt);


/*
  One scenario: seed a row, let info2 change it as mutate says, then have
  info1 update from the record it read before that change.  expect_changed
  is whether hp_rectest() is supposed to notice.
*/

static void run_case(const char *what, uint16 blob_len,
                     mutate_fn mutate, int expect_changed)
{
  HP_SHARE *share;
  HP_INFO *info1, *info2;
  uchar old_rec[REC_LENGTH], new_rec[REC_LENGTH], changed_rec[REC_LENGTH];
  uchar prom[PROM_LENGTH], prom_alt[PROM_LENGTH];
  uchar blob[LONG_BLOB_LEN], blob_alt[LONG_BLOB_LEN];
  char name[64];
  int rc;

  /*
    Run every scenario at both blob sizes.  Which of the three run layouts
    hp_read_blobs() hands out decides whether the record buffer's blob
    pointer happens to equal the stored chain pointer, and a scenario run
    at only one size cannot tell a right answer from that coincidence.
  */
  my_snprintf(name, sizeof(name), "test_rectest_%s_%s", what,
              blob_len > SHORT_BLOB_LEN ? "multirun" : "onerec");

  fill_pattern(prom, PROM_LENGTH, 1);
  fill_pattern(prom_alt, PROM_LENGTH, 100);
  fill_pattern(blob, LONG_BLOB_LEN, 7);
  fill_pattern(blob_alt, LONG_BLOB_LEN, 200);

  if (create_and_open_two(name, &share, &info1, &info2))
  {
    ok(0, "%s: setup failed: %d", name, my_errno);
    skip(1, "setup failed");
    return;
  }

  /*
    A table that promoted nothing would answer every question below the
    way a plain record does, and the tests would pass without covering
    anything.  Pin the layout that makes them mean something.
  */
  ok(share->promoted_count == 1 && share->stored_reclength == STORED_LENGTH,
     "%s: the table is laid out with one promoted column "
     "(promoted_count %u, stored_reclength %u)",
     name, share->promoted_count, share->stored_reclength);

  if (seed_row(info1, old_rec, prom, PROM_LENGTH, blob, blob_len))
  {
    ok(0, "%s: could not seed the row: %d", name, my_errno);
    goto done;
  }

  if (mutate)
  {
    memcpy(changed_rec, old_rec, REC_LENGTH);
    mutate(changed_rec, prom_alt, blob_alt);
    if (change_row(info2, changed_rec))
    {
      ok(0, "%s: the second handle could not change the row: %d",
         name, my_errno);
      goto done;
    }
  }

  /*
    What info1 goes on to write does not matter -- hp_rectest() reads only
    the record info1 claims to have read -- so write back what it read.
  */
  memcpy(new_rec, old_rec, REC_LENGTH);

  rc= heap_update(info1, old_rec, new_rec);
  if (expect_changed)
    ok(rc == HA_ERR_RECORD_CHANGED,
       "%s: the stale record is rejected (got %d)", name, rc);
  else
    ok(rc == 0, "%s: the unchanged record is accepted (got %d)", name, rc);

done:
  heap_close(info2);
  heap_close(info1);
  heap_delete_table(name);
}


static void mutate_prom_same_length(uchar *rec, const uchar *prom_alt,
                                    const uchar *blob_alt)
{
  (void) blob_alt;
  memcpy(rec + PROM_OFFSET + PROM_PACKLEN, prom_alt, PROM_LENGTH);
}


static void mutate_prom_length(uchar *rec, const uchar *prom_alt,
                               const uchar *blob_alt)
{
  (void) prom_alt;
  (void) blob_alt;
  int2store(rec + PROM_OFFSET, (uint16) (PROM_LENGTH - 1));
}


static void mutate_blob_same_length(uchar *rec, const uchar *prom_alt,
                                    const uchar *blob_alt)
{
  (void) prom_alt;
  memcpy(rec + BLOB_OFFSET + BLOB_PACKLEN, &blob_alt, sizeof(blob_alt));
}


static void mutate_int(uchar *rec, const uchar *prom_alt,
                       const uchar *blob_alt)
{
  (void) prom_alt;
  (void) blob_alt;
  int4store(rec + INT_OFFSET, (int32) 7);
}


int main(void)
{
  uint i;
  const uint16 blob_lens[2]= { SHORT_BLOB_LEN, LONG_BLOB_LEN };

  MY_INIT("hp_test_rectest-t");
  plan(20);

  for (i= 0; i < array_elements(blob_lens); i++)
  {
    uint16 blob_len= blob_lens[i];

    /*
      Nothing changed.  The record buffer and the stored record disagree
      at every column stored out of line by construction, so this is the
      case a plain comparison of the two gets wrong the other way round.
    */
    run_case("clean", blob_len, NULL, 0);

    /*
      A promoted column's payload is not in the stored record, so a change
      to it is invisible to anything that compares only what is.
    */
    run_case("prom_value", blob_len, mutate_prom_same_length, 1);
    run_case("prom_length", blob_len, mutate_prom_length, 1);

    /* A blob's payload is not in the stored record either */
    run_case("blob_value", blob_len, mutate_blob_same_length, 1);

    /* The inline case, which has always worked */
    run_case("inline", blob_len, mutate_int, 1);
  }

  my_end(0);
  return exit_status();
}
