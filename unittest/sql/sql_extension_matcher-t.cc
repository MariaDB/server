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
  Unit tests for the SQL extension keyword trie and statement boundary
  scanner (sql/sql_extension_matcher).
*/

#include <my_global.h>
#include <my_sys.h>
#include <m_ctype.h>
#include <tap.h>
#include <string.h>
#include "../sql/sql_extension_matcher.h"

static CHARSET_INFO *utf8= &my_charset_utf8mb4_general_ci;

static Sql_ext_owner A, B, R;

static Sql_ext_owner *claim_of(const Sql_ext_keyword_trie &t, const char *q,
                               CHARSET_INFO *cs= utf8)
{
  return t.match(cs, { q, strlen(q) });
}

/** Add one claim */
static Sql_ext_keyword_trie::Add_result add(Sql_ext_keyword_trie &t,
                                            const char *prefix,
                                            Sql_ext_owner *owner)
{
  const char *prefixes[]= { prefix, nullptr };
  Sql_ext_keyword_trie::Conflict c;
  return t.add_claims(prefixes, owner, &c);
}


static void test_prefix_syntax()
{
  auto valid= [](const char *p)
  { return Sql_ext_keyword_trie::check_prefix_syntax(p) == nullptr; };

  ok(valid("CREATE REST"), "plain keywords are valid");
  ok(valid("  create\tOr  REPLACE rest "), "any case and spacing is valid");
  ok(valid("_X$1"), "'_', '$' and digits are valid inside a keyword");
  ok(!valid(""), "empty prefix is invalid");
  ok(!valid("   "), "blank prefix is invalid");
  ok(!valid("1ABC"), "keyword starting with a digit is invalid");
  ok(!valid("CREATE-REST"), "punctuation inside a keyword is invalid");
  ok(!valid("CREATE `REST`"), "quoted keyword is invalid");
  char long_kw[Sql_ext_keyword_trie::MAX_KEYWORD_LENGTH + 2];
  memset(long_kw, 'K', sizeof(long_kw) - 1);
  long_kw[sizeof(long_kw) - 1]= 0;
  ok(!valid(long_kw), "keyword longer than the limit is invalid");
  long_kw[sizeof(long_kw) - 2]= 0;
  ok(valid(long_kw), "keyword of exactly the limit is valid");
}


static void test_keyword_length()
{
  char max_kw[Sql_ext_keyword_trie::MAX_KEYWORD_LENGTH + 1];
  memset(max_kw, 'K', sizeof(max_kw) - 1);
  max_kw[sizeof(max_kw) - 1]= 0;
  char longer[Sql_ext_keyword_trie::MAX_KEYWORD_LENGTH + 2];
  memset(longer, 'K', sizeof(longer) - 1);
  longer[sizeof(longer) - 1]= 0;

  Sql_ext_keyword_trie t(PSI_NOT_INSTRUMENTED);
  add(t, max_kw, &A);
  ok(claim_of(t, max_kw) == &A, "a keyword of the maximum length matches");
  ok(!claim_of(t, longer),
     "an input word longer than the limit matches no keyword");
  add(t, "CREATE REST", &B);
  char q[sizeof("CREATE ") + sizeof(longer)];
  strxmov(q, "CREATE ", longer, NullS);
  ok(!claim_of(t, q), "a long word after a matched keyword stops the walk");
}


