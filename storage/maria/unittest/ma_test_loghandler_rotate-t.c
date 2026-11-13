/* Copyright (C) 2026 MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  Test of translog_rotate(): forced switch of the log to a new file
*/

#include "../maria_def.h"
#include <errno.h>
#include <tap.h>
#include "../trnman.h"

extern my_bool maria_log_remove(const char *testdir);
extern char *create_tmpdir(const char *progname);

#define PCACHE_SIZE (1024*1024*10)
#define PCACHE_PAGE TRANSLOG_PAGE_SIZE
#define LOG_FILE_SIZE (8*1024L*1024L)
#define LOG_FLAGS 0

#define WRITERS 4
#define ITERATIONS 1000
/*
  Records shorter than a page: records written over several pages hang
  with a concurrent translog_flush(), with or without rotation
*/
#define MAX_REC_LEN (TRANSLOG_PAGE_SIZE / 2)
/* the record type reads a 9 byte header */
#define MIN_REC_LEN 30

static uchar long_tr_id[6];
static uchar long_buffer[MAX_REC_LEN];

static LSN lsns_fixed[WRITERS][ITERATIONS];
static LSN lsns_var[WRITERS][ITERATIONS];
static uint lens[WRITERS][ITERATIONS];
static int writers_running;
static my_bool writer_failed;


static my_bool write_record(LSN *lsn, uint32 id)
{
  LEX_CUSTRING parts[TRANSLOG_INTERNAL_PARTS + 1];
  int4store(long_tr_id, id);
  parts[TRANSLOG_INTERNAL_PARTS + 0].str= long_tr_id;
  parts[TRANSLOG_INTERNAL_PARTS + 0].length= 6;
  return translog_write_record(lsn, LOGREC_FIXED_RECORD_0LSN_EXAMPLE,
                               &dummy_transaction_object, NULL, 6,
                               TRANSLOG_INTERNAL_PARTS + 1, parts,
                               NULL, NULL);
}


static my_bool init_log(PAGECACHE *pagecache, my_bool readonly)
{
  if (ma_control_file_open(TRUE, TRUE, TRUE, control_file_open_flags))
  {
    diag("Can't init control file (%d)", errno);
    return 1;
  }
  if (init_pagecache(pagecache, PCACHE_SIZE, 0, 0, PCACHE_PAGE, 0, 0) == 0)
  {
    diag("Got error: init_pagecache() (errno: %d)", errno);
    return 1;
  }
  if (translog_init_with_table(maria_data_root, LOG_FILE_SIZE, 50112, 0,
                               pagecache, LOG_FLAGS, readonly,
                               &translog_example_table_init, 0))
  {
    diag("Can't init loghandler (%d)", errno);
    return 1;
  }
  return 0;
}


static void end_log(PAGECACHE *pagecache)
{
  translog_destroy();
  end_pagecache(pagecache, 1);
  ma_control_file_end();
}


static void write_or_bail(PAGECACHE *pagecache, LSN *lsn, uint32 id)
{
  if (write_record(lsn, id))
  {
    end_log(pagecache);
    BAIL_OUT("Can't write record %u", (uint) id);
  }
}


/* Check that the record at lsn is the one written with the given id */

static my_bool check_record(TRANSLOG_HEADER_BUFFER *rec, LSN lsn, uint32 id)
{
  return (rec->lsn != lsn ||
          rec->type != LOGREC_FIXED_RECORD_0LSN_EXAMPLE ||
          rec->record_length != 6 ||
          uint4korr(rec->header) != id);
}


