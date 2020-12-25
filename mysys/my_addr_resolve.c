/* Copyright (C) 2011 Monty Program Ab

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

#include "mysys_priv.h"
#include <m_string.h>
#include <my_sys.h>
#include <my_stacktrace.h>

/**
  strip the path, leave the file name and the last dirname
*/
static const char *strip_path(const char *s) __attribute__((unused));
static const char *strip_path(const char *s)
{
  const char *prev, *last;
  for(prev= last= s; *s; s++)
    if (*s == '/' || *s == '\\')
    {
      prev= last;
      last= s + 1;
    }
  return prev;
}

/*
  The following is very much single-threaded code and it's only supposed
  to be used on shutdown or for a crash report
  Or the caller should take care and use mutexes.

  Also it does not free any its memory. For the same reason -
  it's only used for crash reports or on shutdown when we already
  have a memory leak.
*/

#ifdef HAVE_BFD_H
#include <bfd.h>
static bfd *bfdh= 0;
static asymbol **symtable= 0;

#if defined(HAVE_LINK_H) && defined(HAVE_DLOPEN)
#include <link.h>
static ElfW(Addr) offset= 0;
#else
#define offset 0
#endif

#ifndef bfd_get_section_flags
#define bfd_get_section_flags(H, S) bfd_section_flags(S)
#endif /* bfd_get_section_flags */

#ifndef bfd_get_section_size
#define bfd_get_section_size(S) bfd_section_size(S)
#endif /* bfd_get_section_size */

#ifndef bfd_get_section_vma
#define bfd_get_section_vma(H, S) bfd_section_vma(S)
#endif /* bfd_get_section_vma */

/**
  finds a file name, a line number, and a function name corresponding to addr.

  the function name is demangled.
  the file name is stripped of its path, only the two last components are kept
  the resolving logic is mostly based on addr2line of binutils-2.17

  @return 0 on success, 1 on failure
*/
int my_addr_resolve(void *ptr, my_addr_loc *loc)
{
  bfd_vma addr= (intptr)ptr - offset;
  asection *sec;

  for (sec= bfdh->sections; sec; sec= sec->next)
  {
    bfd_vma start;

    if ((bfd_get_section_flags(bfdh, sec) & SEC_ALLOC) == 0)
      continue;

    start = bfd_get_section_vma(bfdh, sec);
    if (addr < start || addr >= start + bfd_get_section_size(sec))
      continue;

    if (bfd_find_nearest_line(bfdh, sec, symtable, addr - start,
                              &loc->file, &loc->func, &loc->line))
    {
      if (loc->file)
        loc->file= strip_path(loc->file);
      else
        loc->file= "";

      if (loc->func)
      {
        const char *str= bfd_demangle(bfdh, loc->func, 3);
        if (str)
          loc->func= str;
      }

      return 0;
    }
  }
  
  return 1;
}

const char *my_addr_resolve_init()
{
  if (!bfdh)
  {
    uint unused;
    char **matching;

#if defined(HAVE_LINK_H) && defined(HAVE_DLOPEN)
    struct link_map *lm = (struct link_map*) dlopen(0, RTLD_NOW);
    if (lm)
      offset= lm->l_addr;
#endif

    bfdh= bfd_openr(my_progname, NULL);
    if (!bfdh)
      goto err;

    if (bfd_check_format(bfdh, bfd_archive))
      goto err;
    if (!bfd_check_format_matches (bfdh, bfd_object, &matching))
      goto err;

    if (bfd_read_minisymbols(bfdh, FALSE, (void *)&symtable, &unused) < 0)
      goto err;
  }
  return 0;

err:
  return bfd_errmsg(bfd_get_error());
}
#elif defined(HAVE_LIBELF_H)
/*
  another possible implementation.
*/
#elif defined(MY_ADDR_RESOLVE_FORK)
/*
  yet another - just execute addr2line pipe the addresses to it, and parse the
  output
*/

#include <m_string.h>
#include <ctype.h>
#include <sys/wait.h>

