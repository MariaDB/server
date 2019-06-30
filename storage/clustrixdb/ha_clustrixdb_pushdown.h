/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/
#ifndef _ha_clustrixdb_pushdown_h
#define _ha_clustrixdb_pushdown_h

#include "select_handler.h"
#include "derived_handler.h"
#include "sql_select.h"

/*@brief base_handler class*/
/***********************************************************
 * DESCRIPTION:
 * To be described
 ************************************************************/
class ha_clustrixdb_base_handler
{
  // To simulate abstract class
  protected:
    ha_clustrixdb_base_handler(): thd__(0),table__(0) {}
    ~ha_clustrixdb_base_handler() {}

    // Copies of pushdown handlers attributes
    // to use them in shared methods.
    THD *thd__;
    TABLE *table__;
    // The bitmap used to sent
    MY_BITMAP scan_fields;
    // Structures to unpack RBR rows from CLX BE
    rpl_group_info *rgi;
    Relay_log_info *rli;
    RPL_TABLE_LIST *rpl_table_list;
    // CLX BE scan operation reference
    ulonglong scan_refid;
    // To unpack rows from CLX BE
    void add_current_table_to_rpl_table_list();
    void remove_current_table_from_rpl_table_list();
};

/*@brief select_handler class*/
/***********************************************************
 * DESCRIPTION:
 *  select_handler API methods. Could be used by the server
 *  tp pushdown the whole query described by SELECT_LEX.
 *  More details in server/sql/select_handler.h
 *  sel semantic tree for the query in SELECT_LEX.
 ************************************************************/
class ha_clustrixdb_select_handler: 
    private ha_clustrixdb_base_handler, 
    public select_handler
{
  public:
    ha_clustrixdb_select_handler(THD* thd_arg, SELECT_LEX* sel,
      ulonglong scan_refid);
    ~ha_clustrixdb_select_handler();

    int init_scan();
    int next_row();
    int end_scan();
    void print_error(int, unsigned long) {}
};

/*@brief derived_handler class*/
/***********************************************************
 * DESCRIPTION:
 *  derived_handler API methods. Could be used by the server
 *  tp pushdown the whole query described by SELECT_LEX.
 *  More details in server/sql/derived_handler.h
 *  sel semantic tree for the query in SELECT_LEX.
 ************************************************************/
class ha_clustrixdb_derived_handler:
    private ha_clustrixdb_base_handler,
    public derived_handler
{
  public:
    ha_clustrixdb_derived_handler(THD* thd_arg, SELECT_LEX* sel,
      ulonglong scan_refid);
    ~ha_clustrixdb_derived_handler();

    int init_scan();
    int next_row();
    int end_scan();
    void print_error(int, unsigned long) {}
};



#endif
