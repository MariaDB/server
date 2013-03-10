/* Copyright (C) Olivier Bertrand 2004 - 2013

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

/**
  @file ha_connect.cc

  @brief
  The ha_connect engine is a stubbed storage engine that enables to create tables
  based on external data. Principally they are based on plain files of many
  different types, but also on collections of such files, collection of tables,
  ODBC tables retrieving data from other DBMS having an ODBC server, and even
  virtual tables.

  @details
  ha_connect will let you create/open/delete tables, the created table can be
  done specifying an already existing file, the drop table command will just
  suppress the table definition but not the eventual data file.
  Indexes are not yet supported but data can be inserted, updated or deleted.

  You can enable the CONNECT storage engine in your build by doing the
  following during your build process:<br> ./configure
  --with-connect-storage-engine (not implemented yet)

  You can install the CONNECT handler as all other storage handlers.

  Once this is done, MySQL will let you create tables with:<br>
  CREATE TABLE <table name> (...) ENGINE=CONNECT;

  The example storage engine does not use table locks. It
  implements an example "SHARE" that is inserted into a hash by table
  name. This is not used yet.

  Please read the object definition in ha_connect.h before reading the rest
  of this file.

  @note
  This MariaDB CONNECT handler is currently an adaptation of the XDB handler
  that was written for MySQL version 4.1.2-alpha. Its overall design should
  be enhanced in the future to meet MariaDB requirements.

  @note
  It was written also from the Brian's ha_example handler and contains parts
  of it that are there but not currently used, such as table variables.

  @note
  When you create an CONNECT table, the MySQL Server creates a table .frm
  (format) file in the database directory, using the table name as the file
  name as is customary with MySQL. No other files are created. To get an idea
  of what occurs, here is an example select that would do a scan of an entire
  table:

  @code
  ha-connect::open
  ha_connect::store_lock
  ha_connect::external_lock
  ha_connect::info
  ha_connect::rnd_init
  ha_connect::extra
  ENUM HA_EXTRA_CACHE        Cache record in HA_rrnd()
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::extra
  ENUM HA_EXTRA_NO_CACHE     End caching of records (def)
  ha_connect::external_lock
  ha_connect::extra
  ENUM HA_EXTRA_RESET        Reset database to after open
  @endcode

  Here you see that the connect storage engine has 9 rows called before
  rnd_next signals that it has reached the end of its data. Calls to
  ha_connect::extra() are hints as to what will be occuring to the request.

  Happy use!<br>
    -Olivier
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#define MYSQL_SERVER 1
#define DONT_DEFINE_VOID
//#include "sql_partition.h"
#include "sql_class.h"
#include "create_options.h"
#include "mysql_com.h"
#include "field.h"
#include "sql_parse.h"
#undef  OFFSET

#define NOPARSE
#if defined(UNIX)
#include "osutil.h"
#endif   // UNIX
#include "global.h"
#include "plgdbsem.h"
#if defined(ODBC_SUPPORT)
#include "odbccat.h"
#endif   // ODBC_SUPPORT
#if defined(MYSQL_SUPPORT)
#include "xtable.h"
#include "tabmysql.h"
#endif   // MYSQL_SUPPORT
#include "filamdbf.h"
#include "tabfmt.h"
#include "reldef.h"
#include "tabcol.h"
#include "xindex.h"
#if defined(WIN32)
#include "tabwmi.h"
#endif   // WIN32
#include "connect.h"
#include "user_connect.h"
#include "ha_connect.h"
#include "mycat.h"
#include "myutil.h"
#include "preparse.h"

#define PLGXINI     "plgcnx.ini"       /* Configuration settings file  */
#define my_strupr(p)    my_caseup_str(default_charset_info, (p));
#define my_strlwr(p)    my_casedn_str(default_charset_info, (p));
#define my_stricmp(a,b) my_strcasecmp(default_charset_info, (a), (b))

#ifdef LIBXML2_SUPPORT
void XmlInitParserLib(void);
void XmlCleanupParserLib(void);
#endif   // LIBXML2_SUPPORT

/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
extern "C" char  plgxini[];
extern "C" char  plgini[];
extern "C" char  nmfile[];
extern "C" char  pdebug[];

extern "C" {
       char  version[]= "Version 1.01.0003 March 02, 2013";

#if defined(XMSG)
       char  msglang[];            // Default message language
#endif
       int  trace= 0;              // The general trace value
} // extern "C"

/****************************************************************************/
/*  Initialize the ha_connect static members.                               */
/****************************************************************************/
#define CONNECT_INI "connect.ini"
char  connectini[_MAX_PATH]= CONNECT_INI;
int   xtrace= 0;
ulong ha_connect::num= 0;
//int  DTVAL::Shift= 0;

static handler *connect_create_handler(handlerton *hton,
                                   TABLE_SHARE *table,
                                   MEM_ROOT *mem_root);

handlerton *connect_hton;

/* Variables for connect share methods */

/*
   Hash used to track the number of open tables; variable for connect share
   methods
*/
static HASH connect_open_tables;

/* The mutex used to init the hash; variable for example share methods */
mysql_mutex_t connect_mutex;


/**
  structure for CREATE TABLE options (table options)

  These can be specified in the CREATE TABLE:
  CREATE TABLE ( ... ) {...here...}
*/
struct ha_table_option_struct {
  const char *type;
  const char *filename;
  const char *optname;
  const char *tabname;
  const char *tablist;
  const char *dbname;
  const char *separator;
//const char *connect;
  const char *qchar;
  const char *module;
  const char *subtype;
  const char *catfunc;
  const char *oplist;
  const char *data_charset;
  int lrecl;
  int elements;
//int estimate;
  int multiple;
  int header;
  int quoted;
  int ending;
  int compressed;
  bool mapped;
  bool huge;
  bool split;
  bool readonly;
  };

