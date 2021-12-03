/*
   Copyright (c) 2015 Daniel Black. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/


#ifndef MY_SERVICE_MANAGER_INCLUDED
#define MY_SERVICE_MANAGER_INCLUDED

#if defined(HAVE_SYSTEMD) && !defined(EMBEDDED_LIBRARY)
/*
  sd-daemon.h may include inttypes.h. Explicitly request format macros before
  the first inclusion of inttypes.h.
*/
#if !defined(__STDC_FORMAT_MACROS)
#define __STDC_FORMAT_MACROS
#endif  // !defined(__STDC_FORMAT_MACROS)
#include <systemd/sd-daemon.h>
/** INTERVAL in seconds followed by printf style status */
#define service_manager_extend_timeout(INTERVAL, FMTSTR, ...) \
  sd_notifyf(0, "STATUS=" FMTSTR "\nEXTEND_TIMEOUT_USEC=%u\n", ##__VA_ARGS__, INTERVAL * 1000000)

#else
#define sd_notify(X, Y)
#define sd_notifyf(E, F, ...)
#define service_manager_extend_timeout(I, FMTSTR, ...)
#endif

#endif /* MY_SERVICE_MANAGER_INCLUDED */
