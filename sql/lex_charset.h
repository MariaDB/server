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

#ifndef LEX_CHARSET_INCLUDED
#define LEX_CHARSET_INCLUDED


/*
  An exact character set, e.g:
    CHARACTER SET latin1
*/
class Lex_exact_charset
{
  CHARSET_INFO *m_ci;
public:
  explicit Lex_exact_charset(CHARSET_INFO *ci)
   :m_ci(ci)
  {
    DBUG_ASSERT(m_ci);
    DBUG_ASSERT(m_ci->state & MY_CS_PRIMARY);
  }
  CHARSET_INFO *charset_info() const { return m_ci; }
  bool raise_if_not_equal(const Lex_exact_charset &rhs) const;
  bool raise_if_not_applicable(const class Lex_exact_collation &cl) const;
};


/*
  An optional contextually typed character set:
    [ CHARACTER SET DEFAULT ]
*/
class Lex_opt_context_charset_st
{
  /*
    Currently we support only DEFAULT as a possible value.
    So "bool" is enough.
  */
  bool m_had_charset_default;
public:
  void init()
  {
    m_had_charset_default= false;
  }
  void merge_charset_default()
  {
    /*
      Ok to specify CHARACTER SET DEFAULT multiple times.
      No error raised here.
    */
    m_had_charset_default= true;
  }
  bool is_empty() const
  {
    return !m_had_charset_default;
  }
  bool is_contextually_typed_charset_default() const
  {
    return m_had_charset_default;
  }
};


/*
  A contextually typed collation, e.g.:
    COLLATE DEFAULT
    CHAR(10) BINARY
*/
class Lex_context_collation
{
  CHARSET_INFO *m_ci;
public:
  explicit Lex_context_collation(CHARSET_INFO *ci)
   :m_ci(ci)
  {
    DBUG_ASSERT(ci);
  }
  CHARSET_INFO *charset_info() const { return m_ci; }
  bool is_contextually_typed_collate_default() const
  {
    return m_ci == &my_collation_contextually_typed_default;
  }
  bool is_contextually_typed_binary_style() const
  {
    return m_ci == &my_collation_contextually_typed_binary;
  }
  bool raise_if_not_equal(const Lex_context_collation &cl) const;
};


/*
  An exact collation, e.g.
    COLLATE latin1_swedish_ci
*/
class Lex_exact_collation
{
  CHARSET_INFO *m_ci;
public:
  explicit Lex_exact_collation(CHARSET_INFO *ci)
   :m_ci(ci)
  {
    DBUG_ASSERT(ci);
  }
  CHARSET_INFO *charset_info() const { return m_ci; }
  // EXACT + EXACT
  bool raise_if_not_equal(const Lex_exact_collation &cl) const;
  // EXACT + CONTEXT
  // CONTEXT + EXACT
  bool raise_if_conflicts_with_context_collation(const Lex_context_collation &,
                                                 bool reverse_order) const;
};


/*
  Parse time COLLATE clause:
    COLLATE colation_name
  The collation can be either exact or contextual:
    COLLATE latin1_bin
    COLLATE DEFAULT
*/
class Lex_extended_collation_st
{
public:
  enum Type
  {
    TYPE_EXACT,
    TYPE_CONTEXTUALLY_TYPED
  };
protected:
  CHARSET_INFO *m_ci;
  Type m_type;
public:
  void init(CHARSET_INFO *ci, Type type)
  {
    m_ci= ci;
    m_type= type;
  }
  CHARSET_INFO *charset_info() const { return m_ci; }
  Type type() const { return m_type; }
  void set_collate_default()
  {
    m_ci= &my_collation_contextually_typed_default;
    m_type= TYPE_CONTEXTUALLY_TYPED;
  }
  bool raise_if_conflicts_with_context_collation(const Lex_context_collation &)
                                                 const;
  bool merge_exact_charset(const Lex_exact_charset &rhs);
  bool merge_exact_collation(const Lex_exact_collation &rhs);
  bool merge(const Lex_extended_collation_st &rhs);
};


class Lex_extended_collation: public Lex_extended_collation_st
{
public:
  Lex_extended_collation(CHARSET_INFO *ci, Type type)
  {
    init(ci, type);
  }
  Lex_extended_collation(const Lex_exact_collation &rhs)
  {
    init(rhs.charset_info(), TYPE_EXACT);
  }
};


