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

#include "mysql_translate.h"
#include <my_sys.h>
#include <m_string.h>

/* Die if a DYNAMIC_STRING call failed to allocate. */
static void check_alloc(bool failed)
{
  if (failed)
    die(EX_EOM, "Couldn't allocate memory");
}

static void append(DYNAMIC_STRING *ds, const char *str)
{
  check_alloc(dynstr_append(ds, str));
}

static void append_mem(DYNAMIC_STRING *ds, const char *str, size_t len)
{
  check_alloc(dynstr_append_mem(ds, str, uint(len)));
}

/*
  MySQL privileges that map to different MariaDB ones. Names in neither this
  table nor mariadb_privs[] are dropped with a # WARNING.

  ALLOW_NONEXISTENT_DEFINER and SERVICE_CONNECTION_ADMIN are not mapped: the
  nearest MariaDB privileges (SET USER, CONNECTION ADMIN) grant more.
*/
struct Mysql_priv_map_entry
{
  const char *mysql_priv;
  size_t len;
  const char *mariadb_priv[2];
};
static const struct Mysql_priv_map_entry mysql_priv_map[]=
{
  {STRING_WITH_LEN("BINLOG_ADMIN"),        {"BINLOG REPLAY", "BINLOG ADMIN"}},
  {STRING_WITH_LEN("CONNECTION_ADMIN"),    {"CONNECTION ADMIN", NULL}},
  {STRING_WITH_LEN("REPLICATION_APPLIER"), {"BINLOG REPLAY", NULL}},
  {STRING_WITH_LEN("REPLICATION_SLAVE_ADMIN"),
                                           {"REPLICATION SLAVE ADMIN", NULL}},
  {STRING_WITH_LEN("SET_ANY_DEFINER"),     {"SET USER", NULL}},
  {STRING_WITH_LEN("SET_USER_ID"),         {"SET USER", NULL}},
  {STRING_WITH_LEN("SHOW_ROUTINE"),        {"SHOW CREATE ROUTINE", NULL}}
};

/*
  Skip the quoted token at *p (' " or `): doubled quotes, and backslash
  escapes except in backticks. Returns the position after the closing quote,
  or the end of string if unterminated.
*/
static const char *skip_quoted(const char *p)
{
  char q= *p++;
  while (*p)
  {
    if (*p == '\\' && p[1] && q != '`')
      p+= 2;
    else if (*p == q)
    {
      if (p[1] == q)
        p+= 2;
      else
        return p + 1;
    }
    else
      p++;
  }
  return p;
}

/*
  Read one name at *p: quoted with ' " or ` (a doubled quote stands for
  itself), or bare up to a comma or '@'. Moves *p past it; the name is set in
  `name`.
*/
static void read_account_part(const char **p, DYNAMIC_STRING *name)
{
  check_alloc(dynstr_set(name, ""));
  while (my_isspace(&my_charset_latin1, **p))
    (*p)++;
  char q= **p;
  if (q == '\'' || q == '"' || q == '`')
  {
    for ((*p)++; **p; (*p)++)
    {
      if (**p == q && (*p)[1] != q)
        break;
      if (**p == q)
        (*p)++;
      append_mem(name, *p, 1);
    }
    if (**p)
      (*p)++;
  }
  else
  {
    const char *start= *p;
    while (**p && **p != ',' && **p != '@')
      (*p)++;
    size_t len= *p - start;
    while (len && my_isspace(&my_charset_latin1, start[len - 1]))
      len--;
    append_mem(name, start, len);
  }
}


