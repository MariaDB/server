/* Copyright (c) 2011, Monty Program Ab

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

#include <my_global.h>
#include <my_sys.h>
#include <my_dbug.h>
#include "tap.h"

#ifndef DBUG_OFF
#define FALLBACK_TEST_COUNT 10
#else
#define FALLBACK_TEST_COUNT 0
#endif

int main(int argc __attribute__((unused)),char *argv[])
{
  char tmp_dir[MAX_PATH];
  char tmp_filename[MAX_PATH];
  HANDLE h, h2;

  MY_INIT(argv[0]);

  plan(16 + FALLBACK_TEST_COUNT);

  GetTempPathA(MAX_PATH, tmp_dir);
  ok(GetTempFileNameA(tmp_dir, "foo", 0,  tmp_filename) != 0, "create temp file");
  ok(my_delete(tmp_filename,MYF(0)) == 0, "Delete closed file");


  /* Delete an open file */
  ok(GetTempFileNameA(tmp_dir, "foo", 0,  tmp_filename) != 0, "create temp file 2");
  h = CreateFileA(tmp_filename, GENERIC_READ|GENERIC_WRITE, 
      FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
  ok (h != INVALID_HANDLE_VALUE || h != 0, "open temp file");
  ok(my_delete(tmp_filename, MYF(0)) == 0, "Delete open file");


  /* 
    Check if it is possible to reuse file name after delete (not all handles 
    to it are closed.
  */
  h2 = CreateFileA(tmp_filename, GENERIC_READ|GENERIC_WRITE, 
        FILE_SHARE_DELETE, NULL, CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, NULL);
  ok(h2 != 0 && h2 != INVALID_HANDLE_VALUE, "Reuse file name");
  CloseHandle(h);
  CloseHandle(h2);

  /* The handle below doesn't grant FILE_SHARE_DELETE, so my_win_unlink()'s
     own CreateFile(DELETE) fails outright and my_delete() must fail too. */
  ok(GetTempFileNameA(tmp_dir, "foo", 0, tmp_filename) != 0,
     "create temp file 3");
  h = CreateFileA(tmp_filename, GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  ok(h != INVALID_HANDLE_VALUE, "open temp file without FILE_SHARE_DELETE");
  ok(my_delete(tmp_filename, MYF(0)) != 0,
     "Delete fails when no strategy can get delete access");
  ok(GetFileAttributesA(tmp_filename) != INVALID_FILE_ATTRIBUTES,
     "...and the file is still there under its original name");
  CloseHandle(h);
  ok(DeleteFileA(tmp_filename) != 0, "cleanup: delete temp file 3");

  /* MDEV-39533: both disposition flavors fail on a read-only file (no
     FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE); check the file is
     still under its original name, not left as a "*.deleted" orphan. */
  ok(GetTempFileNameA(tmp_dir, "foo", 0, tmp_filename) != 0,
     "create temp file 3b (read-only)");
  ok(SetFileAttributesA(tmp_filename, FILE_ATTRIBUTE_READONLY) != 0,
     "mark it read-only");
  ok(my_delete(tmp_filename, MYF(0)) != 0,
     "Delete fails on a read-only file instead of silently losing it");
  ok(GetFileAttributesA(tmp_filename) != INVALID_FILE_ATTRIBUTES,
     "...and the file is still there under its original name, not "
     "renamed to a '*.deleted' orphan");
  SetFileAttributesA(tmp_filename, FILE_ATTRIBUTE_NORMAL);
  ok(DeleteFileA(tmp_filename) != 0, "cleanup: delete temp file 3b");

#ifndef DBUG_OFF
  /* MDEV-39533: force posix-semantics delete to fail (debug hook) and
     check the rename-then-dispose fallback, including that a new file
     can be created under the original name right away. */
  {
    HANDLE h_new;
    char final_path[MAX_PATH + 16];
    DWORD final_len;

    ok(GetTempFileNameA(tmp_dir, "foo", 0, tmp_filename) != 0,
       "create temp file 4");

    /* Hold our own handle, so the file survives my_delete()'s own close. */
    h = CreateFileA(tmp_filename, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, 0, NULL);
    ok(h != INVALID_HANDLE_VALUE, "open temp file 4");

    DBUG_SET("+d,force_posix_delete_fail");
    ok(my_delete(tmp_filename, MYF(0)) == 0,
       "Delete falls back to rename-then-delete-on-close when "
       "posix-semantics delete fails");
    DBUG_SET("-d,force_posix_delete_fail");

    /* Query via the handle: the new name isn't known until we ask it. */
    final_len= GetFinalPathNameByHandleA(h, final_path, sizeof(final_path),
                                          FILE_NAME_NORMALIZED);
    ok(final_len > 0 && final_len < sizeof(final_path) &&
       strstr(final_path, ".deleted") != NULL,
       "...the old file was renamed out of the way: '%s'", final_path);

    h_new = CreateFileA(tmp_filename, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_DELETE, NULL, CREATE_NEW, 0, NULL);
    ok(h_new != INVALID_HANDLE_VALUE,
       "...and the original name can be recreated right away");

    CloseHandle(h);
    CloseHandle(h_new);
    DeleteFileA(tmp_filename);
  }

  /* MDEV-39533: force both posix-semantics delete and the rename to fail;
     my_delete() should still succeed, under the original name. */
  {
    char final_path[MAX_PATH + 16];
    DWORD final_len;

    ok(GetTempFileNameA(tmp_dir, "foo", 0, tmp_filename) != 0,
       "create temp file 5");

    h = CreateFileA(tmp_filename, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, 0, NULL);
    ok(h != INVALID_HANDLE_VALUE, "open temp file 5");

    DBUG_SET("+d,force_posix_delete_fail,force_rename_fail");
    ok(my_delete(tmp_filename, MYF(0)) == 0,
       "Delete still succeeds under the original name when the rename "
       "itself fails");
    DBUG_SET("-d,force_posix_delete_fail,force_rename_fail");

    final_len= GetFinalPathNameByHandleA(h, final_path, sizeof(final_path),
                                          FILE_NAME_NORMALIZED);
    ok(final_len > 0 && final_len < sizeof(final_path) &&
       strstr(final_path, ".deleted") == NULL,
       "...and was NOT renamed: '%s'", final_path);

    CloseHandle(h);
    ok(GetFileAttributesA(tmp_filename) == INVALID_FILE_ATTRIBUTES,
       "...and is gone once the last handle closes");
  }
#endif

  my_end(0);
  return exit_status();
}

