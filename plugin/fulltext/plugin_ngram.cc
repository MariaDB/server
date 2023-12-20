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

#include <stddef.h>
#include <limits.h>
#include <my_attribute.h>
#include <my_global.h>
#include <m_ctype.h>
#include <ft_global.h>

static int ngram_token_size;

static MYSQL_SYSVAR_INT(token_size, ngram_token_size,
  0,
  "Ngram full text plugin parser token size in characters",
  NULL, NULL, 2, 1, 10, 0);

static int my_ci_charlen(CHARSET_INFO *cs, const uchar *str, const uchar *end)
{
  return my_charlen(cs, reinterpret_cast<const char *>(str),
                    reinterpret_cast<const char *>(end));
}

static int my_ci_ctype(CHARSET_INFO *cs, int *char_type, const uchar *str,
                       const uchar *end)
{
  return cs->cset->ctype(cs, char_type, str, end);
}

// Splits a string into ngrams and emits them.
static int split_into_ngrams(MYSQL_FTPARSER_PARAM *param, const char *doc,
                             int len, MYSQL_FTPARSER_BOOLEAN_INFO *info)
{
  const CHARSET_INFO *cs= param->cs;
  const char *start= doc;
  const char *end= doc + len;
  const char *next= start;
  int n_chars= 0;
  int ngram_count= 0;

  while (next < end)
  {
    int char_type;
    int char_len= my_ci_ctype(cs, &char_type,
                              reinterpret_cast<const uchar *>(next),
                              reinterpret_cast<const uchar *>(end));

    // Broken data?
    if (next + char_len > end || char_len == 0)
      break;

    next += char_len;
    n_chars++;

    if (n_chars == ngram_token_size)
    {
      param->mysql_add_word(param, start, static_cast<int>(next - start), info);

      start += my_ci_charlen(cs, reinterpret_cast<const uchar *>(start),
                             reinterpret_cast<const uchar *>(end));
      n_chars= ngram_token_size - 1;
      ngram_count += 1;
    }
  }

  // Strings less than n character long are too small to generate even
  // a single n-gram. Adding such strings to the index as-is is their only
  // chance of being discoverable.
  if (param->mode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO ||
      param->mode == MYSQL_FTPARSER_WITH_STOPWORDS)
  {
      if (n_chars > 0 && ngram_count == 0)
      {
        assert(next > start);
        assert(n_chars < ngram_token_size);
        param->mysql_add_word(param, start, static_cast<int>(next - start),
                              info);
      }
  }

  return 0;
}

static int ngram_parser_add_word_callback(
  struct st_mysql_ftparser_param *cb_param, const char *word, int word_len,
  MYSQL_FTPARSER_BOOLEAN_INFO *info)
{
  int ret;
  struct st_mysql_ftparser_param *param=
    static_cast<struct st_mysql_ftparser_param *>(cb_param->ftparser_state);

  // Short words may be marked as FT_TOKEN_STOPWORD rather than FT_TOKEN_WORD.
  // Ngram parser needs all words, even if they are small.
  if (info->type == FT_TOKEN_STOPWORD)
    info->type= FT_TOKEN_WORD;

  if (info->type != FT_TOKEN_WORD)
  {
    param->mysql_add_word(param, NULL, 0, info);
    return 0;
  }

  // Already a part of a phrase? Just split into n-grams.
  if (info->quot != NULL)
    return split_into_ngrams(param, word, word_len, info);

  // Not a phrase? Convert to a phrase by wrapping in parenthesis and then
  // split word into n-grams.
  info->type= FT_TOKEN_LEFT_PAREN;
  info->quot= reinterpret_cast<char *>(1);
  param->mysql_add_word(param, NULL, 0, info);

  info->type= FT_TOKEN_WORD;
  ret= split_into_ngrams(param, word, word_len, info);
  if (ret != 0)
    return ret;

  info->type= FT_TOKEN_RIGHT_PAREN;
  param->mysql_add_word(param, NULL, 0, info);

  info->type= FT_TOKEN_WORD;
  info->quot= NULL;
  return 0;
}

static int ngram_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
  MYSQL_FTPARSER_BOOLEAN_INFO info= { FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 };
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
      return param->mysql_parse(&bool_ftparser_param, param->doc, param->length);
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

maria_declare_plugin(ngram_parser)
{
  MYSQL_FTPARSER_PLUGIN,        // Type.
  &ngram_parser_descriptor,     // Descriptor.
  "ngram",                      // Name.
  "",                           // Author.
  "Ngram Full-Text Parser",     // Description.
  PLUGIN_LICENSE_GPL,           // License.
  NULL,                         // Initialization function.
  NULL,                         // Deinitialization function.
  0x0100,                       // Numeric version.
  NULL,                         // Status variables.
  ngram_system_variables,       // System variables.
  "1.0",                        // String version representation.
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL, // Maturity.
}
maria_declare_plugin_end;
