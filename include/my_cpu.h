/* Copyright (c) 2013, MariaDB foundation Ab and SkySQL

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02111-1307  USA
*/

/* instructions for specific cpu's */

/*
  Macros for adjusting thread priority (hardware multi-threading)
  The defines are the same ones used by the linux kernel
*/

#ifdef _ARCH_PWR8
#include <sys/platform/ppc.h>
/* Very low priority */
#define HMT_very_low() __ppc_set_ppr_very_low()
/* Low priority */
#define HMT_low() __ppc_set_ppr_low()
/* Medium low priority */
#define HMT_medium_low() __ppc_set_ppr_med_low()
/* Medium priority */
#define HMT_medium() __ppc_set_ppr_med()
/* Medium high priority */
#define HMT_medium_high() __ppc_set_ppr_med_high()
/* High priority */
#define HMT_high() asm volatile("or 3,3,3")
#else
#define HMT_very_low()
#define HMT_low()
#define HMT_medium_low()
#define HMT_medium()
#define HMT_medium_high()
#define HMT_high()
#endif