void parse_mysql_account_list(const char *list,
                              void (*add)(const char *user, const char *host,
                                          void *arg),
                              void *arg)
{
  DYNAMIC_STRING user, host;
  check_alloc(init_dynamic_string(&user, "", 64, 64));
  check_alloc(init_dynamic_string(&host, "", 64, 64));
  for (const char *p= list; *p; )
  {
    while (my_isspace(&my_charset_latin1, *p))
      p++;
    if (!*p)
      break;
    if (*p == ',')
    {
      p++;
      continue;
    }
    read_account_part(&p, &user);
    check_alloc(dynstr_set(&host, "%"));
    while (my_isspace(&my_charset_latin1, *p))
      p++;
    if (*p == '@')
    {
      p++;
      read_account_part(&p, &host);
    }
    add(user.str, host.str, arg);
    while (*p && *p != ',')
      p++;
  }
  dynstr_free(&user);
  dynstr_free(&host);
}


/*
  Find the first position in [start, end) where match(p) is true, outside
  quoted and parenthesised regions (the column lists of a GRANT, as in
  SELECT (a, b)).
*/
template <class Match>
static const char *scan_top_level(const char *start, const char *end,
                                  Match match)
{
  int depth= 0;
  for (const char *p= start; p < end; )
  {
    if (*p == '\'' || *p == '"' || *p == '`') { p= skip_quoted(p); continue; }
    if (*p == '(') { depth++; p++; continue; }
    if (*p == ')') { depth--; p++; continue; }
    if (depth == 0 && match(p))
      return p;
    p++;
  }
  return NULL;
}

/* The first top-level keyword in the string; it must begin with a space. */
static const char *scan_keyword(const char *start, const LEX_CSTRING *kw)
{
  DBUG_ASSERT(kw->str[0] == ' ');
  return scan_top_level(start, start + strlen(start), [kw](const char *p)
  { return strncmp(p, kw->str, kw->length) == 0; });
}

static const struct Mysql_priv_map_entry *
lookup_mysql_priv(const char *name, size_t len)
{
  for (size_t i= 0; i < array_elements(mysql_priv_map); i++)
    if (mysql_priv_map[i].len == len &&
        memcmp(mysql_priv_map[i].mysql_priv, name, len) == 0)
      return &mysql_priv_map[i];
  return NULL;
}

/* Trim whitespace around [*s, e): advance *s, return the length. */
static size_t trim_priv(const char **s, const char *e)
{
  while (*s < e && my_isspace(&my_charset_latin1, **s)) (*s)++;
  while (e > *s && my_isspace(&my_charset_latin1, e[-1])) e--;
  return size_t(e - *s);
}


/* The translated privilege list of one GRANT. */
struct Priv_state
{
  DYNAMIC_STRING privs;
  /* Mapped privileges already in `privs`: several MySQL ones map to the same
     (BINLOG_ADMIN and REPLICATION_APPLIER both give BINLOG REPLAY). */
  const char *mapped[array_elements(mysql_priv_map) *
                     array_elements(mysql_priv_map[0].mariadb_priv)];
  uint n_mapped;
  FILE *out;                            /* "# WARNING" lines; NULL: none */
  DYNAMIC_STRING context;               /* what a warning says it is about */
  DYNAMIC_STRING dropped;               /* the dropped privileges, listed */
};

/* on_clause is " ON ..." to the end of the statement; role_name may be NULL */
static void init_priv_state(struct Priv_state *st, FILE *out,
                            const char *on_clause, const char *role_name)
{
  check_alloc(init_dynamic_string(&st->privs, "", 256, 1024));
  check_alloc(init_dynamic_string(&st->dropped, "", 64, 256));
  check_alloc(init_dynamic_string(&st->context, "", 128, 128));
  append(&st->context, on_clause);
  if (role_name)
  {
    append(&st->context, ", role ");
    append(&st->context, role_name);
  }
  st->n_mapped= 0;
  st->out= out;
}

static void free_priv_state(struct Priv_state *st)
{
  dynstr_free(&st->privs);
  dynstr_free(&st->dropped);
  dynstr_free(&st->context);
}

