#ifndef _SP_INSTR_H_
#define _SP_INSTR_H_

#include "mariadb.h"

#include "sql_alloc.h"    // Sql_alloc
#include "sql_class.h"    // THD, Query_arena
#include "sql_lex.h"      // class sp_lex_local
#include "sp_pcontext.h"  // class sp_pcontext
#include "sp_head.h"      // class sp_head

/*
  Sufficient max length of frame offsets.
*/
static const int SP_INSTR_UINT_MAXLEN= 8;

class sp_lex_cursor: public sp_lex_local, public Query_arena
{
public:
  sp_lex_cursor(THD *thd, const LEX *oldlex, MEM_ROOT *mem_root_arg)
    : sp_lex_local(thd, oldlex),
      Query_arena(mem_root_arg, STMT_INITIALIZED_FOR_SP),
      m_expr_str(empty_clex_str)
  {}

  sp_lex_cursor(THD *thd, const LEX *oldlex)
    : sp_lex_local(thd, oldlex),
      Query_arena(thd->lex->sphead->get_main_mem_root(),
                  STMT_INITIALIZED_FOR_SP),
      m_expr_str(empty_clex_str)
  {}

  ~sp_lex_cursor() { free_items(); }

  bool cleanup_stmt(bool /*restore_set_statement_vars*/) override
  {
    return false;
  }

  Query_arena *query_arena() override
  {
    return this;
  }

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

  void set_expr_str(const LEX_CSTRING &expr_str)
  {
    m_expr_str= expr_str;
  }

  const LEX_CSTRING &get_expr_str() const
  {
    return m_expr_str;
  }

  sp_lex_cursor* get_lex_for_cursor() override
  {
    return this;
  }

private:
  LEX_CSTRING m_expr_str;
};


//
// "Instructions"...
//

// Forward declaration for use in the method sp_instr::opt_move().
class sp_instr_opt_meta;

class sp_instr :public Query_arena, public Sql_alloc
{
  sp_instr(const sp_instr &);	/**< Prevent use of these */
  void operator=(sp_instr &);

public:
  uint marked;
  uint m_ip;			///< My index
  sp_pcontext *m_ctx;		///< My parse context
  uint m_lineno;

  /// Should give each a name or type code for debugging purposes?
  sp_instr(uint ip, sp_pcontext *ctx)
    : Query_arena(0, STMT_INITIALIZED_FOR_SP),
      marked(0),
      m_ip(ip),
      m_ctx(ctx),
      m_lineno(0)
#ifdef PROTECT_STATEMENT_MEMROOT
      , m_has_been_run(false)
#endif
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
    return m_ip+1;
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
  virtual void opt_move(uint dst, List<sp_instr_opt_meta> *ibp)
  {
    m_ip= dst;
  }

  virtual PSI_statement_info* get_psi_info() = 0;

  virtual SQL_I_List<Item_trigger_field>* get_instr_trig_field_list()
  {
    return nullptr;
  }

#ifdef PROTECT_STATEMENT_MEMROOT
  bool has_been_run() const
  {
    return m_has_been_run;
  }

  void mark_as_run()
  {
    m_has_been_run= true;
  }

  void mark_as_not_run()
  {
    m_has_been_run= false;
  }

private:
  bool m_has_been_run;
#endif
}; // class sp_instr : public Sql_alloc


