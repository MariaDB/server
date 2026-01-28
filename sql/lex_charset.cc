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

static void
raise_ER_CONFLICTING_DECLARATIONS(const char *clause1,
                                  const char *name1,
                                  const char *clause2,
                                  const char *name2,
                                  bool reverse_order)
{
  if (!reverse_order)
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
             clause1, name1, clause2, name2);
  else
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
             clause2, name2, clause1, name1);
}


static void
raise_ER_CONFLICTING_DECLARATIONS(const char *clause1,
                                  const char *name1,
                                  const char *name1_part2,
                                  const char *clause2,
                                  const char *name2,
                                  bool reverse_order)
{
  char def[MY_CS_CHARACTER_SET_NAME_SIZE * 2];
  my_snprintf(def, sizeof(def), "%s (%s)", name1, name1_part2);
  raise_ER_CONFLICTING_DECLARATIONS(clause1, def,
                                    clause2, name2,
                                    reverse_order);
}


bool Lex_exact_charset::raise_if_not_equal(const Lex_exact_charset &rhs) const
{
  if (m_ci == rhs.m_ci)
    return false;
  my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
           "CHARACTER SET ", m_ci->cs_name.str,
           "CHARACTER SET ", rhs.m_ci->cs_name.str);
  return true;
}


bool Lex_exact_charset::
  raise_if_not_applicable(const Lex_exact_collation &cl) const
{
  return Lex_exact_charset_opt_extended_collate(m_ci, false).
           raise_if_not_applicable(cl);
}


bool Lex_exact_charset_opt_extended_collate::
  raise_if_charsets_differ(const Lex_exact_charset &cs) const
{
  if (!my_charset_same(m_ci, cs.charset_info()))
  {
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
             "CHARACTER SET ", m_ci->cs_name.str,
             "CHARACTER SET ", cs.charset_info()->cs_name.str);
    return true;
  }
  return false;
}


bool Lex_exact_charset_opt_extended_collate::
  raise_if_not_applicable(const Lex_exact_collation &cl) const
{
  if (!my_charset_same(m_ci, cl.charset_info()))
  {
    my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0),
             cl.charset_info()->coll_name.str, m_ci->cs_name.str);
    return true;
  }
  return false;
}


bool
Lex_exact_collation::raise_if_not_equal(const Lex_exact_collation &cl) const
{
  if (m_ci != cl.m_ci)
  {
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
             "COLLATE ", m_ci->coll_name.str,
             "COLLATE ", cl.m_ci->coll_name.str);
    return true;
  }
  return false;
}


/*
  Merge an exact collation and a contextual collation.
  @param cl            - The contextual collation to merge to "this".
  @param reverse_order - If the contextual collation is on the left side

  Use reverse_order as follows:
  false:  COLLATE latin1_swedish_ci COLLATE DEFAULT
  true:   COLLATE DEFAULT COLLATE latin1_swedish_ci
*/
bool
Lex_exact_collation::
  raise_if_conflicts_with_context_collation(const Lex_context_collation &cl,
                                            bool reverse_order) const
{
  if (cl.is_contextually_typed_collate_default())
  {
    if (!(m_ci->state & MY_CS_PRIMARY))
    {
      raise_ER_CONFLICTING_DECLARATIONS("COLLATE ", m_ci->coll_name.str,
                                        "COLLATE ", "DEFAULT", reverse_order);
      return true;
    }
    return false;
  }

  if (cl.is_contextually_typed_binary_style())
  {
    if (!(m_ci->state & MY_CS_BINSORT))
    {
      raise_ER_CONFLICTING_DECLARATIONS("COLLATE ", m_ci->coll_name.str,
                                        "", "BINARY", reverse_order);
      return true;
    }
    return false;
  }

  DBUG_ASSERT(!strncmp(cl.charset_info()->coll_name.str,
               STRING_WITH_LEN("utf8mb4_uca1400_")));

  Charset_loader_server loader;
  CHARSET_INFO *ci= loader.get_exact_collation_by_context_name(
                             m_ci,
                             cl.collation_name_context_suffix().str,
                             MYF(0));
  if (m_ci != ci)
  {
    raise_ER_CONFLICTING_DECLARATIONS("COLLATE ",
                                      m_ci->coll_name.str,
                                      "COLLATE ",
                                      cl.collation_name_for_show().str,
                                      reverse_order);
    return true;
  }
  return false;
}


