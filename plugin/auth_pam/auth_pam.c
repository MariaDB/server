/*
   Copyright (c) 2011, 2020, MariaDB Corporation.

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


#include <my_global.h>
#include <config_auth_pam.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <spawn.h>
#include <mysql/plugin_auth.h>
#include "auth_pam_tool.h"

#ifndef DBUG_OFF
static char pam_debug = 0;
#define PAM_DEBUG(X)   do { if (pam_debug) { fprintf X; } } while(0)
#else
#define PAM_DEBUG(X)   /* no-op */
#endif

static char winbind_hack = 0;

static char *opt_plugin_dir; /* To be dynamically linked. */
static const char *tool_name= "auth_pam_tool_dir/auth_pam_tool";
static const int tool_name_len= 31;

/*
  sleep_limit is now 5 meaning up to 1 second sleep.
  each step means 10 times longer sleep, so 6 would mean 10 seconds.
*/
static const unsigned int sleep_limit= 5;

static int pam_auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  int p_to_c[2], c_to_p[2]; /* Parent-to-child and child-to-parent pipes. */
  pid_t proc_id;
  int result= CR_ERROR, pkt_len= 0;
  unsigned char field, *pkt;
  unsigned int n_sleep= 0;
  useconds_t sleep_time= 100;
  posix_spawn_file_actions_t file_actions;
  char toolpath[FN_REFLEN];
  size_t plugin_dir_len= strlen(opt_plugin_dir);
  char *const argv[2]= {toolpath, 0};
  int res;

  PAM_DEBUG((stderr, "PAM: opening pipes.\n"));
  if (pipe(p_to_c) < 0 || pipe(c_to_p) < 0)
  {
    my_printf_error(ENOEXEC, "pam: cannot create pipes (errno: %M)",
                    ME_ERROR_LOG_ONLY, errno);
    return CR_ERROR;
  }

  if (plugin_dir_len + tool_name_len + 2 > sizeof(toolpath))
  {
    my_printf_error(ENOEXEC, "pam: too long path to <plugindir>/%s",
                    ME_ERROR_LOG_ONLY, tool_name);
    return CR_ERROR;
  }

  memcpy(toolpath, opt_plugin_dir, plugin_dir_len);
  if (plugin_dir_len && toolpath[plugin_dir_len-1] != FN_LIBCHAR)
    toolpath[plugin_dir_len++]= FN_LIBCHAR;
  memcpy(toolpath+plugin_dir_len, tool_name, tool_name_len+1);

  PAM_DEBUG((stderr, "PAM: forking %s\n", toolpath));
  res= posix_spawn_file_actions_init(&file_actions) ||
       posix_spawn_file_actions_addclose(&file_actions, p_to_c[1]) ||
       posix_spawn_file_actions_addclose(&file_actions, c_to_p[0]) ||
       posix_spawn_file_actions_adddup2(&file_actions, p_to_c[0], 0) ||
       posix_spawn_file_actions_adddup2(&file_actions, c_to_p[1], 1) ||
       posix_spawn(&proc_id, toolpath, &file_actions, NULL, argv, NULL);

  /* Parent process continues. */
  posix_spawn_file_actions_destroy(&file_actions);
  close(p_to_c[0]);
  close(c_to_p[1]);

  if (res)
  {
    my_printf_error(ENOEXEC, "pam: cannot exec %s (errno: %M)",
                    ME_ERROR_LOG_ONLY, toolpath, errno);
    goto error_ret;
  }

  /* no user name yet ? read the client handshake packet with the user name */
  if (info->user_name == 0)
  {
    if ((pkt_len= vio->read_packet(vio, &pkt)) < 0)
      goto error_ret;
  }
  else
    pkt= NULL;

  PAM_DEBUG((stderr, "PAM: parent sends user data [%s], [%s].\n",
               info->user_name, info->auth_string));

#ifndef DBUG_OFF
  field= pam_debug ? 1 : 0;
#else
  field= 0;
#endif
  field|= winbind_hack ? 2 : 0;

  if (write(p_to_c[1], &field, 1) != 1 ||
      write_string(p_to_c[1], (const uchar *) info->user_name,
                                       info->user_name_length) ||
      write_string(p_to_c[1], (const uchar *) info->auth_string,
                                      info->auth_string_length))
    goto error_ret;

  for (;;)
  {
    PAM_DEBUG((stderr, "PAM: listening to the sandbox.\n"));
    if (read(c_to_p[0], &field, 1) < 1)
    {
      PAM_DEBUG((stderr, "PAM: read failed.\n"));
      goto error_ret;
    }

    if (field == AP_EOF)
    {
      PAM_DEBUG((stderr, "PAM: auth OK returned.\n"));
      break;
    }

    switch (field)
    {
    case AP_AUTHENTICATED_AS:
      PAM_DEBUG((stderr, "PAM: reading authenticated_as string.\n"));
      if (read_string(c_to_p[0], info->authenticated_as,
                      sizeof(info->authenticated_as) - 1) < 0)
        goto error_ret;
      break;

    case AP_CONV:
      {
        unsigned char buf[10240];
        int buf_len;

        PAM_DEBUG((stderr, "PAM: getting CONV string.\n"));
        if ((buf_len= read_string(c_to_p[0], (char *) buf, sizeof(buf))) < 0)
          goto error_ret;

        if (!pkt || !*pkt || (buf[0] >> 1) != 2)
        {
          PAM_DEBUG((stderr, "PAM: sending CONV string.\n"));
          if (vio->write_packet(vio, buf, buf_len))
            goto error_ret;

          PAM_DEBUG((stderr, "PAM: reading CONV answer.\n"));
          if ((pkt_len= vio->read_packet(vio, &pkt)) < 0)
            goto error_ret;
        }

        PAM_DEBUG((stderr, "PAM: answering CONV.\n"));
        if (write_string(p_to_c[1], pkt, pkt_len))
          goto error_ret;

        pkt= NULL;
      }
      break;

    default:
      PAM_DEBUG((stderr, "PAM: unknown sandbox field.\n"));
      goto error_ret;
    }
  }
  result= CR_OK;

error_ret:
  close(p_to_c[1]);
  close(c_to_p[0]);
  while (waitpid(proc_id, NULL, WNOHANG) != (int) proc_id)
  {
    if (n_sleep++ == sleep_limit)
    {
      /*
        The auth_pam_tool application doesn't terminate.
        Means something wrong happened there like pam_xxx.so hanged.
      */
      kill(proc_id, SIGKILL);
      sleep_time= 1000000; /* 1 second wait should be enough. */
      PAM_DEBUG((stderr, "PAM: auth_pam_tool doesn't terminate,"
                         " have to kill it.\n"));
    }
    else if (n_sleep > sleep_limit)
      break;
    usleep(sleep_time);
    sleep_time*= 10;
  }

  PAM_DEBUG((stderr, "PAM: auth result %d.\n", result));
  return result;
}


#include "auth_pam_common.c"


static int init(void *p __attribute__((unused)))
{
  if (use_cleartext_plugin)
    info.client_auth_plugin= "mysql_clear_password";
  if (!(opt_plugin_dir= dlsym(RTLD_DEFAULT, "opt_plugin_dir")))
    return 1;
  return 0;
}

maria_declare_plugin(pam)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &info,
  "pam",
  "MariaDB Corp",
  "PAM based authentication",
  PLUGIN_LICENSE_GPL,
  init,
  NULL,
  0x0200,
  NULL,
  vars,
  "2.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
