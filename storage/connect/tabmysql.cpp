/************* TabMySQL C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: TABMYSQL                                               */
/* -------------                                                        */
/*  Version 1.7                                                         */
/*                                                                      */
/* AUTHOR:                                                              */
/* -------                                                              */
/*  Olivier BERTRAND                                      2007-2013     */
/*                                                                      */
/* WHAT THIS PROGRAM DOES:                                              */
/* -----------------------                                              */
/*  Implements a table type that are MySQL tables.                      */
/*  It can optionally use the embedded MySQL library.                   */
/*                                                                      */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                               */
/* --------------------------------------                               */
/*                                                                      */
/*  REQUIRED FILES:                                                     */
/*  ---------------                                                     */
/*    TABMYSQL.CPP   - Source code                                      */
/*    PLGDBSEM.H     - DB application declaration file                  */
/*    TABMYSQL.H     - TABODBC classes declaration file                 */
/*    GLOBAL.H       - Global declaration file                          */
/*                                                                      */
/*  REQUIRED LIBRARIES:                                                 */
/*  -------------------                                                 */
/*    Large model C library                                             */
/*                                                                      */
/*  REQUIRED PROGRAMS:                                                  */
/*  ------------------                                                  */
/*    IBM, Borland, GNU or Microsoft C++ Compiler and Linker            */
/*                                                                      */
/************************************************************************/
#include "my_global.h"
#if defined(WIN32)
//#include <windows.h>
#else   // !WIN32
//#include <fnmatch.h>
//#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "osutil.h"
//#include <io.h>
//#include <fcntl.h>
#endif  // !WIN32

