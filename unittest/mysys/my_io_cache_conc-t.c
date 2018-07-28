/* Copyright (c) 2006, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#define MY_IO_CACHE_CONC
int  n_writers=2;
int  n_readers=20;
unsigned long long n_messages=2000;
int cache_read_with_care=1;

#include "thr_template.c"


#define FILL 0x5A
#define CACHE_SIZE 16384
//IO_CACHE info;
#define INFO_TAIL ", pos_in_file = %llu, pos_in_mem = %lu\n", \
                ptr_log->pos_in_file, (*ptr_log->current_pos - ptr_log->request_pos)

#define BUF_SIZE 2000
#define HDR_SIZE 8

my_off_t _end_pos;
uint last_written= 0;

IO_CACHE write_log;


void set_end_pos(my_off_t val)
{
     // mutext must be hold
     _end_pos= val;
     pthread_cond_broadcast(&cond2);
}


pthread_handler_t writer(void *arg) 
{
  uchar buf[BUF_SIZE];
  longlong param= *(ulonglong*) arg;
  IO_CACHE *ptr_log= &write_log;

  my_thread_init();

  memset(buf, FILL, sizeof(buf));

  diag("MDEV-14014 Dump thread reads past last 'officially' written byte");

  for (; param > 0; param--)
  {
       int res;
       // Generate a message of arb size that has at least 1 byte of payload
       uint32 size= rand() % (BUF_SIZE - HDR_SIZE - 1) + HDR_SIZE + 1;
       int4store(buf,     size );
       // Lock
       pthread_mutex_lock(&mutex);
       int4store(buf + 4, ++last_written);
       res= my_b_write(ptr_log, buf, size);
       //ok(res == 0, "buffer is written" INFO_TAIL);
       res= my_b_flush_io_cache(ptr_log, 1);
       set_end_pos(my_b_write_tell(ptr_log));
       pthread_mutex_unlock(&mutex);
       // Unlock
       //ok(res == 0, "flush" INFO_TAIL);
  }

  pthread_mutex_lock(&mutex);
  if (!--running_threads)
       pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);

  my_thread_end();
  return 0;
}

my_off_t get_end_pos()
{
     my_off_t ret;
     pthread_mutex_lock(&mutex);
     ret= _end_pos;
     pthread_mutex_unlock(&mutex);

     return ret;
}

my_off_t wait_new_events()
{
     my_off_t ret;

     pthread_mutex_lock(&mutex);
     pthread_cond_wait(&cond2, &mutex);
     ret= _end_pos;
     pthread_mutex_unlock(&mutex);

     return ret;
}

pthread_handler_t reader(void *arg) 
{
  int res;
  uchar buf[BUF_SIZE];
  File file= -1;
  const char *log_file_name="my.log";
  IO_CACHE read_log;
  IO_CACHE *ptr_log= &read_log;
  longlong n_messages= (*(longlong*) arg) * n_writers;
  my_off_t log_pos;
  //uint last_read= 0;

  my_thread_init();

  memset(buf,    0, sizeof( buf));

  diag("MDEV-14014 Dump thread reads past last 'officially' written byte");

  file= my_open(//key_file_binlog,
                log_file_name, O_CREAT | O_RDONLY | O_BINARY | O_SHARE,
                MYF(MY_WME));
  //ok(file >= 0, "mysql_file_open\n");
  res= init_io_cache(ptr_log, file, IO_SIZE*2, READ_CACHE, 0, 0,
                     MYF(MY_WME|MY_DONT_CHECK_FILESIZE));
  //ok(res == 0, "init_io_cache");

  log_pos= my_b_tell(ptr_log);
  for (; n_messages > 0;)
  {
       my_off_t end_pos= get_end_pos();
       size_t   size;
       
       if (log_pos >= end_pos)
            end_pos= wait_new_events();

       if (cache_read_with_care)
            ptr_log->end_of_file= end_pos;

       while (log_pos < end_pos)
       {
            // Read a message in two steps
            res= my_b_read(ptr_log, buf, HDR_SIZE);
            //ok(res == 0, "my_b_read HDR_SIZE");
            size= uint4korr(buf);
            ok(size >= HDR_SIZE && size <= BUF_SIZE, "msg size within HDR_SIZE, BUF_SIZE\n");
            //ok(uint4korr(buf+4) == ++last_read, "current msg number succeeds the last one\n");
            res= my_b_read(ptr_log, buf + HDR_SIZE, size - HDR_SIZE);
            //ok(res == 0, "my_b_read payload");
            ok(res == 0 && buf[HDR_SIZE] == FILL && buf[size - 1] == FILL, "my_b_read sane");

            n_messages--;
            //ok(n_messages >= 0, "param is not negative");
            log_pos= my_b_tell(ptr_log);
       }
  }
  //my_sleep(1000000);
  close_cached_file(ptr_log);

  pthread_mutex_lock(&mutex);
  if (!--running_threads)
       pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);

  my_thread_end();
  return 0;
}


void do_tests()
{
     const char *log_file_name="my.log";
     File file= my_open(//key_file_binlog,
          log_file_name, O_CREAT | O_RDWR | O_BINARY | O_SHARE,
          MYF(MY_WME));
     int res;
     IO_CACHE *ptr_log= &write_log;

     ok(file >= 0, "mysql_file_open\n");
     res= init_io_cache(ptr_log, file, IO_SIZE*2, WRITE_CACHE, 0, 0,
                        MYF(MY_WME|MY_DONT_CHECK_FILESIZE));
     ok(res == 0, "init_io_cache");

     test_concurrently2("my_io_cache_conc", writer, reader,
                        n_writers, n_readers, n_messages);
     //my_sync(ptr_log->file, MYF(MY_WME|MY_SYNC_FILESIZE));
     close_cached_file(ptr_log);

}