bool
Lex_context_collation::raise_if_not_equal(const Lex_context_collation &cl) const
{
  /*
    Only equal context collations are possible here so far:
    - Column grammar only supports BINARY, but does not support COLLATE DEFAULT
    - DB/Table grammar only support COLLATE DEFAULT
  */
  if (m_ci != cl.m_ci)
  {
    my_error(ER_CONFLICTING_DECLARATIONS, MYF(0),
             is_contextually_typed_binary_style() ? "" : "COLLATE ",
             collation_name_for_show().str,
             cl.is_contextually_typed_binary_style() ? "" : "COLLATE ",
             cl.collation_name_for_show().str);
    return true;
  }
  return false;
}


/*
  Resolve a context collation to the character set (when the former gets known):
    CREATE TABLE t1 (a CHAR(10) BINARY) CHARACTER SET latin1;
    CREATE DATABASE db1 COLLATE DEFAULT CHARACTER SET latin1;
*/
bool Lex_exact_charset_opt_extended_collate::
  merge_context_collation_override(Sql_used *used,
                                   const Charset_collation_map_st &map,
                                   const Lex_context_collation &cl)
{
  DBUG_ASSERT(m_ci);

  // CHAR(10) BINARY
  if (cl.is_contextually_typed_binary_style())
  {
    CHARSET_INFO *ci= find_bin_collation();
    if (!ci)
      return true;
    m_ci= ci;
    m_with_collate= true;
    return false;
  }

  // COLLATE DEFAULT
  if (cl.is_contextually_typed_collate_default())
  {
    CHARSET_INFO *ci= find_mapped_default_collation(used, map);
    DBUG_ASSERT(ci);
    if (!ci)
      return true;
    m_ci= ci;
    m_with_collate= true;
    return false;
  }

  DBUG_ASSERT(!strncmp(cl.charset_info()->coll_name.str,
               STRING_WITH_LEN("utf8mb4_uca1400_")));

  CHARSET_INFO *ci= Charset_loader_server().
                      get_exact_collation_by_context_name_or_error(m_ci,
                        cl.charset_info()->coll_name.str + 8, MYF(0));
  if (!ci)
    return true;
  m_ci= ci;
  m_with_collate= true;
  return false;
}


bool Lex_extended_collation_st::merge_exact_charset(Sql_used *used,
                                                    const Charset_collation_map_st &map,
                                                    const Lex_exact_charset &cs)
{
  switch (m_type) {
  case TYPE_EXACT:
    {
      // COLLATE latin1_swedish_ci .. CHARACTER SET latin1
      return cs.raise_if_not_applicable(Lex_exact_collation(m_ci));
    }
  case TYPE_CONTEXTUALLY_TYPED:
    {
      // COLLATE DEFAULT .. CHARACTER SET latin1
      Lex_exact_charset_opt_extended_collate tmp(cs);
      if (tmp.merge_context_collation(used, map, Lex_context_collation(m_ci)))
        return true;
      *this= Lex_extended_collation(tmp.collation());
      return false;
    }
  }
  DBUG_ASSERT(0);
  return false;
}


bool Lex_extended_collation_st::
       merge_exact_collation(const Lex_exact_collation &rhs)
{
  switch (m_type) {

  case TYPE_EXACT:
    /*
      EXACT + EXACT
      COLLATE latin1_bin .. COLLATE latin1_bin
    */
    return Lex_exact_collation(m_ci).raise_if_not_equal(rhs);

  case TYPE_CONTEXTUALLY_TYPED:
    {
      /*
        CONTEXT + EXACT
        CHAR(10) COLLATE DEFAULT       .. COLLATE latin1_swedish_ci
        CHAR(10) BINARY                .. COLLATE latin1_bin
        CHAR(10) COLLATE uca1400_as_ci .. COLLATE latin1_bin
      */
      if (rhs.raise_if_conflicts_with_context_collation(
                Lex_context_collation(m_ci), true))
        return true;
      *this= Lex_extended_collation(rhs);
      return false;
    }
  }
  DBUG_ASSERT(0);
  return false;
}


bool Lex_extended_collation_st::
  raise_if_conflicts_with_context_collation(const Lex_context_collation &rhs)
                                                                        const
{
  switch (m_type) {

  case TYPE_EXACT:
    /*
      EXACT + CONTEXT
      COLLATE latin1_swedish_ci .. COLLATE DEFAULT
    */
    return Lex_exact_collation(m_ci).
             raise_if_conflicts_with_context_collation(rhs, false);

  case TYPE_CONTEXTUALLY_TYPED:
    {
      /*
        CONTEXT + CONTEXT:
        CHAR(10) BINARY .. COLLATE DEFAULT - not supported by the parser
        CREATE DATABASE db1 COLLATE DEFAULT COLLATE DEFAULT;
      */
      return Lex_context_collation(m_ci).raise_if_not_equal(rhs);
    }
  }
  DBUG_ASSERT(0);
  return false;
}


