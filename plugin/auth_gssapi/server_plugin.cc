/* Copyright (c) 2015, Shuang Qiu, Robbie Harwood,
   Vladislav Vaintroub & MariaDB Corporation

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

/**
  @file

  GSSAPI authentication plugin, server side
*/
#include <my_sys.h>
#include <mysqld_error.h>
#include <mysql/plugin_auth.h>
#include "server_plugin.h"
#include "common.h"

/* First packet sent from server to client, contains srv_principal_name\0mech\0 */
static char first_packet[PRINCIPAL_NAME_MAX + MECH_NAME_MAX +2];
static int  first_packet_len;

/*
 Target name in GSSAPI/SSPI , for Kerberos it is service principal name
 (often user principal name of the server user will work)
*/
char *srv_principal_name;
char *srv_keytab_path;
char *srv_mech_name=(char *)"";
unsigned long srv_mech;

/**
  The main server function of the GSSAPI plugin.
 */
static int gssapi_auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *auth_info)
{
  int use_full_name;
  const char *user;
  int user_len;

  /* No user name yet ? Read the client handshake packet with the user name. */
  if (auth_info->user_name == 0)
  {
    unsigned char *pkt;
    if (vio->read_packet(vio, &pkt) < 0)
      return CR_ERROR;
  }
  
  /* Send first packet with target name and mech name */
  if (vio->write_packet(vio, (unsigned char *)first_packet, first_packet_len))
  {
    return CR_ERROR;
  }

  /* Figure out whether to use full name (as given in IDENTIFIED AS clause)
   * or just short username auth_string
   */
  if (auth_info->auth_string_length > 0)
  {
    use_full_name= 1;
    user= auth_info->auth_string;
    user_len= auth_info->auth_string_length;
  }
  else
  {
    use_full_name= 0;
    user= auth_info->user_name;
    user_len= auth_info->user_name_length;
  }

  return auth_server(vio, user, user_len, use_full_name);
}

static int initialize_plugin(void *unused)
{
  int rc;
  rc = plugin_init();
  if (rc)
    return rc;

  strcpy(first_packet, srv_principal_name);
  strcpy(first_packet + strlen(srv_principal_name) + 1,srv_mech_name);
  first_packet_len = strlen(srv_principal_name) + strlen(srv_mech_name) + 2;

  return 0;
}

static int deinitialize_plugin(void *unused)
{
  return plugin_deinit();
}

/* system variable */
static MYSQL_SYSVAR_STR(keytab_path, srv_keytab_path,
                        PLUGIN_VAR_RQCMDARG|PLUGIN_VAR_READONLY,
                        "Keytab file path for Kerberos authentication",
                        NULL,
                        NULL,
                        "");
static MYSQL_SYSVAR_STR(principal_name, srv_principal_name,
                        PLUGIN_VAR_RQCMDARG|PLUGIN_VAR_READONLY,
                        "GSSAPI target name - service principal name for Kerberos authentication.",
                        NULL,
                        NULL,
                        "");
#ifdef PLUGIN_SSPI
static const char* mech_names[] = {
  "Kerberos",
  "Negotiate",
  "",
  NULL
};
static TYPELIB mech_name_typelib = {
  array_elements(mech_names) - 1,
  "mech_name_typelib",
  mech_names,
  NULL
};
static MYSQL_SYSVAR_ENUM(mech_name, srv_mech,
                        PLUGIN_VAR_RQCMDARG|PLUGIN_VAR_READONLY,
                        "GSSAPI mechanism",
                        NULL,
                        NULL,
                        2,&mech_name_typelib);
#endif

static struct st_mysql_sys_var *system_variables[]= {
  MYSQL_SYSVAR(principal_name),
#ifdef PLUGIN_SSPI
  MYSQL_SYSVAR(mech_name),
#endif
#ifdef PLUGIN_GSSAPI
  MYSQL_SYSVAR(keytab_path),
#endif
  NULL
};

/* Register authentication plugin */
static struct st_mysql_auth server_handler= {
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  "auth_gssapi_client",
  gssapi_auth
};

maria_declare_plugin(gssapi_server)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &server_handler,
  "gssapi",
  "Shuang Qiu, Robbie Harwood, Vladislav Vaintroub",
  "Plugin for GSSAPI/SSPI based authentication.",
  PLUGIN_LICENSE_BSD,
  initialize_plugin,
  deinitialize_plugin,                   /* destructor */
  0x0100,                                /* version */
  NULL,                                  /* status variables */
  system_variables,                      /* system variables */
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

