/*
   Copyright (c) 2009, 2025, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/**
  @file

  @brief
  This file defines all string functions

  @warning
    Some string functions don't always put and end-null on a String.
    (This shouldn't be needed)
*/

#include "mariadb.h"                          // HAVE_*

#include "sql_priv.h"
/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "sql_base.h"
#include "sql_time.h"
#include "des_key_file.h"       // st_des_keyschedule, st_des_keyblock
#include "password.h"           // my_make_scrambled_password,
                                // my_make_scrambled_password_323
#include <m_ctype.h>
#include <my_md5.h>
C_MODE_START
#include "../mysys/my_static.h"			// For soundex_map
C_MODE_END
#include "sql_show.h"                           // append_identifier
#include <sql_repl.h>
#include "sql_statistics.h"
#include "strfunc.h"
#include "item_numconvfunc.h"

#include "lex_ident_sys.h"
#include "simple_tokenizer.h"
#include "sql_list.h"
#include "sql_string.h"

#define LIST_CONTAINERS_PROVIDE_EMPTY
#include "simple_parser.h"
#undef LIST_CONTAINERS_PROVIDE_EMPTY

class Format_tokenizer: public Extended_string_tokenizer
{
public:
  Format_tokenizer(CHARSET_INFO *cs, const LEX_CSTRING &str)
   :Extended_string_tokenizer(cs, str)
  { }

  enum class TokenID
  {
    // Special purpose tokens:
    tNULL=  0, // returned if the tokenizer failed to detect a token
               // also used if the parser failed to parse a token
    tEMPTY= 1, // returned on empty optional constructs in a grammar like:
               //   rule ::= [ rule1 ]
               // when rule1 does not present in the input.
    tEOF=   2, // returned when the end of input is reached

    // One character tokens
    tCOMMA= ',',
    tDOLLAR= '$',
    tPERIOD= '.',
    tB= 'B',
    tC= 'C',
    tD= 'D',
    tG= 'G',
    tL= 'L',
    tU= 'U',
    tV= 'V',
    tS= 'S',
    // Other tokens, values must be greater than any of the above
    tMI= 256,
    tFM,
    tPR,
    tTM,
    tTM9,
    tTME,
    tZEROS,
    tNINES,
    tXCHAIN,
    tEEEE
  };

  // A helper wrapper for Lex_cstring with convenience methods
  class Lex_cstring_ex: public Lex_cstring
  {
  public:
    using Lex_cstring::Lex_cstring;
    const char *ptr() const { return str; }
    size_t length() const { return Lex_cstring::length; }
    const char *end() const
    {
      return str ? str + Lex_cstring::length : nullptr;
    }
    static Lex_cstring_ex empty()
    {
      return empty_clex_str;
    }
    void print(String *str) const
    {
      str->append(ptr(), length());
    }
  };


  class Token: public Lex_cstring_ex
  {
  protected:
    TokenID m_id;
  public:
    Token()
     :Lex_cstring_ex(), m_id(TokenID::tNULL)
    { }
    Token(const LEX_CSTRING &str, TokenID id)
     :Lex_cstring_ex(str), m_id(id)
    { }
    TokenID id() const { return m_id; }
    static Token empty(const char *pos)
    {
      return Token(Lex_cstring(pos, pos), TokenID::tEMPTY);
    }
    static Token empty()
    {
      return Token(empty_clex_str, TokenID::tEMPTY);
    }
    operator bool() const
    {
      return m_id != TokenID::tNULL;
    }
  };


//protected:
  Token get_token(CHARSET_INFO *cs);
};


static inline constexpr
Format_tokenizer::Lex_cstring_ex operator""_Lex_cstring_ex(const char *str, size_t length)
{
  return Format_tokenizer::Lex_cstring_ex(str, length);
}


