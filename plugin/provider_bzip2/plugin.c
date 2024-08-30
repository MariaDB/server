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
#include <bzlib.h>
#include <providers/bzlib.h>

static int init(void* h)
{
  provider_service_bzip2->BZ2_bzBuffToBuffCompress_ptr= BZ2_bzBuffToBuffCompress;
  provider_service_bzip2->BZ2_bzBuffToBuffDecompress_ptr= BZ2_bzBuffToBuffDecompress;
  provider_service_bzip2->BZ2_bzCompress_ptr= BZ2_bzCompress;
  provider_service_bzip2->BZ2_bzCompressEnd_ptr= BZ2_bzCompressEnd;
  provider_service_bzip2->BZ2_bzCompressInit_ptr= BZ2_bzCompressInit;
  provider_service_bzip2->BZ2_bzDecompress_ptr= BZ2_bzDecompress;
  provider_service_bzip2->BZ2_bzDecompressEnd_ptr= BZ2_bzDecompressEnd;
  provider_service_bzip2->BZ2_bzDecompressInit_ptr= BZ2_bzDecompressInit;

  provider_service_bzip2->is_loaded = true;

  return 0;
}

static int deinit(void *h)
{
  return 1; /* don't unload me */
}

static struct st_mysql_daemon info= { MYSQL_DAEMON_INTERFACE_VERSION  };

maria_declare_plugin(provider_bzip2)
{
  MYSQL_DAEMON_PLUGIN,
  &info,
  "provider_bzip2",
  "Kartik Soneji",
  "BZip2 compression provider",
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
