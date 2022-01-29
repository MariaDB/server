/*
   Copyright (c) 2009, 2022, MariaDB Corporation.

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

#ifndef SQL_YYSTYPE_INCLUDED
#define SQL_YYSTYPE_INCLUDED

#include <my_global.h>
#include <m_ctype.h>
#include "mysqld.h"
#include "lock.h"
#include "mdl.h"
#include "sql_signal.h"


/*
  The following hack is needed because yy_*.cc do not define
  YYSTYPE before including this file
*/
#ifdef MYSQL_YACC
#define LEX_YYSTYPE void *
#else
#include "lex_symbol.h"
#ifdef MYSQL_LEX
#include "item_func.h"            /* Cast_target used in yy_mariadb.hh */
#include "sql_get_diagnostics.h"  /* Types used in yy_mariadb.hh */
#include "sp_pcontext.h"
#include "yy_mariadb.hh"
#define LEX_YYSTYPE YYSTYPE *
#else
#define LEX_YYSTYPE void *
#endif
#endif

class Lex_string_with_metadata_st;
class Lex_ident_cli_st;
struct sql_digest_state;
/**
  The state of the lexical parser, when parsing comments.
*/
enum enum_comment_state
{
  /**
    Not parsing comments.
  */
  NO_COMMENT,
  /**
    Parsing comments that need to be preserved.
    Typically, these are user comments '/' '*' ... '*' '/'.
  */
  PRESERVE_COMMENT,
  /**
    Parsing comments that need to be discarded.
    Typically, these are special comments '/' '*' '!' ... '*' '/',
    or '/' '*' '!' 'M' 'M' 'm' 'm' 'm' ... '*' '/', where the comment
    markers should not be expanded.
  */
  DISCARD_COMMENT
};


/**
  @brief This class represents the character input stream consumed during
  lexical analysis.

  In addition to consuming the input stream, this class performs some
  comment pre processing, by filtering out out of bound special text
  from the query input stream.
  Two buffers, with pointers inside each buffers, are maintained in
  parallel. The 'raw' buffer is the original query text, which may
  contain out-of-bound comments. The 'cpp' (for comments pre processor)
  is the pre-processed buffer that contains only the query text that
  should be seen once out-of-bound data is removed.
*/

class Lex_input_stream
{
  size_t unescape(CHARSET_INFO *cs, char *to,
                  const char *str, const char *end, int sep);
  my_charset_conv_wc_mb get_escape_func(THD *thd, my_wc_t sep) const;
public:
  Lex_input_stream()
  {
  }

  ~Lex_input_stream()
  {
  }

  /**
     Object initializer. Must be called before usage.

     @retval FALSE OK
     @retval TRUE  Error
  */
  bool init(THD *thd, char *buff, size_t length);

  void reset(char *buff, size_t length);

  /**
    The main method to scan the next token, with token contraction processing
    for LALR(2) resolution, e.g. translate "WITH" followed by "ROLLUP"
    to a single token WITH_ROLLUP_SYM.
  */
  int lex_token(union YYSTYPE *yylval, THD *thd);

  void reduce_digest_token(uint token_left, uint token_right);

private:
  /**
    Set the echo mode.

    When echo is true, characters parsed from the raw input stream are
    preserved. When false, characters parsed are silently ignored.
    @param echo the echo mode.
  */
  void set_echo(bool echo)
  {
    m_echo= echo;
  }

  void save_in_comment_state()
  {
    m_echo_saved= m_echo;
    in_comment_saved= in_comment;
  }

  void restore_in_comment_state()
  {
    m_echo= m_echo_saved;
    in_comment= in_comment_saved;
  }

  /**
    Skip binary from the input stream.
    @param n number of bytes to accept.
  */
  void skip_binary(int n)
  {
    if (m_echo)
    {
      memcpy(m_cpp_ptr, m_ptr, n);
      m_cpp_ptr += n;
    }
    m_ptr += n;
  }

  /**
    Get a character, and advance in the stream.
    @return the next character to parse.
  */
  unsigned char yyGet()
  {
    char c= *m_ptr++;
    if (m_echo)
      *m_cpp_ptr++ = c;
    return c;
  }

  /**
    Get the last character accepted.
    @return the last character accepted.
  */
  unsigned char yyGetLast()
  {
    return m_ptr[-1];
  }

  /**
    Look at the next character to parse, but do not accept it.
  */
  unsigned char yyPeek()
  {
    return m_ptr[0];
  }

  /**
    Look ahead at some character to parse.
    @param n offset of the character to look up
  */
  unsigned char yyPeekn(int n)
  {
    return m_ptr[n];
  }

