/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/
#ifndef _ha_clustrixdb_pushdown_h
#define _ha_clustrixdb_pushdown_h

#include "select_handler.h"
#include "sql_select.h"

/*@brief select_handler class*/
/***********************************************************
 * DESCRIPTION:
 *  select_handler API methods. Could be used by the server
 *  tp pushdown the whole query described by SELECT_LEX.
 *  More details in server/sql/select_handler.h
 *  sel semantic tree for the query in SELECT_LEX.
 ************************************************************/
class ha_clustrixdb_select_handler: public select_handler
{
  public:
    ha_clustrixdb_select_handler(THD* thd_arg, SELECT_LEX* sel,
      clustrix_connection* clustrix_net, ulonglong scan_refid);
    ~ha_clustrixdb_select_handler();

    int init_scan();
    int next_row();
    int end_scan();
    void print_error(int, unsigned long);

    MY_BITMAP scan_fields;
  private:
    clustrix_connection *clustrix_net;
    rpl_group_info *rgi;
    Relay_log_info *rli;
    RPL_TABLE_LIST *rpl_table_list;
    ulonglong scan_refid;
    void add_current_table_to_rpl_table_list();
    void remove_current_table_from_rpl_table_list();
};

#endif
