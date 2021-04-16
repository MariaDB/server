/* Copyright (C) MariaDB Corporation Ab

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

/** @file ha_connect.h
	Author Olivier Bertrand

    @brief
  The ha_connect engine is a prototype storage engine to access external data.

   @see
  /sql/handler.h and /storage/connect/ha_connect.cc
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface     /* gcc class implementation */
#endif

/****************************************************************************/
/*  mycat.h contains the TOS, PTOS, ha_table_option_struct declarations.    */
/****************************************************************************/
#include "mycat.h"

#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
bool MongoEnabled(void);
#endif   // JAVA_SUPPORT || CMGO_SUPPORT

/****************************************************************************/
/*  Structures used to pass info between CONNECT and ha_connect.            */
/****************************************************************************/
typedef struct _create_xinfo {
  char *Type;                               /* Retrieved from table comment */
  char *Filename;                           /* Set if not standard          */
  char *IndexFN;                            /* Set if not standard          */
  ulonglong Maxrows;                        /* Estimated max nb of rows     */
  ulong Lrecl;                              /* Set if not default           */
  ulong Elements;                           /* Number of lines in blocks    */
  bool  Fixed;                              /* False for DOS type           */
  void *Pcf;                                /* To list of columns           */
  void *Pxdf;                               /* To list of indexes           */
} CRXINFO, *PCXF;

typedef struct _xinfo {
  ulonglong data_file_length;               /* Length of data file          */
  ha_rows   records;                        /* Records in table             */
  ulong     mean_rec_length;                /* Physical record length       */
  char     *data_file_name;                 /* Physical file name           */
} XINFO, *PXF;

class XCHK : public BLOCK {
public:
  XCHK(void) {oldsep= newsep= false;
              oldopn= newopn= NULL;
              oldpix= newpix= NULL;}

  inline char *SetName(PGLOBAL g, PCSZ name) {return PlugDup(g, name);}

  bool         oldsep;              // Sepindex before create/alter
  bool         newsep;              // Sepindex after create/alter
  char        *oldopn;              // Optname before create/alter
  char        *newopn;              // Optname after create/alter
  PIXDEF       oldpix;              // The indexes before create/alter
  PIXDEF       newpix;              // The indexes after create/alter
}; // end of class XCHK

typedef class XCHK *PCHK;
typedef class user_connect *PCONNECT;
typedef struct ha_field_option_struct FOS, *PFOS;
typedef struct ha_index_option_struct XOS, *PXOS;

extern handlerton *connect_hton;

/**
  structure for CREATE TABLE options (table options)

  These can be specified in the CREATE TABLE:
  CREATE TABLE ( ... ) {...here...}

	------ Was moved to mycat.h ------
	*/

/**
  structure for CREATE TABLE options (field options)

  These can be specified in the CREATE TABLE per field:
  CREATE TABLE ( field ... {...here...}, ... )
*/
struct ha_field_option_struct
{
  ulonglong offset;
  ulonglong freq;
  ulonglong fldlen;
  uint opt;
  const char *dateformat;
  const char *fieldformat;
	const char* jsonpath;
	const char* xmlpath;
	char *special;
};

/*
  index options can be declared similarly
  using the ha_index_option_struct structure.

  Their values can be specified in the CREATE TABLE per index:
  CREATE TABLE ( field ..., .., INDEX .... *here*, ... )
*/
struct ha_index_option_struct
{
  bool dynamic;
  bool mapped;
};

/** @brief
  CONNECT_SHARE is a structure that will be shared among all open handlers.
  This example implements the minimum of what you will probably need.
*/
class CONNECT_SHARE : public Handler_share {
public:
  mysql_mutex_t mutex;
  THR_LOCK lock;
  CONNECT_SHARE()
  {
    thr_lock_init(&lock);
  }
  ~CONNECT_SHARE()
  {
    thr_lock_delete(&lock);
    mysql_mutex_destroy(&mutex);
  }
};

typedef class ha_connect *PHC;

