/* Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Test plugins for the SQL extension framework, used by
  mysql-test/suite/sql_extension. One library, several plugins:

  test_sql_extension          claims "TEST SQLEXT", "WITH SQLEXT" and
                              "CREATE PROCEDURE SQLEXT". The last two start
                              valid SQL (a CTE, a procedure named SQLEXT),
                              for statements the parser rejects in other
                              ways than a plain syntax error. The word after
                              the prefix
                              selects what execute() does:
                                OK (or nothing)  success, the server sends OK
                                ERROR            raises an error
                                FAIL             fails without an error
                                ECHO             raises an error showing the
                                                 exact text it was given
  test_sql_extension_overlap  claims "TEST OVERLAP FREE" and "TEST SQLEXT
                              OVERLAP": refused while test_sql_extension
                              is installed, the free prefix too; counts its
                              init() and deinit() calls in status
                              variables of test_sql_extension
  test_sql_extension_bad      invalid descriptor (no execute()): refused
  test_sql_extension_empty    invalid descriptor (neither prefixes nor
                              recognize()): refused
  test_sql_extension_bad_prefix
                              invalid descriptor (the prefix "TEST 1BAD" is
                              not keywords): refused
  test_sql_extension_mixed    invalid descriptor (both prefixes and
                              recognize()): refused
  test_sql_extension_recognizer
                              recognizer: claims "TEST RECOGNIZE ..." up to
                              the first ';', or only up to the word SHORT
                              if there is one, or past the end of the
                              statement with the word PAST (a bug the
                              server refuses); ECHO as above. In
                              "TEST RECOGNIZE WAIT", recognize() stops at
                              the debug sync point
                              test_sql_extension_recognize
*/

#include <my_global.h>
#include <sql_plugin.h>                         // st_plugin_int
#include <mysql/plugin_sql_extension.h>
#include <mysqld_error.h>
#include <string.h>
#include <stdio.h>

#ifndef STRING_WITH_LEN                         // used by DEBUG_SYNC()
#define STRING_WITH_LEN(X) (X), ((size_t) (sizeof(X) - 1))
#endif

static int execute_noop(MYSQL_THD, const char *, size_t)
{
  return 0;
}


static bool is_space(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static char upper(char c)
{
  return (c >= 'a' && c <= 'z') ? (char) (c - 'a' + 'A') : c;
}

/*
  Find `word` (uppercase) in the text, case-insensitively, as a whole word,
  and return the position after it, or NULL.
*/
static const char *after_word(const char *q, const char *end,
                              const char *word)
{
  size_t len= strlen(word);
  for (const char *p= q; p + len <= end; p++)
  {
    if (p > q && !is_space(p[-1]))
      continue;
    size_t i= 0;
    while (i < len && upper(p[i]) == word[i])
      i++;
    if (i == len && (p + len == end || is_space(p[len]) || p[len] == ';'))
      return p + len;
  }
  return NULL;
}

static const char *skip_space(const char *p, const char *end)
{
  while (p < end && is_space(*p))
    p++;
  return p;
}

/* The next word after `p`: [*word, *word + *len) */
static void next_word(const char *p, const char *end,
                      const char **word, size_t *len)
{
  p= skip_space(p, end);
  *word= p;
  while (p < end && !is_space(*p) && *p != ';')
    p++;
  *len= (size_t) (p - *word);
}

static bool word_is(const char *w, size_t len, const char *kw)
{
  if (strlen(kw) != len)
    return false;
  for (size_t i= 0; i < len; i++)
    if (upper(w[i]) != kw[i])
      return false;
  return true;
}

static int echo(const char *query, size_t length)
{
  my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: [%.*s]", 0,
                  (int) length, query);
  return 1;
}

static int execute_main(MYSQL_THD thd, const char *query, size_t length)
{
  const char *end= query + length;
  const char *p= after_word(query, end, "SQLEXT");
  const char *word;
  size_t len;
  next_word(p ? p : end, end, &word, &len);

  if (len == 0 || word_is(word, len, "OK"))
    return 0;
  if (word_is(word, len, "ERROR"))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: error", 0);
    return 1;
  }
  if (word_is(word, len, "FAIL"))
    return 1;
  if (word_is(word, len, "ECHO"))
    return echo(query, length);
  my_printf_error(ER_UNKNOWN_ERROR, "test_sql_extension: unknown '%.*s'", 0,
                  (int) len, word);
  return 1;
}

static int execute_recognizer(MYSQL_THD, const char *query, size_t length)
{
  if (after_word(query, query + length, "ECHO"))
    return echo(query, length);
  return 0;
}


/*
  "WITH SQLEXT" and "CREATE PROCEDURE SQLEXT" start valid SQL, so a
  statement with them can fail to parse for a reason the dispatch must
  respect.
*/
static const char *const main_prefixes[]= { "TEST SQLEXT", "WITH SQLEXT",
                                            "CREATE PROCEDURE SQLEXT",
                                            nullptr };

static st_mariadb_sql_extension main_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  main_prefixes,
  nullptr,
  execute_main,
  0
};


