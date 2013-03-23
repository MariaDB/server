/* Copyright (C) Olivier Bertrand 2004 - 2011

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/** @file ha_connect.h

    @brief
  The ha_connect engine is a prototype storage engine to access external data.

   @see
  /sql/handler.h and /storage/connect/ha_connect.cc
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface     /* gcc class implementation */
#endif

/****************************************************************************/
/*  Structures used to pass info between CONNECT and ha_connect.            */
/****************************************************************************/
typedef struct _create_xinfo {
  char *Type;                               /* Retrieved from table comment */
  char *Filename;                            /* Set if not standard          */
  char *IndexFN;                            /* Set if not standard          */
  ulonglong Maxrows;                        /* Estimated max nb of rows     */
  ulong Lrecl;                              /* Set if not default           */
  ulong Elements;                            /* Number of lines in blocks    */
  bool  Fixed;                              /* False for DOS type           */
  void *Pcf;                                /* To list of columns           */
  void *Pxdf;                               /* To list of indexes           */
} CRXINFO, *PCXF;

typedef struct _xinfo {
  ulonglong data_file_length;                /* Length of data file          */
  ha_rows   records;                        /* Records in table             */
  ulong     mean_rec_length;                /* Physical record length       */
  char     *data_file_name;                  /* Physical file name           */
} XINFO, *PXF;

typedef class user_connect *PCONNECT;
typedef struct ha_table_option_struct TOS, *PTOS;
typedef struct ha_field_option_struct FOS, *PFOS; 

/** @brief
  CONNECT_SHARE is a structure that will be shared among all open handlers.
  This example implements the minimum of what you will probably need.
*/
typedef struct st_connect_share {
  char *table_name;
  uint  table_name_length, use_count;
  mysql_mutex_t mutex;
  THR_LOCK lock;
#if !defined(MARIADB)
  PTOS table_options;
  PFOS field_options;
#endif   // !MARIADB
} CONNECT_SHARE;

typedef class ha_connect *PHC;

/** @brief
  Class definition for the storage engine
*/
class ha_connect: public handler
{
  THR_LOCK_DATA lock;      ///< MySQL lock
  CONNECT_SHARE *share;        ///< Shared lock info

public:
  ha_connect(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_connect();

  // CONNECT Implementation
  static   bool connect_init(void);
  static   bool connect_end(void);
  char    *GetStringOption(char *opname, char *sdef= NULL);
  PTOS     GetTableOptionStruct(TABLE *table_arg);
  bool     GetBooleanOption(char *opname, bool bdef);
  int      GetIntegerOption(char *opname);
  bool     SetIntegerOption(char *opname, int n);
  PFOS     GetFieldOptionStruct(Field *fp);
  void    *GetColumnOption(void *field, PCOLINFO pcf);
  PIXDEF   GetIndexInfo(int n);
  const char *GetDBName(const char *name);
  const char *GetTableName(void);
  int      GetColNameLen(Field *fp);
  char    *GetColName(Field *fp);
  void     AddColName(char *cp, Field *fp);
  TABLE    *GetTable(void) {return table;}

  PCONNECT GetUser(THD *thd);
  PGLOBAL  GetPlug(THD *thd);
  PTDB     GetTDB(PGLOBAL g);
  bool     OpenTable(PGLOBAL g, bool del= false);
  bool     IsOpened(void); 
  int      CloseTable(PGLOBAL g);
  int      MakeRecord(char *buf);
  int      ScanRecord(PGLOBAL g, uchar *buf);
  int      CheckRecord(PGLOBAL g, const uchar *oldbuf, uchar *newbuf);
  int      ReadIndexed(uchar *buf, OPVAL op, const uchar* key= NULL,
                                             uint key_len= 0);

  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const {return "CONNECT";}

  /** @brief
    The name of the index type that will be used for display.
    Don't implement this method unless you really have indexes.
   */
  const char *index_type(uint inx) { return "XPLUG"; }

  /** @brief
    The file extensions.
   */
  const char **bas_ext() const;

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const
  {
    return (HA_NO_TRANSACTIONS | HA_REC_NOT_IN_SEQ | HA_HAS_RECORDS |
            /*HA_NO_AUTO_INCREMENT |*/ HA_NO_PREFIX_CHAR_KEYS |
#if defined(MARIADB)
            HA_CAN_VIRTUAL_COLUMNS |
#endif   // MARIADB
            HA_NULL_IN_KEY | HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE);
  }

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

      @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MySQL wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint inx, uint part, bool all_parts) const
  {
    return HA_READ_NEXT | HA_READ_RANGE;
  }

  /** @brief
    unireg.cc will call max_supported_record_length(), max_supported_keys(),
    max_supported_key_parts(), uint max_supported_key_length()
    to make sure that the storage engine can handle the data it is about to
    send. Return *real* limits of your storage engine here; MySQL will do
    min(your_limits, MySQL_limits) automatically.
   */
  uint max_supported_record_length() const { return HA_MAX_REC_LENGTH; }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_keys()          const { return 10; }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_key_parts()     const { return 10; }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_key_length()    const { return 255; }

  /** @brief
    Called in test_quick_select to determine if indexes should be used.
  */
  virtual double scan_time() { return (double) (stats.records+stats.deleted) / 20.0+10; }

  /** @brief
    This method will never be called if you do not implement indexes.
  */
  virtual double read_time(uint, uint, ha_rows rows)
    { return (double) rows /  20.0+1; }

  /*
    Everything below are methods that we implement in ha_connect.cc.

    Most of these methods are not obligatory, skip them and
    MySQL will treat them as not implemented
  */
  virtual bool get_error_message(int error, String *buf);