#if defined(MARIADB)
ha_create_table_option connect_table_option_list[]=
{
  // These option are for stand alone Connect tables
  HA_TOPTION_STRING("TABLE_TYPE", type),
  HA_TOPTION_STRING("FILE_NAME", filename),
  HA_TOPTION_STRING("XFILE_NAME", optname),
//HA_TOPTION_STRING("CONNECT_STRING", connect),
  HA_TOPTION_STRING("TABNAME", tabname),
  HA_TOPTION_STRING("TABLE_LIST", tablist),
  HA_TOPTION_STRING("DBNAME", dbname),
  HA_TOPTION_STRING("SEP_CHAR", separator),
  HA_TOPTION_STRING("QCHAR", qchar),
  HA_TOPTION_STRING("MODULE", module),
  HA_TOPTION_STRING("SUBTYPE", subtype),
  HA_TOPTION_STRING("CATFUNC", catfunc),
  HA_TOPTION_STRING("OPTION_LIST", oplist),
  HA_TOPTION_STRING("DATA_CHARSET", data_charset),
  HA_TOPTION_NUMBER("LRECL", lrecl, 0, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("BLOCK_SIZE", elements, 0, 0, INT_MAX32, 1),
//HA_TOPTION_NUMBER("ESTIMATE", estimate, 0, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("MULTIPLE", multiple, 0, 0, 2, 1),
  HA_TOPTION_NUMBER("HEADER", header, 0, 0, 3, 1),
  HA_TOPTION_NUMBER("QUOTED", quoted, -1, 0, 3, 1),
  HA_TOPTION_NUMBER("ENDING", ending, -1, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("COMPRESS", compressed, 0, 0, 2, 1),
//HA_TOPTION_BOOL("COMPRESS", compressed, 0),
  HA_TOPTION_BOOL("MAPPED", mapped, 0),
  HA_TOPTION_BOOL("HUGE", huge, 0),
  HA_TOPTION_BOOL("SPLIT", split, 0),
  HA_TOPTION_BOOL("READONLY", readonly, 0),
  HA_TOPTION_END
};
#endif   // MARIADB


/**
  structure for CREATE TABLE options (field options)

  These can be specified in the CREATE TABLE per field:
  CREATE TABLE ( field ... {...here...}, ... )
*/
struct ha_field_option_struct
{
  int offset;
  int freq;      // Not used by this version
  int opt;       // Not used by this version
  int fldlen;
  const char *dateformat;
  const char *fieldformat;
  char *special;
};

#if defined(MARIADB)
ha_create_table_option connect_field_option_list[]=
{
  HA_FOPTION_NUMBER("FLAG", offset, -1, 0, INT_MAX32, 1),
  HA_FOPTION_NUMBER("FREQUENCY", freq, 0, 0, INT_MAX32, 1), // not used
  HA_FOPTION_NUMBER("OPT_VALUE", opt, 0, 0, 2, 1),  // used for indexing
  HA_FOPTION_NUMBER("FIELD_LENGTH", fldlen, 0, 0, INT_MAX32, 1),
  HA_FOPTION_STRING("DATE_FORMAT", dateformat),
  HA_FOPTION_STRING("FIELD_FORMAT", fieldformat),
  HA_FOPTION_STRING("SPECIAL", special),
  HA_FOPTION_END
};
#endif   // MARIADB

/***********************************************************************/
/*  Push G->Message as a MySQL warning.                                */
/***********************************************************************/
bool PushWarning(PGLOBAL g, PTDBASE tdbp)
  {
  PHC    phc;
  THD   *thd;
  MYCAT *cat= (MYCAT*)tdbp->GetDef()->GetCat();

  if (!cat || !(phc= cat->GetHandler()) || !phc->GetTable() ||
      !(thd= (phc->GetTable())->in_use))
    return true;

  push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN, 0, g->Message);
  return false;
  } // end of PushWarning

/**
  @brief
  Function we use in the creation of our hash to get key.
*/
static uchar* connect_get_key(CONNECT_SHARE *share, size_t *length,
                          my_bool not_used __attribute__((unused)))
{
  *length=share->table_name_length;
  return (uchar*) share->table_name;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key con_key_mutex_connect, con_key_mutex_CONNECT_SHARE_mutex;

static PSI_mutex_info all_connect_mutexes[]=
{
  { &con_key_mutex_connect, "connect", PSI_FLAG_GLOBAL},
  { &con_key_mutex_CONNECT_SHARE_mutex, "CONNECT_SHARE::mutex", 0}
};

static void init_connect_psi_keys()
{
  const char* category= "connect";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_connect_mutexes);
  PSI_server->register_mutex(category, all_connect_mutexes, count);
}
#endif


/**
  @brief
  Plugin initialization
*/
static int connect_init_func(void *p)
{
  DBUG_ENTER("connect_init_func");
  char dir[_MAX_PATH - sizeof(CONNECT_INI) - 1];

#ifdef LIBXML2_SUPPORT
  XmlInitParserLib();
#endif   // LIBXML2_SUPPORT

  /* Build connect.ini file name */
  my_getwd(dir, sizeof(dir) - 1, MYF(0));
  snprintf(connectini, sizeof(connectini), "%s%s", dir, CONNECT_INI);
  sql_print_information("CONNECT: %s=%s", CONNECT_INI, connectini);

  if ((xtrace= GetPrivateProfileInt("CONNECT", "Trace", 0, connectini)))
  {
    sql_print_information("CONNECT: xtrace=%d", xtrace);
    sql_print_information("CONNECT: plgini=%s", plgini);
    sql_print_information("CONNECT: plgxini=%s", plgxini);
    sql_print_information("CONNECT: nmfile=%s", nmfile);
    sql_print_information("CONNECT: pdebug=%s", pdebug);
    sql_print_information("CONNECT: version=%s", version);
    trace= xtrace;
  } // endif xtrace


#ifdef HAVE_PSI_INTERFACE
  init_connect_psi_keys();
#endif

  connect_hton= (handlerton *)p;
  mysql_mutex_init(con_key_mutex_connect, &connect_mutex, MY_MUTEX_INIT_FAST);
//VOID(mysql_mutex_init(&connect_mutex, MY_MUTEX_INIT_FAST));
  (void) my_hash_init(&connect_open_tables, system_charset_info, 32, 0, 0,
                   (my_hash_get_key) connect_get_key, 0, 0);

//connect_hton->name=    "CONNECT";
  connect_hton->state=   SHOW_OPTION_YES;
//connect_hton->comment= "CONNECT handler";
  connect_hton->create=  connect_create_handler;
  connect_hton->flags=   HTON_TEMPORARY_NOT_SUPPORTED | HTON_NO_PARTITION;
#if defined(MARIADB)
  connect_hton->db_type= DB_TYPE_AUTOASSIGN;
  connect_hton->table_options= connect_table_option_list;
  connect_hton->field_options= connect_field_option_list;
#else   // !MARIADB
//connect_hton->system_database= connect_system_database;
//connect_hton->is_supported_system_table= connect_is_supported_system_table;
#endif  // !MARIADB

  if (xtrace)
    sql_print_information("connect_init: hton=%p", p);

  DTVAL::SetTimeShift();      // Initialize time zone shift once for all
  DBUG_RETURN(0);
}


/**
  @brief
  Plugin clean up
*/
static int connect_done_func(void *p)
{
  int error= 0;
  PCONNECT pc, pn;
  DBUG_ENTER("connect_done_func");

#ifdef LIBXML2_SUPPORT
  XmlCleanupParserLib();
#endif   // LIBXML2_SUPPORT

  if (connect_open_tables.records)
    error= 1;

  for (pc= user_connect::to_users; pc; pc= pn) {
    if (pc->g)
      PlugCleanup(pc->g, true);

    pn= pc->next;
    delete pc;
    } // endfor pc

  my_hash_free(&connect_open_tables);
  mysql_mutex_destroy(&connect_mutex);

  DBUG_RETURN(error);
}


/**
  @brief
  Example of simple lock controls. The "share" it creates is a
  structure we will pass to each example handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/

static CONNECT_SHARE *get_share(const char *table_name, TABLE *table)
{
  CONNECT_SHARE *share;
  uint length;
  char *tmp_name;

  mysql_mutex_lock(&connect_mutex);
  length=(uint) strlen(table_name);

  if (!(share=(CONNECT_SHARE*)my_hash_search(&connect_open_tables,
                                      (uchar*) table_name, length))) {
    if (!(share=(CONNECT_SHARE *)my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
                &share, sizeof(*share), &tmp_name, length+1, NullS))) {
      mysql_mutex_unlock(&connect_mutex);
      return NULL;
      } // endif share

    share->use_count=0;
    share->table_name_length=length;
    share->table_name=tmp_name;
    strmov(share->table_name, table_name);

    if (my_hash_insert(&connect_open_tables, (uchar*) share))
      goto error;

    thr_lock_init(&share->lock);
    mysql_mutex_init(con_key_mutex_CONNECT_SHARE_mutex,
                     &share->mutex, MY_MUTEX_INIT_FAST);
    } // endif share

  share->use_count++;
  mysql_mutex_unlock(&connect_mutex);
  return share;

error:
  mysql_mutex_destroy(&share->mutex);
  my_free(share);
  return NULL;
}


/**
  @brief
  Free lock controls. We call this whenever we close a table. If the table had
  the last reference to the share, then we free memory associated with it.
*/

static int free_share(CONNECT_SHARE *share)
{
  mysql_mutex_lock(&connect_mutex);

  if (!--share->use_count) {
    my_hash_delete(&connect_open_tables, (uchar*) share);
    thr_lock_delete(&share->lock);
    mysql_mutex_destroy(&share->mutex);
#if !defined(MARIADB)
    my_free(share->table_options);
    my_free(share->field_options);
#endif   // !MARIADB
    my_free(share);
    } // endif share

  mysql_mutex_unlock(&connect_mutex);
  return 0;
}

static handler* connect_create_handler(handlerton *hton,
                                   TABLE_SHARE *table,
                                   MEM_ROOT *mem_root)
{
  handler *h= new (mem_root) ha_connect(hton, table);

  if (xtrace)
    printf("New CONNECT %p, table: %s\n",
                         h, table ? table->table_name.str : "<null>");

  return h;
} // end of connect_create_handler

/****************************************************************************/
/*  ha_connect constructor.                                                 */
/****************************************************************************/
ha_connect::ha_connect(handlerton *hton, TABLE_SHARE *table_arg)
       :handler(hton, table_arg)
{
  hnum= ++num;
  xp= NULL;         // Tested in next call
  xp= (table) ? GetUser(table->in_use) : NULL;
  tdbp= NULL;
  sdvalin= NULL;
  sdvalout= NULL;
  xmod= MODE_ANY;
  istable= false;
//*tname= '\0';
  bzero((char*) &xinfo, sizeof(XINFO));
  valid_info= false;
  valid_query_id= 0;
  creat_query_id= (table && table->in_use) ? table->in_use->query_id : 0;
  stop= false;
//hascond= false;
  indexing= -1;
  data_file_name= NULL;
  index_file_name= NULL;
  enable_activate_all_index= 0;
  int_table_flags= (HA_NO_TRANSACTIONS | HA_NO_PREFIX_CHAR_KEYS);
  ref_length= sizeof(int);
#if !defined(MARIADB)
  share= NULL;
  table_options= NULL;
  field_options= NULL;
#endif   // !MARIADB
  tshp= NULL;
} // end of ha_connect constructor


/****************************************************************************/
/*  ha_connect destructor.                                                  */
/****************************************************************************/
ha_connect::~ha_connect(void)
{
  if (xp) {
    PCONNECT p;

    xp->count--;

    for (p= user_connect::to_users; p; p= p->next)
      if (p == xp)
        break;

    if (p && !p->count) {
      if (p->next)
        p->next->previous= p->previous;

      if (p->previous)
        p->previous->next= p->next;
      else
        user_connect::to_users= p->next;

      } // endif p

    if (!xp->count) {
      PlugCleanup(xp->g, true);
      delete xp;
      } // endif count

    } // endif xp

#if !defined(MARIADB)
  my_free(table_options);
  my_free(field_options);
#endif   // !MARIADB
} // end of ha_connect destructor


/****************************************************************************/
/*  Get a pointer to the user of this handler.                              */
/****************************************************************************/
PCONNECT ha_connect::GetUser(THD *thd)
{
  const char *dbn= NULL;

  if (!thd)
    return NULL;

  if (xp && thd == xp->thdp)
    return xp;

  for (xp= user_connect::to_users; xp; xp= xp->next)
    if (thd == xp->thdp)
      break;

  if (!xp) {
    xp= new user_connect(thd, dbn);

    if (xp->user_init(this)) {
      delete xp;
      xp= NULL;
      } // endif user_init

  } else
    xp->count++;

  return xp;
} // end of GetUser


/****************************************************************************/
/*  Get the global pointer of the user of this handler.                     */
/****************************************************************************/
PGLOBAL ha_connect::GetPlug(THD *thd)
{
  PCONNECT lxp= GetUser(thd);
  return (lxp) ? lxp->g : NULL;
} // end of GetPlug


/****************************************************************************/
/*  Return the value of an option specified in the option list.             */
/****************************************************************************/
char *ha_connect::GetListOption(const char *opname,
                                const char *oplist,
                                const char *def)
{
  char key[16], val[256];
  char *pk, *pv, *pn;
  char *opval= (char *) def;
  int n;

  for (pk= (char*)oplist; pk; pk= ++pn) {
    pn= strchr(pk, ',');
    pv= strchr(pk, '=');

    if (pv && (!pn || pv < pn)) {
      n= pv - pk;
      memcpy(key, pk, n);
      key[n]= 0;
      pv++;

      if (pn) {
        n= pn - pv;
        memcpy(val, pv, n);
        val[n]= 0;
      } else
        strcpy(val, pv);

    } else {
      if (pn) {
        n= pn - pk;
        memcpy(key, pk, n);
        key[n]= 0;
      } else
        strcpy(key, pk);

      val[0]= 0;
    } // endif pv

    if (!stricmp(opname, key)) {
      opval= (char*)PlugSubAlloc(xp->g, NULL, strlen(val) + 1);
      strcpy(opval, val);
      break;
    } else if (!pn)
      break;

    } // endfor pk

  return opval;
} // end of GetListOption

/****************************************************************************/
/*  Return the table option structure.                                      */
/****************************************************************************/
PTOS ha_connect::GetTableOptionStruct(TABLE *tab)
{
#if defined(MARIADB)
  return (tshp) ? tshp->option_struct : tab->s->option_struct;
#else   // !MARIADB
  if (share && share->table_options)
    return share->table_options;
  else if (table_options)
    return table_options;

  char  *pk, *pv, *pn, *val;
  size_t len= sizeof(ha_table_option_struct) + tab->s->comment.length + 1;
  PTOS   top= (PTOS)my_malloc(len, MYF(MY_FAE | MY_ZEROFILL));

  top->quoted= -1;   // Default value
  top->ending= -1;   // Default value
  pk= (char *)top + sizeof(ha_table_option_struct);
  memcpy(pk, tab->s->comment.str, tab->s->comment.length);

  for (; pk; pk= ++pn) {
    pn= strchr(pk, ',');
    pv= strchr(pk, '=');

    if (pn) *pn= 0;

    if (pv) *pv= 0;

    val= (pv && (!pn || pv < pn)) ? pv + 1 : "";

    if (!stricmp(pk, "type") || !stricmp(pk, "Table_Type")) {
      top->type= val;
    } else if (!stricmp(pk, "fn") || !stricmp(pk, "filename")
                                  || !stricmp(pk, "File_Name")) {
      top->filename= val;
    } else if (!stricmp(pk, "optfn") || !stricmp(pk, "optname")
                                     || !stricmp(pk, "Xfile_Name")) {
      top->optname= val;
    } else if (!stricmp(pk, "name") || !stricmp(pk, "tabname")) {
      top->tabname= val;
    } else if (!stricmp(pk, "tablist") || !stricmp(pk, "tablelist")
                                       || !stricmp(pk, "Table_list")) {
      top->tablist= val;
    } else if (!stricmp(pk, "sep") || !stricmp(pk, "separator")
                                   || !stricmp(pk, "Sep_Char")) {
      top->separator= val;
    } else if (!stricmp(pk, "db") || !stricmp(pk, "DBName")
                                  || !stricmp(pk, "Database") {
      top->dbname= val;
    } else if (!stricmp(pk, "qchar")) {
      top->qchar= val;
    } else if (!stricmp(pk, "module")) {
      top->module= val;
    } else if (!stricmp(pk, "subtype")) {
      top->subtype= val;
    } else if (!stricmp(pk, "lrecl")) {
      top->lrecl= atoi(val);
    } else if (!stricmp(pk, "elements")) {
      top->elements= atoi(val);
    } else if (!stricmp(pk, "multiple")) {
      top->multiple= atoi(val);
    } else if (!stricmp(pk, "header")) {
      top->header= atoi(val);
    } else if (!stricmp(pk, "quoted")) {
      top->quoted= atoi(val);
    } else if (!stricmp(pk, "ending")) {
      top->ending= atoi(val);
    } else if (!stricmp(pk, "compressed")) {
      top->compressed= atoi(val);
    } else if (!stricmp(pk, "mapped")) {
      top->mapped= (!*val || *val == 'y' || *val == 'Y' || atoi(val) != 0);
    } else if (!stricmp(pk, "huge")) {
      top->huge= (!*val || *val == 'y' || *val == 'Y' || atoi(val) != 0);
    } else if (!stricmp(pk, "split")) {
      top->split= (!*val || *val == 'y' || *val == 'Y' || atoi(val) != 0);
    } else if (!stricmp(pk, "readonly") || !stricmp(pk, "protected")) {
      top->readonly= (!*val || *val == 'y' || *val == 'Y' || atoi(val) != 0);
    } // endif's

    if (!pn)
      break;

    } // endfor pk

  // This to get all other options
  top->oplist= tab->s->comment.str;

  if (share)
    share->table_options= top;
  else
    table_options= top;

  return top;
#endif  // !MARIADB
} // end of GetTableOptionStruct

/****************************************************************************/
/*  Return the value of a string option or NULL if not specified.           */
/****************************************************************************/
char *ha_connect::GetStringOption(char *opname, char *sdef)
{
  char *opval= NULL;
  PTOS  options= GetTableOptionStruct(table);

  if (!options)
    ;
  else if (!stricmp(opname, "Type"))
    opval= (char*)options->type;
  else if (!stricmp(opname, "Filename"))
    opval= (char*)options->filename;
  else if (!stricmp(opname, "Optname"))
    opval= (char*)options->optname;
  else if (!stricmp(opname, "Tabname"))
    opval= (char*)options->tabname;
  else if (!stricmp(opname, "Tablist"))
    opval= (char*)options->tablist;
  else if (!stricmp(opname, "Database") ||
           !stricmp(opname, "DBname"))
    opval= (char*)options->dbname;
  else if (!stricmp(opname, "Separator"))
    opval= (char*)options->separator;
  else if (!stricmp(opname, "Connect"))
//  opval= (char*)options->connect;
    opval= table->s->connect_string.str;
  else if (!stricmp(opname, "Qchar"))
    opval= (char*)options->qchar;
  else if (!stricmp(opname, "Module"))
    opval= (char*)options->module;
  else if (!stricmp(opname, "Subtype"))
    opval= (char*)options->subtype;
  else if (!stricmp(opname, "Catfunc"))
    opval= (char*)options->catfunc;
  else if (!stricmp(opname, "Data_charset"))
    opval= (char*)options->data_charset;

  if (!opval && options->oplist)
    opval= GetListOption(opname, options->oplist);

  if (!opval) {
    if (sdef && !strcmp(sdef, "*")) {
      // Return the handler default value
      if (!stricmp(opname, "Dbname") || !stricmp(opname, "Database"))
        opval= (char*)GetDBName(NULL);    // Current database
      else
        opval= sdef;                      // Caller default

    } else
      opval= sdef;                        // Caller default

    } // endif !opval

  return opval;
} // end of GetStringOption

/****************************************************************************/
/*  Return the value of a Boolean option or bdef if not specified.          */
/****************************************************************************/
bool ha_connect::GetBooleanOption(char *opname, bool bdef)
{
  bool  opval= bdef;
  char *pv;
  PTOS  options= GetTableOptionStruct(table);

  if (!options)
    ;
  else if (!stricmp(opname, "Mapped"))
    opval= options->mapped;
  else if (!stricmp(opname, "Huge"))
    opval= options->huge;
//else if (!stricmp(opname, "Compressed"))
//  opval= options->compressed;
  else if (!stricmp(opname, "Split"))
    opval= options->split;
  else if (!stricmp(opname, "Readonly"))
    opval= options->readonly;
  else if (options->oplist)
    if ((pv= GetListOption(opname, options->oplist)))
      opval= (!*pv || *pv == 'y' || *pv == 'Y' || atoi(pv) != 0);

  return opval;
} // end of GetBooleanOption

/****************************************************************************/
/*  Return the value of an integer option or NO_IVAL if not specified.      */
/****************************************************************************/
int ha_connect::GetIntegerOption(char *opname)
{
  int   opval= NO_IVAL;
  char *pv;
  PTOS  options= GetTableOptionStruct(table);

  if (!options)
    ;
  else if (!stricmp(opname, "Lrecl"))
    opval= options->lrecl;
  else if (!stricmp(opname, "Elements"))
    opval= options->elements;
  else if (!stricmp(opname, "Estimate"))
//  opval= options->estimate;
    opval= (int)table->s->max_rows;
  else if (!stricmp(opname, "Avglen"))
    opval= (int)table->s->avg_row_length;
  else if (!stricmp(opname, "Multiple"))
    opval= options->multiple;
  else if (!stricmp(opname, "Header"))
    opval= options->header;
  else if (!stricmp(opname, "Quoted"))
    opval= options->quoted;
  else if (!stricmp(opname, "Ending"))
    opval= options->ending;
  else if (!stricmp(opname, "Compressed"))
    opval= (options->compressed);

  if (opval == NO_IVAL && options->oplist)
    if ((pv= GetListOption(opname, options->oplist)))
      opval= atoi(pv);

  return opval;
} // end of GetIntegerOption

/****************************************************************************/
/*  Set the value of the opname option (does not work for oplist options)   */
/*  Currently used only to set the Lrecl value.                             */
/****************************************************************************/
bool ha_connect::SetIntegerOption(char *opname, int n)
{
  PTOS options= GetTableOptionStruct(table);

  if (!options)
    return true;

  if (!stricmp(opname, "Lrecl"))
    options->lrecl= n;
  else if (!stricmp(opname, "Elements"))
    options->elements= n;
//else if (!stricmp(opname, "Estimate"))
//  options->estimate= n;
  else if (!stricmp(opname, "Multiple"))
    options->multiple= n;
  else if (!stricmp(opname, "Header"))
    options->header= n;
  else if (!stricmp(opname, "Quoted"))
    options->quoted= n;
  else if (!stricmp(opname, "Ending"))
    options->ending= n;
  else if (!stricmp(opname, "Compressed"))
    options->compressed= n;
  else
    return true;
//else if (options->oplist)
//  SetListOption(opname, options->oplist, n);

  return false;
} // end of SetIntegerOption

/****************************************************************************/
/*  Return a field option structure.                                        */
/****************************************************************************/
PFOS ha_connect::GetFieldOptionStruct(Field *fdp)
{
#if defined(MARIADB)
  return fdp->option_struct;
#else   // !MARIADB
  if (share && share->field_options)
    return &share->field_options[fdp->field_index];
  else if (field_options)
    return &field_options[fdp->field_index];

  char  *pc, *pk, *pv, *pn, *val;
  int    i, k, n= table->s->fields;
  size_t len= n + n * sizeof(ha_field_option_struct);
  PFOS   fp, fop;

  for (i= 0; i < n; i++)
    len+= table->s->field[i]->comment.length;

  fop= (PFOS)my_malloc(len, MYF(MY_FAE | MY_ZEROFILL));
  pc= (char*)fop + n * sizeof(ha_field_option_struct);

  for (i= k= 0; i < n;  i++) {
    fp= &fop[i];
    fp->offset= -1;    // Default value

    if (!table->s->field[i]->comment.length)
      continue;

    memcpy(pc, table->s->field[i]->comment.str,
               table->s->field[i]->comment.length);

    for (pk= pc; pk; pk= ++pn) {
      if ((pn= strchr(pk, ','))) *pn= 0;
      if ((pv= strchr(pk, '='))) *pv= 0;
      val= (pv && (!pn || pv < pn)) ? pv + 1 : "";

      if (!stricmp(pk, "datefmt") || !stricmp(pk, "date_format")) {
        fp->dateformat= val;
      } else if (!stricmp(pk, "fieldfmt") || !stricmp(pk, "field_format")) {
        fp->fieldformat= val;
      } else if (!stricmp(pk, "special")) {
        fp->special= val;
      } else if (!stricmp(pk, "offset") || !stricmp(pk, "flag")) {
        fp->offset= atoi(val);
      } else if (!stricmp(pk, "freq")) {
        fp->freq= atoi(val);
      } else if (!stricmp(pk, "opt")) {
        fp->opt= atoi(val);
      } else if (!stricmp(pk, "fldlen") || !stricmp(pk, "field_length")) {
        fp->fldlen= atoi(val);
      } // endif's

      if (!pn)
        break;

      } // endfor pk

    pc+= table->s->field[i]->comment.length + 1;
    } // endfor i

  if (share)
    share->field_options= fop;
  else
    field_options= fop;

  return &fop[fdp->field_index];
#endif  // !MARIADB
} // end of GetFildOptionStruct

/****************************************************************************/
/*  Returns the column description structure used to make the column.       */
/****************************************************************************/
void *ha_connect::GetColumnOption(void *field, PCOLINFO pcf)
{
  const char *cp;
  ha_field_option_struct *fop;
  Field*  fp;
  Field* *fldp;

  // Double test to be on the safe side
  if (!table)
    return NULL;

  // Find the column to describe
  if (field) {
    fldp= (Field**)field;
    fldp++;
  } else
    fldp= (tshp) ? tshp->field : table->field;

  if (!(fp= *fldp))
    return NULL;

  // Get the CONNECT field options structure
  fop= GetFieldOptionStruct(fp);
  pcf->Flags= 0;

  // Now get column information
  if (fop && fop->special) {
    pcf->Name= "*";
    return fldp;
  } else
    pcf->Name= (char*)fp->field_name;

  pcf->Prec= 0;
  pcf->Opt= (fop) ? fop->opt : 0;

  if ((pcf->Length= fp->field_length) < 0)
    pcf->Length= 256;            // BLOB?

  if (fop) {
    pcf->Offset= fop->offset;
//  pcf->Freq= fop->freq;
    pcf->Datefmt= (char*)fop->dateformat;
    pcf->Fieldfmt= (char*)fop->fieldformat;
  } else {
    pcf->Offset= -1;
//  pcf->Freq= 0;
    pcf->Datefmt= NULL;
    pcf->Fieldfmt= NULL;
  } // endif fop

  switch (fp->type()) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VARCHAR:
      pcf->Flags |= U_VAR;
    case MYSQL_TYPE_STRING:
      pcf->Type= TYPE_STRING;

      // Do something for case
      cp= fp->charset()->name;

      // Find if collation name ends by _ci
      if (!strcmp(cp + strlen(cp) - 3, "_ci")) {
        pcf->Prec= 1;      // Case insensitive
        pcf->Opt= 0;       // Prevent index opt until it is safe
        } // endif ci

      break;
    case MYSQL_TYPE_LONG:
      pcf->Type= TYPE_INT;
      break;
    case MYSQL_TYPE_SHORT:
      pcf->Type= TYPE_SHORT;
      break;
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_FLOAT:
      pcf->Type= TYPE_FLOAT;
      pcf->Prec= max(min(fp->decimals(), ((unsigned)pcf->Length - 2)), 0);
      break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      pcf->Type= TYPE_DATE;

      // Field_length is only used for DATE columns
      if (fop->fldlen)
        pcf->Length= fop->fldlen;
      else { 
        int len;

        if (pcf->Datefmt) {
          // Find the (max) length produced by the date format
          char    buf[256];
          PGLOBAL g= GetPlug(table->in_use);
#if defined(WIN32)
          struct tm datm= {0,0,0,12,11,112,0,0,0};
#else   // !WIN32
          struct tm datm= {0,0,0,12,11,112,0,0,0,0,0};
#endif  // !WIN32
          PDTP    pdtp= MakeDateFormat(g, pcf->Datefmt, false, true, 0);

          len= strftime(buf, 256, pdtp->OutFmt, &datm);
        } else
          len= 0;

        // 11 is for signed numeric representation of the date
        pcf->Length= (len) ? len : 11;
        } // endelse

      break;
    case MYSQL_TYPE_LONGLONG:
      pcf->Type= TYPE_BIGINT;
      break;
    default:
      pcf->Type=TYPE_ERROR;
    } // endswitch type

  // This is used to skip null bit
  if (fp->real_maybe_null())
    pcf->Flags |= U_NULLS;

#if defined(MARIADB)
  // Mark virtual columns as such
  if (fp->vcol_info && !fp->stored_in_db)
    pcf->Flags |= U_VIRTUAL;
#endif   // MARIADB

  pcf->Key= 0;   // Not used when called from MySQL
  pcf->Remark= fp->comment.str;
  return fldp;
} // end of GetColumnOption

/****************************************************************************/
/*  Returns the index description structure used to make the index.         */
/****************************************************************************/
PIXDEF ha_connect::GetIndexInfo(int n)
{
  char    *name, *pn;
  bool     unique;
  PIXDEF   xdp= NULL;
  PKPDEF   kpp, pkp= NULL;
  PGLOBAL& g= xp->g;
  KEY      kp;

  // Find the index to describe
  if ((unsigned)n < table->s->keynames.count)
//    kp= table->key_info[n];    which one ???
    kp= table->s->key_info[n];
  else
    return NULL;

  // Now get index information
  pn= (char*)table->s->keynames.type_names[n];
  name= (char*)PlugSubAlloc(g, NULL, strlen(pn) + 1);
  strcpy(name, pn);    // This is probably unuseful
  unique= (kp.flags & 1) != 0;

  // Allocate the index description block
  xdp= new(g) INDEXDEF(name, unique, n);

  // Get the the key parts info
  for (int k= 0; (unsigned)k < kp.key_parts; k++) {
    pn= (char*)kp.key_part[k].field->field_name;
    name= (char*)PlugSubAlloc(g, NULL, strlen(pn) + 1);
    strcpy(name, pn);    // This is probably unuseful

    // Allocate the key part description block
    kpp= new(g) KPARTDEF(name, k + 1);
    kpp->SetKlen(kp.key_part[k].length);

    // Index on auto increment column is an XXROW index
    if (kp.key_part[k].field->flags & AUTO_INCREMENT_FLAG && kp.key_parts == 1)
      xdp->SetAuto(true);

    if (pkp)
      pkp->SetNext(kpp);
    else
      xdp->SetToKeyParts(kpp);

    pkp= kpp;
    } // endfor k

  xdp->SetNParts(kp.key_parts);
  return xdp;
} // end of GetIndexInfo

const char *ha_connect::GetDBName(const char* name)
{
  return (name) ? name : table->s->db.str;
} // end of GetDBName

const char *ha_connect::GetTableName(void)
{
  return table->s->table_name.str;
} // end of GetTableName

/****************************************************************************/
/*  Returns the column real or special name length of a field.              */
/****************************************************************************/
int ha_connect::GetColNameLen(Field *fp)
{
  int n;
  PFOS fop= GetFieldOptionStruct(fp);

  // Now get the column name length
  if (fop && fop->special)
    n= strlen(fop->special) + 1;
  else
    n= strlen(fp->field_name) + 1;

  return n;
} // end of GetColNameLen

/****************************************************************************/
/*  Returns the column real or special name of a field.                     */
/****************************************************************************/
char *ha_connect::GetColName(Field *fp)
{
  PFOS fop= GetFieldOptionStruct(fp);

  return (fop && fop->special) ? fop->special : (char*)fp->field_name;
} // end of GetColName

/****************************************************************************/
/*  Adds the column real or special name of a field to a string.            */
/****************************************************************************/
void ha_connect::AddColName(char *cp, Field *fp)
{
  PFOS fop= GetFieldOptionStruct(fp);

  // Now add the column name
  if (fop && fop->special)
    // The prefix * mark the column as "special"
    strcat(strcpy(cp, "*"), strupr(fop->special));
  else
    strcpy(cp, (char*)fp->field_name);

} // end of AddColName

/****************************************************************************/
/*  Get the table description block of a CONNECT table.                     */
/****************************************************************************/
PTDB ha_connect::GetTDB(PGLOBAL g)
{
  const char *table_name;
  PTDB        tp;

  // Double test to be on the safe side
  if (!g || !table)
    return NULL;

  table_name= GetTableName();

  if (tdbp && !stricmp(tdbp->GetName(), table_name)
           && tdbp->GetMode() == xmod && !xp->CheckQuery(valid_query_id)) {
    tp= tdbp;
    tp->SetMode(xmod);
  } else if ((tp= CntGetTDB(g, table_name, xmod, this)))
    valid_query_id= xp->last_query_id;
  else
    printf("GetTDB: %s\n", g->Message);

  return tp;
} // end of GetTDB

/****************************************************************************/
/*  Open a CONNECT table, restricting column list if cols is true.          */
/****************************************************************************/
bool ha_connect::OpenTable(PGLOBAL g, bool del)
{
  bool  rc= false;
  char *c1= NULL, *c2=NULL;

  // Double test to be on the safe side
  if (!g || !table) {
    printf("OpenTable logical error; g=%p table=%p\n", g, table);
    return true;
    } // endif g

  if (!(tdbp= GetTDB(g)))
    return true;
  else if (tdbp->IsReadOnly())
    switch (xmod) {
      case MODE_WRITE:
      case MODE_INSERT:
      case MODE_UPDATE:
      case MODE_DELETE:
        strcpy(g->Message, MSG(READ_ONLY));
        return true;
      default:
        break;
      } // endswitch xmode

  // Get the list of used fields (columns)
  char        *p;
  unsigned int k1, k2, n1, n2;
  Field*      *field;
  MY_BITMAP   *map= (xmod != MODE_INSERT) ? table->read_set  : table->write_set;
  MY_BITMAP   *ump= (xmod == MODE_UPDATE) ? table->write_set : NULL;

  k1= k2= 0;
  n1= n2= 1;         // 1 is space for final null character

  for (field= table->field; *field; field++) {
    if (bitmap_is_set(map, (*field)->field_index)) {
      n1+= (GetColNameLen(*field) + 1);
      k1++;
      } // endif

    if (ump && bitmap_is_set(ump, (*field)->field_index)) {
      n2+= GetColNameLen(*field);
      k2++;
      } // endif

    } // endfor field

  if (k1) {
    p= c1= (char*)PlugSubAlloc(g, NULL, n1);

    for (field= table->field; *field; field++)
      if (bitmap_is_set(map, (*field)->field_index)) {
        AddColName(p, *field);
        p+= (strlen(p) + 1);
        } // endif used field

    *p= '\0';          // mark end of list
    } // endif k1

  if (k2) {
    p= c2= (char*)PlugSubAlloc(g, NULL, n2);

    for (field= table->field; *field; field++)
      if (bitmap_is_set(ump, (*field)->field_index)) {
        AddColName(p, *field);
        p+= (strlen(p) + 1);
        } // endif used field

    *p= '\0';          // mark end of list
    } // endif k2

  // Open the table
  if (!(rc= CntOpenTable(g, tdbp, xmod, c1, c2, del, this))) {
    istable= true;
//  strmake(tname, table_name, sizeof(tname)-1);

    if (xmod == MODE_ANY && stop && *tdbp->GetName() != '#') {
      // We are in a create index query
      if (!((PTDBASE)tdbp)->GetDef()->Indexable()) {
        sprintf(g->Message, "Table %s cannot be indexed", tdbp->GetName());
        rc= true;
      } else if (xp) // xp can be null when called from create
        xp->tabp= (PTDBDOS)tdbp;  // The table on which the index is created

      } // endif xmod

//  tdbp->SetOrig((PTBX)table);  // used by CheckCond
  } else
    printf("OpenTable: %s\n", g->Message);

  if (rc) {
    tdbp= NULL;
    valid_info= false;
    } // endif rc

  return rc;
} // end of OpenTable


/****************************************************************************/
/*  IsOpened: returns true if the table is already opened.                  */
/****************************************************************************/
bool ha_connect::IsOpened(void)
{
  return (!xp->CheckQuery(valid_query_id) && tdbp
                                          && tdbp->GetUse() == USE_OPEN);
} // end of IsOpened


/****************************************************************************/
/*  Close a CONNECT table.                                                  */
/****************************************************************************/
int ha_connect::CloseTable(PGLOBAL g)
{
  int rc= CntCloseTable(g, tdbp);
  tdbp= NULL;
  sdvalin=NULL;
  sdvalout=NULL;
  valid_info= false;
  indexing= -1;
  return rc;
} // end of CloseTable


/***********************************************************************/
/*  Make a pseudo record from current row values. Specific to MySQL.   */
/***********************************************************************/
int ha_connect::MakeRecord(char *buf)
{
  char            *p, *fmt, val[32];
  int              rc= 0;
  Field*          *field;
  Field           *fp;
  my_bitmap_map   *org_bitmap;
  CHARSET_INFO    *charset= tdbp->data_charset();
  const MY_BITMAP *map;
  PVAL             value;
  PCOL             colp= NULL;
  DBUG_ENTER("ha_connect::MakeRecord");

  if (xtrace > 1)
#if defined(MARIADB)
    printf("Maps: read=%08X write=%08X vcol=%08X defr=%08X defw=%08X\n",
            *table->read_set->bitmap, *table->write_set->bitmap,
            *table->vcol_set->bitmap,
            *table->def_read_set.bitmap, *table->def_write_set.bitmap);
#else   // !MARIADB
    printf("Maps: read=%p write=%p defr=%p defw=%p\n",
            *table->read_set->bitmap, *table->write_set->bitmap,
            *table->def_read_set.bitmap, *table->def_write_set.bitmap);
#endif  // !MARIADB

  // Avoid asserts in field::store() for columns that are not updated
  org_bitmap= dbug_tmp_use_all_columns(table, table->write_set);

  // This is for variable_length rows
  memset(buf, 0, table->s->null_bytes);

  // When sorting read_set selects all columns, so we use def_read_set
  map= (const MY_BITMAP *)&table->def_read_set;

  // Make the pseudo record from field values
  for (field= table->field; *field && !rc; field++) {
    fp= *field;

#if defined(MARIADB)
    if (fp->vcol_info && !fp->stored_in_db)
      continue;            // This is a virtual column
#endif   // MARIADB

    if (bitmap_is_set(map, fp->field_index)) {
      // This is a used field, fill the buffer with value
      for (colp= tdbp->GetColumns(); colp; colp= colp->GetNext())
        if (!stricmp(colp->GetName(), GetColName(fp)))
          break;

      if (!colp) {
        printf("Column %s not found\n", fp->field_name);
        dbug_tmp_restore_column_map(table->write_set, org_bitmap);
        DBUG_RETURN(HA_ERR_WRONG_IN_RECORD);
        } // endif colp

      value= colp->GetValue();

      // All this could be better optimized
      if (!value->IsNull()) {
        switch (value->GetType()) {
          case TYPE_DATE:
            if (!sdvalout)
              sdvalout= AllocateValue(xp->g, TYPE_STRING, 20);
      
            switch (fp->type()) {
              case MYSQL_TYPE_DATE:
                fmt= "%Y-%m-%d";
                break;
              case MYSQL_TYPE_TIME:
                fmt= "%H:%M:%S";
                break;
              default:
                fmt= "%Y-%m-%d %H:%M:%S";
              } // endswitch type
      
            // Get date in the format required by MySQL fields
            value->FormatValue(sdvalout, fmt);
            p= sdvalout->GetCharValue();
            break;
          case TYPE_FLOAT:
            p= NULL;
            break;
          case TYPE_STRING:
            // Passthru
          default:
            p= value->GetCharString(val);
          } // endswitch Type

        if (p) {
          if (fp->store(p, strlen(p), charset, CHECK_FIELD_WARN)) {
            // Avoid "error" on null fields
            if (value->GetIntValue())
              rc= HA_ERR_WRONG_IN_RECORD;
    
            DBUG_PRINT("MakeRecord", (p));
            } // endif store
    
        } else
          if (fp->store(value->GetFloatValue())) {
            rc= HA_ERR_WRONG_IN_RECORD;
            DBUG_PRINT("MakeRecord", (value->GetCharString(val)));
            } // endif store

        fp->set_notnull();
      } else
        fp->set_null();

      } // endif bitmap

    } // endfor field

  // This is copied from ha_tina and is necessary to avoid asserts
  dbug_tmp_restore_column_map(table->write_set, org_bitmap);
  DBUG_RETURN(rc);
} // end of MakeRecord


/***********************************************************************/
/*  Set row values from a MySQL pseudo record. Specific to MySQL.      */
/***********************************************************************/
int ha_connect::ScanRecord(PGLOBAL g, uchar *buf)
{
  char    attr_buffer[1024];
  char    data_buffer[1024];
  char   *fmt;
  int     rc= 0;
  PCOL    colp;
  PVAL    value;
  Field  *fp;
  PTDBASE tp= (PTDBASE)tdbp;
  String  attribute(attr_buffer, sizeof(attr_buffer),
                    table->s->table_charset);
  my_bitmap_map *bmap= dbug_tmp_use_all_columns(table, table->read_set);
  const CHARSET_INFO *charset= tdbp->data_charset();
  String  data_charset_value(data_buffer, sizeof(data_buffer),  charset);

  // Scan the pseudo record for field values and set column values
  for (Field **field=table->field ; *field ; field++) {
    fp= *field;

#if defined(MARIADB)
    if ((fp->vcol_info && !fp->stored_in_db) ||
         fp->option_struct->special)
      continue;            // Is a virtual column possible here ???
#endif   // MARIADB

    if (bitmap_is_set(table->write_set, fp->field_index)) {
      for (colp= tp->GetSetCols(); colp; colp= colp->GetNext())
        if (!stricmp(colp->GetName(), fp->field_name))
          break;

      if (!colp) {
        printf("Column %s not found\n", fp->field_name);
        rc= HA_ERR_WRONG_IN_RECORD;
        goto err;
      } else
        value= colp->GetValue();

      // This is a used field, fill the value from the row buffer
      // All this could be better optimized
      if (fp->is_null()) {
        if (colp->IsNullable())
          value->SetNull(true);

        value->Reset();
      } else switch (value->GetType()) {
        case TYPE_FLOAT:
          value->SetValue(fp->val_real());
          break;
        case TYPE_DATE:
          if (!sdvalin) {
            sdvalin= (DTVAL*)AllocateValue(xp->g, TYPE_DATE, 19);

            // Get date in the format produced by MySQL fields
            switch (fp->type()) {
              case MYSQL_TYPE_DATE:
                fmt= "YYYY-MM-DD";
                break;
              case MYSQL_TYPE_TIME:
                fmt= "hh:mm:ss";
                break;
              default:
                fmt= "YYYY-MM-DD hh:mm:ss";
              } // endswitch type

            ((DTVAL*)sdvalin)->SetFormat(g, fmt, strlen(fmt));
            } // endif sdvalin

          fp->val_str(&attribute);
          sdvalin->SetValue_psz(attribute.c_ptr_safe());
          value->SetValue_pval(sdvalin);
          break;
        default:
          fp->val_str(&attribute);
          if (charset == &my_charset_bin)
          {
            value->SetValue_psz(attribute.c_ptr_safe());
          }
          else
          {
            // Convert from SQL field charset to DATA_CHARSET
            uint cnv_errors;
            data_charset_value.copy(attribute.ptr(), attribute.length(),
                                    attribute.charset(), charset, &cnv_errors);
            value->SetValue_psz(data_charset_value.c_ptr_safe());
          }
        } // endswitch Type

#ifdef NEWCHANGE
    } else if (xmod == MODE_UPDATE) {
      PCOL cp;

      for (cp= tp->GetColumns(); cp; cp= cp->GetNext())
        if (!stricmp(colp->GetName(), cp->GetName()))
          break;

      if (!cp) {
        rc= HA_ERR_WRONG_IN_RECORD;
        goto err;
        } // endif cp

      value->SetValue_pval(cp->GetValue());
    } else // mode Insert
      value->Reset();
#else
    } // endif bitmap_is_set
#endif

    } // endfor field

 err:
  dbug_tmp_restore_column_map(table->read_set, bmap);
  return rc;
} // end of ScanRecord


