/*
  Pam module to change user names arbitrarily in the pam stack.

  Compile as

     gcc pam_user_map.c -shared -lpam -fPIC -o pam_user_map.so

  Install as appropriate (for example, in /lib/security/).
  Add to your /etc/pam.d/mysql (preferably, at the end) this line:
=========================================================
auth            required        pam_user_map.so
=========================================================

  And create /etc/security/user_map.conf with the desired mapping
  in the format:  orig_user_name: mapped_user_name
                  @user's_group_name: mapped_user_name
=========================================================
#comments and empty lines are ignored
john: jack
bob:  admin
top:  accounting
@group_ro: readonly
=========================================================

If something doesn't work as expected you can get verbose
comments with the 'debug' option like this
=========================================================
auth            required        pam_user_map.so debug
=========================================================
These comments are written to the syslog as 'authpriv.debug'
and usually end up in /var/log/secure file.
*/

#include <config_auth_pam.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <grp.h>
#include <pwd.h>

#ifdef HAVE_PAM_EXT_H
#include <security/pam_ext.h>
#endif

#ifdef HAVE_PAM_APPL_H
#include <unistd.h>
#include <security/pam_appl.h>
#endif

#include <security/pam_modules.h>

#ifndef HAVE_PAM_SYSLOG
#include <stdarg.h>
static void
pam_syslog (const pam_handle_t *pamh, int priority,
      const char *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  vsyslog (priority, fmt, args);
  va_end (args);
}
#endif

#define FILENAME "/etc/security/user_map.conf"
#define skip(what) while (*s && (what)) s++
#define SYSLOG_DEBUG if (mode_debug) pam_syslog

#define GROUP_BUFFER_SIZE 100
static const char debug_keyword[]= "debug";

#ifdef HAVE_POSIX_GETGROUPLIST
typedef gid_t my_gid_t;
#else
typedef int my_gid_t;
#endif

static int populate_user_groups(const char *user, my_gid_t **groups)
{
  my_gid_t user_group_id;
  my_gid_t *loc_groups= *groups;
  int ng;

  {
    struct passwd *pw= getpwnam(user);
    if (!pw)
      return 0;
    user_group_id= pw->pw_gid;
  }

  ng= GROUP_BUFFER_SIZE;
  if (getgrouplist(user, user_group_id, loc_groups, &ng) < 0)
  {
    /* The rare case when the user is present in more than */
    /* GROUP_BUFFER_SIZE groups.                           */
    loc_groups= (my_gid_t *) malloc(ng * sizeof (my_gid_t));

    if (!loc_groups)
      return 0;

    (void) getgrouplist(user, user_group_id, loc_groups, &ng);
    *groups= (my_gid_t*)loc_groups;
  }

  return ng;
}


static int user_in_group(const my_gid_t *user_groups, int ng,const char *group)
{
  my_gid_t group_id;
  const my_gid_t *groups_end = user_groups + ng;

  {
    struct group *g= getgrnam(group);
    if (!g)
      return 0;
    group_id= g->gr_gid;
  }

  for (; user_groups < groups_end; user_groups++)
  {
    if (*user_groups == group_id)
      return 1;
  }

  return 0;
}


static void print_groups(pam_handle_t *pamh, const my_gid_t *user_groups, int ng)
{
  char buf[256];
  char *c_buf= buf, *buf_end= buf+sizeof(buf)-2;
  struct group *gr;
  int cg;

  for (cg=0; cg < ng; cg++)
  {
    char *c;
    if (c_buf == buf_end)
      break;
    *(c_buf++)= ',';
    if (!(gr= getgrgid(user_groups[cg])) ||
        !(c= gr->gr_name))
      continue;
    while (*c)
    {
      if (c_buf == buf_end)
        break;
      *(c_buf++)= *(c++);
    }
  }
  c_buf[0]= c_buf[1]= 0;
  pam_syslog(pamh, LOG_DEBUG, "User belongs to %d %s [%s].\n",
                                 ng, (ng == 1) ? "group" : "groups", buf+1);
}

