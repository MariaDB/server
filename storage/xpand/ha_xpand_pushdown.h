/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/
#ifndef _ha_xpand_pushdown_h
#define _ha_xpand_pushdown_h

#include "select_handler.h"
#include "derived_handler.h"
#include "sql_select.h"

/*@brief base_handler class*/
/***********************************************************
 * DESCRIPTION:
 * To be described
 ************************************************************/
class ha_xpand_base_handler
{
  // To simulate abstract class
protected:
  ha_xpand_base_handler(): thd__(0),table__(0) {}
  ~ha_xpand_base_handler() {}

  // Copies of pushdown handlers attributes
  // to use them in shared methods.
  THD *thd__;
  TABLE *table__;
  // The bitmap used to sent
  MY_BITMAP scan_fields;
  // Structures to unpack RBR rows from XPD BE
  rpl_group_info *rgi;
  // XPD BE scan operation reference
  xpand_connection_cursor *scan;
};

/*@brief select_handler class*/
/***********************************************************
 * DESCRIPTION:
 *  select_handler API methods. Could be used by the server
 *  tp pushdown the whole query described by SELECT_LEX.
 *  More details in server/sql/select_handler.h
 *  sel semantic tree for the query in SELECT_LEX.
 ************************************************************/
class ha_xpand_select_handler:
    private ha_xpand_base_handler,
    public select_handler
{
public:
  ha_xpand_select_handler(THD* thd_arg, SELECT_LEX* sel,
                          xpand_connection_cursor *scan);
  ~ha_xpand_select_handler();

  int init_scan() override;
  int next_row() override;
  int end_scan() override;
  void print_error(int, unsigned long) override {}
};

/*@brief derived_handler class*/
/***********************************************************
 * DESCRIPTION:
 *  derived_handler API methods. Could be used by the server
 *  tp pushdown the whole query described by SELECT_LEX.
 *  More details in server/sql/derived_handler.h
 *  sel semantic tree for the query in SELECT_LEX.
 ************************************************************/
class ha_xpand_derived_handler:
    private ha_xpand_base_handler,
    public derived_handler
{
public:
  ha_xpand_derived_handler(THD* thd_arg, SELECT_LEX* sel,
                           xpand_connection_cursor *scan);
  ~ha_xpand_derived_handler();

  int init_scan() override;
  int next_row() override;
  int end_scan() override;
  void print_error(int, unsigned long) override {}
};

select_handler *create_xpand_select_handler(THD* thd, SELECT_LEX* select_lex);
derived_handler *create_xpand_derived_handler(THD* thd, TABLE_LIST *derived);

#endif
