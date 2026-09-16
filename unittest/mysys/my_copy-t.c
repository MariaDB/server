/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* Test of my_copy(), my_copy_file() and my_copy_file_range() */

#include <my_global.h>
#include <my_sys.h>
#include <my_dir.h>
#include <m_string.h>
#include "tap.h"

/* Size of the file that the tests copy */
#define TEST_FILE_SIZE (3*1024*1024 + 4711)

/*
  Size of the file used to test the copying of a file that is big
  enough to be copied without filling the file cache.
  @see COPY_NO_BUFFERING_MIN_SIZE in mysys/my_copy.c
*/
#define BIG_TEST_FILE_SIZE (64*1024*1024 + 4711)

static char source[FN_REFLEN], target[FN_REFLEN];


/*
  Fill a buffer with data that depends on the offset, so that we can
  detect data that was copied to the wrong place.
*/

static void fill_buffer(uchar *buff, size_t length, my_off_t offset)
{
  uchar *pos, *end;
  for (pos= buff, end= buff + length; pos < end; pos++)
    *pos= (uchar) ((offset++ * 7) & 0xff);
}


/*
  Create the file that the tests copy.

  @return 0 on success
*/

static int create_source_file(void)
{
  uchar buff[IO_SIZE];
  my_off_t offset= 0;
  File file= my_create(source, 0666, O_WRONLY, MYF(MY_WME));

  if (file < 0)
    return 1;
  while (offset < TEST_FILE_SIZE)
  {
    size_t length= (size_t) MY_MIN(sizeof(buff), TEST_FILE_SIZE - offset);
    fill_buffer(buff, length, offset);
    if (my_write(file, buff, length, MYF(MY_WME | MY_NABP)))
    {
      my_close(file, MYF(0));
      return 1;
    }
    offset+= length;
  }
  return my_close(file, MYF(MY_WME));
}


/*
  Check that a range of the target file has the same content as the
  source file.

  @return 0 if the content is correct
*/

static int compare_range(my_off_t start, my_off_t end)
{
  uchar buff[IO_SIZE], expected[IO_SIZE];
  my_off_t offset= start;
  File file= my_open(target, O_RDONLY, MYF(MY_WME));

  if (file < 0)
    return 1;
  while (offset < end)
  {
    size_t length= (size_t) MY_MIN(sizeof(buff), end - offset);
    if (my_pread(file, buff, length, offset, MYF(MY_WME | MY_NABP)))
    {
      my_close(file, MYF(0));
      return 1;
    }
    fill_buffer(expected, length, offset);
    if (memcmp(buff, expected, length))
    {
      my_close(file, MYF(0));
      diag("Wrong data at offset %llu", (ulonglong) offset);
      return 1;
    }
    offset+= length;
  }
  return my_close(file, MYF(MY_WME)) != 0;
}


/*
  Check that the target file has the expected size.
*/

static int check_size(my_off_t expected)
{
  MY_STAT stat_area;
  if (!my_stat(target, &stat_area, MYF(MY_WME)))
    return 1;
  if ((my_off_t) stat_area.st_size == expected)
    return 0;
  diag("Size is %llu, expected %llu",
       (ulonglong) stat_area.st_size, (ulonglong) expected);
  return 1;
}


/*
  Test my_copy().
*/

static void test_my_copy(void)
{
  ok(!my_copy(source, target, MYF(MY_WME)), "my_copy");
  ok(!check_size(TEST_FILE_SIZE), "size of the copy");
  ok(!compare_range(0, TEST_FILE_SIZE), "content of the copy");

  ok(my_copy(source, target, MYF(MY_DONT_OVERWRITE_FILE)) != 0,
     "my_copy does not overwrite with MY_DONT_OVERWRITE_FILE");
  my_delete(target, MYF(0));
}


/*
  Test my_copy_file() on files that are already open.
*/

static void test_my_copy_file(void)
{
  File from= my_open(source, O_RDONLY, MYF(MY_WME));
  File to= my_create(target, 0666, O_WRONLY, MYF(MY_WME));
  int error= 1;

  if (from >= 0 && to >= 0)
    error= my_copy_file(from, to, MYF(MY_WME));
  if (from >= 0)
    my_close(from, MYF(0));
  if (to >= 0)
    my_close(to, MYF(0));

  ok(!error, "my_copy_file");
  ok(!check_size(TEST_FILE_SIZE), "size after my_copy_file");
  ok(!compare_range(0, TEST_FILE_SIZE), "content after my_copy_file");
  my_delete(target, MYF(0));
}


/*
  Test my_copy_file_range(). The ranges are copied in a different
  order than they are stored in the file, which the offsets have to
  take care of.
*/

