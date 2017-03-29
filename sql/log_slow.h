/* Copyright (C) 2009, 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 or later of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/* Defining what to log to slow log */

#define LOG_SLOW_VERBOSITY_INIT           0
#define LOG_SLOW_VERBOSITY_INNODB         (1U << 0)
#define LOG_SLOW_VERBOSITY_QUERY_PLAN     (1U << 1)
#define LOG_SLOW_VERBOSITY_EXPLAIN        (1U << 2)

#define QPLAN_INIT            QPLAN_QC_NO

#define QPLAN_ADMIN           (1U << 0)
#define QPLAN_FILESORT        (1U << 1)
#define QPLAN_FILESORT_DISK   (1U << 2)
#define QPLAN_FULL_JOIN       (1U << 3)
#define QPLAN_FULL_SCAN       (1U << 4)
#define QPLAN_QC              (1U << 5)
#define QPLAN_QC_NO           (1U << 6)
#define QPLAN_TMP_DISK        (1U << 7)
#define QPLAN_TMP_TABLE       (1U << 8)
#define QPLAN_FILESORT_PRIORITY_QUEUE       (1U << 9)
 
/* ... */
#define QPLAN_MAX             (1U << 31) /* reserved as placeholder */
