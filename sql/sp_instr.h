#ifndef _SP_INSTR_H_
#define _SP_INSTR_H_

#include "mariadb.h"

#include "sql_alloc.h"    // Sql_alloc
#include "sql_class.h"    // THD, Query_arena
#include "sql_lex.h"      // class sp_lex_local
#include "sp_pcontext.h"  // class sp_pcontext

/*
  Sufficient max length of printed destinations and frame offsets (all uints).
*/
static const int SP_INSTR_UINT_MAXLEN= 8;


class sp_lex_cursor: public sp_lex_local, public Query_arena
{
public:
  sp_lex_cursor(THD *thd, const LEX *oldlex, MEM_ROOT *mem_root_arg)
   :sp_lex_local(thd, oldlex),
    Query_arena(mem_root_arg, STMT_INITIALIZED_FOR_SP)
  { }
  sp_lex_cursor(THD *thd, const LEX *oldlex);
  ~sp_lex_cursor()
  {
    free_items();
  }
  virtual bool cleanup_stmt(bool /*restore_set_statement_vars*/) override
  { return false; }
  Query_arena *query_arena() override { return this; }
  bool validate()
  {
    DBUG_ASSERT(sql_command == SQLCOM_SELECT);
    if (result)
    {
      my_error(ER_SP_BAD_CURSOR_SELECT, MYF(0));
      return true;
    }
    return false;
  }
  bool stmt_finalize(THD *thd)
  {
    if (validate())
      return true;
    sp_lex_in_use= true;
    free_list= thd->free_list;
    thd->free_list= nullptr;
    return false;
  }
};


//
// "Instructions"...
//

class sp_instr :public Query_arena, public Sql_alloc
{
  sp_instr(const sp_instr &);   /**< Prevent use of these */
  void operator=(sp_instr &);

public:

  uint marked;
  uint m_ip;                    ///< My index
  sp_pcontext *m_ctx;           ///< My parse context
  uint m_lineno;

  /// Should give each a name or type code for debugging purposes?
  sp_instr(uint ip, sp_pcontext *ctx)
    :Query_arena(0, STMT_INITIALIZED_FOR_SP), marked(0), m_ip(ip), m_ctx(ctx),
     m_lineno(0)
  {}

  virtual ~sp_instr()
  {
    free_items();
  }


  /**
    Execute this instruction


    @param thd         Thread handle
    @param[out] nextp  index of the next instruction to execute. (For most
                       instructions this will be the instruction following this
                       one). Note that this parameter is undefined in case of
                       errors, use get_cont_dest() to find the continuation
                       instruction for CONTINUE error handlers.

    @retval 0      on success,
    @retval other  if some error occurred
  */

  virtual int execute(THD *thd, uint *nextp) = 0;

  /**
    Execute <code>open_and_lock_tables()</code> for this statement.
    Open and lock the tables used by this statement, as a pre-requisite
    to execute the core logic of this instruction with
    <code>exec_core()</code>.
    @param thd the current thread
    @param tables the list of tables to open and lock
    @return zero on success, non zero on failure.
  */
  int exec_open_and_lock_tables(THD *thd, TABLE_LIST *tables);

  /**
    Get the continuation destination of this instruction.
    @return the continuation destination
  */
  virtual uint get_cont_dest() const;

  /*
    Execute core function of instruction after all preparations (e.g.
    setting of proper LEX, saving part of the thread context have been
    done).

    Should be implemented for instructions using expressions or whole
    statements (thus having to have own LEX). Used in concert with
    sp_lex_keeper class and its descendants (there are none currently).
  */
  virtual int exec_core(THD *thd, uint *nextp);

  virtual void print(String *str) = 0;

  virtual void backpatch(uint dest, sp_pcontext *dst_ctx)
  {}

  /**
    Mark this instruction as reachable during optimization and return the
    index to the next instruction. Jump instruction will add their
    destination to the leads list.
  */
  virtual uint opt_mark(sp_head *sp, List<sp_instr> *leads)
  {
    marked= 1;
    return m_ip + 1;
  }

