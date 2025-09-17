#ifndef OPT_HINTS_PARSER_H
#define OPT_HINTS_PARSER_H
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

#include "lex_ident_sys.h"
#include "simple_tokenizer.h"
#include "sql_list.h"
#include "sql_string.h"
#include "sql_type_int.h"
#include "simple_parser.h"

class st_select_lex;
class Opt_hints_qb;

/**
  Hint types, MAX_HINT_ENUM should be always last.
  This enum should be synchronized with opt_hint_info
  array(see opt_hints.cc).
*/
enum opt_hints_enum
{
  BKA_HINT_ENUM= 0,
  BNL_HINT_ENUM,
  ICP_HINT_ENUM,
  MRR_HINT_ENUM,
  NO_RANGE_HINT_ENUM,
  QB_NAME_HINT_ENUM,
  MAX_EXEC_TIME_HINT_ENUM,
  SEMIJOIN_HINT_ENUM,
  SUBQUERY_HINT_ENUM,
  JOIN_PREFIX_HINT_ENUM,
  JOIN_SUFFIX_HINT_ENUM,
  JOIN_ORDER_HINT_ENUM,
  JOIN_FIXED_ORDER_HINT_ENUM,
  DERIVED_CONDITION_PUSHDOWN_HINT_ENUM,
  MERGE_HINT_ENUM,
  SPLIT_MATERIALIZED_HINT_ENUM,
  INDEX_HINT_ENUM,
  JOIN_INDEX_HINT_ENUM,
  GROUP_INDEX_HINT_ENUM,
  ORDER_INDEX_HINT_ENUM,
  ROWID_FILTER_HINT_ENUM,
  MAX_HINT_ENUM // This one must be the last in the list
};


/**
  Environment data for the name resolution phase
*/
struct Parse_context {
  THD * const thd;              ///< Current thread handler
  MEM_ROOT *mem_root;           ///< Current MEM_ROOT
  st_select_lex * select;       ///< Current SELECT_LEX object

  Parse_context(THD *thd, st_select_lex *select);
};


class Optimizer_hint_tokenizer: public Extended_string_tokenizer
{
public:
  Optimizer_hint_tokenizer(CHARSET_INFO *cs, const LEX_CSTRING &hint)
   :Extended_string_tokenizer(cs, hint)
  { }

  // Let's use "enum class" to easier distinguish token IDs vs rule names
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
    tAT= '@',
    tLPAREN= '(',
    tRPAREN= ')',
    // Other token types
    tIDENT= 'i',
    tUNSIGNED_NUMBER= 'n',

    // Keywords
    keyword_BKA = 256, // Value must be greater than any of the above
    keyword_BNL,
    keyword_NO_BKA,
    keyword_NO_BNL,
    keyword_NO_ICP,
    keyword_NO_MRR,
    keyword_NO_RANGE_OPTIMIZATION,
    keyword_MRR,
    keyword_QB_NAME,
    keyword_MAX_EXECUTION_TIME,
    keyword_SEMIJOIN,
    keyword_NO_SEMIJOIN,
    keyword_SUBQUERY,
    keyword_MATERIALIZATION,
    keyword_FIRSTMATCH,
    keyword_LOOSESCAN,
    keyword_DUPSWEEDOUT,
    keyword_INTOEXISTS,
    keyword_JOIN_PREFIX,
    keyword_JOIN_SUFFIX,
    keyword_JOIN_ORDER,
    keyword_JOIN_FIXED_ORDER,
    keyword_DERIVED_CONDITION_PUSHDOWN,
    keyword_NO_DERIVED_CONDITION_PUSHDOWN,
    keyword_MERGE,
    keyword_NO_MERGE,
    keyword_SPLIT_MATERIALIZED,
    keyword_NO_SPLIT_MATERIALIZED,
    keyword_INDEX,
    keyword_NO_INDEX,
    keyword_JOIN_INDEX,
    keyword_NO_JOIN_INDEX,
    keyword_GROUP_INDEX,
    keyword_NO_GROUP_INDEX,
    keyword_ORDER_INDEX,
    keyword_NO_ORDER_INDEX,
    keyword_ROWID_FILTER,
    keyword_NO_ROWID_FILTER
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

protected:
  Token get_token(CHARSET_INFO *cs);
  static TokenID find_keyword(const LEX_CSTRING &str);
};