/***********************************************************************/
/*  Check change in index column. Specific to MySQL.                   */
/*  Should be elaborated to check for real changes.                    */
/***********************************************************************/
int ha_connect::CheckRecord(PGLOBAL g, const uchar *oldbuf, uchar *newbuf)
{
  return ScanRecord(g, newbuf);
} // end of dummy CheckRecord


/***********************************************************************/
/*  Return the string representing an operator.                        */
/***********************************************************************/
const char *ha_connect::GetValStr(OPVAL vop, bool neg)
{
  const char *val;

  switch (vop) {
    case OP_EQ:
      val= " = ";
      break;
    case OP_NE:
      val= " <> ";
      break;
    case OP_GT:
      val= " > ";
      break;
    case OP_GE:
      val= " >= ";
      break;
    case OP_LT:
      val= " < ";
      break;
    case OP_LE:
      val= " <= ";
      break;
    case OP_IN:
      val= (neg) ? " NOT IN (" : " IN (";
      break;
    case OP_NULL:
      val= " IS NULL";
      break;
    case OP_LIKE:
      val= " LIKE ";
      break;
    case OP_XX:
      val= " BETWEEN ";
      break;
    case OP_EXIST:
      val= " EXISTS ";
      break;
    case OP_AND:
      val= " AND ";
      break;
    case OP_OR:
      val= " OR ";
      break;
    case OP_NOT:
      val= " NOT ";
      break;
    case OP_CNC:
      val= " || ";
      break;
    case OP_ADD:
      val= " + ";
      break;
    case OP_SUB:
      val= " - ";
      break;
    case OP_MULT:
      val= " * ";
      break;
    case OP_DIV:
      val= " / ";
      break;
    default:
      val= " ? ";
    } /* endswitch */

  return val;
} // end of GetValStr