  /**
    Cancel the effect of the last yyGet() or yySkip().
    Note that the echo mode should not change between calls to yyGet / yySkip
    and yyUnget. The caller is responsible for ensuring that.
  */
  void yyUnget()
  {
    m_ptr--;
    if (m_echo)
      m_cpp_ptr--;
  }

  /**
    Accept a character, by advancing the input stream.
  */
  void yySkip()
  {
    if (m_echo)
      *m_cpp_ptr++ = *m_ptr++;
    else
      m_ptr++;
  }

  /**
    Accept multiple characters at once.
    @param n the number of characters to accept.
  */
  void yySkipn(int n)
  {
    if (m_echo)
    {
      memcpy(m_cpp_ptr, m_ptr, n);
      m_cpp_ptr += n;
    }
    m_ptr += n;
  }

  /**
    Puts a character back into the stream, canceling
    the effect of the last yyGet() or yySkip().
    Note that the echo mode should not change between calls
    to unput, get, or skip from the stream.
  */
  char *yyUnput(char ch)
  {
    *--m_ptr= ch;
    if (m_echo)
      m_cpp_ptr--;
    return m_ptr;
  }

  /**
    End of file indicator for the query text to parse.
    @param n number of characters expected
    @return true if there are less than n characters to parse
  */
  bool eof(int n)
  {
    return ((m_ptr + n) >= m_end_of_query);
  }

  /** Mark the stream position as the start of a new token. */
  void start_token()
  {
    m_tok_start_prev= m_tok_start;
    m_tok_start= m_ptr;
    m_tok_end= m_ptr;

    m_cpp_tok_start_prev= m_cpp_tok_start;
    m_cpp_tok_start= m_cpp_ptr;
    m_cpp_tok_end= m_cpp_ptr;
  }

  /**
    Adjust the starting position of the current token.
    This is used to compensate for starting whitespace.
  */
  void restart_token()
  {
    m_tok_start= m_ptr;
    m_cpp_tok_start= m_cpp_ptr;
  }

  /**
    Get the maximum length of the utf8-body buffer.
    The utf8 body can grow because of the character set conversion and escaping.
  */
  size_t get_body_utf8_maximum_length(THD *thd);

  /** Get the length of the current token, in the raw buffer. */
  uint yyLength()
  {
    /*
      The assumption is that the lexical analyser is always 1 character ahead,
      which the -1 account for.
    */
    DBUG_ASSERT(m_ptr > m_tok_start);
    return (uint) ((m_ptr - m_tok_start) - 1);
  }

  /**
    Test if a lookahead token was already scanned by lex_token(),
    for LALR(2) resolution.
  */
  bool has_lookahead() const
  {
    return lookahead_token >= 0;
  }

public:

  /**
    End of file indicator for the query text to parse.
    @return true if there are no more characters to parse
  */
  bool eof()
  {
    return (m_ptr >= m_end_of_query);
  }

  /** Get the raw query buffer. */
  const char *get_buf()
  {
    return m_buf;
  }

  /** Get the pre-processed query buffer. */
  const char *get_cpp_buf()
  {
    return m_cpp_buf;
  }

  /** Get the end of the raw query buffer. */
  const char *get_end_of_query()
  {
    return m_end_of_query;
  }

  /** Get the token start position, in the raw buffer. */
  const char *get_tok_start()
  {
    return has_lookahead() ? m_tok_start_prev : m_tok_start;
  }

  void set_cpp_tok_start(const char *pos)
  {
    m_cpp_tok_start= pos;
  }

  /** Get the token end position, in the raw buffer. */
  const char *get_tok_end()
  {
    return m_tok_end;
  }

  /** Get the current stream pointer, in the raw buffer. */
  const char *get_ptr()
  {
    return m_ptr;
  }

  /** Get the token start position, in the pre-processed buffer. */
  const char *get_cpp_tok_start()
  {
    return has_lookahead() ? m_cpp_tok_start_prev : m_cpp_tok_start;
  }

  /** Get the token end position, in the pre-processed buffer. */
  const char *get_cpp_tok_end()
  {
    return m_cpp_tok_end;
  }

  /**
    Get the token end position in the pre-processed buffer,
    with trailing spaces removed.
  */
  const char *get_cpp_tok_end_rtrim()
  {
    const char *p;
    for (p= m_cpp_tok_end;
         p > m_cpp_buf && my_isspace(system_charset_info, p[-1]);
         p--)
    { }
    return p;
  }