static void test_matching()
{
  Sql_ext_keyword_trie t(PSI_NOT_INSTRUMENTED);
  add(t, "CREATE REST", &A);

  ok(claim_of(t, "CREATE REST SERVICE /x") == &A, "exact keywords match");
  ok(claim_of(t, "create rest service") == &A, "matching ignores case");
  ok(claim_of(t, "  CREATE \t\n  REST") == &A, "any whitespace between");
  ok(claim_of(t, "/* c */ CREATE /* d */ REST") == &A,
     "slash-star comments before and between keywords");
  ok(claim_of(t, "CREATE/*c*/REST") == &A,
     "a comment separates keywords, as in the SQL lexer");
  ok(claim_of(t, "-- note\nCREATE -- x\n REST") == &A, "dash-dash comments");
  ok(claim_of(t, "# note\nCREATE # x\nREST") == &A, "hash comments");
  ok(claim_of(t, "--\tx\nCREATE REST") == &A,
     "dash-dash followed by a tab is a comment");
  ok(claim_of(t, "CREATE REST;") == &A, "prefix followed by ';'");
  ok(claim_of(t, "CREATE REST(") == &A, "prefix followed by punctuation");

  ok(!claim_of(t, "CREATE RESTS"), "no match on a longer word");
  ok(!claim_of(t, "CREATE RES"), "no match on a shorter word");
  ok(!claim_of(t, "CREATE"), "no match on part of a prefix");
  ok(!claim_of(t, "CREATE TABLE t1 (a INT)"), "no match on other statements");
  ok(!claim_of(t, ""), "no match on empty input");
  ok(!claim_of(t, "   /* only a comment */  "), "no match on a comment");
  ok(!claim_of(t, "/* unterminated CREATE REST"),
     "no match inside an unterminated comment");
  ok(!claim_of(t, "--CREATE REST"),
     "'--' without a following space is not a comment");
  ok(!claim_of(t, "/*!CREATE*/ REST"),
     "executable comments are plain comments");
  ok(!claim_of(t, "`CREATE` REST"), "quoted identifiers never match");
  ok(!claim_of(t, "CR\xC3\x89" "ATE REST"),
     "a word with non-ASCII characters never matches");
  ok(!claim_of(t, "CREATE\xC2\xA0REST"),
     "a multi-byte character joins the words around it");
}


static void test_claim_overlaps()
{
  Sql_ext_keyword_trie t(PSI_NOT_INSTRUMENTED);
  Sql_ext_keyword_trie::Conflict c;
  add(t, "CREATE REST", &A);

  ok(t.claim_conflicts("create rest service", &B, &c) && c.owner == &A &&
     !strcmp(c.prefix, "CREATE REST"),
     "longer prefix of another owner conflicts, names the existing prefix");
  ok(t.claim_conflicts("CREATE", &B, &c) && c.owner == &A,
     "shorter prefix of another owner conflicts");
  ok(t.claim_conflicts("CREATE REST", &B, &c) && c.owner == &A,
     "equal prefix of another owner conflicts");
  ok(!t.claim_conflicts("CREATE TABLESPACE X", &B, &c),
     "sibling branch does not conflict");
  ok(!t.claim_conflicts("DROP REST", &B, &c), "other first keyword is fine");
  ok(!t.claim_conflicts("CREATE REST SERVICE", &A, &c),
     "own longer prefix does not conflict");
  ok(!t.claim_conflicts("CREATE", &A, &c),
     "own shorter prefix does not conflict");

  add(t, "DROP REST", &B);
  ok(claim_of(t, "DROP REST x") == &B && claim_of(t, "CREATE REST x") == &A,
     "two owners side by side");
}


static void test_add_claims_all_or_none()
{
  Sql_ext_keyword_trie t(PSI_NOT_INSTRUMENTED);
  Sql_ext_keyword_trie::Conflict c;
  add(t, "CREATE REST", &A);

  const char *refused[]= { "DROP REST", "CREATE REST SERVICE", nullptr };
  ok(t.add_claims(refused, &B, &c) == Sql_ext_keyword_trie::CONFLICT &&
     c.refused == refused[1] && c.owner == &A &&
     !strcmp(c.prefix, "CREATE REST"),
     "a conflict names the refused prefix and the one it overlaps");
  ok(!claim_of(t, "DROP REST x"),
     "a refused owner leaves no claim, not even the ones before the conflict");

  const char *own[]= { "SHOW REST SERVICES", "SHOW REST", nullptr };
  ok(t.add_claims(own, &B, &c) == Sql_ext_keyword_trie::ADDED &&
     claim_of(t, "SHOW REST ROUTES") == &B,
     "an owner's overlapping prefixes are accepted, the shorter one wins");
}