class sp_instr;
class sp_lex_instr;

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
    : m_lex(lex),
      m_lex_resp(lex_resp),
      prelocking_tables(nullptr),
      lex_query_tables_own_last(nullptr),
      m_first_execution(true)
  {
    lex->sp_lex_in_use= true;
  }

  ~sp_lex_keeper()
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
                              sp_instr* instr, bool rerun_the_same_instr);


  /**
    Do several attempts to execute an instruction.

    This method installs Reprepare_observer to catch possible metadata changes
    on depending database objects, then calls reset_lex_and_exec_core()
    to execute the instruction. If execution of the instruction fails, does
    re-parsing of the instruction and re-execute it.

    @param      thd           Thread context.
    @param[out] nextp         Pointer for storing a next instruction to execute
    @param      open_tables   Flag to specify if the function should check read
                              access to tables in LEX's table list and open and
                              lock them (used in instructions which need to
                              calculate some expression and don't execute
                              complete statement).
    @param      instr         instruction which we prepare context and run.

    @return 0 on success, 1 on error
  */
  int validate_lex_and_exec_core(THD *thd, uint *nextp, bool open_tables,
                                 sp_lex_instr* instr);

  int cursor_reset_lex_and_exec_core(THD *thd, uint *nextp, bool open_tables,
                                     sp_lex_instr *instr);

  /**
    (Re-)parse the query corresponding to this instruction and return a new
    LEX-object.

    @param thd  Thread context.
    @param sp   The stored program.

    @return new LEX-object or NULL in case of failure.
  */
  LEX *parse_expr(THD *thd, const sp_head *sp);

  inline uint sql_command() const
  {
    return (uint)m_lex->sql_command;
  }

  void disable_query_cache()
  {
    m_lex->safe_to_cache_query= 0;
  }

private:
  /**
    Clean up and destroy owned LEX object.
  */
  void free_lex(THD *thd);

  /**
    Set LEX object.

    @param lex           LEX-object
  */
  void set_lex(LEX *lex);

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

  bool m_first_execution;
};


/**
  The base class for any stored program instruction that need to get access
  to a LEX object on execution.
*/

class sp_lex_instr : public sp_instr
{
public:
  sp_lex_instr(uint ip, sp_pcontext *ctx, LEX *lex, bool is_lex_owner)
  : sp_instr(ip, ctx),
    m_lex_keeper(lex, is_lex_owner)
  {}

  virtual bool is_invalid() const = 0;

  virtual void invalidate() = 0;

  /**
    Return the query string, which can be passed to the parser,
    that is a valid SQL-statement.

    @param[out] sql_query SQL-statement query string.
  */
  virtual void get_query(String *sql_query) const;


  /**
    (Re-)parse the query corresponding to this instruction and return a new
    LEX-object.

    @param thd  Thread context.
    @param sp   The stored program.
    @param lex  SP instruction's lex

    @return new LEX-object or NULL in case of failure.
  */
  LEX *parse_expr(THD *thd, sp_head *sp, LEX *lex);

  SQL_I_List<Item_trigger_field>* get_instr_trig_field_list() override
  {
    return &m_cur_trigger_stmt_items;
  }

protected:
  /**
    @return the expression query string. This string can't be passed directly
    to the parser as it is most likely not a valid SQL-statement.
  */
  virtual LEX_CSTRING get_expr_query() const = 0;

  /**
    Some expressions may be re-parsed as SELECT statements.
    This method is overridden in derived classes for instructions
    those SQL command should be adjusted.
  */
  virtual void adjust_sql_command(LEX *)
  {}

  /**
    Callback method which is called after an expression string successfully
    parsed and the thread context has not been switched to the outer context.
    The thread context contains new LEX-object corresponding to the parsed
    expression string.

    @param thd  Thread context.

    @return Error flag.
  */
  virtual bool on_after_expr_parsing(THD *)
  {
    return false;
  }

  sp_lex_keeper m_lex_keeper;

private:
  /**
    List of Item_trigger_field objects created on parsing of a SQL statement
    corresponding to this SP-instruction.
  */
  SQL_I_List<Item_trigger_field> m_cur_trigger_stmt_items;

  /**
    Clean up items previously created on behalf of the current instruction.
  */
  void cleanup_before_parsing(enum_sp_type sp_type);


  /**
    Set up field object for every NEW/OLD item of the trigger and
    move the list of Item_trigger_field objects, created on parsing the current
    trigger's instruction, from sp_head to trigger's SP instruction object.

    @param thd  current thread
    @param sp   sp_head object of the trigger
    @param next_trig_items_list  pointer to the next list of Item_trigger_field
                                 objects that used as a link between lists
                                 to support list of lists structure.

    @return false on success, true on failure
  */

  bool setup_table_fields_for_trigger(
    THD *thd, sp_head *sp,
    SQL_I_List<Item_trigger_field> *next_trig_items_list);
};


/**
  The class sp_instr_stmt represents almost all conventional SQL-statements.
*/

