/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/*
  The fulltext parser of a multi-valued index.

  A multi-valued index is a fulltext index whose tokens are the encoded
  elements of a JSON array. The array is somewhere inside the value of the
  column the index is over, and this is what reads it out of there: an
  index that names this parser needs nothing materialised for the engine
  to tokenize.

  Which array, and what to encode its elements as, is what the index was
  declared with. The server hands it over in
  MYSQL_FTPARSER_PARAM::ftparser_arg, the same for every document of that
  index. An index with nothing there is not a multi-valued index -- it is
  a plain fulltext index that happens to name this parser -- and gets the
  built-in parser's behaviour.

  The tokenizing itself lives in the server, next to the encoding, so that
  everything that decides what a key of a multi-valued index is stays in
  one file. See mvi_tokenize_document().
*/

#include <my_global.h>
#include <mysql/plugin_ftparser.h>

/*
  Declared in sql/opt_multi_valued_index.h, which needs a good deal of the
  server to be included first. The argument is opaque here, so a forward
  declaration is all this wants.
*/
struct Mvi_parser_arg;
int mvi_tokenize_document(MYSQL_FTPARSER_PARAM *param, Mvi_parser_arg *arg);

/*
  @brief
    Parse a document being indexed or matched, or a query being searched
    for.

  @detail
    A document and a query are not the same text at all: a document is the
    value of the column the index is over, and has to be turned into the
    keys of the row, while a query is already made of keys -- the
    optimizer encodes them itself, so that the two sides cannot disagree
    about what a key is -- and only has to be split apart.

    MYSQL_FTPARSER_FULL_BOOLEAN_INFO is a query, and a query in that form
    is all the optimizer ever builds for a multi-valued index. So that is
    what goes to the built-in parser, which splits the keys and reads the
    boolean operators, and everything else is taken for a document.

    Everything else is a document with one exception: a natural-language
    query comes in MYSQL_FTPARSER_SIMPLE_MODE, the same mode a document
    being indexed does, and nothing here can tell the two apart. Such a
    query is read as a document, finds no array where the index's path
    says one is, and so matches nothing. Which is the conservative way for
    it to go wrong: MATCH ... AGAINST on a multi-valued index is not
    something the optimizer writes or the user can mean anything by, and
    an empty result is not a wrong row.

    The rest of MYSQL_FTPARSER_SIMPLE_MODE, and all of
    MYSQL_FTPARSER_WITH_STOPWORDS, is a document: one being indexed, one
    being re-read to weigh it against a query, or one being matched
    against a phrase.

  @return
    0 on success, non-zero to fail the parse
*/

static int mvi_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
  Mvi_parser_arg *arg= (Mvi_parser_arg *) param->ftparser_arg;

  if (arg && param->mode != MYSQL_FTPARSER_FULL_BOOLEAN_INFO)
    return mvi_tokenize_document(param, arg);

  return param->mysql_parse(param, param->doc, param->length);
}


static struct st_mysql_ftparser mvi_parser_descriptor=
{
  MYSQL_FTPARSER_INTERFACE_VERSION,
  mvi_parser_parse,
  NULL,                                         /* init */
  NULL                                          /* deinit */
};


maria_declare_plugin(mvi_parser)
{
  MYSQL_FTPARSER_PLUGIN,
  &mvi_parser_descriptor,
  /*
    The name a key carries in the FRM, so it is part of the table
    definition and cannot change once a table uses it.
  */
  "mvi",
  "MariaDB Corporation",
  "Fulltext parser for multi-valued indexes",
  PLUGIN_LICENSE_GPL,
  NULL,                                         /* init */
  NULL,                                         /* deinit */
  0x0100,
  NULL,                                         /* status variables */
  NULL,                                         /* system variables */
  "1.0",
  /*
    A mandatory plugin has to be at least as mature as the server, see the
    assertion in plugin_add().
  */
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
