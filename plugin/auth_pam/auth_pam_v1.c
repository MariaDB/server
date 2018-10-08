/*
   Copyright (c) 2011, 2018 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include <mysql/plugin_auth.h>

struct param {
  unsigned char buf[10240], *ptr;
  MYSQL_PLUGIN_VIO *vio;
};

static int write_packet(struct param *param, const unsigned char *buf,
                        int buf_len)
{
  return param->vio->write_packet(param->vio, buf, buf_len);
}

static int read_packet(struct param *param, unsigned char **pkt)
{
  return param->vio->read_packet(param->vio, pkt);
}

#include "auth_pam_base.c"

static int pam_auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  struct param param;
  param.vio = vio;
  return pam_auth_base(&param, info);
}


#include "auth_pam_common.c"


static int init(void *p __attribute__((unused)))
{
  if (use_cleartext_plugin)
    info.client_auth_plugin= "mysql_clear_password";
  return 0;
}

maria_declare_plugin(pam)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &info,
  "pam",
  "Sergei Golubchik",
  "PAM based authentication",
  PLUGIN_LICENSE_GPL,
  init,
  NULL,
  0x0100,
  NULL,
  vars,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
