#ifndef CLIENT_CONNECT_INCLUDED
#define CLIENT_CONNECT_INCLUDED
/*
   Copyright (c) 2000, 2018, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB Corporation.

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

#include <my_global.h>

/* The common non-specific connection options that can be safely used by any client */
typedef struct st_clnt_connect_options
{
  my_bool secure_auth;
  uint protocol;
  char *plugin_dir;
  const char *program_name;
  my_bool compress;
  uint port;
  char *default_charset;
  char *charsets_dir;
  char *default_auth;
  char *bind_address;
  char *socket;
  ulong read_timeout;
  ulong write_timeout;
  ulong connect_timeout;
  char *host;
  char *user;
  char *database;
  char *password;
#include "sslopt-vars.h"
} CLNT_CONNECT_OPTIONS;

#define CL_INIT_OPTS(...)       \
{ __VA_ARGS__ INIT_SLL_OPTS }

#define CLNT_INIT_OPTS_WITH_PRG_NAME_DEFCHAR_DB_USR(prog_name, defchar, db, usr) \
  CL_INIT_OPTS(0, 0, NULL, prog_name, 0, 0, defchar, NULL, NULL,                 \
               NULL, NULL, 0, 0, 0, NULL, usr, db, NULL)

#define CLNT_INIT_OPTS_WITH_PRG_NAME_DEFCHAR_DB(prog_name, defchar, db)        \
  CLNT_INIT_OPTS_WITH_PRG_NAME_DEFCHAR_DB_USR(prog_name, defchar, db, NULL)

#define CLNT_INIT_OPTS_WITH_PRG_NAME_DEFCHAR(prog_name, defchar)               \
  CLNT_INIT_OPTS_WITH_PRG_NAME_DEFCHAR_DB(prog_name, defchar, NULL)

#define CLNT_INIT_OPTS_WITH_PRG_NAME(prog_name)                                \
  CLNT_INIT_OPTS_WITH_PRG_NAME_DEFCHAR(prog_name, NULL)

#ifdef __cplusplus
extern "C"
{
#endif

/**
  @brief Set connection-specific options and call mysql_real_connect.

  @param  mysql                  Pointer to the MYSQL connector info.
  @param  opts                   Pointer to the generic client connection options.
  @param  flags                  Connection flags. Can be 0.

  @return  Pointer to the MYSQL struct, or NULL in case of error.
*/
MYSQL *STDCALL
do_client_connect(MYSQL *mysql, const CLNT_CONNECT_OPTIONS *opts, ulong flags);
#ifdef __cplusplus
}
#endif
#endif // CLIENT_CONNECT_INCLUDED