/*
  CHARACTER SET cs_exact [COLLATE cl_exact_or_context]
*/
class Lex_exact_charset_opt_extended_collate
{
  CHARSET_INFO *m_ci;
  bool m_with_collate;
public:
  Lex_exact_charset_opt_extended_collate(CHARSET_INFO *ci, bool with_collate)
   :m_ci(ci), m_with_collate(with_collate)
  {
    DBUG_ASSERT(m_ci);
    DBUG_ASSERT((m_ci->state & MY_CS_PRIMARY) || m_with_collate);
  }
  Lex_exact_charset_opt_extended_collate(const Lex_exact_charset &cs)
   :m_ci(cs.charset_info()), m_with_collate(false)
  {
    DBUG_ASSERT(m_ci);
    DBUG_ASSERT(m_ci->state & MY_CS_PRIMARY);
  }
  Lex_exact_charset_opt_extended_collate(const Lex_exact_collation &cl)
   :m_ci(cl.charset_info()), m_with_collate(true)
  {
    DBUG_ASSERT(m_ci);
  }
  bool with_collate() const { return m_with_collate; }
  CHARSET_INFO *find_bin_collation() const;
  CHARSET_INFO *find_default_collation() const;
  bool raise_if_not_applicable(const Lex_exact_collation &cl) const;
  /*
    Add another COLLATE clause (exact or context).
    So the full syntax looks like:
      CHARACTER SET cs [COLLATE cl] ... COLLATE cl2
  */
  bool merge_collation(const Lex_extended_collation_st &cl)
  {
    switch (cl.type()) {
    case Lex_extended_collation_st::TYPE_EXACT:
      return merge_exact_collation(Lex_exact_collation(cl.charset_info()));
    case Lex_extended_collation_st::TYPE_CONTEXTUALLY_TYPED:
      return merge_context_collation(Lex_context_collation(cl.charset_info()));
    }
    DBUG_ASSERT(0);
    return false;
  }
  /*
    Add a context collation:
      CHARACTER SET cs [COLLATE cl] ... COLLATE DEFAULT
  */
  bool merge_context_collation(const Lex_context_collation &cl);
  bool merge_context_collation_override(const Lex_context_collation &cl);
  /*
    Add an exact collation:
      CHARACTER SET cs [COLLATE cl] ... COLLATE latin1_bin
  */
  bool merge_exact_collation(const Lex_exact_collation &cl);
  Lex_exact_collation collation() const
  {
    return Lex_exact_collation(m_ci);
  }
  Lex_exact_charset charset() const
  {
    if ((m_ci->state & MY_CS_PRIMARY))
      return Lex_exact_charset(m_ci);
    return Lex_exact_charset(find_default_collation());
  }
};