/*
  Merge two non-empty COLLATE clauses.
*/
bool Lex_extended_collation_st::merge(const Lex_extended_collation_st &rhs)
{
  switch (rhs.type()) {
  case TYPE_EXACT:
    /*
      EXACT + EXACT
      COLLATE latin1_swedish_ci .. COLLATE latin1_swedish_ci

      CONTEXT + EXACT
      COLLATE DEFAULT           .. COLLATE latin1_swedish_ci
      CHAR(10) BINARY           .. COLLATE latin1_bin
    */
    return merge_exact_collation(Lex_exact_collation(rhs.m_ci));
  case TYPE_CONTEXTUALLY_TYPED:
    /*
       EXACT + CONTEXT
       COLLATE latin1_swedish_ci .. COLLATE DEFAULT

       CONTEXT + CONTEXT
       COLLATE DEFAULT           .. COLLATE DEFAULT
       CHAR(10) BINARY           .. COLLATE DEFAULT
    */
    return raise_if_conflicts_with_context_collation(
             Lex_context_collation(rhs.m_ci));
  }
  DBUG_ASSERT(0);
  return false;
}


LEX_CSTRING Lex_context_collation::collation_name_for_show() const
{
  if (is_contextually_typed_collate_default())
    return LEX_CSTRING({STRING_WITH_LEN("DEFAULT")});
  if (is_contextually_typed_binary_style())
    return LEX_CSTRING({STRING_WITH_LEN("BINARY")});
  return collation_name_context_suffix();
}


bool Lex_extended_collation_st::set_by_name(const char *name, myf my_flags)
{
  Charset_loader_server loader;
  CHARSET_INFO *cs;

  if (!strncasecmp(name, STRING_WITH_LEN("uca1400_")))
  {
    if (!(cs= loader.get_context_collation_or_error(name, my_flags)))
      return true;

    *this= Lex_extended_collation(Lex_context_collation(cs));
    return false;
  }

  if (!(cs= loader.get_exact_collation_or_error(name, my_flags)))
    return true;

  *this= Lex_extended_collation(Lex_exact_collation(cs));
  return false;
}


/** find a collation with binary comparison rules
*/
CHARSET_INFO *Lex_exact_charset_opt_extended_collate::find_bin_collation() const
{
  /*
    We don't need to handle old_mode=UTF8_IS_UTF8MB3 here,
    because "m_ci" points to a real character set name.
    It can be either "utf8mb3" or "utf8mb4". It cannot be "utf8".
    No thd->get_utf8_flag() flag passed to get_charset_by_csname().
  */
  DBUG_ASSERT(m_ci->cs_name.length !=4 || memcmp(m_ci->cs_name.str, "utf8", 4));
  /*
    CREATE TABLE t1 (a CHAR(10) BINARY)
      CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;
    Nothing to do, we have the binary collation already.
  */
  if (m_ci->state & MY_CS_BINSORT)
    return m_ci;

  // CREATE TABLE t1 (a CHAR(10) BINARY) CHARACTER SET utf8mb4;
  CHARSET_INFO *cs;
  if (!(cs= get_charset_by_csname(m_ci->cs_name.str, MY_CS_BINSORT, MYF(0))))
  {
    char tmp[65];
    strxnmov(tmp, sizeof(tmp)-1, m_ci->cs_name.str, "_bin", NULL);
    my_error(ER_UNKNOWN_COLLATION, MYF(0), tmp);
  }
  return cs;
}


CHARSET_INFO *
Lex_exact_charset_opt_extended_collate::find_compiled_default_collation() const
{
  // See comments in find_bin_collation()
  DBUG_ASSERT(m_ci->cs_name.length !=4 || memcmp(m_ci->cs_name.str, "utf8", 4));
  /*
    CREATE TABLE t1 (a CHAR(10) COLLATE DEFAULT) CHARACTER SET utf8mb4;
    Nothing to do, we have the default collation already.
  */
  if (m_ci->state & MY_CS_PRIMARY)
    return m_ci;
  /*
    CREATE TABLE t1 (a CHAR(10) COLLATE DEFAULT)
      CHARACTER SET utf8mb4 COLLATE utf8mb4_bin;

    Don't need to handle old_mode=UTF8_IS_UTF8MB3 here.
    See comments in find_bin_collation.
  */
  CHARSET_INFO *cs= get_charset_by_csname(m_ci->cs_name.str,
                                          MY_CS_PRIMARY, MYF(MY_WME));
  /*
    The above should never fail, as we have default collations for
    all character sets.
  */
  DBUG_ASSERT(cs);
  return cs;
}


