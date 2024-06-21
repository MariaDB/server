#ifndef OPT_HINTS_H
#define OPT_HINTS_H
/*
   Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/


#include "simple_tokenizer.h"
#include "sql_list.h"

class Optimizer_hint_tokenizer: public Extended_string_tokenizer
{
public:
  Optimizer_hint_tokenizer(CHARSET_INFO *cs, const LEX_CSTRING &hint)
   :Extended_string_tokenizer(cs, hint)
  { }
  enum TokenID
  {
    // Special tokens
    tEOF=   0,
    tERROR= 1,
    tEMPTY= 2,
    // One character tokens
    tCOMMA= ',',
    tAT= '@',
    tLPAREN= '(',
    tRPAREN= ')',
    // Keywords
    tBKA,
    tBNL,
//    tDUPSWEEDOUT,
//    tFIRSTMATCH,
//    tINTOEXISTS,
//    tLOOSESCAN,
//    tMATERIALIZATION,
    tNO_BKA,
    tNO_BNL,
    tNO_ICP,
    tNO_MRR,
    tNO_RANGE_OPTIMIZATION,
//    tNO_SEMIJOIN,
    tMRR,
    tQB_NAME,
//    tSEMIJOIN,
//    tSUBQUERY,
//    tDEBUG_HINT1,
//    tDEBUG_HINT2,
//    tDEBUG_HINT3,
    // Other tokens
    tIDENT
  };

protected:

  TokenID find_keyword(const LEX_CSTRING &str)
  {
    switch (str.length)
    {
    case 3:
      if ("BKA"_Lex_ident_column.streq(str)) return tBKA;
      if ("BNL"_Lex_ident_column.streq(str)) return tBNL;
      if ("MRR"_Lex_ident_column.streq(str)) return tMRR;
      break;

    case 6:
      if ("NO_BKA"_Lex_ident_column.streq(str)) return tNO_BKA;
      if ("NO_BNL"_Lex_ident_column.streq(str)) return tNO_BNL;
      if ("NO_ICP"_Lex_ident_column.streq(str)) return tNO_ICP;
      if ("NO_MRR"_Lex_ident_column.streq(str)) return tNO_MRR;
      break;

    case 7:
      if ("QB_NAME"_Lex_ident_column.streq(str)) return tQB_NAME;
      break;

//    case 8:
//      if ("SEMIJOIN"_Lex_ident_column.streq(str)) return tSEMIJOIN;
//      if ("SUBQUERY"_Lex_ident_column.streq(str)) return tSUBQUERY;
//      break;

//    case 9:
//    if ("LOOSESCAN"_Lex_ident_column.streq(str)) return tLOOSESCAN;
//      break;

//    case 10:
//      if ("FIRSTMATCH"_Lex_ident_column.streq(str)) return tFIRSTMATCH;
//      if ("INTOEXISTS"_Lex_ident_column.streq(str)) return tINTOEXISTS;
//      break;

//    case 11:
//      if ("DUPSWEEDOUT"_Lex_ident_column.streq(str)) return tDUPSWEEDOUT;
//      if ("NO_SEMIJOIN"_Lex_ident_column.streq(str)) return tNO_SEMIJOIN;
//      if ("DEBUG_HINT1"_Lex_ident_column.streq(str)) return tDEBUG_HINT1;
//      if ("DEBUG_HINT2"_Lex_ident_column.streq(str)) return tDEBUG_HINT2;
//      if ("DEBUG_HINT3"_Lex_ident_column.streq(str)) return tDEBUG_HINT3;
//      break;

//    case 15:
//      if ("MATERIALIZATION"_Lex_ident_column.streq(str))
//        return tMATERIALIZATION;
//      break;

    case 21:
      if ("NO_RANGE_OPTIMIZATION"_Lex_ident_column.streq(str))
        return tNO_RANGE_OPTIMIZATION;
      break;
    }
    return tIDENT;
  }

public:

  class Token: public Lex_cstring
  {
  protected:
    TokenID m_id;
  public:
    Token()
     :Lex_cstring(), m_id(tERROR)
    { }
    Token(const LEX_CSTRING &str, TokenID id)
     :Lex_cstring(str), m_id(id)
    { }
    TokenID id() const { return m_id; }
    static Token empty(const char *pos)
    {
      return Token(Lex_cstring(pos, pos), tEMPTY);
    }
    operator bool() const
    {
      return m_id != Optimizer_hint_tokenizer::tERROR;
    }
  };

  Token get_token(CHARSET_INFO *cs)
  {
    get_spaces();
    if (eof())
      return Token(Lex_cstring(m_ptr, m_ptr), tEOF);
    const char head= m_ptr[0];
    if (head == '`' || head=='"')
    {
      const Token_with_metadata delimited_ident= get_quoted_string();
      if (delimited_ident.length)
        return Token(delimited_ident, tIDENT);
    }
    const Token_with_metadata ident= get_ident();
    if (ident.length)
      return Token(ident, ident.m_extended_chars ?
                   tIDENT : find_keyword(ident));
    if (!get_char(','))
      return Token(Lex_cstring(m_ptr - 1, 1), tCOMMA);
    if (!get_char('@'))
      return Token(Lex_cstring(m_ptr - 1, 1), tAT);
    if (!get_char('('))
      return Token(Lex_cstring(m_ptr - 1, 1), tLPAREN);
    if (!get_char(')'))
      return Token(Lex_cstring(m_ptr - 1, 1), tRPAREN);
    return Token(Lex_cstring(m_ptr, m_ptr), tERROR);
  }
};


class Optimizer_hint_parser: public Optimizer_hint_tokenizer
{
private:
  Token m_look_ahead_token;
  THD *m_thd;
  bool m_syntax_error;
  bool m_fatal_error;
public:
  Optimizer_hint_parser(THD *thd, CHARSET_INFO *cs, const LEX_CSTRING &hint)
   :Optimizer_hint_tokenizer(cs, hint),
    m_look_ahead_token(get_token(cs)),
    m_thd(thd),
    m_syntax_error(false),
    m_fatal_error(false)
  { }
private:
  bool set_syntax_error()
  {
    m_syntax_error= true;
    return false;
  }
  bool set_fatal_error()
  {
    m_fatal_error= true;
    return false;
  }
  TokenID look_ahead_token_id() const
  {
    return is_error() ? tERROR : m_look_ahead_token.id();
  }
  Token empty_token() const
  {
    return Token::empty(m_look_ahead_token.str);
  }

  /*
    Return the current look ahead token and scan the next one
  */
  Token shift()
  {
    DBUG_ASSERT(!is_error());
    const Token res= m_look_ahead_token;
    m_look_ahead_token= get_token(m_cs);
    return res;
  }

  /*
    Return the current look ahead token if it matches the given ID
    and scan the next one.
  */
  Token token(TokenID id)
  {
    if (m_look_ahead_token.id() != id || is_error())
      return Token();
    return shift();
  }

  /*
    A rule consisting of a single token, e.g.:
      rule ::= @
      rule ::= tIDENT
  */
  template<TokenID tid>
  class TOKEN: public Token
  {
  public:
    TOKEN()
    { }
    TOKEN(const Token &tok)
     :Token(tok)
    { }
    TOKEN(Optimizer_hint_parser *p)
     :Token(p->token(tid))
    { }
    static TOKEN empty(const Optimizer_hint_parser &p)
    {
      return TOKEN(p.empty_token());
    }
  };

  /*
    Parse an optional rule:
      opt_rule ::= [ rule ]
  */
  template<class RULE>
  class OPT: public RULE
  {
  public:
    OPT()
    { }
    OPT(Optimizer_hint_parser *p)
     :RULE(p)
    {
      if (!RULE::operator bool() && !p->is_error())
      {
        RULE::operator=(RULE::empty(*p));
        DBUG_ASSERT(RULE::operator bool());
      }
    }
  };

  /*
    Parse a parenthesized rule:
      parenthesized_rule ::= ( rule )
  */
  template<class RULE>
  class PARENTHESIZED: public RULE
  {
  public:
    PARENTHESIZED()
     :RULE()
    {
      DBUG_ASSERT(!RULE::operator bool());
    }
    PARENTHESIZED(Optimizer_hint_parser *p)
     :RULE(p->token(tLPAREN) ? RULE(p) : RULE())
    {
      if (!RULE::operator bool() || !p->token(tRPAREN))
      {
        p->set_syntax_error(); // TODO: fatal error?
        // Reset RULE so "this" is reported as "false".
        RULE::operator=(RULE());
        DBUG_ASSERT(!RULE::operator bool());
      }
    }
  };

  /*
    Parse a rule consisting of two other rules in a row:
      rule ::= rule1 rule2
  */
  template<class A, class B>
  class AND2: public A, public B
  {
  public:
    AND2()
     :A(), B()
    { }
    AND2(const A &a, const B &b)
     :A(a), B(b)
    { }
    AND2(Optimizer_hint_parser *p)
     :A(p),
      B(A::operator bool() ? B(p) : B())
    {
      if ((A::operator bool() && !B::operator bool()))
      {
        p->set_syntax_error();
        // Reset A to make both A and B reported as "false".
        A::operator=(A());
      }
    }
    operator bool() const
    {
      return A::operator bool() && B::operator bool();
    }
  };

  /*
    A list with at least one element and a token separator between elements:
      list1 ::= elem [ {, elem }... ]
  */
  template<class LIST_CONTAINER, class ELEMENT, TokenID SEP, size_t MIN_COUNT>
  class LIST_SEP: public LIST_CONTAINER
  {
  protected:
    bool m_error;
  public:
    bool add(Optimizer_hint_parser *p, const ELEMENT &elem)
    {
      if (LIST_CONTAINER::add(p, elem))
      {
        p->set_fatal_error();
        return m_error= true;
      }
      return false;
    }
    LIST_SEP()
     :LIST_CONTAINER(), m_error(true)
    { }
    LIST_SEP(Optimizer_hint_parser *p)
     :LIST_CONTAINER(), m_error(true)
    {
      for ( ; ; )
      {
        const ELEMENT elem(p);
        if (!elem)
        {
          if (LIST_CONTAINER::count() == 0)
          {
            // Could not get the very first element
            m_error= p->is_error();
          }
          else
          {
            // Could not get the next element after the separator
            p->set_syntax_error();
            m_error= true;
            DBUG_ASSERT(!operator bool());
          }
          return;
        }
        if (LIST_CONTAINER::add(p, elem))
        {
          p->set_fatal_error();
          m_error= true;
          DBUG_ASSERT(!operator bool());
          return;
        }
        if (!p->token(SEP))
        {
          m_error= false;
          DBUG_ASSERT(operator bool());
          return;
        }
      }
    }
    operator bool() const
    {
      return !m_error && LIST_CONTAINER::count() >= MIN_COUNT;
    }
  };

