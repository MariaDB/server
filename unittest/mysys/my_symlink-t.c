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

int main(int argc __attribute__((unused)),char *argv[])
{
  MY_INIT(argv[0]);
  plan(4);

  test_my_realpath();

  my_end(0);
  return exit_status();
}