/***********************************************************************/
/*  Check the WHERE condition and return an ODBC/WQL filter.           */
/***********************************************************************/
PFIL ha_connect::CheckCond(PGLOBAL g, PFIL filp, AMT tty, Item *cond)
{
  unsigned int i;
  bool  ismul= false;
  PPARM pfirst= NULL, pprec= NULL, pp[2]= {NULL, NULL};
  OPVAL vop= OP_XX;

  if (!cond)
    return NULL;

  if (xtrace > 1)
    printf("Cond type=%d\n", cond->type());

  if (cond->type() == COND::COND_ITEM) {
    char      *p1, *p2;
    Item_cond *cond_item= (Item_cond *)cond;

    if (xtrace > 1)
      printf("Cond: Ftype=%d name=%s\n", cond_item->functype(),
                                         cond_item->func_name());

    switch (cond_item->functype()) {
      case Item_func::COND_AND_FUNC: vop= OP_AND; break;
      case Item_func::COND_OR_FUNC:  vop= OP_OR;  break;
      default: return NULL;
      } // endswitch functype

    List<Item>* arglist= cond_item->argument_list();
    List_iterator<Item> li(*arglist);
    Item *subitem;

    p1= filp + strlen(filp);
    strcpy(p1, "(");
    p2= p1 + 1;

    for (i= 0; i < arglist->elements; i++)
      if ((subitem= li++)) {
        if (!CheckCond(g, filp, tty, subitem)) {
          if (vop == OP_OR)
            return NULL;
          else
            *p2= 0;

        } else {
          p1= p2 + strlen(p2);
          strcpy(p1, GetValStr(vop, FALSE));
          p2= p1 + strlen(p1);
        } // endif CheckCond

      } else
        return NULL;

    if (*p1 != '(')
      strcpy(p1, ")");
    else
      return NULL;

  } else if (cond->type() == COND::FUNC_ITEM) {
    unsigned int i;
//  int   n;
    bool  iscol, neg= FALSE;
    Item_func *condf= (Item_func *)cond;
    Item*     *args= condf->arguments();

    if (xtrace > 1)
      printf("Func type=%d argnum=%d\n", condf->functype(),
                                         condf->argument_count());

//  neg= condf->

    switch (condf->functype()) {
      case Item_func::EQUAL_FUNC:
      case Item_func::EQ_FUNC: vop= OP_EQ;  break;
      case Item_func::NE_FUNC: vop= OP_NE;  break;
      case Item_func::LT_FUNC: vop= OP_LT;  break;
      case Item_func::LE_FUNC: vop= OP_LE;  break;
      case Item_func::GE_FUNC: vop= OP_GE;  break;
      case Item_func::GT_FUNC: vop= OP_GT;  break;
      case Item_func::IN_FUNC: vop= OP_IN;
        neg= ((Item_func_opt_neg *)condf)->negated;
      case Item_func::BETWEEN: ismul= true; break;
      default: return NULL;
      } // endswitch functype

    if (condf->argument_count() < 2)
      return NULL;
    else if (ismul && tty == TYPE_AM_WMI)
      return NULL;        // Not supported by WQL

    for (i= 0; i < condf->argument_count(); i++) {
      if (xtrace > 1)
        printf("Argtype(%d)=%d\n", i, args[i]->type());

      if (i >= 2 && !ismul) {
        if (xtrace > 1)
          printf("Unexpected arg for vop=%d\n", vop);

        continue;
        } // endif i

      if ((iscol= args[i]->type() == COND::FIELD_ITEM)) {
        const char *fnm;
        ha_field_option_struct *fop;
        Item_field *pField= (Item_field *)args[i];

        if (pField->field->table != table)
          return NULL;  // Field does not belong to this table
        else
          fop= GetFieldOptionStruct(pField->field);

        if (fop && fop->special) {
          if (tty == TYPE_AM_TBL && !stricmp(fop->special, "TABID"))
            fnm= "TABID";
          else
            return NULL;

        } else if (tty == TYPE_AM_TBL)
          return NULL;
        else
          fnm= pField->field->field_name;

        if (xtrace > 1) {
          printf("Field index=%d\n", pField->field->field_index);
          printf("Field name=%s\n", pField->field->field_name);
          } // endif xtrace

        // IN and BETWEEN clauses should be col VOP list
        if (i && ismul)
          return NULL;

        strcat(filp, fnm);
      } else {
        char   buff[256];
        String *res, tmp(buff,sizeof(buff), &my_charset_bin);
        Item_basic_constant *pval= (Item_basic_constant *)args[i];

        if ((res= pval->val_str(&tmp)) == NULL)
          return NULL;                      // To be clarified

        if (xtrace > 1)
          printf("Value=%.*s\n", res->length(), res->ptr());

        // IN and BETWEEN clauses should be col VOP list
        if (!i && ismul)
          return NULL;

        // Append the value to the filter
        if (args[i]->type() == COND::STRING_ITEM)
          strcat(strcat(strcat(filp, "'"), res->ptr()), "'");
        else
          strncat(filp, res->ptr(), res->length());

      } // endif

      if (!i)
        strcat(filp, GetValStr(vop, neg));
      else if (vop == OP_XX && i == 1)
        strcat(filp, " AND ");
      else if (vop == OP_IN)
        strcat(filp, (i == condf->argument_count() - 1) ? ")" : ",");

      } // endfor i

  } else {
    if (xtrace > 1)
      printf("Unsupported condition\n");

    return NULL;
  } // endif's type

  return filp;
} // end of CheckCond


 /**
   Push condition down to the table handler.

   @param  cond   Condition to be pushed. The condition tree must not be
                  modified by the caller.

   @return
     The 'remainder' condition that caller must use to filter out records.
     NULL means the handler will not return rows that do not match the
     passed condition.

   @note
     CONNECT handles the filtering only for table types that construct
     an SQL or WQL query, but still leaves it to MySQL because only some
     parts of the filter may be relevant.
     The first suballocate finds the position where the string will be
     constructed in the sarea. The second one does make the suballocation
     with the proper length.
 */