/*
  Parse time character set and collation for:
    [CHARACTER SET cs_exact] [COLLATE cl_exact_or_context]

  Can be:

  1. Empty (not specified on the column level):
     CREATE TABLE t1 (a CHAR(10)) CHARACTER SET latin2;        -- (1a)
     CREATE TABLE t1 (a CHAR(10));                             -- (1b)

  2. Precisely typed:
     CREATE TABLE t1 (a CHAR(10) COLLATE latin1_bin);          -- (2a)
     CREATE TABLE t1 (
       a CHAR(10) CHARACTER SET latin1 COLLATE latin1_bin);    -- (2b)

  3. Contextually typed:
     CREATE TABLE t2 (a CHAR(10) BINARY) CHARACTER SET latin2; -- (3a)
     CREATE TABLE t2 (a CHAR(10) BINARY);                      -- (3b)
     CREATE TABLE t2 (a CHAR(10) COLLATE DEFAULT)
       CHARACER SET latin2 COLLATE latin2_bin;                 -- (3c)

  In case of an empty or a contextually typed collation,
  it is a subject to later resolution, when the context
  character set becomes known in the end of the CREATE statement:
  - either after the explicit table level CHARACTER SET, like in (1a,3a,3c)
  - or by the inhereted database level CHARACTER SET, like in (1b,3b)

  Resolution happens in Type_handler::Column_definition_prepare_stage1().
*/
struct Lex_exact_charset_extended_collation_attrs_st
{
public:
  enum Type
  {
    TYPE_EMPTY= 0,
    TYPE_CHARACTER_SET= 1,
    TYPE_COLLATE_EXACT= 2,
    TYPE_COLLATE_CONTEXTUALLY_TYPED= 3
  };

// Number of bits required to store enum Type values

#define LEX_CHARSET_COLLATION_TYPE_BITS 2
  static_assert(((1<<LEX_CHARSET_COLLATION_TYPE_BITS)-1) >=
                 TYPE_COLLATE_CONTEXTUALLY_TYPED,
                 "Lex_exact_charset_extended_collation_attrs_st::Type bits");

protected:
  CHARSET_INFO *m_ci;
  Type m_type;
protected:
  static Type type_from_lex_collation_type(Lex_extended_collation_st::Type type)
  {
    switch (type) {
    case Lex_extended_collation_st::TYPE_EXACT:
      return TYPE_COLLATE_EXACT;
    case Lex_extended_collation_st::TYPE_CONTEXTUALLY_TYPED:
      return TYPE_COLLATE_CONTEXTUALLY_TYPED;
    }
    DBUG_ASSERT(0);
    return TYPE_COLLATE_EXACT;
  }
public:
  void init()
  {
    m_ci= NULL;
    m_type= TYPE_EMPTY;
  }
  void init(CHARSET_INFO *cs, Type type)
  {
    DBUG_ASSERT(cs || type == TYPE_EMPTY);
    m_ci= cs;
    m_type= type;
  }
  void init(const Lex_exact_charset &cs)
  {
    m_ci= cs.charset_info();
    m_type= TYPE_CHARACTER_SET;
  }
  void init(const Lex_exact_collation &cs)
  {
    m_ci= cs.charset_info();
    m_type= TYPE_COLLATE_EXACT;
  }
  void init(const Lex_exact_charset_opt_extended_collate &cscl)
  {
    cscl.with_collate() ? init(cscl.collation()) :
                          init(cscl.charset());
  }
  bool is_empty() const
  {
    return m_type == TYPE_EMPTY;
  }
  void set_charset(CHARSET_INFO *cs)
  {
    DBUG_ASSERT(cs);
    m_ci= cs;
    m_type= TYPE_CHARACTER_SET;
  }
  bool set_charset_collate_default(CHARSET_INFO *cs)
  {
    DBUG_ASSERT(cs);
    if (!(cs= Lex_exact_charset_opt_extended_collate(cs, true).
                find_default_collation()))
      return true;
    m_ci= cs;
    m_type= TYPE_COLLATE_EXACT;
    return false;
  }
  bool set_charset_collate_binary(CHARSET_INFO *cs)
  {
    DBUG_ASSERT(cs);
    if (!(cs= Lex_exact_charset_opt_extended_collate(cs, true).
                find_bin_collation()))
      return true;
    m_ci= cs;
    m_type= TYPE_COLLATE_EXACT;
    return false;
  }
  void set_collate_default()
  {
    m_ci= &my_collation_contextually_typed_default;
    m_type= TYPE_COLLATE_CONTEXTUALLY_TYPED;
  }
  void set_contextually_typed_binary_style()
  {
    m_ci= &my_collation_contextually_typed_binary;
    m_type= TYPE_COLLATE_CONTEXTUALLY_TYPED;
  }
  bool is_contextually_typed_collate_default() const
  {
    return Lex_context_collation(m_ci).is_contextually_typed_collate_default();
  }
  CHARSET_INFO *charset_info() const
  {
    return m_ci;
  }
  Type type() const
  {
    return m_type;
  }
  bool is_contextually_typed_collation() const
  {
    return m_type == TYPE_COLLATE_CONTEXTUALLY_TYPED;
  }
  CHARSET_INFO *resolved_to_character_set(CHARSET_INFO *cs) const;
  /*
    Merge the column CHARACTER SET clause to:
    - an exact collation name
    - a contextually typed collation
    "this" corresponds to `CHARACTER SET xxx [BINARY]`
    "cl" corresponds to the COLLATE clause
  */
  bool merge_column_charset_clause_and_collate_clause(
                    const Lex_exact_charset_extended_collation_attrs_st &cl)
  {
    switch (cl.type()) {
    case TYPE_EMPTY:
      return false;
    case TYPE_COLLATE_EXACT:
      return merge_exact_collation(Lex_exact_collation(cl.charset_info()));
    case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    case TYPE_CHARACTER_SET:
      break;
    }
    DBUG_ASSERT(0);
    return false;
  }
  /*
    This method is used in the "attribute_list" rule to merge two independent
    COLLATE clauses (not belonging to a CHARACTER SET clause).
    "BINARY" and "COLLATE DEFAULT" are not possible
    in an independent COLLATE clause in a column attribute.
  */
  bool merge_column_collate_clause_and_collate_clause(
                    const Lex_exact_charset_extended_collation_attrs_st &cl)
  {
    DBUG_ASSERT(m_type != TYPE_COLLATE_CONTEXTUALLY_TYPED);
    DBUG_ASSERT(m_type != TYPE_CHARACTER_SET);
    switch (cl.type()) {
    case TYPE_EMPTY:
      return false;
    case TYPE_COLLATE_EXACT:
      return merge_exact_collation(Lex_exact_collation(cl.charset_info()));
    case TYPE_COLLATE_CONTEXTUALLY_TYPED:
    case TYPE_CHARACTER_SET:
      break;
    }
    DBUG_ASSERT(0);
    return false;
  }
  bool merge_exact_charset(const Lex_exact_charset &cs);
  bool merge_exact_collation(const Lex_exact_collation &cl);
  bool merge_context_collation(const Lex_context_collation &cl);
  bool merge_collation(const Lex_extended_collation_st &cl);
};