static const char *const overlap_prefixes[]=
  { "TEST OVERLAP FREE", "TEST SQLEXT OVERLAP", nullptr };

static st_mariadb_sql_extension overlap_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  overlap_prefixes,
  nullptr,
  execute_noop,
  0
};


static const char *const bad_prefixes[]= { "TEST SQLEXT BAD", nullptr };

static st_mariadb_sql_extension bad_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  bad_prefixes,
  nullptr,
  nullptr,                                      // no execute(): invalid
  0
};


/* Neither prefixes nor recognize(): invalid */
static st_mariadb_sql_extension empty_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  nullptr,
  nullptr,
  execute_noop,
  0
};


static bool starts_with(const char *query, size_t length, const char *kw)
{
  size_t len= strlen(kw);
  if (length < len)
    return false;
  for (size_t i= 0; i < len; i++)
    if (upper(query[i]) != kw[i])
      return false;
  return true;
}

/*
  Claims "TEST RECOGNIZE" up to the first ';', or only up to the word SHORT:
  then what follows the claim decides whether it stands.
*/
static size_t recognize_test(const char *query, size_t length)
{
  if (!starts_with(query, length, "TEST RECOGNIZE"))
    return 0;
  const char *semicolon= (const char *) memchr(query, ';', length);
  size_t claimed= semicolon ? (size_t) (semicolon - query) : length;
  if (const char *end= after_word(query + 14, query + claimed, "SHORT"))
    claimed= (size_t) (end - query);
  if (after_word(query + 14, query + claimed, "PAST"))
    return length + 10;
  /*
    Lets a test run INSTALL or UNINSTALL while a recognizer runs. This
    breaks the rule that a recognizer does not touch the THD, on purpose
    and for the test only: the sync point needs the THD.
  */
  if (after_word(query + 14, query + claimed, "WAIT"))  // 14: the prefix
    /* NULL: 12.3 has no THD service; debug_sync() uses the current THD */
    DEBUG_SYNC(nullptr, "test_sql_extension_recognize");
  return claimed;
}

static st_mariadb_sql_extension recognizer_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  nullptr,
  recognize_test,
  execute_recognizer,
  0
};


/* A prefix that is not keywords: invalid */
static const char *const bad_prefix[]= { "TEST 1BAD", nullptr };

static st_mariadb_sql_extension bad_prefix_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  bad_prefix,
  nullptr,
  execute_noop,
  0
};


/* Both prefixes and recognize(): invalid */
static const char *const mixed_prefixes[]= { "TEST MIXED", nullptr };

static st_mariadb_sql_extension mixed_descriptor=
{
  MariaDB_SQL_EXTENSION_INTERFACE_VERSION,
  mixed_prefixes,
  recognize_test,
  execute_recognizer,
  0
};


/*
  init() and deinit() of test_sql_extension_overlap, counted: a plugin
  refused after its init() succeeded must get its deinit() too. Shown by
  test_sql_extension as status variables. init() keeps its state in
  st_plugin_int::data, as daemon_example does, and deinit() counts only
  if the server left it there.
*/
static int overlap_inits= 0, overlap_deinits= 0;

static int overlap_init(void *p)
{
  static_cast<st_plugin_int *>(p)->data= &overlap_inits;
  overlap_inits++;
  return 0;
}

static int overlap_deinit(void *p)
{
  if (static_cast<st_plugin_int *>(p)->data != &overlap_inits)
    return 1;
  overlap_deinits++;
  return 0;
}

static struct st_mysql_show_var main_status[]=
{
  { "test_sql_extension_overlap_inits", (char *) &overlap_inits, SHOW_INT },
  { "test_sql_extension_overlap_deinits", (char *) &overlap_deinits,
    SHOW_INT },
  { NULL, NULL, SHOW_UNDEF }
};


maria_declare_plugin(test_sql_extension)
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &main_descriptor,
  "test_sql_extension",
  "MariaDB plc",
  "SQL extension test: claims TEST SQLEXT",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  main_status, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &overlap_descriptor,
  "test_sql_extension_overlap",
  "MariaDB plc",
  "SQL extension test: claims TEST SQLEXT OVERLAP",
  PLUGIN_LICENSE_GPL,
  overlap_init, overlap_deinit,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &bad_descriptor,
  "test_sql_extension_bad",
  "MariaDB plc",
  "SQL extension test: invalid descriptor",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &empty_descriptor,
  "test_sql_extension_empty",
  "MariaDB plc",
  "SQL extension test: neither prefixes nor recognize()",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &bad_prefix_descriptor,
  "test_sql_extension_bad_prefix",
  "MariaDB plc",
  "SQL extension test: invalid prefix",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &mixed_descriptor,
  "test_sql_extension_mixed",
  "MariaDB plc",
  "SQL extension test: both prefixes and recognize()",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_SQL_EXTENSION_PLUGIN,
  &recognizer_descriptor,
  "test_sql_extension_recognizer",
  "MariaDB plc",
  "SQL extension test: recognizer",
  PLUGIN_LICENSE_GPL,
  NULL, NULL,
  0x0100,
  NULL, NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