const COND *ha_connect::cond_push(const COND *cond)
{
  DBUG_ENTER("ha_connect::cond_push");

  if (tdbp) {
    AMT tty= tdbp->GetAmType();

    if (tty == TYPE_AM_WMI || tty == TYPE_AM_ODBC ||
        tty == TYPE_AM_TBL || tty == TYPE_AM_MYSQL) {
      PGLOBAL& g= xp->g;
      PFIL filp= (PFIL)PlugSubAlloc(g, NULL, 0);

      *filp= 0;

      if (CheckCond(g, filp, tty, (Item *)cond)) {
        if (xtrace)
          puts(filp);

        tdbp->SetFilter(filp);
//      cond= NULL;     // This does not work anyway
        PlugSubAlloc(g, NULL, strlen(filp) + 1);
        } // endif filp

      } // endif tty

    } // endif tdbp

  // Let MySQL do the filtering
  DBUG_RETURN(cond);
} // end of cond_push

/**
  Number of rows in table. It will only be called if
  (table_flags() & (HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT)) != 0
*/
ha_rows ha_connect::records()
{
  if (!valid_info)
    info(HA_STATUS_VARIABLE);

  if (tdbp && tdbp->Cardinality(NULL))
    return stats.records;
  else
    return HA_POS_ERROR;

} // end of records


/**
  Return an error message specific to this handler.

  @param error  error code previously returned by handler
  @param buf    pointer to String where to add error message

  @return
    Returns true if this is a temporary error
*/
bool ha_connect::get_error_message(int error, String* buf)
{
  DBUG_ENTER("ha_connect::get_error_message");

  if (xp && xp->g)
    buf->copy(xp->g->Message, (uint)strlen(xp->g->Message),
              system_charset_info);

  DBUG_RETURN(false);
} // end of get_error_message


/**
  @brief
  If frm_error() is called then we will use this to determine
  the file extensions that exist for the storage engine. This is also
  used by the default rename_table and delete_table method in
  handler.cc.

  For engines that have two file name extentions (separate meta/index file
  and data file), the order of elements is relevant. First element of engine
  file name extentions array should be meta/index file extention. Second
  element - data file extention. This order is assumed by
  prepare_for_repair() when REPAIR TABLE ... USE_FRM is issued.

  @note: PlugDB will handle all file creation/deletion. When dropping
  a CONNECT table, we don't want the PlugDB table to be dropped or erased.
  Therefore we provide a void list of extensions.

  @see
  rename_table method in handler.cc and
  delete_table method in handler.cc
*/
static const char *ha_connect_exts[]= {
  NullS
};


const char **ha_connect::bas_ext() const
{
  return ha_connect_exts;    // a null list, see @note above
} // end of bas_ext


/**
  @brief
  Used for opening tables. The name will be the name of the file.

  @details
  A table is opened when it needs to be opened; e.g. when a request comes in
  for a SELECT on the table (tables are not open and closed for each request,
  they are cached).

  Called from handler.cc by handler::ha_open(). The server opens all tables by
  calling ha_open() which then calls the handler specific open().

  @note
  For CONNECT no open can be done here because field information is not yet
  updated. >>>>> TO BE CHECKED <<<<<
  (Thread information could be get by using 'ha_thd')

  @see
  handler::ha_open() in handler.cc
*/
int ha_connect::open(const char *name, int mode, uint test_if_locked)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::open");

  if (xtrace)
     printf("open: name=%s mode=%d test=%ud\n", name, mode, test_if_locked);

  if (!(share= get_share(name, table)))
    DBUG_RETURN(1);

  thr_lock_data_init(&share->lock,&lock,NULL);

  // Try to get the user if possible
  if (table && table->in_use) {
    PGLOBAL g= GetPlug(table->in_use);

    // Try to set the database environment
    if (g)
      rc= (CntCheckDB(g, this, name)) ? (-2) : 0;

    } // endif table

  DBUG_RETURN(rc);
} // end of open

/**
  @brief
  Make the indexes for this table
*/
int ha_connect::optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  PDBUSER  dup= PlgGetUser(g);

  // Ignore error on the opt file
  dup->Check &= ~CHK_OPT;
  tdbp= GetTDB(g);
  dup->Check |= CHK_OPT;

  if (tdbp || (tdbp= GetTDB(g))) {
    if (!((PTDBASE)tdbp)->GetDef()->Indexable()) {
      sprintf(g->Message, "Table %s is not indexable", tdbp->GetName());
      rc= HA_ERR_INTERNAL_ERROR;
    } else
      if (((PTDBASE)tdbp)->ResetTableOpt(g, true))
        rc = HA_ERR_INTERNAL_ERROR;

  } else
    rc= HA_ERR_INTERNAL_ERROR;

  return rc;
} // end of optimize

/**
  @brief
  Closes a table. We call the free_share() function to free any resources
  that we have allocated in the "shared" structure.

  @details
  Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
  only used to close up temporary tables or during the process where a
  temporary table is converted over to being a myisam table.

  For sql_base.cc look at close_data_tables().

  @see
  sql_base.cc, sql_select.cc and table.cc
*/
int ha_connect::close(void)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::close");

  // If this is called by a later query, the table may have
  // been already closed and the tdbp is not valid anymore.
  if (tdbp && xp->last_query_id == valid_query_id)
    rc= CloseTable(xp->g);

  DBUG_RETURN(free_share(share) || rc);
} // end of close


/**
  @brief
  write_row() inserts a row. No extra() hint is given currently if a bulk load
  is happening. buf() is a byte array of data. You can use the field
  information to extract the data from the native byte array type.

    @details
  Example of this would be:
    @code
  for (Field **field=table->field ; *field ; field++)
  {
    ...
  }
    @endcode

  See ha_tina.cc for an example of extracting all of the data as strings.
  ha_berekly.cc has an example of how to store it intact by "packing" it
  for ha_berkeley's own native storage type.

  See the note for update_row() on auto_increments and timestamps. This
  case also applies to write_row().

  Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

    @see
  item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/
int ha_connect::write_row(uchar *buf)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("ha_connect::write_row");

  // Open the table if it was not opened yet (possible ???)
  if (!IsOpened())
    if (OpenTable(g)) {
      if (strstr(g->Message, "read only"))
        rc= HA_ERR_TABLE_READONLY;
      else
        rc= HA_ERR_INITIALIZATION;

      DBUG_RETURN(rc);
      } // endif tdbp

  if (tdbp->GetMode() == MODE_ANY)
    DBUG_RETURN(0);

  // Set column values from the passed pseudo record
  if ((rc= ScanRecord(g, buf)))
    DBUG_RETURN(rc);

  // Return result code from write operation
  if (CntWriteRow(g, tdbp)) {
    DBUG_PRINT("write_row", (g->Message));
    printf("write_row: %s\n", g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
    } // endif RC

  DBUG_RETURN(rc);
} // end of write_row


/**
  @brief
  Yes, update_row() does what you expect, it updates a row. old_data will have
  the previous row record in it, while new_data will have the newest data in it.
  Keep in mind that the server can do updates based on ordering if an ORDER BY
  clause was used. Consecutive ordering is not guaranteed.

    @details
  Currently new_data will not have an updated auto_increament record, or
  and updated timestamp field. You can do these for example by doing:
    @code
  if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
    table->timestamp_field->set_time();
  if (table->next_number_field && record == table->record[0])
    update_auto_increment();
    @endcode

  Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

    @see
  sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_connect::update_row(const uchar *old_data, uchar *new_data)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("ha_connect::update_row");

  if (xtrace > 1)
    printf("update_row: old=%s new=%s\n", old_data, new_data);

  // Check values for possible change in indexed column
  if ((rc= CheckRecord(g, old_data, new_data)))
    return rc;

  if (CntUpdateRow(g, tdbp)) {
    DBUG_PRINT("update_row", (g->Message));
    printf("update_row CONNECT: %s\n", g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
    } // endif RC

  DBUG_RETURN(rc);
} // end of update_row


/**
  @brief
  This will delete a row. buf will contain a copy of the row to be deleted.
  The server will call this right after the current row has been called (from
  either a previous rnd_nexT() or index call).

  @details
  If you keep a pointer to the last row or can access a primary key it will
  make doing the deletion quite a bit easier. Keep in mind that the server does
  not guarantee consecutive deletions. ORDER BY clauses can be used.

  Called in sql_acl.cc and sql_udf.cc to manage internal table
  information.  Called in sql_delete.cc, sql_insert.cc, and
  sql_select.cc. In sql_select it is used for removing duplicates
  while in insert it is used for REPLACE calls.

  @see
  sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/
int ha_connect::delete_row(const uchar *buf)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::delete_row");

  if (CntDeleteRow(xp->g, tdbp, false)) {
    rc= HA_ERR_INTERNAL_ERROR;
    printf("delete_row CONNECT: %s\n", xp->g->Message);
    } // endif DeleteRow

  DBUG_RETURN(rc);
} // end of delete_row


/****************************************************************************/
/*  We seem to come here at the begining of an index use.                   */
/****************************************************************************/
int ha_connect::index_init(uint idx, bool sorted)
{
  int rc;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("index_init");

  if ((rc= rnd_init(0)))
    return rc;

  indexing= CntIndexInit(g, tdbp, (signed)idx);

  if (indexing <= 0) {
    DBUG_PRINT("index_init", (g->Message));
    printf("index_init CONNECT: %s\n", g->Message);
    active_index= MAX_KEY;
    rc= HA_ERR_INTERNAL_ERROR;
  } else {
    if (((PTDBDOX)tdbp)->To_Kindex->GetNum_K()) {
      if (((PTDBASE)tdbp)->GetFtype() != RECFM_NAF)
        ((PTDBDOX)tdbp)->GetTxfp()->ResetBuffer(g);

      active_index= idx;
    } else        // Void table
      indexing= 0;

    rc= 0;
  } // endif indexing

  DBUG_RETURN(rc);
} // end of index_init

/****************************************************************************/
/*  We seem to come here at the end of an index use.                        */
/****************************************************************************/
int ha_connect::index_end()
{
  DBUG_ENTER("index_end");
  active_index= MAX_KEY;
  DBUG_RETURN(rnd_end());
} // end of index_end


/****************************************************************************/
/*  This is internally called by all indexed reading functions.             */
/****************************************************************************/
int ha_connect::ReadIndexed(uchar *buf, OPVAL op, const uchar *key, uint key_len)
{
  int rc;

//statistic_increment(ha_read_key_count, &LOCK_status);

  switch (CntIndexRead(xp->g, tdbp, op, key, (int)key_len)) {
    case RC_OK:
      xp->fnd++;
      rc= MakeRecord((char*)buf);
      break;
    case RC_EF:         // End of file
      rc= HA_ERR_END_OF_FILE;
      break;
    case RC_NF:         // Not found
      xp->nfd++;
      rc= (op == OP_SAME) ? HA_ERR_END_OF_FILE : HA_ERR_KEY_NOT_FOUND;
      break;
    default:          // Read error
      DBUG_PRINT("ReadIndexed", (xp->g->Message));
      printf("ReadIndexed: %s\n", xp->g->Message);
      rc= HA_ERR_INTERNAL_ERROR;
    } // endswitch RC

  if (xtrace > 1)
    printf("ReadIndexed: op=%d rc=%d\n", op, rc);

  table->status= (rc == RC_OK) ? 0 : STATUS_NOT_FOUND;
  return rc;
} // end of ReadIndexed