/* One warning for the privileges dropped from a statement */
static void warn_dropped(const struct Priv_state *st)
{
  if (st->out && st->dropped.length)
    dump_emit_comment_warning(st->out, "WARNING: dropped MySQL privilege(s) "
                              "%s (no MariaDB equivalent):%s",
                              st->dropped.str, st->context.str);
}

static void append_priv(struct Priv_state *st, const char *tok, size_t len)
{
  if (st->privs.length)
    append(&st->privs, ", ");
  append_mem(&st->privs, tok, len);
}

static void append_mapped_priv(struct Priv_state *st, const char *tok)
{
  for (uint i= 0; i < st->n_mapped; i++)
    if (strcmp(st->mapped[i], tok) == 0)
      return;
  DBUG_ASSERT(st->n_mapped < array_elements(st->mapped));
  st->mapped[st->n_mapped++]= tok;
  append_priv(st, tok, strlen(tok));
}


/* Privileges MariaDB accepts as they are, in a GRANT or DENY. */
static const char *const mariadb_privs[]=
{
  "ALL", "ALL PRIVILEGES", "ALTER", "ALTER ROUTINE", "BINLOG ADMIN",
  "BINLOG MONITOR", "BINLOG REPLAY", "CONNECTION ADMIN", "CREATE",
  "CREATE ROUTINE", "CREATE TABLESPACE", "CREATE TEMPORARY TABLES",
  "CREATE USER", "CREATE VIEW",
  "DELETE", "DELETE HISTORY", "DROP", "EVENT", "EXECUTE", "FEDERATED ADMIN",
  "FILE", "GRANT OPTION", "INDEX", "INSERT", "LOCK TABLES", "PROCESS",
  "PROXY", "READ_ONLY ADMIN", "REFERENCES", "RELOAD", "REPLICA MONITOR",
  "REPLICATION CLIENT", "REPLICATION MASTER ADMIN", "REPLICATION SLAVE",
  "REPLICATION SLAVE ADMIN", "SELECT", "SET USER", "SHOW CREATE ROUTINE",
  "SHOW DATABASES", "SHOW VIEW", "SHUTDOWN", "SLAVE MONITOR", "SUPER",
  "TRIGGER", "UPDATE", "USAGE", NULL
};

/* Is [s, s + len) a name in the NULL-terminated list? */
static bool in_list(const char *const *list, const char *s, size_t len)
{
  for (; *list; list++)
    if (strlen(*list) == len && memcmp(*list, s, len) == 0)
      return true;
  return false;
}

/* Is [s, s + len), less a column list, in mariadb_privs? */
static bool is_mariadb_priv(const char *s, size_t len)
{
  const char *paren= static_cast<const char*>(memchr(s, '(', len));
  if (paren)
    len= trim_priv(&s, paren);
  return in_list(mariadb_privs, s, len);
}


/* Add privilege [start, end) to st: as is, mapped or dropped. */
static void process_priv(const char *start, const char *end,
                         struct Priv_state *st)
{
  size_t plen= trim_priv(&start, end);
  if (!plen)
    return;

  const struct Mysql_priv_map_entry *e= lookup_mysql_priv(start, plen);
  if (!e && is_mariadb_priv(start, plen))
  {
    append_priv(st, start, plen);
    return;
  }
  if (!e)
  {
    if (st->dropped.length)
      append(&st->dropped, ", ");
    append_mem(&st->dropped, start, plen);
    return;
  }
  for (uint i= 0; i < array_elements(e->mariadb_priv); i++)
    if (e->mariadb_priv[i])
      append_mapped_priv(st, e->mariadb_priv[i]);
}


/* Split [start, end) at top-level commas and process_priv() each part. */
static void translate_priv_list(const char *start, const char *end,
                                struct Priv_state *st)
{
  auto is_comma= [](const char *p) { return *p == ','; };
  const char *comma;
  for (; (comma= scan_top_level(start, end, is_comma)); start= comma + 1)
    process_priv(start, comma, st);
  process_priv(start, end, st);
}