/***********************************************************************/
/*  Include application header files:                                  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "tabcol.h"
#include "colblk.h"
#include "mycat.h"
#include "reldef.h"
#include "tabmysql.h"
#include "valblk.h"
#include "tabutil.h"

#if defined(_CONSOLE)
void PrintResult(PGLOBAL, PSEM, PQRYRES);
#endif   // _CONSOLE

extern "C" int   trace;
extern MYSQL_PLUGIN_IMPORT uint mysqld_port;

/* -------------- Implementation of the MYSQLDEF class --------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
MYSQLDEF::MYSQLDEF(void)
  {
  Pseudo = 2;                            // SERVID is Ok but not ROWID
  Hostname = NULL;
  Database = NULL;
  Tabname = NULL;
  Srcdef = NULL;
  Username = NULL;
  Password = NULL;
  Portnumber = 0;
  Isview = FALSE;
  Bind = FALSE;
  Delayed = FALSE;
  } // end of MYSQLDEF constructor

/***********************************************************************/
/* Parse connection string                                             */
/*                                                                     */
/* SYNOPSIS                                                            */
/*   ParseURL()                                                        */
/*   url                 The connection string to parse                */
/*                                                                     */
/* DESCRIPTION                                                         */
/*   Populates the table with information about the connection         */
/*   to the foreign database that will serve as the data source.       */
/*   This string must be specified (currently) in the "CONNECTION"     */
/*   field, listed in the CREATE TABLE statement.                      */
/*                                                                     */
/*   This string MUST be in the format of any of these:                */
/*                                                                     */
/*   CONNECTION="scheme://user:pwd@host:port/database/table"           */
/*   CONNECTION="scheme://user@host/database/table"                    */
/*   CONNECTION="scheme://user@host:port/database/table"               */
/*   CONNECTION="scheme://user:pwd@host/database/table"                */
/*                                                                     */
/*   _OR_                                                              */
/*                                                                     */
/*   CONNECTION="connection name" (NIY)                                */
/*                                                                     */
/* An Example:                                                         */
/*                                                                     */
/* CREATE TABLE t1 (id int(32))                                        */
/*   ENGINE="CONNECT" TABLE_TYPE="MYSQL"                               */
/*   CONNECTION="mysql://joe:pwd@192.168.1.111:9308/dbname/tabname";   */
/*                                                                     */
/* CREATE TABLE t2 (                                                   */
/*   id int(4) NOT NULL auto_increment,                                */
/*   name varchar(32) NOT NULL,                                        */
/*   PRIMARY KEY(id)                                                   */
/*   ) ENGINE="CONNECT" TABLE_TYPE="MYSQL"                             */
/*   CONNECTION="my_conn";    (NIY)                                    */
/*                                                                     */
/*  'password' and 'port' are both optional.                           */
/*                                                                     */
/* RETURN VALUE                                                        */
/*   false       success                                               */
/*   true        error                                                 */
/*                                                                     */
/***********************************************************************/
bool MYSQLDEF::ParseURL(PGLOBAL g, char *url)
  {
  if ((!strstr(url, "://") && (!strchr(url, '@')))) {
    // No :// or @ in connection string. Must be a straight
    // connection name of either "server" or "server/table"
    strcpy(g->Message, "Using Federated server not implemented yet");
    return true;
#if 0
    /* ok, so we do a little parsing, but not completely! */
    share->parsed= FALSE;
    /*
      If there is a single '/' in the connection string, this means the user is
      specifying a table name
    */

    if ((share->table_name= strchr(share->connection_string, '/')))
    {
      *share->table_name++= '\0';
      share->table_name_length= strlen(share->table_name);

      DBUG_PRINT("info", 
                 ("internal format, parsed table_name "
                  "share->connection_string: %s  share->table_name: %s",
                  share->connection_string, share->table_name));

      /*
        there better not be any more '/'s !
      */
      if (strchr(share->table_name, '/'))
        goto error;
    }
    /*
      Otherwise, straight server name, use tablename of federatedx table
      as remote table name
    */
    else
    {
      /*
        Connection specifies everything but, resort to
        expecting remote and foreign table names to match
      */
      share->table_name= strmake_root(mem_root, table->s->table_name.str,
                                      (share->table_name_length=
                                       table->s->table_name.length));
      DBUG_PRINT("info", 
                 ("internal format, default table_name "
                  "share->connection_string: %s  share->table_name: %s",
                  share->connection_string, share->table_name));
    }

    if ((error_num= get_connection(mem_root, share)))
      goto error;
#endif // 0
  } else {
    // URL, parse it
    char *sport, *scheme = url;

    if (!(Username = strstr(url, "://"))) {
      strcpy(g->Message, "Connection is not an URL");
      return true;
      } // endif User

    scheme[Username - scheme] = 0;

    if (stricmp(scheme, "mysql")) {
      strcpy(g->Message, "scheme must be mysql");
      return true;
      } // endif scheme

    Username += 3;

    if (!(Hostname = strchr(Username, '@'))) {
      strcpy(g->Message, "No host specified in URL");
      return true;
    } else
      *Hostname++ = 0;                   // End Username

    if ((Password = strchr(Username, ':'))) {
      *Password++ = 0;                   // End username

      // Make sure there isn't an extra / or @
      if ((strchr(Password, '/') || strchr(Hostname, '@'))) {
        strcpy(g->Message, "Syntax error in URL");
        return true;
        } // endif

      // Found that if the string is:
      // user:@hostname:port/db/table
      // Then password is a null string, so set to NULL
      if ((Password[0] == 0))
        Password = NULL;

      } // endif password

    // Make sure there isn't an extra / or @ */
    if ((strchr(Username, '/')) || (strchr(Hostname, '@'))) {
      strcpy(g->Message, "Syntax error in URL");
      return true;
      } // endif

    if ((Database = strchr(Hostname, '/'))) {
      *Database++ = 0;

      if ((Tabname = strchr(Database, '/')))
        *Tabname++ = 0;

      // Make sure there's not an extra /
      if ((strchr(Tabname, '/'))) {
        strcpy(g->Message, "Syntax error in URL");
        return true;
        } // endif /
        
      } // endif database

    if ((sport = strchr(Hostname, ':')))
      *sport++ = 0;

    Portnumber = (sport && sport[0]) ? atoi(sport) : mysqld_port;

    if (Username[0] == 0)
      Username = Cat->GetStringCatInfo(g, "User", "*");

    if (Hostname[0] == 0)
      Hostname = Cat->GetStringCatInfo(g, "Host", "localhost");

    if (!Database || !*Database)
      Database = Cat->GetStringCatInfo(g, "Database", "*");

    if (!Tabname || !*Tabname)
      Tabname = Name;

    } // endif URL

#if 0
  if (!share->port)
    if (!share->hostname || strcmp(share->hostname, my_localhost) == 0)
      share->socket= (char *) MYSQL_UNIX_ADDR;
    else
      share->port= MYSQL_PORT;
#endif // 0

  return false;
  } // end of ParseURL