/** @brief
  Class definition for the storage engine
*/
class ha_connect final : public handler
{
  THR_LOCK_DATA lock;      ///< MySQL lock
  CONNECT_SHARE *share;        ///< Shared lock info
  CONNECT_SHARE *get_share();

protected:
  char *PlugSubAllocStr(PGLOBAL g, void *memp, const char *str, size_t length)
  {
    char *ptr= (char*)PlgDBSubAlloc(g, memp, length + 1);

    if (ptr) {
      memcpy(ptr, str, length);
      ptr[length]= '\0';
      } // endif ptr

    return ptr;
  } // end of PlugSubAllocStr

public:
  ha_connect(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_connect();

  // CONNECT Implementation
//static   bool connect_init(void);
//static   bool connect_end(void);
  TABTYPE  GetRealType(PTOS pos= NULL);
  char    *GetRealString(PCSZ s);
	PCSZ     GetStringOption(PCSZ opname, PCSZ sdef= NULL);
  PTOS     GetTableOptionStruct(TABLE_SHARE *s= NULL);
  bool     GetBooleanOption(PCSZ opname, bool bdef);
  bool     SetBooleanOption(PCSZ opname, bool b);
  int      GetIntegerOption(PCSZ opname);
  bool     GetIndexOption(KEY *kp, PCSZ opname);
  bool     CheckString(PCSZ str1, PCSZ str2);
  bool     SameString(TABLE *tab, PCSZ opn);
  bool     SetIntegerOption(PCSZ opname, int n);
  bool     SameInt(TABLE *tab, PCSZ opn);
  bool     SameBool(TABLE *tab, PCSZ opn);
  bool     FileExists(const char *fn, bool bf);
  bool     NoFieldOptionChange(TABLE *tab);
  PFOS     GetFieldOptionStruct(Field *fp);
  void    *GetColumnOption(PGLOBAL g, void *field, PCOLINFO pcf);
  PXOS     GetIndexOptionStruct(KEY *kp);
  PIXDEF   GetIndexInfo(TABLE_SHARE *s= NULL);
  bool     CheckVirtualIndex(TABLE_SHARE *s);
  PCSZ     GetDBName(PCSZ name);
  PCSZ     GetTableName(void);
  char    *GetPartName(void);
//int      GetColNameLen(Field *fp);
//char    *GetColName(Field *fp);
//void     AddColName(char *cp, Field *fp);
  TABLE   *GetTable(void) {return table;}
  bool     IsSameIndex(PIXDEF xp1, PIXDEF xp2);
  bool     IsPartitioned(void);
  bool     IsUnique(uint n);
  PCSZ     GetDataPath(void) {return datapath;}

  bool     SetDataPath(PGLOBAL g, PCSZ path);
  PTDB     GetTDB(PGLOBAL g);
  int      OpenTable(PGLOBAL g, bool del= false);
  bool     CheckColumnList(PGLOBAL g);
  bool     IsOpened(void);
  int      CloseTable(PGLOBAL g);
  int      MakeRecord(char *buf);
  int      ScanRecord(PGLOBAL g, const uchar *buf);
  int      CheckRecord(PGLOBAL g, const uchar *oldbuf, const uchar *newbuf);
	int      ReadIndexed(uchar *buf, OPVAL op, const key_range *kr= NULL);
	bool     IsIndexed(Field *fp);
  bool     MakeKeyWhere(PGLOBAL g, PSTRG qry, OPVAL op, char q,
                                   const key_range *kr);
//inline char *Strz(LEX_STRING &ls);
	key_range start_key;


  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const {return "CONNECT";}

  /** @brief
    The name of the index type that will be used for display.
    Don't implement this method unless you really have indexes.
   */
  const char *index_type(uint inx);

  /** @brief
    The file extensions.
   */
//const char **bas_ext() const;

 /**
    Check if a storage engine supports a particular alter table in-place
    @note Called without holding thr_lock.c lock.
 */
 virtual enum_alter_inplace_result
 check_if_supported_inplace_alter(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info);

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const;

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

      @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MySQL wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint inx, uint part, bool all_parts) const;

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
PCFIL CheckCond(PGLOBAL g, PCFIL filp, const Item *cond);
const char *GetValStr(OPVAL vop, bool neg);
PFIL  CondFilter(PGLOBAL g, Item *cond);
//PFIL  CheckFilter(PGLOBAL g);

/** admin commands - called from mysql_admin_table */
virtual int check(THD* thd, HA_CHECK_OPT* check_opt);