static void *writer_thread(void *arg)
{
  uint num= *(uint*) arg;
  TRN trn;
  uchar id[6];
  uint i;

  my_thread_init();
  bzero(&trn, sizeof(trn));
  trn.short_id= num;
  trn.first_undo_lsn= TRANSACTION_LOGGED_LONG_ID;
  for (i= 0; i < ITERATIONS && !writer_failed; i++)
  {
    LEX_CUSTRING parts[TRANSLOG_INTERNAL_PARTS + 1];
    uint len= MIN_REC_LEN +
      (num * 7919 + i * 104729) % (MAX_REC_LEN - MIN_REC_LEN);
    lens[num][i]= len;

    int2store(id, num);
    int4store(id + 2, i);
    parts[TRANSLOG_INTERNAL_PARTS + 0].str= id;
    parts[TRANSLOG_INTERNAL_PARTS + 0].length= 6;
    if (translog_write_record(&lsns_fixed[num][i],
                              LOGREC_FIXED_RECORD_0LSN_EXAMPLE,
                              &trn, NULL, 6, TRANSLOG_INTERNAL_PARTS + 1,
                              parts, NULL, NULL))
      writer_failed= 1;
    parts[TRANSLOG_INTERNAL_PARTS + 0].str= long_buffer;
    parts[TRANSLOG_INTERNAL_PARTS + 0].length= len;
    if (!writer_failed &&
        translog_write_record(&lsns_var[num][i],
                              LOGREC_VARIABLE_RECORD_0LSN_EXAMPLE,
                              &trn, NULL, len, TRANSLOG_INTERNAL_PARTS + 1,
                              parts, NULL, NULL))
      writer_failed= 1;
  }
  my_atomic_add32(&writers_running, -1);
  my_thread_end();
  return 0;
}


/*
  Check the records the writers wrote: they are readable with the right
  content, and each finished file has a header max LSN covering them.
*/

static my_bool check_writers_records(uchar *buffer, my_bool *bad_header)
{
  uint num, i, j;
  uint32 cur_file= LSN_FILE_NO(translog_get_horizon());
  *bad_header= 0;
  for (num= 0; num < WRITERS; num++)
  {
    for (i= 0; i < ITERATIONS; i++)
    {
      TRANSLOG_HEADER_BUFFER rec;
      int len;
      my_bool res;
      LSN lsn= lsns_var[num][i];

      bzero(&rec, sizeof(rec));
      len= translog_read_record_header(lsns_fixed[num][i], &rec);
      res= (len == RECHEADER_READ_ERROR ||
            rec.type != LOGREC_FIXED_RECORD_0LSN_EXAMPLE ||
            rec.short_trid != num ||
            uint2korr(rec.header) != num || uint4korr(rec.header + 2) != i);
      translog_free_record_header(&rec);
      if (!res)
      {
        bzero(&rec, sizeof(rec));
        len= translog_read_record_header(lsn, &rec);
        res= (len == RECHEADER_READ_ERROR ||
              rec.type != LOGREC_VARIABLE_RECORD_0LSN_EXAMPLE ||
              rec.record_length != lens[num][i] ||
              translog_read_record(lsn, 0, rec.record_length, buffer,
                                   NULL) != rec.record_length);
        for (j= 0; !res && j < lens[num][i]; j++)
          res= buffer[j] != (uchar) j;
        translog_free_record_header(&rec);
      }
      if (res)
      {
        diag("Record of writer %u iteration %u at " LSN_FMT " is wrong",
             num, i, LSN_IN_PARTS(lsn));
        return 1;
      }
      if (LSN_FILE_NO(lsn) < cur_file &&
          cmp_translog_addr(translog_get_file_max_lsn_stored(LSN_FILE_NO(lsn)),
                            lsn) < 0)
        *bad_header= 1;
    }
  }
  return 0;
}