class Charset_collation_context
{
  /*
    Although the goal of m_charset_default is to store the meaning
    of CHARACTER SET DEFAULT, it does not necessarily point to a
    default collation of CHARACTER SET DEFAULT. It can point to its any
    arbitrary collation.
    For performance purposes we don't need to find the default
    collation at the instantiation time of "this", because:
    - m_charset_default may not be even needed during the resolution
    - when it's needed, in many cases it's passed to my_charset_same(),
      which does not need the default collation again.

    Note, m_charset_default and m_collate_default are not necessarily equal.

    - The default value for CHARACTER SET is taken from the upper level:
        CREATE DATABASE db1 CHARACTER SET DEFAULT; <-- @@character_set_server
        ALTER DATABASE db1 CHARACTER SET DEFAULT;  <-- @@character_set_server

    - The default value for COLLATE is taken from the upper level for CREATE:
        CREATE DATABASE db1 COLLATE DEFAULT; <-- @@collation_server
        CREATE TABLE db1.t1 COLLATE DEFAULT; <-- character set of "db1"

    - The default value for COLLATE is taken from the same level for ALTER:
        ALTER DATABASE db1 COLLATE DEFAULT; <-- the default collation of the
                                                current db1 character set
        ALTER TABLE db1.t1 COLLATE DEFAULT; <-- the default collation of the
                                                current db1.t1 character set
  */

  // comes from the upper level
  Lex_exact_charset_opt_extended_collate m_charset_default;

  // comes from the upper or the current level
  Lex_exact_collation m_collate_default;
public:
  Charset_collation_context(CHARSET_INFO *charset_default,
                            CHARSET_INFO *collate_default)
   :m_charset_default(charset_default,
                      !(charset_default->state & MY_CS_PRIMARY)),
    m_collate_default(collate_default)
  { }
  const Lex_exact_charset_opt_extended_collate charset_default() const
  {
    return m_charset_default;
  }
  const Lex_exact_collation collate_default() const
  {
    return m_collate_default;
  }
};