/*
  Print a single-quoted literal with each star-slash backslash-escaped, so it
  is safe inside a version comment.
*/
void dump_put_comment_safe_literal(FILE *out, const char *literal)
{
  DBUG_ASSERT(literal[0] == '\'');
  for (; *literal; literal++)
  {
    if (literal[0] == '*' && literal[1] == '/')
    {
      fputs("*\\/", out);
      literal++;
    }
    else
      fputc(*literal, out);
  }
}


void dump_put_create_user(FILE *out, bool replace_into, bool ignore)
{
  fputs("CREATE ", out);
  if (replace_into)
    fputs("/*M!100103 OR REPLACE */ ", out);
  fputs("USER", out);
  if (ignore)
    fputs(" IF NOT EXISTS", out);
}


/* Fold CR/LF to a space, keeping a "#" comment on one line. */
static void fold_eol(char *s)
{
  for (; *s; s++)
    if (*s == '\n' || *s == '\r')
      *s= ' ';
}


/* printf() into a my_malloc()'ed string, to be freed with my_free() */
static char *vformat_heap(const char *fmt, va_list args)
{
  va_list copy;
  va_copy(copy, args);
  int len= vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (len < 0)
    die(EX_EOM, "Couldn't format a statement");

  char *buf= static_cast<char*>(my_malloc(PSI_NOT_INSTRUMENTED, len + 1,
                                          MYF(MY_WME)));
  if (!buf)
    die(EX_EOM, "Couldn't allocate memory");
  vsnprintf(buf, len + 1, fmt, args);
  return buf;
}


/*
  Append a "# ..." line to `out`. CR/LF are folded: QUOTE() and backticks do
  not escape them, and the rest of the line would run as SQL.
*/
static void vformat_comment_line(DYNAMIC_STRING *out, const char *fmt,
                                 va_list args)
{
  char *buf= vformat_heap(fmt, args);
  fold_eol(buf);
  append(out, "# ");
  append(out, buf);
  append(out, "\n");
  my_free(buf);
}


void dump_emit_comment_warning(FILE *out, const char *fmt, ...)
{
  DYNAMIC_STRING line;
  va_list args;

  check_alloc(init_dynamic_string(&line, "", 256, 256));
  va_start(args, fmt);
  vformat_comment_line(&line, fmt, args);
  va_end(args);
  fputs(line.str, out);
  dynstr_free(&line);
}


/*
  Print a statement in the version comment `gate`. If it has a comment end (in
  a name or a column list: neither QUOTE() nor backticks escape it), print it
  plain with a warning: an older server rejects it.
*/
static void emit_gated(FILE *out, const char *gate, const char *fmt, ...)
  ATTRIBUTE_FORMAT(printf, 3, 4);
static void emit_gated(FILE *out, const char *gate, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  char *stmt= vformat_heap(fmt, args);
  va_end(args);

  bool safe= !strstr(stmt, "*/");
  if (safe)
    fputs(gate, out);
  else
    fputs("# WARNING: the statement below has no version gate: it has a "
          "comment end, and an older server rejects it\n", out);
  fputs(stmt, out);
  fputs(safe ? " */;\n" : ";\n", out);
  my_free(stmt);
}


/* The parts of a GRANT from SHOW GRANTS, pointing into its text */
struct Grant_parts
{
  const char *text;
  const char *on;               /* " ON ..." */
  const char *as;               /* " AS ..." (partial revokes), or NULL */
  const char *object_end;       /* the end of the object, any " AS " cut off */
  bool with_opt;                /* the object ends in " WITH GRANT OPTION" */
};

static const LEX_CSTRING grant_opt= {STRING_WITH_LEN(" WITH GRANT OPTION")};

