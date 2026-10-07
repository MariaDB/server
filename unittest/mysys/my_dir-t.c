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

/* Test of my_dir_open() & co */

#include <my_global.h>
#include <my_sys.h>
#include <my_dir.h>
#include <m_string.h>
#include "tap.h"

/* Files and directories that the test creates in the test directory */

static const char *test_files[]=
{ "t1.MAD", "t1.MAI", "t1.frm", "db.opt", NullS };

static const char *test_dirs[]= { "sub1", "sub2", NullS };

static char test_dir[FN_REFLEN];


/*
  Create the directory tree that the tests run on.
*/

static int create_test_dir(const char *name)
{
  char path[FN_REFLEN];
  const char **ptr;

  if (my_mkdir(name, 0777, MYF(MY_WME)))
    return 1;

  for (ptr= test_files; *ptr; ptr++)
  {
    File file;
    strxnmov(path, sizeof(path) - 1, name, "/", *ptr, NullS);
    if ((file= my_create(path, 0666, O_WRONLY, MYF(MY_WME))) < 0)
      return 1;
    if (my_write(file, (const uchar*) "test", 4, MYF(MY_WME | MY_NABP)) ||
        my_close(file, MYF(MY_WME)))
      return 1;
  }

  for (ptr= test_dirs; *ptr; ptr++)
  {
    strxnmov(path, sizeof(path) - 1, name, "/", *ptr, NullS);
    if (my_mkdir(path, 0777, MYF(MY_WME)))
      return 1;
  }
  return 0;
}


/*
  Remove the directory tree created by create_test_dir().
*/

static void remove_test_dir(const char *name)
{
  char path[FN_REFLEN];
  const char **ptr;

  for (ptr= test_files; *ptr; ptr++)
  {
    strxnmov(path, sizeof(path) - 1, name, "/", *ptr, NullS);
    my_delete(path, MYF(0));
  }
  for (ptr= test_dirs; *ptr; ptr++)
  {
    strxnmov(path, sizeof(path) - 1, name, "/", *ptr, NullS);
    rmdir(path);
  }
  rmdir(name);
}


/*
  Count the entries that my_dir_open()/my_dir_read_next() gives us.

  @param dirs   where to store the number of directories found
  @param files  where to store the number of files found
  @return number of entries, or -1 in case of error
*/

static int count_entries(const char *filter, myf open_flags,
                         myf read_flags, int *dirs, int *files)
{
  MY_NO_CACHE_DIR *dir;
  char name[FN_REFLEN];
  MY_STAT stat_area;
  int entries= 0, error;

  *dirs= *files= 0;
  if (!(dir= my_dir_open(test_dir, filter, MYF(open_flags | MY_WME))))
    return -1;

  while (!(error= my_dir_read_next(dir, name, sizeof(name), &stat_area,
                                   MYF(read_flags))))
  {
    entries++;
    if (MY_S_ISDIR(stat_area.st_mode))
      (*dirs)++;
    if (MY_S_ISREG(stat_area.st_mode))
      (*files)++;
  }
  if (error != MY_DIR_EOF)
    entries= -1;
  my_dir_close(dir);
  return entries;
}


/*
  Read all entries and check that we find the expected names.
*/

static void test_read_all(void)
{
  int entries, dirs, files;

  entries= count_entries(NullS, 0, 0, &dirs, &files);
  ok(entries == 6, "found %d entries, expected 6", entries);
  ok(dirs == 2, "found %d directories, expected 2", dirs);
  ok(files == 4, "found %d files, expected 4", files);
}


/*
  Test MY_DIR_ONLY_DIRS and MY_DIR_ONLY_FILES.
*/

static void test_only_flags(void)
{
  int entries, dirs, files;

  entries= count_entries(NullS, MY_DIR_ONLY_DIRS, 0, &dirs, &files);
  ok(entries == 2 && dirs == 2 && files == 0,
     "MY_DIR_ONLY_DIRS: %d entries, %d dirs, %d files",
     entries, dirs, files);

  entries= count_entries(NullS, MY_DIR_ONLY_FILES, 0, &dirs, &files);
  ok(entries == 4 && dirs == 0 && files == 4,
     "MY_DIR_ONLY_FILES: %d entries, %d dirs, %d files",
     entries, dirs, files);
}


/*
  Test the wildcard filter.
*/

static void test_filter(void)
{
  int entries, dirs, files;

  entries= count_entries("*.MA?", MY_DIR_ONLY_FILES, 0, &dirs, &files);
  ok(entries == 2, "filter '*.MA?' gave %d entries, expected 2", entries);

  entries= count_entries("sub?", 0, 0, &dirs, &files);
  ok(entries == 2, "filter 'sub?' gave %d entries, expected 2", entries);

  entries= count_entries("no_such_file*", 0, 0, &dirs, &files);
  ok(entries == 0, "filter 'no_such_file*' gave %d entries, expected 0",
     entries);
}


/*
  Test that MY_WANT_STAT gives the file size while the cheap path
  still tells files and directories apart.
*/

