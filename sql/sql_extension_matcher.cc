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

#include "mariadb.h"
#include "m_ctype.h"
#include "sql_extension_matcher.h"
#include <new>

namespace {

inline bool ascii_alpha(unsigned char c)
{ return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

inline bool ascii_digit(unsigned char c)
{ return c >= '0' && c <= '9'; }

inline unsigned char ascii_upper(unsigned char c)
{ return (c >= 'a' && c <= 'z') ? static_cast<unsigned char>(c - 'a' + 'A')
                                : c; }

inline bool ascii_space(unsigned char c)
{ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v'; }

/** Identifier character of an input statement (ASCII part) */
inline bool word_char(unsigned char c)
{ return ascii_alpha(c) || ascii_digit(c) || c == '_' || c == '$'; }


/**
  Check a prefix written by a plugin author: keywords separated by
  whitespace. Allocates nothing.
  @return NULL if valid, otherwise why not
*/
const char *prefix_syntax_error(const char *prefix)
{
  const unsigned char *p= reinterpret_cast<const unsigned char *>(prefix);
  bool any= false;
  for (;;)
  {
    while (ascii_space(*p))
      p++;
    if (!*p)
      break;
    if (!ascii_alpha(*p) && *p != '_')
      return "a keyword must start with a letter or '_'";
    const unsigned char *start= p;
    for (; *p && !ascii_space(*p); p++)
      if (!word_char(*p))
        return "a keyword may contain only letters, digits, '_' and '$'";
    if (size_t(p - start) > Sql_ext_keyword_trie::MAX_KEYWORD_LENGTH)
      return "a keyword is too long";
    any= true;
  }
  return any ? nullptr : "a prefix must have at least one keyword";
}


/**
  The keywords of a prefix valid for prefix_syntax_error(), uppercased
  one at a time into a buffer of its own: walking a prefix allocates
  nothing, so it cannot fail.
*/
class Prefix_keywords
{
  const unsigned char *m_pos;
  char m_keyword[Sql_ext_keyword_trie::MAX_KEYWORD_LENGTH];
public:
  explicit Prefix_keywords(const char *prefix)
    : m_pos(reinterpret_cast<const unsigned char *>(prefix)) {}
  /** @return false after the last keyword */
  bool next(LEX_CSTRING *keyword)
  {
    while (ascii_space(*m_pos))
      m_pos++;
    if (!*m_pos)
      return false;
    size_t n= 0;
    for (; *m_pos && !ascii_space(*m_pos); m_pos++)
    {
      DBUG_ASSERT(n < sizeof(m_keyword));       // the syntax was checked
      if (n < sizeof(m_keyword))
        m_keyword[n++]= static_cast<char>(ascii_upper(*m_pos));
    }
    *keyword= { m_keyword, n };
    return true;
  }
};


/**
  A prefix as error messages show it: its keywords uppercased, separated
  by one space.
  @return the copy (free with my_free()), or NULL if out of memory
*/
char *normalized_prefix(PSI_memory_key key, const char *prefix)
{
  LEX_CSTRING kw;
  size_t length= 0;
  for (Prefix_keywords words(prefix); words.next(&kw); )
    length+= kw.length + 1;
  char *copy= static_cast<char *>(my_malloc(key, length, MYF(0)));
  if (!copy)
    return nullptr;
  char *to= copy;
  for (Prefix_keywords words(prefix); words.next(&kw); )
  {
    if (to != copy)
      *to++= ' ';
    memcpy(to, kw.str, kw.length);
    to+= kw.length;
  }
  *to= 0;
  return copy;
}


/**
  Splits the start of a statement into words, skipping whitespace and
  comments. A word is a run of identifier characters (ASCII letters and
  digits, '_', '$', and any multi-byte or non-ASCII character, consumed
  whole). Any other character is returned as a one-byte word, which never
  equals a keyword.
*/
class Sql_ext_word_reader
{
  const charset_info_st *m_cs;
  const char *m_pos, *m_end;
  bool skip_space_and_comments();
public:
  Sql_ext_word_reader(const charset_info_st *cs, const LEX_CSTRING &str)
    : m_cs(cs), m_pos(str.str), m_end(str.str + str.length) {}
  /**
    @return false at the end of input or inside an unterminated comment
  */
  bool next(LEX_CSTRING *word);
};

} // namespace


/**
  Skip a comment that starts at p: slash-star ... star-slash (executable
  comments included), "--" followed by whitespace, a control character or
  the end of the input, up to the end of the line (as the server lexer
  reads it), and "#" up to the end of the line.

  @return the byte after the comment, p itself if no comment starts at p,
          or NULL if the input ends inside a slash-star comment
*/
static const char *skip_comment(const charset_info_st *cs, const char *p,
                                const char *end)
{
  if (p >= end)
    return p;
  unsigned char c= static_cast<unsigned char>(*p);
  if (c == '/' && p + 1 < end && p[1] == '*')
  {
    for (const char *q= p + 2; q + 1 < end; q++)
      if (q[0] == '*' && q[1] == '/')
        return q + 2;
    return nullptr;                             // unterminated
  }
  bool dash_comment= c == '-' && p + 1 < end && p[1] == '-' &&
                     (p + 2 == end ||
                      my_isspace(cs, static_cast<uchar>(p[2])) ||
                      my_iscntrl(cs, static_cast<uchar>(p[2])));
  if (dash_comment || c == '#')
  {
    while (p < end && *p != '\n')
      p++;
  }
  return p;
}


/**
  Skip a quoted string or identifier that starts at p, as the server lexer
  reads it: the quote doubled stands for itself, a multi-byte character is
  taken whole, and with escapes a backslash takes the byte after it.

  @return the byte after the closing quote, or NULL if there is none
*/
static const char *skip_quoted(const charset_info_st *cs, const char *p,
                               const char *end, bool escapes)
{
  const char quote= *p++;
  while (p < end)
  {
    if (uint mb= my_ismbchar(cs, p, end))
    {
      p+= mb;
      continue;
    }
    if (*p == '\\' && escapes)
    {
      if (end - p < 2)
        return nullptr;
      p+= 2;
      continue;
    }
    if (*p++ == quote)
    {
      if (p < end && *p == quote)
      {
        p++;                                    // doubled quote
        continue;
      }
      return p;
    }
  }
  return nullptr;                               // unterminated
}


/* Sql_ext_word_reader */

/**
  Skip whitespace and comments (see skip_comment()).

  @return false if the input ended inside a slash-star comment
*/
bool Sql_ext_word_reader::skip_space_and_comments()
{
  while (m_pos < m_end)
  {
    if (my_isspace(m_cs, static_cast<unsigned char>(*m_pos)))
    {
      m_pos++;
      continue;
    }
    const char *after= skip_comment(m_cs, m_pos, m_end);
    if (!after)
    {
      m_pos= m_end;
      return false;                             // unterminated comment
    }
    if (after == m_pos)
      break;
    m_pos= after;
  }
  return true;
}


bool Sql_ext_word_reader::next(LEX_CSTRING *word)
{
  if (!skip_space_and_comments() || m_pos >= m_end)
    return false;

  const char *start= m_pos;
  unsigned char c= static_cast<unsigned char>(*m_pos);
  if (!word_char(c) && c < 0x80)
  {
    m_pos++;                                    // punctuation: one byte
  }
  else
  {
    while (m_pos < m_end)
    {
      if (uint mb= my_ismbchar(m_cs, m_pos, m_end))
      {
        m_pos+= mb;                             // whole multi-byte char
        continue;
      }
      c= static_cast<unsigned char>(*m_pos);
      if (!word_char(c) && c < 0x80)
        break;
      m_pos++;
    }
  }
  *word= { start, static_cast<size_t>(m_pos - start) };
  return true;
}


/* Statement boundary */

size_t sql_ext_statement_length(const charset_info_st *cs,
                                const LEX_CSTRING &query,
                                bool ansi_quotes, bool no_backslash_escapes)
{
  const char *p= query.str, *end= query.str + query.length;
  while (p < end)
  {
    if (uint mb= my_ismbchar(cs, p, end))
    {
      p+= mb;
      continue;
    }
    char c= *p;
    if (c == ';')
      return size_t(p - query.str);
    if (c == '\'' || c == '"' || c == '`')
    {
      /* A backquoted or, with ANSI_QUOTES, double-quoted identifier has
         no escapes */
      bool identifier= c == '`' || (c == '"' && ansi_quotes);
      if (!(p= skip_quoted(cs, p, end, !identifier && !no_backslash_escapes)))
        return query.length;
      continue;
    }
    const char *after= skip_comment(cs, p, end);
    if (!after)
      return query.length;
    p= after == p ? p + 1 : after;
  }
  return query.length;
}


/* Sql_ext_keyword_trie */

struct Sql_ext_keyword_trie::Node
{
  PSI_memory_key key;
  /** The edge leading here: an uppercased keyword (empty for the root) */
  LEX_CSTRING keyword;
  /** Edges below, sorted by keyword */
  Dynamic_array<Node *> children;
  /** The owner whose prefix ends here, if any. Set with claim_prefix
      by set_claim(), cleared with it by clear_claim() */
  Sql_ext_owner *claim= nullptr;
  /** That prefix, normalized (for error messages) */
  char *claim_prefix= nullptr;

  Node(PSI_memory_key key_arg, LEX_CSTRING keyword_arg)
    : key(key_arg), keyword(keyword_arg), children(key_arg, 2, 4) {}
  Node(const Node &)= delete;
  Node &operator=(const Node &)= delete;
  ~Node()
  {
    for (size_t i= 0; i < children.elements(); i++)
      delete children.at(i);
    my_free(const_cast<char *>(keyword.str));
    my_free(claim_prefix);
  }

  /** Claim the node for owner, with its normalized prefix, which the node
      then owns */
  void set_claim(Sql_ext_owner *owner, char *prefix)
  {
    DBUG_ASSERT(!claim && !claim_prefix && owner && prefix);
    claim= owner;
    claim_prefix= prefix;
  }

  void clear_claim()
  {
    claim= nullptr;
    my_free(claim_prefix);
    claim_prefix= nullptr;
  }

  /**
    Compare a word of a statement with a keyword, case-insensitively for
    ASCII (keywords are stored uppercased). A byte of the word that is not
    ASCII compares by its value, so it never equals a keyword byte.
  */
  static int cmp(const LEX_CSTRING &word, const LEX_CSTRING &keyword)
  {
    size_t n= MY_MIN(word.length, keyword.length);
    for (size_t i= 0; i < n; i++)
    {
      unsigned char a= static_cast<unsigned char>(word.str[i]);
      if (a < 0x80)
        a= ascii_upper(a);
      unsigned char b= static_cast<unsigned char>(keyword.str[i]);
      if (a != b)
        return a < b ? -1 : 1;
    }
    return word.length == keyword.length ? 0 :
           (word.length < keyword.length ? -1 : 1);
  }

  /**
    Binary search for a word among the children.
    @param pos  receives where the word's child is, or would be inserted
    @return the child, or NULL
  */
  Node *find_word(const LEX_CSTRING &word, size_t *pos) const
  {
    size_t lo= 0, hi= children.elements();
    while (lo < hi)
    {
      size_t mid= (lo + hi) / 2;
      Node *n= children.at(mid);
      int c= cmp(word, n->keyword);
      if (c == 0)
      {
        *pos= mid;
        return n;
      }
      if (c < 0)
        hi= mid;
      else
        lo= mid + 1;
    }
    *pos= lo;
    return nullptr;
  }

  Node *find_word(const LEX_CSTRING &word) const
  {
    size_t pos;
    return find_word(word, &pos);
  }

  /** @return the child for a normalized keyword, or NULL if out of memory */
  Node *find_or_add(const LEX_CSTRING &kw)
  {
    size_t pos;
    if (Node *n= find_word(kw, &pos))
      return n;
    char *copy= static_cast<char *>(my_malloc(key, kw.length + 1, MYF(0)));
    if (!copy)
      return nullptr;
    memcpy(copy, kw.str, kw.length);
    copy[kw.length]= 0;
    Node *n= new (std::nothrow) Node(key, { copy, kw.length });
    if (!n || children.append(n))
    {
      delete n;
      if (!n)
        my_free(copy);
      return nullptr;
    }
    /* Keep the children sorted: move the new one into place */
    for (size_t i= children.elements() - 1; i > pos; i--)
      children.at(i)= children.at(i - 1);
    children.at(pos)= n;
    return n;
  }

  bool unused() const
  { return !claim && children.elements() == 0; }

  /** A claim of another owner in this subtree (this node included) */
  const Node *other_claim(const Sql_ext_owner *owner) const
  {
    if (claim && claim != owner)
      return this;
    for (size_t i= 0; i < children.elements(); i++)
      if (const Node *n= children.at(i)->other_claim(owner))
        return n;
    return nullptr;
  }

  /**
    Remove the claims of `owner` in the subtree below this node,
    and prune nodes left unused.
  */
  void remove_below(const Sql_ext_owner *owner)
  {
    for (size_t i= 0; i < children.elements();)
    {
      Node *n= children.at(i);
      if (n->claim == owner)
        n->clear_claim();
      n->remove_below(owner);
      if (n->unused())
      {
        delete n;
        children.del(i);
      }
      else
        i++;
    }
  }
};


Sql_ext_keyword_trie::Sql_ext_keyword_trie(PSI_memory_key key)
  : m_key(key), m_root(new (std::nothrow) Node(key, { nullptr, 0 }))
{
}


Sql_ext_keyword_trie::~Sql_ext_keyword_trie()
{
  delete m_root;
}


const char *Sql_ext_keyword_trie::check_prefix_syntax(const char *prefix)
{
  return prefix_syntax_error(prefix);
}


bool Sql_ext_keyword_trie::claim_conflicts(const char *prefix,
                                           const Sql_ext_owner *owner,
                                           Conflict *conflict) const
{
  DBUG_ASSERT(is_valid());
  const Node *node= m_root;
  LEX_CSTRING kw;
  for (Prefix_keywords words(prefix); words.next(&kw); )
  {
    node= node->find_word(kw);
    if (!node)
      return false;                             // a new branch: no overlap
    if (node->claim && node->claim != owner)
    {
      /* An existing claim is a prefix of (or equal to) this one */
      conflict->owner= node->claim;
      conflict->prefix= node->claim_prefix;
      return true;
    }
  }
  /* This prefix is a prefix of existing ones: any of another owner? */
  if (const Node *n= node->other_claim(owner))
  {
    conflict->owner= n->claim;
    conflict->prefix= n->claim_prefix;
    return true;
  }
  return false;
}


bool Sql_ext_keyword_trie::add_claim(const char *prefix, Sql_ext_owner *owner)
{
  DBUG_ASSERT(is_valid());
  Node *node= m_root;
  LEX_CSTRING kw;
  for (Prefix_keywords words(prefix); words.next(&kw); )
  {
    if (!(node= node->find_or_add(kw)))
      return true;
    if (node->claim == owner)
      return false;                             // a shorter own prefix wins
    DBUG_ASSERT(!node->claim);                  // claim_conflicts() said so
  }
  char *normalized= normalized_prefix(m_key, prefix);
  if (!normalized)
    return true;
  node->set_claim(owner, normalized);
  node->remove_below(owner);          // longer own prefixes are redundant
  return false;
}


Sql_ext_keyword_trie::Add_result
Sql_ext_keyword_trie::add_claims(const char *const *prefixes,
                                 Sql_ext_owner *owner, Conflict *conflict)
{
  for (const char *const *p= prefixes; *p; p++)
  {
    /* An invalid one would be cut short and claim another prefix */
    DBUG_ASSERT(!check_prefix_syntax(*p));
    if (claim_conflicts(*p, owner, conflict))
    {
      conflict->refused= *p;
      return CONFLICT;
    }
  }
  for (const char *const *p= prefixes; *p; p++)
    if (add_claim(*p, owner) ||
        (p != prefixes && DBUG_IF("sql_ext_trie_oom_after_first")))
    {
      remove(owner);
      return OUT_OF_MEMORY;
    }
  return ADDED;
}


void Sql_ext_keyword_trie::remove(const Sql_ext_owner *owner)
{
  DBUG_ASSERT(is_valid());
  m_root->remove_below(owner);
}


Sql_ext_owner *Sql_ext_keyword_trie::match(const charset_info_st *cs,
                                           const LEX_CSTRING &query) const
{
  DBUG_ASSERT(is_valid());
  Sql_ext_word_reader reader(cs, query);
  const Node *node= m_root;
  LEX_CSTRING word;

  while (reader.next(&word))
  {
    if (!(node= node->find_word(word)))
      break;
    if (node->claim)
      return node->claim;
  }
  return nullptr;
}


bool Sql_ext_keyword_trie::is_valid() const
{
  return m_root != nullptr;
}


bool Sql_ext_keyword_trie::empty() const
{
  DBUG_ASSERT(is_valid());
  return m_root->children.elements() == 0;
}