static void test_add_claims_out_of_memory()
{
#ifndef DBUG_OFF
  Sql_ext_keyword_trie t(PSI_NOT_INSTRUMENTED);
  Sql_ext_keyword_trie::Conflict c;
  ok(t.is_valid(), "the trie has its root");
  add(t, "CREATE REST", &A);

  DBUG_SET("+d,simulate_out_of_memory");
  Sql_ext_keyword_trie::Add_result r= add(t, "DROP REST", &B);
  DBUG_SET("-d,simulate_out_of_memory");
  ok(r == Sql_ext_keyword_trie::OUT_OF_MEMORY && !claim_of(t, "DROP REST x"),
     "an owner refused for lack of memory has no claim");

  const char *two[]= { "SHOW X", "SHOW Y", nullptr };
  DBUG_SET("+d,sql_ext_trie_oom_after_first");
  r= t.add_claims(two, &B, &c);
  DBUG_SET("-d,sql_ext_trie_oom_after_first");
  ok(r == Sql_ext_keyword_trie::OUT_OF_MEMORY &&
     !claim_of(t, "SHOW X") && !claim_of(t, "SHOW Y"),
     "a failure after some prefixes were added removes them all");
  ok(claim_of(t, "CREATE REST SERVICE") == &A,
     "the claims of other owners are untouched");
  ok(t.add_claims(two, &B, &c) == Sql_ext_keyword_trie::ADDED &&
     claim_of(t, "SHOW Y") == &B, "the owner can be added afterwards");
#else
  skip(5, "needs a debug build");
#endif
}


static void test_remove_shared_path()
{
  Sql_ext_keyword_trie t(PSI_NOT_INSTRUMENTED);
  Sql_ext_keyword_trie::Conflict c;
  add(t, "CREATE REST", &A);
  add(t, "CREATE TABLESPACE X", &B);
  add(t, "DROP REST", &B);

  t.remove(&A);
  ok(!claim_of(t, "CREATE REST SERVICE"), "the removed owner's claim is gone");
  ok(claim_of(t, "CREATE TABLESPACE X y") == &B,
     "a claim sharing the first keyword with it stays");
  ok(claim_of(t, "DROP REST x") == &B, "an unrelated claim stays");
  ok(!t.claim_conflicts("CREATE REST", &R, &c), "the prefix is free again");
  add(t, "CREATE REST", &R);
  ok(claim_of(t, "CREATE REST SERVICE") == &R,
     "and another owner can claim it");

  t.remove(&B);
  ok(claim_of(t, "CREATE REST SERVICE") == &R &&
     !claim_of(t, "CREATE TABLESPACE X y") && !claim_of(t, "DROP REST x"),
     "removing the other owner leaves the shared keyword for the one left");
  t.remove(&R);
  ok(t.empty(), "removing the last owner empties the trie");
}


static void test_same_owner_shorter_wins()
{
  Sql_ext_keyword_trie t(PSI_NOT_INSTRUMENTED);
  add(t, "SHOW REST SERVICES", &A);
  add(t, "SHOW REST", &A);
  ok(claim_of(t, "SHOW REST ROUTES") == &A,
     "after adding a shorter own prefix, it wins");
  add(t, "SHOW REST VIEWS", &A);
  ok(claim_of(t, "SHOW REST anything") == &A,
     "a longer own prefix added later is redundant");
  t.remove(&A);
  ok(t.empty(), "removing the owner empties the trie");
}


static void test_multibyte_charset()
{
  /*
    In SJIS the second byte of a two-byte character can be an ASCII letter:
    0x83 0x41 is one character. It must not be read as the keyword "A".
  */
  CHARSET_INFO *sjis= get_charset_by_name("sjis_japanese_ci", MYF(0));
  if (!sjis)
  {
    skip(2, "sjis not available");
    return;
  }
  Sql_ext_keyword_trie t(PSI_NOT_INSTRUMENTED);
  add(t, "A B", &A);
  ok(claim_of(t, "A B", sjis) == &A, "ASCII keywords match in sjis");
  ok(!claim_of(t, "\x83" "A B", sjis),
     "a trail byte is never read as a keyword");
}