  /**
    Short-cut jumps to jumps during optimization. This is used by the
    jump instructions' opt_mark() methods. 'start' is the starting point,
    used to prevent the mark sweep from looping for ever. Return the
    end destination.
  */
  virtual uint opt_shortcut_jump(sp_head *sp, sp_instr *start)
  {
    return m_ip;
  }

  /**
    Inform the instruction that it has been moved during optimization.
    Most instructions will simply update its index, but jump instructions
    must also take care of their destination pointers. Forward jumps get
    pushed to the backpatch list 'ibp'.
  */
  virtual void opt_move(uint dst, List<sp_instr> *ibp)
  {
    m_ip= dst;
  }
  virtual PSI_statement_info* get_psi_info() = 0;

}; // class sp_instr : public Sql_alloc


/**
  Auxilary class to which instructions delegate responsibility
  for handling LEX and preparations before executing statement
  or calculating complex expression.

  Exist mainly to avoid having double hierarchy between instruction
  classes.

  @todo
    Add ability to not store LEX and do any preparations if
    expression used is simple.
*/

class sp_lex_keeper final
{
  /** Prevent use of these */
  sp_lex_keeper(const sp_lex_keeper &);
  void operator=(sp_lex_keeper &);
public:

  sp_lex_keeper(LEX *lex, bool lex_resp)
    : m_lex(lex), m_lex_resp(lex_resp),
      prelocking_tables(nullptr),
      lex_query_tables_own_last(nullptr)
  {
    lex->sp_lex_in_use= true;
  }
  virtual ~sp_lex_keeper()
  {
    if (m_lex_resp)
    {
      /* Prevent endless recursion. */
      m_lex->sphead= nullptr;
      lex_end(m_lex);
      delete m_lex;
    }
  }

  /**
    Prepare execution of instruction using LEX, if requested check whenever
    we have read access to tables used and open/lock them, call instruction's
    exec_core() method, perform cleanup afterwards.

    @todo Conflicting comment in sp_head.cc
  */
  int reset_lex_and_exec_core(THD *thd, uint *nextp, bool open_tables,
                              sp_instr* instr);

  int cursor_reset_lex_and_exec_core(THD *thd, uint *nextp, bool open_tables,
                                     sp_instr *instr);

  inline uint sql_command() const
  {
    return (uint)m_lex->sql_command;
  }

  void disable_query_cache()
  {
    m_lex->safe_to_cache_query= 0;
  }

private:

  LEX *m_lex;
  /**
    Indicates whenever this sp_lex_keeper instance responsible
    for LEX deletion.
  */
  bool m_lex_resp;

  /*
    Support for being able to execute this statement in two modes:
    a) inside prelocked mode set by the calling procedure or its ancestor.
    b) outside of prelocked mode, when this statement enters/leaves
       prelocked mode itself.
  */

  /**
    List of additional tables this statement needs to lock when it
    enters/leaves prelocked mode on its own.
  */
  TABLE_LIST *prelocking_tables;

  /**
    The value m_lex->query_tables_own_last should be set to this when the
    statement enters/leaves prelocked mode on its own.
  */
  TABLE_LIST **lex_query_tables_own_last;
};


/**
  Call out to some prepared SQL statement.
*/
class sp_instr_stmt : public sp_instr
{
  sp_instr_stmt(const sp_instr_stmt &); /**< Prevent use of these */
  void operator=(sp_instr_stmt &);

public:

  LEX_STRING m_query;           ///< For thd->query

  sp_instr_stmt(uint ip, sp_pcontext *ctx, LEX *lex)
    : sp_instr(ip, ctx), m_lex_keeper(lex, true)
  {
    m_query.str= 0;
    m_query.length= 0;
  }

  virtual ~sp_instr_stmt()
  {};

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

private:

  sp_lex_keeper m_lex_keeper;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;

}; // class sp_instr_stmt : public sp_instr


class sp_instr_set : public sp_instr
{
  sp_instr_set(const sp_instr_set &);   /**< Prevent use of these */
  void operator=(sp_instr_set &);

public:

  sp_instr_set(uint ip, sp_pcontext *ctx,
               const Sp_rcontext_handler *rh,
               uint offset, Item *val,
               LEX *lex, bool lex_resp)
    : sp_instr(ip, ctx),
      m_rcontext_handler(rh), m_offset(offset), m_value(val),
      m_lex_keeper(lex, lex_resp)
  {}

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

protected:
  sp_rcontext *get_rcontext(THD *thd) const;
  const Sp_rcontext_handler *m_rcontext_handler;
  uint m_offset;                ///< Frame offset
  Item *m_value;
  sp_lex_keeper m_lex_keeper;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_set : public sp_instr


/*
  This class handles assignments of a ROW fields:
    DECLARE rec ROW (a INT,b INT);
    SET rec.a= 10;
*/
class sp_instr_set_row_field : public sp_instr_set
{
  sp_instr_set_row_field(const sp_instr_set_row_field &); // Prevent use of this
  void operator=(sp_instr_set_row_field &);
  uint m_field_offset;

public:

  sp_instr_set_row_field(uint ip, sp_pcontext *ctx,
                         const Sp_rcontext_handler *rh,
                         uint offset, uint field_offset,
                         Item *val,
                         LEX *lex, bool lex_resp)
    : sp_instr_set(ip, ctx, rh, offset, val, lex, lex_resp),
      m_field_offset(field_offset)
  {}

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;
}; // class sp_instr_set_field : public sp_instr_set


/**
  This class handles assignment instructions like this:
  DECLARE
    CURSOR cur IS SELECT * FROM t1;
    rec cur%ROWTYPE;
  BEGIN
    rec.column1:= 10; -- This instruction
  END;

  The idea is that during sp_rcontext::create() we do not know the extact
  structure of "rec". It gets resolved at run time, during the corresponding
  sp_instr_cursor_copy_struct::exec_core().

  So sp_instr_set_row_field_by_name searches for ROW fields by name,
  while sp_instr_set_row_field (see above) searches for ROW fields by index.
*/
class sp_instr_set_row_field_by_name : public sp_instr_set
{
  // Prevent use of this
  sp_instr_set_row_field_by_name(const sp_instr_set_row_field &);
  void operator=(sp_instr_set_row_field_by_name &);
  const LEX_CSTRING m_field_name;

public:

  sp_instr_set_row_field_by_name(uint ip, sp_pcontext *ctx,
                                 const Sp_rcontext_handler *rh,
                                 uint offset, const LEX_CSTRING &field_name,
                                 Item *val,
                                 LEX *lex, bool lex_resp)
    : sp_instr_set(ip, ctx, rh, offset, val, lex, lex_resp),
      m_field_name(field_name)
  {}

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;
}; // class sp_instr_set_field_by_name : public sp_instr_set


/**
  Set NEW/OLD row field value instruction. Used in triggers.
*/
class sp_instr_set_trigger_field : public sp_instr
{
  sp_instr_set_trigger_field(const sp_instr_set_trigger_field &);
  void operator=(sp_instr_set_trigger_field &);

public:

  sp_instr_set_trigger_field(uint ip, sp_pcontext *ctx,
                             Item_trigger_field *trg_fld,
                             Item *val, LEX *lex)
    : sp_instr(ip, ctx),
      trigger_field(trg_fld),
      value(val), m_lex_keeper(lex, true)
  {}

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

private:
  Item_trigger_field *trigger_field;
  Item *value;
  sp_lex_keeper m_lex_keeper;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_trigger_field : public sp_instr


/**
  An abstract class for all instructions with destinations that
  needs to be updated by the optimizer.

  Even if not all subclasses will use both the normal destination and
  the continuation destination, we put them both here for simplicity.
*/
class sp_instr_opt_meta : public sp_instr
{
public:

  uint m_dest;                  ///< Where we will go
  uint m_cont_dest;             ///< Where continue handlers will go

  sp_instr_opt_meta(uint ip, sp_pcontext *ctx)
    : sp_instr(ip, ctx),
      m_dest(0), m_cont_dest(0), m_optdest(0), m_cont_optdest(0)
  {}

  sp_instr_opt_meta(uint ip, sp_pcontext *ctx, uint dest)
    : sp_instr(ip, ctx),
      m_dest(dest), m_cont_dest(0), m_optdest(0), m_cont_optdest(0)
  {}