/* False if the statement has no " ON ", as a grant of a role */
static bool split_grant(const char *grant, Grant_parts *g)
{
  static const LEX_CSTRING kw_on= {STRING_WITH_LEN(" ON ")};
  static const LEX_CSTRING kw_as= {STRING_WITH_LEN(" AS ")};

  g->text= grant;
  g->on= scan_keyword(grant, &kw_on);
  if (!g->on)
    return false;
  g->as= scan_keyword(g->on, &kw_as);
  g->object_end= g->as ? g->as : grant + strlen(grant);
  g->with_opt= size_t(g->object_end - g->on) >= grant_opt.length &&
               memcmp(g->object_end - grant_opt.length, grant_opt.str,
                      grant_opt.length) == 0;
  return true;
}


/* Is the grant on all databases and tables? */
static bool is_global_grant(const Grant_parts &g)
{
  return strncmp(g.on, STRING_WITH_LEN(" ON *.* TO ")) == 0;
}


/*
  Emit the GRANT of the translated privileges in st, gated 11.4: all of them
  exist by then. For a role grantee, the object ends before " TO " and the
  role follows.
*/
static void emit_translated_grant(FILE *out, const struct Priv_state *st,
                                  const Grant_parts &g, const char *role_name,
                                  bool drop_option)
{
  static const LEX_CSTRING kw_to= {STRING_WITH_LEN(" TO ")};

  if (g.as)
    dump_emit_comment_warning(out,
      "WARNING: partial-revokes AS clause dropped (no MariaDB "
      "equivalent): %s; the grant below is wider than on the MySQL source",
      g.as + 1);

  const char *object_end= g.object_end;
  bool with_opt= g.with_opt;
  /*
    MySQL keeps the grant option per dynamic privilege, MariaDB per level:
    keeping it would make every global privilege grantable. Drop it when a
    mapped privilege is emitted. MySQL prints dynamic privileges on a GRANT
    of their own, so the static grant keeps it, unless drop_option: the
    account also has a mapped privilege without it.
  */
  if (with_opt && (st->n_mapped || drop_option))
  {
    if (st->n_mapped)
      dump_emit_comment_warning(out,
        "WARNING: WITH GRANT OPTION dropped from a translated privilege: "
        "MariaDB would make all global privileges of the account grantable:%s",
        st->context.str);
    else
      dump_emit_comment_warning(out,
        "WARNING: WITH GRANT OPTION dropped: MariaDB would let the account "
        "grant its translated privileges, which MySQL did not:%s",
        st->context.str);
    object_end-= grant_opt.length;
    with_opt= false;
  }
  if (role_name)
  {
    const char *to= scan_keyword(g.on, &kw_to);
    if (!to)
    {
      /* Skip it: it would be granted to the MySQL account instead */
      dump_emit_comment_warning(out,
        "WARNING: unrecognized GRANT of a role account ignored: %s", g.text);
      return;
    }
    object_end= to;
  }
  emit_gated(out, "/*M!110400 ", "GRANT %.*s%.*s%s%s%s",
             int(st->privs.length), st->privs.str, int(object_end - g.on),
             g.on, role_name ? " TO " : "", role_name ? role_name : "",
             role_name && with_opt ? grant_opt.str : "");
}


/*
  Translate and emit a MySQL 5.7+ GRANT. Returns false if it is portable and
  the caller should print it as is. A pure role grant emits nothing: the
  caller dumps role grants separately.
  Output: one "# WARNING" listing the dropped privileges, and one for a
  dropped AS clause and for a dropped grant option (only if a privilege
  survives), then the GRANT if any privilege survived.

  role_name: the quoted name of a role grantee, else NULL. A MariaDB role
  has no host, so it replaces `role`@`host`.
*/
bool translate_mysql_grant(const char *grant, FILE *out, const char *role_name,
                           bool drop_global_option)
{
  DBUG_ASSERT(strncmp(grant, STRING_WITH_LEN("GRANT ")) == 0);

  Grant_parts g;
  if (!split_grant(grant, &g))
    return true;

  bool drop_opt= drop_global_option && g.with_opt && is_global_grant(g);
  struct Priv_state st;
  init_priv_state(&st, out, g.on, role_name);
  translate_priv_list(grant + sizeof("GRANT ") - 1, g.on, &st);
  warn_dropped(&st);
  bool portable= !st.n_mapped && !st.dropped.length && !g.as && !role_name &&
                 !drop_opt;
  if (!portable && st.privs.length)
    emit_translated_grant(out, &st, g, role_name, drop_opt);
  free_priv_state(&st);
  return !portable;
}


