/*
   Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2020, MariaDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <my_global.h>
#include <m_ctype.h>
#include <mysql/plugin_ftparser.h>

#define NGRAM_TOKEN_SIZE_MAX 10

static int ngram_token_size;

// Read-only, as in MySQL: n-grams of another size would not match an existing
// index.
static MYSQL_SYSVAR_INT(token_size, ngram_token_size,
  PLUGIN_VAR_READONLY,
  "Ngram full text plugin parser token size in characters",
  NULL, NULL, 2, 1, NGRAM_TOKEN_SIZE_MAX, 0);

// A character that can be part of a word, as in the built-in parsers.
static inline bool is_word_char(int char_type, char c)
{
  return (char_type & (_MY_U | _MY_L | _MY_NMR)) || c == '_';
}

// Counts the characters of a string.
static int count_chars(const CHARSET_INFO *cs, const char *str, int len)
{
  const uchar *s= reinterpret_cast<const uchar *>(str);
  const uchar *end= s + len;
  int n_chars= 0;

  while (s < end)
  {
    int char_len= my_ci_charlen(cs, s, end);
    if (char_len <= 0)
      break;
    s+= char_len;
    n_chars++;
  }
  return n_chars;
}

/*
  Splits a string into n-grams of n characters and emits them.

  As in MySQL, a single-byte character that is not a word character (a space
  or punctuation) separates words, and n-grams do not span it. Multi-byte
  characters always belong to n-grams. An illegal or incomplete byte sequence
  ends the string.
*/
static int split_into_ngrams(MYSQL_FTPARSER_PARAM *param, const char *doc,
                             int len, MYSQL_FTPARSER_BOOLEAN_INFO *info)
{
  const CHARSET_INFO *cs= param->cs;
  const int n= ngram_token_size;
  const char *start= doc;               // first character of the n-gram
  const char *next= start;
  const char *end= doc + len;
  // Byte lengths of the characters in [start, next), a ring buffer, so that
  // moving start forward does not scan the string again.
  int char_lens[NGRAM_TOKEN_SIZE_MAX];
  int first= 0;                         // index of the length of *start
  int n_chars= 0;
  bool added= false;
  int ret;

  while (next < end)
  {
    // charlen() returns 0 for an illegal sequence and a negative value
    // (MY_CS_TOOSMALLn) for an incomplete one. Unlike ctype(), it accepts
    // a well-formed character that has no Unicode mapping.
    int char_len= my_ci_charlen(cs, reinterpret_cast<const uchar *>(next),
                                reinterpret_cast<const uchar *>(end));
    if (char_len <= 0 || next + char_len > end)
      break;

    if (char_len == 1)
    {
      int char_type;
      my_ci_ctype(cs, &char_type, reinterpret_cast<const uchar *>(next),
                  reinterpret_cast<const uchar *>(end));
      if (!is_word_char(char_type, *next))
      {
        // End of a word: the next n-gram starts after this character.
        start= ++next;
        first= 0;
        n_chars= 0;
        continue;
      }
    }

    char_lens[(first + n_chars) % NGRAM_TOKEN_SIZE_MAX]= char_len;
    next+= char_len;
    n_chars++;

    if (n_chars == n)
    {
      if ((ret= param->mysql_add_word(param, start,
                                      static_cast<int>(next - start), info)))
        return ret;
      added= true;

      start+= char_lens[first];
      first= (first + 1) % NGRAM_TOKEN_SIZE_MAX;
      n_chars--;
    }
  }

  // A string shorter than n characters cannot give even a single n-gram. In
  // a boolean query or for phrase matching, it is passed on as it is, as
  // MySQL does.
  if ((param->mode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO ||
       param->mode == MYSQL_FTPARSER_WITH_STOPWORDS) &&
      !added && n_chars > 0)
  {
    assert(next > start);
    assert(n_chars < n);
    return param->mysql_add_word(param, start, static_cast<int>(next - start),
                                 info);
  }

  return 0;
}