/***********************************************************************/
/*  DefineAM: define specific AM block values from XCV file.           */
/***********************************************************************/
bool MYSQLDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  char *url = Cat->GetStringCatInfo(g, "Connect", NULL);

  Desc = "MySQL Table";

  if (stricmp(am, "MYPRX")) {
    // Normal case of specific MYSQL table
    if (!url || !*url) { 
      // Not using the connection URL
      Hostname = Cat->GetStringCatInfo(g, "Host", "localhost");
      Database = Cat->GetStringCatInfo(g, "Database", "*");
      Tabname = Cat->GetStringCatInfo(g, "Name", Name); // Deprecated
      Tabname = Cat->GetStringCatInfo(g, "Tabname", Tabname);
      Username = Cat->GetStringCatInfo(g, "User", "*");
      Password = Cat->GetStringCatInfo(g, "Password", NULL);
      Portnumber = Cat->GetIntCatInfo("Port", mysqld_port);
    } else if (ParseURL(g, url))
      return TRUE;

    Bind = !!Cat->GetIntCatInfo("Bind", 0);
    Delayed = !!Cat->GetIntCatInfo("Delayed", 0);
  } else {
    // MYSQL access from a PROXY table, not using URL 
    Database = Cat->GetStringCatInfo(g, "Database", "*");
    Tabname = Name;
    Isview = Cat->GetBoolCatInfo("View", FALSE);

    // We must get connection parms from the calling table
    Remove_tshp(Cat);
    Hostname = Cat->GetStringCatInfo(g, "Host", "localhost");
    Username = Cat->GetStringCatInfo(g, "User", "*");
    Password = Cat->GetStringCatInfo(g, "Password", NULL);
    Portnumber = Cat->GetIntCatInfo("Port", mysqld_port);
  } // endif am

  if ((Srcdef = Cat->GetStringCatInfo(g, "Srcdef", NULL)))
    Isview = TRUE;

  return FALSE;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB MYSQLDEF::GetTable(PGLOBAL g, MODE m)
  {
  if (Catfunc == FNC_COL)
    return new(g) TDBMCL(this);
  else
    return new(g) TDBMYSQL(this);

  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBMYSQL class.                              */
/***********************************************************************/
TDBMYSQL::TDBMYSQL(PMYDEF tdp) : TDBASE(tdp)
  {
  if (tdp) {
    Host = tdp->GetHostname();
    Database = tdp->GetDatabase();
    Tabname = tdp->GetTabname();
    Srcdef = tdp->GetSrcdef();
    User = tdp->GetUsername();
    Pwd = tdp->GetPassword();
    Port = tdp->GetPortnumber();
    Isview = tdp->Isview;
    Prep = tdp->Bind;
    Delayed = tdp->Delayed;
  } else {
    Host = NULL;
    Database = NULL;
    Tabname = NULL;
    Srcdef = NULL;
    User = NULL;
    Pwd = NULL;
    Port = 0;
    Isview = FALSE;
    Prep = FALSE;
    Delayed = FALSE;
  } // endif tdp

  Bind = NULL;
  Query = NULL;
  Qbuf = NULL;
  Fetched = FALSE;
  m_Rc = RC_FX;
  AftRows = 0;
  N = -1;
  Nparm = 0;
  } // end of TDBMYSQL constructor

TDBMYSQL::TDBMYSQL(PGLOBAL g, PTDBMY tdbp) : TDBASE(tdbp)
  {
  Host = tdbp->Host;
  Database = tdbp->Database;
  Tabname = tdbp->Tabname;
  Srcdef = tdbp->Srcdef;
  User = tdbp->User;
  Pwd =  tdbp->Pwd; 
  Port = tdbp->Port;
  Isview = tdbp->Isview;
  Prep = tdbp->Prep;
  Delayed = tdbp->Delayed;
  Bind = NULL;
  Query = tdbp->Query;
  Qbuf = NULL;
  Fetched = tdbp->Fetched;
  m_Rc = tdbp->m_Rc;
  AftRows = tdbp->AftRows;
  N = tdbp->N;
  Nparm = tdbp->Nparm;
  } // end of TDBMYSQL copy constructor

// Is this really useful ???
PTDB TDBMYSQL::CopyOne(PTABS t)
  {
  PTDB    tp;
  PCOL    cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBMYSQL(g, this);

  for (cp1 = Columns; cp1; cp1 = cp1->GetNext()) {
    cp2 = new(g) MYSQLCOL((PMYCOL)cp1, tp);

    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of CopyOne

/***********************************************************************/
/*  Allocate MYSQL column description block.                           */
/***********************************************************************/
PCOL TDBMYSQL::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) MYSQLCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  MakeSelect: make the Select statement use with MySQL connection.   */
/*  Note: when implementing EOM filtering, column only used in local   */
/*  filter should be removed from column list.                         */
/***********************************************************************/
bool TDBMYSQL::MakeSelect(PGLOBAL g)
  {
  char   *tk = "`";
  int     rank = 0;
  bool    b = FALSE;
  PCOL    colp;
//PDBUSER dup = PlgGetUser(g);

  if (Query)
    return FALSE;        // already done

  if (Srcdef) {
    Query = Srcdef;
    return false;
    } // endif Srcdef

  //Find the address of the suballocated query
  Query = (char*)PlugSubAlloc(g, NULL, 0);
  strcpy(Query, "SELECT ");

  if (Columns) {
    for (colp = Columns; colp; colp = colp->GetNext())
      if (colp->IsSpecial()) {
        strcpy(g->Message, MSG(NO_SPEC_COL));
        return TRUE;
      } else {
        if (b)
          strcat(Query, ", ");
        else
          b = TRUE;

        strcat(strcat(strcat(Query, tk), colp->GetName()), tk);
        ((PMYCOL)colp)->Rank = rank++;
      } // endif colp

  } else {
    // ncol == 0 can occur for views or queries such as
    // Query count(*) from... for which we will count the rows from
    // Query '*' from...
    // (the use of a char constant minimize the result storage)
    strcat(Query, (Isview) ? "*" : "'*'");
  } // endif ncol

  strcat(strcat(strcat(strcat(Query, " FROM "), tk), Tabname), tk);

  if (To_Filter)
    strcat(strcat(Query, " WHERE "), To_Filter);

  // Now we know how much to suballocate
  PlugSubAlloc(g, NULL, strlen(Query) + 1);
  return FALSE;
  } // end of MakeSelect

/***********************************************************************/
/*  MakeInsert: make the Insert statement used with MySQL connection.  */
/***********************************************************************/
bool TDBMYSQL::MakeInsert(PGLOBAL g)
  {
  char *colist, *valist = NULL;
  char *tk = "`";
  int   len = 0, qlen = 0;
  bool  b = FALSE;
  PCOL  colp;

  if (Query)
    return FALSE;        // already done

  for (colp = Columns; colp; colp = colp->GetNext())
    if (colp->IsSpecial()) {
      strcpy(g->Message, MSG(NO_SPEC_COL));
      return TRUE;
    } else {
      len += (strlen(colp->GetName()) + 4);
      ((PMYCOL)colp)->Rank = Nparm++;
    } // endif colp

  colist = (char*)PlugSubAlloc(g, NULL, len);
  *colist = '\0';

  if (Prep) {
#if defined(MYSQL_PREPARED_STATEMENTS)
    valist = (char*)PlugSubAlloc(g, NULL, 2 * Nparm);
    *valist = '\0';
#else   // !MYSQL_PREPARED_STATEMENTS
    strcpy(g->Message, "Prepared statements not used (not supported)");
    PushWarning(g, this);
    Prep = FALSE;
#endif  // !MYSQL_PREPARED_STATEMENTS 
    } // endif Prep

  for (colp = Columns; colp; colp = colp->GetNext()) {
    if (b) {
      strcat(colist, ", ");
      if (Prep) strcat(valist, ",");
    } else
      b = TRUE;

    strcat(strcat(strcat(colist, tk), colp->GetName()), tk);

    // Parameter marker
    if (!Prep) {
      if (colp->GetResultType() == TYPE_DATE)
        qlen += 20;
      else
        qlen += colp->GetLength();

    } // endif Prep

    if (Prep)
      strcat(valist, "?");

    } // endfor colp

  // Below 40 is enough to contain the fixed part of the query
  len = (strlen(Tabname) + strlen(colist)
                         + ((Prep) ? strlen(valist) : 0) + 40);
  Query = (char*)PlugSubAlloc(g, NULL, len);

  if (Delayed)
    strcpy(Query, "INSERT DELAYED INTO ");
  else
    strcpy(Query, "INSERT INTO ");

  strcat(strcat(strcat(Query, tk), Tabname), tk);
  strcat(strcat(strcat(Query, " ("), colist), ") VALUES (");

  if (Prep)
    strcat(strcat(Query, valist), ")");
  else {
    qlen += (strlen(Query) + Nparm);
    Qbuf = (char *)PlugSubAlloc(g, NULL, qlen);
    } // endelse Prep

  return FALSE;
  } // end of MakeInsert

#if 0
/***********************************************************************/
/*  MakeUpdate: make the Update statement use with MySQL connection.   */
/*  Note: currently limited to local values and filtering.             */
/***********************************************************************/
bool TDBMYSQL::MakeUpdate(PGLOBAL g, PSELECT selist)
  {
  char   *setlist, *colname, *where = NULL, *tk = "`";
  int     len = 0, nset = 0;
  bool    b = FALSE;
  PXOB    xp;
  PSELECT selp;

  if (Query)
    return FALSE;        // already done

  if (To_Filter)
    if (To_Filter->CheckLocal(this)) {
      where = (char*)PlugSubAlloc(g, NULL, 512);  // Should be enough
      *where = '\0';

      if (!PlugRephraseSQL(g, where, To_Filter, TYPE_FILTER, tk))
        return TRUE;

      To_Filter = NULL;
      len = strlen(where);
    } else {
      strcpy(g->Message, MSG(NO_REF_UPDATE));
      return TRUE;
    } // endif Local

  for (selp = selist; selp; selp = selp->GetNext_Proj())
    nset++;

  assert(nset);

  // Allocate a pretty big buffer
  setlist = (char*)PlugSubAlloc(g, NULL, 256 * nset);
  *setlist = '\0';

  for (selp = selist; selp; selp = selp->GetNext_Proj()) {
    if (selp->GetSetType() == TYPE_COLBLK) {
      colname = selp->GetSetCol()->GetName();
    } else if (selp->GetSetType() == TYPE_COLUMN) {
      colname = (char*)((PCOLUMN)selp->GetSetCol())->GetName();
    } else {
      sprintf(g->Message, MSG(BAD_SET_TYPE), selp->GetSetType());
      return TRUE;
    } // endif Type

    if (b)
      strcat(setlist, ", ");
    else
      b = TRUE;

    strcat(strcat(strcat(strcat(setlist, tk), colname), tk), " = ");

    xp = selp->GetObject();

    if (!xp->CheckLocal(this)) {
      strcpy(g->Message, MSG(NO_REF_UPDATE));
      return TRUE;
    } else if (xp->GetType() == TYPE_SUBQ)
      // Cannot be correlated because CheckLocal would have failed
      xp = new(g) CONSTANT(xp->GetValue());

    if (!PlugRephraseSQL(g, setlist + strlen(setlist),
                         xp, TYPE_XOBJECT, tk))
      return TRUE;

    } // endfor selp

  // Below 16 is enough to take care of the fixed part of the query
  len += (strlen(setlist) + strlen(Tabname) + 16);
  Query = (char*)PlugSubAlloc(g, NULL, len);
  strcat(strcat(strcat(strcpy(Query, "UPDATE "), tk), Tabname), tk);
  strcat(strcat(Query, " SET "), setlist);

  if (where)
    strcat(Query, where);

  return FALSE;
  } // end of MakeUpdate

/***********************************************************************/
/*  MakeDelete: make the Delete statement use with MySQL connection.   */
/*  If no filtering Truncate is used because it is faster than Delete. */
/*  However, the number of deleted lines is not returned by MySQL.     */
/*  Note: currently limited to local filtering.                        */
/***********************************************************************/
bool TDBMYSQL::MakeDelete(PGLOBAL g)
  {
  char *tk = "`";
  int   len = 0;

  if (Query)
    return FALSE;        // already done

  if (!To_Filter)
    AftRows = -1;         // Means "all lines deleted"

  // Below 16 is more than length of 'delete from ' + 3
  len += (strlen(Tabname) + 16);
  len += (To_Filter ? strlen(To_Filter) + 7 : 0);
  Query = (char*)PlugSubAlloc(g, NULL, len);
  strcpy(Query, (To_Filter) ? "DELETE FROM " : "TRUNCATE ");
  strcat(strcat(strcat(Query, tk), Tabname), tk);

  if (To_Filter)
    strcat(strcat(Query, " WHERE "), To_Filter);

  return FALSE;
  } // end of MakeDelete
#endif // 0

/***********************************************************************/
/*  XCV GetMaxSize: returns the maximum number of rows in the table.   */
/***********************************************************************/
int TDBMYSQL::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
#if 0
    if (MakeSelect(g))
      return -2;

    if (!Myc.Connected()) {
      if (Myc.Open(g, Host, Database, User, Pwd, Port))
        return -1;

      } // endif connected

    if ((MaxSize = Myc.GetResultSize(g, Query)) < 0) {
      Myc.Close();
      return -3;
      } // endif MaxSize

    // FIXME: Columns should be known when Info calls GetMaxSize
    if (!Columns)
      Query = NULL;     // Must be remade when columns are known
#endif // 0

    MaxSize = 10;       // To make MySQL happy
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  This a fake routine as ROWID does not exist in MySQL.              */
/***********************************************************************/
int TDBMYSQL::RowNumber(PGLOBAL g, bool b)
  {
  return N;
  } // end of RowNumber

/***********************************************************************/
/*  Return 0 in mode DELETE to tell that the delete is done.           */
/***********************************************************************/
int TDBMYSQL::GetProgMax(PGLOBAL g)
  {
  return (Mode == MODE_DELETE || Mode == MODE_UPDATE) ? 0
                                                      : GetMaxSize(g);
  } // end of GetProgMax

/***********************************************************************/
/*  MySQL Bind Parameter function.                                     */
/***********************************************************************/
int TDBMYSQL::BindColumns(PGLOBAL g)
  {
#if defined(MYSQL_PREPARED_STATEMENTS)
  if (Prep) {
    Bind = (MYSQL_BIND*)PlugSubAlloc(g, NULL, Nparm * sizeof(MYSQL_BIND));

    for (PMYCOL colp = (PMYCOL)Columns; colp; colp = (PMYCOL)colp->Next)
      colp->InitBind(g);

    return Myc.BindParams(g, Bind);
    } // endif prep
#endif   // MYSQL_PREPARED_STATEMENTS

  for (PMYCOL colp = (PMYCOL)Columns; colp; colp = (PMYCOL)colp->Next)
    if (colp->Buf_Type == TYPE_DATE)
      // Format must match DATETIME MySQL type
      ((DTVAL*)colp->GetValue())->SetFormat(g, "YYYY-MM-DD hh:mm:ss", 19);

  return RC_OK;
  } // end of BindColumns

/***********************************************************************/
/*  MySQL Access Method opening routine.                               */
/***********************************************************************/
bool TDBMYSQL::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
    Myc.Rewind();
    return false;
    } // endif use

  /*********************************************************************/
  /*  Open a MySQL connection for this table.                          */
  /*  Note: this may not be the proper way to do. Perhaps it is better */
  /*  to test whether a connection is already open for this server     */
  /*  and if so to allocate just a new result set. But this only for   */
  /*  servers allowing concurency in getting results ???               */
  /*********************************************************************/
  if (!Myc.Connected()) {
    if (Myc.Open(g, Host, Database, User, Pwd, Port))
      return true;

    } // endif Connected

  /*********************************************************************/
  /*  Allocate whatever is used for getting results.                   */
  /*********************************************************************/
  if (Mode == MODE_READ) {
    if (!MakeSelect(g))
      m_Rc = Myc.ExecSQL(g, Query);

#if 0
    if (!Myc.m_Res || !Myc.m_Fields) {
      sprintf(g->Message, "%s result", (Myc.m_Res) ? "Void" : "No");
      Myc.Close();
      return true;
      } // endif m_Res
#endif // 0

    if (Srcdef)
      if (SetColumnRanks(g))
        return true;

  } else if (Mode == MODE_INSERT) {
    if (Srcdef) {
      strcpy(g->Message, "No insert into anonym views");
      return true;
      } // endif Srcdef

    if (!MakeInsert(g)) {
#if defined(MYSQL_PREPARED_STATEMENTS)
      int n = (Prep) ? Myc.PrepareSQL(g, Query) : Nparm;

      if (Nparm != n) {
        if (n >= 0)          // Other errors return negative values
          strcpy(g->Message, MSG(BAD_PARM_COUNT));

      } else
#endif   // MYSQL_PREPARED_STATEMENTS
        m_Rc = BindColumns(g);

      } // endif MakeInsert

    if (m_Rc != RC_FX) {
      char cmd[64];
      int  w;

      sprintf(cmd, "ALTER TABLE `%s` DISABLE KEYS", Tabname);
      m_Rc = Myc.ExecSQL(g, cmd, &w);
      } // endif m_Rc

#if 0
  } else if (Next) {
    strcpy(g->Message, MSG(NO_JOIN_UPDEL));
  } else  if (Mode == MODE_DELETE) {
    strcpy(g->Message, "MySQL table delete not implemented yet\n");
    bool rc = MakeDelete(g);

    if (!rc && Myc.ExecSQL(g, Query) == RC_NF) {
      if (!AftRows)
        AftRows = Myc.GetRows();

      m_Rc = RC_OK;
      } // endif ExecSQL
#endif // 0

  } else {
//  bool rc = MakeUpdate(g, sqlp->GetProj());
    strcpy(g->Message, "MySQL table delete/update not implemented yet\n");
  } // endelse

  if (m_Rc == RC_FX) {
    Myc.Close();
    return TRUE;
    } // endif rc

  Use = USE_OPEN;       // Do it now in case we are recursively called
  return FALSE;
  } // end of OpenDB