 /**
   Number of rows in table. It will only be called if
   (table_flags() & (HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT)) != 0
 */
 virtual ha_rows records();

 /**
   Type of table for caching query
   CONNECT should not use caching because its tables are external
   data prone to me modified out of MariaDB
 */
 virtual uint8 table_cache_type(void)
 {
#if defined(MEMORY_TRACE)
   // Temporary until bug MDEV-4771 is fixed
   return HA_CACHE_TBL_NONTRANSACT;
#else
   return HA_CACHE_TBL_NOCACHE;
#endif
 }

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
  int write_row(const uchar *buf);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int update_row(const uchar *old_data, const uchar *new_data);

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
int index_prev(uchar *buf);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_first(uchar *buf);

  /** @brief
    We implement this in ha_connect.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_last(uchar *buf);

  /* Index condition pushdown implementation */
//Item *idx_cond_push(uint keyno, Item* idx_cond);

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
  int start_stmt(THD *thd, thr_lock_type lock_type);
  int external_lock(THD *thd, int lock_type);                   ///< required
  int delete_all_rows(void);
  ha_rows records_in_range(uint inx, const key_range *start_key,
                           const key_range *end_key, page_range *pages);
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
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info);                      ///< required
  bool check_if_incompatible_data(HA_CREATE_INFO *info,
                                  uint table_changes);

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);     ///< required
  int optimize(THD* thd, HA_CHECK_OPT* check_opt);

  /**
   * Multi Range Read interface
   */
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode, HANDLER_BUFFER *buf);
  int multi_range_read_next(range_id_t *range_info);
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param,
                                      uint n_ranges, uint *bufsz,
                                      uint *flags, Cost_estimate *cost);
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint key_parts, uint *bufsz,
                                uint *flags, Cost_estimate *cost);
  int multi_range_read_explain_info(uint mrr_mode, char *str, size_t size);

  int reset(void) {ds_mrr.dsmrr_close(); return 0;}

  /* Index condition pushdown implementation */
//  Item *idx_cond_push(uint keyno, Item* idx_cond);
private:
  DsMrr_impl ds_mrr;

protected:
  bool check_privileges(THD *thd, PTOS options, const char *dbn, bool quick=false);
  MODE CheckMode(PGLOBAL g, THD *thd, MODE newmode, bool *chk, bool *cras);
	int  check_stmt(PGLOBAL g, MODE newmode, bool cras);
	char *GetDBfromName(const char *name);

  // Members
  static ulong  num;                  // Tracable handler number
  PCONNECT      xp;                   // To user_connect associated class
  ulong         hnum;                 // The number of this handler
  query_id_t    valid_query_id;       // The one when tdbp was allocated
  query_id_t    creat_query_id;       // The one when handler was allocated
  PCSZ          datapath;             // Is the Path of DB data directory
  PTDB          tdbp;                 // To table class object
  PVAL          sdvalin1;             // Used to convert date values
  PVAL          sdvalin2;             // Used to convert date values
  PVAL          sdvalin3;             // Used to convert date values
  PVAL          sdvalin4;             // Used to convert date values
  PVAL          sdvalout;             // Used to convert date values
  bool          istable;              // True for table handler
  char          partname[65];         // The partition name
  MODE          xmod;                 // Table mode
  XINFO         xinfo;                // The table info structure
  bool          valid_info;           // True if xinfo is valid
  bool          stop;                 // Used when creating index
  bool          alter;                // True when converting to other engine
  bool          mrr;                  // True when getting index positions
  bool          nox;                  // True when index should not be made
  bool          abort;                // True after error in UPDATE/DELETE
  int           indexing;             // Type of indexing for CONNECT
  int           locked;               // Table lock
  MY_BITMAP    *part_id;              // Columns used for partition func
  THR_LOCK_DATA lock_data;

public:
  TABLE_SHARE  *tshp;                 // Used by called tables
  char   *data_file_name;
  char   *index_file_name;
  uint    int_table_flags;            // Inherited from MyISAM
  bool    enable_activate_all_index;  // Inherited from MyISAM
};  // end of ha_connect class definition

#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
bool MongoEnabled(void);
#endif   // JAVA_SUPPORT || CMGO_SUPPORT
