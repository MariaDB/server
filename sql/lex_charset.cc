/* Copyright (c) 2021, 2022, MariaDB Corporation.

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


#include "my_global.h"
#include "my_sys.h"
#include "m_ctype.h"
#include "lex_charset.h"
#include "mysqld_error.h"


/** find a collation with binary comparison rules
*/
CHARSET_INFO *Lex_charset_collation_st::find_bin_collation(CHARSET_INFO *cs)
{
  /*
    We don't need to handle old_mode=UTF8_IS_UTF8MB3 here,
    because "cs" points to a real character set name.
    It can be either "utf8mb3" or "utf8mb4". It cannot be "utf8".
    No thd->get_utf8_flag() flag passed to get_charset_by_csname().
  */
  DBUG_ASSERT(cs->cs_name.length !=4 || memcmp(cs->cs_name.str, "utf8", 4));
  /*
    CREATE TABLE t1 (a CHAR(10) BINARY)
      CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;
    Nothing to do, we have the binary collation already.
  */
  if (cs->state & MY_CS_BINSORT)
    return cs;

  // CREATE TABLE t1 (a CHAR(10) BINARY) CHARACTER SET utf8mb4;
  const LEX_CSTRING &cs_name= cs->cs_name;
  if (!(cs= get_charset_by_csname(cs->cs_name.str, MY_CS_BINSORT, MYF(0))))
  {
    char tmp[65];
    strxnmov(tmp, sizeof(tmp)-1, cs_name.str, "_bin", NULL);
    my_error(ER_UNKNOWN_COLLATION, MYF(0), tmp);
  }
  return cs;
}


CHARSET_INFO *Lex_charset_collation_st::find_default_collation(CHARSET_INFO *cs)
{
  // See comments in find_bin_collation()
  DBUG_ASSERT(cs->cs_name.length !=4 || memcmp(cs->cs_name.str, "utf8", 4));
  /*
    CREATE TABLE t1 (a CHAR(10) COLLATE DEFAULT) CHARACTER SET utf8mb4;
    Nothing to do, we have the default collation already.
  */
  if (cs->state & MY_CS_PRIMARY)
    return cs;
  /*
    CREATE TABLE t1 (a CHAR(10) COLLATE DEFAULT)
      CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;

    Don't need to handle old_mode=UTF8_IS_UTF8MB3 here.
    See comments in find_bin_collation.
  */
  cs= get_charset_by_csname(cs->cs_name.str, MY_CS_PRIMARY, MYF(MY_WME));
  /*
    The above should never fail, as we have default collations for
    all character sets.
  */
  DBUG_ASSERT(cs);
  return cs;
}


bool Lex_charset_collation_st::set_charset_collate_exact(CHARSET_INFO *cs,
                                                         CHARSET_INFO *cl)
{
  DBUG_ASSERT(cs != nullptr && cl != nullptr);
  if (!my_charset_same(cl, cs))
  {
    my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0),
             cl->coll_name.str, cs->cs_name.str);
    return true;
  }
  set_collate_exact(cl);
  return false;
}


/*
  Resolve an empty or a contextually typed collation according to the
  upper level default character set (and optionally a collation), e.g.:
    CREATE TABLE t1 (a CHAR(10)) CHARACTER SET latin1;
    CREATE TABLE t1 (a CHAR(10) BINARY) CHARACTER SET latin1;
    CREATE TABLE t1 (a CHAR(10) COLLATE DEFAULT)
      CHARACTER SET latin1 COLLATE latin1_bin;

  "this" is the COLLATE clause                  (e.g. of a column)
  "def" is the upper level CHARACTER SET clause (e.g. of a table)
*/
CHARSET_INFO *
Lex_charset_collation_st::resolved_to_character_set(CHARSET_INFO *def) const
{
  DBUG_ASSERT(def);

  switch (m_type) {
  case TYPE_EMPTY:
    return def;
  case TYPE_CHARACTER_SET:
    DBUG_ASSERT(m_ci);
    return m_ci;
  case TYPE_COLLATE_EXACT:
    DBUG_ASSERT(m_ci);
    return m_ci;
  case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    break;
  }

  // Contextually typed
  DBUG_ASSERT(m_ci);

  if (is_contextually_typed_binary_style())    // CHAR(10) BINARY
    return find_bin_collation(def);

  if (is_contextually_typed_collate_default()) // CHAR(10) COLLATE DEFAULT
    return find_default_collation(def);

  /*
    Non-binary and non-default contextually typed collation.
    We don't have such yet - the parser cannot produce this.
    But will have soon, e.g. "uca1400_as_ci".
  */
  DBUG_ASSERT(0);
  return NULL;
}


