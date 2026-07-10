/* Copyright (c) 2017, MariaDB Plc

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

#ifdef _WIN32
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <libproc.h>
#elif defined(__FreeBSD__)
#include <sys/sysctl.h>
#elif defined(__NetBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

/** Fill the buffer with the executable program name
@param[out]     buf      buffer to place obtained executable name.
@param[in]      size     size available in the buffer.
@param[in]      argv0    the argv0 from which the executable argv might be used.
@return 0, for successful filling of buf, non-zero for failure. */
int my_get_exepath(char *buf, size_t size, const char *argv0)
{
#ifdef _WIN32
  DWORD ret = GetModuleFileNameA(NULL, buf, (DWORD)size);
  if (ret > 0)
    return 0;
#elif defined(__linux__)
  ssize_t ret = readlink("/proc/self/exe", buf, size-1);
  if(ret > 0)
    return 0;
#elif defined(__APPLE__)
  size_t ret = proc_pidpath(getpid(), buf, (uint32_t)size);
  if (ret > 0) {
    buf[ret] = 0;
    return 0;
  }
#elif defined(__FreeBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
    return 0;
  }
#elif defined(__NetBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME };
  if (sysctl(mib, 4, buf, &size, NULL, 0) == 0)
    return 0;
#endif

  if (argv0)
    return my_realpath(buf, argv0, 0);
  return 1;
}
