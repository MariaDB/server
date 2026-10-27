/*
  Unit test: concurrent blob insert/delete on shared HEAP table.

  Two threads share one HP_SHARE via separate HP_INFO handles and
  do insert/delete cycles with blobs.  A pthread mutex serializes
  all operations on the shared table, and hp_flush_pending_blob_free()
  is called while the mutex is still held -- matching what
  ha_heap::external_lock(F_UNLCK) does under thr_lock.

  Without the external_lock flush fix (i.e. if the mutex is released
  before flushing), this test crashes with SIGSEGV from concurrent
  del_link corruption.  With the fix (flush under lock), it passes.
*/

#include "hp_test_helpers.h"

#define ITERATIONS 5000

static pthread_mutex_t table_mutex;
static volatile int go;

typedef struct
{
  HP_INFO *info;
  int base_id;
} thread_arg_t;


static void spin_wait(void)
{
  while (!go)
    ;
}


static void *thread_worker(void *arg)
{
  thread_arg_t *ta= (thread_arg_t *) arg;
  HP_INFO *info= ta->info;
  int base= ta->base_id;
  uchar rec[REC_LENGTH];
  uchar blob_data[500];
  int i;

  spin_wait();

  for (i= 0; i < ITERATIONS; i++)
  {
    uchar key[4];
    int32 id= base + (i % 500);

    memset(blob_data, 'A' + (i % 26), sizeof(blob_data));
    build_record(rec, id, blob_data, sizeof(blob_data));

    pthread_mutex_lock(&table_mutex);

    if (heap_write(info, rec) != 0)
    {
      int4store(key, id);
      if (heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0)
      {
        heap_delete(info, rec);
        hp_flush_pending_blob_free(info);
      }
      pthread_mutex_unlock(&table_mutex);
      continue;
    }

    int4store(key, id);
    if (heap_rkey(info, rec, 0, key, 4, HA_READ_KEY_EXACT) == 0)
    {
      heap_delete(info, rec);
      hp_flush_pending_blob_free(info);
    }

    pthread_mutex_unlock(&table_mutex);
  }
  return NULL;
}


int main(int argc __attribute__((unused)),
         char **argv __attribute__((unused)))
{
  HP_SHARE *share;
  HP_INFO *info1, *info2;
  pthread_t t1, t2;
  thread_arg_t arg1, arg2;

  plan(1);
  MY_INIT("hp_test_concurrent-t");
  pthread_mutex_init(&table_mutex, NULL);

  if (create_and_open("test_concurrent", &share, &info1))
  {
    ok(0, "setup failed");
    return exit_status();
  }

  info2= heap_open("test_concurrent", 2);
  if (!info2)
  {
    ok(0, "second open failed");
    heap_close(info1);
    return exit_status();
  }
  heap_extra(info2, HA_EXTRA_NO_READCHECK);

  go= 0;

  arg1.info= info1;
  arg1.base_id= 10000;
  arg2.info= info2;
  arg2.base_id= 20000;

  pthread_create(&t1, NULL, thread_worker, &arg1);
  pthread_create(&t2, NULL, thread_worker, &arg2);

  go= 1;

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  ok(1, "concurrent blob insert/delete completed without crash");

  heap_close(info2);
  heap_drop_table(info1);
  pthread_mutex_destroy(&table_mutex);
  my_end(0);
  return exit_status();
}