/*
  Merge the CHARACTER SET clause to:
  - an empty COLLATE clause
  - an explicitly typed collation name
  - a contextually typed collation

  "this" corresponds to `CHARACTER SET xxx [BINARY]`
  "cl" corresponds to the COLLATE clause
*/
bool
Lex_charset_collation_st::
  merge_charset_clause_and_collate_clause(const Lex_charset_collation_st &cl)
{
  if (cl.is_empty()) // No COLLATE clause
    return false;

  switch (m_type) {
  case TYPE_EMPTY:
    /*
      No CHARACTER SET clause
      CHAR(10) NOT NULL COLLATE latin1_bin
      CHAR(10) NOT NULL COLLATE DEFAULT
    */
    *this= cl;
    return false;
  case TYPE_CHARACTER_SET:
  case TYPE_COLLATE_EXACT:
    {
      Lex_explicit_charset_opt_collate ecs(m_ci, m_type == TYPE_COLLATE_EXACT);
      if (ecs.merge_collate_or_error(cl))
        return true;
      set_collate_exact(ecs.charset_and_collation());
      return false;
    }
  case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    break;
  }

  if (is_contextually_typed_collation())
  {
    if (cl.is_contextually_typed_collation())
    {
      /*
        CONTEXT + CONTEXT:
        CHAR(10) BINARY .. COLLATE DEFAULT - not supported by the parser
        CHAR(10) BINARY .. COLLATE uca1400_as_ci - not supported yet
      */
      DBUG_ASSERT(0); // Not possible yet
      return false;
    }

    /*
      CONTEXT + EXPLICIT
      CHAR(10) COLLATE DEFAULT       .. COLLATE latin1_swedish_ci
      CHAR(10) BINARY                .. COLLATE latin1_bin
      CHAR(10) COLLATE uca1400_as_ci .. COLLATE latin1_bin
    */
    if (is_contextually_typed_collate_default() &&
        !(cl.charset_collation()->state & MY_CS_PRIMARY))
    {
      my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
               "COLLATE ", "DEFAULT", "COLLATE ",
               cl.charset_collation()->coll_name.str);
      return true;
    }

    if (is_contextually_typed_binary_style() &&
        !(cl.charset_collation()->state & MY_CS_BINSORT))
    {
      my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
               "", "BINARY", "COLLATE ", cl.charset_collation()->coll_name.str);
      return true;
    }
    *this= cl;
    return false;
  }

  DBUG_ASSERT(0);
  return false;
}


bool
Lex_explicit_charset_opt_collate::
  merge_collate_or_error(const Lex_charset_collation_st &cl)
{
  DBUG_ASSERT(cl.type() != Lex_charset_collation_st::TYPE_CHARACTER_SET);

  switch (cl.type()) {
  case Lex_charset_collation_st::TYPE_EMPTY:
    return false;
  case Lex_charset_collation_st::TYPE_CHARACTER_SET:
    DBUG_ASSERT(0);
    return false;
  case Lex_charset_collation_st::TYPE_COLLATE_EXACT:
    /*
      EXPLICIT + EXPLICIT
      CHAR(10) CHARACTER SET latin1                    .. COLLATE latin1_bin
      CHAR(10) CHARACTER SET latin1 COLLATE latin1_bin .. COLLATE latin1_bin
      CHAR(10) COLLATE latin1_bin                      .. COLLATE latin1_bin
      CHAR(10) COLLATE latin1_bin                      .. COLLATE latin1_bin
      CHAR(10) CHARACTER SET latin1 BINARY             .. COLLATE latin1_bin
    */
    if (m_with_collate && m_ci != cl.charset_collation())
    {
      my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
               "COLLATE ", m_ci->coll_name.str,
               "COLLATE ", cl.charset_collation()->coll_name.str);
      return true;
    }
    if (!my_charset_same(m_ci, cl.charset_collation()))
    {
      my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0),
               cl.charset_collation()->coll_name.str, m_ci->cs_name.str);
      return true;
    }
    m_ci= cl.charset_collation();
    m_with_collate= true;
    return false;

  case Lex_charset_collation_st::TYPE_COLLATE_CONTEXTUALLY_TYPED:
    if (cl.is_contextually_typed_collate_default())
    {
      /*
        SET NAMES latin1 COLLATE DEFAULT;
        ALTER TABLE t1 CONVERT TO CHARACTER SET latin1 COLLATE DEFAULT;
      */
      CHARSET_INFO *tmp= Lex_charset_collation_st::find_default_collation(m_ci);
      if (!tmp)
        return true;
      m_ci= tmp;
      m_with_collate= true;
      return false;
    }
    else
    {
      /*
        EXPLICIT + CONTEXT
        CHAR(10) COLLATE latin1_bin .. COLLATE DEFAULT not possible yet
        CHAR(10) COLLATE latin1_bin .. COLLATE uca1400_as_ci
      */

      DBUG_ASSERT(0); // Not possible yet
      return false;
    }
  }
  DBUG_ASSERT(0);
  return false;
}


/*
  This method is used in the "attribute_list" rule to merge two independent
  COLLATE clauses (not belonging to a CHARACTER SET clause).
*/
bool
Lex_charset_collation_st::
  merge_collate_clause_and_collate_clause(const Lex_charset_collation_st &cl)
{
  /*
    "BINARY" and "COLLATE DEFAULT" are not possible
    in an independent COLLATE clause in a column attribute.
  */
  DBUG_ASSERT(!is_contextually_typed_collation());
  DBUG_ASSERT(!cl.is_contextually_typed_collation());

  if (cl.is_empty())
    return false;

  switch (m_type) {
  case TYPE_EMPTY:
    *this= cl;
    return false;
  case TYPE_CHARACTER_SET:
    DBUG_ASSERT(0);
    return false;
  case TYPE_COLLATE_EXACT:
  case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    break;
  }

  /*
    Two independent explicit collations:
      CHAR(10) NOT NULL COLLATE latin1_bin DEFAULT 'a' COLLATE latin1_bin
    Note, we should perhaps eventually disallow double COLLATE clauses.
    But for now let's just disallow only conflicting ones.
  */
  if (charset_collation() != cl.charset_collation())
  {
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
             "COLLATE ", charset_collation()->coll_name.str,
             "COLLATE ", cl.charset_collation()->coll_name.str);
    return true;
  }
  return false;
}
