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
 *  sel in the constructor is the semantic tree for the query.
 *  Methods:
 *   init_scan - get plan and send it to ExeMgr. Get the execution result.
 *   next_row - get a row back from sm.
 *   end_scan - finish and clean the things up.
 ************************************************************/
class ha_clustrixdb_select_handler: public select_handler
{
  public:
    ha_clustrixdb_select_handler(THD* thd_arg, SELECT_LEX* sel,
      clustrix_connection* clustrix_net);
    ~ha_clustrixdb_select_handler();

    int init_scan();
    int next_row();
    int end_scan();
    void print_error(int, unsigned long);

  private:
    clustrix_connection *clustrix_net;
    rpl_group_info *rgi;
    ulonglong scan_refid;
    bool has_hidden_key;

};

#endif
