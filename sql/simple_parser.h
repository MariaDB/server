#ifndef SIMPLE_PARSER_H
#define SIMPLE_PARSER_H
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

class Parser_templates
{
protected:

  // Templates to parse common rule sequences

  /*
    A rule consisting of a single token, e.g.:
      rule ::= @
      rule ::= IDENT
  */
  template<class PARSER, typename PARSER::TokenID tid>
  class TOKEN: public PARSER::Token
  {
  public:
    TOKEN()
    { }
    TOKEN(const class PARSER::Token &tok)
     :PARSER::Token(tok)
    { }
    TOKEN(PARSER *p)
     :PARSER::Token(p->token(tid))
    { }
    static TOKEN empty(const PARSER &p)
    {
      return TOKEN(p.empty_token());
    }
  };

  /*
    A rule consisting of a choice of multiple tokens
      rule ::= TOK1 | TOK2 | TOK3
  */
  template<class PARSER, class COND>
  class TokenChoice: public PARSER::Token
  {
  public:
    TokenChoice()
    { }
    TokenChoice(PARSER *p)
     :PARSER::Token(COND::allowed_token_id(p->look_ahead_token_id()) ?
                    p->shift() :
                    p->null_token())
    {
      DBUG_ASSERT(!p->is_error() || !PARSER::Token::operator bool());
    }
  };

  /*
    An optional rule:
      opt_rule ::= [ rule ]
  */
  template<class PARSER, class RULE>
  class OPT: public RULE
  {
  public:
    OPT()
    { }
    OPT(PARSER *p)
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
    A parenthesized rule:
      parenthesized_rule ::= ( rule )
  */
  template<class PARSER, class RULE,
           typename PARSER::TokenID tLPAREN,
           typename PARSER::TokenID tRPAREN>
  class PARENTHESIZED: public RULE
  {
  public:
    PARENTHESIZED()
     :RULE()
    {
      DBUG_ASSERT(!RULE::operator bool());
    }
    PARENTHESIZED(PARSER *p)
     :RULE(p->token(tLPAREN) ? RULE(p) : RULE())
    {
      if (!RULE::operator bool() || !p->token(tRPAREN))
      {
        p->set_syntax_error(); // TODO: handle fatal error differently?
        // Reset RULE so "this" is reported as "false".
        RULE::operator=(RULE());
        DBUG_ASSERT(!RULE::operator bool());
      }
    }
  };

  /*
    A rule consisting of two other rules in a row:
      rule ::= rule1 rule2
  */
  template<class PARSER, class A, class B>
  class AND2: public A, public B
  {
  public:
    AND2()
     :A(), B()
    { }
    AND2(const A &a, const B &b)
     :A(a), B(b)
    { }
    AND2(PARSER *p)
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
    static AND2 empty(const PARSER &p)
    {
      return AND2(A::empty(p), B::empty(p));
    }
  };


  /*
    A rule consisting of a choice of rwo rules:
      rule ::= rule1 | rule2

    For the cases when the two branches have incompatible storage.
  */
  template<class PARSER, class A, class B>
  class OR2: public A, public B
  {
  public:
    OR2()
    { }
    OR2(PARSER *p)
    {
      if (A::operator=(A(p)) || B::operator=(B(p)))
      {
        DBUG_ASSERT(!p->is_error());
        DBUG_ASSERT(operator bool());
        return;
      }
      DBUG_ASSERT(!operator bool());
    }
    operator bool() const
    {
      return A::operator bool() || B::operator bool();
    }
  };


  /*
    A rule consisting of a choice of rwo rules, e.g.
      rule ::= rule1 | rule2

    For the cases when the two branches have a compatible storage,
    passed as a CONTAINER, which must have constructors:
      CONTAINER(const A &a)
      CONTAINER(const B &b)
  */
  template<class PARSER, class CONTAINER, class A, class B>
  class OR2C: public CONTAINER
  {
  public:
    OR2C()
    { }
    OR2C(const A &a)
     :CONTAINER(a)
    { }
    OR2C(const B &b)
     :CONTAINER(b)
    { }
    OR2C(PARSER *p)
    {
      if (const A a= A(p))
      {
        *this= a;
        DBUG_ASSERT(CONTAINER::operator bool());
        return;
      }
      if (const B b= B(p))
      {
        *this= b;
        DBUG_ASSERT(CONTAINER::operator bool());
        return;
      }
      DBUG_ASSERT(!CONTAINER::operator bool());
    }
  };


  /*
    A rule consisting of a choice of thee rules:
      rule ::= rule1 | rule2 | rule3

    For the case when the three branches have incompatible storage
  */
  template<class PARSER, class A, class B, class C>
  class OR3: public A, public B, public C
  {
  public:
    OR3()
    { }
    OR3(PARSER *p)
    {
      if (A::operator=(A(p)) || B::operator=(B(p)) || C::operator=(C(p)))
      {
        DBUG_ASSERT(!p->is_error());
        DBUG_ASSERT(operator bool());
        return;
      }
      DBUG_ASSERT(!operator bool());
    }
    operator bool() const
    {
      return A::operator bool() || B::operator bool() || C::operator bool();
    }
  };

  /*
    A list with at least MIN_COUNT elements (typlically 0 or 1),
    with or without a token separator between elements:

      list ::= element [ {, element }... ]       // with a separator
      list ::= element [    element  ... ]       // without a separator

    Pass the null-token special purpose ID in SEP for a non-separated list,
    or a real token ID for a separated list.

    If MIN_COUNT is 0, then the list becomes optional,
    which corresponds to the following grammar:

      list ::= [ element [ {, element }... ] ]   // with a separator
      list ::= [ element [    element  ... ] ]   // without a separator
  */
  template<class PARSER,
           class LIST_CONTAINER, class ELEMENT,
           typename PARSER::TokenID SEP, size_t MIN_COUNT>
  class LIST: public LIST_CONTAINER
  {
  protected:
    bool m_error;
  public:
    LIST()
     :m_error(true)
    { }
    LIST(PARSER *p)
     :m_error(true)
    {
      // Determine if the caller wants a separated or a non-separated list
      const bool separated= SEP != PARSER::null_token().id();
      for ( ; ; )
      {
        const ELEMENT elem(p);
        if (!elem)
        {
          if (LIST_CONTAINER::count() == 0 || !separated)
          {
            /*
              Could not get the very first element,
              or not-first element in a non-separated list.
            */
            m_error= p->is_error();
            DBUG_ASSERT(!m_error || !operator bool());
            return;
          }
          // Could not get the next element after the separator
          p->set_syntax_error();
          m_error= true;
          DBUG_ASSERT(!operator bool());
          return;
        }
        if (LIST_CONTAINER::add(p, elem))
        {
          p->set_fatal_error();
          m_error= true;
          DBUG_ASSERT(!operator bool());
          return;
        }
        if (separated)
        {
          if (!p->token(SEP))
          {
            m_error= false;
            DBUG_ASSERT(operator bool());
            return;
          }
        }
      }
    }
    operator bool() const
    {
      return !m_error && LIST_CONTAINER::count() >= MIN_COUNT;
    }
  };

};

#endif // SIMPLE_PARSER_H
