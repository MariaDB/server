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

/*
  This file contains code to interact with the PAM module.
  To be included into auth_pam_tool.c and auth_pam_v2.c,

  Before the #include these sould be defined:
  
  struct param {
    unsigned char buf[10240], *ptr;
    MYSQL_PLUGIN_VIO *vio;
    ...  other arbitrary fields allowed.
  };
  static int write_packet(struct param *param, const unsigned char *buf,
                          int buf_len)
  static int read_packet(struct param *param, unsigned char **pkt)
*/

#include <config_auth_pam.h>
#include <stdio.h>
#include <string.h>
#include <security/pam_appl.h>
#include <security/pam_modules.h>

/* It least solaris doesn't have strndup */

#ifndef HAVE_STRNDUP
char *strndup(const char *from, size_t length)
{
  char *ptr;
  size_t max_length= strlen(from);
  if (length > max_length)
    length= max_length;
  if ((ptr= (char*) malloc(length+1)) != 0)
  {
    memcpy((char*) ptr, (char*) from, length);
    ptr[length]=0;
  }
  return ptr;
}
#endif

#ifndef DBUG_OFF
static char pam_debug = 0;
#define PAM_DEBUG(X)   do { if (pam_debug) { fprintf X; } } while(0)
#else
#define PAM_DEBUG(X)   /* no-op */
#endif

static char winbind_hack = 0;

static int conv(int n, const struct pam_message **msg,
                struct pam_response **resp, void *data)
{
  struct param *param = (struct param *)data;
  unsigned char *end = param->buf + sizeof(param->buf) - 1;
  int i;

  *resp = 0;

  for (i = 0; i < n; i++)
  {
    /* if there's a message - append it to the buffer */
    if (msg[i]->msg)
    {
      int len = strlen(msg[i]->msg);
      if (len > end - param->ptr)
        len = end - param->ptr;
      if (len > 0)
      {
        memcpy(param->ptr, msg[i]->msg, len);
        param->ptr+= len;
        *(param->ptr)++ = '\n';
      }
    }
    /* if the message style is *_PROMPT_*, meaning PAM asks a question,
       send the accumulated text to the client, read the reply */
    if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
        msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
    {
      int pkt_len;
      unsigned char *pkt;

      /* allocate the response array.
         freeing it is the responsibility of the caller */
      if (*resp == 0)
      {
        *resp = calloc(sizeof(struct pam_response), n);
        if (*resp == 0)
          return PAM_BUF_ERR;
      }

      /* dialog plugin interprets the first byte of the packet
         as the magic number.
           2 means "read the input with the echo enabled"
           4 means "password-like input, echo disabled"
         C'est la vie. */
      param->buf[0] = msg[i]->msg_style == PAM_PROMPT_ECHO_ON ? 2 : 4;
      PAM_DEBUG((stderr, "PAM: conv: send(%.*s)\n",
                (int)(param->ptr - param->buf - 1), param->buf));
      pkt_len= roundtrip(param, param->buf, param->ptr - param->buf - 1, &pkt);
      if (pkt_len < 0)
        return PAM_CONV_ERR;

      PAM_DEBUG((stderr, "PAM: conv: recv(%.*s)\n", pkt_len, pkt));
      /* allocate and copy the reply to the response array */
      if (!((*resp)[i].resp= strndup((char*) pkt, pkt_len)))
        return PAM_CONV_ERR;
      param->ptr = param->buf + 1;
    }
  }
  return PAM_SUCCESS;
}

#define DO(X) if ((status = (X)) != PAM_SUCCESS) goto end

#if defined(SOLARIS) || defined(__sun)
typedef void** pam_get_item_3_arg;
#else
typedef const void** pam_get_item_3_arg;
#endif

static int pam_auth_base(struct param *param, MYSQL_SERVER_AUTH_INFO *info)
{
  pam_handle_t *pamh = NULL;
  int status;
  const char *new_username= NULL;
  /* The following is written in such a way to make also solaris happy */
  struct pam_conv pam_start_arg = { &conv, (char*) param };

  /*
    get the service name, as specified in

     CREATE USER ... IDENTIFIED WITH pam AS "service"
  */
  const char *service = info->auth_string && info->auth_string[0]
                          ? info->auth_string : "mysql";

  param->ptr = param->buf + 1;

  PAM_DEBUG((stderr, "PAM: pam_start(%s, %s)\n", service, info->user_name));
  DO( pam_start(service, info->user_name, &pam_start_arg, &pamh) );

  PAM_DEBUG((stderr, "PAM: pam_authenticate(0)\n"));
  DO( pam_authenticate (pamh, 0) );

  PAM_DEBUG((stderr, "PAM: pam_acct_mgmt(0)\n"));
  DO( pam_acct_mgmt(pamh, 0) );

  PAM_DEBUG((stderr, "PAM: pam_get_item(PAM_USER)\n"));
  DO( pam_get_item(pamh, PAM_USER, (pam_get_item_3_arg) &new_username) );

  if (new_username &&
      (winbind_hack ? strcasecmp : strcmp)(new_username, info->user_name))
    strncpy(info->authenticated_as, new_username,
            sizeof(info->authenticated_as));
  info->authenticated_as[sizeof(info->authenticated_as)-1]= 0;

end:
  PAM_DEBUG((stderr, "PAM: status = %d (%s) user = %s\n",
             status, pam_strerror(pamh, status), info->authenticated_as));
  pam_end(pamh, status);
  return status == PAM_SUCCESS ? CR_OK : CR_ERROR;
}

