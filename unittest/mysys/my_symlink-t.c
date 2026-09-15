#include <my_global.h>
#include <my_sys.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "tap.h"

static void test_my_realpath()
{
#ifndef HAVE_REALPATH
  skip(2, "realpath not supported");
  return;
#else
  char cwd[PATH_MAX];
  char *resolved_cwd;
  char result[FN_REFLEN];
  char link_name[64] = {0};
  int errcode;
  if (getcwd(cwd, sizeof(cwd)) == NULL)
  {
    diag("getcwd() failed: %s", strerror(errno));
    skip(2, "getcwd() failed");
    return;
  }

  resolved_cwd= realpath(cwd, NULL);
  if (!resolved_cwd)
  {
    diag("realpath() failed: %s", strerror(errno));
    skip(2, "realpath() failed");
    return;
  }

  /* Call my_realpath with empty string in current working directory,
  expecting resolved abolute path to that directory */
  errcode= my_realpath(result, "", MYF(0));
  ok(errcode == 0, "my_realpath returned error code: %d", errcode);
  ok(!strcmp(result, resolved_cwd),
     "Output of my_realpath:  %s, expected: %s",
     result, resolved_cwd);
  /* Creae a symlink, chdir to it and resolve empty string */

  /* Generate a unique name for the symlink */
  srand(time(NULL));
  snprintf(link_name, sizeof(link_name), "my_link_%d.%06d", getpid(), rand() % 1000000);
  if (symlink(cwd, link_name) != 0)
  {
    diag("symlink() failed: %s", strerror(errno));
    skip(1, "symlink() failed");
    goto cwd_cleanup;
  }
  if (chdir(link_name) != 0)
  {
    diag("chdir() failed: %s", strerror(errno));
    skip(1, "chdir() failed");
    goto link_cleanup;
  }
  errcode= my_realpath(result, "", MYF(0));
  ok(errcode == 0, "my_realpath returned error code: %d", errcode);
  ok(!strcmp(result, resolved_cwd),
     "Output of my_realpath:  %s, expected: %s",
     result, resolved_cwd);
link_cleanup:
  unlink(link_name);
cwd_cleanup:
  free(resolved_cwd);
#endif
}

#ifdef _WIN32
/* MDEV-39533: my_realpath()/my_open()/my_delete() and NTFS junctions. */
#define WIN_TEST_COUNT 8

static void test_win_realpath_and_nosymlinks()
{
  char base[FN_REFLEN], realdir[FN_REFLEN], linkdir[FN_REFLEN];
  char file_via_real[FN_REFLEN], file_via_link[FN_REFLEN];
  char resolved[FN_REFLEN], resolved_real[FN_REFLEN];
  char cmd[FN_REFLEN * 2];
  char tmp[FN_REFLEN];
  File fd;
  int err;

  if (!GetTempPath(sizeof(tmp), tmp))
  {
    diag("GetTempPath() failed: %lu", GetLastError());
    skip(WIN_TEST_COUNT, "GetTempPath() failed");
    return;
  }
  srand(GetTickCount());
  my_snprintf(base, sizeof(base), "%smdev39533_%lu_%06d", tmp,
              GetCurrentProcessId(), rand() % 1000000);
  my_snprintf(realdir, sizeof(realdir), "%s\\realdir", base);
  my_snprintf(linkdir, sizeof(linkdir), "%s\\linkdir", base);
  my_snprintf(file_via_real, sizeof(file_via_real), "%s\\data.txt", realdir);
  my_snprintf(file_via_link, sizeof(file_via_link), "%s\\data.txt", linkdir);

  if (!CreateDirectory(base, NULL))
  {
    diag("CreateDirectory() failed: %lu", GetLastError());
    skip(WIN_TEST_COUNT, "CreateDirectory() failed");
    return;
  }
  if (!CreateDirectory(realdir, NULL))
  {
    diag("CreateDirectory() failed: %lu", GetLastError());
    skip(WIN_TEST_COUNT, "CreateDirectory() failed");
    goto rmdir_base;
  }

  fd= my_create(file_via_real, 0, O_RDWR, MYF(0));
  if (fd < 0)
  {
    diag("my_create() failed: %d", my_errno);
    skip(WIN_TEST_COUNT, "my_create() failed");
    goto rmdir_base;
  }
  my_close(fd, MYF(0));

  /* Junctions need no special privilege on NTFS, unlike symlinks. */
  my_snprintf(cmd, sizeof(cmd), "cmd /c mklink /J \"%s\" \"%s\" >nul 2>&1",
              linkdir, realdir);
  if (system(cmd) != 0)
  {
    diag("mklink /J failed, skipping (not NTFS, or junctions unsupported?)");
    skip(WIN_TEST_COUNT, "mklink /J failed");
    goto delete_file;
  }

  err= my_realpath(resolved, file_via_link, MYF(0));
  ok(err == 0, "my_realpath() resolves through a junction: err=%d", err);

  err= my_realpath(resolved_real, file_via_real, MYF(0));
  ok(err == 0 && !strcmp(resolved, resolved_real),
     "my_realpath() output for the junctioned path matches the real path: "
     "'%s' vs '%s'", resolved, resolved_real);

  fd= my_open(resolved, O_RDONLY, MYF(MY_NOSYMLINKS));
  ok(fd >= 0, "my_open(MY_NOSYMLINKS) succeeds on an already-resolved name");
  if (fd >= 0)
    my_close(fd, MYF(0));

  fd= my_open(file_via_link, O_RDONLY, MYF(MY_NOSYMLINKS));
  ok(fd < 0 && my_errno == ENOTDIR,
     "my_open(MY_NOSYMLINKS) rejects a path still containing a junction: "
     "fd=%d my_errno=%d", (int) fd, my_errno);
  if (fd >= 0)
    my_close(fd, MYF(0));

  fd= my_open(file_via_link, O_RDONLY, MYF(0));
  ok(fd >= 0, "my_open() without MY_NOSYMLINKS still follows the junction");
  if (fd >= 0)
    my_close(fd, MYF(0));

  {
    char missing[FN_REFLEN], missing_resolved[FN_REFLEN];
    my_snprintf(missing, sizeof(missing), "%s\\does_not_exist.txt", realdir);
    err= my_realpath(missing_resolved, missing, MYF(0));
    ok(err == 1 && my_errno == ENOENT,
       "my_realpath() on a non-existent path returns 1/ENOENT: err=%d "
       "my_errno=%d", err, my_errno);
  }

  err= my_delete(file_via_link, MYF(MY_NOSYMLINKS));
  ok(err != 0 && my_errno == ENOTDIR,
     "my_delete(MY_NOSYMLINKS) rejects a path still containing a junction: "
     "err=%d my_errno=%d", err, my_errno);

  err= my_delete(resolved, MYF(MY_NOSYMLINKS));
  ok(err == 0, "my_delete(MY_NOSYMLINKS) succeeds on an already-resolved "
     "name: err=%d", err);

  RemoveDirectory(linkdir);
delete_file:
  my_delete(file_via_real, MYF(0));
  RemoveDirectory(realdir);
rmdir_base:
  RemoveDirectory(base);
}
#endif /* _WIN32 */

int main(int argc __attribute__((unused)),char *argv[])
{
  MY_INIT(argv[0]);
  /* test_my_realpath() only emits 2 points on Windows, not 4. */
  plan(IF_WIN(2 + WIN_TEST_COUNT, 4));

  test_my_realpath();
#ifdef _WIN32
  test_win_realpath_and_nosymlinks();
#endif

  my_end(0);
  return exit_status();
}
