/*
   Copyright (c) 2017, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/************************** CLIENT *************************************/

#include <stdlib.h>
#include "common.h"
#include <mysql/client_plugin.h>
#include <errmsg.h>

#if !defined(__attribute__) && !defined(__GNUC__)
#define __attribute__(A)
#endif

static int do_auth(MYSQL_PLUGIN_VIO *vio, MYSQL *mysql)
{
  unsigned char reply[CRYPTO_BYTES + NONCE_BYTES], *pkt;
  int pkt_len;

  /* read the nonce */
  if ((pkt_len= vio->read_packet(vio, &pkt)) != NONCE_BYTES)
    return CR_SERVER_HANDSHAKE_ERR;

  /* sign the nonce */
  crypto_sign(reply, pkt, NONCE_BYTES,
              (unsigned char*)mysql->passwd, strlen(mysql->passwd));

  /* send the signature */
  if (vio->write_packet(vio, reply, CRYPTO_BYTES))
    return CR_ERROR;

  return CR_OK;
}

static int init_client(char *unused1   __attribute__((unused)),
                       size_t unused2  __attribute__((unused)),
                       int unused3     __attribute__((unused)),
                       va_list unused4 __attribute__((unused)))
{
  return 0;
}

mysql_declare_client_plugin(AUTHENTICATION)
  "client_ed25519",
  "Sergei Golubchik",
  "Elliptic curve ED25519 based authentication",
  {0,1,0},
  "GPL",
  NULL,
  init_client,
  NULL,
  NULL,
  do_auth,
mysql_end_client_plugin;

