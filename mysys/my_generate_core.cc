#ifndef _WIN32
#include <sys/wait.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>
#include <atomic>
#include "my_generate_core.h"

constexpr int FN_REFLEN= 512;

void my_generate_coredump(enum my_coredump_place_t which,
                          const char *coredump_path)
{
  static std::atomic_flag in_process{false};
  static std::atomic_uint cores_count[]= {{5}, {5}, {5}};

  if (!cores_count[which])
    return;

  if (in_process.test_and_set())
  {
    fputs("my_generate_coredump is already executing\n", stderr);
    return;
  }

  --cores_count[which];

  pid_t parent_pid= getpid();
  pid_t pid= fork();
  if (!pid)
  {
    /* Child */
    char parent_pid_str[42];
    snprintf(parent_pid_str, sizeof(parent_pid_str), "%" PRIdMAX,
             (intmax_t) parent_pid);
    char prefix[FN_REFLEN];
    if (snprintf(prefix, sizeof(prefix), "%s/core.%u.%u", coredump_path, which,
                 cores_count[which].load()) >= FN_REFLEN)
      snprintf(prefix, sizeof(prefix), "core.%u.%u", which,
               cores_count[which].load());
    execlp("gcore", "gcore", "-o", prefix, parent_pid_str, NULL);
  }
  else if (pid > 0)
  {
    /* Parent */
    int wstatus;
    waitpid(pid, &wstatus, 0);
  }
  else
    fputs("Fork error for my_generate_coredump\n", stderr);

  in_process.clear();
}
#endif /* _WIN32 */

