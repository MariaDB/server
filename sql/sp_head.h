/* -*- C++ -*- */
/*
   Copyright (c) 2002, 2011, Oracle and/or its affiliates.
   Copyright (c) 2020, 2022, MariaDB

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

#ifndef _SP_HEAD_H_
#define _SP_HEAD_H_

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

/*
  It is necessary to include set_var.h instead of item.h because there
  are dependencies on include order for set_var.h and item.h. This
  will be resolved later.
*/
#include "sql_class.h"                          // THD, set_var.h: THD
#include "set_var.h"                            // Item
#include "sp_pcontext.h"                        // sp_pcontext
#include <stddef.h>
#include "sp.h"

/**
  @defgroup Stored_Routines Stored Routines
  @ingroup Runtime_Environment
  @{
*/

uint
sp_get_flags_for_command(LEX *lex);

class sp_instr;
class sp_instr_opt_meta;
class sp_instr_jump_if_not;

/**
  Number of PSI_statement_info instruments
  for internal stored programs statements.
*/
#ifdef HAVE_PSI_INTERFACE
void init_sp_psi_keys(void);
#endif

/*************************************************************************/

/**
  Stored_program_creation_ctx -- base class for creation context of stored
  programs (stored routines, triggers, events).
*/

class Stored_program_creation_ctx :public Default_object_creation_ctx
{
public:
  CHARSET_INFO *get_db_cl()
  {
    return m_db_cl;
  }

public:
  virtual Stored_program_creation_ctx *clone(MEM_ROOT *mem_root) = 0;

protected:
  Stored_program_creation_ctx(THD *thd)
    : Default_object_creation_ctx(thd),
      m_db_cl(thd->variables.collation_database)
  { }

  Stored_program_creation_ctx(CHARSET_INFO *client_cs,
                              CHARSET_INFO *connection_cl,
                              CHARSET_INFO *db_cl)
    : Default_object_creation_ctx(client_cs, connection_cl),
      m_db_cl(db_cl)
  { }

protected:
  void change_env(THD *thd) const override
  {
    thd->variables.collation_database= m_db_cl;

    Default_object_creation_ctx::change_env(thd);
  }

protected:
  /**
    db_cl stores the value of the database collation. Both character set
    and collation attributes are used.

    Database collation is included into the context because it defines the
    default collation for stored-program variables.
  */
  CHARSET_INFO *m_db_cl;
};

/*************************************************************************/

class sp_name : public Sql_alloc,
                public Database_qualified_name
{
public:
  bool       m_explicit_name;                   /**< Prepend the db name? */

  sp_name(const LEX_CSTRING *db, const LEX_CSTRING *name,
          bool use_explicit_name)
    : Database_qualified_name(db, name), m_explicit_name(use_explicit_name)
  {
    if (lower_case_table_names && m_db.length)
      m_db.length= my_casedn_str(files_charset_info, (char*) m_db.str);
  }

  /** Create temporary sp_name object from MDL key. Store in qname_buff */
  sp_name(const MDL_key *key, char *qname_buff);

  ~sp_name() = default;
};


bool
check_routine_name(const LEX_CSTRING *ident);