  /** Get the current stream pointer, in the pre-processed buffer. */
  const char *get_cpp_ptr()
  {
    return m_cpp_ptr;
  }

  /**
    Get the current stream pointer, in the pre-processed buffer,
    with traling spaces removed.
  */
  const char *get_cpp_ptr_rtrim()
  {
    const char *p;
    for (p= m_cpp_ptr;
         p > m_cpp_buf && my_isspace(system_charset_info, p[-1]);
         p--)
    { }
    return p;
  }
  /** Get the utf8-body string. */
  const char *get_body_utf8_str()
  {
    return m_body_utf8;
  }

  /** Get the utf8-body length. */
  size_t get_body_utf8_length()
  {
    return (size_t) (m_body_utf8_ptr - m_body_utf8);
  }

  void body_utf8_start(THD *thd, const char *begin_ptr);
  void body_utf8_append(const char *ptr);
  void body_utf8_append(const char *ptr, const char *end_ptr);
  void body_utf8_append_ident(THD *thd,
                              const Lex_string_with_metadata_st *txt,
                              const char *end_ptr);
  void body_utf8_append_escape(THD *thd,
                               const LEX_CSTRING *txt,
                               CHARSET_INFO *txt_cs,
                               const char *end_ptr,
                               my_wc_t sep);

private:
  /**
    LALR(2) resolution, look ahead token.
    Value of the next token to return, if any,
    or -1, if no token was parsed in advance.
    Note: 0 is a legal token, and represents YYEOF.
  */
  int lookahead_token;

  /** LALR(2) resolution, value of the look ahead token.*/
  LEX_YYSTYPE lookahead_yylval;

  bool get_text(Lex_string_with_metadata_st *to,
                uint sep, int pre_skip, int post_skip);

  void add_digest_token(uint token, LEX_YYSTYPE yylval);

  bool consume_comment(int remaining_recursions_permitted);
  int lex_one_token(union YYSTYPE *yylval, THD *thd);
  int find_keyword(Lex_ident_cli_st *str, uint len, bool function);
  LEX_CSTRING get_token(uint skip, uint length);
  int scan_ident_sysvar(THD *thd, Lex_ident_cli_st *str);
  int scan_ident_start(THD *thd, Lex_ident_cli_st *str);
  int scan_ident_middle(THD *thd, Lex_ident_cli_st *str,
                        CHARSET_INFO **cs, my_lex_states *);
  int scan_ident_delimited(THD *thd, Lex_ident_cli_st *str, uchar quote_char);
  bool get_7bit_or_8bit_ident(THD *thd, uchar *last_char);

  /** Current thread. */
  THD *m_thd;

  /** Pointer to the current position in the raw input stream. */
  char *m_ptr;

  /** Starting position of the last token parsed, in the raw buffer. */
  const char *m_tok_start;

  /** Ending position of the previous token parsed, in the raw buffer. */
  const char *m_tok_end;

  /** End of the query text in the input stream, in the raw buffer. */
  const char *m_end_of_query;

  /** Starting position of the previous token parsed, in the raw buffer. */
  const char *m_tok_start_prev;

  /** Begining of the query text in the input stream, in the raw buffer. */
  const char *m_buf;

  /** Length of the raw buffer. */
  size_t m_buf_length;

  /** Echo the parsed stream to the pre-processed buffer. */
  bool m_echo:1;
  bool m_echo_saved:1;

  /** Pre-processed buffer. */
  char *m_cpp_buf;

  /** Pointer to the current position in the pre-processed input stream. */
  char *m_cpp_ptr;

  /**
    Starting position of the last token parsed,
    in the pre-processed buffer.
  */
  const char *m_cpp_tok_start;

  /**
    Starting position of the previous token parsed,
    in the pre-procedded buffer.
  */
  const char *m_cpp_tok_start_prev;

  /**
    Ending position of the previous token parsed,
    in the pre-processed buffer.
  */
  const char *m_cpp_tok_end;

  /** UTF8-body buffer created during parsing. */
  char *m_body_utf8;

  /** Pointer to the current position in the UTF8-body buffer. */
  char *m_body_utf8_ptr;

  /**
    Position in the pre-processed buffer. The query from m_cpp_buf to
    m_cpp_utf_processed_ptr is converted to UTF8-body.
  */
  const char *m_cpp_utf8_processed_ptr;

public:

  /** Current state of the lexical analyser. */
  enum my_lex_states next_state;

  /**
    Position of ';' in the stream, to delimit multiple queries.
    This delimiter is in the raw buffer.
  */
  const char *found_semicolon;