class Printable_parser_rule
{
public:
  virtual void append_args(THD *thd, String *str) const = 0;
  virtual ~Printable_parser_rule() {}
};


class Optimizer_hint_parser: public Optimizer_hint_tokenizer,
                             public Parser_templates
{
private:
  Token m_look_ahead_token;
  THD *m_thd;
  const char *m_start;
  bool m_syntax_error;
  bool m_fatal_error;
public:
  Optimizer_hint_parser(THD *thd, CHARSET_INFO *cs, const LEX_CSTRING &hint)
   :Optimizer_hint_tokenizer(cs, hint),
    m_look_ahead_token(get_token(cs)),
    m_thd(thd),
    m_start(hint.str),
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

  bool parse_token_list(THD *thd); // For debug purposes

  void push_warning_syntax_error(THD *thd, uint lineno);


private:

  using Parser= Optimizer_hint_parser; // for a shorter notation

  // Rules consisting of a single token

  class TokenAT: public TOKEN<Parser, TokenID::tAT>
  {
  public:
    using TOKEN::TOKEN;
  };

  class TokenEOF: public TOKEN<Parser, TokenID::tEOF>
  {
  public:
    using TOKEN::TOKEN;
  };

  class Keyword_QB_NAME: public TOKEN<Parser, TokenID::keyword_QB_NAME>
  {
  public:
    using TOKEN::TOKEN;
  };

  class Keyword_MAX_EXECUTION_TIME:
      public TOKEN<Parser, TokenID::keyword_MAX_EXECUTION_TIME>
  {
  public:
    using TOKEN::TOKEN;
  };

  class Keyword_SUBQUERY: public TOKEN<Parser, TokenID::keyword_SUBQUERY>
  {
  public:
    using TOKEN::TOKEN;
  };

  class Identifier: public TOKEN<Parser, TokenID::tIDENT>
  {
  public:
    using TOKEN::TOKEN;
    Lex_ident_cli_st to_ident_cli() const
    {
      Lex_ident_cli_st cli;
      if (length >= 2 && (str[0] == '`' || str[0] == '"'))
        return cli.set_ident_quoted(str + 1, length - 2, true, str[0]);
      return cli.set_ident(str, length, true);
    }
    Lex_ident_sys to_ident_sys(THD *thd) const
    {
      const Lex_ident_cli_st cli= to_ident_cli();
      return Lex_ident_sys(thd, &cli);
    }
  };

  class Unsigned_Number: public TOKEN<Parser, TokenID::tUNSIGNED_NUMBER>
  {
  public:
    using TOKEN::TOKEN;

    /*
      Converts token string to a non-negative number ( >=0 ).
      Returns the converted number if the conversion succeeds.
      Returns non-NULL ULonglong_null value on successful string conversion and
      NULL ULonglong_null if the conversion failed or the number is negative
    */
    ULonglong_null get_ulonglong() const
    {
      int error;
      char *end= const_cast<char *>(str + length);
      longlong n= my_strtoll10(str, &end, &error);
      if (error != 0 || end != str + length || n < 0)
        return ULonglong_null(0, true);
      return ULonglong_null(n, false);
    }
  };

  class LParen: public TOKEN<Parser, TokenID::tLPAREN>
  {
  public:
    using TOKEN::TOKEN;
  };

  class RParen: public TOKEN<Parser, TokenID::tRPAREN>
  {
  public:
    using TOKEN::TOKEN;
  };


  // Rules consisting of multiple choices of tokens

  // table_level_hint_type ::= BKA | BNL | NO_BKA | NO_BNL
  class Table_level_hint_type_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::keyword_BKA ||
             id == TokenID::keyword_BNL ||
             id == TokenID::keyword_NO_BKA ||
             id == TokenID::keyword_NO_BNL ||
             id == TokenID::keyword_DERIVED_CONDITION_PUSHDOWN ||
             id == TokenID::keyword_NO_DERIVED_CONDITION_PUSHDOWN ||
             id == TokenID::keyword_MERGE ||
             id == TokenID::keyword_NO_MERGE ||
             id == TokenID::keyword_SPLIT_MATERIALIZED ||
             id == TokenID::keyword_NO_SPLIT_MATERIALIZED;
    }
  };
  class Table_level_hint_type: public TokenChoice<Parser,
                                                  Table_level_hint_type_cond>
  {
  public:
    using TokenChoice::TokenChoice;
  };


  /*
    index_level_hint_type ::= MRR | NO_RANGE_OPTIMIZATION | NO_ICP | NO_MRR |
      INDEX | NO_INDEX | JOIN_INDEX | NO_JOIN_INDEX | ORDER_INDEX |
      NO_ORDER_INDEX | GROUP_INDEX | NO_GROUP_INDEX
  */
  class Index_level_hint_type_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::keyword_MRR ||
             id == TokenID::keyword_NO_RANGE_OPTIMIZATION ||
             id == TokenID::keyword_NO_ICP ||
             id == TokenID::keyword_NO_MRR ||
             id == TokenID::keyword_INDEX ||
             id == TokenID::keyword_NO_INDEX ||
             id == TokenID::keyword_JOIN_INDEX ||
             id == TokenID::keyword_NO_JOIN_INDEX ||
             id == TokenID::keyword_ORDER_INDEX ||
             id == TokenID::keyword_NO_ORDER_INDEX ||
             id == TokenID::keyword_GROUP_INDEX ||
             id == TokenID::keyword_NO_GROUP_INDEX ||
             id == TokenID::keyword_ROWID_FILTER ||
             id == TokenID::keyword_NO_ROWID_FILTER;
    }
  };
  class Index_level_hint_type: public TokenChoice<Parser,
                                                  Index_level_hint_type_cond>
  {
  public:
    using TokenChoice::TokenChoice;
  };

  // Identifiers of various kinds

  // query_block_name ::= identifier
  class Query_block_name: public Identifier
  {
  public:
    using Identifier::Identifier;
  };

  // table_name ::= identifier
  class Table_name: public Identifier
  {
  public:
    using Identifier::Identifier;
  };

  // hint_param_index ::= identifier
  class Hint_param_index: public Identifier
  {
  public:
    using Identifier::Identifier;
  };


  // More complex rules

  /*
    at_query_block_name ::= @ query_block_name
  */
  class At_query_block_name: public AND2<Parser, TokenAT, Query_block_name>
  {
  public:
    using AND2::AND2;
    using AND2::operator=;
  };

  /*
    opt_qb_name ::=  [ @ query_block_name ]
  */
  class Opt_qb_name: public OPT<Parser, At_query_block_name>
  {
  public:
    using OPT::OPT;
  };

  /*
    hint_param_table ::= table_name opt_qb_name
  */
  class Hint_param_table: public AND2<Parser, Table_name, Opt_qb_name>
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
    bool add(Optimizer_hint_parser *p, Hint_param_table &&table);
    size_t count() const { return elements; }
  };

  class Opt_hint_param_table_list: public LIST<Parser,
                                               Hint_param_table_list_container,
                                               Hint_param_table,
                                               TokenID::tCOMMA, 0>
  {
    using LIST::LIST;
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
    bool add(Optimizer_hint_parser *p, Table_name &&table);
    size_t count() const { return elements; }
  };

  class Opt_table_name_list: public LIST<Parser,
                                         Table_name_list_container,
                                         Table_name, TokenID::tCOMMA, 0>
  {
  public:
    using LIST::LIST;
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
    bool add(Optimizer_hint_parser *p, Hint_param_index &&table);
    size_t count() const { return elements; }
  };

  class Opt_hint_param_index_list: public LIST<Parser,
                                               Hint_param_index_list_container,
                                               Hint_param_index,
                                               TokenID::tCOMMA, 0>
  {
  public:
    using LIST::LIST;
  };


  /*
    hint_param_table_ext ::=   hint_param_table
                             | @ query_block_name table_name
  */
  class At_query_block_name_table_name: public AND2<Parser,
                                                    At_query_block_name,
                                                    Table_name>
  {
  public:
    using AND2::AND2;
  };

  class Hint_param_table_ext_container: public Query_block_name,
                                        public Table_name
  {
  public:
    Hint_param_table_ext_container()
    { }
    Hint_param_table_ext_container(const Hint_param_table &hint_param_table)
     :Query_block_name(hint_param_table), Table_name(hint_param_table)
    { }
    Hint_param_table_ext_container(const At_query_block_name_table_name &qbt)
     :Query_block_name(qbt), Table_name(qbt)
    { }
    operator bool() const
    {
      return Query_block_name::operator bool() && Table_name::operator bool();
    }
  };

  class Hint_param_table_ext: public OR2C<Parser,
                                          Hint_param_table_ext_container,
                                          Hint_param_table,
                                          At_query_block_name_table_name>
  {
  public:
    using OR2C::OR2C;
  };


  /*
    at_query_block_name_opt_table_name_list ::=
      @ query_block_name opt_table_name_list
  */
  class At_query_block_name_opt_table_name_list: public AND2<
                                                    Parser,
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
  class Table_level_hint_body: public OR2<
                                        Parser,
                                        At_query_block_name_opt_table_name_list,
                                        Opt_hint_param_table_list>
  {
  public:
    using OR2::OR2;
  };

  // table_level_hint ::= table_level_hint_type ( table_level_hint_body )
  class Table_level_hint: public AND4<Parser,
                                      Table_level_hint_type,
                                      LParen,
                                      Table_level_hint_body,
                                      RParen>
  {
  public:
    using AND4::AND4;

    bool resolve(Parse_context *pc) const;
  };


  // index_level_hint_body ::= hint_param_table_ext opt_hint_param_index_list
  class Index_level_hint_body: public AND2<Parser,
                                           Hint_param_table_ext,
                                           Opt_hint_param_index_list>
  {
  public:
    using AND2::AND2;
  };