CHARSET_INFO *
Lex_exact_charset_opt_extended_collate::
  find_mapped_default_collation(Sql_used *used,
                                const Charset_collation_map_st &map) const
{
  CHARSET_INFO *cs= find_compiled_default_collation();
  if (!cs)
    return nullptr;
  return map.get_collation_for_charset(used, cs);
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
CHARSET_INFO *Lex_exact_charset_extended_collation_attrs_st::
                resolved_to_character_set(Sql_used *used,
                                          const Charset_collation_map_st &map,
                                          CHARSET_INFO *def) const
{
  DBUG_ASSERT(def);

  switch (m_type) {
  case TYPE_EMPTY:
    return def;
  case TYPE_CHARACTER_SET:
  case TYPE_CHARACTER_SET_ANY_CS:
  {
    DBUG_ASSERT(m_ci);
    return map.get_collation_for_charset(used, m_ci);
  }
  case TYPE_CHARACTER_SET_COLLATE_EXACT:
  case TYPE_COLLATE_EXACT:
    DBUG_ASSERT(m_ci);
    return m_ci;
  case TYPE_COLLATE_CONTEXTUALLY_TYPED:
  {
    Lex_exact_charset_opt_extended_collate tmp(def, true);
    if (tmp.merge_context_collation_override(used, map, Lex_context_collation(m_ci)))
      return NULL;
    return tmp.collation().charset_info();
  }
  }
  DBUG_ASSERT(0);
  return NULL;
}


Lex_exact_charset_extended_collation_attrs 
    Lex_exact_charset_extended_collation_attrs::any_cs()
{
  return Lex_exact_charset_extended_collation_attrs(
      &my_charset_utf8mb3_general_ci, TYPE_CHARACTER_SET_ANY_CS);
}

bool Lex_exact_charset_extended_collation_attrs_st::
       merge_exact_collation(const Lex_exact_collation &cl)
{
  switch (m_type) {
  case TYPE_EMPTY:
    /*
      No CHARACTER SET clause
      CHAR(10) NOT NULL COLLATE latin1_bin
    */
    *this= Lex_exact_charset_extended_collation_attrs(cl);
    return false;
  case TYPE_CHARACTER_SET:
  case TYPE_CHARACTER_SET_ANY_CS:
    {
      // CHARACTER SET latin1 .. COLLATE latin1_swedish_ci
      Lex_exact_charset_opt_extended_collate tmp(m_ci, false);
      if (tmp.merge_exact_collation(cl))
        return true;
      *this= Lex_exact_charset_extended_collation_attrs(tmp);
      return false;
    }
  case TYPE_CHARACTER_SET_COLLATE_EXACT:
  case TYPE_COLLATE_EXACT:
    {
      // [CHARACTER SET latin1] COLLATE latin1_bin .. COLLATE latin1_bin
      return Lex_exact_collation(m_ci).raise_if_not_equal(cl);
    }
  case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    {
      // COLLATE DEFAULT .. COLLATE latin1_swedish_ci
      if (cl.raise_if_conflicts_with_context_collation(
               Lex_context_collation(m_ci), true))
        return true;
      *this= Lex_exact_charset_extended_collation_attrs(cl);
      return false;
    }
  }
  DBUG_ASSERT(0);
  return false;
}


bool Lex_exact_charset_extended_collation_attrs_st::
       merge_context_collation(Sql_used *used,
                               const Charset_collation_map_st &map,
                               const Lex_context_collation &cl)
{
  switch (m_type) {
  case TYPE_EMPTY:
    /*
      No CHARACTER SET clause
      CHAR(10) NOT NULL .. COLLATE DEFAULT
    */
    *this= Lex_exact_charset_extended_collation_attrs(cl);
    return false;
  case TYPE_CHARACTER_SET:
  case TYPE_CHARACTER_SET_ANY_CS:
    {
      // CHARACTER SET latin1 .. COLLATE DEFAULT
      Lex_exact_charset_opt_extended_collate tmp(m_ci, false);
      if (tmp.merge_context_collation(used, map, cl))
        return true;
      *this= Lex_exact_charset_extended_collation_attrs(tmp);
      return false;
    }
  case TYPE_CHARACTER_SET_COLLATE_EXACT:
  case TYPE_COLLATE_EXACT:
    // [CHARACTER SET latin1] COLLATE latin1_swedish_ci .. COLLATE DEFAULT
    return Lex_exact_collation(m_ci).
             raise_if_conflicts_with_context_collation(cl, false);
  case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    // COLLATE DEFAULT .. COLLATE DEFAULT
    return Lex_context_collation(m_ci).raise_if_not_equal(cl);
  }

  DBUG_ASSERT(0);
  return false;
}


bool Lex_exact_charset_opt_extended_collate::
       merge_exact_collation(const Lex_exact_collation &cl)
{
 // CHARACTER SET latin1 [COLLATE latin1_bin] .. COLLATE latin1_bin
  if (m_with_collate)
    return Lex_exact_collation(m_ci).raise_if_not_equal(cl);
  return merge_exact_collation_override(cl);
}


bool Lex_exact_charset_opt_extended_collate::
       merge_exact_collation_override(const Lex_exact_collation &cl)
{
 // CHARACTER SET latin1 [COLLATE latin1_bin] .. COLLATE latin1_bin
  if (raise_if_not_applicable(cl))
    return true;
  *this= Lex_exact_charset_opt_extended_collate(cl);
  return false;
}


bool Lex_exact_charset_opt_extended_collate::
       merge_context_collation(Sql_used *used,
                               const Charset_collation_map_st &map,
                               const Lex_context_collation &cl)
{
  // CHARACTER SET latin1 [COLLATE latin1_bin] .. COLLATE DEFAULT
  if (m_with_collate)
    return Lex_exact_collation(m_ci).
             raise_if_conflicts_with_context_collation(cl, false);
  return merge_context_collation_override(used, map, cl);
}


bool Lex_exact_charset_extended_collation_attrs_st::
       merge_collation(Sql_used *used,
                       const Charset_collation_map_st &map,
                       const Lex_extended_collation_st &cl)
{
  switch (cl.type()) {
  case Lex_extended_collation_st::TYPE_EXACT:
    return merge_exact_collation(Lex_exact_collation(cl.charset_info()));
  case Lex_extended_collation_st::TYPE_CONTEXTUALLY_TYPED:
    return merge_context_collation(used, map,
                                   Lex_context_collation(cl.charset_info()));
  }
  DBUG_ASSERT(0);
  return false;
}


/*
  Mix an unordered combination of CHARACTER SET and COLLATE clauses
  (i.e. COLLATE can come before CHARACTER SET).
  Merge a CHARACTER SET clause.
  @param cs         - The "CHARACTER SET exact_charset_name".
*/
bool Lex_exact_charset_extended_collation_attrs_st::
       merge_exact_charset(Sql_used *used,
                           const Charset_collation_map_st &map,
                           const Lex_exact_charset &cs)
{
  DBUG_ASSERT(cs.charset_info());

  switch (m_type) {
  case TYPE_EMPTY:
    // CHARACTER SET cs
    *this= Lex_exact_charset_extended_collation_attrs(cs);
    return false;

  case TYPE_CHARACTER_SET:
  case TYPE_CHARACTER_SET_ANY_CS:
    // CHARACTER SET cs1 .. CHARACTER SET cs2
    return Lex_exact_charset(m_ci).raise_if_not_equal(cs);

  case TYPE_COLLATE_EXACT:
    // COLLATE latin1_bin .. CHARACTER SET cs
    if (cs.raise_if_not_applicable(Lex_exact_collation(m_ci)))
      return true;
    m_type= TYPE_CHARACTER_SET_COLLATE_EXACT;
    return false;

  case TYPE_CHARACTER_SET_COLLATE_EXACT:
    // CHARACTER SET cs1 COLLATE cl .. CHARACTER SET cs2
    return Lex_exact_charset_opt_extended_collate(m_ci, true).
             raise_if_charsets_differ(cs);

  case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    // COLLATE DEFAULT .. CHARACTER SET cs
    {
      Lex_exact_charset_opt_extended_collate tmp(cs);
      if (tmp.merge_context_collation(used, map, Lex_context_collation(m_ci)))
        return true;
      *this= Lex_exact_charset_extended_collation_attrs(tmp);
      return false;
    }
  }
  DBUG_ASSERT(0);
  return false;
}


bool Lex_extended_charset_extended_collation_attrs_st::merge_charset_default()
{
  if (m_charset_order == CHARSET_TYPE_EMPTY)
    m_charset_order= CHARSET_TYPE_CONTEXT;
  Lex_opt_context_charset_st::merge_charset_default();
  return false;
}


bool Lex_extended_charset_extended_collation_attrs_st::
       merge_exact_charset(Sql_used *used,
                           const Charset_collation_map_st &map,
                           const Lex_exact_charset &cs)
{
  if (m_charset_order == CHARSET_TYPE_EMPTY)
    m_charset_order= CHARSET_TYPE_EXACT;
  return Lex_exact_charset_extended_collation_attrs_st::
           merge_exact_charset(used, map, cs);
}


bool Lex_extended_charset_extended_collation_attrs_st::
       raise_if_charset_conflicts_with_default(
         const Lex_exact_charset_opt_extended_collate &def) const
{
  DBUG_ASSERT(m_charset_order != CHARSET_TYPE_EMPTY || is_empty());
  if (!my_charset_same(def.collation().charset_info(), m_ci))
  {
    raise_ER_CONFLICTING_DECLARATIONS("CHARACTER SET ", "DEFAULT",
                                  def.collation().charset_info()->cs_name.str,
                                  "CHARACTER SET ", m_ci->cs_name.str,
                                  m_charset_order == CHARSET_TYPE_EXACT);
    return true;
  }
  return false;
}


CHARSET_INFO *
Lex_extended_charset_extended_collation_attrs_st::
  resolved_to_context(Sql_used *used,
                      const Charset_collation_map_st &map,
                      const Charset_collation_context &ctx) const
{
  if (Lex_opt_context_charset_st::is_empty())
  {
    // Without CHARACTER SET DEFAULT
    return Lex_exact_charset_extended_collation_attrs_st::
             resolved_to_character_set(used, map,
                                       ctx.collate_default().charset_info());
  }

  // With CHARACTER SET DEFAULT
  switch (type()) {
  case TYPE_EMPTY:
  case TYPE_CHARACTER_SET_ANY_CS:
    // CHARACTER SET DEFAULT;
    return ctx.charset_default().charset().charset_info();

  case TYPE_CHARACTER_SET:
    // CHARACTER SET DEFAULT CHARACTER SET cs_exact
    if (raise_if_charset_conflicts_with_default(ctx.charset_default()))
    {
      /*
        A possible scenario:
          SET character_set_server=utf8mb4;
          CREATE DATABASE db1  CHARACTER SET latin1  CHARACTER SET DEFAULT;
      */
      return NULL;
    }
    return m_ci;

  case TYPE_CHARACTER_SET_COLLATE_EXACT:
  case TYPE_COLLATE_EXACT:
  {
    /*
      CREATE DATABASE db1
        COLLATE cl_exact
        [ CHARACTER SET cs_exact ]
        CHARACTER SET DEFAULT;
    */
    if (m_type == TYPE_CHARACTER_SET_COLLATE_EXACT &&
        raise_if_charset_conflicts_with_default(ctx.charset_default()))
    {
      /*
        A possible scenario:
          SET character_set_server=utf8mb4;
          CREATE DATABASE db1
            COLLATE latin1_bin
            CHARACTER SET latin1
            CHARACTER SET DEFAULT;
      */
      return NULL;
    }
    /*
      Now check that "COLLATE cl_exact" does not conflict with
      CHARACTER SET DEFAULT.
    */
    if (ctx.charset_default().
          raise_if_not_applicable(Lex_exact_collation(m_ci)))
    {
      /*
        A possible scenario:
          SET character_set_server=utf8mb4;
          CREATE DATABASE db1
            COLLATE latin1_bin
            CHARACTER SET DEFAULT;
      */
      return NULL;
    }
    return m_ci;
  }

  case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    /*
      Both CHARACTER SET and COLLATE are contextual:
        ALTER DATABASE db1 CHARACTER SET DEFAULT COLLATE DEFAULT;
        ALTER DATABASE db1 COLLATE DEFAULT CHARACTER SET DEFAULT;
    */
    return Lex_exact_charset_extended_collation_attrs_st::
             resolved_to_character_set(used, map,
                                       ctx.charset_default().
                                         collation().charset_info());
  }
  DBUG_ASSERT(0);
  return NULL;
}