class sp_instr_stmt : public sp_lex_instr
{
  sp_instr_stmt(const sp_instr_stmt &);	/**< Prevent use of these */
  void operator=(sp_instr_stmt &);

  /**
    Flag to tell whether a metadata this instruction depends on
    has been changed and a LEX object should be reinitialized.
  */
  bool m_valid;

  LEX_STRING m_query;		///< For thd->query

public:
  sp_instr_stmt(uint ip, sp_pcontext *ctx, LEX *lex, const LEX_STRING& query)
    : sp_lex_instr(ip, ctx, lex, true),
      m_valid(true),
      m_query(query)
  {}

  virtual ~sp_instr_stmt() = default;

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  bool is_invalid() const override
  {
    return !m_valid;
  }

  void invalidate() override
  {
    m_valid= false;
  }

  void get_query(String *sql_query) const override
  {
    sql_query->append(get_expr_query());
  }

protected:
  LEX_CSTRING get_expr_query() const override
  {
    return LEX_CSTRING{m_query.str, m_query.length};
  }

  bool on_after_expr_parsing(THD *) override
  {
    m_valid= true;
    return false;
  }

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_stmt : public sp_lex_instr


class sp_instr_set : public sp_lex_instr
{
  sp_instr_set(const sp_instr_set &);	/**< Prevent use of these */
  void operator=(sp_instr_set &);

public:
  sp_instr_set(uint ip, sp_pcontext *ctx,
               const Sp_rcontext_handler *rh,
	       uint offset, Item *val,
               LEX *lex, bool lex_resp,
	       const LEX_CSTRING &expr_str)
    : sp_lex_instr(ip, ctx, lex, lex_resp),
      m_rcontext_handler(rh),
      m_offset(offset),
      m_value(val),
      m_expr_str(expr_str)
  {}

  virtual ~sp_instr_set() = default;

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  bool is_invalid() const override
  {
    return m_value == nullptr;
  }

  void invalidate() override
  {
    m_value= nullptr;
  }

protected:
  LEX_CSTRING get_expr_query() const override
  {
    return m_expr_str;
  }

  void adjust_sql_command(LEX *lex) override
  {
    DBUG_ASSERT(lex->sql_command == SQLCOM_SELECT);
    lex->sql_command= SQLCOM_SET_OPTION;
  }

  bool on_after_expr_parsing(THD *thd) override
  {
    DBUG_ASSERT(thd->lex->current_select->item_list.elements == 1);

    m_value= thd->lex->current_select->item_list.head();
    DBUG_ASSERT(m_value != nullptr);

    // Return error in release version if m_value == nullptr
    return m_value == nullptr;
  }

  sp_rcontext *get_rcontext(THD *thd) const;
  const Sp_rcontext_handler *m_rcontext_handler;
  uint m_offset;		///< Frame offset
  Item *m_value;

private:
  LEX_CSTRING m_expr_str;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_set : public sp_lex_instr


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
                         LEX *lex, bool lex_resp,
                         const LEX_CSTRING &value_query)
    : sp_instr_set(ip, ctx, rh, offset, val, lex, lex_resp, value_query),
      m_field_offset(field_offset)
  {}

  virtual ~sp_instr_set_row_field() = default;

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
                                 LEX *lex, bool lex_resp,
                                 const LEX_CSTRING &value_query)
    : sp_instr_set(ip, ctx, rh, offset, val, lex, lex_resp, value_query),
      m_field_name(field_name)
  {}

  virtual ~sp_instr_set_row_field_by_name() = default;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;
}; // class sp_instr_set_field_by_name : public sp_instr_set


/**
  Set NEW/OLD row field value instruction. Used in triggers.
*/

class sp_instr_set_trigger_field : public sp_lex_instr
{
  sp_instr_set_trigger_field(const sp_instr_set_trigger_field &);
  void operator=(sp_instr_set_trigger_field &);

public:
  sp_instr_set_trigger_field(uint ip, sp_pcontext *ctx,
                             Item_trigger_field *trg_fld,
                             Item *val, LEX *lex,
                             const LEX_CSTRING &value_query)
    : sp_lex_instr(ip, ctx, lex, true),
      trigger_field(trg_fld),
      value(val),
      m_expr_str(value_query)
  {
    m_trigger_field_name=
      LEX_CSTRING{strdup_root(current_thd->mem_root, trg_fld->field_name.str),
                              trg_fld->field_name.length};
  }

