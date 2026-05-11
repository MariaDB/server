/*
   Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "mysql_roles.h"
#include "mysql_translate.h"
#include <m_string.h>

/* A role name is at most 128 characters: 100 bytes and a suffix always fit */
static constexpr size_t MAX_ROLE_BASE_LEN= 100;

/* The role the importer runs as, see dump_all_users_roles_and_grants() */
static const char import_role[]= "mariadb_dump_import_role";


static const uchar *name_key(const void *entry, size_t *length, my_bool)
{
  *length= strlen(static_cast<const char*>(entry));
  return static_cast<const uchar*>(entry);
}


[[noreturn]] static void oom()
{
  die(EX_EOM, "Couldn't allocate memory for the role names");
}


Mysql_roles::Mysql_roles() : n_roles(0), counter(0)
{
  init_alloc_root(PSI_NOT_INSTRUMENTED, &root, 8192, 0, MYF(0));
  /* Role names are case-sensitive, except PUBLIC and NONE (unique_name) */
  if (my_hash_init(PSI_NOT_INSTRUMENTED, &names, &my_charset_bin, 16, 0, 0,
                   name_key, 0, 0) ||
      my_init_dynamic_array(PSI_NOT_INSTRUMENTED, &roles, sizeof(Mysql_role*),
                            16, 16, MYF(0)))
    oom();
  take_name(import_role);
}


Mysql_roles::~Mysql_roles()
{
  delete_dynamic(&roles);
  my_hash_free(&names);
  free_root(&root, MYF(0));
}


void *Mysql_roles::alloc(size_t size)
{
  void *res= alloc_root(&root, size);
  if (!res)
    oom();
  return res;
}


char *Mysql_roles::strdup(const char *s)
{
  char *res= strdup_root(&root, s);
  if (!res)
    oom();
  return res;
}


void Mysql_roles::take_name(const char *name)
{
  if (my_hash_insert(&names, reinterpret_cast<const uchar*>(strdup(name))))
    oom();
}


char *Mysql_roles::quote_literal(const char *s)
{
  char *res, *to;
  to= res= static_cast<char*>(alloc(2*strlen(s) + 3));
  *to++= '\'';
  for (; *s; s++)
  {
    if (*s == '*' && s[1] == '/')
    {
      *to++= '*';
      *to++= '\\';
      continue;
    }
    if (*s == '\'' || *s == '\\')
      *to++= *s;
    *to++= *s;
  }
  *to++= '\'';
  *to= 0;
  return res;
}


char *Mysql_roles::quote_account(const char *user, const char *host)
{
  char *quoted_user= quote_literal(user);
  char *quoted_host= quote_literal(host);
  size_t len= strlen(quoted_user) + strlen(quoted_host) + 2;
  char *account= static_cast<char*>(alloc(len));
  my_snprintf(account, len, "%s@%s", quoted_user, quoted_host);
  return account;
}


const char *Mysql_roles::unique_name(const char *base, bool plain_ok)
{
  size_t blen= strlen(base);
  if (!strcasecmp(base, "PUBLIC") || !strcasecmp(base, "NONE"))
    plain_ok= false;
  if (blen > MAX_ROLE_BASE_LEN)
  {
    /* Leave room for the suffix, and cut at a character boundary */
    blen= MAX_ROLE_BASE_LEN;
    while (blen && (static_cast<uchar>(base[blen]) & 0xC0) == 0x80)
      blen--;
    char *cut= static_cast<char*>(alloc(blen + 1));
    memcpy(cut, base, blen);
    cut[blen]= 0;
    base= cut;
    plain_ok= false;
  }
  size_t len= blen + 16;
  char *name= static_cast<char*>(alloc(len));

  if (plain_ok && !my_hash_search(&names, reinterpret_cast<const uchar*>(base),
                                  strlen(base)))
    strcpy(name, base);
  else
    do
      my_snprintf(name, len, "%s_%04u", base, ++counter);
    while (my_hash_search(&names, reinterpret_cast<const uchar*>(name),
                          strlen(name)));
  take_name(name);
  return name;
}


char *Mysql_roles::default_roles_name(const char *user)
{
  size_t len= strlen(user) + sizeof("_default_roles");
  char *base= static_cast<char*>(alloc(len));
  my_snprintf(base, len, "%s_default_roles", user);
  return quote_literal(unique_name(base, true));
}


void Mysql_roles::add(const char *user, const char *host, bool mandatory,
                      bool locked)
{
  Mysql_role *r= static_cast<Mysql_role*>(alloc(sizeof(Mysql_role)));
  r->user= strdup(user);
  r->host= strdup(host);
  r->mandatory= mandatory;
  r->locked= locked;
  r->account= quote_account(user, host);
  const char *name= unique_name(user, strcmp(host, "%") == 0);
  r->renamed= strcmp(name, user) != 0;
  r->name= quote_literal(name);
  if (insert_dynamic(&roles, &r))
    oom();
  n_roles++;
}


const Mysql_role *Mysql_roles::find(const char *user, const char *host) const
{
  for (uint i= 0; i < n_roles; i++)
  {
    const Mysql_role &r= (*this)[i];
    if (strcmp(r.user, user) == 0 && strcmp(r.host, host) == 0)
      return &r;
  }
  return NULL;
}