int pam_sm_authenticate(pam_handle_t *pamh, int flags,
    int argc, const char *argv[])
{
  int mode_debug= 0;
  int pam_err, line= 0;
  const char *username;
  char buf[256];
  FILE *f;
  my_gid_t group_buffer[GROUP_BUFFER_SIZE];
  my_gid_t *groups= group_buffer;
  int n_groups= -1;

  for (; argc > 0; argc--) 
  {
    if (strcasecmp(argv[argc-1], debug_keyword) == 0)
      mode_debug= 1;
  }

  SYSLOG_DEBUG(pamh, LOG_DEBUG, "Opening file '%s'.\n", FILENAME);

  f= fopen(FILENAME, "r");
  if (f == NULL)
  {
    pam_syslog(pamh, LOG_ERR, "Cannot open '%s'\n", FILENAME);
    return PAM_SYSTEM_ERR;
  }

  pam_err = pam_get_item(pamh, PAM_USER, (const void**)&username);
  if (pam_err != PAM_SUCCESS)
  {
    pam_syslog(pamh, LOG_ERR, "Cannot get username.\n");
    goto ret;
  }

  SYSLOG_DEBUG(pamh, LOG_DEBUG, "Incoming username '%s'.\n", username);

  while (fgets(buf, sizeof(buf), f) != NULL)
  {
    char *s= buf, *from, *to, *end_from, *end_to;
    int check_group;
    int cmp_result;

    line++;

    skip(isspace(*s));
    if (*s == '#' || *s == 0) continue;
    if ((check_group= *s == '@'))
    {
      if (n_groups < 0)
      {
        n_groups= populate_user_groups(username, &groups);
        if (mode_debug)
          print_groups(pamh, groups, n_groups);
      }
      s++;
    }
    from= s;
    skip(isalnum(*s) || (*s == '_') || (*s == '.') || (*s == '-') ||
         (*s == '$') || (*s == '\\') || (*s == '/'));
    end_from= s;
    skip(isspace(*s));
    if (end_from == from || *s++ != ':') goto syntax_error;
    skip(isspace(*s));
    to= s;
    skip(isalnum(*s) || (*s == '_') || (*s == '.') || (*s == '-') ||
         (*s == '$'));
    end_to= s;
    if (end_to == to) goto syntax_error;

    *end_from= *end_to= 0;

    if (check_group)
    {
      cmp_result= user_in_group(groups, n_groups, from);
      SYSLOG_DEBUG(pamh, LOG_DEBUG, "Check if user is in group '%s': %s\n",
                                    from, cmp_result ? "YES":"NO");
    }
    else
    {
      cmp_result= (strcmp(username, from) == 0);
      SYSLOG_DEBUG(pamh, LOG_DEBUG, "Check if username '%s': %s\n",
                                    from, cmp_result ? "YES":"NO");
    }
    if (cmp_result)
    {
      pam_err= pam_set_item(pamh, PAM_USER, to);
      SYSLOG_DEBUG(pamh, LOG_DEBUG, 
          (pam_err == PAM_SUCCESS) ? "User mapped as '%s'\n" :
                                     "Couldn't map as '%s'\n", to);
      goto ret;
    }
  }

  SYSLOG_DEBUG(pamh, LOG_DEBUG, "User not found in the list.\n");
  pam_err= PAM_AUTH_ERR;
  goto ret;

syntax_error:
  pam_syslog(pamh, LOG_ERR, "Syntax error at %s:%d", FILENAME, line);
  pam_err= PAM_SYSTEM_ERR;
ret:
  if (groups != group_buffer)
    free(groups);

  fclose(f);

  return pam_err;
}


int pam_sm_setcred(pam_handle_t *pamh, int flags,
                   int argc, const char *argv[])
{

    return PAM_SUCCESS;
}