/** sql_ext_statement_length() with the default sql_mode, or the one given */
static size_t stmt_len(const char *q, bool ansi_quotes= false,
                       bool no_backslash_escapes= false,
                       CHARSET_INFO *cs= utf8)
{
  return sql_ext_statement_length(cs, { q, strlen(q) }, ansi_quotes,
                                  no_backslash_escapes);
}


static void test_statement_length()
{
  ok(stmt_len("A B") == 3, "no ';': the whole text");
  ok(stmt_len("") == 0, "empty text");
  ok(stmt_len("A B; C") == 3, "ends at the first ';'");
  ok(stmt_len(";") == 0, "a ';' first: an empty statement");
  ok(stmt_len("A 'x;y'; C") == 7, "';' in a string");
  ok(stmt_len("A \"x;y\"; C") == 7, "';' in a double-quoted string");
  ok(stmt_len("A `x;y`; C") == 7, "';' in a quoted identifier");
  ok(stmt_len("A 'x'';y'; C") == 9, "doubled quote in a string");
  ok(stmt_len("A `x``;y`; C") == 9, "doubled quote in an identifier");
  ok(stmt_len("A 'x\\';y'; C") == 9, "backslash escapes a quote");
  ok(stmt_len("A 'x\\';y'; C", false, true) == 6,
     "NO_BACKSLASH_ESCAPES: a backslash is plain");
  ok(stmt_len("A `x\\`;y`; C") == 6, "no escapes in a quoted identifier");
  ok(stmt_len("A \"x\\\";y\"; C") == 9, "escapes in a double-quoted string");
  ok(stmt_len("A \"x\\\";y\"; C", true) == 6,
     "ANSI_QUOTES: no escapes in a double-quoted identifier");
  ok(stmt_len("A /* x;y */; C") == 11, "';' in a comment");
  ok(stmt_len("A /*!100000 x;y */; C") == 18,
     "';' in an executable comment");
  ok(stmt_len("A -- x;y\n; C") == 9, "';' in a '-- ' comment");
  ok(stmt_len("A --x; C") == 5, "'--' without a space is no comment");
  ok(stmt_len("A # x;y\n; C") == 8, "';' in a '#' comment");
  ok(stmt_len("A -- x;y") == 8, "a '-- ' comment up to the end");
  ok(stmt_len("A 'x; C") == 7, "unterminated string: the whole text");
  ok(stmt_len("A `x; C") == 7, "unterminated identifier: the whole text");
  ok(stmt_len("A /* x; C") == 9, "unterminated comment: the whole text");
  ok(stmt_len("A 'x\\") == 5, "backslash at the end: the whole text");
  ok(stmt_len("A 'x'") == 5, "string up to the end");

  /*
    In SJIS 0x83 0x5C is one character whose second byte is a backslash,
    and 0x83 0x60 one whose second byte is a backquote: neither escapes
    nor ends anything.
  */
  CHARSET_INFO *sjis= get_charset_by_name("sjis_japanese_ci", MYF(0));
  if (!sjis)
  {
    skip(2, "sjis not available");
    return;
  }
  ok(stmt_len("A '\x83\x5C'; C", false, false, sjis) == 6,
     "a trail byte is never read as a backslash");
  ok(stmt_len("A `\x83\x60`; C", false, false, sjis) == 6,
     "a trail byte is never read as a quote");
}


int main(int argc, char **argv)
{
  MY_INIT(argv[0]);
  plan(90);

  test_prefix_syntax();
  test_keyword_length();
  test_matching();
  test_claim_overlaps();
  test_add_claims_all_or_none();
  test_add_claims_out_of_memory();
  test_remove_shared_path();
  test_same_owner_shorter_wins();
  test_multibyte_charset();
  test_statement_length();

  my_end(0);
  return exit_status();
}