static void test_stat(void)
{
  MY_NO_CACHE_DIR *dir;
  char name[FN_REFLEN];
  MY_STAT stat_area;
  int sizes_ok= 1, entries= 0;

  if (!(dir= my_dir_open(test_dir, "*.MAD", MYF(MY_DIR_ONLY_FILES |
                                                MY_WME))))
  {
    ok(0, "my_dir_open failed");
    return;
  }
  while (!my_dir_read_next(dir, name, sizeof(name), &stat_area,
                           MYF(MY_WANT_STAT)))
  {
    entries++;
    if (stat_area.st_size != 4 || !MY_S_ISREG(stat_area.st_mode))
      sizes_ok= 0;
  }
  ok(entries == 1 && sizes_ok,
     "MY_WANT_STAT: %d entries with correct size and mode", entries);
  my_dir_close(dir);
}


/*
  Test my_dir_rewind().
*/

static void test_rewind(void)
{
  MY_NO_CACHE_DIR *dir;
  char name[FN_REFLEN];
  int first= 0, second= 0;

  if (!(dir= my_dir_open(test_dir, NullS, MYF(MY_WME))))
  {
    ok(0, "my_dir_open failed");
    return;
  }
  while (!my_dir_read_next(dir, name, sizeof(name), NULL, MYF(0)))
    first++;
  ok(!my_dir_rewind(dir, MYF(MY_WME)), "my_dir_rewind");
  while (!my_dir_read_next(dir, name, sizeof(name), NULL, MYF(0)))
    second++;
  ok(first == second && first == 6,
     "%d entries before rewind, %d after", first, second);
  my_dir_close(dir);
}


/*
  Test that a name that does not fit into the buffer is reported.
*/

static void test_name_too_long(void)
{
  MY_NO_CACHE_DIR *dir;
  char name[4];
  int too_long= 0, error;

  if (!(dir= my_dir_open(test_dir, "t1.MAD", MYF(MY_WME))))
  {
    ok(0, "my_dir_open failed");
    return;
  }
  while ((error= my_dir_read_next(dir, name, sizeof(name), NULL,
                                  MYF(0))) != MY_DIR_EOF)
  {
    if (error == MY_DIR_NAME_TOO_LONG)
      too_long++;
  }
  ok(too_long == 1 && !name[3] && !strcmp(name, "t1."),
     "name too long gave '%s', errors: %d", name, too_long);
  my_dir_close(dir);
}


/*
  Test that symbolic links are resolved; a link to a directory has to
  be reported as a directory and a link to a file as a file.
*/

static void test_symlinks(void)
{
#ifdef _WIN32
  skip(2, "symlinks are not tested on Windows");
#else
  char link_to_dir[FN_REFLEN], link_to_file[FN_REFLEN];
  int entries, dirs, files;

  strxnmov(link_to_dir, sizeof(link_to_dir) - 1, test_dir, "/link_dir",
           NullS);
  strxnmov(link_to_file, sizeof(link_to_file) - 1, test_dir, "/link_file",
           NullS);
  /* The targets are relative to the directory of the link */
  if (symlink("sub1", link_to_dir))
  {
    skip(2, "could not create a symlink");
    return;
  }
  if (symlink("t1.MAD", link_to_file))
  {
    my_delete(link_to_dir, MYF(0));
    skip(2, "could not create a symlink");
    return;
  }

  entries= count_entries(NullS, MY_DIR_ONLY_DIRS, 0, &dirs, &files);
  ok(entries == 3 && dirs == 3,
     "a link to a directory is a directory: %d entries, %d dirs",
     entries, dirs);
  entries= count_entries(NullS, MY_DIR_ONLY_FILES, 0, &dirs, &files);
  ok(entries == 5 && files == 5,
     "a link to a file is a file: %d entries, %d files", entries, files);

  my_delete(link_to_dir, MYF(0));
  my_delete(link_to_file, MYF(0));
#endif /* _WIN32 */
}


/*
  Test that the path is stored without any end separator and that
  opening a directory that does not exist fails.
*/

static void test_path_and_errors(void)
{
  MY_NO_CACHE_DIR *dir;
  char path[FN_REFLEN];

  strxnmov(path, sizeof(path) - 1, test_dir, "/", NullS);
  if (!(dir= my_dir_open(path, NullS, MYF(MY_WME))))
  {
    ok(0, "my_dir_open failed");
    return;
  }
  ok(!strcmp(dir->path.str, test_dir) && dir->path.length == strlen(test_dir),
     "stored path is '%s'", dir->path.str);
  my_dir_close(dir);

  strxnmov(path, sizeof(path) - 1, test_dir, "/no_such_dir", NullS);
  dir= my_dir_open(path, NullS, MYF(0));
  ok(!dir, "my_dir_open of a directory that does not exist fails");
  if (dir)
    my_dir_close(dir);
}


int main(int argc __attribute__((unused)), char *argv[])
{
  MY_INIT(argv[0]);
  plan(16);

  snprintf(test_dir, sizeof(test_dir), "my_dir_test_%d", (int) getpid());
  if (create_test_dir(test_dir))
  {
    remove_test_dir(test_dir);
    diag("Could not create the test directory %s", test_dir);
    my_end(0);
    return exit_status();
  }

  test_read_all();
  test_only_flags();
  test_filter();
  test_stat();
  test_rewind();
  test_name_too_long();
  test_symlinks();
  test_path_and_errors();

  remove_test_dir(test_dir);
  my_end(0);
  return exit_status();
}
