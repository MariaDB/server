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
//#include "sql_type_int.h"
#include "simple_parser.h"

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

  class Token: public Lex_cstring
  {
  protected:
    TokenID m_id;
  public:
    Token()
     :Lex_cstring(), m_id(TokenID::tNULL)
    { }
    Token(const LEX_CSTRING &str, TokenID id)
     :Lex_cstring(str), m_id(id)
    { }
    TokenID id() const { return m_id; }
    static Token empty(const char *pos)
    {
      return Token(Lex_cstring(pos, pos), TokenID::tEMPTY);
    }
    operator bool() const
    {
      return m_id != TokenID::tNULL;
    }
  };

//protected:
  Token get_token(CHARSET_INFO *cs);
};


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
          return m_ptr+= 3, Token(str, TokenID::tTM9);
        if (m_ptr[2]=='E')
          return m_ptr+= 3, Token(str, TokenID::tTME);
      }
      return m_ptr+=2, Token(str, TokenID::tTM);
    }
  }

  if (m_ptr + 4 <= m_end)
  {
    const LEX_CSTRING str= Lex_cstring(m_ptr, 4);
    m_ptr+= 4;
    if ("EEEE"_Lex_ident_column.streq(str)) return Token(str, TokenID::tEEEE);
    m_ptr-= 4;
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


  void push_warning_syntax_error(THD *thd, uint start_lineno)
  {
    DBUG_ASSERT(m_start <= m_ptr);
    DBUG_ASSERT(m_ptr <= m_end);
    const char *msg= ER_THD(thd, ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR);
    ErrConvString txt(m_look_ahead_token.str,
                      m_end - m_look_ahead_token.str,
                      //strlen(m_look_ahead_token.str),
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


//private:

  using Parser= Format_parser; // for a shorter notation


  // A common parent class for various containers
  class Lex_cstring_container: public Lex_cstring
  {
  public:
    using Lex_cstring::Lex_cstring;
    static Lex_cstring_container empty(const Parser &parser)
    {
      return parser.empty_token();
    }
    const char *ptr() const { return str; }
    const char *end() const { return str ? str + length() : nullptr; }
    size_t length() const { return Lex_cstring::length; }
    /*
      Contatenate list elements into a single container.
      There must be no any delimiters in between.
    */
    bool concat(const Lex_cstring &rhs)
    {
      if (!ptr())
      {
        Lex_cstring::operator=(rhs);
        return false;
      }
      if (end() != rhs.str)
        return true;
      Lex_cstring::length+= rhs.length;
      return false;
    }
    // Methods that allow to pass it as a container to LIST.
    size_t count() const { return length(); }
    bool add(Parser *p, const Lex_cstring &rhs)
    {
      return concat(rhs);
    }
  };


  class Dec_delimiter_pDVCLU_container: public Lex_cstring_container
  {
  public:
    using Lex_cstring_container::Lex_cstring_container;
  };

  class Currency_CLU_container: public Lex_cstring_container
  {
  public:
    using Lex_cstring_container::Lex_cstring_container;
  };


  // Rules consisting of a single token
  class TokenEOF: public TOKEN<Parser, TokenID::tEOF> { using TOKEN::TOKEN; };
  class TokenS: public TOKEN<Parser, TokenID::tS>     { using TOKEN::TOKEN; };
  class TokenFM: public TOKEN<Parser, TokenID::tFM>   { using TOKEN::TOKEN; };
  class EEEE: public TOKEN<Parser, TokenID::tEEEE>    { using TOKEN::TOKEN; };


  /*
    The following chains are returned by the tokenizer as a single token:

      zeros: '0' [ '0'... ]
      nines: '9' [ '9'... ]
      xchain: 'X' [ 'X'...]
  */
  class Zeros: public TOKEN<Parser, TokenID::tZEROS>   { using TOKEN::TOKEN; };
  class Nines: public TOKEN<Parser, TokenID::tNINES>   { using TOKEN::TOKEN; };
  class XChain: public TOKEN<Parser, TokenID::tXCHAIN> { using TOKEN::TOKEN; };


  // Optional token rules
  class Opt_EEEE: public OPT<Parser, EEEE>  { using OPT::OPT; };
  class Opt_FM: public OPT<Parser, TokenFM> { using OPT::OPT; };


  // Rules consisting of token choices with their Opt_ variants (when needed)

  // decimal_flag: 'B' | '$'
  class Decimal_flag_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    { return id == TokenID::tB || id == TokenID::tDOLLAR; }
  };
  class Decimal_flag: public TokenChoice<Parser, Decimal_flag_cond>
  { using TokenChoice::TokenChoice; };


  // group_separator: ',' | 'G'
  class Group_separator_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    { return id == TokenID::tCOMMA || id == TokenID::tG; }
  };
  class Group_separator: public TokenChoice<Parser, Group_separator_cond>
  { using TokenChoice::TokenChoice; };


  // zeros_or_nines: zeros | nines
  class Zeros_or_nines_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    { return id == TokenID::tZEROS || id == TokenID::tNINES; }
  };
  class Zeros_or_nines: public TokenChoice<Parser, Zeros_or_nines_cond>
  { using TokenChoice::TokenChoice; };


  // non_group_decimal_element: zeros_or_nines | decimal_flag
  class Non_group_decimal_element_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return Zeros_or_nines_cond::allowed_token_id(id) ||
             Decimal_flag_cond::allowed_token_id(id);
    }
  };
  class Non_group_decimal_element:
                           public TokenChoice<Parser,
                                              Non_group_decimal_element_cond>
  { using TokenChoice::TokenChoice; };


  // integer_element_or_group: non_group_decimal_element | group_separator
  class Integer_element_or_group_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return Non_group_decimal_element_cond::allowed_token_id(id) ||
             Group_separator_cond::allowed_token_id(id);
    }
  };
  class Integer_element_or_group:
                           public TokenChoice<Parser,
                                              Integer_element_or_group_cond>
  { using TokenChoice::TokenChoice; };


  // postfix_sign: 'S' | 'MI' | 'PR'
  class Postfix_sign_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::tS || id == TokenID::tMI || id == TokenID::tPR;
    }
  };
  class Postfix_sign: public TokenChoice<Parser, Postfix_sign_cond>
  { using TokenChoice::TokenChoice; };

  class Opt_postfix_sign: public OPT<Parser, Postfix_sign> { using OPT::OPT; };


  // format_TM_signature:      'TM' | 'TM9' | 'TME'
  class Format_TM_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::tTM || id == TokenID::tTM9 || id == TokenID::tTME;
    }
  };
  class Format_TM_signature: public TokenChoice<Parser, Format_TM_cond>
  { using TokenChoice::TokenChoice; };


  // positional_currency: 'C' | 'L' | 'U'
  class Positional_currency_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::tC || id == TokenID::tL || id == TokenID::tU;
    }
  };
  class Positional_currency: public TokenChoice<Parser,
                                                Positional_currency_cond>
  { using TokenChoice::TokenChoice; };

  class Opt_positional_currency: public OPT<Parser, Positional_currency>
  { using OPT::OPT; };


  // fraction_pDV_signature: '.' | 'D' | 'V'
  class Fraction_pDV_signature_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::tPERIOD || id == TokenID::tD || id == TokenID::tV;
    }
  };
  class Fraction_pDV_signature: public TokenChoice<Parser,
                                                   Fraction_pDV_signature_cond>
  { using TokenChoice::TokenChoice; };

  // TODO: removed the grammar:
  // TODO: fraction_pDVCLU_signature: '.' | 'D' | 'V' | 'C' | 'L' | 'U'



  /*
    Rules consisting of a LIST of token choices
  */

  // decimal_flags: decimal_flag [ decimal_flag...]
  class Decimal_flags_container: public Lex_cstring_container
  {
  public:
    using Lex_cstring_container::Lex_cstring_container;
  };
  class Decimal_flags: public LIST<Parser, Decimal_flags_container,
                                   Decimal_flag,
                                   TokenID::tNULL/*not separated*/, 1>
  { using LIST::LIST; };


  // integer_with_group: integer_element_or_group [ integer_element_or_group...]
  class Integer_with_group_container: public Lex_cstring_container
  {
  public:
    using Lex_cstring_container::Lex_cstring_container;
  };
  class Integer_with_group: public LIST<Parser, Integer_with_group_container,
                                        Integer_element_or_group,
                                        TokenID::tNULL/*not separated*/, 1>
  { using LIST::LIST; };


  // fraction_body: non_group_decimal_element [ non_group_decimal_element ... ]
  class Fraction_body_container: public Lex_cstring_container
  {
  public:
    using Lex_cstring_container::Lex_cstring_container;
  };
  // The grammar needs only an optional fraction_body
  class Opt_fraction_body: public LIST<Parser, Fraction_body_container,
                                       Non_group_decimal_element,
                                       TokenID::tNULL/*not separated list*/, 0>
  { using LIST::LIST; };



  /********** More complex rules here **********/


  // fraction_pDV: fraction_pDV_signature [ fraction_body ]
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


  /*
    TODO: changed the frammar
    < currency_fraction: positional_currency [ fraction_body ]
    < fraction_pDVCLU: fraction_pDV | currency_fraction

    To this:

    fraction_pDVCLU: positional_currency [ fraction_body ]                 #1
        | fraction_pDV_signature [ fraction_body ] [ positional_currency ] #2
  */

  class Fraction_pDVCLU_1: public AND2<Parser,
                                       Positional_currency,
                                       Opt_fraction_body>
  { using AND2::AND2; };

  class Fraction_pDVCLU_2: public AND3<Parser,
                                       Fraction_pDV_signature,
                                       Opt_fraction_body,
                                       Opt_positional_currency>
  { using AND3::AND3; };


  class Fraction_container
  {
  public:
    Dec_delimiter_pDVCLU_container m_decimal_delimiter_pDVCLU;
    Fraction_body_container m_fraction_body;
    Currency_CLU_container m_trailing_currency_CLU;

    Fraction_container()
    { }

    Fraction_container(Fraction_pDV && rhs)
     :m_decimal_delimiter_pDVCLU(
         std::move(static_cast<Fraction_pDV_signature&&>(rhs))),
      m_fraction_body(
         std::move(static_cast<Fraction_body_container&&>(rhs)))
    { }

    Fraction_container(Fraction_pDVCLU_1 && rhs)
     :m_decimal_delimiter_pDVCLU(
         std::move(static_cast<Positional_currency&&>(rhs))),
      m_fraction_body(
         std::move(static_cast<Fraction_body_container&&>(rhs)))
    { }

    Fraction_container(Fraction_pDVCLU_2 && rhs)
     :m_decimal_delimiter_pDVCLU(
         std::move(static_cast<Fraction_pDV_signature&&>(rhs))),
      m_fraction_body(
         std::move(static_cast<Fraction_body_container&&>(rhs))),
      m_trailing_currency_CLU(
         std::move(static_cast<Opt_positional_currency&&>(rhs)))
    { }


    Fraction_container(Dec_delimiter_pDVCLU_container && dec,
                       Fraction_body_container && body,
                       Currency_CLU_container && trailing_currency)
     :m_decimal_delimiter_pDVCLU(std::move(dec)),
      m_fraction_body(std::move(body)),
      m_trailing_currency_CLU(std::move(trailing_currency))
    { }

    static Fraction_container empty(const Parser &parser)
    {
      Fraction_container ec(Dec_delimiter_pDVCLU_container::empty(parser),
                            Fraction_body_container::empty(parser),
                            Currency_CLU_container::empty(parser));
      return ec;
    }

    operator bool() const
    {
      return m_decimal_delimiter_pDVCLU.str != nullptr;
    }
  };

  class Fraction_pDVCLU: public OR2C<Parser,
                                     Fraction_container,
                                     Fraction_pDVCLU_1,
                                     Fraction_pDVCLU_2>
  { using OR2C::OR2C; };

  class Opt_fraction_pDVCLU: public OPT<Parser, Fraction_pDVCLU>
  { using OPT::OPT; };


  /*
    decimal_with_group_pDVCLU: integer_with_group [ fraction_pDVCLU ]
                             | fraction_pDVCLU
  */
  class Decimal_with_group_pDVCLU_choice1: public AND2<Parser,
                                                       Integer_with_group,
                                                       Opt_fraction_pDVCLU>
  { using AND2::AND2; };


  /*
    decimal_with_group_pDV: integer_with_group [ fraction_pDV ]
                          | fraction_pDV
  */
  class Decimal_with_group_pDV_choice1: public AND2<Parser,
                                                    Integer_with_group,
                                                    Opt_fraction_pDV>
  { using AND2::AND2; };


  class Decimal_container: public Integer_with_group_container,
                           public Fraction_container
  {
  public:
    Decimal_container()
    { }
    Decimal_container(Integer_with_group_container && a,
                      Fraction_pDVCLU && b)
     :Integer_with_group_container(std::move(a)),
      Fraction_container(std::move(b))
    { }
    Decimal_container(Decimal_with_group_pDVCLU_choice1 && rhs)
     :Integer_with_group_container(std::move(rhs))
    { }
    Decimal_container(Decimal_with_group_pDV_choice1 && rhs)
     :Integer_with_group_container(std::move(rhs))
    { }
    Decimal_container(Fraction_pDVCLU && rhs)
     :Fraction_container(std::move(rhs))
    { }
    Decimal_container(Fraction_pDV && rhs)
     :Fraction_container(std::move(rhs))
    { }
    static Decimal_container empty(const Parser &parser)
    {
      return Decimal_container(Integer_with_group_container::empty(parser),
                               Fraction_container::empty(parser));
    }
    operator bool() const
    {
      return Integer_with_group_container::count() ||
             Fraction_container::operator bool();
    }
  };

  class Decimal_with_group_pDVCLU: public OR2C<
                                            Parser,
                                            Decimal_container,
                                            Decimal_with_group_pDVCLU_choice1,
                                            Fraction_pDVCLU>
  { using OR2C::OR2C; };


  class Decimal_with_group_pDV: public OR2C<Parser,
                                            Decimal_container,
                                            Decimal_with_group_pDV_choice1,
                                            Fraction_pDV>
  { using OR2C::OR2C; };



  /*
    numeric_tail_pDVCLU: decimal_with_group_pDVCLU [ EEEE ]
                       | EEEE
  */
  class numeric_tail_pDVCLU_choice1: public AND2<Parser,
                                                 Decimal_with_group_pDVCLU,
                                                 Opt_EEEE>
  { using AND2::AND2; };


  /*
    numeric_tail_pDV: decimal_with_group_pDV [ EEEE ]
                    | EEEE
  */
  class numeric_tail_pDV_choice1: public AND2<Parser,
                                              Decimal_with_group_pDV,
                                              Opt_EEEE>
  { using AND2::AND2; };


  class Numeric_container: public Decimal_container,
                           public EEEE
  {
  public:
    Numeric_container()
    { }
    Numeric_container(numeric_tail_pDVCLU_choice1 && rhs)
     :Decimal_container(std::move(rhs)),
      EEEE(std::move(rhs))
    { }
    Numeric_container(numeric_tail_pDV_choice1 && rhs)
     :Decimal_container(std::move(rhs)),
      EEEE(std::move(rhs))
    { }
    Numeric_container(EEEE && rhs)
     :EEEE(std::move(rhs))
    { }
    operator bool() const
    {
      return Decimal_container::operator bool() || EEEE::operator bool();
    }
    static Numeric_container empty(const Parser &parser)
    {
      Numeric_container tmp;
      tmp.Decimal_container::operator=(Decimal_container::empty(parser));
      tmp.EEEE::operator=(EEEE::empty(parser));
      return tmp;
    }
  };

  class Numeric_tail_pDVCLU: public OR2C<Parser,
                                         Numeric_container,
                                         numeric_tail_pDVCLU_choice1,
                                         Opt_EEEE>
  { using OR2C::OR2C; };

  class Opt_numeric_tail_pDVCLU: public OPT<Parser, Numeric_tail_pDVCLU>
  { using OPT::OPT; };


  class Numeric_tail_pDV: public OR2C<Parser,
                                      Numeric_container,
                                      numeric_tail_pDV_choice1,
                                      Opt_EEEE>
  { using OR2C::OR2C; };

  class Opt_numeric_tail_pDV: public OPT<Parser, Numeric_tail_pDV>
  { using OPT::OPT; };



  // number0: zeros [ numeric_tail_pDVCLU ]
  class Number0_zeros: public Zeros { using Zeros::Zeros; };

  class Number0: public AND2<Parser, Number0_zeros, Opt_numeric_tail_pDVCLU>
  {
  public:
    using AND2::AND2;
    static Number0 empty(const Parser &parser)
    {
      return Number0(Zeros::empty(parser),
                     Opt_numeric_tail_pDVCLU::empty(parser));
    }
  };


  /*
    TODO: modified grammar: added #2b

    number1: nines                        [ numeric_tail_pDVCLU ]    -- #1
           | decimal_flags zeros_or_nines [ numeric_tail_pDVCLU ]    -- #2a
           | decimal_flags                [ fraction_pDVCLU ]        -- #2b
           | positional_currency zeros_or_nines [ numeric_tail_pDV ] -- #3a
           | positional_currency [ fraction_pDV ]                    -- #3b
           | fraction_pDV [ positional_currency ]                    -- #4
  */
  class Number1_nines: public Nines {  using Nines::Nines; };
  class Number1_choice1: public AND2<Parser,
                                     Number1_nines,
                                     Opt_numeric_tail_pDVCLU>
  {
  public:
    using AND2::AND2;
    static Number1_choice1 empty2(const Parser &p)
    {
      return Number1_choice1(Number1_nines::empty(p),
                             Opt_numeric_tail_pDVCLU::empty(p));
    }
  };

  class Number1_choice2_zeros_or_nines: public Zeros_or_nines
  {
  public:
    using Zeros_or_nines::Zeros_or_nines;
  };
  class Number1_choice2_tail1: public AND2<Parser,
                                           Number1_choice2_zeros_or_nines,
                                           Opt_numeric_tail_pDVCLU>
  {
  public:
    using AND2::AND2;
    static Number1_choice2_tail1 empty(const Parser &p)
    {
      return Number1_choice2_tail1(Number1_choice2_zeros_or_nines::empty(p),
                                   Opt_numeric_tail_pDVCLU::empty(p));
    }
  };
  class Number1_choice2_tail: public OR2<Parser,
                                         Number1_choice2_tail1,
                                         Opt_fraction_pDVCLU>
  {
  public:
    using OR2::OR2;
    static Number1_choice2_tail empty(const Parser &p)
    {
      Number1_choice2_tail tmp;
      tmp.Number1_choice2_tail1::operator=(Number1_choice2_tail1::empty(p));
      tmp.Opt_fraction_pDVCLU::operator=(Opt_fraction_pDVCLU::empty(p));
      return tmp;
    }
  };

  class Number1_choice2: public AND2<Parser,
                                     Decimal_flags,
                                     Number1_choice2_tail>
  {
  public:
    using AND2::AND2;
    static Number1_choice2 empty2(const Parser &p)
    {
      return Number1_choice2(Decimal_flags::empty(p),
                             Number1_choice2_tail::empty(p));
    }
  };

  class Zeros_or_nines_opt_numeric_tail_pDV: public AND2<Parser,
                                                         Zeros_or_nines,
                                                         Opt_numeric_tail_pDV>
  {
  public:
    using AND2::AND2;
    static Zeros_or_nines_opt_numeric_tail_pDV empty(const Parser &parser)
    {
      return Zeros_or_nines_opt_numeric_tail_pDV(
               Zeros_or_nines::empty(parser),
               Opt_numeric_tail_pDV::empty(parser));
    }
  };

  class Number1_choice3_tail: public OR2<Parser,
                                         Zeros_or_nines_opt_numeric_tail_pDV,
                                         Opt_fraction_pDV>
  {
  public:
    using OR2::OR2;
    static Number1_choice3_tail empty2(const Parser &parser)
    {
      // TODO: hack
      Number1_choice3_tail tmp;
      tmp.Zeros_or_nines_opt_numeric_tail_pDV::operator=
         (Zeros_or_nines_opt_numeric_tail_pDV::empty(parser));
      tmp.Opt_fraction_pDV::operator=(Opt_fraction_pDV::empty(parser));
      return tmp;
    }
  };

  class Number1_choice3: public AND2<Parser,
                                     Positional_currency,
                                     Number1_choice3_tail>
  {
  public:
    using AND2::AND2;
    static Number1_choice3 empty2(const Parser &parser)
    {
      // TODO: hack
      Number1_choice3 tmp;
      tmp.Positional_currency::operator=(Positional_currency::empty(parser));
      tmp.Number1_choice3_tail::operator=(Number1_choice3_tail::empty2(parser));
      return tmp;
    }
  };

  /*
    TODO: fixed grammar here: added EEEE
      <  | fraction_pDV        [ positional_currency ]             -- #4
      >  | fraction_pDV [EEEE] [ positional_currency ]             -- #4
  */
  class Number1_choice4: public AND3<Parser,
                                     Fraction_pDV,
                                     Opt_EEEE,
                                     Opt_positional_currency>
  {
  public:
    using AND3::AND3;

    static Number1_choice4 empty2(const Parser &parser)
    {
      return Number1_choice4(Fraction_pDV::empty(parser),
                             Opt_EEEE::empty(parser),
                             Opt_positional_currency::empty(parser));
    }
  };

  class Number1: public OR4<Parser,
                            Number1_choice1,
                            Number1_choice2,
                            Number1_choice3,
                            Number1_choice4>
  {
  public:
    using OR4::OR4;
    static Number1 empty(const Parser &parser)
    {
      // TODO: hack
      Number1 tmp;
      tmp.Number1_choice1::operator=(Number1_choice1::empty2(parser));
      tmp.Number1_choice2::operator=(Number1_choice2::empty2(parser));
      tmp.Number1_choice3::operator=(Number1_choice3::empty2(parser));
      tmp.Number1_choice4::operator=(Number1_choice4::empty2(parser));
      return tmp;
    }
  };


  /*
    format_without_prefix_sign: xchain                                 #1
                      | zeros xchain                                   #2
                      | zeros [ numeric_tail_pDVCLU ] [ postfix_sign ] #3
                      | number1 [ postfix_sign ]                       #4
                      | format_TM_signature                            #5
  */

  class Format_without_prefix_sign_choice1: public XChain
  { using XChain::XChain; };

  /*
    Rewriting branches #2 and #3 as:
    zero [ zero_tail ]
    zero_tail: xchain
             | numeric_tail_pDVCLU [ postfix_sign ]
             | postfix_sign
  */

  class Opt_postfix_sign23a: public Opt_postfix_sign
  {
  public:
    using Opt_postfix_sign::Opt_postfix_sign;
  };
  class Numeric_tail_pDVCLU_Opt_postfix_sign: public AND2<Parser,
                                                          Numeric_tail_pDVCLU,
                                                          Opt_postfix_sign23a>
  {
  public:
    using AND2::AND2;
    static Numeric_tail_pDVCLU_Opt_postfix_sign empty2(const Parser &parser)
    {
      // TODO: hack
      Numeric_tail_pDVCLU_Opt_postfix_sign tmp;
      tmp.Numeric_tail_pDVCLU::operator=(Numeric_tail_pDVCLU::empty(parser));
      tmp.Opt_postfix_sign23a::operator=(Opt_postfix_sign23::empty(parser));
      return tmp;
    }
  };

  class XChain23: public XChain { using XChain::XChain; };

  class Opt_postfix_sign23: public Opt_postfix_sign
  { using Opt_postfix_sign::Opt_postfix_sign; };

  class Format_without_prefix_sign_23_tail:
                              public OR3<Parser,
                                         XChain23,
                                         Numeric_tail_pDVCLU_Opt_postfix_sign,
                                         Opt_postfix_sign23>
  {
  public:
    using OR3::OR3;
    static Format_without_prefix_sign_23_tail empty(const Parser &parser)
    {
      // TODO: hack
      Format_without_prefix_sign_23_tail tmp;
      tmp.XChain23::operator=(XChain23::empty(parser));
      tmp.Numeric_tail_pDVCLU_Opt_postfix_sign::operator=
         (Numeric_tail_pDVCLU_Opt_postfix_sign::empty2(parser));
      tmp.Opt_postfix_sign23::operator=
         (Opt_postfix_sign23::empty(parser));
      return tmp;
    }
  };

  class Opt_format_without_prefix_sign_23_tail:
                           public OPT<Parser,
                                      Format_without_prefix_sign_23_tail>
  { using OPT::OPT; };


  class Format_without_prefix_sign_choice23:
                          public AND2<Parser,
                                      Zeros,
                                      Opt_format_without_prefix_sign_23_tail>
  {
  public:
    using AND2::AND2;
    static Format_without_prefix_sign_choice23 empty2(const Parser &parser)
    {
      Format_without_prefix_sign_choice23 tmp;
      ((Zeros) tmp)= Zeros::empty(parser);
      tmp.Opt_format_without_prefix_sign_23_tail::operator=
         (Format_without_prefix_sign_23_tail::empty(parser));
      return tmp;
    }
  };

  class Format_without_prefix_sign_choice4: public AND2<Parser,
                                                        Number1,
                                                        Opt_postfix_sign>
  {
  public:
    using AND2::AND2;
    static Format_without_prefix_sign_choice4 empty2(const Parser &parser)
    {
      Format_without_prefix_sign_choice4 tmp;
      tmp.Number1::operator=(Number1::empty(parser));
      tmp.Opt_postfix_sign::operator=(Opt_postfix_sign::empty(parser));
      return tmp;
    }
  };

  class Format_without_prefix_sign:
                              public OR4<Parser,
                                        Format_without_prefix_sign_choice1,
                                        Format_without_prefix_sign_choice23,
                                        Format_without_prefix_sign_choice4,
                                        Format_TM_signature>
  {
  public:
    using OR4::OR4;
    static Format_without_prefix_sign empty(const Parser &parser)
    {
      Format_without_prefix_sign tmp;
      tmp.Format_without_prefix_sign_choice1::operator=
         (tmp.Format_without_prefix_sign_choice1::empty(parser));
      tmp.Format_without_prefix_sign_choice23::operator=
         (tmp.Format_without_prefix_sign_choice23::empty2(parser));
      tmp.Format_without_prefix_sign_choice4::operator=
         (tmp.Format_without_prefix_sign_choice4::empty2(parser));
      return tmp;
    }
  };

  /*
    format_with_prefix_sign: number0
                           | number1
                           | format_TM_signature
  */

  class Format_with_prefix_sign: public OR3<Parser,
                                            Number0,
                                            Number1,
                                            Format_TM_signature>
  {
  public:
    using OR3::OR3;
    static Format_with_prefix_sign empty(const Parser &parser)
    {
      Format_with_prefix_sign tmp;
      tmp.Number0::operator=(Number0::empty(parser));
      tmp.Number1::operator=(Number1::empty(parser));
      tmp.Format_TM_signature::operator=(Format_TM_signature::empty(parser));
      return tmp;
    }
  };


  /*
    format: format_without_prefix_sign                           #1
          | prefix_with_sign [ format_with_prefix_sign ]         #2
          | prefix_without_sign [ format_without_prefix_sign ]   #3


    Rewritten to:

    format_FM_tail: format_without_prefix_sign           #1
                  | 'S' [ format_with_prefix_sign ]      #2

    format: format_without_prefix_sign                   #1
          | 'FM' [ format_FM_tail ]                      #2
          | 'S' ['FM'] [ format_with_prefix_sign ]       #3
  */

  class Opt_format_with_prefix_sign: public OPT<Parser,
                                                Format_with_prefix_sign>
  { using OPT::OPT; };

  class Format_FM_tail_choice2: public AND2<Parser,
                                            TokenS,
                                            Opt_format_with_prefix_sign>
  {
  public:
    using AND2::AND2;
    static Format_FM_tail_choice2 empty2(const Parser &parser)
    {
      Format_FM_tail tmp;
      tmp.TokenS::operator= (TokenS::empty(parser));
      tmp.Opt_format_with_prefix_sign::operator=
         (tmp.Opt_format_with_prefix_sign::empty(parser));
      return tmp;
    }
  };
  class Format_FM_tail: public OR2<Parser,
                                   Format_without_prefix_sign,
                                   Format_FM_tail_choice2>
  {
  public:
    using OR2::OR2;
    static Format_FM_tail empty(const Parser &parser)
    {
      Format_FM_tail tmp;
      tmp.Format_without_prefix_sign::operator=
         (Format_without_prefix_sign::empty(parser));
      tmp.Format_FM_tail_choice2::operator=
         (Format_FM_tail_choice2::empty2(parser));
      return tmp;
    }
  };
  class Opt_format_FM_tail: public OPT<Parser, Format_FM_tail>
  { using OPT::OPT; };


  class Format_1: public Format_without_prefix_sign
  {
    using Format_without_prefix_sign::Format_without_prefix_sign;
  };
  class Format_2: public AND2<Parser, TokenFM, Opt_format_FM_tail>
  {
    using AND2::AND2;
  };
  class Format_3: public AND3<Parser,
                              TokenS,
                              Opt_FM,
                              Opt_format_with_prefix_sign>
  { using AND3::AND3; };

  class Format: public OR3<Parser, Format_1, Format_2, Format_3>
  { using OR3::OR3; };

