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

#include "mysql8_translate.h"
#include <my_sys.h>
#include <m_string.h>

/* Die if a DYNAMIC_STRING call failed to allocate. */
static void check_alloc(bool failed)
{
  if (failed)
    dump_fatal("Couldn't allocate memory");
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
  Privileges that need translating. Other dynamic privileges (see
  is_dynamic_priv_name()) are dropped with a # WARNING; other names pass
  through.

  mariadb_priv: the MariaDB privileges to emit; NULL in the first slot means
  no equivalent, drop with a # WARNING. CREATE ROLE and DROP ROLE have no '_',
  so they are listed to be dropped.

  ALLOW_NONEXISTENT_DEFINER and SERVICE_CONNECTION_ADMIN are not mapped: the
  nearest MariaDB privileges (SET USER, CONNECTION ADMIN) grant more.
*/
struct Mysql8_priv_map_entry
{
  const char *mysql_priv;
  size_t len;
  const char *mariadb_priv[2];
};
static const struct Mysql8_priv_map_entry mysql8_priv_map[]=
{
  {STRING_WITH_LEN("BINLOG_ADMIN"),        {"BINLOG REPLAY", "BINLOG ADMIN"}},
  {STRING_WITH_LEN("CONNECTION_ADMIN"),    {"CONNECTION ADMIN", NULL}},
  {STRING_WITH_LEN("CREATE ROLE"),         {NULL, NULL}},
  {STRING_WITH_LEN("DROP ROLE"),           {NULL, NULL}},
  {STRING_WITH_LEN("REPLICATION_APPLIER"), {"BINLOG REPLAY", NULL}},
  {STRING_WITH_LEN("REPLICATION_SLAVE_ADMIN"),
                                           {"REPLICATION SLAVE ADMIN", NULL}},
  {STRING_WITH_LEN("SET_ANY_DEFINER"),     {"SET USER", NULL}},
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
  What to do with a CREATE USER clause (see user_clauses): keep, drop, drop
  with a warning unless the value is DEFAULT or 0 (CL_DROP_WARN: password
  policy, CL_DROP_WARN_OPT: other options), or the authentication (CL_AUTH)
  or account lock (CL_ACCOUNT) clause.
*/
enum Clause_action { CL_KEEP, CL_DROP, CL_DROP_WARN, CL_DROP_WARN_OPT, CL_AUTH,
                     CL_ACCOUNT };

/* A keyword for scan_top_level(); it must begin with a space. */
struct Kw_entry
{
  LEX_CSTRING kw;
  enum Clause_action action;
};

/*
  Find the first top-level keyword of `kws` ({NULL,0}-terminated) in `start`,
  skipping quoted and parenthesised regions. The longest keyword wins at a
  position. Sets *idx to its index; NULL if none match. Resume past a hit at
  pos + kws[*idx].kw.length.
*/
static const char *scan_top_level(const char *start,
                                  const struct Kw_entry *kws, int *idx)
{
  int depth= 0;
  for (const char *p= start; *p; )
  {
    if (*p == '\'' || *p == '"' || *p == '`') { p= skip_quoted(p); continue; }
    if (*p == '(') { depth++; p++; continue; }
    if (*p == ')') { depth--; p++; continue; }
    if (depth == 0 && *p == ' ')
    {
      int best= -1;
      size_t best_len= 0;
      for (int i= 0; kws[i].kw.str; i++)
      {
        DBUG_ASSERT(kws[i].kw.str[0] == ' ');
        if (kws[i].kw.length > best_len &&
            strncmp(p, kws[i].kw.str, kws[i].kw.length) == 0)
        { best= i; best_len= kws[i].kw.length; }
      }
      if (best >= 0) { *idx= best; return p; }
    }
    p++;
  }
  return NULL;
}

/* scan_top_level() for a single keyword. */
static const char *scan_keyword(const char *start, const LEX_CSTRING *kw)
{
  const struct Kw_entry kws[]= {{*kw, CL_KEEP}, {{NULL, 0}, CL_KEEP}};
  int idx;
  return scan_top_level(start, kws, &idx);
}

static const struct Mysql8_priv_map_entry *
lookup_mysql8_priv(const char *name, size_t len)
{
  for (size_t i= 0; i < array_elements(mysql8_priv_map); i++)
    if (mysql8_priv_map[i].len == len &&
        memcmp(mysql8_priv_map[i].mysql_priv, name, len) == 0)
      return &mysql8_priv_map[i];
  return NULL;
}

/* Trim spaces and tabs around [s, e); return the length. */
static size_t trim_priv(const char **s, const char *e)
{
  while (*s < e && (**s == ' ' || **s == '\t')) (*s)++;
  while (e > *s && (e[-1] == ' ' || e[-1] == '\t')) e--;
  return size_t(e - *s);
}


/* The translated privilege list of one GRANT. */
struct Priv_state
{
  DYNAMIC_STRING privs;
  /* Mapped privileges already in `privs`: several MySQL ones map to the same
     (BINLOG_ADMIN and REPLICATION_APPLIER both give BINLOG REPLAY). */
  const char *mapped[2 * array_elements(mysql8_priv_map)];
  uint n_mapped;
  FILE *out;                            /* "# WARNING" lines go here */
};

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


/* MySQL dynamic privilege names are [A-Z0-9_]+ with a '_' (BACKUP_ADMIN). */
static bool is_dynamic_priv_name(const char *s, size_t len)
{
  bool underscore= false;
  for (size_t i= 0; i < len; i++)
  {
    if (s[i] == '_')
      underscore= true;
    else if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')))
      return false;
  }
  return underscore;
}


/* Add privilege [start, end) to st; true if it was mapped or dropped. */
static bool process_priv(const char *start, const char *end,
                         struct Priv_state *st)
{
  size_t plen= trim_priv(&start, end);
  if (!plen)
    return false;

  const struct Mysql8_priv_map_entry *e= lookup_mysql8_priv(start, plen);
  if (!e && !is_dynamic_priv_name(start, plen))
  {
    append_priv(st, start, plen);
    return false;
  }
  if (!e || !e->mariadb_priv[0])
  {
    dump_emit_comment_warning(st->out, "WARNING: dropped MySQL privilege "
                              "%.*s (no MariaDB equivalent)",
                              int(plen), start);
    return true;
  }
  for (uint i= 0; i < array_elements(e->mariadb_priv); i++)
    if (e->mariadb_priv[i])
      append_mapped_priv(st, e->mariadb_priv[i]);
  return true;
}


/* Split [start, end) at top-level commas and process_priv() each part. */
static bool translate_priv_list(const char *start, const char *end,
                                struct Priv_state *st)
{
  bool changed= false;
  const char *p= start;
  const char *priv_start= start;
  int depth= 0;
  while (p < end)
  {
    if (*p == '\'' || *p == '"' || *p == '`')
    {
      p= skip_quoted(p);
      continue;
    }
    if (*p == '(') { depth++; p++; continue; }
    if (*p == ')') { depth--; p++; continue; }
    if (depth == 0 && *p == ',')
    {
      changed|= process_priv(priv_start, p, st);
      priv_start= p + 1;
    }
    p++;
  }
  changed|= process_priv(priv_start, end, st);
  return changed;
}


/*
  True if s contains star-slash, which would end an enclosing version comment
  and run the rest as SQL; neither QUOTE() nor backticks escape it.
*/
static bool has_comment_close(const char *s)
{
  return strstr(s, "*/") != NULL;
}


/*
  Print s with each star-slash split over two literals, '..*', '/..'; the
  caller wraps the result in CONCAT(). With `escape`, s is the inside of a
  literal and its quotes and backslashes are doubled; else s is a whole
  literal, printed as is.
*/
static void put_split(FILE *out, const char *s, bool escape)
{
  for (; *s; s++)
  {
    if (s[0] == '*' && s[1] == '/')
    {
      fputs("*', '/", out);
      s++;
    }
    else
    {
      if (escape && (*s == '\'' || *s == '\\'))
        fputc(*s, out);
      fputc(*s, out);
    }
  }
}


void dump_put_comment_safe_literal(FILE *out, const char *literal)
{
  DBUG_ASSERT(literal[0] == '\'');
  if (has_comment_close(literal))
  {
    fputs("CONCAT(", out);
    put_split(out, literal, false);
    fputc(')', out);
  }
  else
    fputs(literal, out);
}


/*
  Format fmt/args into stackbuf, or into a my_malloc'ed buffer if it does not
  fit. Returns the string; the caller my_free()s it unless it is stackbuf.
  Dies on a formatting error or OOM. args is traversed via va_copy.
*/
static char *vformat_dup(const char *fmt, va_list args,
                         char *stackbuf, size_t stacksz)
{
  va_list copy;
  int len;

  va_copy(copy, args);
  len= vsnprintf(stackbuf, stacksz, fmt, copy);
  va_end(copy);
  if (len < 0)
    dump_fatal("vsnprintf failed formatting dump output");
  if (size_t(len) < stacksz)
    return stackbuf;

  char *heap= static_cast<char*>(my_malloc(PSI_NOT_INSTRUMENTED,
                                           size_t(len) + 1, MYF(MY_WME)));
  if (!heap)
    dump_fatal("Couldn't allocate memory for dump output");
  va_copy(copy, args);
  vsnprintf(heap, size_t(len) + 1, fmt, copy);
  va_end(copy);
  return heap;
}


/* Fold CR/LF to a space, keeping a "#" comment on one line. */
static void fold_eol(char *s)
{
  for (; *s; s++)
    if (*s == '\n' || *s == '\r')
      *s= ' ';
}


/*
  Append a "# ..." line to `out`. CR/LF are folded, as QUOTE() and backticks
  do not escape them and the rest would run as SQL. The "#" counterpart of
  the star-slash split in dump_emit_versioned_wrap().
*/
static void vformat_comment_line(DYNAMIC_STRING *out, const char *fmt,
                                 va_list args)
{
  char stackbuf[1024], *body;

  body= vformat_dup(fmt, args, stackbuf, sizeof(stackbuf));
  fold_eol(body);
  append(out, "# ");
  append(out, body);
  append(out, "\n");
  if (body != stackbuf)
    my_free(body);
}


static void vemit_comment_warning(FILE *out, const char *fmt, va_list args)
{
  DYNAMIC_STRING line;

  check_alloc(init_dynamic_string(&line, "", 256, 256));
  vformat_comment_line(&line, fmt, args);
  fputs(line.str, out);
  dynstr_free(&line);
}


void dump_emit_comment_warning(FILE *out, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vemit_comment_warning(out, fmt, args);
  va_end(args);
}


/*
  Emit prefix, the formatted body, comment end and ';'. Pass user data via
  %s, not in fmt, so a '%' in an identifier is not a format specifier. A body
  with star-slash is emitted as EXECUTE IMMEDIATE CONCAT('..*', '/..'), which
  needs M!100203 whatever the prefix.
*/
static void vemit_versioned_wrap(FILE *out, const char *prefix,
                                 const char *fmt, va_list args)
{
  char stackbuf[1024], *body;

  body= vformat_dup(fmt, args, stackbuf, sizeof(stackbuf));
  if (has_comment_close(body))
  {
    fputs("/*M!100203 EXECUTE IMMEDIATE CONCAT('", out);
    put_split(out, body, true);
    fputs("') */;\n", out);
  }
  else
  {
    fputs(prefix, out);
    fputs(body, out);
    fputs(" */;\n", out);
  }
  if (body != stackbuf)
    my_free(body);
}


void dump_emit_versioned_wrap(FILE *out, const char *prefix,
                              const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vemit_versioned_wrap(out, prefix, fmt, args);
  va_end(args);
}


/*
  Translate and emit a MySQL 5.7+ GRANT. Returns false if it is portable and
  the caller should print it as is. A pure role grant emits nothing:
  dump_role_hierarchy_mysql() covers it.
  Output: a "# WARNING" per dropped privilege, and for a dropped AS clause
  and grant option (only if a privilege survives), then the GRANT, if any
  privilege survived.

  role_name: the QUOTE()'d name of a role grantee, else NULL. A MariaDB role
  has no host, so it replaces `role`@`host`.
*/
bool translate_mysql8_grant(const char *grant, FILE *out, const char *role_name)
{
  DBUG_ASSERT(strncmp(grant, "GRANT ", 6) == 0);
  static const LEX_CSTRING kw_on= {STRING_WITH_LEN(" ON ")};
  static const LEX_CSTRING kw_as= {STRING_WITH_LEN(" AS ")};
  static const LEX_CSTRING kw_to= {STRING_WITH_LEN(" TO ")};
  static const LEX_CSTRING grant_opt= {STRING_WITH_LEN(" WITH GRANT OPTION")};

  const char *on_ptr= scan_keyword(grant, &kw_on);
  if (!on_ptr)
  {
    /* Pure role grant, see the function header. */
    return true;
  }
  const char *as_ptr= scan_keyword(on_ptr, &kw_as);

  struct Priv_state st;
  check_alloc(init_dynamic_string(&st.privs, "", 256, 1024));
  st.n_mapped= 0;
  st.out= out;

  bool changed= translate_priv_list(grant + sizeof("GRANT ") - 1, on_ptr, &st);
  if (!changed && !as_ptr && !role_name)
  {
    dynstr_free(&st.privs);
    return false;
  }

  if (st.privs.length)
  {
    if (as_ptr)
      dump_emit_comment_warning(out,
        "WARNING: partial-revokes AS clause dropped (no MariaDB "
        "equivalent): %s; the grant below is wider than on the MySQL source",
        as_ptr + 1);
    const char *tail_end= as_ptr ? as_ptr : grant + strlen(grant);
    bool with_opt= size_t(tail_end - on_ptr) >= grant_opt.length &&
                   memcmp(tail_end - grant_opt.length, grant_opt.str,
                          grant_opt.length) == 0;
    /*
      MySQL keeps the grant option per dynamic privilege, MariaDB per level:
      keeping it would make every global privilege grantable. Drop it when a
      mapped privilege is emitted; MySQL prints dynamic privileges on a GRANT
      of their own, so no static privilege loses it.
    */
    if (st.n_mapped && with_opt)
    {
      dump_emit_comment_warning(out,
        "WARNING: WITH GRANT OPTION dropped from a translated privilege: "
        "MariaDB would make all global privileges of the account grantable");
      tail_end-= grant_opt.length;
      with_opt= false;
    }
    const char *to_ptr= role_name ? scan_keyword(on_ptr, &kw_to) : NULL;
    if (to_ptr && to_ptr < tail_end)
      dump_emit_versioned_wrap(out, "/*M!100005 ", "GRANT %.*s%.*s TO %s%s",
                               int(st.privs.length), st.privs.str,
                               int(to_ptr - on_ptr), on_ptr, role_name,
                               with_opt ? grant_opt.str : "");
    else
      dump_emit_versioned_wrap(out, "/*M!100005 ", "GRANT %.*s%.*s",
                               int(st.privs.length), st.privs.str,
                               int(tail_end - on_ptr), on_ptr);
  }
  dynstr_free(&st.privs);
  return true;
}


/* SHOW GRANTS emits REVOKE only for partial_revokes, which MariaDB lacks. */
void translate_mysql8_revoke(const char *revoke, FILE *out)
{
  dump_emit_comment_warning(out,
    "WARNING: partial REVOKE has no MariaDB equivalent, ignored: %s", revoke);
  dump_emit_comment_warning(out,
    "  the wider GRANT remains in effect; re-grant at the narrower "
    "scope if needed.");
}


/* SHOW CREATE USER clauses. A clause runs to the next, taking its value. */
static const struct Kw_entry user_clauses[]=
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
  Auth plugins MariaDB can use on import: mysql_native_password and
  caching_sha2_password (needs the auth_mysql_sha2 plugin, else the statement
  fails). Any other leading plugin gets a warning, loses its IDENTIFIED
  clause and gets ACCOUNT LOCK, see translate_mysql8_create_user().
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
    bool supported= false;
    for (const char *const *m= mariadb_known_auth_plugins; *m; m++)
    {
      if (strlen(*m) == plen && strncmp(plugin_start, *m, plen) == 0)
      {
        supported= true;
        break;
      }
    }
    if (!first)
      queue_warning(warnings_out,
                    "WARNING: additional authentication factor '%.*s' "
                    "dropped: MariaDB cannot require it", int(plen),
                    plugin_start);
    else if (!supported)
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
void translate_mysql8_create_user(const char *user_str, FILE *out,
                                  bool replace_into, bool ignore)
{
  DYNAMIC_STRING warnings;
  check_alloc(init_dynamic_string(&warnings, "", 256, 256));
  bool ident_incompat= scan_identified_plugins(user_str, &warnings);

  fputs("CREATE ", out);
  if (replace_into)
    fputs("/*M!100103 OR REPLACE */ ", out);
  fputs("USER", out);
  if (ignore)
    fputs(" IF NOT EXISTS", out);

  /* The text before the first clause (" `u`@`h`") is verbatim. */
  const char *cur= user_str;
  bool account_emitted= false;
  int idx;
  const char *clause= scan_top_level(user_str, user_clauses, &idx);
  while (clause)
  {
    int nidx= 0;
    const char *next= scan_top_level(clause + user_clauses[idx].kw.length,
                                     user_clauses, &nidx);
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
      bool no_policy= !strncmp(val, "DEFAULT", 7) ||
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
