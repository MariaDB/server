#ifndef SP_RCONTEXT_HANDLER_INCLUDED
#define SP_RCONTEXT_HANDLER_INCLUDED

/* Copyright (c) 2009, 2025, MariaDB Corporation.

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


class sp_rcontext;
class sp_cursor; 

/**
  A helper class to handle the run time context of various components of SP:
  Variables:
  - local SP variables and SP parameters
  - PACKAGE BODY routine variables
  - (there will be more kinds in the future)
  Cursors:
  - static cursors
  - SYS_REFCURSORs
*/

class Sp_rcontext_handler
{
public:
  /*
    Get a cursor using its direct address or a reference.

    addr_or_ref.deref_rcontext_handler()==nullptr means
    that addr_or_ref contains the direct address of the cursor.

    A not-null addr_or_ref.deref_rcontext_handler() value
    means that it is a reference and should be dereferenced.
  */
  static sp_cursor *get_cursor(THD *thd,
                               const sp_rcontext_ref &addr_or_ref)
  {
    return addr_or_ref.deref_rcontext_handler() ?
     addr_or_ref.deref_rcontext_handler()->get_cursor_by_ref(thd, addr_or_ref,
                                                             false) :
     addr_or_ref.rcontext_handler()->get_cursor(thd, addr_or_ref.offset());
  }
  /*
    Get a cursor using its direct address or a reference.
    If the cursor is not found or is not open,
    the ER_SP_CURSOR_NOT_OPEN error is raised.
  */
  static sp_cursor *get_open_cursor_or_error(THD *thd,
                                          const sp_rcontext_ref &addr_or_ref);

  virtual ~Sp_rcontext_handler() = default;


  /**
    A prefix used for SP variable names in queries:
    - EXPLAIN EXTENDED
    - SHOW PROCEDURE CODE
    Local variables and SP parameters have empty prefixes.
    Package body variables are marked with a special prefix.
    This improves readability of the output of these queries,
    especially when a local variable or a parameter has the same
    name with a package body variable.
  */
  virtual const LEX_CSTRING *get_name_prefix() const= 0;
  /**
    At execution time THD->spcont points to the run-time context (sp_rcontext)
    of the currently executed routine.
    Local variables store their data in the sp_rcontext pointed by thd->spcont.
    Package body variables store data in separate sp_rcontext that belongs
    to the package.
    This method provides access to the proper sp_rcontext structure,
    depending on the SP variable kind.
  */
  virtual sp_rcontext *get_rcontext(sp_rcontext *ctx) const= 0;
  virtual Item_field *get_variable(THD *thd, uint offset) const= 0;
  virtual sp_cursor *get_cursor(THD *thd, uint offset) const= 0;
  virtual sp_cursor *get_cursor_by_ref(THD *thd,
                                       const sp_rcontext_addr &ref,
                                       bool for_open) const= 0;
};


class Sp_rcontext_handler_local final :public Sp_rcontext_handler
{
public:
  const LEX_CSTRING *get_name_prefix() const override;
  sp_rcontext *get_rcontext(sp_rcontext *ctx) const override;
  Item_field *get_variable(THD *thd, uint offset) const override;
  sp_cursor *get_cursor(THD *thd, uint offset) const override;
  sp_cursor *get_cursor_by_ref(THD *thd, const sp_rcontext_addr &ref,
                               bool for_open) const override
  {
    DBUG_ASSERT(0); // References to static cursors are not supported
    return nullptr;
  }
};


class Sp_rcontext_handler_package_body final :public Sp_rcontext_handler
{
public:
  const LEX_CSTRING *get_name_prefix() const override;
  sp_rcontext *get_rcontext(sp_rcontext *ctx) const override;
  Item_field *get_variable(THD *thd, uint offset) const override;
  sp_cursor *get_cursor(THD *thd, uint offset) const override
  {
    /*
      There are no package body wide static cursors yet:
      MDEV-36053 Syntax error on a CURSOR..IS declaration in PACKAGE BODY
    */
    DBUG_ASSERT(0);
    return nullptr;
  }
  sp_cursor *get_cursor_by_ref(THD *thd, const sp_rcontext_addr &ref,
                               bool for_open) const override
  {
    DBUG_ASSERT(0); // References to static cursors are not supported
    return nullptr;
  }
};


class Sp_rcontext_handler_statement final :public Sp_rcontext_handler
{
public:
  const LEX_CSTRING *get_name_prefix() const override;
  sp_rcontext *get_rcontext(sp_rcontext *ctx) const override
  {
    DBUG_ASSERT(0); // There are no session wide SP variables yet.
    return nullptr;
  }
  Item_field *get_variable(THD *thd, uint offset) const override
  {
    DBUG_ASSERT(0); // There are no session wide SP variables yet.
    return nullptr;
  }
  sp_cursor *get_cursor(THD *thd, uint offset) const override;
  sp_cursor *get_cursor_by_ref(THD *thd, const sp_rcontext_addr &ref,
                               bool for_open) const override;
};


extern MYSQL_PLUGIN_IMPORT
  Sp_rcontext_handler_local sp_rcontext_handler_local;


extern MYSQL_PLUGIN_IMPORT
  Sp_rcontext_handler_package_body sp_rcontext_handler_package_body;


extern MYSQL_PLUGIN_IMPORT
  Sp_rcontext_handler_statement sp_rcontext_handler_statement;

#endif  // SP_RCONTEXT_HANDLER_INCLUDED