public:
  // index_level_hint ::= index_level_hint_type ( index_level_hint_body )
  class Index_level_hint: public AND4<Parser,
                                      Index_level_hint_type,
                                      LParen,
                                      Index_level_hint_body,
                                      RParen>,
                           public Printable_parser_rule
  {
  public:
    using AND4::AND4;

    /*
      If no index names are given, this is a table level hint, for example:
      GROUP_INDEX(t1), NO_MRR(t2).
      Otherwise this is an index-level hints:
      NO_INDEX(t1 idx1, idx2) NO_ICP(t2 idx_a, idx_b, idx_c)
    */
    bool is_table_level_hint() const { return is_empty(); }

    bool resolve(Parse_context *pc) const;
    void append_args(THD *thd, String *str) const override;
  };


  // qb_name_hint ::= QB_NAME ( query_block_name )
  class Qb_name_hint: public AND4<Parser,
                                  Keyword_QB_NAME,
                                  LParen,
                                  Query_block_name,
                                  RParen>
  {
  public:
    using AND4::AND4;

    bool resolve(Parse_context *pc) const;
  };


public:
  // max_execution_time_hint ::= MAX_EXECUTION_TIME ( milliseconds )
  class Max_execution_time_hint: public AND4<Parser,
                                  Keyword_MAX_EXECUTION_TIME,
                                  LParen,
                                  Unsigned_Number,
                                  RParen>,
                                public Printable_parser_rule
  {
  public:
    using AND4::AND4;

    bool resolve(Parse_context *pc) const;
    void append_args(THD *thd, String *str) const override;
    ulonglong get_milliseconds() const;
  };