void scan_mysql_grant(const char *grant, Mysql_grant_scan *scan)
{
  Grant_parts g;
  if (strncmp(grant, STRING_WITH_LEN("GRANT ")) != 0 ||
      !split_grant(grant, &g) || !is_global_grant(g))
    return;

  struct Priv_state st;
  init_priv_state(&st, NULL, g.on, NULL);
  translate_priv_list(grant + sizeof("GRANT ") - 1, g.on, &st);
  if (g.with_opt && !st.n_mapped && st.privs.length)
    scan->global_option= true;
  if (!g.with_opt && st.n_mapped)
    scan->mapped_without_option= true;
  free_priv_state(&st);
}


/*
  SHOW GRANTS emits REVOKE only for partial_revokes: a db-level revoke of a
  privilege granted globally. Emitted as the equivalent DENY (13.1.1+), after
  a "# WARNING": an older server skips it.
  role_name is as for translate_mysql_grant().
*/
void translate_mysql_revoke(const char *revoke, FILE *out,
                            const char *role_name)
{
  DBUG_ASSERT(strncmp(revoke, STRING_WITH_LEN("REVOKE ")) == 0);
  static const LEX_CSTRING kw_on= {STRING_WITH_LEN(" ON ")};
  static const LEX_CSTRING kw_from= {STRING_WITH_LEN(" FROM ")};

  const char *on_ptr= scan_keyword(revoke, &kw_on);
  const char *from_ptr= on_ptr ? scan_keyword(on_ptr, &kw_from) : NULL;
  if (!from_ptr)
  {
    dump_emit_comment_warning(out,
      "WARNING: unrecognized REVOKE ignored, the account is wider than on "
      "the MySQL source: %s", revoke);
    return;
  }

  struct Priv_state st;
  init_priv_state(&st, out, on_ptr, role_name);
  translate_priv_list(revoke + sizeof("REVOKE ") - 1, on_ptr, &st);
  warn_dropped(&st);
  if (st.privs.length)
  {
    const char *grantee= role_name ? role_name : from_ptr + kw_from.length;
    dump_emit_comment_warning(out,
      "WARNING: a partial REVOKE is a DENY, which needs MariaDB 13.1.1: an "
      "older server does not run it, and the account is wider than on the "
      "MySQL source:%s", st.context.str);
    emit_gated(out, "/*M!130101 ", "DENY %.*s%.*s TO %s",
               int(st.privs.length), st.privs.str, int(from_ptr - on_ptr),
               on_ptr, grantee);
  }
  free_priv_state(&st);
}


/*
  What to do with a CREATE USER clause (see user_clauses): keep, drop, drop
  with a warning unless the value is DEFAULT or 0 (CL_DROP_WARN: password
  policy, CL_DROP_WARN_OPT: other options), or the authentication (CL_AUTH)
  or account lock (CL_ACCOUNT) clause.
*/
enum Clause_action { CL_KEEP, CL_DROP, CL_DROP_WARN, CL_DROP_WARN_OPT, CL_AUTH,
                     CL_ACCOUNT };

/* A clause keyword; it must begin with a space. */
struct User_clause
{
  LEX_CSTRING kw;
  enum Clause_action action;
};