  virtual ~sp_instr_set_trigger_field() = default;

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  bool is_invalid() const override
  {
    return value == nullptr;
  }

  void invalidate() override
  {
    value= nullptr;
  }

protected:
  LEX_CSTRING get_expr_query() const override
  {
    return m_expr_str;
  }

  bool on_after_expr_parsing(THD *thd) override;

private:
  Item_trigger_field *trigger_field;
  Item *value;
  /**
    SQL clause corresponding to the expression value.
  */
  LEX_CSTRING m_expr_str;

  LEX_CSTRING m_trigger_field_name;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_trigger_field : public sp_lex_instr


/**
  An abstract class for all instructions with destinations that
  needs to be updated by the optimizer.

  Even if not all subclasses will use both the normal destination and
  the continuation destination, we put them both here for simplicity.
*/

class sp_instr_opt_meta
{
public:
  uint m_dest;			///< Where we will go
  uint m_cont_dest;             ///< Where continue handlers will go

  explicit sp_instr_opt_meta(uint dest)
    : m_dest(dest),
      m_cont_dest(0),
      m_optdest(0),
      m_cont_optdest(0)
  {}

  virtual ~sp_instr_opt_meta() = default;

  virtual void set_destination(uint old_dest, uint new_dest) = 0;

protected:
  sp_instr *m_optdest;		///< Used during optimization
  sp_instr *m_cont_optdest;     ///< Used during optimization
}; // class sp_instr_opt_meta


class sp_instr_jump : public sp_instr, public sp_instr_opt_meta
{
  sp_instr_jump(const sp_instr_jump &);	/**< Prevent use of these */
  void operator=(sp_instr_jump &);

public:
  sp_instr_jump(uint ip, sp_pcontext *ctx)
    : sp_instr(ip, ctx),
      sp_instr_opt_meta(0)
  {}

  sp_instr_jump(uint ip, sp_pcontext *ctx, uint dest)
    : sp_instr(ip, ctx),
      sp_instr_opt_meta(dest)
  {}

  virtual ~sp_instr_jump() = default;

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override;

  uint opt_shortcut_jump(sp_head *sp, sp_instr *start) override;

  void opt_move(uint dst, List<sp_instr_opt_meta> *ibp) override;

  void backpatch(uint dest, sp_pcontext *dst_ctx) override
  {
    /* Calling backpatch twice is a logic flaw in jump resolution. */
    DBUG_ASSERT(m_dest == 0);
    m_dest= dest;
  }

  uint get_cont_dest() const override
  {
    return m_cont_dest;
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
}; // class sp_instr_jump : public sp_instr, public sp_instr_opt_meta


class sp_instr_jump_if_not : public sp_lex_instr, public sp_instr_opt_meta
{
  /**< Prevent use of these */
  sp_instr_jump_if_not(const sp_instr_jump_if_not &);
  void operator=(sp_instr_jump_if_not &);

public:
  sp_instr_jump_if_not(uint ip, sp_pcontext *ctx, Item *i, LEX *lex,
                       const LEX_CSTRING &expr_query)
    : sp_lex_instr(ip, ctx, lex, true),
      sp_instr_opt_meta(0),
      m_expr(i),
      m_expr_str(expr_query)
  {}

  sp_instr_jump_if_not(uint ip, sp_pcontext *ctx, Item *i, uint dest, LEX *lex,
                       const LEX_CSTRING &expr_query)
    : sp_lex_instr(ip, ctx, lex, true),
      sp_instr_opt_meta(dest),
      m_expr(i),
      m_expr_str(expr_query)
  {}

  virtual ~sp_instr_jump_if_not() = default;

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override;

  /** Override sp_instr_jump's shortcut; we stop here */
  uint opt_shortcut_jump(sp_head *sp, sp_instr *start) override
  {
    return m_ip;
  }

  void opt_move(uint dst, List<sp_instr_opt_meta> *ibp) override;

  uint get_cont_dest() const override
  {
    return m_cont_dest;
  }

