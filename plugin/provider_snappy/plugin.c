/* Copyright (c) 2021, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include <stdbool.h>
#include <mysql_version.h>
#include <mysql/plugin.h>
#include <snappy-c.h>
#define SNAPPY_C
#include <providers/snappy-c.h>

static int init(void* h)
{
  provider_service_snappy->snappy_max_compressed_length_ptr= snappy_max_compressed_length;
  provider_service_snappy->snappy_compress_ptr= snappy_compress;
  provider_service_snappy->snappy_uncompressed_length_ptr= snappy_uncompressed_length;
  provider_service_snappy->snappy_uncompress_ptr= snappy_uncompress;

  provider_service_snappy->is_loaded = true;

  return 0;
}

static int deinit(void *h)
{
  return 1; /* don't unload me */
}

static struct st_mysql_daemon info= { MYSQL_DAEMON_INTERFACE_VERSION  };

maria_declare_plugin(provider_snappy)
{
  MYSQL_DAEMON_PLUGIN,
  &info,
  "provider_snappy",
  "Kartik Soneji",
  "SNAPPY compression provider",
  PLUGIN_LICENSE_GPL,
  init,
  deinit,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