  /** SQL_MODE = IGNORE_SPACE. */
  bool ignore_space:1;

  /**
    TRUE if we're parsing a prepared statement: in this mode
    we should allow placeholders.
  */
  bool stmt_prepare_mode:1;
  /**
    TRUE if we should allow multi-statements.
  */
  bool multi_statements:1;

  /** Current line number. */
  uint yylineno;

  /**
    Current statement digest instrumentation.
  */
  sql_digest_state* m_digest;

private:
  /** State of the lexical analyser for comments. */
  enum_comment_state in_comment;
  enum_comment_state in_comment_saved;

  /**
    Starting position of the TEXT_STRING or IDENT in the pre-processed
    buffer.

    NOTE: this member must be used within MYSQLlex() function only.
  */
  const char *m_cpp_text_start;

  /**
    Ending position of the TEXT_STRING or IDENT in the pre-processed
    buffer.

    NOTE: this member must be used within MYSQLlex() function only.
    */
  const char *m_cpp_text_end;

  /**
    Character set specified by the character-set-introducer.

    NOTE: this member must be used within MYSQLlex() function only.
  */
  CHARSET_INFO *m_underscore_cs;
};



/**
  The internal state of the syntax parser.
  This object is only available during parsing,
  and is private to the syntax parser implementation (sql_yacc.yy).
*/
class Yacc_state
{
public:
  Yacc_state() : yacc_yyss(NULL), yacc_yyvs(NULL) { reset(); }

  void reset()
  {
    if (yacc_yyss != NULL) {
      my_free(yacc_yyss);
      yacc_yyss = NULL;
    }
    if (yacc_yyvs != NULL) {
      my_free(yacc_yyvs);
      yacc_yyvs = NULL;
    }
    m_set_signal_info.clear();
    m_lock_type= TL_READ_DEFAULT;
    m_mdl_type= MDL_SHARED_READ;
  }

  ~Yacc_state();

  /**
    Reset part of the state which needs resetting before parsing
    substatement.
  */
  void reset_before_substatement()
  {
    m_lock_type= TL_READ_DEFAULT;
    m_mdl_type= MDL_SHARED_READ;
  }

  /**
    Bison internal state stack, yyss, when dynamically allocated using
    my_yyoverflow().
  */
  uchar *yacc_yyss;

  /**
    Bison internal semantic value stack, yyvs, when dynamically allocated using
    my_yyoverflow().
  */
  uchar *yacc_yyvs;

  /**
    Fragments of parsed tree,
    used during the parsing of SIGNAL and RESIGNAL.
  */
  Set_signal_information m_set_signal_info;

  /**
    Type of lock to be used for tables being added to the statement's
    table list in table_factor, table_alias_ref, single_multi and
    table_wild_one rules.
    Statements which use these rules but require lock type different
    from one specified by this member have to override it by using
    st_select_lex::set_lock_for_tables() method.

    The default value of this member is TL_READ_DEFAULT. The only two
    cases in which we change it are:
    - When parsing SELECT HIGH_PRIORITY.
    - Rule for DELETE. In which we use this member to pass information
      about type of lock from delete to single_multi part of rule.

    We should try to avoid introducing new use cases as we would like
    to get rid of this member eventually.
  */
  thr_lock_type m_lock_type;

  /**
    The type of requested metadata lock for tables added to
    the statement table list.
  */
  enum_mdl_type m_mdl_type;

  /*
    TODO: move more attributes from the LEX structure here.
  */
};


/**
  Internal state of the parser.
  The complete state consist of:
  - state data used during lexical parsing,
  - state data used during syntactic parsing.
*/
class Parser_state
{
public:
  Parser_state()
      : m_yacc()
  {}

  /**
     Object initializer. Must be called before usage.

     @retval FALSE OK
     @retval TRUE  Error
  */
  bool init(THD *thd, char *buff, size_t length)
  {
    return m_lip.init(thd, buff, length);
  }

  ~Parser_state()
  {}

  Lex_input_stream m_lip;
  Yacc_state m_yacc;

  /**
    Current performance digest instrumentation.
  */
  PSI_digest_locker* m_digest_psi;

  void reset(char *found_semicolon, unsigned int length)
  {
    m_lip.reset(found_semicolon, length);
    m_yacc.reset();
  }
};


extern sql_digest_state *
digest_add_token(sql_digest_state *state, uint token, LEX_YYSTYPE yylval);

extern sql_digest_state *
digest_reduce_token(sql_digest_state *state, uint token_left, uint token_right);

#endif // SQL_YYSTYPE_INCLUDED
