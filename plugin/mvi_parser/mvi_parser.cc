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
  elements of a JSON array. Today the server encodes them into a hidden
  column and the engine tokenizes that column's text with the built-in
  parser; this is where that work is going to move, so that the elements
  can be read out of the base column and nothing has to be materialised.

  Not there yet. Every mode still hands the text to the built-in parser,
  which is what the engine would have done without a parser at all, so an
  index using this one behaves exactly as before. What changes is only that
  there is now somewhere for it to happen.

  The index tells the parser what it is parsing for through
  MYSQL_FTPARSER_PARAM::ftparser_arg. Nothing sets it yet either.
*/

#include <my_global.h>
#include <mysql/plugin_ftparser.h>

/*
  @brief
    Parse a document being indexed, or a query being searched for.

  @detail
    param->mode says which, and the two are not the same text at all.

    MYSQL_FTPARSER_SIMPLE_MODE is a document -- the value of the column the
    index is over -- and this is where the elements of the array will be
    walked and encoded once ftparser_arg carries the path and the datatype
    to encode for. Until then the column already holds the encoded keys,
    separated by spaces, so splitting them out is exactly what the built-in
    parser does.

    The other two modes are a query: the boolean-mode string the optimizer
    built out of keys it encoded itself, or a phrase being matched against
    a document. Both are keys already, with the boolean operators the
    built-in parser knows, so they go the same way. There will be nothing
    to encode here even later -- the encoding happens where the query is
    built, so that the two sides cannot disagree about what a key is.

  @return
    0 on success, non-zero to fail the parse
*/

static int mvi_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
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