static void test_my_copy_file_range(void)
{
  File from= my_open(source, O_RDONLY, MYF(MY_WME));
  File to= my_create(target, 0666, O_RDWR, MYF(MY_WME));
  int error= 1;

  if (from >= 0 && to >= 0)
    error= my_copy_file_range(from, to, 1024*1024, TEST_FILE_SIZE,
                              MYF(MY_WME)) ||
      my_copy_file_range(from, to, 0, 1024*1024, MYF(MY_WME)) ||
      my_copy_file_range(from, to, 4711, 4711, MYF(MY_WME));
  if (from >= 0)
    my_close(from, MYF(0));
  if (to >= 0)
    my_close(to, MYF(0));

  ok(!error, "my_copy_file_range");
  ok(!check_size(TEST_FILE_SIZE), "size after my_copy_file_range");
  ok(!compare_range(0, TEST_FILE_SIZE), "content after my_copy_file_range");
  my_delete(target, MYF(0));
}


/*
  Test the flags that my_copy() handles after the copy itself.
*/

static void test_my_copy_flags(void)
{
  MY_STAT stat_buff, source_stat;
  int mode_kept;

  /* A source that does not exist has to fail and create nothing */
  ok(my_copy("no_such_source_file", target, MYF(0)) != 0,
     "my_copy of a file that does not exist fails");
  ok(!my_stat(target, &stat_buff, MYF(0)), "no target file was created");

  /* MY_SYNC syncs the copy to disk */
  ok(!my_copy(source, target, MYF(MY_WME | MY_SYNC)), "my_copy with MY_SYNC");
  ok(!check_size(TEST_FILE_SIZE), "size after MY_SYNC");

  /*
    MY_HOLD_ORIGINAL_MODES keeps the modes of an existing target.
    The target was created above; give it modes of its own.
  */
  if (chmod(target, 0600))
  {
    skip(4, "chmod failed");
    my_delete(target, MYF(0));
    return;
  }
  ok(!my_copy(source, target, MYF(MY_WME | MY_HOLD_ORIGINAL_MODES)),
     "my_copy with MY_HOLD_ORIGINAL_MODES");
  mode_kept= (my_stat(target, &stat_buff, MYF(MY_WME)) &&
              (stat_buff.st_mode & 0777) == 0600);
  ok(mode_kept, "the modes of the target were kept");

  /* MY_COPYTIME copies the modify time of the source */
  my_delete(target, MYF(0));
  ok(!my_copy(source, target, MYF(MY_WME | MY_COPYTIME)),
     "my_copy with MY_COPYTIME");
  ok(my_stat(source, &source_stat, MYF(MY_WME)) &&
     my_stat(target, &stat_buff, MYF(MY_WME)) &&
     stat_buff.st_mtime == source_stat.st_mtime,
     "the modify time was copied");
  my_delete(target, MYF(0));
}


/*
  Test the copying of a file that is big enough to be copied without
  filling the file cache.
*/

static void test_big_file(void)
{
  MY_STAT stat_buff;
  uchar buff[IO_SIZE];
  my_off_t offset= 0;
  File file= my_create(source, 0666, O_WRONLY, MYF(MY_WME));

  if (file < 0)
  {
    skip(2, "could not create the file");
    return;
  }
  bzero(buff, sizeof(buff));
  while (offset < BIG_TEST_FILE_SIZE)
  {
    size_t length= (size_t) MY_MIN(sizeof(buff),
                                   BIG_TEST_FILE_SIZE - offset);
    if (my_write(file, buff, length, MYF(MY_WME | MY_NABP)))
    {
      my_close(file, MYF(0));
      skip(2, "could not write the file");
      return;
    }
    offset+= length;
  }
  my_close(file, MYF(MY_WME));

  ok(!my_copy(source, target, MYF(MY_WME)), "my_copy of a big file");
  ok(my_stat(target, &stat_buff, MYF(MY_WME)) &&
     (my_off_t) stat_buff.st_size == BIG_TEST_FILE_SIZE,
     "size of the big copy");
  my_delete(target, MYF(0));
}


int main(int argc __attribute__((unused)), char *argv[])
{
  MY_INIT(argv[0]);
  plan(20);

  snprintf(source, sizeof(source), "my_copy_test_%d.src", (int) getpid());
  snprintf(target, sizeof(target), "my_copy_test_%d.dst", (int) getpid());

  if (create_source_file())
  {
    diag("Could not create the test file %s", source);
    my_delete(source, MYF(0));
    my_end(0);
    return exit_status();
  }

  test_my_copy();
  test_my_copy_file();
  test_my_copy_file_range();
  test_my_copy_flags();
  my_delete(source, MYF(0));
  test_big_file();

  my_delete(source, MYF(0));
  my_end(0);
  return exit_status();
}