/* SHOW CREATE USER clauses. A clause runs to the next, taking its value. */
static const struct User_clause user_clauses[]=
{
  {{STRING_WITH_LEN(" IDENTIFIED ")},               CL_AUTH},
  {{STRING_WITH_LEN(" AND IDENTIFIED ")},           CL_DROP},
  {{STRING_WITH_LEN(" INITIAL AUTHENTICATION ")},   CL_DROP},
  {{STRING_WITH_LEN(" DEFAULT ROLE ")},             CL_DROP},
  {{STRING_WITH_LEN(" REQUIRE ")},                  CL_KEEP},
  {{STRING_WITH_LEN(" WITH MAX_")},                 CL_KEEP},
  {{STRING_WITH_LEN(" PASSWORD EXPIRE")},           CL_KEEP},
  {{STRING_WITH_LEN(" PASSWORD HISTORY ")},         CL_DROP_WARN},
  {{STRING_WITH_LEN(" PASSWORD REUSE INTERVAL ")},  CL_DROP_WARN},
  /* No trailing space: the bare form can end the string, and " REQUIRE "
     would match instead. */
  {{STRING_WITH_LEN(" PASSWORD REQUIRE CURRENT")},  CL_DROP_WARN},
  {{STRING_WITH_LEN(" FAILED_LOGIN_ATTEMPTS ")},    CL_DROP_WARN_OPT},
  {{STRING_WITH_LEN(" PASSWORD_LOCK_TIME ")},       CL_DROP_WARN_OPT},
  {{STRING_WITH_LEN(" COMMENT ")},                  CL_DROP_WARN_OPT},
  {{STRING_WITH_LEN(" ATTRIBUTE ")},                CL_DROP_WARN_OPT},
  {{STRING_WITH_LEN(" ACCOUNT UNLOCK")},            CL_ACCOUNT},
  {{STRING_WITH_LEN(" ACCOUNT LOCK")},              CL_ACCOUNT},
  {{NULL, 0}, CL_KEEP}
};

/*
  The first top-level clause keyword in `start`, the longest at a position.
  Sets *idx to its index in user_clauses, unchanged if none matches. Resume
  past a hit at pos + user_clauses[*idx].kw.length.
*/
static const char *scan_clause(const char *start, int *idx)
{
  return scan_top_level(start, start + strlen(start), [idx](const char *p)
  {
    int best= -1;
    size_t best_len= 0;
    for (int i= 0; user_clauses[i].kw.str; i++)
    {
      DBUG_ASSERT(user_clauses[i].kw.str[0] == ' ');
      if (user_clauses[i].kw.length > best_len &&
          strncmp(p, user_clauses[i].kw.str, user_clauses[i].kw.length) == 0)
      { best= i; best_len= user_clauses[i].kw.length; }
    }
    if (best < 0)
      return false;
    *idx= best;
    return true;
  });
}

/*
  Auth plugins MariaDB can use on import: mysql_native_password and
  caching_sha2_password (needs the auth_mysql_sha2 plugin, else the statement
  fails). Any other leading plugin gets a warning, loses its IDENTIFIED
  clause and gets ACCOUNT LOCK, see translate_mysql_create_user().
*/
static const char *const mariadb_known_auth_plugins[]= {
  "mysql_native_password",
  "caching_sha2_password",
  NULL
};


/* Queue a "# ..." line, to be printed after its statement. */
static void queue_warning(DYNAMIC_STRING *warnings, const char *fmt, ...)
  ATTRIBUTE_FORMAT(printf, 2, 3);
static void queue_warning(DYNAMIC_STRING *warnings, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vformat_comment_line(warnings, fmt, args);
  va_end(args);
}