/*
  Receives the words of a boolean mode query from mysql_parse() and passes
  them on as n-grams, as MySQL does:
  - a word in a phrase is split into the n-grams of the phrase;
  - a word with a wildcard that is shorter than n characters stays a prefix
    search;
  - any other word becomes a phrase of its n-grams, and a wildcard is
    ignored: 'abc' and 'abc*' both mean '"ab bc"'.
*/
static int ngram_parser_add_word_callback(
  struct st_mysql_ftparser_param *cb_param, const char *word, int word_len,
  MYSQL_FTPARSER_BOOLEAN_INFO *info)
{
  int ret;
  MYSQL_FTPARSER_PARAM *param=
    static_cast<MYSQL_FTPARSER_PARAM *>(cb_param->ftparser_state);

  // Short words may be marked as FT_TOKEN_STOPWORD rather than FT_TOKEN_WORD.
  // Ngram parser needs all words, even if they are small.
  if (info->type == FT_TOKEN_STOPWORD)
    info->type= FT_TOKEN_WORD;

  if (info->type != FT_TOKEN_WORD)
    return param->mysql_add_word(param, NULL, 0, info);

  // Already a part of a phrase? Just split into n-grams.
  if (info->quot != NULL)
    return split_into_ngrams(param, word, word_len, info);

  if (info->trunc &&
      count_chars(param->cs, word, word_len) < ngram_token_size)
    return param->mysql_add_word(param, word, word_len, info);

  // Convert to a phrase by wrapping in parenthesis, and split the word into
  // n-grams without a wildcard. As for the words of a quoted phrase, yesno is
  // 1: MyISAM and Aria check the phrase only for required words, InnoDB
  // ignores operators inside a phrase.
  MYSQL_FTPARSER_BOOLEAN_INFO ngram_info=
    { FT_TOKEN_WORD, 1, 0, 0, 0, ' ', NULL };

  info->type= FT_TOKEN_LEFT_PAREN;
  info->quot= reinterpret_cast<char *>(1);
  if ((ret= param->mysql_add_word(param, NULL, 0, info)))
    return ret;

  if ((ret= split_into_ngrams(param, word, word_len, &ngram_info)))
    return ret;

  info->type= FT_TOKEN_RIGHT_PAREN;
  ret= param->mysql_add_word(param, NULL, 0, info);

  info->type= FT_TOKEN_WORD;
  info->quot= NULL;
  return ret;
}

static int ngram_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
  MYSQL_FTPARSER_BOOLEAN_INFO info= { FT_TOKEN_WORD, 0, 0, 0, 0, ' ', NULL };
  MYSQL_FTPARSER_PARAM bool_ftparser_param= *param;

  switch (param->mode)
  {
    case MYSQL_FTPARSER_SIMPLE_MODE:
    case MYSQL_FTPARSER_WITH_STOPWORDS:
      // Simple case: generate n-grams from the string.
      return split_into_ngrams(param, param->doc, param->length, &info);

    case MYSQL_FTPARSER_FULL_BOOLEAN_INFO:
      // Delegate tedious bits of boolean query parsing to the FTS engine.
      bool_ftparser_param.mysql_add_word= ngram_parser_add_word_callback;
      bool_ftparser_param.ftparser_state= param;
      return param->mysql_parse(&bool_ftparser_param, param->doc,
                                param->length);
  }

  return 0;
}

static struct st_mysql_ftparser ngram_parser_descriptor=
{
  MYSQL_FTPARSER_INTERFACE_VERSION,
  ngram_parser_parse,
  NULL,
  NULL,
};

static struct st_mysql_sys_var *ngram_system_variables[]=
{
  MYSQL_SYSVAR(token_size),
  NULL
};

maria_declare_plugin(fulltext_ngram)
{
  MYSQL_FTPARSER_PLUGIN,        // Type.
  &ngram_parser_descriptor,     // Descriptor.
  "ngram",                      // Name.
  "Rinat Ibragimov, Futureweb", // Author.
  "Ngram Full-Text Parser",     // Description.
  PLUGIN_LICENSE_GPL,           // License.
  NULL,                         // Initialization function.
  NULL,                         // Deinitialization function.
  0x0100,                       // Numeric version.
  NULL,                         // Status variables.
  ngram_system_variables,       // System variables.
  "1.0",                        // String version representation.
  MariaDB_PLUGIN_MATURITY_BETA,  // Maturity.
}
maria_declare_plugin_end;
