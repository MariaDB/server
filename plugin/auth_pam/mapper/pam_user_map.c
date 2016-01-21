/*
  Pam module to change user names arbitrarily in the pam stack.

  Compile as
  
     gcc pam_user_map.c -shared -lpam -fPIC -o pam_user_map.so

  Install as appropriate (for example, in /lib/security/).
  Add to your /etc/pam.d/mysql (preferrably, at the end) this line:
=========================================================
auth            required        pam_user_map.so
=========================================================

  And create /etc/security/user_map.conf with the desired mapping
  in the format:  orig_user_name: mapped_user_name
                  @user's_group_name: mapped_user_name
=========================================================
#comments and emtpy lines are ignored
john: jack
bob:  admin
top:  accounting
@group_ro: readonly
=========================================================

*/

#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <grp.h>
#include <pwd.h>

#include <security/pam_modules.h>

#define FILENAME "/etc/security/user_map.conf"
#define skip(what) while (*s && (what)) s++

#define GROUP_BUFFER_SIZE 100


static int populate_user_groups(const char *user, gid_t **groups)
{
  gid_t user_group_id;
  gid_t *loc_groups= *groups;
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
    loc_groups= (gid_t *) malloc(ng * sizeof (gid_t));
    if (!loc_groups)
      return 0;

    (void) getgrouplist(user, user_group_id, loc_groups, &ng);
    *groups= loc_groups;
  }

  return ng;
}


static int user_in_group(const gid_t *user_groups, int ng,const char *group)
{
  gid_t group_id;

  {
    struct group *g= getgrnam(group);
    if (!g)
      return 0;
    group_id= g->gr_gid;
  }

  for (; user_groups < user_groups + ng; user_groups++)
  {
    if (*user_groups == group_id)
      return 1;
  }

  return 0;
}


int pam_sm_authenticate(pam_handle_t *pamh, int flags,
    int argc, const char *argv[])
{
  int pam_err, line= 0;
  const char *username;
  char buf[256];
  FILE *f;
  gid_t group_buffer[GROUP_BUFFER_SIZE];
  gid_t *groups= group_buffer;
  int n_groups= -1;

  f= fopen(FILENAME, "r");
  if (f == NULL)
  {
    pam_syslog(pamh, LOG_ERR, "Cannot open '%s'\n", FILENAME);
    return PAM_SYSTEM_ERR;
  }

  pam_err = pam_get_item(pamh, PAM_USER, (const void**)&username);
  if (pam_err != PAM_SUCCESS)
    goto ret;

  while (fgets(buf, sizeof(buf), f) != NULL)
  {
    char *s= buf, *from, *to, *end_from, *end_to;
    int check_group;

    line++;

    skip(isspace(*s));
    if (*s == '#' || *s == 0) continue;
    if ((check_group= *s == '@'))
    {
      if (n_groups < 0)
        n_groups= populate_user_groups(username, &groups);
      s++;
    }
    from= s;
    skip(isalnum(*s) || (*s == '_') || (*s == '.') || (*s == '-') || (*s == '$'));
    end_from= s;
    skip(isspace(*s));
    if (end_from == from || *s++ != ':') goto syntax_error;
    skip(isspace(*s));
    to= s;
    skip(isalnum(*s) || (*s == '_') || (*s == '.') || (*s == '-') || (*s == '$'));
    end_to= s;
    if (end_to == to) goto syntax_error;

    *end_from= *end_to= 0;
    if (check_group ?
          user_in_group(groups, n_groups, from) :
          (strcmp(username, from) == 0))
    {
      pam_err= pam_set_item(pamh, PAM_USER, to);
      goto ret;
    }
  }
  pam_err= PAM_SUCCESS;
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