  void set_destination(uint old_dest, uint new_dest) override
  {
    if (m_dest == old_dest)
      m_dest= new_dest;
    if (m_cont_dest == old_dest)
      m_cont_dest= new_dest;
  }

  void backpatch(uint dest, sp_pcontext *dst_ctx) override
  {
    /* Calling backpatch twice is a logic flaw in jump resolution. */
    DBUG_ASSERT(m_dest == 0);
    m_dest= dest;
  }

  bool is_invalid() const override
  {
    return m_expr == nullptr;
  }

  void invalidate() override
  {
    m_expr= nullptr;
  }

protected:
  LEX_CSTRING get_expr_query() const override
  {
    return m_expr_str;
  }

  void adjust_sql_command(LEX *lex) override
  {
    assert(lex->sql_command == SQLCOM_SELECT);
    lex->sql_command= SQLCOM_END;
  }

  bool on_after_expr_parsing(THD *thd) override
  {
    DBUG_ASSERT(thd->lex->current_select->item_list.elements == 1);

    m_expr= thd->lex->current_select->item_list.head();
    DBUG_ASSERT(m_expr != nullptr);

    // Return error in release version if m_expr == nullptr
    return m_expr == nullptr;
  }

private:
  Item *m_expr;			///< The condition
  LEX_CSTRING m_expr_str;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_jump_if_not


class sp_instr_preturn : public sp_instr
{
  sp_instr_preturn(const sp_instr_preturn &);	/**< Prevent use of these */
  void operator=(sp_instr_preturn &);

public:
  sp_instr_preturn(uint ip, sp_pcontext *ctx)
    : sp_instr(ip, ctx)
  {}

  virtual ~sp_instr_preturn() = default;

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


class sp_instr_freturn : public sp_lex_instr
{
  sp_instr_freturn(const sp_instr_freturn &);	/**< Prevent use of these */
  void operator=(sp_instr_freturn &);

public:
  sp_instr_freturn(uint ip, sp_pcontext *ctx,
		   Item *val, const Type_handler *handler, sp_expr_lex *lex)
    : sp_lex_instr(ip, ctx, lex, true),
      m_value(val),
      m_type_handler(handler),
      m_expr_str(lex->get_expr_str())
  {}

  virtual ~sp_instr_freturn() = default;

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override
  {
    marked= 1;
    return UINT_MAX;
  }

  bool is_invalid() const override
  {
    return m_value == nullptr;
  }

  void invalidate() override
  {
    m_value= nullptr;
  }

protected:
  LEX_CSTRING get_expr_query() const override
  {
    return m_expr_str;
  }

  bool on_after_expr_parsing(THD *thd) override
  {
    DBUG_ASSERT(thd->lex->current_select->item_list.elements == 1);
    m_value= thd->lex->current_select->item_list.head();
    DBUG_ASSERT(m_value != nullptr);

    // Return error in release version if m_value == nullptr
    return m_value == nullptr;
  }

  Item *m_value;
  const Type_handler *m_type_handler;

private:
  /**
    SQL-query corresponding to the RETURN-expression.
  */
  LEX_CSTRING m_expr_str;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_freturn : public sp_lex_instr


class sp_instr_hpush_jump : public sp_instr_jump
{
  sp_instr_hpush_jump(const sp_instr_hpush_jump &); /**< Prevent use of these */
  void operator=(sp_instr_hpush_jump &);

public:
  sp_instr_hpush_jump(uint ip,
                      sp_pcontext *ctx,
                      sp_handler *handler)
    : sp_instr_jump(ip, ctx),
      m_handler(handler),
      m_opt_hpop(0),
      m_frame(ctx->current_var_count())
  {
    DBUG_ASSERT(m_handler->condition_values.elements == 0);
  }

  ~sp_instr_hpush_jump() override
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
  {
    m_handler->condition_values.push_back(condition_value);
  }

  sp_handler *get_handler()
  {
    return m_handler;
  }

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
  sp_instr_hpop(const sp_instr_hpop &);	/**< Prevent use of these */
  void operator=(sp_instr_hpop &);

public:
  sp_instr_hpop(uint ip, sp_pcontext *ctx, uint count)
    : sp_instr(ip, ctx),
      m_count(count)
  {}