int main(int argc __attribute__((unused)), char *argv[])
{
  PAGECACHE pagecache;
  LSN lsn1, lsn2, lsn3, max_lsn;
  char path[FN_REFLEN];
  MY_STAT stat_buff;
  TRANSLOG_HEADER_BUFFER rec;
  TRANSLOG_SCANNER_DATA scanner;
  pthread_t threads[WRITERS];
  uint thread_num[WRITERS];
  uint i, rotations, rotate_errors, files_before;
  my_bool bad_header;
  uchar *read_buffer;
  int len;
  my_bool res;

  MY_INIT(argv[0]);

  plan(16);

  bzero(&pagecache, sizeof(pagecache));
  maria_data_root= create_tmpdir(argv[0]);
  if (maria_log_remove(0))
    BAIL_OUT("Can't remove old logs");
  for (i= 0; i < MAX_REC_LEN; i++)
    long_buffer[i]= (uchar) i;

  if (init_log(&pagecache, 0))
    BAIL_OUT("Can't init log");
  /* Suppressing of automatic record writing */
  dummy_transaction_object.first_undo_lsn|= TRANSACTION_LOGGED_LONG_ID;

  /* Rotation of a log without records */
  ok(!translog_rotate() && LSN_FILE_NO(translog_get_horizon()) == 2 &&
     translog_is_file(1) && translog_is_file(2),
     "rotation of empty log creates file 2");

  /* LSN_IMPOSSIBLE in the header would mean "file in writing" for purge */
  max_lsn= translog_get_file_max_lsn_stored(1);
  diag("file 1 max lsn: " LSN_FMT, LSN_IN_PARTS(max_lsn));
  ok(max_lsn != LSN_IMPOSSIBLE && max_lsn != LSN_ERROR &&
     cmp_translog_addr(max_lsn, MAKE_LSN(2, 0)) < 0,
     "finished empty file has a max LSN in its header");

  write_or_bail(&pagecache, &lsn1, 1);

  ok(!translog_rotate() && LSN_FILE_NO(lsn1) == 2 &&
     LSN_FILE_NO(translog_get_horizon()) == 3 && translog_is_file(3),
     "rotation after a record creates file 3");

  ok(translog_get_file_max_lsn_stored(2) == lsn1,
     "max LSN of the finished file is stored in its header");

  /* The finished file must be written up to the end of the last page */
  res= (my_stat(translog_filename_by_fileno(2, path), &stat_buff, MYF(0)) &&
        stat_buff.st_size % TRANSLOG_PAGE_SIZE == 0 &&
        (my_off_t) stat_buff.st_size > LSN_OFFSET(lsn1));
  ok(res, "finished file is flushed to disk");

  ok(!translog_purge(lsn1) && translog_get_first_needed_file() == 2,
     "purge does not stop at the finished empty file");

  write_or_bail(&pagecache, &lsn2, 2);
  ok(LSN_FILE_NO(lsn2) == 3, "next record goes to the new file");

  end_log(&pagecache);

  /* Rotation after restart, before any record is written */
  if (init_log(&pagecache, 0))
    BAIL_OUT("Can't init log after restart");
  max_lsn= LSN_IMPOSSIBLE;
  res= (!translog_rotate() && LSN_FILE_NO(translog_get_horizon()) == 4);
  if (res)
    max_lsn= translog_get_file_max_lsn_stored(3);
  diag("file 3 max lsn: " LSN_FMT, LSN_IN_PARTS(max_lsn));
  ok(res && max_lsn != LSN_ERROR && cmp_translog_addr(max_lsn, lsn2) >= 0 &&
     LSN_FILE_NO(max_lsn) == 3,
     "rotation right after restart stores a max LSN covering the file");
  end_log(&pagecache);

  /* Records must be readable across the file boundaries after restart */
  if (init_log(&pagecache, 1))
    BAIL_OUT("Can't init read only log");
  res= 0;
  bzero(&rec, sizeof(rec));
  len=translog_read_record_header(lsn1, &rec);
  if (len == RECHEADER_READ_ERROR || check_record(&rec, lsn1, 1))
    res= 1;
  translog_free_record_header(&rec);
  if (!res && translog_scanner_init(lsn1, 1, &scanner, 0))
    res= 1;
  if (!res)
  {
    len= translog_read_next_record_header(&scanner, &rec);
    if (len == RECHEADER_READ_ERROR || len == RECHEADER_READ_EOF ||
        check_record(&rec, lsn2, 2))
      res= 1;
    translog_free_record_header(&rec);
    if (!res &&
        translog_read_next_record_header(&scanner, &rec) != RECHEADER_READ_EOF)
      res= 1;
    translog_destroy_scanner(&scanner);
  }
  ok(!res, "records before and after rotation are read after restart");
  end_log(&pagecache);

  /* Rotations while other threads write records */
  if (init_log(&pagecache, 0))
    BAIL_OUT("Can't init log for concurrent writers");
  files_before= LSN_FILE_NO(translog_get_horizon());
  writers_running= WRITERS;
  for (i= 0; i < WRITERS; i++)
  {
    thread_num[i]= i;
    if (pthread_create(&threads[i], NULL, writer_thread, thread_num + i))
      BAIL_OUT("Can't create writer thread (errno: %d)", errno);
  }
  rotations= rotate_errors= 0;
  while (my_atomic_load32(&writers_running) > 0)
  {
    if (translog_rotate())
      rotate_errors++;
    rotations++;
    my_sleep(200);
  }
  for (i= 0; i < WRITERS; i++)
    pthread_join(threads[i], NULL);
  diag("rotations: %u", rotations);
  ok(!writer_failed && !rotate_errors && rotations > 1 &&
     LSN_FILE_NO(translog_get_horizon()) >= files_before + rotations,
     "rotations while threads write records succeed");

  /*
    Read back from the files, as a backup copy of them would be read
    (reading of a live log with concurrent flushes is not tested here)
  */
  end_log(&pagecache);
  if (init_log(&pagecache, 1))
    BAIL_OUT("Can't init read only log");
  if (!(read_buffer= (uchar*) my_malloc(PSI_NOT_INSTRUMENTED, MAX_REC_LEN,
                                        MYF(MY_WME))))
    BAIL_OUT("Out of memory");
  res= writer_failed || check_writers_records(read_buffer, &bad_header);
  ok(!res, "records written during rotations are read back");
  ok(!res && !bad_header,
     "header max LSN of each finished file covers its records");
  my_free(read_buffer);
  end_log(&pagecache);

  /* Rotations of a new empty log, then a standard (not read only) start */
  if (maria_log_remove(0))
    BAIL_OUT("Can't remove old logs");
  if (init_log(&pagecache, 0))
    BAIL_OUT("Can't init new log");
  ok(!translog_rotate() && !translog_rotate() &&
     LSN_FILE_NO(translog_get_horizon()) == 3 &&
     translog_is_file(1) && translog_is_file(2) && translog_is_file(3),
     "two rotations of empty log create files 2 and 3");
  end_log(&pagecache);

  if (init_log(&pagecache, 0))
    BAIL_OUT("Can't start log of empty rotated files");
  res= (LSN_FILE_NO(translog_get_horizon()) == 3 &&
        translog_first_lsn_in_log() == LSN_IMPOSSIBLE);
  if (res && translog_scanner_init(translog_first_theoretical_lsn(), 1,
                                   &scanner, 0))
    res= 0;
  if (res)
  {
    res= (translog_read_next_record_header(&scanner, &rec) ==
          RECHEADER_READ_EOF);
    translog_free_record_header(&rec);
    translog_destroy_scanner(&scanner);
  }
  ok(res, "log of empty rotated files is started and read as empty");

  write_or_bail(&pagecache, &lsn3, 3);
  ok(LSN_FILE_NO(lsn3) == 3 && !translog_rotate(),
     "record is written after the start and the log is rotated");
  end_log(&pagecache);

  if (init_log(&pagecache, 0))
    BAIL_OUT("Can't start log after the record");
  res= (translog_first_lsn_in_log() == lsn3);
  bzero(&rec, sizeof(rec));
  len= translog_read_record_header(lsn3, &rec);
  if (len == RECHEADER_READ_ERROR || check_record(&rec, lsn3, 3))
    res= 0;
  translog_free_record_header(&rec);
  /* scan from the very beginning of the log over the empty files */
  if (res && translog_scanner_init(translog_first_theoretical_lsn(), 1,
                                   &scanner, 0))
    res= 0;
  if (res)
  {
    len= translog_read_next_record_header(&scanner, &rec);
    res= (len != RECHEADER_READ_ERROR && len != RECHEADER_READ_EOF &&
          !check_record(&rec, lsn3, 3));
    translog_free_record_header(&rec);
    if (res &&
        translog_read_next_record_header(&scanner, &rec) != RECHEADER_READ_EOF)
      res= 0;
    translog_free_record_header(&rec);
    translog_destroy_scanner(&scanner);
  }
  ok(res, "record after the empty files is the first one in the log");

  end_log(&pagecache);
  if (maria_log_remove(maria_data_root))
    exit(1);

  my_uuid_end();
  my_free_open_file_info();
  my_end(0);

  exit(0);
}

#include "../ma_check_standalone.h"