#ifdef NOT_USED
/**
  @brief
  Positions an index cursor to the index specified in the handle. Fetches the
  row if available. If the key value is null, begin at the first key of the
  index.
*/
int ha_connect::index_read_map(uchar *buf, const uchar *key,
                               key_part_map keypart_map __attribute__((unused)),
                               enum ha_rkey_function find_flag
                               __attribute__((unused)))
{
  DBUG_ENTER("ha_connect::index_read");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif // NOT_USED


/****************************************************************************/
/*  This is called by handler::index_read_map.                              */
/****************************************************************************/
int ha_connect::index_read(uchar * buf, const uchar * key, uint key_len,
                           enum ha_rkey_function find_flag)
{
  int rc;
  OPVAL op= OP_XX;
  DBUG_ENTER("ha_connect::index_read");

  switch(find_flag) {
    case HA_READ_KEY_EXACT:   op= OP_EQ; break;
    case HA_READ_AFTER_KEY:   op= OP_GT; break;
    case HA_READ_KEY_OR_NEXT: op= OP_GE; break;
    default: DBUG_RETURN(-1);
    } // endswitch find_flag

  if (xtrace > 1)
    printf("%p index_read: op=%d\n", this, op);

  if (indexing > 0)
    rc= ReadIndexed(buf, op, key, key_len);
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of index_read


/**
  @brief
  Used to read forward through the index.
*/
int ha_connect::index_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::index_next");
  //statistic_increment(ha_read_next_count, &LOCK_status);

  if (indexing > 0)
    rc= ReadIndexed(buf, OP_NEXT);
  else if (!indexing)
    rc= rnd_next(buf);
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of index_next


#ifdef NOT_USED
/**
  @brief
  Used to read backwards through the index.
*/
int ha_connect::index_prev(uchar *buf)
{
  DBUG_ENTER("ha_connect::index_prev");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif // NOT_USED


/**
  @brief
  index_first() asks for the first key in the index.

    @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

    @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_connect::index_first(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::index_first");

  if (indexing > 0)
    rc= ReadIndexed(buf, OP_FIRST);
  else if (indexing < 0)
    rc= HA_ERR_INTERNAL_ERROR;
  else if (CntRewindTable(xp->g, tdbp)) {
    table->status= STATUS_NOT_FOUND;
    rc= HA_ERR_INTERNAL_ERROR;
  } else
    rc= rnd_next(buf);

  DBUG_RETURN(rc);
} // end of index_first


#ifdef NOT_USED
/**
  @brief
  index_last() asks for the last key in the index.

    @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

    @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_connect::index_last(uchar *buf)
{
  DBUG_ENTER("ha_connect::index_last");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif // NOT_USED


/****************************************************************************/
/*  This is called to get more rows having the same index value.            */
/****************************************************************************/
int ha_connect::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  int rc;
  DBUG_ENTER("ha_connect::index_next_same");
//statistic_increment(ha_read_next_count, &LOCK_status);

  if (!indexing)
    rc= rnd_next(buf);
  else if (indexing > 0)
    rc= ReadIndexed(buf, OP_SAME);
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of index_next_same


/**
  @brief
  rnd_init() is called when the system wants the storage engine to do a table
  scan. See the example in the introduction at the top of this file to see when
  rnd_init() is called.

    @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
  and sql_update.cc.

    @note
  We always call open and extern_lock/start_stmt before comming here.

    @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_connect::rnd_init(bool scan)
{
  PGLOBAL g= ((table && table->in_use) ? GetPlug(table->in_use) :
              (xp) ? xp->g : NULL);
  DBUG_ENTER("ha_connect::rnd_init");

  if (xtrace)
    printf("%p in rnd_init: scan=%d\n", this, scan);

  if (g) {
    // Open the table if it was not opened yet (possible ???)
    if (!IsOpened()) {
      if (!table || xmod == MODE_INSERT)
        DBUG_RETURN(HA_ERR_INITIALIZATION);

      if (OpenTable(g, xmod == MODE_DELETE))
#if defined(MARIADB)
        DBUG_RETURN(HA_ERR_INITIALIZATION);
#else   // !MARIADB
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
#endif  // !MARIADB

    } else
      void(CntRewindTable(g, tdbp));      // Read from beginning

    } // endif g

  xp->nrd= xp->fnd= xp->nfd= 0;
  xp->tb1= my_interval_timer();
  DBUG_RETURN(0);
} // end of rnd_init

/**
  @brief
  Not described.

  @note
  The previous version said:
  Stop scanning of table. Note that this may be called several times during
  execution of a sub select.
  =====> This has been moved to external lock to avoid closing subselect tables.
*/
int ha_connect::rnd_end()
{
  int rc= 0;
  DBUG_ENTER("ha_connect::rnd_end");

  // If this is called by a later query, the table may have
  // been already closed and the tdbp is not valid anymore.
//  if (tdbp && xp->last_query_id == valid_query_id)
//    rc= CloseTable(xp->g);

  DBUG_RETURN(rc);
} // end of rnd_end


/**
  @brief
  This is called for each row of the table scan. When you run out of records
  you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
  The Field structure for the table is the key to getting data into buf
  in a manner that will allow the server to understand it.

    @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
  and sql_update.cc.

    @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_connect::rnd_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::rnd_next");
//statistic_increment(ha_read_rnd_next_count, &LOCK_status);

#if !defined(MARIADB)
  if (!tdbp)   // MySQL ignores error from rnd_init
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
#endif   // !MARIADB

  if (tdbp->GetMode() == MODE_ANY) {
    // We will stop on next read
    if (!stop) {
      stop= true;
      DBUG_RETURN(RC_OK);
    } else
      DBUG_RETURN(HA_ERR_END_OF_FILE);

    } // endif Mode

  switch (CntReadNext(xp->g, tdbp)) {
    case RC_OK:
      rc= MakeRecord((char*)buf);
      break;
    case RC_EF:         // End of file
      rc= HA_ERR_END_OF_FILE;
      break;
    case RC_NF:         // Not found
      rc= HA_ERR_RECORD_DELETED;
      break;
    default:            // Read error
      printf("rnd_next CONNECT: %s\n", xp->g->Message);
      rc= (records()) ? HA_ERR_INTERNAL_ERROR : HA_ERR_END_OF_FILE;
      break;
    } // endswitch RC

#ifndef DBUG_OFF
  if (rc || !(xp->nrd++ % 16384)) {
    ulonglong tb2= my_interval_timer();
    double elapsed= (double) (tb2 - xp->tb1) / 1000000000ULL;
    DBUG_PRINT("rnd_next", ("rc=%d nrd=%u fnd=%u nfd=%u sec=%.3lf\n",
                             rc, (uint)xp->nrd, (uint)xp->fnd,
                             (uint)xp->nfd, elapsed));
    xp->tb1= tb2;
    xp->fnd= xp->nfd= 0;
    } // endif nrd
#endif

  table->status= (!rc) ? 0 : STATUS_NOT_FOUND;
  DBUG_RETURN(rc);
} // end of rnd_next


/**
  @brief
  position() is called after each call to rnd_next() if the data needs
  to be ordered. You can do something like the following to store
  the position:
    @code
  my_store_ptr(ref, ref_length, current_position);
    @endcode

    @details
  The server uses ref to store data. ref_length in the above case is
  the size needed to store current_position. ref is just a byte array
  that the server will maintain. If you are using offsets to mark rows, then
  current_position should be the offset. If it is a primary key like in
  BDB, then it needs to be a primary key.

  Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

    @see
  filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_connect::position(const uchar *record)
{
  DBUG_ENTER("ha_connect::position");
  if (((PTDBASE)tdbp)->GetDef()->Indexable())
    my_store_ptr(ref, ref_length, (my_off_t)((PTDBASE)tdbp)->GetRecpos());
  DBUG_VOID_RETURN;
} // end of position


/**
  @brief
  This is like rnd_next, but you are given a position to use
  to determine the row. The position will be of the type that you stored in
  ref. You can use my_get_ptr(pos,ref_length) to retrieve whatever key
  or position you saved when position() was called.

    @details
  Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and sql_update.cc.

    @note
  Is this really useful? It was never called even when sorting.

    @see
  filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_connect::rnd_pos(uchar *buf, uchar *pos)
{
  int     rc;
  PTDBASE tp= (PTDBASE)tdbp;
  DBUG_ENTER("ha_connect::rnd_pos");

  if (!tp->SetRecpos(xp->g, (int)my_get_ptr(pos, ref_length)))
    rc= rnd_next(buf);
  else
    rc= HA_ERR_KEY_NOT_FOUND;

  DBUG_RETURN(rc);
} // end of rnd_pos


/**
  @brief
  ::info() is used to return information to the optimizer. See my_base.h for
  the complete description.

    @details
  Currently this table handler doesn't implement most of the fields really needed.
  SHOW also makes use of this data.

  You will probably want to have the following in your code:
    @code
  if (records < 2)
    records= 2;
    @endcode
  The reason is that the server will optimize for cases of only a single
  record. If, in a table scan, you don't know the number of records, it
  will probably be better to set records to two so you can return as many
  records as you need. Along with records, a few more variables you may wish
  to set are:
    records
    deleted
    data_file_length
    index_file_length
    delete_length
    check_time
  Take a look at the public variables in handler.h for more information.

  Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_table.cc, sql_union.cc, and sql_update.cc.

    @see
  filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc, sql_delete.cc,
  sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_table.cc,
  sql_union.cc and sql_update.cc
*/
int ha_connect::info(uint flag)
{
  bool    pure= false;
  PGLOBAL g= GetPlug((table) ? table->in_use : NULL);

  DBUG_ENTER("ha_connect::info");

  if (xtrace)
    printf("%p In info: flag=%u valid_info=%d\n", this, flag, valid_info);

  if (!valid_info) {
    // tdbp must be available to get updated info
    if (xp->CheckQuery(valid_query_id) || !tdbp) {
      if (xmod == MODE_ANY) {               // Pure info, not a query
        pure= true;
        xp->CheckCleanup();
        } // endif xmod

//    tdbp= OpenTable(g, xmod == MODE_DELETE);
      tdbp= GetTDB(g);
      } // endif tdbp

    valid_info= CntInfo(g, tdbp, &xinfo);
    } // endif valid_info

  if (flag & HA_STATUS_VARIABLE) {
    stats.records= xinfo.records;
    stats.deleted= 0;
    stats.data_file_length= xinfo.data_file_length;
    stats.index_file_length= 0;
    stats.delete_length= 0;
    stats.check_time= 0;
    stats.mean_rec_length= xinfo.mean_rec_length;
    } // endif HA_STATUS_VARIABLE

  if (flag & HA_STATUS_CONST) {
    // This is imported from the previous handler and must be reconsidered
    stats.max_data_file_length= LL(4294967295);
    stats.max_index_file_length= LL(4398046510080);
    stats.create_time= 0;
    data_file_name= xinfo.data_file_name;
    index_file_name= NULL;
//  sortkey= (uint) - 1;           // Table is not sorted
    ref_length= sizeof(int);      // Pointer size to row
    table->s->db_options_in_use= 03;
    stats.block_size= 1024;
    table->s->keys_in_use.set_prefix(table->s->keys);
    table->s->keys_for_keyread= table->s->keys_in_use;
//  table->s->keys_for_keyread.subtract(table->s->read_only_keys);
    table->s->db_record_offset= 0;
    } // endif HA_STATUS_CONST

  if (flag & HA_STATUS_ERRKEY) {
    errkey= 0;
    } // endif HA_STATUS_ERRKEY

  if (flag & HA_STATUS_TIME)
    stats.update_time= 0;

  if (flag & HA_STATUS_AUTO)
    stats.auto_increment_value= 1;

  if (tdbp && pure)
    CloseTable(g);        // Not used anymore

  DBUG_RETURN(0);
} // end of info


/**
  @brief
  extra() is called whenever the server wishes to send a hint to
  the storage engine. The myisam engine implements the most hints.
  ha_innodb.cc has the most exhaustive list of these hints.

  @note
  This is not yet implemented for CONNECT.

  @see
  ha_innodb.cc
*/
int ha_connect::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_connect::extra");
  DBUG_RETURN(0);
} // end of extra


/**
  @brief
  Used to delete all rows in a table, including cases of truncate and cases where
  the optimizer realizes that all rows will be removed as a result of an SQL statement.

    @details
  Called from item_sum.cc by Item_func_group_concat::clear(),
  Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
  Called from sql_delete.cc by mysql_delete().
  Called from sql_select.cc by JOIN::reinit().
  Called from sql_union.cc by st_select_lex_unit::exec().

    @see
  Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
  Item_func_group_concat::clear() in item_sum.cc;
  mysql_delete() in sql_delete.cc;
  JOIN::reinit() in sql_select.cc and
  st_select_lex_unit::exec() in sql_union.cc.
*/
int ha_connect::delete_all_rows()
{
  int     rc= 0;
  PGLOBAL g= xp->g;
  DBUG_ENTER("ha_connect::delete_all_rows");

  // Close and reopen the table so it will be deleted
  rc= CloseTable(g);

  if (!(OpenTable(g))) {
    if (CntDeleteRow(g, tdbp, true)) {
      printf("%s\n", g->Message);
      rc= HA_ERR_INTERNAL_ERROR;
      } // endif

  } else
    rc= HA_ERR_INITIALIZATION;

  DBUG_RETURN(rc);
} // end of delete_all_rows

/**
  @brief
  This create a lock on the table. If you are implementing a storage engine
  that can handle transacations look at ha_berkely.cc to see how you will
  want to go about doing this. Otherwise you should consider calling flock()
  here. Hint: Read the section "locking functions for mysql" in lock.cc to understand
  this.

    @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

    @note
  Following what we did in the MySQL XDB handler, we use this call to actually
  physically open the table. This could be reconsider when finalizing this handler
  design, which means we have a better understanding of what MariaDB does.

    @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
int ha_connect::external_lock(THD *thd, int lock_type)
{
  int rc= 0;
  bool del= false;
  MODE newmode;
  PGLOBAL g= GetPlug(thd);
  DBUG_ENTER("ha_connect::external_lock");

  if (xtrace)
    printf("%p external_lock: lock_type=%d\n", this, lock_type);

  if (!g)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  // Action will depend on lock_type
  switch (lock_type) {
    case F_WRLCK:
      newmode= MODE_WRITE;
      break;
    case F_RDLCK:
      newmode= MODE_READ;
      break;
    case F_UNLCK:
    default:
      newmode= MODE_ANY;
    } // endswitch mode

  if (newmode == MODE_ANY) {
    // This is unlocking, do it by closing the table
    if (xp->CheckQueryID())
      rc= 2;          // Logical error ???
    else if (tdbp) {
      if (tdbp->GetMode() == MODE_ANY && *tdbp->GetName() == '#'
                                      && xp->tabp) {
        PDOSDEF defp1= (PDOSDEF)((PTDBASE)tdbp)->GetDef();
        PDOSDEF defp2= (PDOSDEF)xp->tabp->GetDef();
        PIXDEF  xp1, xp2, sxp;

        // Look for new created indexes
        for (xp1= defp1->GetIndx(); xp1; xp1= xp1->GetNext()) {
          for (xp2= defp2->GetIndx(); xp2; xp2= xp2->GetNext())
            if (!stricmp(xp1->GetName(), xp2->GetName()))
              break;        // Index already made

          if (!xp2) {
            // Here we do make the index on tabp
            sxp= xp1->GetNext();
            xp1->SetNext(NULL);
            xp->tabp->MakeIndex(g, xp1, true);
            xp1->SetNext(sxp);
            } // endif xp2

          } // endfor xp1

        // Look for dropped indexes
        for (xp2= defp2->GetIndx(); xp2; xp2= xp2->GetNext()) {
          for (xp1= defp1->GetIndx(); xp1; xp1= xp1->GetNext())
            if (!stricmp(xp1->GetName(), xp2->GetName()))
              break;        // Index not to drop

          if (!xp1) {
            // Here we erase the index file
            sxp= xp2->GetNext();
            xp2->SetNext(NULL);
            defp2->DeleteIndexFile(g, xp2);
            xp2->SetNext(sxp);
            } // endif xp1

          } // endfor xp2

        } // endif Mode

      if (CloseTable(g))
        rc= HA_ERR_INTERNAL_ERROR;

    } // endif tdbp

    DBUG_RETURN(rc);
    } // endif MODE_ANY

  if (xtrace) {
    printf("%p external_lock: cmdtype=%d\n", this, thd->lex->sql_command);
    printf("Cmd=%.*s\n", (int) thd->query_string.length(),
                         thd->query_string.str());
    } // endif xtrace

  // Next code is temporarily replaced until sql_command is set
  stop= false;

  if (newmode == MODE_WRITE) {
    switch (thd->lex->sql_command) {
      case SQLCOM_INSERT:
      case SQLCOM_CREATE_TABLE:
      case SQLCOM_LOAD:
      case SQLCOM_INSERT_SELECT:
        newmode= MODE_INSERT;
        break;
//    case SQLCOM_REPLACE:
//    case SQLCOM_REPLACE_SELECT:
//      newmode= MODE_UPDATE;               // To be checked
//      break;
      case SQLCOM_DELETE:
      case SQLCOM_DELETE_MULTI:
        del= true;
      case SQLCOM_TRUNCATE:
        newmode= MODE_DELETE;
        break;
      case SQLCOM_UPDATE:
      case SQLCOM_UPDATE_MULTI:
        newmode= MODE_UPDATE;
        break;
      case SQLCOM_SELECT:
      case SQLCOM_OPTIMIZE:
        newmode= MODE_READ;
        break;
      case SQLCOM_DROP_TABLE:
      case SQLCOM_RENAME_TABLE:
      case SQLCOM_ALTER_TABLE:
        newmode= MODE_ANY;
        break;
      case SQLCOM_DROP_INDEX:
      case SQLCOM_CREATE_INDEX:
        newmode= MODE_ANY;
        stop= true;
        break;
      default:
        printf("Unsupported sql_command=%d", thd->lex->sql_command);
        sprintf(g->Message, "Unsupported sql_command=%d", thd->lex->sql_command);
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      } // endswitch newmode

  } else if (newmode == MODE_READ) {
    switch (thd->lex->sql_command) {
      case SQLCOM_INSERT:
      case SQLCOM_CREATE_TABLE:
      case SQLCOM_LOAD:
      case SQLCOM_INSERT_SELECT:
//    case SQLCOM_REPLACE:
//    case SQLCOM_REPLACE_SELECT:
      case SQLCOM_DELETE:
      case SQLCOM_DELETE_MULTI:
      case SQLCOM_TRUNCATE:
      case SQLCOM_UPDATE:
      case SQLCOM_UPDATE_MULTI:
      case SQLCOM_SELECT:
      case SQLCOM_OPTIMIZE:
        break;
      case SQLCOM_DROP_INDEX:
      case SQLCOM_CREATE_INDEX:
        stop= true;
      case SQLCOM_DROP_TABLE:
      case SQLCOM_RENAME_TABLE:
      case SQLCOM_ALTER_TABLE:
        newmode= MODE_ANY;
        break;
      default:
        printf("Unsupported sql_command=%d", thd->lex->sql_command);
        sprintf(g->Message, "Unsupported sql_command=%d", thd->lex->sql_command);
        DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      } // endswitch newmode

  } // endif's newmode


  if (xtrace)
    printf("New mode=%d\n", newmode);

  // If this is the start of a new query, cleanup the previous one
  if (xp->CheckCleanup()) {
    tdbp= NULL;
    valid_info= false;
    } // endif CheckCleanup

  if (xtrace)
    printf("Calling CntCheckDB db=%s\n", GetDBName(NULL));

  // Set or reset the good database environment
  if (CntCheckDB(g, this, GetDBName(NULL))) {
    printf("%p external_lock: %s\n", this, g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
  // This can NOT be called without open called first, but
  // the table can have been closed since then
  } else if (!tdbp || xp->CheckQuery(valid_query_id) || xmod != newmode) {
    if (tdbp)
      CloseTable(g);

    xmod= newmode;

    if (!table)
      rc= 3;          // Logical error

    // Delay open until used fields are known
  } // endif tdbp

  if (xtrace)
    printf("external_lock: rc=%d\n", rc);

  DBUG_RETURN(rc);
} // end of external_lock


/**
  @brief
  The idea with handler::store_lock() is: The statement decides which locks
  should be needed for the table. For updates/deletes/inserts we get WRITE
  locks, for SELECT... we get read locks.

    @details
  Before adding the lock into the table lock handler (see thr_lock.c),
  mysqld calls store lock with the requested locks. Store lock can now
  modify a write lock to a read lock (or some other lock), ignore the
  lock (if we don't want to use MySQL table locks at all), or add locks
  for many tables (like we do when we are using a MERGE handler).

  Berkeley DB, for example, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
  (which signals that we are doing WRITES, but are still allowing other
  readers and writers).

  When releasing locks, store_lock() is also called. In this case one
  usually doesn't have to do anything.

  In some exceptional cases MySQL may send a request for a TL_IGNORE;
  This means that we are requesting the same lock as last time and this
  should also be ignored. (This may happen when someone does a flush
  table when we have opened a part of the tables, in which case mysqld
  closes and reopens the tables and tries to get the same locks at last
  time). In the future we will probably try to remove this.

  Called from lock.cc by get_lock_data().

    @note
  In this method one should NEVER rely on table->in_use, it may, in fact,
  refer to a different thread! (this happens if get_lock_data() is called
  from mysql_lock_abort_for_thread() function)

    @see
  get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_connect::store_lock(THD *thd,
                                       THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type=lock_type;
  *to++ = &lock;
  return to;
}


/**
  @brief
  Used to delete a table. By the time delete_table() has been called all
  opened references to this table will have been closed (and your globally
  shared references released). The variable name will just be the name of
  the table. You will need to remove any files you have created at this point.

    @details
  If you do not implement this, the default delete_table() is called from
  handler.cc and it will delete all files with the file extensions returned
  by bas_ext().

  Called from handler.cc by delete_table and ha_create_table(). Only used
  during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
  the storage engine.

    @see
  delete_table and ha_create_table() in handler.cc
*/
int ha_connect::delete_table(const char *name)
{
  DBUG_ENTER("ha_connect::delete_table");
  /* This is not implemented but we want someone to be able that it works. */
  DBUG_RETURN(0);
}


/**
  @brief
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.

  @details
  end_key may be empty, in which case determine if start_key matches any rows.

  Called from opt_range.cc by check_quick_keys().

  @see
  check_quick_keys() in opt_range.cc
*/
ha_rows ha_connect::records_in_range(uint inx, key_range *min_key,
                                               key_range *max_key)
{
  ha_rows rows;
  DBUG_ENTER("ha_connect::records_in_range");

  if (indexing < 0 || inx != active_index)
    index_init(inx, false);

  if (xtrace)
    printf("records_in_range: inx=%d indexing=%d\n", inx, indexing);

  if (indexing > 0) {
    int          nval;
    uint         len[2];
    const uchar *key[2];
    bool         incl[2];
    key_part_map kmap[2];

    key[0]= (min_key) ? min_key->key : NULL;
    key[1]= (max_key) ? max_key->key : NULL;
    len[0]= (min_key) ? min_key->length : 0;
    len[1]= (max_key) ? max_key->length : 0;
    incl[0]= (min_key) ? (min_key->flag == HA_READ_KEY_EXACT) : false;
    incl[1]= (max_key) ? (max_key->flag == HA_READ_AFTER_KEY) : false;
    kmap[0]= (min_key) ? min_key->keypart_map : 0;
    kmap[1]= (max_key) ? max_key->keypart_map : 0;

    if ((nval= CntIndexRange(xp->g, tdbp, key, len, incl, kmap)) < 0)
      rows= HA_POS_ERROR;
    else
      rows= (ha_rows)nval;

  } else if (indexing < 0)
    rows= HA_POS_ERROR;
  else
    rows= 100000000;        // Don't use missing index

  DBUG_RETURN(rows);
} // end of records_in_range

#if defined(MARIADB)
/**
  Convert an ISO-8859-1 column name to UTF-8
*/
char *ha_connect::encode(PGLOBAL g, char *cnm)
  {
  char  *buf= (char*)PlugSubAlloc(g, NULL, strlen(cnm) * 3);
  uint   dummy_errors;
  uint32 len= copy_and_convert(buf, strlen(cnm) * 3,
                               &my_charset_utf8_general_ci,
                               cnm, strlen(cnm),
                               &my_charset_latin1,
                               &dummy_errors);
  buf[len]= '\0';
  return buf;
  } // end of Encode

/**
  Store field definition for create.

  @return
    Return 0 if ok
*/

bool ha_connect::add_fields(THD *thd, void *alt_info,
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
           void *vcolinfo,
           engine_option_value *create_options)
{
  register Create_field *new_field;
  Alter_info *alter_info= (Alter_info*)alt_info;
  Virtual_column_info *vcol_info= (Virtual_column_info *)vcolinfo;

  DBUG_ENTER("ha_connect::add_fields");

  if (check_string_char_length(field_name, "", NAME_CHAR_LEN,
                               system_charset_info, 1))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), field_name->str); /* purecov: inspected */
    DBUG_RETURN(1);       /* purecov: inspected */
  }