 /**
   Push condition down to the table handler.

   @param  cond   Condition to be pushed. The condition tree must not be
                  modified by the by the caller.

   @return
     The 'remainder' condition that caller must use to filter out records.
     NULL means the handler will not return rows that do not match the
     passed condition.

   @note
   The pushed conditions form a stack (from which one can remove the
   last pushed condition using cond_pop).
   The table handler filters out rows using (pushed_cond1 AND pushed_cond2 
   AND ... AND pushed_condN)
   or less restrictive condition, depending on handler's capabilities.

   handler->ha_reset() call empties the condition stack.
   Calls to rnd_init/rnd_end, index_init/index_end etc do not affect the
   condition stack.
 */ 
virtual const COND *cond_push(const COND *cond);
PFIL  CheckCond(PGLOBAL g, PFIL filp, AMT tty, Item *cond);
const char *GetValStr(OPVAL vop, bool neg);

 /**
   Number of rows in table. It will only be called if
   (table_flags() & (HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT)) != 0
 */
 virtual ha_rows records();

 /** @brief
    We implement this in ha_connect.cc; it's a required method.
  */
  int open(const char *name, int mode, uint test_if_locked);    // required

  /** @brief
    We implement this in ha_connect.cc; it's a required method.
  */
  int close(void);                                              // required

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int write_row(uchar *buf);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int update_row(const uchar *old_data, uchar *new_data);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int delete_row(const uchar *buf);

  // Added to the connect handler
  int index_init(uint idx, bool sorted);
  int index_end();
  int index_read(uchar * buf, const uchar * key, uint key_len,
                              enum ha_rkey_function find_flag);
  int index_next_same(uchar *buf, const uchar *key, uint keylen);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
//int index_read_map(uchar *buf, const uchar *key,
//                   key_part_map keypart_map, enum ha_rkey_function find_flag);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_next(uchar *buf);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
//int index_prev(uchar *buf);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_first(uchar *buf);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
//int index_last(uchar *buf);

  /** @brief
    Unlike index_init(), rnd_init() can be called two consecutive times
    without rnd_end() in between (it only makes sense if scan=1). In this
    case, the second call should prepare for the new table scan (e.g if
    rnd_init() allocates the cursor, the second call should position the
    cursor to the start of the table; no need to deallocate and allocate
    it again. This is a required method.
  */
  int rnd_init(bool scan);                                      //required
  int rnd_end();
  int rnd_next(uchar *buf);                                     ///< required
  int rnd_pos(uchar *buf, uchar *pos);                          ///< required
  void position(const uchar *record);                           ///< required
  int info(uint);                                               ///< required
  int extra(enum ha_extra_function operation);
  int external_lock(THD *thd, int lock_type);                   ///< required
  int delete_all_rows(void);
  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key);
  /**
    These methods can be overridden, but their default implementation
    provide useful functionality.
  */
  int rename_table(const char *from, const char *to);
  /**
    Delete a table in the engine. Called for base as well as temporary
    tables.
  */
  int delete_table(const char *name);
  /**
    Called by delete_table and rename_table
  */
  int delete_or_rename_table(const char *from, const char *to);
#if defined(MARIADB)
  bool pre_create(THD *thd, HA_CREATE_INFO *crt_info, void *alt_info);
#endif   // MARIADB
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info);                      ///< required
  bool check_if_incompatible_data(HA_CREATE_INFO *info,
                                  uint table_changes);

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);     ///< required
  int optimize(THD* thd, HA_CHECK_OPT* check_opt);

protected:
  bool check_privileges(THD *thd, PTOS options);
  char *GetListOption(const char *opname, const char *oplist, const char *def= NULL);
#if defined(MARIADB)
  char *encode(PGLOBAL g, char *cnm);
  bool  add_fields(THD *thd, void *alter_info, 
           LEX_STRING *field_name,
           enum_field_types type,
           char *length, char *decimals,
           uint type_modifier,
//         Item *default_value, Item *on_update_value,
           LEX_STRING *comment,
//         char *change,
//         List<String> *interval_list, 
           CHARSET_INFO *cs,
//         uint uint_geom_type,
           void *vcol_info,
           engine_option_value *create_options);
#endif   // MARIADB

  // Members
  static ulong  num;                  // Tracable handler number
  PCONNECT      xp;                   // To user_connect associated class
  ulong         hnum;                 // The number of this handler
  query_id_t    valid_query_id;       // The one when tdbp was allocated
  query_id_t    creat_query_id;       // The one when handler was allocated
  PTDB          tdbp;                 // To table class object
  PVAL          sdvalin;              // Used to convert date values
  PVAL          sdvalout;             // Used to convert date values
  bool          istable;              // True for table handler
//char          tname[64];            // The table name
  MODE          xmod;                 // Table mode
  XINFO         xinfo;                // The table info structure
  bool          valid_info;           // True if xinfo is valid
  bool          stop;                 // Used when creating index
  bool          createas;             // True for CREATE TABLE ... AS SELECT
  int           indexing;             // Type of indexing for CONNECT
#if !defined(MARIADB)
  PTOS          table_options;
  PFOS          field_options;
#endif   // !MARIADB
  THR_LOCK_DATA lock_data;

public:
  TABLE_SHARE  *tshp;                 // Used by called tables 
  char   *data_file_name;
  char   *index_file_name;
  uint    int_table_flags;            // Inherited from MyISAM
  bool    enable_activate_all_index;  // Inherited from MyISAM
};  // end of ha_connect class definition