/*
  Queue a warning for the first IDENTIFIED WITH plugin if it is not in
  mariadb_known_auth_plugins, and for each further factor (dropped, MariaDB
  has no second factor). Returns true if the first plugin is unsupported.
*/
static bool scan_identified_plugins(const char *user_str,
                                    DYNAMIC_STRING *warnings_out)
{
  static const LEX_CSTRING kw_idw= {STRING_WITH_LEN(" IDENTIFIED WITH '")};
  bool first_unsupported= false;
  bool first= true;

  for (const char *p= user_str;
       (p= scan_keyword(p, &kw_idw)) != NULL; )
  {
    const char *plugin_start= p + kw_idw.length;
    const char *plugin_end= strchr(plugin_start, '\'');
    if (!plugin_end)
      break;                /* malformed: unterminated plugin name */
    size_t plen= plugin_end - plugin_start;

    /* Only the first plugin decides whether IDENTIFIED is dropped. */
    if (!first)
      queue_warning(warnings_out,
                    "WARNING: additional authentication factor '%.*s' "
                    "dropped: MariaDB cannot require it", int(plen),
                    plugin_start);
    else if (!in_list(mariadb_known_auth_plugins, plugin_start, plen))
    {
      queue_warning(warnings_out,
                    "WARNING: auth plugin '%.*s' not shipped with "
                    "MariaDB; user created with no password and ACCOUNT LOCK - "
                    "set a password and ACCOUNT UNLOCK before login",
                    int(plen), plugin_start);
      first_unsupported= true;
    }
    first= false;
    p= plugin_end + 1;
  }
  return first_unsupported;
}

/*
  Emit CREATE USER from MySQL 5.7+: drop clauses MariaDB lacks, add ACCOUNT
  LOCK when the authentication is dropped, then the queued # WARNING lines.
*/
void translate_mysql_create_user(const char *user_str, FILE *out,
                                 bool replace_into, bool ignore)
{
  DYNAMIC_STRING warnings;
  check_alloc(init_dynamic_string(&warnings, "", 256, 256));
  bool ident_incompat= scan_identified_plugins(user_str, &warnings);

  dump_put_create_user(out, replace_into, ignore);

  /* The text before the first clause (" `u`@`h`") is verbatim. */
  const char *cur= user_str;
  bool account_emitted= false;
  int idx= 0;
  const char *clause= scan_clause(user_str, &idx);
  while (clause)
  {
    int nidx= 0;
    const char *next= scan_clause(clause + user_clauses[idx].kw.length, &nidx);
    const char *clause_end= next ? next : user_str + strlen(user_str);

    fwrite(cur, 1, clause - cur, out);

    switch (user_clauses[idx].action) {
    case CL_DROP:
      break;
    case CL_DROP_WARN:
    case CL_DROP_WARN_OPT:
    {
      /* The value follows the keyword; DEFAULT and 0 mean no policy. */
      const char *val= clause + user_clauses[idx].kw.length;
      while (val < clause_end && *val == ' ')
        val++;
      bool no_policy= !strncmp(val, STRING_WITH_LEN("DEFAULT")) ||
                      (*val == '0' && !my_isdigit(&my_charset_latin1, val[1]));
      if (!no_policy)
        queue_warning(&warnings,
                      user_clauses[idx].action == CL_DROP_WARN ?
                      "WARNING: password policy clause dropped (no MariaDB "
                      "equivalent):%.*s" :
                      "WARNING: account option dropped (no MariaDB "
                      "equivalent):%.*s", int(clause_end - clause), clause);
      break;
    }
    case CL_AUTH:
      if (!ident_incompat)
        fwrite(clause, 1, clause_end - clause, out);
      break;
    case CL_ACCOUNT:
      account_emitted= true;
      if (ident_incompat)
        fputs(" ACCOUNT LOCK", out);
      else
        fwrite(clause, 1, clause_end - clause, out);
      break;
    case CL_KEEP:
      fwrite(clause, 1, clause_end - clause, out);
      break;
    }

    cur= clause_end;
    clause= next;
    idx= nidx;
  }
  fputs(cur, out);
  if (ident_incompat && !account_emitted)
    fputs(" ACCOUNT LOCK", out);
  fputs(";\n", out);
  fputs(warnings.str, out);
  dynstr_free(&warnings);
}