#if 0
  if (type_modifier & PRI_KEY_FLAG)
  {
    Key *key;
    lex->col_list.push_back(new Key_part_spec(*field_name, 0));
    key= new Key(Key::PRIMARY, null_lex_str,
                      &default_key_create_info,
                      0, lex->col_list, NULL);
    alter_info->key_list.push_back(key);
    lex->col_list.empty();
  }
  if (type_modifier & (UNIQUE_FLAG | UNIQUE_KEY_FLAG))
  {
    Key *key;
    lex->col_list.push_back(new Key_part_spec(*field_name, 0));
    key= new Key(Key::UNIQUE, null_lex_str,
                 &default_key_create_info, 0,
                 lex->col_list, NULL);
    alter_info->key_list.push_back(key);
    lex->col_list.empty();
  }

  if (default_value)
  {
    /*
      Default value should be literal => basic constants =>
      no need fix_fields()

      We allow only one function as part of default value -
      NOW() as default for TIMESTAMP type.
    */
    if (default_value->type() == Item::FUNC_ITEM &&
        !(((Item_func*)default_value)->functype() == Item_func::NOW_FUNC &&
         type == MYSQL_TYPE_TIMESTAMP))
    {
      my_error(ER_INVALID_DEFAULT, MYF(0), field_name->str);
      DBUG_RETURN(1);
    }
    else if (default_value->type() == Item::NULL_ITEM)
    {
      default_value= 0;
      if ((type_modifier & (NOT_NULL_FLAG | AUTO_INCREMENT_FLAG)) ==
    NOT_NULL_FLAG)
      {
  my_error(ER_INVALID_DEFAULT, MYF(0), field_name->str);
  DBUG_RETURN(1);
      }
    }
    else if (type_modifier & AUTO_INCREMENT_FLAG)
    {
      my_error(ER_INVALID_DEFAULT, MYF(0), field_name->str);
      DBUG_RETURN(1);
    }
  }

  if (on_update_value && type != MYSQL_TYPE_TIMESTAMP)
  {
    my_error(ER_INVALID_ON_UPDATE, MYF(0), field_name->str);
    DBUG_RETURN(1);
  }
#endif // 0

  if (!(new_field= new Create_field()) ||
      new_field->init(thd, field_name->str, type, length, decimals, type_modifier,
                      NULL, NULL, comment, NULL,
                      NULL, cs, 0, vcol_info,
                      create_options))
    DBUG_RETURN(1);

  alter_info->create_list.push_back(new_field);
//lex->last_field=new_field;
  DBUG_RETURN(0);
} // end of add_fields