/***********************************************************************/
/*  Set the rank of columns in the result set.                         */
/***********************************************************************/
bool TDBMYSQL::SetColumnRanks(PGLOBAL g)
  {
  for (PCOL colp = Columns; colp; colp = colp->GetNext())
    if (((PMYCOL)colp)->FindRank(g))
      return TRUE;

  return FALSE;
  } // end of SetColumnRanks

/***********************************************************************/
/*  Called by Parent table to make the columns of a View.              */
/***********************************************************************/
PCOL TDBMYSQL::MakeFieldColumn(PGLOBAL g, char *name)
  {
  int          n;
  MYSQL_FIELD *fld;
  PCOL         cp, colp = NULL;

  for (n = 0; n < Myc.m_Fields; n++) {
    fld = &Myc.m_Res->fields[n];

    if (!stricmp(name, fld->name)) {
      colp = new(g) MYSQLCOL(fld, this, n);

      if (colp->InitValue(g))
        return NULL;

      if (!Columns)
        Columns = colp;
      else for (cp = Columns; cp; cp = cp->GetNext())
        if (!cp->GetNext()) {
          cp->SetNext(colp);
          break;
          } // endif Next

      break;
      } // endif name

    } // endfor n

  if (!colp)
    sprintf(g->Message, "Column %s is not in view", name);

  return colp;
  } // end of MakeFieldColumn

