#ifndef SQL_EXTENSION_MATCHER_INCLUDED
#define SQL_EXTENSION_MATCHER_INCLUDED
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

/**
  @file
  Keyword trie for SQL extension prefixes.

  Edges are uppercased keywords, not bytes. A statement is matched by one
  sweep over its first words: whitespace and comments are skipped before and
  between keywords, and the walk stops at the first word with no edge. Each
  word costs a binary search among the keywords that follow the previous
  one, so the cost grows with the length of the matched prefix and only
  logarithmically with the number of registered prefixes.

  This file has no dependency on THD, so it can be unit-tested on its own.
  Locking is the caller's job (see sql_extension.cc).
*/

#include "sql_array.h"                          // Dynamic_array
#include <m_string.h>                           // LEX_CSTRING

struct charset_info_st;

/**
  The party that owns prefixes in the trie (one per installed plugin). The
  trie keeps and compares pointers to it only, and never creates one: every
  owner it returns is one the caller added, so a caller that adds only
  objects of one derived type may cast the owners it gets back to that type.
*/
struct Sql_ext_owner
{
};


/**
  The length of the statement at the start of a text: the bytes before its
  first ';' outside quoted strings, quoted identifiers and comments, read
  with the server lexer's rules. Executable comments are comments here, as
  for the prefix walk. The text of a prefix plugin's statement ends there.

  @param cs                    character set of the text
  @param query                 the text, from the start of the statement
  @param ansi_quotes           sql_mode ANSI_QUOTES: "..." is an identifier
  @param no_backslash_escapes  sql_mode NO_BACKSLASH_ESCAPES

  @return the offset of that ';', or query.length if there is none or the
          text ends inside a quoted string, identifier or comment (the
          statement then runs to the end of the text, and its plugin reports
          what is wrong with it)
*/
size_t sql_ext_statement_length(const charset_info_st *cs,
                                const LEX_CSTRING &query,
                                bool ansi_quotes, bool no_backslash_escapes);


class Sql_ext_keyword_trie
{
public:
  /** Maximum keyword length accepted in a prefix */
  static constexpr size_t MAX_KEYWORD_LENGTH= 64;

  /** Describes a refused claim */
  struct Conflict
  {
    /** The refused prefix, as the caller gave it */
    const char *refused= nullptr;
    /** The owner that holds the other prefix */
    Sql_ext_owner *owner= nullptr;
    /** That prefix, normalized. Points into the trie: use it before the
        trie changes */
    const char *prefix= nullptr;
  };

  enum Add_result { ADDED, CONFLICT, OUT_OF_MEMORY };

  /**
    @param key  instruments the memory of the trie
    Check is_valid() before any other use: the other members assert it.
  */
  explicit Sql_ext_keyword_trie(PSI_memory_key key);
  ~Sql_ext_keyword_trie();
  Sql_ext_keyword_trie(const Sql_ext_keyword_trie &)= delete;
  Sql_ext_keyword_trie &operator=(const Sql_ext_keyword_trie &)= delete;

  /**
    Check a prefix written by a plugin author. Allocates nothing.
    @return NULL if valid, otherwise a message saying why not
  */
  static const char *check_prefix_syntax(const char *prefix);

  /**
    Would a claim of `prefix`, valid for check_prefix_syntax(), by `owner`
    overlap another owner's claim? Allocates nothing. The registry goes
    through add_claims(); this is public for the unit tests.
    @return true and fill the `owner` and `prefix` members of `conflict`
            (not `refused`, which add_claims() sets) if it would
  */
  bool claim_conflicts(const char *prefix, const Sql_ext_owner *owner,
                       Conflict *conflict) const;

  /**
    Add the claims of an owner, all or none: every prefix is checked with
    claim_conflicts() before any is added, and an owner refused for lack of
    memory is left with no claim at all (the registry adds each owner once).
    Overlaps with the owner's own claims resolve to the shorter prefix.

    @param prefixes  NULL-terminated list of prefixes, each one valid for
                     check_prefix_syntax()
    @param conflict  filled when the result is CONFLICT
  */
  Add_result add_claims(const char *const *prefixes, Sql_ext_owner *owner,
                        Conflict *conflict);

  /** Remove every claim of `owner`; other owners are untouched */
  void remove(const Sql_ext_owner *owner);

  /**
    Walk the start of a statement. Allocates nothing.
    @return the owner whose prefix the statement starts with, or NULL
  */
  Sql_ext_owner *match(const charset_info_st *cs,
                       const LEX_CSTRING &query) const;

  /** False if the constructor ran out of memory */
  bool is_valid() const;

  bool empty() const;

private:
  struct Node;
  PSI_memory_key m_key;
  Node *m_root;

  /** Add one claim checked by claim_conflicts(); true if out of memory */
  bool add_claim(const char *prefix, Sql_ext_owner *owner);
};

#endif /* SQL_EXTENSION_MATCHER_INCLUDED */
