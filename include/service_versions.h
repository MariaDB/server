/* Copyright (c) 2009, 2010, Oracle and/or its affiliates.
   Copyright (c) 2012, 2021, MariaDB

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

#ifdef _WIN32
#define SERVICE_VERSION __declspec(dllexport) void *
#else
#define SERVICE_VERSION void *
#endif

#define VERSION_debug_sync              0x1000
#define VERSION_kill_statement          0x1000

#define VERSION_base64                  0x0100
#define VERSION_encryption              0x0300
#define VERSION_encryption_scheme       0x0100
#define VERSION_logger                  0x0100
#define VERSION_my_crypt                0x0100
#define VERSION_my_md5                  0x0100
#define VERSION_my_print_error          0x0100
#define VERSION_my_sha1                 0x0101
#define VERSION_my_sha2                 0x0100
#define VERSION_my_snprintf             0x0100
#define VERSION_progress_report         0x0100
#define VERSION_thd_alloc               0x0200
#define VERSION_thd_autoinc             0x0100
#define VERSION_thd_error_context       0x0200
#define VERSION_thd_rnd                 0x0100
#define VERSION_thd_specifics           0x0100
#define VERSION_thd_timezone            0x0100
#define VERSION_thd_wait                0x0100
#define VERSION_wsrep                   0x0500
#define VERSION_json                    0x0100
#define VERSION_thd_mdl                 0x0100
#define VERSION_sql_service             0x0100