/***********************************************************************/
/*  Called by Pivot tables to find default column names in a View      */
/*  as the name of last field not equal to the passed name.            */
/***********************************************************************/
char *TDBMYSQL::FindFieldColumn(char *name)
  {
  int          n;
  MYSQL_FIELD *fld;
  char        *cp = NULL;

  for (n = Myc.m_Fields - 1; n >= 0; n--) {
    fld = &Myc.m_Res->fields[n];

    if (!name || stricmp(name, fld->name)) {
      cp = fld->name;
      break;
      } // endif name

    } // endfor n

  return cp;
  } // end of FindFieldColumn

/***********************************************************************/
/*  Data Base read routine for MYSQL access method.                    */
/***********************************************************************/
int TDBMYSQL::ReadDB(PGLOBAL g)
  {
  int rc;

  if (trace > 1)
    htrc("MySQL ReadDB: R%d Mode=%d key=%p link=%p Kindex=%p\n",
          GetTdb_No(), Mode, To_Key_Col, To_Link, To_Kindex);

  /*********************************************************************/
  /*  Now start the reading process.                                   */
  /*  Here is the place to fetch the line.                             */
  /*********************************************************************/
  N++;
  Fetched = ((rc = Myc.Fetch(g, -1)) == RC_OK);

  if (trace > 1)
    htrc(" Read: rc=%d\n", rc);

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for MYSQL access methods.         */
/***********************************************************************/
int TDBMYSQL::WriteDB(PGLOBAL g)
  {
#if defined(MYSQL_PREPARED_STATEMENTS)
  if (Prep)
    return Myc.ExecStmt(g);
#endif   // MYSQL_PREPARED_STATEMENTS

  // Statement was not prepared, we must construct and execute
  // an insert query for each line to insert
  int  rc;
  char buf[32];

  strcpy(Qbuf, Query);

  // Make the Insert command value list
  for (PCOL colp = Columns; colp; colp = colp->GetNext()) {
    if (!colp->GetValue()->IsNull()) {
      if (colp->GetResultType() == TYPE_STRING || 
          colp->GetResultType() == TYPE_DATE)
        strcat(Qbuf, "'");

      strcat(Qbuf, colp->GetValue()->GetCharString(buf));

      if (colp->GetResultType() == TYPE_STRING || 
          colp->GetResultType() == TYPE_DATE)
        strcat(Qbuf, "'");

    } else
      strcat(Qbuf, "NULL");

    strcat(Qbuf, (colp->GetNext()) ? "," : ")");
    } // endfor colp

  Myc.m_Rows = -1;      // To execute the query
  rc = Myc.ExecSQL(g, Qbuf);
  return (rc == RC_NF) ? RC_OK : rc;      // RC_NF is Ok
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for MYSQL access methods.            */
/***********************************************************************/
int TDBMYSQL::DeleteDB(PGLOBAL g, int irc)
  {
  strcpy(g->Message, MSG(NO_MYSQL_DELETE));
  return RC_FX;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for MySQL access method.                   */
/***********************************************************************/
void TDBMYSQL::CloseDB(PGLOBAL g)
  {
  if (Mode == MODE_INSERT) {
    char cmd[64];
    int  w;
    PDBUSER dup = PlgGetUser(g);

    dup->Step = "Enabling indexes";
    sprintf(cmd, "ALTER TABLE `%s` ENABLE KEYS", Tabname);
    Myc.m_Rows = -1;      // To execute the query
    m_Rc = Myc.ExecSQL(g, cmd, &w);
    } // endif m_Rc

  Myc.Close();

  if (trace)
    htrc("MySQL CloseDB: closing %s rc=%d\n", Name, m_Rc);

  } // end of CloseDB

// ------------------------ MYSQLCOL functions --------------------------

/***********************************************************************/
/*  MYSQLCOL public constructor.                                       */
/***********************************************************************/
MYSQLCOL::MYSQLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
        : COLBLK(cdp, tdbp, i)
  {
  if (cprec) {
    Next = cprec->GetNext();
    cprec->SetNext(this);
  } else {
    Next = tdbp->GetColumns();
    tdbp->SetColumns(this);
  } // endif cprec

  // Set additional MySQL access method information for column.
  Long = cdp->GetLong();
  Bind = NULL;
  To_Val = NULL;
  Slen = 0;
  Rank = -1;            // Not known yet

  if (trace)
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of MYSQLCOL constructor

/***********************************************************************/
/*  MYSQLCOL public constructor.                                       */
/***********************************************************************/
MYSQLCOL::MYSQLCOL(MYSQL_FIELD *fld, PTDB tdbp, int i, PSZ am)
        : COLBLK(NULL, tdbp, i)
  {
  Name = fld->name;
  Opt = 0;
  Long = fld->length;
  Buf_Type = MYSQLtoPLG(fld->type);
  strcpy(Format.Type, GetFormatType(Buf_Type));
  Format.Length = Long;
  Format.Prec = fld->decimals;
  ColUse = U_P;
  Nullable = !IS_NOT_NULL(fld->flags);

  // Set additional MySQL access method information for column.
  Bind = NULL;
  To_Val = NULL;
  Slen = 0;
  Rank = i;

  if (trace)
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of MYSQLCOL constructor

/***********************************************************************/
/*  MYSQLCOL constructor used for copying columns.                     */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
MYSQLCOL::MYSQLCOL(MYSQLCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  Long = col1->Long;
  Bind = NULL;
  To_Val = NULL;
  Slen = col1->Slen;
  Rank = col1->Rank;
  } // end of MYSQLCOL copy constructor

/***********************************************************************/
/*  FindRank: Find the rank of this column in the result set.          */
/***********************************************************************/
bool MYSQLCOL::FindRank(PGLOBAL g)
{
  int    n;
  MYSQLC myc = ((PTDBMY)To_Tdb)->Myc;

  for (n = 0; n < myc.m_Fields; n++)
    if (!stricmp(Name, myc.m_Res->fields[n].name)) {
      Rank = n;
      return false;
      } // endif Name

  sprintf(g->Message, "Column %s not in result set", Name);
  return true;
} // end of FindRank

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool MYSQLCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
  {
  if (!(To_Val = value)) {
    sprintf(g->Message, MSG(VALUE_ERROR), Name);
    return TRUE;
  } else if (Buf_Type == value->GetType()) {
    // Values are of the (good) column type
    if (Buf_Type == TYPE_DATE) {
      // If any of the date values is formatted
      // output format must be set for the receiving table
      if (GetDomain() || ((DTVAL *)value)->IsFormatted())
        goto newval;          // This will make a new value;

    } else if (Buf_Type == TYPE_FLOAT)
      // Float values must be written with the correct (column) precision
      // Note: maybe this should be forced by ShowValue instead of this ?
      value->SetPrec(GetPrecision());

    Value = value;            // Directly access the external value
  } else {
    // Values are not of the (good) column type
    if (check) {
      sprintf(g->Message, MSG(TYPE_VALUE_ERR), Name,
              GetTypeName(Buf_Type), GetTypeName(value->GetType()));
      return TRUE;
      } // endif check

 newval:
    if (InitValue(g))         // Allocate the matching value block
      return TRUE;

  } // endif's Value, Buf_Type

  // Because Colblk's have been made from a copy of the original TDB in
  // case of Update, we must reset them to point to the original one.
  if (To_Tdb->GetOrig())
    To_Tdb = (PTDB)To_Tdb->GetOrig();

  // Set the Column
  Status = (ok) ? BUF_EMPTY : BUF_NO;
  return FALSE;
  } // end of SetBuffer

/***********************************************************************/
/*  InitBind: Initialize the bind structure according to type.         */
/***********************************************************************/
void MYSQLCOL::InitBind(PGLOBAL g)
  {
  PTDBMY tdbp = (PTDBMY)To_Tdb;

  assert(tdbp->Bind && Rank < tdbp->Nparm);

  Bind = &tdbp->Bind[Rank];
  memset(Bind, 0, sizeof(MYSQL_BIND));

  if (Buf_Type == TYPE_DATE) {
    // Default format must match DATETIME MySQL type
//  if (!((DTVAL*)Value)->IsFormatted())
      ((DTVAL*)Value)->SetFormat(g, "YYYY-MM-DD hh:mm:ss", 19);

    Bind->buffer_type = PLGtoMYSQL(TYPE_STRING, false);
    Bind->buffer = (char *)PlugSubAlloc(g,NULL, 20);
    Bind->buffer_length = 20;
    Bind->length = &Slen;
  } else {
    Bind->buffer_type = PLGtoMYSQL(Buf_Type, false);
    Bind->buffer = (char *)Value->GetTo_Val();
    Bind->buffer_length = Value->GetClen();
    Bind->length = (IsTypeChar(Buf_Type)) ? &Slen : NULL;
  } // endif Buf_Type

  } // end of InitBind

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void MYSQLCOL::ReadColumn(PGLOBAL g)
  {
  char  *buf;
  int    rc;
  PTDBMY tdbp = (PTDBMY)To_Tdb;

  /*********************************************************************/
  /*  If physical fetching of the line was deferred, do it now.        */
  /*********************************************************************/
  if (!tdbp->Fetched)
    if ((rc = tdbp->Myc.Fetch(g, tdbp->N)) != RC_OK) {
      if (rc == RC_EF)
        sprintf(g->Message, MSG(INV_DEF_READ), rc);

      longjmp(g->jumper[g->jump_level], 11);
    } else
      tdbp->Fetched = TRUE;

  if ((buf = ((PTDBMY)To_Tdb)->Myc.GetCharField(Rank))) {
    if (trace)
      htrc("MySQL ReadColumn: name=%s buf=%s\n", Name, buf);

    Value->SetValue_char(buf, Long);
  } else {
    if (Nullable)
      Value->SetNull(true);

    Value->Reset();              // Null value
  } // endif buf

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: make sure the bind buffer is updated.                 */
/***********************************************************************/
void MYSQLCOL::WriteColumn(PGLOBAL g)
  {
  /*********************************************************************/
  /*  Do convert the column value if necessary.                        */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, FALSE);   // Convert the inserted value

#if defined(MYSQL_PREPARED_STATEMENTS)
  if (((PTDBMY)To_Tdb)->Prep) {
    if (Buf_Type == TYPE_DATE) {
      Value->ShowValue((char *)Bind->buffer, (int)*Bind->length);
      Slen = strlen((char *)Bind->buffer);
    } else if (IsTypeChar(Buf_Type))
      Slen = strlen(Value->GetCharValue());

    } // endif Prep
#endif   // MYSQL_PREPARED_STATEMENTS

  } // end of WriteColumn

/* ---------------------------TDBMCL class --------------------------- */

/***********************************************************************/
/*  TDBMCL class constructor.                                          */
/***********************************************************************/
TDBMCL::TDBMCL(PMYDEF tdp) : TDBCAT(tdp)
  {
  Host = tdp->Hostname;  
  Db   = tdp->Database;    
  Tab  = tdp->Tabname;    
  User = tdp->Username;  
  Pwd  = tdp->Password;   
  Port = tdp->Portnumber;
  } // end of TDBMCL constructor

/***********************************************************************/
/*  GetResult: Get the list the MYSQL table columns.                   */
/***********************************************************************/
PQRYRES TDBMCL::GetResult(PGLOBAL g)
  {
  return MyColumns(g, Host, Db, User, Pwd, Tab, NULL, Port, false);
	} // end of GetResult