Format_tokenizer::Token
Format_tokenizer::get_token(CHARSET_INFO *cs)
{
  if (eof())
    return Token(Lex_cstring(m_ptr, m_ptr), TokenID::tEOF);

  if (!get_char(','))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tCOMMA);
  if (!get_char('$'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tDOLLAR);
  if (!get_char('.'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tPERIOD);
  if (!get_char('B'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tB);
  if (!get_char('C'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tC);
  if (!get_char('D'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tD);
  if (!get_char('G'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tG);
  if (!get_char('L'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tL);
  if (!get_char('U'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tU);
  if (!get_char('V'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tV);
  if (!get_char('S'))
    return Token(Lex_cstring(m_ptr - 1, 1), TokenID::tS);

  const char *head= m_ptr;
  if (head[0] == '0' || head[0] == '9' || head[0] == 'X')
  {
    for ( ; !get_char(head[0]) ;)
    { }
    if (head[0] == '0')
      return Token(Lex_cstring(head, m_ptr), TokenID::tZEROS);
    if (head[0] == '9')
      return Token(Lex_cstring(head, m_ptr), TokenID::tNINES);
    if (head[0] == 'X')
      return Token(Lex_cstring(head, m_ptr), TokenID::tXCHAIN);
  }

  if (m_ptr + 2 <= m_end)
  {
    const LEX_CSTRING str= Lex_cstring(m_ptr, 2);
    if ("MI"_Lex_ident_column.streq(str))
      return m_ptr+=2, Token(str, TokenID::tMI);
    if ("FM"_Lex_ident_column.streq(str))
      return m_ptr+=2, Token(str, TokenID::tFM);
    if ("PR"_Lex_ident_column.streq(str))
      return m_ptr+=2, Token(str, TokenID::tPR);
    if ("TM"_Lex_ident_column.streq(str))
    {
      if (m_ptr + 3 <= m_end)
      {
        if (m_ptr[2] == '9')
          return m_ptr+= 3, Token(Lex_cstring(head, m_ptr), TokenID::tTM9);
        if (m_ptr[2]=='E')
          return m_ptr+= 3, Token(Lex_cstring(head, m_ptr), TokenID::tTME);
      }
      return m_ptr+=2, Token(Lex_cstring(head, m_ptr), TokenID::tTM);
    }
  }

  if (m_ptr + 4 <= m_end)
  {
    const LEX_CSTRING str= Lex_cstring(m_ptr, 4);
    if ("EEEE"_Lex_ident_column.streq(str))
      return m_ptr+=4, Token(str, TokenID::tEEEE);
  }

  return Token(Lex_cstring(m_ptr, m_ptr), TokenID::tNULL);
}




class Format_parser: public Format_tokenizer,
                     public Parser_templates
{
private:
  Token m_look_ahead_token;
  THD *m_thd;
  const char *m_start;
  bool m_syntax_error;
  bool m_fatal_error;
public:
  Format_parser(THD *thd, CHARSET_INFO *cs, const LEX_CSTRING &str)
   :Format_tokenizer(cs, str),
    m_look_ahead_token(get_token(cs)),
    m_thd(thd),
    m_start(str.str),
    m_syntax_error(false),
    m_fatal_error(false)
  { }
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
  // Calculate the line number inside the whole hint
  uint lineno(const char *ptr) const
  {
    DBUG_ASSERT(m_start <= ptr);
    DBUG_ASSERT(ptr <= m_end);
    uint lineno= 0;
    for ( ; ptr >= m_start; ptr--)
    {
      if (*ptr == '\n')
        lineno++;
    }
    return lineno;
  }
  uint lineno() const
  {
    return lineno(m_ptr);
  }


  TokenID look_ahead_token_id() const
  {
    return is_error() ? TokenID::tNULL : m_look_ahead_token.id();
  }
  /*
    Return an empty token at the position of the current
    look ahead token with a zero length. Used for optional grammar constructs.

    For example, if the grammar is "rule ::= ruleA [ruleB] ruleC"
    and the input is "A C", then:
    - the optional rule "ruleB" will point to the input position "C"
      with a zero length
    - while the rule "ruleC" will point to the same input position "C"
      with a non-zero length
  */
  Token empty_token() const
  {
    return Token::empty(m_look_ahead_token.str);
  }
  static Token null_token()
  {
    return Token();
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

public:

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

  Lex_cstring buffer() const
  {
    return Lex_cstring(m_start, m_end);
  }

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


  void parse_error_at(THD *thd, const char *pos) const
  {
    const char *msg= ER_THD(thd, ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR);
    const Lex_cstring buf= buffer();
    const char *bufend= buf.str + buf.length;
    DBUG_ASSERT(pos >= buf.str && pos <= bufend);
    ErrConvString txt(pos, bufend - pos, thd->variables.character_set_client);
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_PARSE_ERROR, ER_THD(thd, ER_PARSE_ERROR),
                        msg, txt.ptr(), 0/*lineno*/);
  }


  void push_warning_syntax_error(THD *thd, uint start_lineno) const
  {
    DBUG_ASSERT(m_start <= m_ptr);
    DBUG_ASSERT(m_ptr <= m_end);
    const char *msg= ER_THD(thd, ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR);
    ErrConvString txt(m_look_ahead_token.str,
                      m_end - m_look_ahead_token.str,
                      thd->variables.character_set_client);
    /*
      start_lineno is the line number on which the whole hint started.
      Add the line number of the current tokenizer position inside the hint
      (in case hints are written in multiple lines).
    */
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_PARSE_ERROR, ER_THD(thd, ER_PARSE_ERROR),
                        msg, txt.ptr(), start_lineno + lineno());
  }


  // A helper method for debugging
  void trace_tokens(CHARSET_INFO *cs, const LEX_CSTRING &fmt) const
  {
    Format_tokenizer t(cs, fmt);
    Format_tokenizer::Token tok;
    for (tok= t.get_token(cs) ;
         tok && tok.id() != Format_tokenizer::TokenID::tEOF;
         tok= t.get_token(cs))
    { }
  }

//private:

  using Parser= Format_parser; // for a shorter notation


  // A common parent class for various containers
  class Lex_cstring_container: public Lex_cstring_ex
  {
  public:
    using Lex_cstring_ex::Lex_cstring_ex;
    static Lex_cstring_container empty(const Parser &parser)
    {
      return parser.empty_token();
    }
    static Lex_cstring_container empty()
    {
      return Lex_cstring_ex::empty();
    }
    operator bool() const
    {
      return Lex_cstring::str != nullptr;
    }
    /*
      Contatenate list elements into a single container.
      There must be no any delimiters in between.
    */
    bool concat(const Lex_cstring && rhs)
    {
      if (!ptr())
      {
        Lex_cstring::operator=(std::move(rhs));
        return false;
      }
      if (rhs.length == 0)
        return false;
      if (end() != rhs.str)
      {
        DBUG_ASSERT(0);
        return true;
      }
      Lex_cstring::length+= rhs.length;
      return false;
    }
    // Methods that allow to pass it as a container to LIST.
    size_t count() const { return length(); }
    bool add(Parser *p, const Lex_cstring && rhs)
    {
      return concat(std::move(rhs));
    }
  };


  template<class PARSER>
  class Simple_container: public Lex_cstring_container
  {
    using PARENT= Lex_cstring_container;
    using SELF= Simple_container;
    using PARENT::PARENT;
  public:
    Simple_container() :PARENT() { }
    Simple_container(SELF && rhs) :PARENT(std::move(rhs))  { }
    explicit Simple_container(PARENT && rhs) :PARENT(std::move(rhs)) { }
    SELF & operator=(SELF && rhs)
    {
      PARENT::operator=(std::move(rhs));
      return *this;
    }
    static SELF empty(const Parser &p) { return SELF(PARENT::empty(p)); }
    static SELF empty() { return SELF(PARENT::empty()); }

    Simple_container(PARSER && rhs)
     :PARENT(std::move(static_cast<PARENT&&>(rhs)))
    { }
    void print(String *str) const
    {
      str->append(ptr(), length());
    }
  };


  template<class PARENT>
  class Simple_container1: public PARENT
  {
    using SELF= Simple_container1;
    using PARENT::PARENT;
  public:
    using PARENT::operator bool;
    Simple_container1() :PARENT() { }
    Simple_container1(SELF && rhs) :PARENT(std::move(rhs))  { }
    explicit Simple_container1(PARENT && rhs) :PARENT(std::move(rhs)) { }
    SELF & operator=(SELF && rhs)
    {
      PARENT::operator=(std::move(rhs));
      return *this;
    }
    static SELF empty(const Parser &p) { return SELF(PARENT::empty(p)); }
    static SELF empty() { return SELF(PARENT::empty()); }
  };


  template<class PARENT, class A, class B>
  class Simple_container2: public PARENT
  {
    using SELF= Simple_container2;
  public:
    using PARENT::PARENT;
    // Initialization on parse error
    Simple_container2()
     :PARENT(A(), B())
    { }
    // Initialization from its components
    Simple_container2(A && rhs)
     :PARENT(A(std::move(rhs)),
             B(B::empty()))
    { }
    Simple_container2(B && rhs)
     :PARENT(A(A::empty()),
             B(std::move(rhs)))
    { }
    // The rest
    Simple_container2(SELF && rhs):PARENT(std::move(rhs))  { }
    explicit Simple_container2(PARENT && rhs) :PARENT(std::move(rhs)) { }
    SELF & operator=(SELF && rhs)
    {
      A::operator=(std::move(static_cast<A&&>(rhs)));
      B::operator=(std::move(static_cast<B&&>(rhs)));
      return *this;
    }
    static SELF empty(const Parser &p)
    {
      return SELF(PARENT(A::empty(p), B::empty(p)));
    }
    static SELF empty()
    {
      return SELF(PARENT(A::empty(), B::empty()));
    }
    operator bool() const
    {
      return A::operator bool() && B::operator bool();
    }
  };


  template<class PARENT, class A, class B, class C>
  class Simple_container3: public PARENT
  {
    using SELF= Simple_container3;
  public:
    using PARENT::PARENT;

    // Initialization on parse error
    Simple_container3()
     :PARENT(A(), B(), C())
    { }
    // Initialization from its components
    Simple_container3(A && rhs)
     :PARENT(A(std::move(rhs)),
             B(B::empty()),
             C(C::empty()))
    { }
    Simple_container3(B && rhs)
     :PARENT(A(A::empty()),
             B(std::move(rhs)),
             C(C::empty()))
    { }
    Simple_container3(C && rhs)
     :PARENT(A(A::empty()),
             B(B::empty()),
             C(std::move(rhs)))
    { }

    // The rest
    Simple_container3(SELF && rhs):PARENT(std::move(rhs))  { }
    explicit Simple_container3(PARENT && rhs) :PARENT(std::move(rhs)) { }
    SELF & operator=(SELF && rhs)
    {
      A::operator=(std::move(static_cast<A&&>(rhs)));
      B::operator=(std::move(static_cast<B&&>(rhs)));
      C::operator=(std::move(static_cast<C&&>(rhs)));
      return *this;
    }
    static SELF empty(const Parser &p)
    {
      return SELF(PARENT(A::empty(p), B::empty(p), C::empty(p)));
    }
    static SELF empty()
    {
      return SELF(PARENT(A::empty(), B::empty(), C::empty()));
    }
    operator bool() const
    {
      return A::operator bool() && B::operator bool() && C::operator bool();
    }
  };


  template<class PARENT, class A, class B, class C, class D>
  class Simple_container4: public PARENT
  {
    using SELF= Simple_container4;
  public:
    using PARENT::PARENT;

    // Initialization on parse error
    Simple_container4()
     :PARENT(A(), B(), C(), D())
    { }
    // Initialization from its components
    Simple_container4(A && rhs)
     :PARENT(A(std::move(rhs)),
             B(B::empty()),
             C(C::empty()),
             D(D::empty()))
    { }
    Simple_container4(B && rhs)
     :PARENT(A(A::empty()),
             B(std::move(rhs)),
             C(C::empty()),
             D(D::empty()))
    { }
    Simple_container4(C && rhs)
     :PARENT(A(A::empty()),
             B(B::empty()),
             C(std::move(rhs)),
             D(D::empty()))
    { }

    // The rest
    Simple_container4(SELF && rhs):PARENT(std::move(rhs))  { }
    explicit Simple_container4(PARENT && rhs) :PARENT(std::move(rhs)) { }
    SELF & operator=(SELF && rhs)
    {
      A::operator=(std::move(static_cast<A&&>(rhs)));
      B::operator=(std::move(static_cast<B&&>(rhs)));
      C::operator=(std::move(static_cast<C&&>(rhs)));
      D::operator=(std::move(static_cast<D&&>(rhs)));
      return *this;
    }
    static SELF empty(const Parser &p)
    {
      return SELF(PARENT(A::empty(p), B::empty(p), C::empty(p), D::empty(p)));
    }
    static SELF empty()
    {
      return SELF(PARENT(A::empty(), B::empty(), C::empty(), D::empty()));
    }
    operator bool() const
    {
      return A::operator bool() &&
             B::operator bool() &&
             C::operator bool() &&
             D::operator bool();
    }
  };


  /*
    Counters for flags '$' and 'B' and group separators '.' and 'G'.
    - Currency prefix flags can have flags '$' and 'B' only:    'BC99.9'
    - Integer digits can have both flags and group separators:  '9,B,9$'
    - Fractional digits can have flags '$' and 'B' only:        '.9B$9'
  */
  class Decimal_flag_counters
  {
  public:
    size_t m_dollar_count;
    size_t m_B_count;
    size_t m_cG_count;     // comma's and G's
    Decimal_flag_counters()
     :m_dollar_count(0),
      m_B_count(0),
      m_cG_count(0)
    { }
    Decimal_flag_counters(Decimal_flag_counters && rhs)
     :m_dollar_count(rhs.m_dollar_count),
      m_B_count(rhs.m_B_count),
      m_cG_count(rhs.m_cG_count)
    { }
    Decimal_flag_counters & operator=(Decimal_flag_counters && rhs)
    {
      m_dollar_count= rhs.m_dollar_count;
      m_B_count= rhs.m_B_count;
      m_cG_count= rhs.m_cG_count;
      return *this;
    }
    void join(Decimal_flag_counters && rhs)
    {
      m_dollar_count+= rhs.m_dollar_count;
      m_B_count+= rhs.m_B_count;
      m_cG_count+= rhs.m_cG_count;
    }
    void add(char ch)
    {
      if (ch == '$')
        m_dollar_count++;
      if (ch == 'B')
        m_B_count++;
      if (ch == '.' || ch == 'G')
        m_cG_count++;
    }
  };


  /*
    The following chains are returned by the tokenizer as a single token:

      zeros: '0' [ '0'... ]
      nines: '9' [ '9'... ]
      xchain: 'X' [ 'X'...]
  */
  class Zeros: public TOKEN<Parser, TokenID::tZEROS>   { using TOKEN::TOKEN; };
  class Nines: public TOKEN<Parser, TokenID::tNINES>   { using TOKEN::TOKEN; };
  class XChain: public TOKEN<Parser, TokenID::tXCHAIN> { using TOKEN::TOKEN; };


  /********** Rules consisting of a single token *****/
  /*  with their Opt_ variants when needed           */

  class TokenEOF: public TOKEN<Parser, TokenID::tEOF> { using TOKEN::TOKEN; };

  // Inline flag 'B'
  class TokenB: public TOKEN<Parser, TokenID::tB> { using TOKEN::TOKEN; };
  class Opt_B: public OPT<Parser, TokenB>   { using OPT::OPT; };

  // Prefix flag 'FM'
  class FlagFM: public TOKEN<Parser, TokenID::tFM> { using TOKEN::TOKEN; };
  class Opt_FM: public OPT<Parser, FlagFM>  { using OPT::OPT; };

  class Format_flags_container: public Lex_cstring_container
  { using Lex_cstring_container::Lex_cstring_container; };

  // EEEE - scientific modifier
  class EEEE: public TOKEN<Parser, TokenID::tEEEE> { using TOKEN::TOKEN; };
  class Opt_EEEE: public OPT<Parser, EEEE>  { using OPT::OPT; };
  using EEEE_container= Simple_container<EEEE>;

  // prefix_sign: 'S'
  class Prefix_sign: public TOKEN<Parser, TokenID::tS> { using TOKEN::TOKEN; };
  using Prefix_sign_container= Simple_container<Prefix_sign>;



  /********** Rules consisting of 2 token choices ***/
  /* with their Opt_ variants (when needed)         */


  // decimal_flag: 'B' | '$'
  class Decimal_flag_cond: public TokenChoiceCond2<Parser,
                                                   TokenID::tB,
                                                   TokenID::tDOLLAR>
  { };

  // group_separator: ',' | 'G'
  class Group_separator_cond: public TokenChoiceCond2<Parser,
                                                       TokenID::tCOMMA,
                                                       TokenID::tG>
  { };
  class Group_separator: public TokenChoice<Parser, Group_separator_cond>
  { using TokenChoice::TokenChoice; };


  // zeros_or_nines: zeros | nines
  class Zeros_or_nines_cond: public TokenChoiceCond2<Parser,
                                                       TokenID::tZEROS,
                                                       TokenID::tNINES>
  { };
  class Zeros_or_nines: public TokenChoice<Parser, Zeros_or_nines_cond>
  { using TokenChoice::TokenChoice; };


  // Rules consisting of 3 token choices with their Opt_ variants (when needed)

  // postfix_sign: 'S' | 'MI' | 'PR'
  class Postfix_sign_cond: public TokenChoiceCond3<Parser,
                                                   TokenID::tS,
                                                   TokenID::tMI,
                                                   TokenID::tPR>
  { };
  class Postfix_sign: public TokenChoice<Parser, Postfix_sign_cond>
  { using TokenChoice::TokenChoice; };
  class Opt_postfix_sign: public OPT<Parser, Postfix_sign> { using OPT::OPT; };


  // postfix_only_sign: 'MI' | 'PR'
  class Postfix_only_sign_cond: public TokenChoiceCond2<Parser,
                                                        TokenID::tMI,
                                                        TokenID::tPR>
  { };
  class Postfix_only_sign: public TokenChoice<Parser, Postfix_only_sign_cond>
  { using TokenChoice::TokenChoice; };


  /*
    A shared parser for the postfix sign.
    Depending on the grammar rule, the postfix sign can be:
    - Postfix_only_sign:  MI, PR
    - Postfix_sign:       MI, PR, S (all sign variants)
    This class provides parsing for both variants with further
    conversion to and indermediate storage - Lex_cstring (the base).
  */
  class Postfix_sign_parser: public Lex_cstring
  {
    using PARENT= Lex_cstring;
  public:
    Postfix_sign_parser(Postfix_sign && rhs)
     :PARENT(std::move(static_cast<PARENT&&>(rhs)))
    { }
    Postfix_sign_parser(Postfix_only_sign && rhs)
     :PARENT(std::move(static_cast<PARENT&&>(rhs)))
    { }
  };
  // The final storage for the postfix sign
  using Postfix_sign_container= Simple_container<Postfix_sign_parser>;


  /*
    positional_currency_signature: 'C' | 'L' | 'U'

    The position of CLU inside the format is important, hence the name.

    Note, the position of the dollar sign is not important, it can be
    specified once on any place inside the number.
  */
  class Positional_currency_signature_cond: public TokenChoiceCond3<Parser,
                                                                    TokenID::tC,
                                                                    TokenID::tL,
                                                                    TokenID::tU>
  { };
  class Positional_currency_signature:
                         public TokenChoice<Parser,
                                            Positional_currency_signature_cond>
  { using TokenChoice::TokenChoice; };

  class Opt_postfix_currency: public OPT<Parser, Positional_currency_signature>
  { using OPT::OPT; };

  // prefix_currency_signature: positional_currency_signature
  class Prefix_currency_signature: public Positional_currency_signature
  { using Positional_currency_signature::Positional_currency_signature; };
  using Prefix_currency_container= Simple_container<Prefix_currency_signature>;

  // postfix_currency_signature: positional_currency_signature
  class Postfix_currency_signature: public Positional_currency_signature
  { using Positional_currency_signature::Positional_currency_signature; };
  using Postfix_currency_container= Simple_container<Postfix_currency_signature>;

  // dec_delimiter_currency_signature: positional_currency_signature
  class Dec_delimiter_currency_signature: public Positional_currency_signature
  { using Positional_currency_signature::Positional_currency_signature; };
  using Dec_delimiter_pDVCLU_container=
          Simple_container<Dec_delimiter_currency_signature>;


  // format_TM_signature: 'TM' | 'TM9' | 'TME'
  class Format_TM_cond: public TokenChoiceCond3<Parser,
                                                TokenID::tTM,
                                                TokenID::tTM9,
                                                TokenID::tTME>
  { };
  class Format_TM_signature: public TokenChoice<Parser, Format_TM_cond>
  { using TokenChoice::TokenChoice; };
  using Format_TM_container= Simple_container<Format_TM_signature>;


  // fraction_pDV_signature: '.' | 'D' | 'V'
  class Fraction_pDV_signature_cond: public TokenChoiceCond3<Parser,
                                                             TokenID::tPERIOD,
                                                             TokenID::tD,
                                                             TokenID::tV>
  { };
  class Fraction_pDV_signature: public TokenChoice<Parser,
                                                   Fraction_pDV_signature_cond>
  { using TokenChoice::TokenChoice; };



  // Rules consisting of more complex token choices

  // fractional_element: zeros_or_nines | decimal_flag
  class Fractional_element_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return Zeros_or_nines_cond::allowed_token_id(id) ||
             Decimal_flag_cond::allowed_token_id(id);
    }
  };
  class Fractional_element: public TokenChoice<Parser, Fractional_element_cond>
  { using TokenChoice::TokenChoice; };


  // integer_element: fractional_element | group_separator
  class Integer_element_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return Fractional_element_cond::allowed_token_id(id) ||
             Group_separator_cond::allowed_token_id(id);
    }
  };
  class Integer_element: public TokenChoice<Parser, Integer_element_cond>
  { using TokenChoice::TokenChoice; };



  /*
    Rules consisting of a LIST of token choices
  */

  // currency_prefix_flag: decimal_flag
  // currency_prefix_flags: currency_prefix_flag [ currency_prefix_flag...]
  class Currency_prefix_flag: public TokenChoice<Parser, Decimal_flag_cond>
  { using TokenChoice::TokenChoice; };
  class Currency_prefix_flags_container: public Lex_cstring_container
  {
  public:
    using Lex_cstring_container::Lex_cstring_container;
    Decimal_flag_counters prefix_flag_counters() const
    {
      Decimal_flag_counters tmp;
      for (size_t i= 0; i < length(); i++)
        tmp.add(ptr()[i]);
      return tmp;
    }
  };
  class Currency_prefix_flags: public LIST<Parser,
                                           Currency_prefix_flags_container,
                                           Currency_prefix_flag,
                                           TokenID::tNULL /* not separated */,
                                           1 /* needs at list one element */>
  { using LIST::LIST; };


  /*
    A container for format digits - elements that can appear in a number body:
    - decimal digit placeholders: 0 9
    - hex digit placeholders:     X
    - inline flags:               $ B
  */
  class Digit_container: public Lex_cstring_container,
                         public Decimal_flag_counters
  {
  public:
    Digit_container()
    { }
    Digit_container(Digit_container && rhs)
     :Lex_cstring_container(std::move(rhs)),
      Decimal_flag_counters(std::move(rhs))
    { }
    Digit_container & operator=(Digit_container && rhs)
    {
      Lex_cstring::operator=(std::move(rhs));
      Decimal_flag_counters::operator=(std::move(rhs));
      return *this;
    }
    static Digit_container empty(const Parser &p)
    {
      Digit_container tmp;
      tmp.Lex_cstring_container::operator=(Lex_cstring_container::empty(p));
      return tmp;
    }
    static Digit_container empty()
    {
      Digit_container tmp;
      tmp.Lex_cstring_container::operator=(Lex_cstring_container::empty());
      return tmp;
    }
    bool check_counters() const
    {
      return m_dollar_count > 1 || m_B_count > 1;
    }
    operator bool() const
    {
      return Lex_cstring_container::operator bool() &&
             !check_counters();
    }

    /*
      Hide the inherited contat() to avoid descendants use it
      in a mistake instead of join().
    */
    bool concat()= delete;
    bool join(Digit_container && rhs)
    {
      Decimal_flag_counters::join(std::move(rhs));
      return Lex_cstring_container::concat(std::move(rhs));
    }
    bool add(Parser *p, const Lex_cstring && rhs) // for LIST
    {
      DBUG_ASSERT(rhs.length > 0);
      Decimal_flag_counters::add(rhs.str[0]);
      if (check_counters())
        return true;
      return Lex_cstring_container::add(p, std::move(rhs));
    }

    Digit_container(Zeros_or_nines && rhs)
     :Lex_cstring_container(std::move(rhs))
    { }
    Digit_container(Zeros && rhs)
     :Lex_cstring_container(std::move(rhs))
    { }
    Digit_container(Nines && rhs)
     :Lex_cstring_container(std::move(rhs))
    { }
    Digit_container(XChain && rhs)
     :Lex_cstring_container(std::move(rhs))
    { }

  };

  // integer: integer_digit [ integer_digit...]
  class Integer_container: public Digit_container
  {
  public:
    using Digit_container::Digit_container;
    Integer_container(Integer_container && rhs)
     :Digit_container(std::move(rhs))
    { }
    explicit Integer_container(Digit_container && rhs)
     :Digit_container(std::move(rhs))
    { }
    Integer_container & operator=(Integer_container && rhs)
    {
      Digit_container::operator=(std::move(rhs));
      return *this;
    }
    static Integer_container empty(const Parser &p)
    {
      return Integer_container(Digit_container::empty(p));
    }
    static Integer_container empty()
    {
      return Integer_container(Digit_container::empty());
    }

    Integer_container(Zeros && zeros, XChain && xchain)
     :Digit_container(std::move(zeros))
    {
      Lex_cstring_container::concat(std::move(xchain));
    }
    Integer_container(Zeros_or_nines && zn, XChain && xchain)
     :Digit_container(std::move(static_cast<Zeros_or_nines&&>(zn)))
    {
      Lex_cstring_container::concat(std::move(xchain));
    }

    // Zeros + extra digits
    Integer_container(Zeros && head, Integer_container && tail)
     :Digit_container(std::move(head))
    {
      Integer_container::join(std::move(tail));
    }

    // Nines + extta digits
    Integer_container(Nines && head, Integer_container && tail)
     :Digit_container(std::move(head))
    {
      Integer_container::join(std::move(tail));
    }

    // Zeros_or_nines + extra digits
    Integer_container(Zeros_or_nines && head, Integer_container && tail)
     :Digit_container(std::move(head))
    {
      Integer_container::join(std::move(tail));
    }

  };

  /*
    A tail of an integer number.
    It can start with a group character.
  */
  class Integer_tail: public LIST<Parser, Integer_container,
                                          Integer_element,
                                          TokenID::tNULL /*not separated*/,
                                          1 /* needs at least one element*/>
  { using LIST::LIST; };



  // fraction_body: fractional_digit [ fractional_digit ... ]
  class Fraction_body_container: public Digit_container
  {
  public:
    using Digit_container::Digit_container;
    Fraction_body_container(Fraction_body_container && rhs)
     :Digit_container(std::move(rhs))
    { }
    explicit Fraction_body_container(Digit_container && rhs)
     :Digit_container(std::move(rhs))
    { }
    Fraction_body_container & operator=(Fraction_body_container && rhs)
    {
      Digit_container::operator=(std::move(rhs));
      return *this;
    }
    static Fraction_body_container empty(const Parser &p)
    {
      return Fraction_body_container(Digit_container::empty(p));
    }
    static Fraction_body_container empty()
    {
      return Fraction_body_container(Digit_container::empty());
    }
  };
  // The grammar needs only an optional fraction_body
  class Opt_fraction_body: public LIST<Parser, Fraction_body_container,
                                       Fractional_element,
                                       TokenID::tNULL /* not separated */,
                                       0 /* optional: no elements ok */>
  { using LIST::LIST; };



  /********** Rules related to the fractional part of a number ***/
  /*
    fraction_pDVCLU: positional_currency [ fraction_body ]
        | fraction_pDV_signature [ fraction_body ] [ positional_currency ]
  */

  class Fraction_pDV: public AND2<Parser,
                                  Fraction_pDV_signature,
                                  Opt_fraction_body>
  {
  public:
    using AND2::AND2;
    static Fraction_pDV empty(const Parser &parser)
    {
      return Fraction_pDV(Fraction_pDV_signature::empty(parser),
                          Opt_fraction_body::empty(parser));
    }
  };

  class Opt_fraction_pDV: public OPT<Parser, Fraction_pDV> { using OPT::OPT; };



  class Fraction_pDVCLU_1: public AND2<Parser,
                                       Positional_currency_signature,
                                       Opt_fraction_body>
  { using AND2::AND2; };

  class Fraction_pDVCLU_2: public AND3<Parser,
                                       Fraction_pDV_signature,
                                       Opt_fraction_body,
                                       Opt_postfix_currency>
  { using AND3::AND3; };


  class Fraction_container_base: public Dec_delimiter_pDVCLU_container,
                                 public Fraction_body_container,
                                 public Postfix_currency_container
  {
    using A= Dec_delimiter_pDVCLU_container;
    using B= Fraction_body_container;
    using C= Postfix_currency_container;
  public:

    // Initialization from its components
    Fraction_container_base(Dec_delimiter_pDVCLU_container && a,
                            Fraction_body_container && b,
                            Postfix_currency_container && c)
     :A(std::move(a)),
      B(std::move(b)),
      C(std::move(c))
    { }

    // Initialization from rules
    Fraction_container_base(Fraction_pDV && rhs)
     :A(std::move(static_cast<Fraction_pDV_signature&&>(rhs))),
      B(std::move(static_cast<Fraction_body_container&&>(rhs))),
      C(C::empty())
    { }

    /*
      TODO: Opt_fraction_body can return true while
      Fraction_body_container after initialization from it
      (from its Opt_fraction_body::Fraction_body_container actually)
      can return false.
    */
    Fraction_container_base(Fraction_pDVCLU_1 && rhs)
     :A(std::move(static_cast<Positional_currency_signature&&>(rhs))),
      B(std::move(static_cast<Opt_fraction_body&&>(rhs))),
      C(C::empty())
    { }

    Fraction_container_base(Fraction_pDVCLU_2 && rhs)
     :A(std::move(static_cast<Fraction_pDV_signature&&>(rhs))),
      B(std::move(static_cast<Fraction_body_container&&>(rhs))),
      C(std::move(static_cast<Opt_postfix_currency&&>(rhs)))
    { }
  };


  using Fraction_container= Simple_container3<Fraction_container_base,
                                              Dec_delimiter_pDVCLU_container,
                                              Fraction_body_container,
                                              Postfix_currency_container>;

  class Fraction_pDVCLU: public OR2C<Parser,
                                     Fraction_container,
                                     Fraction_pDVCLU_1,
                                     Fraction_pDVCLU_2>
  { using OR2C::OR2C; };

  class Opt_fraction_pDVCLU: public OPT<Parser, Fraction_pDVCLU>
  { using OPT::OPT; };



  /********** Rules related to a decimal number ***/

  /*
    decimal_tail_pDVCLU: integer_tail [ fraction_pDVCLU ]
                       | fraction_pDVCLU
  */
  class Decimal_tail_pDVCLU_choice1: public AND2<Parser,
                                                 Integer_tail,
                                                 Opt_fraction_pDVCLU>
  { using AND2::AND2; };


  /*
    decimal_tail_pDV: integer_tail [ fraction_pDV ]
                    | fraction_pDV
  */
  class Decimal_tail_pDV_choice1: public AND2<Parser,
                                              Integer_tail,
                                              Opt_fraction_pDV>
  { using AND2::AND2; };



  class Decimal_tail_container_base: public Integer_container,
                                     public Fraction_container
  {
  public:


    Decimal_tail_container_base(Decimal_tail_container_base && rhs)
     :Integer_container(std::move(rhs)),
      Fraction_container(std::move(rhs))
    { }
    Decimal_tail_container_base(Integer_container && a,
                                Fraction_container && b)
     :Integer_container(std::move(a)),
      Fraction_container(std::move(b))
    { }

    Decimal_tail_container_base(Decimal_tail_pDVCLU_choice1 && rhs)
     :Integer_container(std::move(rhs)),
      Fraction_container(std::move(rhs))
    { }
    Decimal_tail_container_base(Decimal_tail_pDV_choice1 && rhs)
     :Integer_container(std::move(rhs)),
      Fraction_container(std::move(rhs))
    { }

    Decimal_tail_container_base(Fraction_pDVCLU && rhs)
     :Integer_container(Integer_container::empty()),
      Fraction_container(std::move(rhs))
    { }
    Decimal_tail_container_base(Fraction_pDV && rhs)
     :Integer_container(Integer_container::empty()),
      Fraction_container(std::move(rhs))
    { }
  };

  using Decimal_tail_container=
    Simple_container2<Decimal_tail_container_base,
                      Integer_container,
                      Fraction_container>;


  class Decimal_tail_pDVCLU: public OR2C<Parser,
                                         Decimal_tail_container,
                                         Decimal_tail_pDVCLU_choice1,
                                         Fraction_pDVCLU>
  { using OR2C::OR2C; };

  class Decimal_tail_pDV: public OR2C<Parser,
                                      Decimal_tail_container,
                                      Decimal_tail_pDV_choice1,
                                      Fraction_pDV>
  { using OR2C::OR2C; };



  class Decimal_container_base: public Decimal_tail_container
  {
    using PARENT= Decimal_tail_container;
  public:
    using PARENT::PARENT;

    Decimal_container_base(XChain && rhs)
     :Decimal_tail_container(std::move(rhs), Fraction_container::empty())
    { }

    Decimal_container_base(Zeros && head, Decimal_tail_container_base && tail)
     :Decimal_tail_container(
        Integer_container(std::move(head),
                          std::move(static_cast<Integer_container&&>(tail))),
        std::move(static_cast<Fraction_container&&>(tail)))
    { }

    Decimal_container_base(Nines && head, Decimal_tail_container_base && tail)
     :Decimal_tail_container(
        Integer_container(std::move(head),
                          std::move(static_cast<Integer_container&&>(tail))),
        std::move(static_cast<Fraction_container&&>(tail)))
    { }

    Decimal_container_base(Zeros_or_nines && head,
                           Decimal_tail_container_base && tail)
     :Decimal_tail_container(
        Integer_container(std::move(head),
                          std::move(static_cast<Integer_container&&>(tail))),
        std::move(static_cast<Fraction_container&&>(tail)))
    { }
  };

  using Decimal_container= Simple_container1<Decimal_container_base>;



  /********** Rules related to an approximate number (scientific notation) ***/

  /*
    approximate_tail_pDVCLU: decimal_tail_pDVCLU [ EEEE ]
                           | EEEE
  */
  class Decimal_tail_pDVCLU_Opt_EEEE: public AND2<Parser,
                                                  Decimal_tail_pDVCLU,
                                                  Opt_EEEE>
  { using AND2::AND2; };

  /*
    approximate_tail_pDV: decimal_tail_pDV [ EEEE ]
                        | EEEE
  */
  class Decimal_tail_pDV_Opt_EEEE: public AND2<Parser,
                                               Decimal_tail_pDV,
                                               Opt_EEEE>
  { using AND2::AND2; };


  /*
    Approximate tail:
    Its integer part can start with a group character.
  */
  class Approximate_tail_container_base: public Decimal_tail_container,
                                         public EEEE_container
  {
  public:

    // Initialization from itself and its components
    Approximate_tail_container_base(Approximate_tail_container_base && rhs)
     :Decimal_tail_container(std::move(rhs)),
      EEEE_container(std::move(rhs))
    { }
    Approximate_tail_container_base(Decimal_tail_container && a,
                                    EEEE_container && b)
     :Decimal_tail_container(std::move(a)),
      EEEE_container(std::move(b))
    { }

    // Initialization from rules
    Approximate_tail_container_base(EEEE && rhs)
     :Decimal_tail_container(Decimal_tail_container::empty()),
      EEEE_container(std::move(rhs))
    { }
    Approximate_tail_container_base(Decimal_tail_pDVCLU_Opt_EEEE && rhs)
     :Decimal_tail_container(std::move(static_cast<Decimal_tail_pDVCLU&&>(rhs))),
      EEEE_container(std::move(static_cast<EEEE&&>(rhs)))
    { }
    Approximate_tail_container_base(Decimal_tail_pDV_Opt_EEEE && rhs)
     :Decimal_tail_container(std::move(static_cast<Decimal_tail_pDV&&>(rhs))),
      EEEE_container(std::move(static_cast<EEEE&&>(rhs)))
    { }

    // Strange rules
    Approximate_tail_container_base(XChain && rhs)
     :Decimal_tail_container(std::move(rhs)),
      EEEE_container(EEEE_container::empty())
    { }
    Approximate_tail_container_base(Fraction_pDV && rhs)
     :Decimal_tail_container(std::move(rhs)),
      EEEE_container(EEEE_container::empty())
    { }
    Approximate_tail_container_base(Fraction_pDVCLU && rhs)
     :Decimal_tail_container(std::move(rhs)),
      EEEE_container(EEEE_container::empty())
    { }
    Approximate_tail_container_base(Zeros && head,
                                    Approximate_tail_container_base && tail)
     :Decimal_tail_container(
        //Integer_container(std::move(head),std::move(tail)),
        std::move(head),
        std::move(tail)),
      EEEE_container(std::move(tail))
    { }
  };

  using Approximate_tail_container=
    Simple_container2<Approximate_tail_container_base,
                      Decimal_tail_container,
                      EEEE_container>;

  class Approximate_tail_pDVCLU: public OR2C<Parser,
                                             Approximate_tail_container,
                                             Decimal_tail_pDVCLU_Opt_EEEE,
                                             Opt_EEEE>
  { using OR2C::OR2C; };

  class Opt_approximate_tail_pDVCLU: public OPT<Parser, Approximate_tail_pDVCLU>
  { using OPT::OPT; };

  class Nines_Opt_approximate_tail_pDVCLU:
                                       public AND2<Parser,
                                                   Zeros_or_nines,
                                                   Opt_approximate_tail_pDVCLU>
  { using AND2::AND2; };

  class Zeros_or_nines_Opt_approximate_tail_pDVCLU:
                                       public AND2<Parser,
                                                  Zeros_or_nines,
                                                  Opt_approximate_tail_pDVCLU>
  { using AND2::AND2; };


  class Approximate_tail_pDV: public OR2C<Parser,
                                          Approximate_tail_container,
                                          Decimal_tail_pDV_Opt_EEEE,
                                          Opt_EEEE>
  { using OR2C::OR2C; };

  class Opt_approximate_tail_pDV: public OPT<Parser, Approximate_tail_pDV>
  { using OPT::OPT; };

  class Zeros_or_nines_Opt_approximate_tail_pDV:
                                          public AND2<Parser,
                                                      Zeros_or_nines,
                                                      Opt_approximate_tail_pDV>
  { using AND2::AND2; };


  class Zeros_Opt_approximate_tail_pDVCLU:
                                       public AND2<Parser,
                                                   Zeros,
                                                   Opt_approximate_tail_pDVCLU>
  { using AND2::AND2; };


  /*
    A container for a well formed approximate number.
    Its integer part starts with a valid character such as a digit.
    It cannot start with a group character.
  */

  class Approximate_container_base: public Approximate_tail_container
  {
    using PARENT= Approximate_tail_container;
    using SELF= Approximate_container_base;
  public:
    using PARENT::PARENT;

    Approximate_container_base(EEEE && rhs)
     :PARENT(Decimal_container(Decimal_container::empty()),
             EEEE_container(std::move(rhs)))
    { }
    Approximate_container_base(EEEE_container && rhs)
     :PARENT(Decimal_container(Decimal_container::empty()),
             EEEE_container(std::move(rhs)))
    { }

    Approximate_container_base(XChain && rhs)
     :PARENT(Decimal_container(std::move(rhs)),
             EEEE_container::empty())
    { }

    Approximate_container_base(Fraction_container && rhs)
     :PARENT(Decimal_container(std::move(rhs)),
             EEEE_container::empty())
    { }

    Approximate_container_base(Fraction_pDV &&rhs)
     :PARENT(Decimal_container(std::move(rhs)),
             EEEE_container::empty())
    { }

    Approximate_container_base(Fraction_container && frac, EEEE && eeee)
     :PARENT(Decimal_container(std::move(frac)),
             EEEE(std::move(eeee)))
    { }

    // Stating with "Zeros"
    Approximate_container_base(Zeros && head, Approximate_tail_container && tail)
     :PARENT(
       Decimal_container(std::move(head),
                         std::move(static_cast<Decimal_tail_container&&>(tail))),
       EEEE_container(std::move(static_cast<EEEE_container&&>(tail))))
    { }

    Approximate_container_base(Zeros && head, Approximate_container_base && tail)
     :PARENT(
       Decimal_container(std::move(head),
                         std::move(static_cast<Decimal_tail_container&&>(tail))),
       EEEE_container(std::move(static_cast<EEEE_container&&>(tail))))
    { }

    Approximate_container_base(Zeros_Opt_approximate_tail_pDVCLU && rhs)
     :PARENT(
       Decimal_container(std::move(static_cast<Zeros &&>(rhs)),
                         std::move(static_cast<Decimal_tail_container &&>(rhs))),
       EEEE_container(std::move(static_cast<EEEE_container&&>(rhs))))
    {  }

    // Starting with "Nines"
    Approximate_container_base(Nines_Opt_approximate_tail_pDVCLU && rhs)
     :PARENT(
       Decimal_container(std::move(static_cast<Nines &&>(rhs)),
                         std::move(static_cast<Decimal_tail_container &&>(rhs))),
       EEEE_container(std::move(static_cast<EEEE_container&&>(rhs))))
    { }

    Approximate_container_base(Nines && head, Approximate_tail_container && tail)
     :PARENT(
       Decimal_container(std::move(head),
                         std::move(static_cast<Decimal_tail_container&&>(tail))),
       EEEE_container(std::move(static_cast<EEEE_container&&>(tail))))
    { }

    // Starting with "Zeros_or_Nines"
    Approximate_container_base(Zeros_or_nines_Opt_approximate_tail_pDVCLU && rhs)
     :PARENT(
       Decimal_container(std::move(static_cast<Zeros_or_nines &&>(rhs)),
                         std::move(static_cast<Decimal_tail_container &&>(rhs))),
       EEEE_container(std::move(static_cast<EEEE_container&&>(rhs))))
    { }

    Approximate_container_base(Zeros_or_nines_Opt_approximate_tail_pDV && rhs)
     :PARENT(
       Decimal_container(std::move(static_cast<Zeros_or_nines &&>(rhs)),
                         std::move(static_cast<Decimal_tail_container &&>(rhs))),
       EEEE_container(std::move(static_cast<EEEE_container&&>(rhs))))
    { }

    Approximate_container_base(Zeros_or_nines && head,
                               Approximate_tail_container && tail)
     :PARENT(
       Decimal_container(std::move(head),
                         std::move(static_cast<Decimal_tail_container&&>(tail))),
       EEEE_container(std::move(static_cast<EEEE_container&&>(tail))))
    { }
  };


  using Approximate_container= Simple_container1<Approximate_container_base>;

  /*
    approximate_pDVCLU: zero_or_nines [ approximate_tail_pDVCLU ]
                      | fraction_pDVCLU
  */
  class Approximate_pDVCLU: public OR2C<Parser,
                                   Approximate_container,
                                   Zeros_or_nines_Opt_approximate_tail_pDVCLU,
                                   Fraction_pDVCLU>
  { using OR2C::OR2C; };

  class Opt_approximate_pDVCLU: public OPT<Parser, Approximate_pDVCLU>
  { using OPT::OPT; };

  /*
    approximate_pDV: zero_or_nines [ approximate_tail_pDV ]
                   | fraction_pDV
  */
  class Approximate_pDV: public OR2C<Parser,
                                     Approximate_container,
                                     Zeros_or_nines_Opt_approximate_tail_pDV,
                                     Fraction_pDV>
  { using OR2C::OR2C; };

  class Opt_approximate_pDV: public OPT<Parser, Approximate_pDV>
  { using OPT::OPT; };


  /********** Rules related to a number with a prefix currency signature ***/

  /*
    left_currency: positional_currency_signature     approximate_pDV
                 | positional_currency_signature 'B' approximate_pDV
  */

  class B_Opt_Approximate_pDV: public AND2<Parser, TokenB, Opt_approximate_pDV>
  { using AND2::AND2; };

  class Left_currency_tail_container_base:
                                        public Currency_prefix_flags_container,
                                        public Approximate_container
  {
    using A= Currency_prefix_flags_container;
    using B= Approximate_container;
  public:

    // Initialization from its components
    Left_currency_tail_container_base(A && a, B && b)
     :A(std::move(a)),
      B(std::move(b))
    { }

    // Initialization from rules
    Left_currency_tail_container_base(Fraction_pDV && rhs)
     :Currency_prefix_flags_container(Currency_prefix_flags_container::empty()),
      Approximate_container(Fraction_container(std::move(rhs)),
                            EEEE(EEEE::empty()))
    { }
    Left_currency_tail_container_base(B_Opt_Approximate_pDV && rhs)
     :Currency_prefix_flags_container(std::move(static_cast<TokenB&&>(rhs))),
      Approximate_container(std::move(static_cast<Approximate_container&&>(rhs)))
    { }
    Left_currency_tail_container_base(
      Zeros_or_nines_Opt_approximate_tail_pDV && rhs)
     :Currency_prefix_flags_container(std::move(static_cast<Token&&>(rhs))),
      Approximate_container(std::move(rhs))
    { }
  };

  using Left_currency_tail_container=
     Simple_container2<Left_currency_tail_container_base,
                       Currency_prefix_flags_container,
                       Approximate_container>;

  class Left_currency_tail: public OR3C<Parser,
                                        Left_currency_tail_container,
                                        B_Opt_Approximate_pDV,
                                        Zeros_or_nines_Opt_approximate_tail_pDV,
                                        Fraction_pDV>
  { using OR3C::OR3C; };

  class  Opt_Left_currency_tail: public OPT<Parser, Left_currency_tail>
  {
    using OPT::OPT;
  };

  class Left_currency: public AND2<Parser,
                                   Positional_currency_signature,
                                   Opt_Left_currency_tail>
  { using AND2::AND2; };

  /*
    TODO: modified grammar: added [ 'EEEE' ]

    unsigned_currency1:
        nines  [ approximate_tail_pDVCLU ]
      | decimal_flags [ approximate_pDVCLU ]
      | left_currency
      | fraction_pDV [EEEE] [ positional_currency ]
  */


  class Decimal_flags_Opt_approximate_pDVCLU:
                                            public AND2<Parser,
                                                        Currency_prefix_flags,
                                                        Opt_approximate_pDVCLU>
  { using AND2::AND2; };



  /*
    TODO: fixed grammar here: added EEEE
      <  | fraction_pDV        [ positional_currency ]             -- #4
      >  | fraction_pDV [EEEE] [ positional_currency ]             -- #4
  */
  class Fraction_pDV_Opt_EEEE_Opt_postfix_currency:
                                             public AND3<Parser,
                                                         Fraction_pDV,
                                                         Opt_EEEE,
                                                         Opt_postfix_currency>
  { using AND3::AND3; };

  class Unsigned_currency_container_base:
                                      public Prefix_currency_container,
                                      public Currency_prefix_flags_container,
                                      public Approximate_container
  {
    using A= Prefix_currency_container;
    using B= Currency_prefix_flags_container;
    using C= Approximate_container;
  public:
    // Initializing from its componenents
    Unsigned_currency_container_base(A && a, B && b, C && c)
     :A(std::move(a)),
      B(std::move(b)),
      C(std::move(c))
    { }

    // Initializing from rules
    Unsigned_currency_container_base(Nines_Opt_approximate_tail_pDVCLU &&rhs)
     :A(A::empty()),
      B(B::empty()),
      C(Approximate_container(std::move(rhs)))
    { }
    Unsigned_currency_container_base(Decimal_flags_Opt_approximate_pDVCLU &&rhs)
     :A(Prefix_currency_container::empty()),
      B(std::move(static_cast<Currency_prefix_flags&&>(rhs))),
      C(std::move(static_cast<Approximate_container&&>(rhs)))
    { }
    Unsigned_currency_container_base(Left_currency &&rhs)
     :A(std::move(static_cast<Positional_currency_signature&&>(rhs))),
      B(std::move(static_cast<Currency_prefix_flags_container&&>(rhs))),
      C(std::move(static_cast<Approximate_container&&>(rhs)))
    { }
    Unsigned_currency_container_base(
                            Fraction_pDV_Opt_EEEE_Opt_postfix_currency &&rhs)
     :A(Positional_currency_signature::empty()),
      B(Currency_prefix_flags_container::empty()),
      C(Fraction_container(
          std::move(static_cast<Fraction_pDV_signature&&>(rhs)),
          std::move(static_cast<Opt_fraction_body&&>(rhs)),
          std::move(static_cast<Opt_postfix_currency&&>(rhs))),
        std::move(static_cast<EEEE&&>(rhs)))
    { }
    Unsigned_currency_container_base(Zeros_Opt_approximate_tail_pDVCLU && rhs)
     :A(A::empty()),
      B(B::empty()),
      C(Approximate_container(std::move(rhs)))
    { }
  };

  using Unsigned_currency_container=
          Simple_container3<Unsigned_currency_container_base,
                            Prefix_currency_container,
                            Currency_prefix_flags_container,
                            Approximate_container>;


  class Unsigned_currency1: public OR4C<Parser,
                                    Unsigned_currency_container,
                                    Nines_Opt_approximate_tail_pDVCLU,
                                    Decimal_flags_Opt_approximate_pDVCLU,
                                    Left_currency,
                                    Fraction_pDV_Opt_EEEE_Opt_postfix_currency>
  { using OR4C::OR4C; };


  /********** Format without a postfix sign ***/

  /*
    currency_without_postfix_sign: zeros [ approximate_tail_pDVCLU ]
                                 | unsigned_currency1
  */

  class Currency_without_postfix_sign:
                           public OR2C<Parser,
                                       Unsigned_currency_container,
                                       Zeros_Opt_approximate_tail_pDVCLU,
                                       Unsigned_currency1>
  { using OR2C::OR2C; };
  class Opt_currency_without_postfix_sign:
                                       public OPT<Parser,
                                                  Currency_without_postfix_sign>
  { using OPT::OPT; };


  /*
    unsigned_format: currency_without_postfix_sign
                   | format_TM_signature
  */

  class Unsigned_format_container_base: public Unsigned_currency_container,
                                        public Format_TM_container
  {
    using A= Unsigned_currency_container;
    using B= Format_TM_container;
  public:

    // Initializing from its componenet
    Unsigned_format_container_base(A && a, B && b)
     :A(std::move(a)), B(std::move(b))
    { }

    // Initilizing from rules
    Unsigned_format_container_base(Currency_without_postfix_sign && rhs)
     :A(std::move(rhs)), B(B::empty())
    { }

    Unsigned_format_container_base(Format_TM_signature && rhs)
     :A(A::empty()), B(std::move(rhs))
    { }
  };

  using Unsigned_format_container=
          Simple_container2<Unsigned_format_container_base,
                            Unsigned_currency_container,
                            Format_TM_container>;

  class Unsigned_format: public OR2C<Parser,
                                     Unsigned_format_container,
                                     Currency_without_postfix_sign,
                                     Format_TM_signature>
  { using OR2C::OR2C; };

  class Opt_unsigned_format: public OPT<Parser, Unsigned_format>
  { using OPT::OPT; };



  /********** Format with a postfix sign ***/
  /*
    currency0_with_postfix_sign:
                             zeros xchain
                           | zeros [ approximate_tail_pDVCLU ] [ postfix_sign ]

    currency_with_postfix_sign: xchain
                      | currency0_with_postfix_sign
                      | unsigned_currency1 [ postfix_sign ]
                      | postfix_only_sign

    Rewriting branches #2a and #3b as:
    zeros [ zeros_tail ]
    zeros_tail: xchain
              | approximate_tail_pDVCLU [ postfix_sign ]
              | postfix_sign
  */

  class Approximate_tail_pDVCLU_Opt_postfix_sign:
                                          public AND2<Parser,
                                                      Approximate_tail_pDVCLU,
                                                      Opt_postfix_sign>
  { using AND2::AND2; };


  class Approximate_with_postfix_sign_tail_container_base:
                                             public Approximate_tail_container,
                                             public Postfix_sign_container
  {
    using A= Approximate_tail_container;
    using B= Postfix_sign_container;
  public:
    Approximate_with_postfix_sign_tail_container_base()
     :A(Approximate_container::empty()),
      B(Postfix_sign::empty())
    { }
    // Initialization from its componebts
    Approximate_with_postfix_sign_tail_container_base(
                                               Approximate_tail_container &&a,
                                               Postfix_sign_container && b)
     :A(std::move(a)),
      B(std::move(b))
    { }

    // Initialization from rules
    Approximate_with_postfix_sign_tail_container_base(XChain && rhs)
     :A(Decimal_tail_container(Integer_container(std::move(rhs)))),
      B(Postfix_sign::empty())
    { }
    Approximate_with_postfix_sign_tail_container_base(
                                Approximate_tail_pDVCLU_Opt_postfix_sign &&rhs)
     :A(std::move(static_cast<Approximate_tail_container&&>(rhs))),
      B(std::move(static_cast<Postfix_sign&&>(rhs)))
    { }
    Approximate_with_postfix_sign_tail_container_base(Postfix_sign && rhs)
     :A(Approximate_container::empty()),
      B(std::move(rhs))
    { }
  };

  using Approximate_with_postfix_sign_tail_container=
          Simple_container2<Approximate_with_postfix_sign_tail_container_base,
                            Approximate_tail_container,
                            Postfix_sign_container>;

  class Currency0_with_postfix_sign_tail:
                       public OR3C<Parser,
                                   Approximate_with_postfix_sign_tail_container,
                                   XChain,
                                   Approximate_tail_pDVCLU_Opt_postfix_sign,
                                   Opt_postfix_sign>
  { using OR3C::OR3C; };
  class Opt_currency_with_postfix_sign_zeros_tail:
                         public OPT<Parser,
                                    Currency0_with_postfix_sign_tail>
  { using OPT::OPT; };

  class Currency0_with_postfix_sign:
                             public AND2<Parser,
                                         Zeros,
                                         Currency0_with_postfix_sign_tail>
  { using AND2::AND2; };

  class Unsigned_currency1_Opt_Postfix_sign: public AND2<Parser,
                                                         Unsigned_currency1,
                                                         Opt_postfix_sign>
  { using AND2::AND2; };


  class Currency_with_postfix_sign_container_base:
                                            public Unsigned_currency_container,
                                            public Postfix_sign_container
  {
    using A= Unsigned_currency_container;
    using B= Postfix_sign_container;
  public:

    // Initialization from its components
    Currency_with_postfix_sign_container_base(A && a, B && b)
     :A(std::move(a)),
      B(std::move(b))
    { }

    // Initialization from rules
    Currency_with_postfix_sign_container_base(XChain && rhs)
     :A(Approximate_container(
         Decimal_container(Integer_container(std::move(rhs))),
         EEEE_container(EEEE_container::empty()))),
      B(Postfix_sign_container::empty())
    { }

    Currency_with_postfix_sign_container_base(
                                            Currency0_with_postfix_sign && rhs)
     :A(Unsigned_currency_container(
          Prefix_currency_container::empty(),
          Currency_prefix_flags_container(
            Currency_prefix_flags_container::empty()),
          Approximate_container(
            std::move(static_cast<Zeros&&>(rhs)),
            std::move(static_cast<Approximate_tail_container&&>(rhs))))),
      B(std::move(static_cast<Postfix_sign&&>(rhs)))
    { }

    Currency_with_postfix_sign_container_base(
                                    Unsigned_currency1_Opt_Postfix_sign && rhs)
     :A(Unsigned_currency_container(std::move(rhs))),
      B(std::move(static_cast<Postfix_sign&&>(rhs)))
    { }

    Currency_with_postfix_sign_container_base(Postfix_only_sign && rhs)
     :A(A::empty()),
      B(std::move(rhs))
    { }

    bool check(const Parser & parser,
               const Decimal_flag_counters & prefix_flag_counters) const
    {
      //DBUG_ASSERT(Fraction_container::m_cG_count == 0);

      // TODO: , and G cannot co-exist!!!

      // '$9C'  '9$C'
      if (prefix_flag_counters.m_dollar_count ||
          Integer_container::m_dollar_count ||
          Fraction_container::m_dollar_count)
      {
        if (Dec_delimiter_pDVCLU_container::length())
        {
          const char *dec= Dec_delimiter_pDVCLU_container::ptr();
          if (dec[0] == 'C'|| dec[0] == 'L'|| dec[0] == 'U')
          {
            parser.parse_error_at(current_thd, dec);
            return true;
          }
        }
        // '$.C'
        if (Postfix_currency_container::length())
        {
          parser.parse_error_at(current_thd, Postfix_currency_container::ptr());
          return true;
        }
      }

      // 'C0$', 'C$U' 'V$U'
      if (Prefix_currency_container::length() &&
          (Integer_container::m_dollar_count ||
           Fraction_container::m_dollar_count))
      {
        // TODO: better position
        parser.parse_error_at(current_thd, Prefix_currency_container::end());
        return true;
      }

      // '$.$'  'B.B'
      if (prefix_flag_counters.m_dollar_count +
          Integer_container::m_dollar_count + 
          Fraction_container::m_dollar_count > 1 ||
          prefix_flag_counters.m_B_count +
          Integer_container::m_B_count +
          Fraction_container::m_B_count > 1)
      {
        // TODO: better position
        parser.parse_error_at(current_thd, parser.buffer().str);
        return true;
      }

      if (Integer_container::length() == 0 && EEEE_container::length() != 0)
      {
        parser.parse_error_at(current_thd, EEEE_container::ptr());
        return true;
      }

      return false;
    }

    void print(String *str)
    {
      str->append("CFl='"_Lex_cstring_ex);
      Currency_prefix_flags_container::print(str);
      str->append("'"_Lex_cstring_ex);

      str->append("LCurr='"_Lex_cstring_ex);
      Prefix_currency_container::print(str);
      str->append("'"_Lex_cstring_ex);

      str->append("Dec='"_Lex_cstring_ex);
      Dec_delimiter_pDVCLU_container::print(str);
      str->append("'"_Lex_cstring_ex);

      str->append("RCurr='"_Lex_cstring_ex);
      Postfix_currency_container::print(str);
      str->append("'"_Lex_cstring_ex);

      str->append("RSign='"_Lex_cstring_ex);
      Postfix_sign_container::print(str);
      str->append("'"_Lex_cstring_ex);
    }

  };

  using Currency_with_postfix_sign_container=
           Simple_container2<Currency_with_postfix_sign_container_base,
                             Unsigned_currency_container,
                             Postfix_sign_container>;

  class Currency_with_postfix_sign:
                               public OR4C<Parser,
                                           Currency_with_postfix_sign_container,
                                           XChain,
                                           Currency0_with_postfix_sign,
                                           Unsigned_currency1_Opt_Postfix_sign,
                                           Postfix_only_sign>
  {
  public:
    using OR4C::OR4C;
  };




  /*
    format_FM_tail: currency_with_postfix_sign           #1
                  | 'S' [ unsigned_format ]              #2

    format: currency_with_postfix_sign                   #1
          | 'FM' [ format_FM_tail ]                      #2
          | 'S' ['FM'] [ unsigned_format ]               #3
          | postfix_only_sign                            #4
  */

  class Prefix_sign_Opt_unsigned_format: public AND2<Parser,
                                                     Prefix_sign,
                                                     Opt_unsigned_format>
  { using AND2::AND2; };


  class Format_FM_tail_container_base: public Prefix_sign_container,
                                       public Unsigned_format_container,
                                       public Postfix_sign_container
  {
    using A= Prefix_sign_container;
    using B= Unsigned_format_container;
    using C= Postfix_sign_container;
  public:

    // Initialization from its components
    Format_FM_tail_container_base(A && a, B && b, C && c)
     :A(std::move(a)),
      B(std::move(b)),
      C(std::move(c))
    { }

    // Initializing from rules
    Format_FM_tail_container_base(Format_TM_signature &&rhs)
     :A(A::empty()),
      B(Format_TM_container(std::move(rhs))),
      C(C::empty())
    { }

    Format_FM_tail_container_base(Currency_with_postfix_sign && rhs)
     :A(Prefix_sign_container::empty()),
      B(Unsigned_format_container(
          static_cast<Unsigned_currency_container&&>(rhs))),
      C(std::move(static_cast<Postfix_sign_container&&>(rhs)))
    { }

    Format_FM_tail_container_base(Prefix_sign_Opt_unsigned_format && rhs)
     :A(std::move(static_cast<Prefix_sign&&>(rhs))),
      B(std::move(static_cast<Unsigned_format_container&&>(rhs))),
      C(Postfix_sign_container::empty())
    { }
  };


  using Format_FM_tail_container=
          Simple_container3<Format_FM_tail_container_base,
                            Prefix_sign_container,
                            Unsigned_format_container,
                            Postfix_sign_container>;


  class Format_FM_tail: public OR3C<Parser,
                                    Format_FM_tail_container,
                                    //TODO: should work w the other order:
                                    Format_TM_signature,
                                    Prefix_sign_Opt_unsigned_format,
                                    Currency_with_postfix_sign>
  { using OR3C::OR3C; };
  class Opt_format_FM_tail: public OPT<Parser, Format_FM_tail>
  { using OPT::OPT; };


  class Format_FM_xxx: public AND2<Parser, FlagFM, Opt_format_FM_tail>
  { using AND2::AND2; };

  class Format_prefix_sign_Opt_FM_Opt_unsigned_format:
                                               public AND3<Parser,
                                                           Prefix_sign,
                                                           Opt_FM,
                                                           Opt_unsigned_format>
  { using AND3::AND3; };


  class Format_container_base: public Format_flags_container,
                               public Prefix_sign_container,
                               public Currency_with_postfix_sign_container,
                               public Format_TM_container
  {
    using A= Format_flags_container;
    using B= Prefix_sign_container;
    using C= Currency_with_postfix_sign_container;
    using D= Format_TM_container;
  public:

    // Initialization from its components
    Format_container_base(A && a, B && b, C && c, D && d)
     :A(std::move(a)),
      B(std::move(b)),
      C(std::move(c)),
      D(std::move(d))
    { }

    // Initialization from rules
    Format_container_base(Format_FM_xxx && rhs)
     :A(std::move(static_cast<FlagFM&&>(rhs))),
      B(std::move(static_cast<Prefix_sign_container&&>(rhs))),
      C(Currency_with_postfix_sign_container(
         std::move(static_cast<Unsigned_currency_container&&>(rhs)),
         Postfix_sign_container::empty())),
      D(std::move(static_cast<Format_TM_container&&>(rhs)))
    { }

    Format_container_base(Format_prefix_sign_Opt_FM_Opt_unsigned_format && rhs)
     :A(std::move(static_cast<FlagFM&&>(rhs))),
      B(std::move(static_cast<Prefix_sign&&>(rhs))),
      C(std::move(static_cast<Unsigned_currency_container&&>(rhs)),
        Postfix_sign_container::empty()),
      D(std::move(static_cast<Format_TM_container&&>(rhs)))
    { }

    Format_container_base(Postfix_only_sign && rhs)
     :A(Format_flags_container::empty()),
      B(Prefix_sign_container::empty()),
      C(Currency_with_postfix_sign_container(std::move(rhs))),
      D(Format_TM_container::empty())
    { }

    Format_container_base(Format_TM_signature && rhs)
     :A(Format_flags_container::empty()),
      B(Prefix_sign_container::empty()),
      C(Currency_with_postfix_sign_container::empty()),
      D(Format_TM_container(std::move(rhs)))
    { }

    void print(String *str)
    {
      str->append("FFl='"_Lex_cstring_ex);
      Format_flags_container::print(str);
      str->append("'"_Lex_cstring_ex);

      str->append("LSign='"_Lex_cstring_ex);
      Prefix_sign_container::print(str);
      str->append("'"_Lex_cstring_ex);

      if (Currency_with_postfix_sign_container::operator bool())
        Currency_with_postfix_sign_container::print(str);

      if (Format_TM_container::operator bool())
      {
        str->append("TM='"_Lex_cstring_ex);
        Format_TM_container::print(str);
        str->append("'"_Lex_cstring_ex);
      }
    }

    void print_as_note()
    {
      StringBuffer<64> tmp;
      print(&tmp);
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_UNKNOWN_ERROR,
                          "%.*s", (int) tmp.length(), tmp.ptr());
    }
  };


  using Format_container=
          Simple_container4<Format_container_base,
                            Format_flags_container,
                            Prefix_sign_container,
                            Currency_with_postfix_sign_container,
                            Format_TM_container>;
public:

  class Format: public OR5C<Parser, Format_container,
                            Currency_with_postfix_sign,
                            Format_FM_xxx,
                            Format_prefix_sign_Opt_FM_Opt_unsigned_format,
                            Format_TM_signature,
                            Postfix_only_sign>
  {
  public:
    using OR5C::OR5C;
    bool check(const Parser & parser) const
    {
      if (!operator bool())
      {
        parser.push_warning_syntax_error(current_thd, 0);
        return true;
      }
      const auto prefix_flag_counters= Format_container::prefix_flag_counters();
      if (Currency_with_postfix_sign_container::operator bool() &&
          Currency_with_postfix_sign_container_base::check(parser,
                                                      prefix_flag_counters))
        return true;
      return false;
    }

  };

  class Opt_format: public OPT<Parser, Format>
  { using OPT::OPT; };


#ifndef DBUG_OFF

  // A helper type for debugging, to trace subrules
  using Goal2= AND2<Parser, Format_FM_xxx, TokenEOF>;
#endif

public:
  // goal: [ format ] EOF
  using Goal= AND2<Parser, Opt_format, TokenEOF>;

};


double Item_func_to_number::val_real()
{
  StringBuffer<STRING_BUFFER_USUAL_SIZE> buffer0, buffer1;
  String *subject= args[0]->val_str(&buffer0);
  if ((null_value= !subject))
    return 0;
  String *fmt= args[1]->val_str(&buffer1);
  if ((null_value= !fmt))
    return 0;

  //Format_parser::Res2 res(&parser);
  //if ((null_value= !res))
  //  return 0;

  CHARSET_INFO *cs= args[1]->collation.collation;
  Format_parser parser(current_thd, cs, fmt->to_lex_cstring());
  Format_parser::Goal res(&parser);
  if ((null_value= res.check(parser)))
    return 0;
  DBUG_EXECUTE_IF("numconv",(res.print_as_note()););

  return 1;
}