public:

  bool is_error() const
  {
    return m_syntax_error || m_fatal_error;
  }
  bool is_syntax_error() const
  {
    return m_syntax_error;
  }
  bool is_fatal_error() const
  {
    return m_fatal_error;
  }

  bool parse_token_list(THD *thd); // For debug purposes

  void push_warning_syntax_error(THD *thd);

  /*
    The main parsing routine:

    optimizer_hints ::= hist_list EOF
  */
  bool parse()
  {
    return Hint_list(this) && token(tEOF);
  }


private:

  // Rules consisting of a single token

  class TokenAT: public TOKEN<tAT>
  {
  public:
    using TOKEN::TOKEN;
  };

  class TokenQB_NAME: public TOKEN<tQB_NAME>
  {
  public:
    using TOKEN::TOKEN;
  };


  // Rules consisting of multiple choices of tokens

  /*
    table_level_hint_type ::= BKA | BNL | NO_BKA | NO_BNL
  */
  class Table_level_hint_type: public Token
  {
  public:
    Table_level_hint_type()
    { }
    Table_level_hint_type(Optimizer_hint_parser *p)
     :Token(allowed_token_id(p->look_ahead_token_id()) ? p->shift() :
                                                         Token())
    { }
    static bool allowed_token_id(TokenID id)
    {
      return id == tBKA || id == tBNL || id == tNO_BKA || id == tNO_BNL;
    }
  };

  /*
    index_level_hint_type ::= MRR | NO_RANGE_OPTIMIZATION | NO_ICP | NO_MRR
  */
  class Index_level_hint_type: public Token
  {
  public:
    Index_level_hint_type()
    { }
    Index_level_hint_type(Optimizer_hint_parser *p)
     :Token(allowed_token_id(p->look_ahead_token_id()) ? p->shift() :
                                                         Token())
    { }
    static bool allowed_token_id(TokenID id)
    {
      return id == tMRR || id == tNO_RANGE_OPTIMIZATION ||
             id == tNO_ICP || id == tNO_MRR;
    }
  };


  // More complex rules

  class Identifier: public Token
  {
  public:
    Identifier()
    { }
    Identifier(const Token &tok)
     :Token(tok)
    { }
    Identifier(Optimizer_hint_parser *p)
     :Token(p->token(tIDENT))
    { }
    static Identifier empty(const Optimizer_hint_parser &p)
    {
      return Identifier(p.empty_token());
    }
  };

  /*
    query_block_name ::= identifier
  */
  class Query_block_name: public Identifier
  {
  public:
    using Identifier::Identifier;
  };

  /*
    parenthesized_query_block_name ::= ( query_block_name )
  */
  class Parenthesized_query_block_name: public PARENTHESIZED<Query_block_name>
  {
  public:
    using PARENTHESIZED::PARENTHESIZED;
  };

  /*
    at_query_block_name ::= @ query_block_name
  */
  class At_query_block_name: public AND2<TokenAT, Query_block_name>
  {
  public:
    using AND2::AND2;
    static At_query_block_name empty(const Optimizer_hint_parser &p)
    {
      return At_query_block_name(TokenAT::empty(p), Query_block_name::empty(p));
    }
  };

  /*
    opt_qb_name ::=  [ @ query_block_name ]
  */
  class Opt_qb_name: public OPT<At_query_block_name>
  {
  public:
    using OPT::OPT;
  };

  /*
    table_name ::= identifier
  */
  class Table_name: public Identifier
  {
  public:
    using Identifier::Identifier;
  };

  /*
    hint_param_table_empty_qb ::= table_name
  */
  class Hint_param_table_empty_qb: public Table_name
  {
  public:
    using Table_name::Table_name;
  };

  /*
    hint_param_index ::= identifier
  */
  class Hint_param_index: public Identifier
  {
  public:
    using Identifier::Identifier;
  };



  /*
    hint_param_table ::= table_name opt_qb_name
  */
  class Hint_param_table: public AND2<Table_name, Opt_qb_name>
  {
  public:
    using AND2::AND2;
  };


  /*
    hint_param_table_list ::= hint_param_table [ {, hint_param_table}... ]
    opt_hint_param_table_list ::= [ hint_param_table_list ]
  */
  class Hint_param_table_list_container: public List<Hint_param_table>
  {
  public:
    Hint_param_table_list_container()
    { }
    bool add(Optimizer_hint_parser *p, const Hint_param_table &table);
    size_t count() const { return elements; }
  };

  class Opt_hint_param_table_list: public LIST_SEP<
                                            Hint_param_table_list_container,
                                            Hint_param_table, tCOMMA, 0>
  {
    using LIST_SEP::LIST_SEP;
  };

  /*
    table_name_list ::= table_name [ {, table_name }... ]
    opt_table_name_list ::= [ table_name_list ]
  */
  class Table_name_list_container: public List<Table_name>
  {
  public:
    Table_name_list_container()
    { }
    bool add(Optimizer_hint_parser *p, const Table_name &table);
    size_t count() const { return elements; }
  };

  class Opt_table_name_list: public LIST_SEP<Table_name_list_container,
                                             Table_name, tCOMMA, 0>
  {
  public:
    using LIST_SEP::LIST_SEP;
  };


  /*
    hint_param_index_list ::= hint_param_index [ {, hint_param_index }...]
    opt_hint_param_index_list ::= [ hint_param_index_list ]
  */
  class Hint_param_index_list_container: public List<Hint_param_index>
  {
  public:
    Hint_param_index_list_container()
    { }
    bool add(Optimizer_hint_parser *p, const Hint_param_index &table);
    size_t count() const { return elements; }
  };

  class Opt_hint_param_index_list: public LIST_SEP<
                                            Hint_param_index_list_container,
                                            Hint_param_index, tCOMMA, 0>
  {
  public:
    using LIST_SEP::LIST_SEP;
  };


  /*
    at_query_block_name_table_name ::= @ query_block_name table_name
  */
  class At_query_block_name_table_name: public AND2<At_query_block_name,
                                                    Table_name>
  {
  public:
    using AND2::AND2;
  };

  /*
    hint_param_table_ext ::=   hint_param_table
                             | @ query_block_name table_name
  */
  class Hint_param_table_ext: public Query_block_name,
                              public Table_name
  {
  public:
    Hint_param_table_ext()
    { }
    Hint_param_table_ext(const Hint_param_table &hint_param_table)
     :Query_block_name(hint_param_table), Table_name(hint_param_table)
    { }
    Hint_param_table_ext(const At_query_block_name_table_name &qb_table)
     :Query_block_name(qb_table), Table_name(qb_table)
    { }
    Hint_param_table_ext(Optimizer_hint_parser *p)
     :Query_block_name(), Table_name()
    {
      const Hint_param_table table(p);
      if (table)
      {
        *this= Hint_param_table_ext(table);
        DBUG_ASSERT(operator bool());
        return;
      }
      const At_query_block_name_table_name qs(p);
      if (qs)
      {
        *this= Hint_param_table_ext(qs);
        DBUG_ASSERT(operator bool());
        return;
      }
      DBUG_ASSERT(!operator bool());
    }
    operator bool() const
    {
      return Query_block_name::operator bool() && Table_name::operator bool();
    }
  };

  /*
    at_query_block_name_opt_table_name_list ::=
      @ query_block_name opt_table_name_list
  */
  class At_query_block_name_opt_table_name_list: public AND2<
                                                    At_query_block_name,
                                                    Opt_table_name_list>
  {
  public:
    using AND2::AND2;
  };

  /*
    table_level_hint_body:   @ query_block_name opt_table_name_list
                           | opt_hint_param_table_list
  */
  class Table_level_hint_body: public At_query_block_name_opt_table_name_list,
                               public Opt_hint_param_table_list
  {
  public:
    Table_level_hint_body()
    { }
    Table_level_hint_body(Optimizer_hint_parser *p)
    {
      const At_query_block_name_opt_table_name_list a(p);
      if (a)
      {
        At_query_block_name_opt_table_name_list::operator=(a);
        DBUG_ASSERT(operator bool());
        return;
      }
      const Opt_hint_param_table_list b(p);
      if (b)
      {
        Opt_hint_param_table_list::operator=(b);
        DBUG_ASSERT(operator bool());
        return;
      }
      DBUG_ASSERT(!operator bool());
    }
    operator bool() const
    {
      return At_query_block_name_opt_table_name_list::operator bool() ||
             Opt_hint_param_table_list::operator bool();
    }
  };


  /*
    table_level_hint ::= table_level_hint_type ( table_level_hint_body )
  */
  class Parenthesized_table_level_hint_body: public PARENTHESIZED<
                                                      Table_level_hint_body>
  {
  public:
    using PARENTHESIZED::PARENTHESIZED;
  };

  class Table_level_hint: public AND2<Table_level_hint_type,
                                      Parenthesized_table_level_hint_body>
  {
  public:
    using AND2::AND2;
  };


  /*
    index_level_hint ::=   key_level_hint_type
                           ( hint_param_table_ext opt_hint_param_index_list )
  */
  class Index_level_hint: public Index_level_hint_type,
                          public Hint_param_table_ext,
                          public Opt_hint_param_index_list
  {
    bool m_error;
  public:
    Index_level_hint()
    { }
    Index_level_hint(Optimizer_hint_parser *p)
     :Index_level_hint_type(p),
      Hint_param_table_ext(),
      Opt_hint_param_index_list(),
      m_error(false)
    {
      if (!Index_level_hint_type::operator bool())
      {
        DBUG_ASSERT(!operator bool());
        return;
      }
      if (p->token(tLPAREN) &&
          Hint_param_table_ext::operator=(Hint_param_table_ext(p)) &&
          Opt_hint_param_index_list::operator=(Opt_hint_param_index_list(p)) &&
          p->token(tRPAREN))
      {
        DBUG_ASSERT(operator bool());
        return;
      }
      m_error= true; // Could not parse
      p->set_syntax_error();
      DBUG_ASSERT(!operator bool());
    }
    operator bool() const
    {
      return !m_error &&
             Index_level_hint_type::operator bool() &&
             Hint_param_table_ext::operator bool() &&
             Opt_hint_param_index_list::operator bool();
    }
  };


  /*
    qb_name_hint ::= QB_NAME ( query_block_name )
  */
  class Qb_name_hint: public AND2<TokenQB_NAME, Parenthesized_query_block_name>
  {
  public:
    using AND2::AND2;
  };

  /*
    hint ::=   index_level_hint
             | table_level_hint
             | qb_name_hint
  */
  class Hint: public Index_level_hint,
              public Table_level_hint,
              public Qb_name_hint
  {
  public:
    Hint()
    { }
    Hint(Optimizer_hint_parser *p)
    {
      const Index_level_hint a(p);
      if (a)
      {
        DBUG_ASSERT(!p->is_error());
        Index_level_hint::operator=(a);
        DBUG_ASSERT(operator bool());
        return;
      }
      const Table_level_hint b(p);
      if (b)
      {
        DBUG_ASSERT(!p->is_error());
        Table_level_hint::operator=(b);
        DBUG_ASSERT(operator bool());
        return;
      }
      const Qb_name_hint c(p);
      if (c)
      {
        DBUG_ASSERT(!p->is_error());
        Qb_name_hint::operator=(c);
        DBUG_ASSERT(operator bool());
        return;
      }
      DBUG_ASSERT(!operator bool());
    }
    operator bool() const
    {
      return Index_level_hint::operator bool() ||
             Table_level_hint::operator bool() ||
             Qb_name_hint::operator bool();
    }
  };

  /*
    hint_list ::= hint [ hint... ]
  */
  class Hint_list_container: public List<Hint>
  {
  public:
    Hint_list_container()
    { }
    bool add(Optimizer_hint_parser *p, const Hint &hint);
    size_t count() const { return elements; }
  };

  class Hint_list: public Hint_list_container
  {
  protected:
    bool m_error;
  public:
    Hint_list(Optimizer_hint_parser *p)
     :m_error(true)
    {
      for ( ; ; )
      {
        const Hint hint(p);
        if (!hint)
        {
          /*
            If "hint" was not parsed because it could not even start parsing
            it (found an unexpected token in the very beginning), then m_error
            will be set to "false" and "this" will be evaluated as "true"
            if at least one hint was parsed on an earlier iteration.
            Otherwise, if "hint" consumed some expected token, but then
            it failed on syntax or fatal error, m_error will be set to true,
            and "this" will be evaluated as "false"
          */
          m_error= p->is_error();
          DBUG_ASSERT(!m_error || !operator bool());
          return;
        }
        DBUG_ASSERT(!p->is_error());
        if (Hint_list_container::add(p, hint))
        {
          m_error= true;
          DBUG_ASSERT(!operator bool());
          return;
        }
      }
    }
    operator bool() const
    {
      return !m_error && Hint_list_container::count() > 0;
    }
  };

};

#endif // OPT_HINTS