public:
  class Res: public AND2<Parser,
                         Format,
                         //Format_1,/*OK*/
                         //Format_2,/*OK*/
                         //Format_3,/*OK*/
                         //Format_FM_tail2,/*OK*/
                         //Format_FM_tail,/*OK*/
                         //Opt_format_FM_tail,/*OK*/
                         //Fraction_pDVCLU,/*OK*/
                         //Opt_fraction_pDVCLU,/*OK*/
                         //Opt_format_without_prefix_sign,
                         //Opt_format_with_prefix_sign,/*OK*/
                         //Integer_with_group,
                         //Decimal_with_group_pDVCLU,
                         //Numeric_tail_pDVCLU,
                         //Opt_numeric_tail_pDVCLU,/*ERR->OK with hach*/
                         //Format_without_prefix_sign,/*ERR->OK with hack*/
                         //Format_with_prefix_sign,/*OK*/
                         //XChain23, /*OK*/
                         //Opt_postfix_sign,/*OK*/
                         //Numeric_tail_pDVCLU_Opt_postfix_sign,/*OK*/
                         //Opt_postfix_sign23,/*OK*/
                         //Format_without_prefix_sign_23_tail,
                         //Opt_format_without_prefix_sign_23_tail,
                         //Format_without_prefix_sign_choice23,

                         //Format_without_prefix_sign_choice23,
                         //Format_without_prefix_sign_choice3,/*OK*/
                         //Format_without_prefix_sign_choice4,/*ERR->OK*/
                         //Number1,

                         //Nines, /*OK*/
                         //Numeric_tail_pDVCLU,/*OK*/
                         //numeric_tail_pDVCLU_choice1,/*OK*/
                         //numeric_tail_pDVCLU_choice2,/*OK*/

                         //Decimal_with_group_pDVCLU,
                         //Number1_choice1,
                         //Number1,
                         //Format_without_prefix_sign,
                         //Format_without_prefix_sign_choice3,
                         //Format_without_prefix_sign_choice4,

                         //Opt_fraction_pDV, /*ERR -> OK*/
                         //Fraction_pDV, /*OK*/
                         //OPT<Parser, Fraction_pDV_signature>, /*OK*/
                         //Opt_fraction_body, /*OK*/
                         //Opt_fraction_pDVCLU, /*ERR->OK*/
                         //Opt_positional_currency, /*OK*/
                         //Fraction_pDVCLU, /*OK*/
                         TokenEOF>
  {
  public:
    using AND2::AND2;
  };

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

  CHARSET_INFO *cs= args[1]->collation.collation;

  /*
  Format_tokenizer t(cs, fmt->to_lex_cstring());
  Format_tokenizer::Token tok;
  for (tok= t.get_token(cs) ;
       tok && tok.id() != Format_tokenizer::TokenID::tEOF;
       tok= t.get_token(cs))
  { }
  */

  Format_parser parser(current_thd, cs, fmt->to_lex_cstring());
  Format_parser::Res res(&parser);
  if (!res)
    parser.push_warning_syntax_error(current_thd, 0);

  return (bool) res;
}