  virtual ~sp_instr_hpop() = default;

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
  sp_instr_hreturn(const sp_instr_hreturn &);	/**< Prevent use of these */
  void operator=(sp_instr_hreturn &);

public:
  sp_instr_hreturn(uint ip, sp_pcontext *ctx)
    : sp_instr_jump(ip, ctx),
      m_frame(ctx->current_var_count())
  {}

  virtual ~sp_instr_hreturn() = default;

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


/**
  This is DECLARE CURSOR
*/

class sp_instr_cpush : public sp_lex_instr, public sp_cursor
{
  sp_instr_cpush(const sp_instr_cpush &); /**< Prevent use of these */
  void operator=(sp_instr_cpush &);

public:
  sp_instr_cpush(uint ip, sp_pcontext *ctx, sp_lex_cursor *lex, uint offset)
    : sp_lex_instr(ip, ctx, lex, true),
      m_cursor(offset),
      m_metadata_changed(false),
      m_cursor_stmt(lex->get_expr_str())
  {}

  virtual ~sp_instr_cpush() = default;

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  /**
    This call is used to cleanup the instruction when a sensitive
    cursor is closed. For now stored procedures always use materialized
    cursors and the call is not used.
  */
  bool cleanup_stmt(bool /*restore_set_statement_vars*/) override
  {
    return false;
  }

  bool is_invalid() const override
  {
    return m_metadata_changed;
  }

  void invalidate() override
  {
    m_metadata_changed= true;
  }

  sp_lex_keeper *get_lex_keeper() override
  {
    return &m_lex_keeper;
  }

  void get_query(String *sql_query) const override
  {
    sql_query->append(get_expr_query());
  }

  sp_instr_cpush *get_push_instr() override { return this; }

protected:
  LEX_CSTRING get_expr_query() const override
  {
    /*
      Lexer on processing the clause CURSOR FOR / CURSOR IS doesn't
      move a pointer on cpp_buf after the token FOR/IS so skip it explicitly
      in order to get correct value of cursor's query string.
    */
    if (strncasecmp(m_cursor_stmt.str, "FOR ", 4) == 0)
      return LEX_CSTRING{m_cursor_stmt.str + 4, m_cursor_stmt.length - 4};
    if (strncasecmp(m_cursor_stmt.str, "IS ", 3) == 0)
      return LEX_CSTRING{m_cursor_stmt.str + 3, m_cursor_stmt.length - 3};
    return m_cursor_stmt;
  }

  bool on_after_expr_parsing(THD *) override
  {
    m_metadata_changed= false;
    return false;
  }

private:
  uint m_cursor;                /**< Frame offset (for debugging) */
  /**
    Flag if a statement's metadata has been changed in result of running DDL
    on depending database objects used in the statement.
  */
  bool m_metadata_changed;

  LEX_CSTRING m_cursor_stmt;

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
    : sp_instr(ip, ctx),
      m_count(count)
  {}

  virtual ~sp_instr_cpop() = default;

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
    : sp_instr(ip, ctx),
      m_cursor(c)
  {}

  virtual ~sp_instr_copen() = default;

  int execute(THD *thd, uint *nextp) override;

  void print(String *str) override;

private:
  uint m_cursor;		///< Stack index

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_copen : public sp_instr_stmt


/**
  Initialize the structure of a cursor%ROWTYPE variable
  from the LEX containing the cursor SELECT statement.
*/

class sp_instr_cursor_copy_struct: public sp_lex_instr
{
  /**< Prevent use of these */
  sp_instr_cursor_copy_struct(const sp_instr_cursor_copy_struct &);
  void operator=(sp_instr_cursor_copy_struct &);
  uint m_cursor;
  uint m_var;
  /**
    Flag to tell whether metadata has been changed and the LEX object should
    be reinitialized.
  */
  bool m_valid;
  LEX_CSTRING m_cursor_stmt;

public:
  sp_instr_cursor_copy_struct(uint ip, sp_pcontext *ctx, uint coffs,
                              sp_lex_cursor *lex, uint voffs)
    : sp_lex_instr(ip, ctx, lex, false),
      m_cursor(coffs),
      m_var(voffs),
      m_valid(true),
      m_cursor_stmt(lex->get_expr_str())
  {}
  virtual ~sp_instr_cursor_copy_struct() = default;
  int execute(THD *thd, uint *nextp) override;
  int exec_core(THD *thd, uint *nextp) override;
  void print(String *str) override;
  bool is_invalid() const override
  {
    return !m_valid;
  }