class sp_head :private Query_arena,
               public Database_qualified_name,
               public Sql_alloc
{
  sp_head(const sp_head &)= delete;
  void operator=(sp_head &)= delete;

protected:
  MEM_ROOT main_mem_root;
#ifdef PROTECT_STATEMENT_MEMROOT
  /*
    The following data member is wholly for debugging purpose.
    It can be used for possible crash analysis to determine how many times
    the stored routine was executed before the mem_root marked read_only
    was requested for a memory chunk. Additionally, a value of this data
    member is output to the log with DBUG_PRINT.
  */
  ulong executed_counter;
#endif
public:
  /** Possible values of m_flags */
  enum {
    HAS_RETURN= 1,              // For FUNCTIONs only: is set if has RETURN
    MULTI_RESULTS= 8,           // Is set if a procedure with SELECT(s)
    CONTAINS_DYNAMIC_SQL= 16,   // Is set if a procedure with PREPARE/EXECUTE
    IS_INVOKED= 32,             // Is set if this sp_head is being used
    HAS_SET_AUTOCOMMIT_STMT= 64,// Is set if a procedure with 'set autocommit'
    /* Is set if a procedure with COMMIT (implicit or explicit) | ROLLBACK */
    HAS_COMMIT_OR_ROLLBACK= 128,
    LOG_SLOW_STATEMENTS= 256,   // Used by events
    LOG_GENERAL_LOG= 512,        // Used by events
    HAS_SQLCOM_RESET= 1024,
    HAS_SQLCOM_FLUSH= 2048,

    /**
      Marks routines that directly (i.e. not by calling other routines)
      change tables. Note that this flag is set automatically based on
      type of statements used in the stored routine and is different
      from routine characteristic provided by user in a form of CONTAINS
      SQL, READS SQL DATA, MODIFIES SQL DATA clauses. The latter are
      accepted by parser but pretty much ignored after that.
      We don't rely on them:
      a) for compatibility reasons.
      b) because in CONTAINS SQL case they don't provide enough
      information anyway.
     */
    MODIFIES_DATA= 4096,
    /*
      Marks routines that have column type references: DECLARE a t1.a%TYPE;
    */
    HAS_COLUMN_TYPE_REFS= 8192,
    /* Set if has FETCH GROUP NEXT ROW instr. Used to ensure that only
       functions with AGGREGATE keyword use the instr. */
    HAS_AGGREGATE_INSTR= 16384
  };

  sp_package *m_parent;
  const Sp_handler *m_handler;
  uint m_flags;                 // Boolean attributes of a stored routine

  /**
    Instrumentation interface for SP.
  */
  PSI_sp_share *m_sp_share;

  Column_definition m_return_field_def; /**< This is used for FUNCTIONs only. */

  const char *m_tmp_query;	///< Temporary pointer to sub query string
private:
  /*
    Private to guarantee that m_chistics.comment is properly set to:
    - a string which is alloced on this->mem_root
    - or (NULL,0)
    set_chistics() makes sure this.
  */
  Sp_chistics m_chistics;
  void set_chistics(const st_sp_chistics &chistics);
  inline void set_chistics_agg_type(enum enum_sp_aggregate_type type)
  {
    m_chistics.agg_type= type;
  }
public:
  sql_mode_t m_sql_mode;		///< For SHOW CREATE and execution
  bool       m_explicit_name;                   /**< Prepend the db name? */
  LEX_CSTRING m_qname;		///< db.name
  LEX_CSTRING m_params;
  LEX_CSTRING m_body;
  LEX_CSTRING m_body_utf8;
  LEX_CSTRING m_defstr;
  AUTHID      m_definer;

  const st_sp_chistics &chistics() const { return m_chistics; }
  const LEX_CSTRING &comment() const { return m_chistics.comment; }
  void set_suid(enum_sp_suid_behaviour suid) { m_chistics.suid= suid; }
  enum_sp_suid_behaviour suid() const { return m_chistics.suid; }
  bool detistic() const { return m_chistics.detistic; }
  enum_sp_data_access daccess() const { return m_chistics.daccess; }
  enum_sp_aggregate_type agg_type() const { return m_chistics.agg_type; }
  /**
    Is this routine being executed?
  */
  virtual bool is_invoked() const { return m_flags & IS_INVOKED; }

  /**
    Get the value of the SP cache version, as remembered
    when the routine was inserted into the cache.
  */
  ulong sp_cache_version() const;

  /** Set the value of the SP cache version.  */
  void set_sp_cache_version(ulong version_arg) const
  {
    m_sp_cache_version= version_arg;
  }

  sp_rcontext *rcontext_create(THD *thd, Field *retval, List<Item> *args);
  sp_rcontext *rcontext_create(THD *thd, Field *retval,
                               Item **args, uint arg_count);
  sp_rcontext *rcontext_create(THD *thd, Field *retval,
                               Row_definition_list *list,
                               bool switch_security_ctx);
  bool eq_routine_spec(const sp_head *) const;
private:
  /**
    Version of the stored routine cache at the moment when the
    routine was added to it. Is used only for functions and
    procedures, not used for triggers or events.  When sp_head is
    created, its version is 0. When it's added to the cache, the
    version is assigned the global value 'Cversion'.
    If later on Cversion is incremented, we know that the routine
    is obsolete and should not be used --
    sp_cache_flush_obsolete() will purge it.
  */
  mutable ulong m_sp_cache_version;
  Stored_program_creation_ctx *m_creation_ctx;
  /**
    Boolean combination of (1<<flag), where flag is a member of
    LEX::enum_binlog_stmt_unsafe.
  */
  uint32 unsafe_flags;

  bool new_query_arena_is_set;
public:
  inline Stored_program_creation_ctx *get_creation_ctx()
  {
    return m_creation_ctx;
  }

  inline void set_creation_ctx(Stored_program_creation_ctx *creation_ctx)
  {
    m_creation_ctx= creation_ctx->clone(mem_root);
  }

  longlong m_created;
  longlong m_modified;
  /** Recursion level of the current SP instance. The levels are numbered from 0 */
  ulong m_recursion_level;
  /**
    A list of diferent recursion level instances for the same procedure.
    For every recursion level we have a sp_head instance. This instances
    connected in the list. The list ordered by increasing recursion level
    (m_recursion_level).
  */
  sp_head *m_next_cached_sp;
  /**
    Pointer to the first element of the above list
  */
  sp_head *m_first_instance;
  /**
    Pointer to the first free (non-INVOKED) routine in the list of
    cached instances for this SP. This pointer is set only for the first
    SP in the list of instences (see above m_first_cached_sp pointer).
    The pointer equal to 0 if we have no free instances.
    For non-first instance value of this pointer meanless (point to itself);
  */
  sp_head *m_first_free_instance;
  /**
    Pointer to the last element in the list of instances of the SP.
    For non-first instance value of this pointer meanless (point to itself);
  */
  sp_head *m_last_cached_sp;
  /**
    Set containing names of stored routines used by this routine.
    Note that unlike elements of similar set for statement elements of this
    set are not linked in one list. Because of this we are able save memory
    by using for this set same objects that are used in 'sroutines' sets
    for statements of which this stored routine consists.
  */
  HASH m_sroutines;
  // Pointers set during parsing
  const char *m_param_begin;
  const char *m_param_end;

private:
  /*
    A pointer to the body start inside the cpp buffer.
    Used only during parsing. Should be removed eventually.
    The affected functions/methods should be fixed to get the cpp body start
    as a parameter, rather than through this member.
  */
  const char *m_cpp_body_begin;

public:
  /*
    Security context for stored routine which should be run under
    definer privileges.
  */
  Security_context m_security_ctx;

protected:
  sp_head(MEM_ROOT *mem_root, sp_package *parent, const Sp_handler *handler,
          enum_sp_aggregate_type agg_type);
  virtual ~sp_head();
public:
  static void destroy(sp_head *sp);
  static sp_head *create(sp_package *parent, const Sp_handler *handler,
                         enum_sp_aggregate_type agg_type,
                         MEM_ROOT *sp_mem_root);

  /// Initialize after we have reset mem_root
  void
  init(LEX *lex);

  /** Copy sp name from parser. */
  bool
  init_sp_name(const sp_name *spname);

  /** Set the body-definition start position. */
  void
  set_body_start(THD *thd, const char *cpp_body_start);

  /** Set the statement-definition (body-definition) end position. */
  void
  set_stmt_end(THD *thd, const char *cpp_body_end);

  bool
  execute_trigger(THD *thd,
                  const LEX_CSTRING *db_name,
                  const LEX_CSTRING *table_name,
                  GRANT_INFO *grant_info);

  bool
  execute_function(THD *thd, Item **args, uint argcount, Field *return_fld,
                   sp_rcontext **nctx, Query_arena *call_arena);

  bool
  execute_procedure(THD *thd, List<Item> *args);

  static void
  show_create_routine_get_fields(THD *thd, const Sp_handler *sph,
                                 List<Item> *fields);

  bool
  show_create_routine(THD *thd, const Sp_handler *sph);

  MEM_ROOT *get_main_mem_root() { return &main_mem_root; }

  int
  add_instr(sp_instr *instr);

  bool
  add_instr_jump(THD *thd, sp_pcontext *spcont);

  bool
  add_instr_jump(THD *thd, sp_pcontext *spcont, uint dest);

  bool
  add_instr_jump_forward_with_backpatch(THD *thd, sp_pcontext *spcont,
                                        sp_label *lab);
  bool
  add_instr_jump_forward_with_backpatch(THD *thd, sp_pcontext *spcont)
  {
    return add_instr_jump_forward_with_backpatch(thd, spcont,
                                                 spcont->last_label());
  }

  bool
  add_instr_freturn(THD *thd, sp_pcontext *spcont, Item *item,
                    sp_expr_lex *lex);

  bool
  add_instr_preturn(THD *thd, sp_pcontext *spcont);

  Item *adjust_assignment_source(THD *thd, Item *val, Item *val2);
  /**
    @param thd                     - the current thd
    @param spcont                  - the current parse context
    @param spv                     - the SP variable
    @param val                     - the value to be assigned to the variable
    @param lex                     - the LEX that was used to create "val"
    @param responsible_to_free_lex - if the generated sp_instr_set should
                                     free "lex".
    @retval true                   - on error
    @retval false                  - on success
  */
  bool set_local_variable(THD *thd, sp_pcontext *spcont,
                          const Sp_rcontext_handler *rh,
                          sp_variable *spv, Item *val, LEX *lex,
                          bool responsible_to_free_lex,
                          const LEX_CSTRING &value_query);
  bool set_local_variable_row_field(THD *thd, sp_pcontext *spcont,
                                    const Sp_rcontext_handler *rh,
                                    sp_variable *spv, uint field_idx,
                                    Item *val, LEX *lex,
                                    const LEX_CSTRING &value_query);
  bool set_local_variable_row_field_by_name(THD *thd, sp_pcontext *spcont,
                                            const Sp_rcontext_handler *rh,
                                            sp_variable *spv,
                                            const LEX_CSTRING *field_name,
                                            Item *val, LEX *lex,
                                            const LEX_CSTRING &value_query);
  bool check_package_routine_end_name(const LEX_CSTRING &end_name) const;
  bool check_standalone_routine_end_name(const sp_name *end_name) const;
  bool check_group_aggregate_instructions_function() const;
  bool check_group_aggregate_instructions_forbid() const;
  bool check_group_aggregate_instructions_require() const;
private:
  /**
    Generate a code to set a single cursor parameter variable.
    @param thd          - current thd, for mem_root allocations.
    @param param_spcont - the context of the parameter block
    @param idx          - the index of the parameter
    @param prm          - the actual parameter (contains information about
                          the assignment source expression Item,
                          its free list, and its LEX)
  */
  bool add_set_cursor_param_variable(THD *thd,
                                     sp_pcontext *param_spcont, uint idx,
                                     sp_assignment_lex *prm)
  {
    DBUG_ASSERT(idx < param_spcont->context_var_count());
    sp_variable *spvar= param_spcont->get_context_variable(idx);
    /*
      add_instr() gets free_list from m_thd->free_list.
      Initialize it before the set_local_variable() call.
    */
    DBUG_ASSERT(m_thd->free_list == NULL);
    m_thd->free_list= prm->get_free_list();
    if (set_local_variable(thd, param_spcont,
                           &sp_rcontext_handler_local,
                           spvar, prm->get_item(), prm, true,
                           prm->get_expr_str()))
      return true;
    /*
      Safety:
      The item and its free_list are now fully owned by the sp_instr_set
      instance, created by set_local_variable(). The sp_instr_set instance
      is now responsible for freeing the item and the free_list.
      Reset the "item" and the "free_list" members of "prm",
      to avoid double pointers to the same objects from "prm" and
      from the sp_instr_set instance.
    */
    prm->set_item_and_free_list(NULL, NULL);
    return false;
  }

  /**
    Generate a code to set all cursor parameter variables.
    This method is called only when parameters exists,
    and the number of formal parameters matches the number of actual
    parameters. See also comments to add_open_cursor().
  */
  bool add_set_cursor_param_variables(THD *thd, sp_pcontext *param_spcont,
                                      List<sp_assignment_lex> *parameters)
  {
    DBUG_ASSERT(param_spcont->context_var_count() == parameters->elements);
    sp_assignment_lex *prm;
    List_iterator<sp_assignment_lex> li(*parameters);
    for (uint idx= 0; (prm= li++); idx++)
    {
      if (add_set_cursor_param_variable(thd, param_spcont, idx, prm))
        return true;
    }
    return false;
  }

  /**
    Generate a code to set all cursor parameter variables for a FOR LOOP, e.g.:
      FOR index IN cursor(1,2,3)
    @param
  */
  bool add_set_for_loop_cursor_param_variables(THD *thd,
                                               sp_pcontext *param_spcont,
                                               sp_assignment_lex *param_lex,
                                               Item_args *parameters);

  bool bind_input_param(THD *thd,
                        Item *arg_item,
                        uint arg_no,
                        sp_rcontext *nctx,
                        bool is_function);

  bool bind_output_param(THD *thd,
                         Item *arg_item,
                         uint arg_no,
                         sp_rcontext *octx,
                         sp_rcontext *nctx);

public:
  /**
    Generate a code for an "OPEN cursor" statement.
    @param thd          - current thd, for mem_root allocations
    @param spcont       - the context of the cursor
    @param offset       - the offset of the cursor
    @param param_spcont - the context of the cursor parameter block
    @param parameters   - the list of the OPEN actual parameters

    The caller must make sure that the number of local variables
    in "param_spcont" (formal parameters) matches the number of list elements
    in "parameters" (actual parameters).
    NULL in either of them means 0 parameters.
  */
  bool add_open_cursor(THD *thd, sp_pcontext *spcont,
                       uint offset,
                       sp_pcontext *param_spcont,
                       List<sp_assignment_lex> *parameters);

  /**
    Generate an initiation code for a CURSOR FOR LOOP, e.g.:
      FOR index IN cursor         -- cursor without parameters
      FOR index IN cursor(1,2,3)  -- cursor with parameters

    The code generated by this method does the following during SP run-time:
    - Sets all cursor parameter vartiables from "parameters"
    - Initializes the index ROW-type variable from the cursor
      (the structure is copied from the cursor to the index variable)
    - The cursor gets opened
    - The first records is fetched from the cursor to the variable "index".

    @param thd        - the current thread (for mem_root and error reporting)
    @param spcont     - the current parse context
    @param index      - the loop "index" ROW-type variable
    @param pcursor    - the cursor
    @param coffset    - the cursor offset
    @param param_lex  - the LEX that owns Items in "parameters"
    @param parameters - the cursor parameters Item array
    @retval true      - on error (EOM)
    @retval false     - on success
  */
  bool add_for_loop_open_cursor(THD *thd, sp_pcontext *spcont,
                                sp_variable *index,
                                const sp_pcursor *pcursor, uint coffset,
                                sp_assignment_lex *param_lex,
                                Item_args *parameters);
  /**
    Returns true if any substatement in the routine directly
    (not through another routine) modifies data/changes table.

    @sa Comment for MODIFIES_DATA flag.
  */
  bool modifies_data() const
  { return m_flags & MODIFIES_DATA; }

  inline uint instructions()
  { return (uint)m_instr.elements; }

  inline sp_instr *
  last_instruction()
  {
    sp_instr *i;

    get_dynamic(&m_instr, (uchar*)&i, m_instr.elements-1);
    return i;
  }

  bool replace_instr_to_nop(THD *thd, uint ip);

  /*
    Resets lex in 'thd' and keeps a copy of the old one.

    @todo Conflicting comment in sp_head.cc
  */
  bool
  reset_lex(THD *thd);

  bool
  reset_lex(THD *thd, sp_lex_local *sublex);

  /**
    Merge two LEX instances.
    @param oldlex - the upper level LEX we're going to restore to.
    @param sublex - the local lex that have just parsed some substatement.
    @returns      - false on success, true on error (e.g. failed to
                    merge the routine list or the table list).
    This method is shared by:
    - restore_lex(), when the old LEX is popped by sp_head::m_lex.pop()
    - THD::restore_from_local_lex_to_old_lex(), when the old LEX
      is stored in the caller's local variable.
  */
  bool
  merge_lex(THD *thd, LEX *oldlex, LEX *sublex);

  /**
    Restores lex in 'thd' from our copy, but keeps some status from the
    one in 'thd', like ptr, tables, fields, etc.

    @todo Conflicting comment in sp_head.cc
  */
  bool
  restore_lex(THD *thd)
  {
    DBUG_ENTER("sp_head::restore_lex");
    /*
      There is no a need to free the current thd->lex here.
      - In the majority of the cases restore_lex() is called
        on success and thd->lex does not need to be deleted.
      - In cases when restore_lex() is called on error,
        e.g. from sp_create_assignment_instr(), thd->lex is
        already linked to some sp_instr_xxx (using sp_lex_keeper).

      Note, we don't get to here in case of a syntax error
      when the current thd->lex is not yet completely
      initialized and linked. It gets automatically deleted
      by the Bison %destructor in sql_yacc.yy.
    */
    LEX *oldlex= (LEX *) m_lex.pop();
    if (!oldlex)
      DBUG_RETURN(false); // Nothing to restore
    // This restores thd->lex and thd->stmt_lex
    DBUG_RETURN(thd->restore_from_local_lex_to_old_lex(oldlex));
  }


  /**
    Delete all auxiliary LEX objects created on parsing a statement and
    restore a value of the data member THD::lex to point on the LEX object
    that was actual before parsing started.
  */

  void unwind_aux_lexes_and_restore_original_lex();


  /**
    Iterate through the LEX stack from the top (the newest) to the bottom
    (the oldest) and find the one that contains a non-zero spname.
    @returns - the address of spname, or NULL of no spname found.
  */
  const sp_name *find_spname_recursive()
  {
    uint count= m_lex.elements;
    for (uint i= 0; i < count; i++)
    {
      const LEX *tmp= m_lex.elem(count - i - 1);
      if (tmp->spname)
        return tmp->spname;
    }
    return NULL;
  }

  /// Put the instruction on the backpatch list, associated with the label.
  int
  push_backpatch(THD *thd, sp_instr *, sp_label *);
  int
  push_backpatch_goto(THD *thd, sp_pcontext *ctx, sp_label *lab);

  /// Update all instruction with this label in the backpatch list to
  /// the current position.
  void
  backpatch(sp_label *);
  void
  backpatch_goto(THD *thd, sp_label *, sp_label *);

  /// Check for unresolved goto label
  bool
  check_unresolved_goto();

  /// Start a new cont. backpatch level. If 'i' is NULL, the level is just incr.
  int
  new_cont_backpatch(sp_instr_opt_meta *i);

  /// Add an instruction to the current level
  int
  add_cont_backpatch(sp_instr_opt_meta *i);

  /// Backpatch (and pop) the current level to the current position.
  void
  do_cont_backpatch();

  /// Add cpush instructions for all cursors declared in the current frame
  bool sp_add_instr_cpush_for_cursors(THD *thd, sp_pcontext *pcontext);

  const LEX_CSTRING *name() const
  { return &m_name; }

  char *create_string(THD *thd, ulong *lenp);

  Field *create_result_field(uint field_max_length, const LEX_CSTRING *field_name,
                             TABLE *table) const;


  /**
    Check and prepare an instance of Column_definition for field creation
    (fill all necessary attributes), for variables, parameters and
    function return values.

    @param[in]  thd          Thread handle
    @param[in]  lex          Yacc parsing context
    @param[out] field_def    An instance of create_field to be filled

    @retval false on success
    @retval true  on error
  */
  bool fill_field_definition(THD *thd, Column_definition *field_def)
  {
    const Type_handler *h= field_def->type_handler();
    return h->Column_definition_fix_attributes(field_def) ||
           field_def->sp_prepare_create_field(thd, mem_root);
  }
  bool row_fill_field_definitions(THD *thd, Row_definition_list *row)
  {
    /*
      Prepare all row fields. This will (among other things)
      - convert VARCHAR lengths from character length to octet length
      - calculate interval lengths for SET and ENUM
    */
    List_iterator<Spvar_definition> it(*row);
    for (Spvar_definition *def= it++; def; def= it++)
    {
      if (fill_spvar_definition(thd, def))
        return true;
    }
    return false;
  }
  /**
    Check and prepare a Column_definition for a variable or a parameter.
  */
  bool fill_spvar_definition(THD *thd, Column_definition *def)
  {
    if (fill_field_definition(thd, def))
      return true;
    def->pack_flag|= FIELDFLAG_MAYBE_NULL;
    return false;
  }
  bool fill_spvar_definition(THD *thd, Column_definition *def,
                             LEX_CSTRING *name)
  {
    def->field_name= *name;
    return fill_spvar_definition(thd, def);
  }

private:
  /**
    Set a column type reference for a parameter definition
  */
  void fill_spvar_using_type_reference(sp_variable *spvar,
                                       Qualified_column_ident *ref)
  {
    spvar->field_def.set_column_type_ref(ref);
    spvar->field_def.field_name= spvar->name;
    m_flags|= sp_head::HAS_COLUMN_TYPE_REFS;
  }

  void fill_spvar_using_table_rowtype_reference(THD *thd,
                                                sp_variable *spvar,
                                                Table_ident *ref)
  {
    spvar->field_def.set_table_rowtype_ref(ref);
    spvar->field_def.field_name= spvar->name;
    fill_spvar_definition(thd, &spvar->field_def);
    m_flags|= sp_head::HAS_COLUMN_TYPE_REFS;
  }

public:
  bool spvar_fill_row(THD *thd, sp_variable *spvar, Row_definition_list *def);
  bool spvar_fill_type_reference(THD *thd, sp_variable *spvar,
                                 const LEX_CSTRING &table,
                                 const LEX_CSTRING &column);
  bool spvar_fill_type_reference(THD *thd, sp_variable *spvar,
                                 const LEX_CSTRING &db,
                                 const LEX_CSTRING &table,
                                 const LEX_CSTRING &column);
  bool spvar_fill_table_rowtype_reference(THD *thd, sp_variable *spvar,
                                          const LEX_CSTRING &table);
  bool spvar_fill_table_rowtype_reference(THD *thd, sp_variable *spvar,
                                          const LEX_CSTRING &db,
                                          const LEX_CSTRING &table);

  void set_c_chistics(const st_sp_chistics &chistics);
  void set_info(longlong created, longlong modified,
		const st_sp_chistics &chistics, sql_mode_t sql_mode);

  void set_definer(const char *definer, size_t definerlen)
  {
    AUTHID tmp;
    tmp.parse(definer, definerlen);
    m_definer.copy(mem_root, &tmp.user, &tmp.host);
  }
  void set_definer(const LEX_CSTRING *user_name, const LEX_CSTRING *host_name)
  {
    m_definer.copy(mem_root, user_name, host_name);
  }

  void set_definition_string(LEX_STRING &defstr)
  {
    m_definition_string= defstr;
  }
  void reset_thd_mem_root(THD *thd);

  void restore_thd_mem_root(THD *thd);

  /**
    Optimize the code.
  */
  void optimize();

  /**
    Helper used during flow analysis during code optimization.
    See the implementation of <code>opt_mark()</code>.
    @param ip the instruction to add to the leads list
    @param leads the list of remaining paths to explore in the graph that
    represents the code, during flow analysis.
  */
  void add_mark_lead(uint ip, List<sp_instr> *leads);

  inline sp_instr *
  get_instr(uint i)
  {
    sp_instr *ip;

    if (i < m_instr.elements)
      get_dynamic(&m_instr, (uchar*)&ip, i);
    else
      ip= NULL;
    return ip;
  }

#ifdef PROTECT_STATEMENT_MEMROOT
  int has_all_instrs_executed();
  void reset_instrs_executed_counter();
#endif

  /* Add tables used by routine to the table list. */
  bool add_used_tables_to_table_list(THD *thd,
                                     TABLE_LIST ***query_tables_last_ptr,
                                     TABLE_LIST *belong_to_view);

  /**
    Check if this stored routine contains statements disallowed
    in a stored function or trigger, and set an appropriate error message
    if this is the case.
  */
  bool is_not_allowed_in_function(const char *where)
  {
    if (m_flags & CONTAINS_DYNAMIC_SQL)
      my_error(ER_STMT_NOT_ALLOWED_IN_SF_OR_TRG, MYF(0), "Dynamic SQL");
    else if (m_flags & MULTI_RESULTS)
      my_error(ER_SP_NO_RETSET, MYF(0), where);
    else if (m_flags & HAS_SET_AUTOCOMMIT_STMT)
      my_error(ER_SP_CANT_SET_AUTOCOMMIT, MYF(0));
    else if (m_flags & HAS_COMMIT_OR_ROLLBACK)
      my_error(ER_COMMIT_NOT_ALLOWED_IN_SF_OR_TRG, MYF(0));
    else if (m_flags & HAS_SQLCOM_RESET)
      my_error(ER_STMT_NOT_ALLOWED_IN_SF_OR_TRG, MYF(0), "RESET");
    else if (m_flags & HAS_SQLCOM_FLUSH)
      my_error(ER_STMT_NOT_ALLOWED_IN_SF_OR_TRG, MYF(0), "FLUSH");

    return MY_TEST(m_flags &
                  (CONTAINS_DYNAMIC_SQL | MULTI_RESULTS |
                   HAS_SET_AUTOCOMMIT_STMT | HAS_COMMIT_OR_ROLLBACK |
                   HAS_SQLCOM_RESET | HAS_SQLCOM_FLUSH));
  }

#ifndef DBUG_OFF
  int show_routine_code(THD *thd);
#endif

  /*
    This method is intended for attributes of a routine which need
    to propagate upwards to the Query_tables_list of the caller (when
    a property of a sp_head needs to "taint" the calling statement).
  */
  void propagate_attributes(Query_tables_list *prelocking_ctx)
  {
    DBUG_ENTER("sp_head::propagate_attributes");
    /*
      If this routine needs row-based binary logging, the entire top statement
      too (we cannot switch from statement-based to row-based only for this
      routine, as in statement-based the top-statement may be binlogged and
      the substatements not).
    */
    DBUG_PRINT("info", ("lex->get_stmt_unsafe_flags(): 0x%x",
                        prelocking_ctx->get_stmt_unsafe_flags()));
    DBUG_PRINT("info", ("sp_head(%p=%s)->unsafe_flags: 0x%x",
                        this, name()->str, unsafe_flags));
    prelocking_ctx->set_stmt_unsafe_flags(unsafe_flags);
    DBUG_VOID_RETURN;
  }

  sp_pcontext *get_parse_context() { return m_pcont; }

  /*
    Check EXECUTE access:
    - in case of a standalone rotuine, for the routine itself
    - in case of a package routine, for the owner package body
  */
  bool check_execute_access(THD *thd) const;

  virtual sp_package *get_package()
  {
    return NULL;
  }

  virtual void init_psi_share();

protected:

  MEM_ROOT *m_thd_root;		///< Temp. store for thd's mem_root
  THD *m_thd;			///< Set if we have reset mem_root

  sp_pcontext *m_pcont;		///< Parse context
  List<LEX> m_lex;		///< Temp. store for the other lex
  DYNAMIC_ARRAY m_instr;	///< The "instructions"

  enum backpatch_instr_type { GOTO, CPOP, HPOP };
  typedef struct
  {
    sp_label *lab;
    sp_instr *instr;
    backpatch_instr_type instr_type;
  } bp_t;
  List<bp_t> m_backpatch;	///< Instructions needing backpatching
  List<bp_t> m_backpatch_goto; // Instructions needing backpatching (for goto)

  /**
    We need a special list for backpatching of instructions with a continue
    destination (in the case of a continue handler catching an error in
    the test), since it would otherwise interfere with the normal backpatch
    mechanism - e.g. jump_if_not instructions have two different destinations
    which are to be patched differently.
    Since these occur in a more restricted way (always the same "level" in
    the code), we don't need the label.
  */
  List<sp_instr_opt_meta> m_cont_backpatch;
  uint m_cont_level;            // The current cont. backpatch level

  /**
    Multi-set representing optimized list of tables to be locked by this
    routine. Does not include tables which are used by invoked routines.

    @note
    For prelocking-free SPs this multiset is constructed too.
    We do so because the same instance of sp_head may be called both
    in prelocked mode and in non-prelocked mode.
  */
  HASH m_sptabs;

  /**
    Text of the query CREATE PROCEDURE/FUNCTION/TRIGGER/EVENT ...
    used for DDL parsing.
  */
  LEX_STRING m_definition_string;

  bool
  execute(THD *thd, bool merge_da_on_success);

  /**
    Perform a forward flow analysis in the generated code.
    Mark reachable instructions, for the optimizer.
  */
  void opt_mark();

  /**
    Merge the list of tables used by query into the multi-set of tables used
    by routine.
  */
  bool merge_table_list(THD *thd, TABLE_LIST *table, LEX *lex_for_tmp_check);

  /// Put the instruction on the a backpatch list, associated with the label.
  int
  push_backpatch(THD *thd, sp_instr *, sp_label *, List<bp_t> *list,
                 backpatch_instr_type itype);

public:
  /*
    List of lists of Item_trigger_field objects representing all fields in
    old/new version of row in trigger. We use this list of lists for checking
    whenever all such fields are valid at trigger creation time and for binding
    these fields to TABLE object at table open (although for latter pointer
    to table being opened is probably enough).
  */
  SQL_I_List<SQL_I_List<Item_trigger_field> > m_trg_table_fields;

  /**
    The object of the Trigger class corresponding to this sp_head object.
    This data member is set on table's triggers loading at the function
    check_n_load and is used at the method sp_lex_instr::parse_expr
    for accessing to the trigger's table after re-parsing of failed
    trigger's instruction.
  */
  Trigger *m_trg= nullptr;

  /*
    List of Item_trigger_field objects created on parsing
    a current instruction of trigger's body
  */
  SQL_I_List<Item_trigger_field> m_cur_instr_trig_field_items;
}; // class sp_head : public Sql_alloc


