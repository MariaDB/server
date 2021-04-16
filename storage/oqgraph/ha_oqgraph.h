/* Copyright (C) 2007-2013 Arjen G Lentz & Antony T Curtis for Open Query
   Portions of this file copyright (C) 2000-2006 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* ======================================================================
   Open Query Graph Computation Engine, based on a concept by Arjen Lentz
   v3 implementation by Antony Curtis, Arjen Lentz, Andrew McDonnell
   For more information, documentation, support, enhancement engineering,
   see http://openquery.com/graph or contact graph@openquery.com
   ======================================================================
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "handler.h"
#include "table.h"

typedef struct oqgraph_info_st OQGRAPH_INFO;
typedef uchar byte;

namespace open_query
{
  struct row;
  class oqgraph;
  class oqgraph_share;
}

/* class for the the Open Query Graph handler */

class ha_oqgraph: public handler
{
  TABLE_SHARE share[1];
  bool have_table_share;
  TABLE edges[1];
  Field *origid;
  Field *destid;
  Field *weight;

  open_query::oqgraph_share *graph_share;
  open_query::oqgraph *graph;

  int fill_record(byte*, const open_query::row&);

public:
#if MYSQL_VERSION_ID >= 50100
  ha_oqgraph(handlerton *hton, TABLE_SHARE *table);
  ulonglong table_flags() const;
#else
  ha_oqgraph(TABLE *table);
  Table_flags table_flags() const;
#endif
  virtual ~ha_oqgraph();
  const char *index_type(uint inx)
  {
    return "HASH";
  }
  /* Rows also use a fixed-size format */
  enum row_type get_row_type() const { return ROW_TYPE_FIXED; }
  ulong index_flags(uint inx, uint part, bool all_parts) const;
  const char **bas_ext() const;
  uint max_supported_keys()          const { return MAX_KEY; }
  uint max_supported_key_part_length() const { return MAX_KEY_LENGTH; }
  double scan_time() { return (double) 1000000000; }
  double read_time(uint index, uint ranges, ha_rows rows)
  { return 1; }

  // Doesn't make sense to change the engine on a virtual table.
  virtual bool can_switch_engines() { return false; }

  int open(const char *name, int mode, uint test_if_locked);
  int close(void);
  int write_row(const byte * buf);
  int update_row(const uchar * old_data, const uchar * new_data);
  int delete_row(const byte * buf);
  int index_read(byte * buf, const byte * key,
		 uint key_len, enum ha_rkey_function find_flag);
  int index_read_idx(byte * buf, uint idx, const byte * key,
		     uint key_len, enum ha_rkey_function find_flag);
  int index_next_same(byte * buf, const byte * key, uint key_len);
  int rnd_init(bool scan);
  int rnd_next(byte *buf);
  int rnd_pos(byte * buf, byte *pos);
  void position(const byte *record);
  int info(uint);
  int extra(enum ha_extra_function operation);
  int external_lock(THD *thd, int lock_type);
  int delete_all_rows(void);
  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key, page_range *pages);
  int delete_table(const char *from);
  int rename_table(const char * from, const char * to);
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info);
  void update_create_info(HA_CREATE_INFO *create_info);

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
			     enum thr_lock_type lock_type);
  int cmp_ref(const byte *ref1, const byte *ref2);

  bool get_error_message(int error, String* buf);

  void fprint_error(const char* fmt, ...);

#if MYSQL_VERSION_ID < 100000
  // Allow compatibility for build with 5.5.32
  virtual const char *table_type() const { return hton_name(ht)->str; }
#endif

  my_bool register_query_cache_table(THD *thd, const char *table_key,
                                     uint key_length,
                                     qc_engine_callback
                                     *engine_callback,
                                     ulonglong *engine_data)
  {
    /* 
      Do not put data from OQGRAPH tables into query cache (because there 
      is no way to tell whether the data in the backing table has changed or 
      not)
    */
    return FALSE;
  }

private:

  // Various helper functions
  int oqgraph_check_table_structure (TABLE *table_arg); 
  bool validate_oqgraph_table_options();
  void update_key_stats();
  String error_message;
};