  void invalidate() override
  {
    m_valid= false;
  }

  void get_query(String *sql_query) const override
  {
    sql_query->append(get_expr_query());
  }

protected:
  LEX_CSTRING get_expr_query() const override
  {
    /*
      Lexer on processing the clause CURSOR FOR / CURSOR IS doesn't
      move a pointer on cpp_buf after the token FOR/IS so skip it explicitly
      in order to get correct value of cursor's query string.
    */
    if (strncasecmp(m_cursor_stmt.str, "FOR ", 4) == 0)
      return LEX_CSTRING{m_cursor_stmt.str + 4, m_cursor_stmt.length - 4};
    if (strncasecmp(m_cursor_stmt.str, "IS ", 3) == 0)
      return LEX_CSTRING{m_cursor_stmt.str + 3, m_cursor_stmt.length - 3};
    return m_cursor_stmt;
  }

  bool on_after_expr_parsing(THD *) override
  {
    m_valid= true;
    return false;
  }

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
    : sp_instr(ip, ctx),
      m_cursor(c)
  {}

  virtual ~sp_instr_cclose() = default;

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
    : sp_instr(ip, ctx),
      m_cursor(c),
      m_error_on_no_data(error_on_no_data)
  {
    m_varlist.empty();
  }

  virtual ~sp_instr_cfetch() = default;

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
    : sp_instr(ip, ctx)
  {}

  virtual ~sp_instr_agg_cfetch() = default;

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
    : sp_instr(ip, ctx),
      m_errcode(errcode)
  {}

  virtual ~sp_instr_error() = default;

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


class sp_instr_set_case_expr : public sp_lex_instr, public sp_instr_opt_meta
{
public:
  sp_instr_set_case_expr(uint ip, sp_pcontext *ctx, uint case_expr_id,
                         Item *case_expr, LEX *lex,
                         const LEX_CSTRING &case_expr_query)
    : sp_lex_instr(ip, ctx, lex, true),
      sp_instr_opt_meta(0),
      m_case_expr_id(case_expr_id),
      m_case_expr(case_expr),
      m_expr_str(case_expr_query)
  {}

  virtual ~sp_instr_set_case_expr() = default;

  int execute(THD *thd, uint *nextp) override;

  int exec_core(THD *thd, uint *nextp) override;

  void print(String *str) override;

  uint opt_mark(sp_head *sp, List<sp_instr> *leads) override;

  void opt_move(uint dst, List<sp_instr_opt_meta> *ibp) override;

  uint get_cont_dest() const override
  {
    return m_cont_dest;
  }

  void set_destination(uint old_dest, uint new_dest) override
  {
    if (m_cont_dest == old_dest)
      m_cont_dest= new_dest;
  }

  bool is_invalid() const override
  {
    return m_case_expr == nullptr;
  }

  void invalidate() override
  {
    m_case_expr= nullptr;
  }

protected:
  LEX_CSTRING get_expr_query() const override
  {
    return m_expr_str;
  }

  void adjust_sql_command(LEX *lex) override
  {
    assert(lex->sql_command == SQLCOM_SELECT);
    lex->sql_command= SQLCOM_END;
  }

  bool on_after_expr_parsing(THD *thd) override
  {
    DBUG_ASSERT(thd->lex->current_select->item_list.elements == 1);

    m_case_expr= thd->lex->current_select->item_list.head();
    DBUG_ASSERT(m_case_expr != nullptr);

    // Return error in release version if m_case_expr == nullptr
    return m_case_expr == nullptr;
  }

private:
  uint m_case_expr_id;
  Item *m_case_expr;
  LEX_CSTRING m_expr_str;

public:
  PSI_statement_info* get_psi_info() override { return & psi_info; }
  static PSI_statement_info psi_info;
}; // class sp_instr_set_case_expr : public sp_lex_instr,

#endif
