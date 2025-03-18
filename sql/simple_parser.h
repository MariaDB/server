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

/*
  A set of templates for constructing a recursive-descent LL(1) parser.

  One is supposed to define classes corresponding to grammar productions.
  The class should inherit from the grammar rule template. For example, a
  grammar rule

    foo := bar, baz

  is implemented with

    class Bar ... ; // "bar" is parsed into Bar object
    class Baz ... ; // "baz" is parsed into Baz object

    // "foo" is parsed into a Foo object.
    class Foo: public Parser_templates::AND2<PARSER, Bar, Baz> {
      using AND2::AND2;
      ...
    };

  Inheriting AND2's constructors with "using" will generate the parsing code.
  Parsing is done by constructing parser output from the parser object:

    Foo parsed_output(parser);

  All parser objects should also have
  - A capability to construct an "empty" or "invalid" object with constructor
    that accepts no arguments. This is necessary when parsing fails.
  - operator bool() which returns true if the object is valid/non-empty and
    false if otherwise.
*/

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
    TOKEN(class PARSER::Token &&tok)
     :PARSER::Token(std::move(tok))
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
    AND2(AND2 && rhs)
     :A(std::move(static_cast<A&&>(rhs))),
      B(std::move(static_cast<B&&>(rhs)))
    { }
    AND2(A &&a, B &&b)
     :A(std::move(a)), B(std::move(b))
    { }
    AND2 & operator=(AND2 &&rhs)
    {
      A::operator=(std::move(static_cast<A&&>(rhs)));
      B::operator=(std::move(static_cast<B&&>(rhs)));
      return *this;
    }
    AND2(PARSER *p)
     :A(p),
      B(A::operator bool() ? B(p) : B())
    {
      if (A::operator bool() && !B::operator bool())
      {
        p->set_syntax_error();
        // Reset A to have A, B reported as "false" by their operator bool()
        A::operator=(std::move(A()));
      }
      DBUG_ASSERT(!operator bool() || !p->is_error());
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
    A rule consisting of three other rules in a row:
      rule ::= rule1 rule2 rule3
  */
  template<class PARSER, class A, class B, class C>
  class AND3: public A, public B, public C
  {
  public:
    AND3()
     :A(), B(), C()
    { }
    AND3(AND3 && rhs)
     :A(std::move(static_cast<A&&>(rhs))),
      B(std::move(static_cast<B&&>(rhs))),
      C(std::move(static_cast<C&&>(rhs)))
    { }
    AND3(A &&a, B &&b, C &&c)
     :A(std::move(a)), B(std::move(b)), C(std::move(c))
    { }
    AND3 & operator=(AND3 &&rhs)
    {
      A::operator=(std::move(static_cast<A&&>(rhs)));
      B::operator=(std::move(static_cast<B&&>(rhs)));
      C::operator=(std::move(static_cast<C&&>(rhs)));
      return *this;
    }
    AND3(PARSER *p)
     :A(p),
      B(A::operator bool() ? B(p) : B()),
      C(A::operator bool() && B::operator bool() ? C(p) : C())
    {
      if (A::operator bool() && (!B::operator bool() || !C::operator bool()))
      {
        p->set_syntax_error();
        // Reset A to have A, B, C reported as "false" by their operator bool()
        A::operator=(A());
        B::operator=(B());
        C::operator=(C());
      }
      DBUG_ASSERT(!operator bool() || !p->is_error());
    }
    operator bool() const
    {
      return A::operator bool() && B::operator bool() && C::operator bool();
    }
    static AND3 empty(const PARSER &p)
    {
      return AND3(A::empty(p), B::empty(p), C::empty());
    }
  };


  /*
    A rule consisting of four other rules in a row:
      rule ::= rule1 rule2 rule3 rule4
  */
  template<class PARSER, class A, class B, class C, class D>
  class AND4: public A, public B, public C, public D
  {
  public:
    AND4()
     :A(), B(), C(), D()
    { }
    AND4(AND4 && rhs)
     :A(std::move(static_cast<A&&>(rhs))),
      B(std::move(static_cast<B&&>(rhs))),
      C(std::move(static_cast<C&&>(rhs))),
      D(std::move(static_cast<D&&>(rhs)))
    { }
    AND4(A &&a, B &&b, C &&c, D &&d)
     :A(std::move(a)), B(std::move(b)), C(std::move(c)), D(std::move(d))
    { }
    AND4 & operator=(AND4 &&rhs)
    {
      A::operator=(std::move(static_cast<A&&>(rhs)));
      B::operator=(std::move(static_cast<B&&>(rhs)));
      C::operator=(std::move(static_cast<C&&>(rhs)));
      D::operator=(std::move(static_cast<D&&>(rhs)));
      return *this;
    }
    AND4(PARSER *p)
     :A(p),
      B(A::operator bool() ? B(p) : B()),
      C(A::operator bool() && B::operator bool() ? C(p) : C()),
      D(A::operator bool() && B::operator bool() && C::operator bool() ?
        D(p) : D())
    {
      if (A::operator bool() &&
          (!B::operator bool() || !C::operator bool() || !D::operator bool()))
      {
        p->set_syntax_error();
        // Reset A to have A, B, C reported as "false" by their operator bool()
        A::operator=(A());
        B::operator=(B());
        C::operator=(C());
        D::operator=(D());
      }
      DBUG_ASSERT(!operator bool() || !p->is_error());
    }
    operator bool() const
    {
      return A::operator bool() && B::operator bool() &&
             C::operator bool() && D::operator bool();
    }
    static AND4 empty(const PARSER &p)
    {
      return AND4(A::empty(p), B::empty(p), C::empty(), D::empty());
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
    OR2(OR2 &&rhs)
     :A(std::move(static_cast<A&&>(rhs))),
      B(std::move(static_cast<B&&>(rhs)))
    { }
    OR2(A && rhs)
     :A(std::move(rhs)), B()
    { }
    OR2(B && rhs)
     :A(), B(std::move(rhs))
    { }
    OR2 & operator=(OR2 &&rhs)
    {
      A::operator=(std::move(static_cast<A&&>(rhs)));
      B::operator=(std::move(static_cast<B&&>(rhs)));
      return *this;
    }
    OR2(PARSER *p)
     :A(p), B(A::operator bool() ? B() :B(p))
    {
      DBUG_ASSERT(!operator bool() || !p->is_error());
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
    OR2C(A &&a)
     :CONTAINER(std::move(a))
    { }
    OR2C(B &&b)
     :CONTAINER(std::move(b))
    { }
    OR2C(OR2C &&rhs)
     :CONTAINER(std::move(rhs))
    { }
    OR2C & operator=(OR2C &&rhs)
    {
      CONTAINER::operator=(std::move(rhs));
      return *this;
    }
    OR2C & operator=(A &&rhs)
    {
      CONTAINER::operator=(std::move(rhs));
      return *this;
    }
    OR2C & operator=(B &&rhs)
    {
      CONTAINER::operator=(std::move(rhs));
      return *this;
    }
    OR2C(PARSER *p)
     :CONTAINER(A(p))
    {
      if (CONTAINER::operator bool() ||
          CONTAINER::operator=(B(p)))
        return;
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
    OR3(OR3 &&rhs)
     :A(std::move(static_cast<A&&>(rhs))),
      B(std::move(static_cast<B&&>(rhs))),
      C(std::move(static_cast<C&&>(rhs)))
    { }
    OR3 & operator=(OR3 &&rhs)
    {
      A::operator=(std::move(static_cast<A&&>(rhs)));
      B::operator=(std::move(static_cast<B&&>(rhs)));
      C::operator=(std::move(static_cast<C&&>(rhs)));
      return *this;
    }
    OR3(PARSER *p)
     :A(p),
      B(A::operator bool() ? B() : B(p)),
      C(A::operator bool() || B::operator bool() ? C() : C(p))
    {
      DBUG_ASSERT(!operator bool() || !p->is_error());
    }
    operator bool() const
    {
      return A::operator bool() || B::operator bool() || C::operator bool();
    }
  };

  /*
    A rule consisting of a choice of three rules, e.g.
      rule ::= rule1 | rule2 | rule3

    For the cases when the three branches have a compatible storage,
    passed as a CONTAINER, which must have constructors:
      CONTAINER(const A &a)
      CONTAINER(const B &b)
      CONTAINER(const C &c)
  */
  template<class PARSER, class CONTAINER, class A, class B, class C>
  class OR3C: public CONTAINER
  {
  public:
    OR3C()
    { }
    OR3C(OR3C &&rhs)
     :CONTAINER(std::move(rhs))
    { }
    OR3C(A &&a)
     :CONTAINER(std::move(a))
    { }
    OR3C(B &&b)
     :CONTAINER(std::move(b))
    { }
    OR3C(C &&c)
     :CONTAINER(std::move(c))
    { }
    OR3C & operator=(OR3C &&rhs)
    {
      CONTAINER::operator=(std::move(rhs));
      return *this;
    }
    OR3C & operator=(A &&rhs)
    {
      CONTAINER::operator=(std::move(rhs));
      return *this;
    }
    OR3C & operator=(B &&rhs)
    {
      CONTAINER::operator=(std::move(rhs));
      return *this;
    }
    OR3C & operator=(C &&rhs)
    {
      CONTAINER::operator=(std::move(rhs));
      return *this;
    }

    OR3C(PARSER *p)
     :CONTAINER(A(p))
    {
      if (CONTAINER::operator bool() ||
          CONTAINER::operator=(B(p)) ||
          CONTAINER::operator=(C(p)))
        return;
      DBUG_ASSERT(!CONTAINER::operator bool());
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
    LIST(LIST &&rhs)
     :LIST_CONTAINER(std::move(rhs)),
      m_error(rhs.m_error)
    { }
    LIST & operator=(LIST &&rhs)
    {
      LIST_CONTAINER::operator=(std::move(rhs));
      m_error= rhs.m_error;
      return *this;
    }
    LIST(PARSER *p)
     :m_error(true)
    {
      // Determine if the caller wants a separated or a non-separated list
      const bool separated= SEP != PARSER::null_token().id();
      for ( ; ; )
      {
        ELEMENT elem(p);
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
        if (LIST_CONTAINER::add(p, std::move(elem)))
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