class sp_package: public sp_head
{
  bool validate_public_routines(THD *thd, sp_package *spec);
  bool validate_private_routines(THD *thd);
public:
  class LexList: public List<LEX>
  {
  public:
    LexList() { elements= 0; }
    // Find a package routine by a non qualified name
    LEX *find(const LEX_CSTRING &name, enum_sp_type type);
    // Find a package routine by a package-qualified name, e.g. 'pkg.proc'
    LEX *find_qualified(const LEX_CSTRING &name, enum_sp_type type);
    // Check if a routine with the given qualified name already exists
    bool check_dup_qualified(const LEX_CSTRING &name, const Sp_handler *sph)
    {
      if (!find_qualified(name, sph->type()))
        return false;
      my_error(ER_SP_ALREADY_EXISTS, MYF(0), sph->type_str(), name.str);
      return true;
    }
    bool check_dup_qualified(const sp_head *sp)
    {
      return check_dup_qualified(sp->m_name, sp->m_handler);
    }
    void cleanup();
  };
  /*
    The LEX for a new package subroutine is initially assigned to
    m_current_routine. After scanning parameters, return type and chistics,
    the parser detects if we have a declaration or a definition, e.g.:
         PROCEDURE p1(a INT);
      vs
         PROCEDURE p1(a INT) AS BEGIN NULL; END;
    (i.e. either semicolon or the "AS" keyword)
    m_current_routine is then added either to m_routine_implementations,
    or m_routine_declarations, and then m_current_routine is set to NULL.
  */
  LEX *m_current_routine;
  LexList m_routine_implementations;
  LexList m_routine_declarations;