#if defined(HAVE_POLL_H)
#include <poll.h>
#elif defined(HAVE_SYS_POLL_H)
#include <sys/poll.h>
#endif /* defined(HAVE_POLL_H) */

static int in[2], out[2];
static pid_t pid;
static char addr2line_binary[1024];
static char output[1024];
static struct pollfd poll_fds;
static Dl_info info;

int start_addr2line_fork(const char *binary_path)
{

  if (pid > 0)
  {
    /* Don't leak FDs */
    close(in[1]);
    close(out[0]);
    /* Don't create zombie processes. */
    waitpid(pid, NULL, 0);
  }

  if (pipe(in) < 0)
    return 1;
  if (pipe(out) < 0)
    return 1;

  pid = fork();
  if (pid == -1)
    return 1;

  if (!pid) /* child */
  {
    dup2(in[0], 0);
    dup2(out[1], 1);
    close(in[0]);
    close(in[1]);
    close(out[0]);
    close(out[1]);
    execlp("addr2line", "addr2line", "-C", "-f", "-e", binary_path, NULL);
    exit(1);
  }

  close(in[0]);
  close(out[1]);

  return 0;
}

int my_addr_resolve(void *ptr, my_addr_loc *loc)
{
  char input[32];
  size_t len;

  ssize_t total_bytes_read = 0;
  ssize_t extra_bytes_read = 0;
  ssize_t parsed = 0;

  int ret;

  int filename_start = -1;
  int line_number_start = -1;

  void *offset;

  poll_fds.fd = out[0];
  poll_fds.events = POLLIN | POLLRDBAND;

  if (!dladdr(ptr, &info))
    return 1;

  if (strcmp(addr2line_binary, info.dli_fname))
  {
    /* We use dli_fname in case the path is longer than the length of our static
       string. We don't want to allocate anything dynamicaly here as we are in
       a "crashed" state. */
    if (start_addr2line_fork(info.dli_fname))
    {
      addr2line_binary[0] = '\0';
      return 2;
    }
    /* Save result for future comparisons. */
    strnmov(addr2line_binary, info.dli_fname, sizeof(addr2line_binary));
  }
  offset = info.dli_fbase;
  len= my_snprintf(input, sizeof(input), "%08x\n", (ulonglong)(ptr - offset));
  if (write(in[1], input, len) <= 0)
    return 3;


  /* 500 ms should be plenty of time for addr2line to issue a response. */
  /* Read in a loop till all the output from addr2line is complete. */
  while (parsed == total_bytes_read &&
         (ret= poll(&poll_fds, 1, 500)))
  {
    /* error during poll */
    if (ret < 0)
      return 1;

    extra_bytes_read= read(out[0], output + total_bytes_read,
                           sizeof(output) - total_bytes_read);
    if (extra_bytes_read < 0)
      return 4;
    /* Timeout or max bytes read. */
    if (extra_bytes_read == 0)
      break;

    total_bytes_read += extra_bytes_read;

    /* Go through the addr2line response and get the required data.
       The response is structured in 2 lines. The first line contains the function
       name, while the second one contains <filename>:<line number> */
    for (; parsed < total_bytes_read; parsed++)
    {
      if (output[parsed] == '\n')
      {
        filename_start = parsed + 1;
        output[parsed] = '\0';
      }
      if (filename_start != -1 && output[parsed] == ':')
      {
        line_number_start = parsed + 1;
        output[parsed] = '\0';
        break;
      }
    }
  }

  /* Response is malformed. */
  if (filename_start == -1 || line_number_start == -1)
   return 5;

  loc->func= output;
  loc->file= output + filename_start;
  loc->line= atoi(output + line_number_start);

  /* Addr2line was unable to extract any meaningful information. */
  if (strcmp(loc->file, "??") == 0)
    return 6;

  loc->file= strip_path(loc->file);

  return 0;
}

const char *my_addr_resolve_init()
{
  return 0;
}
#endif
