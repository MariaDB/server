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

#ifndef MYSQL_ROLES_INCLUDED
#define MYSQL_ROLES_INCLUDED

#include <my_global.h>
#include <my_sys.h>
#include <hash.h>

/*
  MySQL has no is_role column: an account is a user, a role, or both (a login
  account granted to others). Every account is dumped as a MariaDB user (a pure
  role is a locked one). A role is an account granted to another account
  (role_edges.FROM) or a mandatory one. It becomes a MariaDB role of its own,
  named after the account's user part, or user_NNNN if the host is not '%' or
  the name is taken (PUBLIC, NONE and mariadb_dump_import_role are never
  available; a long name is cut and suffixed): a MariaDB role has no host, and
  two accounts may share a user part. The privileges of the account are
  granted to its role, which its members get, and, unless it is locked, to the
  account itself: the role is not its default role, so the roles granted to a
  login account (a hybrid) stay inactive until enabled, as on MySQL.
*/
struct Mysql_role
{
  const char *user, *host;      /* the account, as in mysql.user */
  const char *account;          /* the account, 'user'@'host' */
  const char *name;             /* the MariaDB role, quoted */
  bool renamed;                 /* name is not the user part */
  bool mandatory;
  bool locked;                  /* account_locked: nobody logs in as it */
};


/* The MySQL roles of a dump, and the MariaDB role names taken so far */
class Mysql_roles
{
public:
  Mysql_roles();
  ~Mysql_roles();
  Mysql_roles(const Mysql_roles &)= delete;
  Mysql_roles &operator=(const Mysql_roles &)= delete;

  /* Add the role of an account, naming it */
  void add(const char *user, const char *host, bool mandatory, bool locked);
  const Mysql_role *find(const char *user, const char *host) const;
  uint count() const { return n_roles; }
  const Mysql_role &operator[](uint i) const
  { return **dynamic_element(&roles, i, Mysql_role**); }

  /*
    A free role name: base if plain_ok and free, else base_NNNN. The name is
    then taken. PUBLIC and NONE, in any letter case, are never used as is.
  */
  const char *unique_name(const char *base, bool plain_ok);
  /* The quoted role of the several default roles of user */
  char *default_roles_name(const char *user);

  /*
    s as a '' literal. A comment end is backslash-escaped to be safe in a
    version comment.
  */
  char *quote_literal(const char *s);
  /* 'user'@'host' */
  char *quote_account(const char *user, const char *host);

  /* Memory that lives as long as the roles; dies on failure */
  void *alloc(size_t size);
  char *strdup(const char *s);

private:
  void take_name(const char *name);

  MEM_ROOT root;
  HASH names;                   /* the role names taken */
  DYNAMIC_ARRAY roles;          /* Mysql_role* */
  uint n_roles;
  uint counter;                 /* the last NNNN of user_NNNN */
};

#endif