  LEX *m_top_level_lex;
  sp_rcontext *m_rcontext;
  uint m_invoked_subroutine_count;
  bool m_is_instantiated;
  bool m_is_cloning_routine;

private:
  sp_package(MEM_ROOT *mem_root,
             LEX *top_level_lex,
             const sp_name *name,
             const Sp_handler *sph);
  ~sp_package();
public:
  static sp_package *create(LEX *top_level_lex, const sp_name *name,
                            const Sp_handler *sph, MEM_ROOT *sp_mem_root);

  bool add_routine_declaration(LEX *lex)
  {
    return m_routine_declarations.check_dup_qualified(lex->sphead) ||
           m_routine_declarations.push_back(lex, &main_mem_root);
  }
  bool add_routine_implementation(LEX *lex)
  {
    return m_routine_implementations.check_dup_qualified(lex->sphead) ||
           m_routine_implementations.push_back(lex, &main_mem_root);
  }
  sp_package *get_package() override { return this; }
  void init_psi_share() override;
  bool is_invoked() const override
  {
    /*
      Cannot flush a package out of the SP cache when:
      - its initialization block is running
      - one of its subroutine is running
    */
    return sp_head::is_invoked() || m_invoked_subroutine_count > 0;
  }
  sp_variable *find_package_variable(const LEX_CSTRING *name) const
  {
    /*
      sp_head::m_pcont is a special level for routine parameters.
      Variables declared inside CREATE PACKAGE BODY reside in m_children.at(0).
    */
    sp_pcontext *ctx= m_pcont->child_context(0);
    return ctx ? ctx->find_variable(name, true) : NULL;
  }
  bool validate_after_parser(THD *thd);
  bool instantiate_if_needed(THD *thd);
};


bool check_show_routine_access(THD *thd, sp_head *sp, bool *full_access);
bool check_db_routine_access(THD *thd, privilege_t privilege,
                             const char *db, const char *name,
                             const Sp_handler *sph,
                             bool no_errors);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
bool
sp_change_security_context(THD *thd, sp_head *sp,
                           Security_context **backup);
void
sp_restore_security_context(THD *thd, Security_context *backup);

bool
set_routine_security_ctx(THD *thd, sp_head *sp, Security_context **save_ctx);
#endif /* NO_EMBEDDED_ACCESS_CHECKS */

TABLE_LIST *
sp_add_to_query_tables(THD *thd, LEX *lex,
		       const LEX_CSTRING *db, const LEX_CSTRING *name,
                       thr_lock_type locktype,
                       enum_mdl_type mdl_type);

/**
  @} (end of group Stored_Routines)
*/

#endif /* _SP_HEAD_H_ */