private:
  // semijoin_hint_type ::= SEMIJOIN | NO_SEMIJOIN
  class Semijoin_hint_type_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::keyword_SEMIJOIN ||
             id == TokenID::keyword_NO_SEMIJOIN;
    }
  };
  class Semijoin_hint_type: public TokenChoice<Parser,
                                               Semijoin_hint_type_cond>
  {
  public:
    using TokenChoice::TokenChoice;
  };


  // semijoin_strategy ::= MATERIALIZATION | FIRSTMATCH | LOOSESCAN | DUPSWEEDOUT
  class Semijoin_strategy_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::keyword_MATERIALIZATION ||
             id == TokenID::keyword_FIRSTMATCH ||
             id == TokenID::keyword_LOOSESCAN ||
             id == TokenID::keyword_DUPSWEEDOUT;
    }
  };

  class Semijoin_strategy: public TokenChoice<Parser, Semijoin_strategy_cond>
  {
  public:
    using TokenChoice::TokenChoice;
  };


  /*
    strategy_list ::= strategy_name [ {, strategy_name }... ]
    opt_strategy_list ::= [ strategy_list ]
  */
  class Semijoin_strategy_list_container: public List<Semijoin_strategy>
  {
  public:
    Semijoin_strategy_list_container()
    { }
    bool add(Optimizer_hint_parser *p, Semijoin_strategy &&strategy);
    size_t count() const { return elements; }
  };

  class Opt_sj_strategy_list: public LIST<Parser,
                                       Semijoin_strategy_list_container,
                                       Semijoin_strategy, TokenID::tCOMMA, 0>
  {
  public:
    using LIST::LIST;
  };


  class Hint_param_opt_sj_strategy_list: public LIST<Parser,
                                       Semijoin_strategy_list_container,
                                       Semijoin_strategy, TokenID::tCOMMA, 0>
  {
  public:
    using LIST::LIST;
  };


  /*
    at_query_block_name_opt_strategies_list ::=
      @ query_block_name opt_strategies_list
  */
  class At_query_block_name_opt_strategy_list: public AND2<
                                                    Parser,
                                                    At_query_block_name,
                                                    Opt_sj_strategy_list>
  {
  public:
    using AND2::AND2;
  };

  /*
    semijoin_hint_body:   @ query_block_name opt_sj_strategy_list
                           | opt_sj_strategy_list
  */
  class Semijoin_hint_body: public OR2<Parser,
                                       At_query_block_name_opt_strategy_list,
                                       Hint_param_opt_sj_strategy_list>
  {
  public:
    using OR2::OR2;
  };