/*
  A universal container. It can store at the same time:
  - CHARACTER SET DEFAULT
  - CHARACTER SET cs_exact
  - COLLATE {cl_exact|cl_context}
  All three parts can co-exist.
  All three parts are optional.
  Parts can come in any arbitrary order, e.g:

    CHARACTER SET DEFAULT [CHARACTER SET latin1] COLLATE latin1_bin
    CHARACTER SET latin1 CHARACTER SET DEFAULT COLLATE latin1_bin
    COLLATE latin1_bin [CHARACTER SET latin1] CHARACTER SET DEFAULT
    COLLATE latin1_bin CHARACTER SET DEFAULT [CHARACTER SET latin1]
*/
class Lex_extended_charset_extended_collation_attrs_st:
                        public Lex_opt_context_charset_st,
                        public Lex_exact_charset_extended_collation_attrs_st
{
  enum charset_type_t
  {
    CHARSET_TYPE_EMPTY,
    CHARSET_TYPE_CONTEXT,
    CHARSET_TYPE_EXACT
  };
  /*
    Which part came first:
    - CHARACTER SET DEFAULT or
    - CHARACTER SET cs_exact
    e.g. to produce error messages preserving the user typed
    order of CHARACTER SET clauses in case of conflicts.
  */
  charset_type_t m_charset_order;
  /*
    The parent class Lex_exact_charset_extended_collation_attrs_st
    does not let know if a "COLLATE cl_exact" was used in combination with
    "CHARACTER SET cs_exact" or just alone.
    Here we need to distinguish:
    - CHARACTER SET cs_exact COLLATE cl_exact, or
    - COLLATE cl_exact CHARACTER SET cs_exact
    versus just:
    - COLLATE cl_exact
    to produce better error messages in case of conflicts.
    So let's add a flag member:
  */
  bool m_had_charset_exact;
public:
  void init()
  {
    Lex_opt_context_charset_st::init();
    Lex_exact_charset_extended_collation_attrs_st::init();
    m_charset_order= CHARSET_TYPE_EMPTY;
    m_had_charset_exact= false;
  }
  void init(const Lex_exact_charset_opt_extended_collate &c)
  {
    Lex_opt_context_charset_st::init();
    Lex_exact_charset_extended_collation_attrs_st::init(c);
    m_charset_order= CHARSET_TYPE_EXACT;
    m_had_charset_exact= true;
  }
  bool is_empty() const
  {
    return Lex_opt_context_charset_st::is_empty() &&
           Lex_exact_charset_extended_collation_attrs_st::is_empty();
  }
  bool raise_if_charset_conflicts_with_default(
                        const Lex_exact_charset_opt_extended_collate &def) const;
  CHARSET_INFO *resolved_to_context(const Charset_collation_context &ctx) const;
  bool merge_charset_default();
  bool merge_exact_charset(const Lex_exact_charset &cs);
};


class Lex_exact_charset_extended_collation_attrs:
                        public Lex_exact_charset_extended_collation_attrs_st
{
public:
  Lex_exact_charset_extended_collation_attrs()
  {
    init();
  }
  Lex_exact_charset_extended_collation_attrs(CHARSET_INFO *collation, Type type)
  {
    init(collation, type);
  }
  explicit
  Lex_exact_charset_extended_collation_attrs(const Lex_exact_charset &cs)
  {
    init(cs.charset_info(), TYPE_CHARACTER_SET);
  }
  explicit
  Lex_exact_charset_extended_collation_attrs(const Lex_exact_collation &cl)
  {
    init(cl.charset_info(), TYPE_COLLATE_EXACT);
  }
  explicit
  Lex_exact_charset_extended_collation_attrs(const Lex_context_collation &cl)
  {
    init(cl.charset_info(), TYPE_COLLATE_CONTEXTUALLY_TYPED);
  }
  explicit
  Lex_exact_charset_extended_collation_attrs(
                    const Lex_exact_charset_opt_extended_collate &cscl)
  {
    init(cscl);
  }
  explicit
  Lex_exact_charset_extended_collation_attrs(const Lex_extended_collation_st &cl)
  {
    init(cl.charset_info(), type_from_lex_collation_type(cl.type()));
  }
  static Lex_exact_charset_extended_collation_attrs national(bool bin_mod)
  {
    return bin_mod ?
      Lex_exact_charset_extended_collation_attrs(&my_charset_utf8mb3_bin,
                                                 TYPE_COLLATE_EXACT) :
      Lex_exact_charset_extended_collation_attrs(&my_charset_utf8mb3_general_ci,
                                                 TYPE_CHARACTER_SET);
  }
};


class Lex_extended_charset_extended_collation_attrs:
  public Lex_extended_charset_extended_collation_attrs_st
{
public:
  Lex_extended_charset_extended_collation_attrs()
  {
    init();
  }
  explicit Lex_extended_charset_extended_collation_attrs(
    const Lex_exact_charset_opt_extended_collate &c)
  {
    init(c);
  }
};



using Lex_column_charset_collation_attrs_st =
        Lex_exact_charset_extended_collation_attrs_st;

using Lex_column_charset_collation_attrs =
        Lex_exact_charset_extended_collation_attrs;


using Lex_table_charset_collation_attrs_st =
        Lex_extended_charset_extended_collation_attrs_st;

using Lex_table_charset_collation_attrs =
        Lex_extended_charset_extended_collation_attrs;


#endif // LEX_CHARSET_INCLUDED