/**
  @brief
  pre_create() is called when creating a table with no columns.

  @details
  When pre_create() is called  the .frm file have not already been
  created. You can overwrite some definitions at this point but the
  main purpose of it is to define the columns for some table types.

  @note
  Not really implemented yet.
*/
bool ha_connect::pre_create(THD *thd, HA_CREATE_INFO *create_info,
                            void *alt_info)
{
  char        spc= ',', qch= 0;
  const char *typn= "?";
  const char *fncn= "?";
  const char *user;
  char       *fn, *dsn, *tab, *db, *host, *pwd, *prt, *sep; // *csn;
#if defined(WIN32)
  char       *nsp= NULL, *cls= NULL;
#endif   // WIN32
  int         port= MYSQL_PORT, hdr= 0, mxr= 0;
  uint        tm, fnc= FNC_NO, supfnc= (FNC_NO | FNC_COL);
  bool        b= false, ok= false, dbf= false;
  TABTYPE     ttp= TAB_UNDEF;
  LEX_STRING *comment, *name, *val;
  MEM_ROOT   *mem= thd->mem_root;
  CHARSET_INFO *cs;
  Alter_info *alter_info= (Alter_info*)alt_info;
  engine_option_value *pov, *start= create_info->option_list, *end= NULL;
  PQRYRES     qrp;
  PCOLRES     crp;
  PGLOBAL     g= GetPlug(thd);

  if (!g)
    return true;

  fn= dsn= tab= db= host= pwd= prt= sep= NULL;
  user= NULL;

  // Get the useful create options
  for (pov= start; pov; pov= pov->next) {
    if (!stricmp(pov->name.str, "table_type")) {
      typn= pov->value.str;
      ttp= GetTypeID(typn);
    } else if (!stricmp(pov->name.str, "file_name")) {
      fn= pov->value.str;
    } else if (!stricmp(pov->name.str, "tabname")) {
      tab= pov->value.str;
    } else if (!stricmp(pov->name.str, "dbname")) {
      db= pov->value.str;
    } else if (!stricmp(pov->name.str, "catfunc")) {
      fncn= pov->value.str;
      fnc= GetFuncID(fncn);
    } else if (!stricmp(pov->name.str, "sep_char")) {
      sep= pov->value.str;
      spc= (!strcmp(sep, "\\t")) ? '\t' : *sep;
    } else if (!stricmp(pov->name.str, "qchar")) {
      qch= *pov->value.str;
    } else if (!stricmp(pov->name.str, "quoted")) {
      if (!qch)
        qch= '"';

    } else if (!stricmp(pov->name.str, "header")) {
      hdr= atoi(pov->value.str);
    } else if (!stricmp(pov->name.str, "option_list")) {
      host= GetListOption("host", pov->value.str, "localhost");
      user= GetListOption("user", pov->value.str, "root");
      // Default value db can come from the DBNAME=xxx option.
      db= GetListOption("database", pov->value.str, db);
      pwd= GetListOption("password", pov->value.str);
      prt= GetListOption("port", pov->value.str);
      port= (prt) ? atoi(prt) : MYSQL_PORT;
#if defined(WIN32)
      nsp= GetListOption("namespace", pov->value.str);
      cls= GetListOption("class", pov->value.str);
#endif   // WIN32
      mxr= atoi(GetListOption("maxerr", pov->value.str, "0"));
    } // endelse option_list

    end= pov;
    } // endfor pov

  if (!db)
    db= thd->db;                     // Default value

  // Check table type
  if (ttp == TAB_UNDEF || ttp == TAB_NIY) {
    sprintf(g->Message, "Unknown Table_type '%s'", typn);
    push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN, 0, g->Message);
    strcpy(g->Message, "Using Table_type DOS");
    push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN, 0, g->Message);
    ttp= TAB_DOS;
    typn= "DOS";
    name= thd->make_lex_string(NULL, "table_type", 10, true);
    val= thd->make_lex_string(NULL, typn, strlen(typn), true);
    pov= new(mem) engine_option_value(*name, *val, false, &start, &end);
    } // endif ttp

  if (!tab && !(fnc & (FNC_TABLE | FNC_COL)))
    tab= (char*)create_info->alias;

  switch (ttp) {
#if defined(ODBC_SUPPORT)
    case TAB_ODBC:
      if (!(dsn= create_info->connect_string.str)
                 && !(fnc & (FNC_DSN | FNC_DRIVER)))
        sprintf(g->Message, "Missing %s connection string", typn);
      else
        ok= true;

      supfnc |= (FNC_TABLE | FNC_DSN | FNC_DRIVER);
      break;
#endif   // ODBC_SUPPORT
    case TAB_DBF:
      dbf= true;
      // Passthru
    case TAB_CSV:
      if (!fn)
        sprintf(g->Message, "Missing %s file name", typn);
      else
        ok= true;

      break;
#if defined(MYSQL_SUPPORT)
    case TAB_MYSQL:
      ok= true;

      if ((dsn= create_info->connect_string.str)) {
        PDBUSER dup= PlgGetUser(g);
        PCATLG  cat= (dup) ? dup->Catalog : NULL;
        PMYDEF  mydef= new(g) MYSQLDEF();

        dsn= (char*)PlugSubAlloc(g, NULL, strlen(dsn) + 1);
        strncpy(dsn, create_info->connect_string.str,
                     create_info->connect_string.length);
        dsn[create_info->connect_string.length] = 0;
        mydef->Name= (char*)create_info->alias;
        mydef->Cat= cat;

        if (!mydef->ParseURL(g, dsn)) {
          host= mydef->Hostname;
          user= mydef->Username;
          pwd=  mydef->Password;
          db=   mydef->Database;
          tab=  mydef->Tabname;
          port= mydef->Portnumber;
        } else
          ok= false;

      } else if (!user)
        user= "root";       // Avoid crash

      break;
#endif   // MYSQL_SUPPORT
#if defined(WIN32)
    case TAB_WMI:
      ok= true;
      break;
#endif   // WIN32
    default:
      sprintf(g->Message, "Cannot get column info for table type %s", typn);
    } // endif ttp

  // Check for supported catalog function
  if (ok && !(supfnc & fnc)) {
    sprintf(g->Message, "Unsupported catalog function %s for table type %s",
                        fncn, typn);
    ok= false;
    } // endif supfnc

  // If file name is not specified, set a default file name
  // in the database directory from alias.type.
  if (IsFileType(ttp) && !fn) {
    char buf[256];

    strcat(strcat(strcpy(buf, (char*)create_info->alias), "."), typn); 
    name= thd->make_lex_string(NULL, "file_name", 9, true);
    val= thd->make_lex_string(NULL, buf, strlen(buf), true);
    pov= new(mem) engine_option_value(*name, *val, false, &start, &end);
    sprintf(g->Message, "Unspecified file name was set to %s", buf);
    push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN, 0, g->Message);
    } // endif ttp && fn

  // Test whether columns must be specified
  if (alter_info->create_list.elements)
    return false;

  if (ok) {
    char *length, *decimals, *cnm, *rem;
    int   i, len, dec, typ;
    enum_field_types type;
    PDBUSER dup= PlgGetUser(g);
    PCATLG  cat= (dup) ? dup->Catalog : NULL;

    if (cat)
      cat->SetDataPath(g, thd->db);
    else
      return true;           // Should never happen

    switch (ttp) {
      case TAB_DBF:
        qrp= DBFColumns(g, fn, fnc == FNC_COL);
        break;
#if defined(ODBC_SUPPORT)
      case TAB_ODBC:
        switch (fnc) {
          case FNC_NO:
          case FNC_COL:
            qrp= ODBCColumns(g, dsn, tab, NULL, fnc == FNC_COL);
            break;
          case FNC_TABLE:
            qrp= ODBCTables(g, dsn, tab, true);
            break;
          case FNC_DSN:
            qrp= ODBCDataSources(g, true);
            break;
          case FNC_DRIVER:
            qrp= ODBCDrivers(g, true);
            break;
          default:
            sprintf(g->Message, "invalid catfunc %s", fncn);
        } // endswitch info

        break;
#endif   // ODBC_SUPPORT
#if defined(MYSQL_SUPPORT)
      case TAB_MYSQL:
        qrp= MyColumns(g, host, db, user, pwd, tab, 
                       NULL, port, false, fnc == FNC_COL);
        break;
#endif   // MYSQL_SUPPORT
      case TAB_CSV:
        qrp= CSVColumns(g, fn, spc, qch, hdr, mxr, fnc == FNC_COL);
        break;
#if defined(WIN32)
      case TAB_WMI:
        qrp= WMIColumns(g, nsp, cls, fnc == FNC_COL);
        break;
#endif   // WIN32
      default:
        strcpy(g->Message, "System error in pre_create");
        break;
      } // endswitch ttp

    if (!qrp) {
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      return true;
      } // endif qrp

    if (fnc != FNC_NO) {
      // Catalog table
      for (crp=qrp->Colresp; !b && crp; crp= crp->Next) {
        cnm= encode(g, crp->Name);
        name= thd->make_lex_string(NULL, cnm, strlen(cnm), true);
        type= PLGtoMYSQL(crp->Type, dbf);
        len= crp->Length;
        length= (char*)PlugSubAlloc(g, NULL, 8);
        sprintf(length, "%d", len);
        decimals= NULL;
        comment= thd->make_lex_string(NULL, "", 0, true);
     
        // Now add the field
        b= add_fields(thd, alt_info, name, type, length, decimals,
                      NOT_NULL_FLAG, comment, NULL, NULL, NULL);
        } // endfor crp

    } else              // Not a catalog table
      for (i= 0; !b && i < qrp->Nblin; i++) {
        rem= "";
        typ= len= dec= 0;
        length= "";
        decimals= NULL;
        tm= NOT_NULL_FLAG;
        cs= NULL;

        for (crp= qrp->Colresp; crp; crp= crp->Next)
          switch (crp->Fld) {
            case FLD_NAME:
              cnm= encode(g, crp->Kdata->GetCharValue(i));
              name= thd->make_lex_string(NULL, cnm, strlen(cnm), true);
              break;
            case FLD_TYPE:
              typ= crp->Kdata->GetIntValue(i);
              break;
            case FLD_PREC:
              len= crp->Kdata->GetIntValue(i);
              break;
            case FLD_SCALE:
              if ((dec= crp->Kdata->GetIntValue(i))) {
                decimals= (char*)PlugSubAlloc(g, NULL, 8);
                 sprintf(decimals, "%d", dec);
              } else
                decimals= NULL;

              break;
            case FLD_NULL:
              if (crp->Kdata->GetIntValue(i))
                tm= 0;               // Nullable

              break;
            case FLD_REM:
              rem= crp->Kdata->GetCharValue(i);
              break;
//          case FLD_CHARSET:    
              // No good because remote table is already translated
//            if (*(csn= crp->Kdata->GetCharValue(i)))
//              cs= get_charset_by_name(csn, 0);

//            break;
            default:
              break;                 // Ignore
            } // endswitch Fld

#if defined(ODBC_SUPPORT)
        if (ttp == TAB_ODBC) {
          int plgtyp;

          // typ must be PLG type, not SQL type
          if (!(plgtyp= TranslateSQLType(typ, dec, len))) {
            sprintf(g->Message, "Unsupported SQL type %d", typ);
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            return true;
          } else
            typ= plgtyp;

          // Some data sources do not count dec in length
          if (typ == TYPE_FLOAT)
            len += (dec + 2);        // To be safe

          } // endif ttp
#endif   // ODBC_SUPPORT

        // Make the arguments as required by add_fields
        type= PLGtoMYSQL(typ, true);
        length= (char*)PlugSubAlloc(g, NULL, 8);
        sprintf(length, "%d", len);
        comment= thd->make_lex_string(NULL, rem, strlen(rem), true);
     
        // Now add the field
        b= add_fields(thd, alt_info, name, type, length, decimals,
                      tm, comment, cs, NULL, NULL);
        } // endfor i

    return b;
    } // endif ok

  my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
  return true;
} // end of pre_create
#endif   // MARIADB

/**
  @brief
  create() is called to create a database. The variable name will have the name
  of the table.

  @details
  When create() is called you do not need to worry about
  opening the table. Also, the .frm file will have already been
  created so adjusting create_info is not necessary. You can overwrite
  the .frm file at this point if you wish to change the table
  definition, but there are no methods currently provided for doing
  so.

  Called from handle.cc by ha_create_table().

  @note
  Currently we do some checking on the create definitions and stop
  creating if an error is found. We wish we could change the table
  definition such as providing a default table type. However, as said
  above, there are no method to do so.

  @see
  ha_create_table() in handle.cc
*/

int ha_connect::create(const char *name, TABLE *table_arg,
                       HA_CREATE_INFO *create_info)
{
  int     rc= RC_OK;
  bool    dbf;
  Field* *field;
  Field  *fp;
  TABLE  *st= table;                       // Probably unuseful
  PIXDEF  xdp, pxd= NULL, toidx= NULL;
  PGLOBAL g= GetPlug(table_arg->in_use);

  DBUG_ENTER("ha_connect::create");
  PTOS options= GetTableOptionStruct(table_arg);

  // CONNECT engine specific table options:
  DBUG_ASSERT(options);

  if (options->data_charset)
  {
    const CHARSET_INFO *data_charset;
    if (!(data_charset= get_charset_by_csname(options->data_charset,
                                              MY_CS_PRIMARY, MYF(0))))
    {
      my_error(ER_UNKNOWN_CHARACTER_SET, MYF(0), options->data_charset);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
    if (GetTypeID(options->type) == TAB_XML &&
        data_charset != &my_charset_utf8_general_ci)
    {
      my_printf_error(ER_UNKNOWN_ERROR,
                      "DATA_CHARSET='%s' is not supported for TABLE_TYPE=XML",
                        MYF(0), options->data_charset);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    }
  }

  if (!g) {
    rc= HA_ERR_INTERNAL_ERROR;
    DBUG_RETURN(rc);
  } else
    dbf= (GetTypeID(options->type) == TAB_DBF);

  // Check column types
  for (field= table_arg->field; *field; field++) {
    fp= *field;

#if defined(MARIADB)
    if (fp->vcol_info && !fp->stored_in_db)
      continue;            // This is a virtual column
#endif   // MARIADB

    switch (fp->type()) {
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_LONGLONG:
        break;                     // Ok
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_INT24:
        break;                     // To be checked
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_GEOMETRY:
      default:
//      fprintf(stderr, "Unsupported type column %s\n", fp->field_name);
        sprintf(g->Message, "Unsupported type for column %s",
                            fp->field_name);
        rc= HA_ERR_INTERNAL_ERROR;
        my_printf_error(ER_UNKNOWN_ERROR,
                        "Unsupported type for column '%s'",
                        MYF(0), fp->field_name);
        DBUG_RETURN(rc);
      } // endswitch type


    if (dbf) {
      bool b= false;

      if ((b= strlen(fp->field_name) > 10))
        sprintf(g->Message, "DBF: Column name '%s' is too long (max=10)",
                            fp->field_name);
      else if ((b= fp->field_length > 255))
        sprintf(g->Message, "DBF: Column length too big for '%s' (max=255)",
                            fp->field_name);

      if (b) {
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        rc= HA_ERR_INTERNAL_ERROR;
        DBUG_RETURN(rc);
        } // endif b

      } // endif dbf

    } // endfor field

  // Check whether indexes were specified
  table= table_arg;       // Used by called functions

  // Get the index definitions
  for (int n= 0; (unsigned)n < table->s->keynames.count; n++) {
    if (xtrace)
      printf("Getting created index %d info\n", n + 1);

    xdp= GetIndexInfo(n);

    if (pxd)
      pxd->SetNext(xdp);
    else
      toidx= xdp;

    pxd= xdp;
    } // endfor n

  if (toidx) {
    PDBUSER dup= PlgGetUser(g);
    PCATLG  cat= (dup) ? dup->Catalog : NULL;

    DBUG_ASSERT(cat);

    if (cat)
      cat->SetDataPath(g, table_arg->in_use->db);

    if ((rc= optimize(NULL, NULL))) {
      printf("Create rc=%d %s\n", rc, g->Message);
      rc= HA_ERR_INTERNAL_ERROR;
    } else
      CloseTable(g);

    } // endif toidx

  table= st;
  DBUG_RETURN(rc);
} // end of create


/**
  check_if_incompatible_data() called if ALTER TABLE can't detect otherwise
  if new and old definition are compatible

  @details If there are no other explicit signs like changed number of
  fields this function will be called by compare_tables()
  (sql/sql_tables.cc) to decide should we rewrite whole table or only .frm
  file.

*/

bool ha_connect::check_if_incompatible_data(HA_CREATE_INFO *info,
                                        uint table_changes)
{
//ha_table_option_struct *param_old, *param_new;
  DBUG_ENTER("ha_connect::check_if_incompatible_data");
  // TO DO: implement it.
  if (table)
    push_warning(table->in_use, MYSQL_ERROR::WARN_LEVEL_WARN, 0, 
      "The current version of CONNECT did not check what you changed in ALTER. Use on your own risk");
  DBUG_RETURN(COMPATIBLE_DATA_YES);
}


struct st_mysql_storage_engine connect_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

struct st_mysql_daemon unusable_connect=
{ MYSQL_DAEMON_INTERFACE_VERSION };

mysql_declare_plugin(connect)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &connect_storage_engine,
  "CONNECT",
  "Olivier Bertrand",
  "Direct access to external data, including many file formats",
  PLUGIN_LICENSE_GPL,
  connect_init_func,                                /* Plugin Init */
  connect_done_func,                                /* Plugin Deinit */
  0x0001 /* 0.1 */,
  NULL,                                         /* status variables */
  NULL,                                         /* system variables */
  NULL,                                         /* config options */
  0,                                            /* flags */
}
mysql_declare_plugin_end;

#if defined(MARIADB)
maria_declare_plugin(connect)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &connect_storage_engine,
  "CONNECT",
  "Olivier Bertrand",
  "Direct access to external data, including many file formats",
  PLUGIN_LICENSE_GPL,
  connect_init_func,                            /* Plugin Init */
  connect_done_func,                            /* Plugin Deinit */
  0x0001,                                       /* version number (0.1) */
  NULL,                                         /* status variables */
  NULL,                                         /* system variables */
  "0.1",                                        /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
},
{
  MYSQL_DAEMON_PLUGIN,
  &unusable_connect,
  "UNUSABLE",
  "Olivier Bertrand",
  "Unusable Daemon",
  PLUGIN_LICENSE_PROPRIETARY,
  NULL,                                         /* Plugin Init */
  NULL,                                         /* Plugin Deinit */
  0x0101,                                       /* version number (1.1) */
  NULL,                                         /* status variables */
  NULL,                                         /* system variables */
  "1.01.00.000" ,                               /* version, as a string */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
}
maria_declare_plugin_end;
#endif   // MARIADB