public:
  /*
    semijoin_hint ::= semijoin_hint_type ( semijoin_hint_body )
  */
  class Semijoin_hint: public AND4<Parser,
                                   Semijoin_hint_type,
                                   LParen,
                                   Semijoin_hint_body,
                                   RParen>,
                        public Printable_parser_rule
  {
  public:
    using AND4::AND4;

    bool resolve(Parse_context *pc) const;
    void append_args(THD *thd, String *str) const override;

  private:
    Opt_hints_qb* resolve_for_qb_name(Parse_context *pc, bool hint_state,
                                      const Lex_ident_sys *qb_name) const;
    void fill_strategies_map(Opt_hints_qb *qb) const;
    void add_strategy_to_map(TokenID token_id, Opt_hints_qb *qb) const;
    void append_strategy_name(TokenID token_id, String *str) const;
  };

private:
  // subquery_strategy ::= MATERIALIZATION | INTOEXISTS
  class Subquery_strategy_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::keyword_MATERIALIZATION ||
             id == TokenID::keyword_INTOEXISTS;
    }
  };

  class Subquery_strategy: public TokenChoice<Parser, Subquery_strategy_cond>
  {
  public:
    using TokenChoice::TokenChoice;
  };

  class Hint_param_subquery_strategy: public TokenChoice<Parser,
                                                         Subquery_strategy_cond>
  {
  public:
    using TokenChoice::TokenChoice;
  };


  /*
    at_query_block_name_subquery_strategy ::=
      @ query_block_name subquery_strategy
  */
  class At_query_block_name_subquery_strategy: public AND2<
                                                    Parser,
                                                    At_query_block_name,
                                                    Subquery_strategy>
  {
  public:
    using AND2::AND2;
  };


  /*
    subquery_hint_body:   @ query_block_name subquery_strategy
                           | subquery_strategy
  */
  class Subquery_hint_body: public OR2<Parser,
                                       At_query_block_name_subquery_strategy,
                                       Hint_param_subquery_strategy>
  {
  public:
    using OR2::OR2;
  };