  virtual ~sp_instr_opt_meta()
  {}

  virtual void set_destination(uint old_dest, uint new_dest)
    = 0;

  uint get_cont_dest() const override;

protected:

  sp_instr *m_optdest;          ///< Used during optimization
  sp_instr *m_cont_optdest;     ///< Used during optimization

}; // class sp_instr_opt_meta : public sp_instr


class sp_instr_jump : public sp_instr_opt_meta
{
  sp_instr_jump(const sp_instr_jump &); /**< Prevent use of these */
  void operator=(sp_instr_jump &);

public:

  sp_instr_jump(uint ip, sp_pcontext *ctx)
    : sp_instr_opt_meta(ip, ctx)
  {}

  sp_instr_jump(uint ip, sp_pcontext *ctx, uint dest)
    : sp_instr_opt_meta(ip, ctx, dest)
  {}

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override;

  uint opt_shortcut_jump(sp_head *sp, sp_instr *start) override;

  void opt_move(uint dst, List<sp_instr> *ibp) override;

  void backpatch(uint dest, sp_pcontext *dst_ctx) override
  {
    /* Calling backpatch twice is a logic flaw in jump resolution. */
    DBUG_ASSERT(m_dest == 0);
    m_dest= dest;
  }

  /**
    Update the destination; used by the optimizer.
  */
  void set_destination(uint old_dest, uint new_dest) override
  {
    if (m_dest == old_dest)
      m_dest= new_dest;
  }

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_jump : public sp_instr_opt_meta


class sp_instr_jump_if_not : public sp_instr_jump
{
  sp_instr_jump_if_not(const sp_instr_jump_if_not &); /**< Prevent use of these */
  void operator=(sp_instr_jump_if_not &);

public:

  sp_instr_jump_if_not(uint ip, sp_pcontext *ctx, Item *i, LEX *lex)
    : sp_instr_jump(ip, ctx), m_expr(i),
      m_lex_keeper(lex, true)
  {}

  sp_instr_jump_if_not(uint ip, sp_pcontext *ctx, Item *i, uint dest, LEX *lex)
    : sp_instr_jump(ip, ctx, dest), m_expr(i),
      m_lex_keeper(lex, true)
  {}

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override;

  /** Override sp_instr_jump's shortcut; we stop here */
  uint opt_shortcut_jump(sp_head *sp, sp_instr *start) override
  {
    return m_ip;
  }

  void opt_move(uint dst, List<sp_instr> *ibp) override;

  void set_destination(uint old_dest, uint new_dest) override
  {
    sp_instr_jump::set_destination(old_dest, new_dest);
    if (m_cont_dest == old_dest)
      m_cont_dest= new_dest;
  }

private:

  Item *m_expr;                 ///< The condition
  sp_lex_keeper m_lex_keeper;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_jump_if_not : public sp_instr_jump


class sp_instr_preturn : public sp_instr
{
  sp_instr_preturn(const sp_instr_preturn &);   /**< Prevent use of these */
  void operator=(sp_instr_preturn &);

public:

  sp_instr_preturn(uint ip, sp_pcontext *ctx)
    : sp_instr(ip, ctx)
  {}

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override
  {
    marked= 1;
    return UINT_MAX;
  }

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_preturn : public sp_instr


class sp_instr_freturn : public sp_instr
{
  sp_instr_freturn(const sp_instr_freturn &);   /**< Prevent use of these */
  void operator=(sp_instr_freturn &);

public:

  sp_instr_freturn(uint ip, sp_pcontext *ctx,
                   Item *val, const Type_handler *handler, LEX *lex)
    : sp_instr(ip, ctx), m_value(val), m_type_handler(handler),
      m_lex_keeper(lex, true)
  {}

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override
  {
    marked= 1;
    return UINT_MAX;
  }

protected:

  Item *m_value;
  const Type_handler *m_type_handler;
  sp_lex_keeper m_lex_keeper;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_freturn : public sp_instr


class sp_instr_hpush_jump : public sp_instr_jump
{
  sp_instr_hpush_jump(const sp_instr_hpush_jump &); /**< Prevent use of these */
  void operator=(sp_instr_hpush_jump &);

public:

  sp_instr_hpush_jump(uint ip,
                      sp_pcontext *ctx,
                      sp_handler *handler)
   :sp_instr_jump(ip, ctx),
    m_handler(handler),
    m_opt_hpop(0),
    m_frame(ctx->current_var_count())
  {
    DBUG_ASSERT(m_handler->condition_values.elements == 0);
  }

  virtual ~sp_instr_hpush_jump()
  {
    m_handler->condition_values.empty();
    m_handler= nullptr;
  }

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override;

  /** Override sp_instr_jump's shortcut; we stop here. */
  uint opt_shortcut_jump(sp_head *sp, sp_instr *start) override
  {
    return m_ip;
  }

  void backpatch(uint dest, sp_pcontext *dst_ctx) override
  {
    DBUG_ASSERT(!m_dest || !m_opt_hpop);
    if (!m_dest)
      m_dest= dest;
    else
      m_opt_hpop= dest;
  }

  void add_condition(sp_condition_value *condition_value)
  { m_handler->condition_values.push_back(condition_value); }

  sp_handler *get_handler()
  { return m_handler; }

private:
  /// Handler.
  sp_handler *m_handler;

  /// hpop marking end of handler scope.
  uint m_opt_hpop;

  // This attribute is needed for SHOW PROCEDURE CODE only (i.e. it's needed in
  // debug version only). It's used in print().
  uint m_frame;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_hpush_jump : public sp_instr_jump


class sp_instr_hpop : public sp_instr
{
  sp_instr_hpop(const sp_instr_hpop &); /**< Prevent use of these */
  void operator=(sp_instr_hpop &);

public:

  sp_instr_hpop(uint ip, sp_pcontext *ctx, uint count)
    : sp_instr(ip, ctx), m_count(count)
  {}

  void update_count(uint count)
  {
    m_count= count;
  }

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

private:

  uint m_count;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_hpop : public sp_instr


class sp_instr_hreturn : public sp_instr_jump
{
  sp_instr_hreturn(const sp_instr_hreturn &);   /**< Prevent use of these */
  void operator=(sp_instr_hreturn &);

public:

  sp_instr_hreturn(uint ip, sp_pcontext *ctx)
   :sp_instr_jump(ip, ctx),
    m_frame(ctx->current_var_count())
  {}

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

  /* This instruction will not be short cut optimized. */
  uint opt_shortcut_jump(sp_head *sp, sp_instr *start) override
  {
    return m_ip;
  }

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override;

private:

  uint m_frame;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_hreturn : public sp_instr_jump


/** This is DECLARE CURSOR */
class sp_instr_cpush : public sp_instr, public sp_cursor
{
  sp_instr_cpush(const sp_instr_cpush &); /**< Prevent use of these */
  void operator=(sp_instr_cpush &);

public:

  sp_instr_cpush(uint ip, sp_pcontext *ctx, LEX *lex, uint offset)
    : sp_instr(ip, ctx), m_lex_keeper(lex, true), m_cursor(offset)
  {}

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

  /**
    This call is used to cleanup the instruction when a sensitive
    cursor is closed. For now stored procedures always use materialized
    cursors and the call is not used.
  */
  bool cleanup_stmt(bool /*restore_set_statement_vars*/) override
  { return false; }
private:

  sp_lex_keeper m_lex_keeper;
  uint m_cursor;                /**< Frame offset (for debugging) */

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_cpush : public sp_instr


class sp_instr_cpop : public sp_instr
{
  sp_instr_cpop(const sp_instr_cpop &); /**< Prevent use of these */
  void operator=(sp_instr_cpop &);

public:

  sp_instr_cpop(uint ip, sp_pcontext *ctx, uint count)
    : sp_instr(ip, ctx), m_count(count)
  {}

  void update_count(uint count)
  {
    m_count= count;
  }

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

private:

  uint m_count;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_cpop : public sp_instr


class sp_instr_copen : public sp_instr
{
  sp_instr_copen(const sp_instr_copen &); /**< Prevent use of these */
  void operator=(sp_instr_copen &);

public:

  sp_instr_copen(uint ip, sp_pcontext *ctx, uint c)
    : sp_instr(ip, ctx), m_cursor(c)
  {}

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

private:

  uint m_cursor;                ///< Stack index

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_copen : public sp_instr_stmt


/**
  Initialize the structure of a cursor%ROWTYPE variable
  from the LEX containing the cursor SELECT statement.
*/
class sp_instr_cursor_copy_struct: public sp_instr
{
  /**< Prevent use of these */
  sp_instr_cursor_copy_struct(const sp_instr_cursor_copy_struct &);
  void operator=(sp_instr_cursor_copy_struct &);
  sp_lex_keeper m_lex_keeper;
  uint m_cursor;
  uint m_var;
public:
  sp_instr_cursor_copy_struct(uint ip, sp_pcontext *ctx, uint coffs,
                              sp_lex_cursor *lex, uint voffs)
    : sp_instr(ip, ctx), m_lex_keeper(lex, false),
      m_cursor(coffs),
      m_var(voffs)
  {}
  int execute(THD *thd, uint *nextp) override;
  int exec_core(THD *thd, uint *nextp) override;
  void print(String *str) override;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
};


class sp_instr_cclose : public sp_instr
{
  sp_instr_cclose(const sp_instr_cclose &); /**< Prevent use of these */
  void operator=(sp_instr_cclose &);

public:

  sp_instr_cclose(uint ip, sp_pcontext *ctx, uint c)
    : sp_instr(ip, ctx), m_cursor(c)
  {}

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

private:

  uint m_cursor;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_cclose : public sp_instr


class sp_instr_cfetch : public sp_instr
{
  sp_instr_cfetch(const sp_instr_cfetch &); /**< Prevent use of these */
  void operator=(sp_instr_cfetch &);

public:

  sp_instr_cfetch(uint ip, sp_pcontext *ctx, uint c, bool error_on_no_data)
    : sp_instr(ip, ctx), m_cursor(c), m_error_on_no_data(error_on_no_data)
  {
    m_varlist.empty();
  }

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

  void add_to_varlist(sp_variable *var)
  {
    m_varlist.push_back(var);
  }

private:

  uint m_cursor;
  List<sp_variable> m_varlist;
  bool m_error_on_no_data;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_cfetch : public sp_instr

/*
This class is created for the special fetch instruction
FETCH GROUP NEXT ROW, used in the user-defined aggregate
functions
*/

class sp_instr_agg_cfetch : public sp_instr
{
  sp_instr_agg_cfetch(const sp_instr_cfetch &); /**< Prevent use of these */
  void operator=(sp_instr_cfetch &);

public:

  sp_instr_agg_cfetch(uint ip, sp_pcontext *ctx)
    : sp_instr(ip, ctx){}

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_agg_cfetch : public sp_instr


class sp_instr_error : public sp_instr
{
  sp_instr_error(const sp_instr_error &); /**< Prevent use of these */
  void operator=(sp_instr_error &);

public:

  sp_instr_error(uint ip, sp_pcontext *ctx, int errcode)
    : sp_instr(ip, ctx), m_errcode(errcode)
  {}

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override
  {
    marked= 1;
    return UINT_MAX;
  }

private:

  int m_errcode;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_error : public sp_instr


class sp_instr_set_case_expr : public sp_instr_opt_meta
{
public:

  sp_instr_set_case_expr(uint ip, sp_pcontext *ctx, uint case_expr_id,
                         Item *case_expr, LEX *lex)
    : sp_instr_opt_meta(ip, ctx),
      m_case_expr_id(case_expr_id), m_case_expr(case_expr),
      m_lex_keeper(lex, true)
  {}

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override;

  void opt_move(uint dst, List<sp_instr> *ibp) override;

  void set_destination(uint old_dest, uint new_dest) override
  {
    if (m_cont_dest == old_dest)
      m_cont_dest= new_dest;
  }

private:

  uint m_case_expr_id;
  Item *m_case_expr;
  sp_lex_keeper m_lex_keeper;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_set_case_expr : public sp_instr_opt_meta

#endif