public:
  // subquery_hint ::= SUBQUERY( subquery_hint_body )
  class Subquery_hint: public AND4<Parser,
                                  Keyword_SUBQUERY,
                                  LParen,
                                  Subquery_hint_body,
                                  RParen>,
                       public Printable_parser_rule
  {
  public:
    using AND4::AND4;

    bool resolve(Parse_context *pc) const;
    void append_args(THD *thd, String *str) const override;

  private:
    void set_subquery_strategy(TokenID token_id, Opt_hints_qb *qb) const;
    Opt_hints_qb* resolve_for_qb_name(Parse_context *pc, TokenID token_id,
                                     const Lex_ident_sys *qb_name) const;
  };


  /*
    join_order_hint_type ::= JOIN_FIXED_ORDER
                             | JOIN_ORDER
                             | JOIN_PREFIX
                             | JOIN_SUFFIX
  */
  class Join_order_hint_type_cond
  {
  public:
    static bool allowed_token_id(TokenID id)
    {
      return id == TokenID::keyword_JOIN_FIXED_ORDER ||
             id == TokenID::keyword_JOIN_ORDER ||
             id == TokenID::keyword_JOIN_PREFIX ||
             id == TokenID::keyword_JOIN_SUFFIX;
    }
  };
  class Join_order_hint_type: public TokenChoice<Parser,
                                                 Join_order_hint_type_cond>
  {
  public:
    using TokenChoice::TokenChoice;
  };

  /*
    Struct representing table names listed in optimizer hints bodies.
    They may optionally include query block names, for example:
    t1, t2@qb1, t3, t4@qb5
  */
  struct Table_name_and_Qb: public Sql_alloc
  {
    Lex_ident_sys table_name;
    Lex_ident_sys qb_name;      // may be empty

    Table_name_and_Qb(const Lex_ident_sys& tbl, const Lex_ident_sys& qb) :
      table_name(tbl), qb_name(qb)
    {}

    Table_name_and_Qb(Lex_ident_sys&& tbl, Lex_ident_sys&& qb) :
      table_name(std::move(tbl)), qb_name(std::move(qb))
    {}
  };

  /*
    join_order_hint ::= join_order_hint_type ( table_level_hint_body )
  */
  class Join_order_hint: public AND4<Parser,
                                   Join_order_hint_type,
                                   LParen,
                                   Table_level_hint_body,
                                   RParen>,
                         public Printable_parser_rule
  {
  public:
    using AND4::AND4;

    opt_hints_enum hint_type= MAX_HINT_ENUM;

    bool resolve(Parse_context *pc);

    void append_args(THD *thd, String *str) const override;

    /*
      Table names (optionally augmented with query block names) listed in
      the hint body.
    */
    List<Table_name_and_Qb> table_names;
  };


  /*
    hint ::=   index_level_hint
             | table_level_hint
             | qb_name_hint
             | max_execution_time_hint
             | semijoin_hint
             | subquery_hint
             | join_order_hint
  */
  class Hint: public OR7<Parser,
                         Index_level_hint,
                         Table_level_hint,
                         Qb_name_hint,
                         Max_execution_time_hint,
                         Semijoin_hint,
                         Subquery_hint,
                         Join_order_hint>
  {
  public:
    using OR7::OR7;
  };

private:
  // hint_list ::= hint [ hint... ]
  class Hint_list_container: public List<Hint>
  {
  public:
    Hint_list_container()
    { }
    bool add(Optimizer_hint_parser *p, Hint &&hint);
    size_t count() const { return elements; }
  };


  class Hint_list: public LIST<Parser, Hint_list_container,
                               Hint, TokenID::tNULL/*not separated list*/, 1>
  {
  public:
    using LIST::LIST;
    
    bool resolve(Parse_context *pc) const;
  };

public:
  /*
    The main rule:
      hints ::= hint_list EOF
  */
  class Hints: public AND2<Parser, Hint_list, TokenEOF>
  {
  public:
    using AND2::AND2;
  };
};


/*
  These wrapper class is needed to use a forward declarations in sql_lex.h
  instead of including the entire opt_hints_parser.h.
  (forward declarations of qualified nested classes are not possible in C++)
*/
class Optimizer_hint_parser_output: public Optimizer_hint_parser::Hints
{
public:
  using Hints::Hints;
};

#endif // OPT_HINTS_PARSER
