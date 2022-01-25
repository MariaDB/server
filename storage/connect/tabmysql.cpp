/************* TabMySQL C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: TABMYSQL                                               */
/* -------------                                                        */
/*  Version 2.0                                                         */
/*                                                                      */
/* AUTHOR:                                                              */
/* -------                                                              */
/*  Olivier BERTRAND                                      2007-2017     */
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
/*    TABMYSQL.H     - TABMYSQL classes declaration file                */
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
#define MYSQL_SERVER 1
#include "my_global.h"
#include "sql_class.h"
#include "sql_servers.h"
#if defined(_WIN32)
//#include <windows.h>
#else   // !_WIN32
//#include <fnmatch.h>
//#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "osutil.h"
//#include <io.h>
//#include <fcntl.h>
#endif  // !_WIN32

/***********************************************************************/
/*  Include application header files:                                  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "tabext.h"
#include "tabcol.h"
#include "colblk.h"
//#include "reldef.h"
#include "tabmysql.h"
#include "valblk.h"
#include "tabutil.h"
#include "ha_connect.h"

#if defined(_CONSOLE)
void PrintResult(PGLOBAL, PSEM, PQRYRES);
#endif   // _CONSOLE

// Used to check whether a MYSQL table is created on itself
bool CheckSelf(PGLOBAL g, TABLE_SHARE *s, PCSZ host, PCSZ db,
	                                        PCSZ tab, PCSZ src, int port);

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
bool ExactInfo(void);

/* -------------- Implementation of the MYSQLDEF class --------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
MYSQLDEF::MYSQLDEF(void)
  {
  Pseudo = 2;                            // SERVID is Ok but not ROWID
  Hostname = NULL;
//Tabschema = NULL;
//Tabname = NULL;
//Srcdef = NULL;
//Username = NULL;
//Password = NULL;
  Portnumber = 0;
  Isview = false;
  Bind = false;
  Delayed = false;
//Xsrc = false;
  Huge = false;
  } // end of MYSQLDEF constructor

/***********************************************************************/
/*  Get connection info from the declared server.                      */
/***********************************************************************/
bool MYSQLDEF::GetServerInfo(PGLOBAL g, const char *server_name)
{
  THD      *thd= current_thd;
  MEM_ROOT *mem= thd->mem_root;
  FOREIGN_SERVER *server, server_buffer;
  DBUG_ENTER("GetServerInfo");
  DBUG_PRINT("info", ("server_name %s", server_name));

  if (!server_name || !strlen(server_name)) {
    DBUG_PRINT("info", ("server_name not defined!"));
    strcpy(g->Message, "server_name not defined!");
    DBUG_RETURN(true);
    } // endif server_name

  // get_server_by_name() clones the server if exists and allocates
  // copies of strings in the supplied mem_root
  if (!(server= get_server_by_name(mem, server_name, &server_buffer))) {
    DBUG_PRINT("info", ("get_server_by_name returned > 0 error condition!"));
    /* need to come up with error handling */
    strcpy(g->Message, "get_server_by_name returned > 0 error condition!");
    DBUG_RETURN(true);
    } // endif server

  DBUG_PRINT("info", ("get_server_by_name returned server at %p",
                     server));

  // TODO: We need to examine which of these can really be NULL
  Hostname = PlugDup(g, server->host);
  Tabschema = PlugDup(g, server->db);
  Username = PlugDup(g, server->username);
  Password = PlugDup(g, server->password);
  Portnumber = (server->port) ? server->port : GetDefaultPort();

  DBUG_RETURN(false);
} // end of GetServerInfo

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
bool MYSQLDEF::ParseURL(PGLOBAL g, char *url, bool b)
  {
	char *tabn, *pwd, *schema;

  if ((!strstr(url, "://") && (!strchr(url, '@')))) {
    // No :// or @ in connection string. Must be a straight
    // connection name of either "server" or "server/table"
    // ok, so we do a little parsing, but not completely!
    if ((tabn= strchr(url, '/'))) {
      // If there is a single '/' in the connection string,
      // this means the user is specifying a table name
      *tabn++= '\0';

      // there better not be any more '/'s !
      if (strchr(tabn, '/'))
        return true;

			Tabname = tabn;
    } else
      // Otherwise, straight server name, 
      Tabname = (b) ? GetStringCatInfo(g, "Tabname", Name) : NULL;

    if (trace(1))
      htrc("server: %s  TableName: %s", url, Tabname);

    Server = url;
    return GetServerInfo(g, url);
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

    if (!(Hostname = (char*)strchr(Username, '@'))) {
      strcpy(g->Message, "No host specified in URL");
      return true;
    } else {
      *Hostname++ = 0;                   // End Username
      Server = Hostname;
    } // endif Hostname

    if ((pwd = (char*)strchr(Username, ':'))) {
      *pwd++ = 0;                   // End username

      // Make sure there isn't an extra /
      if (strchr(pwd, '/')) {
        strcpy(g->Message, "Syntax error in URL");
        return true;
        } // endif

      // Found that if the string is:
      // user:@hostname:port/db/table
      // Then password is a null string, so set to NULL
			if ((pwd[0] == 0))
				Password = NULL;
			else
				Password = pwd;

      } // endif password

    // Make sure there isn't an extra / or @ */
    if ((strchr(Username, '/')) || (strchr(Hostname, '@'))) {
      strcpy(g->Message, "Syntax error in URL");
      return true;
      } // endif

    if ((schema = strchr(Hostname, '/'))) {
      *schema++ = 0;

      if ((tabn = strchr(schema, '/'))) {
        *tabn++ = 0;

        // Make sure there's not an extra /
        if ((strchr(tabn, '/'))) {
          strcpy(g->Message, "Syntax error in URL");
          return true;
          } // endif /

				Tabname = tabn;
        } // endif TableName

			Tabschema = schema;
		} // endif database

    if ((sport = strchr(Hostname, ':')))
      *sport++ = 0;

    // For unspecified values, get the values of old style options
    // but only if called from MYSQLDEF, else set them to NULL
    Portnumber = (sport && sport[0]) ? atoi(sport) 
               : (b) ? GetIntCatInfo("Port", GetDefaultPort()) : 0;

    if (Username[0] == 0)
      Username = (b) ? GetStringCatInfo(g, "User", "*") : NULL;

    if (Hostname[0] == 0)
      Hostname = (b) ? GetStringCatInfo(g, "Host", "localhost") : NULL;

    if (!Tabschema || !*Tabschema)
      Tabschema = (b) ? GetStringCatInfo(g, "Database", "*") : NULL;

    if (!Tabname || !*Tabname)
      Tabname = (b) ? GetStringCatInfo(g, "Tabname", Name) : NULL;

    if (!Password)
      Password = (b) ? GetStringCatInfo(g, "Password", NULL) : NULL;
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
bool MYSQLDEF::DefineAM(PGLOBAL g, LPCSTR am, int)
  {
  char *url;

  Desc = "MySQL Table";

  if (stricmp(am, "MYPRX")) {
    // Normal case of specific MYSQL table
    url = GetStringCatInfo(g, "Connect", NULL);

    if (!url || !*url) {
      // Not using the connection URL
      Hostname = GetStringCatInfo(g, "Host", "localhost");
      Tabschema = GetStringCatInfo(g, "Database", "*");
      Tabname = GetStringCatInfo(g, "Name", Name); // Deprecated
      Tabname = GetStringCatInfo(g, "Tabname", Tabname);
      Username = GetStringCatInfo(g, "User", "*");
      Password = GetStringCatInfo(g, "Password", NULL);
      Portnumber = GetIntCatInfo("Port", GetDefaultPort());
      Server = Hostname;
    } else if (ParseURL(g, url))
      return true;

    Bind = !!GetIntCatInfo("Bind", 0);
    Delayed = !!GetIntCatInfo("Delayed", 0);
  } else {
    // MYSQL access from a PROXY table 
		TABLE_SHARE* s;

		Tabschema = GetStringCatInfo(g, "Database", Tabschema ? Tabschema : PlugDup(g, "*"));
    Isview = GetBoolCatInfo("View", false);

    // We must get other connection parms from the calling table
    s = Remove_tshp(Cat);
    url = GetStringCatInfo(g, "Connect", NULL);

    if (!url || !*url) { 
      Hostname = GetStringCatInfo(g, "Host", "localhost");
      Username = GetStringCatInfo(g, "User", "*");
      Password = GetStringCatInfo(g, "Password", NULL);
      Portnumber = GetIntCatInfo("Port", GetDefaultPort());
      Server = Hostname;
    } else {
      PCSZ locdb = Tabschema;

      if (ParseURL(g, url))
        return true;

      Tabschema = locdb;
    } // endif url

    Tabname = Name;

		// Needed for column description
		Restore_tshp(Cat, s);
  } // endif am

  if ((Srcdef = GetStringCatInfo(g, "Srcdef", NULL))) {
    Read_Only = true;
    Isview = true;
  } else if (CheckSelf(g, Hc->GetTable()->s, Hostname, Tabschema,
                       Tabname, Srcdef, Portnumber))
    return true;

  // Used for Update and Delete
  Qrystr = GetStringCatInfo(g, "Query_String", "?");
  Quoted = GetIntCatInfo("Quoted", 0);

  // Specific for command executing tables
  Xsrc = GetBoolCatInfo("Execsrc", false);
  Maxerr = GetIntCatInfo("Maxerr", 0);
  Huge = GetBoolCatInfo("Huge", false);
  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB MYSQLDEF::GetTable(PGLOBAL g, MODE)
  {
  if (Xsrc)
    return new(g) TDBMYEXC(this);
  else if (Catfunc == FNC_COL)
    return new(g) TDBMCL(this);
  else
    return new(g) TDBMYSQL(this);

  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBMYSQL class.                              */
/***********************************************************************/
TDBMYSQL::TDBMYSQL(PMYDEF tdp) : TDBEXT(tdp)
  {
  if (tdp) {
    Host = tdp->Hostname;
//  Schema = tdp->Tabschema;
//  TableName = tdp->Tabname;
//  Srcdef = tdp->Srcdef;
//  User = tdp->Username;
//  Pwd = tdp->Password;
    Server = tdp->Server;
//  Qrystr = tdp->Qrystr;
    Quoted = MY_MAX(0, tdp->Quoted);
    Port = tdp->Portnumber;
    Isview = tdp->Isview;
    Prep = tdp->Bind;
    Delayed = tdp->Delayed;
    Myc.m_Use = tdp->Huge;
  } else {
    Host = NULL;
//  Schema = NULL;
//  TableName = NULL;
//  Srcdef = NULL;
//  User = NULL;
//  Pwd = NULL;
    Server = NULL;
//  Qrystr = NULL;
//  Quoted = 0;
    Port = 0;
    Isview = false;
    Prep = false;
    Delayed = false;
  } // endif tdp

  Bind = NULL;
//Query = NULL;
  Fetched = false;
  m_Rc = RC_FX;
//AftRows = 0;
  N = -1;
//Nparm = 0;
  } // end of TDBMYSQL constructor

TDBMYSQL::TDBMYSQL(PTDBMY tdbp) : TDBEXT(tdbp)
  {
  Host = tdbp->Host;
//Schema = tdbp->Schema;
//TableName = tdbp->TableName;
//Srcdef = tdbp->Srcdef;
//User = tdbp->User;
//Pwd =  tdbp->Pwd;
//Qrystr = tdbp->Qrystr;
//Quoted = tdbp->Quoted;
	Server = tdbp->Server;
  Port = tdbp->Port;
  Isview = tdbp->Isview;
  Prep = tdbp->Prep;
  Delayed = tdbp->Delayed;
  Bind = NULL;
//Query = tdbp->Query;
  Fetched = tdbp->Fetched;
  m_Rc = tdbp->m_Rc;
//AftRows = tdbp->AftRows;
  N = tdbp->N;
//Nparm = tdbp->Nparm;
  } // end of TDBMYSQL copy constructor

// Is this really useful ??? --> Yes for UPDATE
PTDB TDBMYSQL::Clone(PTABS t)
  {
  PTDB    tp;
  PCOL    cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBMYSQL(this);

  for (cp1 = Columns; cp1; cp1 = cp1->GetNext()) {
    cp2 = new(g) MYSQLCOL((PMYCOL)cp1, tp);

    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

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
bool TDBMYSQL::MakeSelect(PGLOBAL g, bool mx)
{
//char   *tk = "`";
  char    tk = '`';
  int     len = 0, rank = 0;
  bool    b = false;
  PCOL    colp;
//PDBUSER dup = PlgGetUser(g);

  if (Query)
    return false;        // already done

	if (Srcdef)
		return MakeSrcdef(g);

  // Allocate the string used to contain Query
  Query = new(g) STRING(g, 1023, "SELECT ");

  if (Columns) {
    for (colp = Columns; colp; colp = colp->GetNext())
      if (!colp->IsSpecial()) {
        if (b)
          Query->Append(", ");
        else
          b = true;

        Query->Append(tk);
        Query->Append(colp->GetName());
        Query->Append(tk);
        ((PMYCOL)colp)->Rank = rank++;
      } // endif colp

  } else {
    // ncol == 0 can occur for views or queries such as
    // Query count(*) from... for which we will count the rows from
    // Query '*' from...
    // (the use of a char constant minimize the result storage)
    if (Isview)
      Query->Append('*');
    else
      Query->Append("'*'");

  } // endif ncol

  Query->Append(" FROM ");
  Query->Append(tk);
  Query->Append(TableName);
  Query->Append(tk);
  len = Query->GetLength();

  if (To_CondFil) {
    if (!mx) {
      Query->Append(" WHERE ");
      Query->Append(To_CondFil->Body);
      len = Query->GetLength() + 1;
    } else
      len += (strlen(To_CondFil->Body) + 256);

  } else
    len += (mx ? 256 : 1);

  if (Query->IsTruncated() || Query->Resize(len)) {
    strcpy(g->Message, "MakeSelect: Out of memory");
    return true;
  } // endif Query

  if (trace(33))
    htrc("Query=%s\n", Query->GetStr());

  return false;
} // end of MakeSelect

/***********************************************************************/
/*  MakeInsert: make the Insert statement used with MySQL connection.  */
/***********************************************************************/
bool TDBMYSQL::MakeInsert(PGLOBAL g)
  {
  const char *tk = "`";
  uint  len = 0;
  bool  oom, b = false;
  PCOL  colp;

  if (Query)
    return false;        // already done

  if (Prep) {
#if !defined(MYSQL_PREPARED_STATEMENTS)
    strcpy(g->Message, "Prepared statements not used (not supported)");
    PushWarning(g, this);
    Prep = false;
#endif  // !MYSQL_PREPARED_STATEMENTS 
    } // endif Prep

  for (colp = Columns; colp; colp = colp->GetNext())
    if (colp->IsSpecial()) {
      strcpy(g->Message, MSG(NO_SPEC_COL));
      return true;
    } else {
      len += (strlen(colp->GetName()) + 4);

      // Parameter marker
      if (!Prep) {
        if (colp->GetResultType() == TYPE_DATE)
          len += 20;
        else
          len += colp->GetLength();
  
      } else
        len += 2;

      ((PMYCOL)colp)->Rank = Nparm++;
    } // endif colp

  // Below 40 is enough to contain the fixed part of the query
  len += (strlen(TableName) + 40);
  Query = new(g) STRING(g, len);

  if (Delayed)
    Query->Set("INSERT DELAYED INTO ");
  else
    Query->Set("INSERT INTO ");

  Query->Append(tk);
  Query->Append(TableName);
  Query->Append("` (");

  for (colp = Columns; colp; colp = colp->GetNext()) {
    if (b)
      Query->Append(", ");
    else
      b = true;
  
    Query->Append(tk);
    Query->Append(colp->GetName());
    Query->Append(tk);
    } // endfor colp

  Query->Append(") VALUES (");

#if defined(MYSQL_PREPARED_STATEMENTS)
  if (Prep) {
    for (int i = 0; i < Nparm; i++)
      Query->Append("?,");

    Query->RepLast(')');
    Query->Trim();
    }  // endif Prep
#endif  // MYSQL_PREPARED_STATEMENTS 

  if ((oom = Query->IsTruncated()))
    strcpy(g->Message, "MakeInsert: Out of memory");

  return oom;
  } // end of MakeInsert

/***********************************************************************/
/*  MakeCommand: make the Update or Delete statement to send to the    */
/*  MySQL server. Limited to remote values and filtering.              */
/***********************************************************************/
bool TDBMYSQL::MakeCommand(PGLOBAL g)
  {
  Query = new(g) STRING(g, strlen(Qrystr) + 64);

  if (Quoted > 0 || stricmp(Name, TableName)) {
    char *p, *qrystr, name[68];
    bool  qtd = Quoted > 0;


    // Make a lower case copy of the originale query
    qrystr = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 5);
    strlwr(strcpy(qrystr, Qrystr));

    // Check whether the table name is equal to a keyword
    // If so, it must be quoted in the original query
    strlwr(strcat(strcat(strcpy(name, "`"), Name), "`"));

    if (!strstr("`update`delete`low_priority`ignore`quick`from`", name))
      strlwr(strcpy(name, Name));     // Not a keyword

    if ((p = strstr(qrystr, name))) {
      Query->Set(Qrystr, (uint)(p - qrystr));

      if (qtd && *(p-1) == ' ') {
        Query->Append('`');
        Query->Append(TableName);
        Query->Append('`');
      } else
        Query->Append(TableName);

      Query->Append(Qrystr + (p - qrystr) + strlen(name));

      if (Query->IsTruncated()) {
        strcpy(g->Message, "MakeCommand: Out of memory");
        return true;
      } else
        strlwr(strcpy(qrystr, Query->GetStr()));

    } else {
      sprintf(g->Message, "Cannot use this %s command",
                   (Mode == MODE_UPDATE) ? "UPDATE" : "DELETE");
      return true;
    } // endif p

  } else
    (void)Query->Set(Qrystr);

  return false;
  } // end of MakeCommand

#if 0
/***********************************************************************/
/*  MakeUpdate: make the Update statement use with MySQL connection.   */
/*  Limited to remote values and filtering.                            */
/***********************************************************************/
int TDBMYSQL::MakeUpdate(PGLOBAL g)
  {
  char *qc, cmd[8], tab[96], end[1024];

  Query = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 64);
  memset(end, 0, sizeof(end));

  if (sscanf(Qrystr, "%s `%[^`]`%1023c", cmd, tab, end) > 2 ||
      sscanf(Qrystr, "%s \"%[^\"]\"%1023c", cmd, tab, end) > 2)
    qc = "`";
  else if (sscanf(Qrystr, "%s %s%1023c", cmd, tab, end) > 2
                  && !stricmp(tab, Name))
    qc = (Quoted) ? "`" : "";
  else {
    strcpy(g->Message, "Cannot use this UPDATE command");
    return RC_FX;
  } // endif sscanf

  assert(!stricmp(cmd, "update"));
  strcat(strcat(strcat(strcpy(Query, "UPDATE "), qc), TableName), qc);
  strcat(Query, end);
  return RC_OK;
  } // end of MakeUpdate

/***********************************************************************/
/*  MakeDelete: make the Delete statement used with MySQL connection.  */
/*  Limited to remote filtering.                                       */
/***********************************************************************/
int TDBMYSQL::MakeDelete(PGLOBAL g)
  {
  char *qc, cmd[8], from[8], tab[96], end[512];

  Query = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 64);
  memset(end, 0, sizeof(end));

  if (sscanf(Qrystr, "%s %s `%[^`]`%511c", cmd, from, tab, end) > 2 ||
      sscanf(Qrystr, "%s %s \"%[^\"]\"%511c", cmd, from, tab, end) > 2)
    qc = "`";
  else if (sscanf(Qrystr, "%s %s %s%511c", cmd, from, tab, end) > 2)
    qc = (Quoted) ? "`" : "";
  else {
    strcpy(g->Message, "Cannot use this DELETE command");
    return RC_FX;
  } // endif sscanf

  assert(!stricmp(cmd, "delete") && !stricmp(from, "from"));
  strcat(strcat(strcat(strcpy(Query, "DELETE FROM "), qc), TableName), qc);

  if (*end)
    strcat(Query, end);

  return RC_OK;
  } // end of MakeDelete
#endif // 0

/***********************************************************************/
/*  MYSQL Cardinality: returns the number of rows in the table.        */
/***********************************************************************/
int TDBMYSQL::Cardinality(PGLOBAL g)
{
  if (!g)
    return (Mode == MODE_ANY && !Srcdef) ? 1 : 0;

  if (Cardinal < 0 && Mode == MODE_ANY && !Srcdef && ExactInfo()) {
    // Info command, we must return the exact table row number
    char   query[96];
    MYSQLC myc;

    if (myc.Open(g, Host, Schema, User, Pwd, Port, csname))
      return -1;

    strcpy(query, "SELECT COUNT(*) FROM ");

    if (Quoted > 0)
      strcat(strcat(strcat(query, "`"), TableName), "`");
    else
      strcat(query, TableName);

    Cardinal = myc.GetTableSize(g, query);
    myc.Close();
  } else
    Cardinal = 10;    // To make MySQL happy

  return Cardinal;
} // end of Cardinality

#if 0
/***********************************************************************/
/*  MYSQL GetMaxSize: returns the maximum number of rows in the table. */
/***********************************************************************/
int TDBMYSQL::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    if (Mode == MODE_DELETE)
      // Return 0 in mode DELETE in case of delete all.
      MaxSize = 0;
    else if (!Cardinality(NULL))
      MaxSize = 10;   // To make MySQL happy
    else if ((MaxSize = Cardinality(g)) < 0)
      MaxSize = 12;   // So we can see an error occurred

    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize
#endif // 0

/***********************************************************************/
/*  This a fake routine as ROWID does not exist in MySQL.              */
/***********************************************************************/
int TDBMYSQL::RowNumber(PGLOBAL, bool)
  {
  return N + 1;
  } // end of RowNumber

/***********************************************************************/
/*  Return 0 in mode UPDATE to tell that the update is done.           */
/***********************************************************************/
int TDBMYSQL::GetProgMax(PGLOBAL g)
  {
  return (Mode == MODE_UPDATE) ? 0 : GetMaxSize(g);
  } // end of GetProgMax

/***********************************************************************/
/*  MySQL Bind Parameter function.                                     */
/***********************************************************************/
int TDBMYSQL::BindColumns(PGLOBAL g __attribute__((unused)))
  {
#if defined(MYSQL_PREPARED_STATEMENTS)
  if (Prep) {
    Bind = (MYSQL_BIND*)PlugSubAlloc(g, NULL, Nparm * sizeof(MYSQL_BIND));

    for (PMYCOL colp = (PMYCOL)Columns; colp; colp = (PMYCOL)colp->Next)
      colp->InitBind(g);

    return Myc.BindParams(g, Bind);
    } // endif prep
#endif   // MYSQL_PREPARED_STATEMENTS

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
		if (Myc.Rewind(g, (Mode == MODE_READX) ? Query->GetStr() : NULL) != RC_OK)
			return true;

    N = -1;
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
    if (Myc.Open(g, Host, Schema, User, Pwd, Port, csname))
      return true;

    } // endif Connected

  /*********************************************************************/
  /*  Take care of DATE columns.                                       */
  /*********************************************************************/
  for (PMYCOL colp = (PMYCOL)Columns; colp; colp = (PMYCOL)colp->Next)
    if (colp->Buf_Type == TYPE_DATE)
      // Format must match DATETIME MySQL type
      ((DTVAL*)colp->GetValue())->SetFormat(g, "YYYY-MM-DD hh:mm:ss", 19);

  /*********************************************************************/
  /*  Allocate whatever is used for getting results.                   */
  /*********************************************************************/
  if (Mode == MODE_READ || Mode == MODE_READX) {
    MakeSelect(g, Mode == MODE_READX);
    if (Mode == MODE_READ && !Query)
    {
      Myc.Close();
      return true;
    }
    m_Rc = (Mode == MODE_READ)
         ? Myc.ExecSQL(g, Query->GetStr()) : RC_OK;

#if 0
    if (!Myc.m_Res || !Myc.m_Fields) {
      sprintf(g->Message, "%s result", (Myc.m_Res) ? "Void" : "No");
      Myc.Close();
      return true;
      } // endif m_Res
#endif // 0

    if (!m_Rc && Srcdef)
      if (SetColumnRanks(g))
        return true;

  } else if (Mode == MODE_INSERT) {
    if (Srcdef) {
      strcpy(g->Message, "No insert into anonym views");
      Myc.Close();
      return true;
      } // endif Srcdef

    if (!MakeInsert(g)) {
#if defined(MYSQL_PREPARED_STATEMENTS)
      int n = (Prep) 
            ? Myc.PrepareSQL(g, Query->GetCharValue()) : Nparm;

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

      sprintf(cmd, "ALTER TABLE `%s` DISABLE KEYS", TableName);
      
      m_Rc = Myc.ExecSQL(g, cmd, &w);   // may fail for some engines
      } // endif m_Rc

  } else
//  m_Rc = (Mode == MODE_DELETE) ? MakeDelete(g) : MakeUpdate(g);
    m_Rc = (MakeCommand(g)) ? RC_FX : RC_OK;

  if (m_Rc == RC_FX) {
    Myc.Close();
    return true;
    } // endif rc

  Use = USE_OPEN;
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  Set the rank of columns in the result set.                         */
/***********************************************************************/
bool TDBMYSQL::SetColumnRanks(PGLOBAL g)
  {
  for (PCOL colp = Columns; colp; colp = colp->GetNext())
    if (((PMYCOL)colp)->FindRank(g))
      return true;

  return false;
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
/*  Send an UPDATE or DELETE command to the remote server.             */
/***********************************************************************/
int TDBMYSQL::SendCommand(PGLOBAL g)
  {
  int w;

  if (Myc.ExecSQLcmd(g, Query->GetStr(), &w) == RC_NF) {
    AftRows = Myc.m_Afrw;
    sprintf(g->Message, "%s: %d affected rows", TableName, AftRows);
    PushWarning(g, this, 0);    // 0 means a Note

    if (trace(1))
      htrc("%s\n", g->Message);

    if (w && Myc.ExecSQL(g, "SHOW WARNINGS") == RC_OK) {
      // We got warnings from the remote server
      while (Myc.Fetch(g, -1) == RC_OK) {
        sprintf(g->Message, "%s: (%s) %s", TableName,
                Myc.GetCharField(1), Myc.GetCharField(2));
        PushWarning(g, this);
        } // endwhile Fetch

      Myc.FreeResult();
      } // endif w

    return RC_EF;               // Nothing else to do
  } else
    return RC_FX;               // Error

  } // end of SendCommand

/***********************************************************************/
/*  Data Base indexed read routine for MYSQL access method.            */
/***********************************************************************/
bool TDBMYSQL::ReadKey(PGLOBAL g, OPVAL op, const key_range *kr)
{
  int  oldlen = Query->GetLength();
	PHC  hc = To_Def->GetHandler();

	if (!(kr || hc->end_range) || op == OP_NEXT ||
         Mode == MODE_UPDATE || Mode == MODE_DELETE) {
    if (!kr && Mode == MODE_READX) {
      // This is a false indexed read
      m_Rc = Myc.ExecSQL(g, Query->GetStr());
      Mode = MODE_READ;
      return (m_Rc == RC_FX) ? true : false;
      } // endif key

    return false;
  } else {
    if (Myc.m_Res)
      Myc.FreeResult();

		if (hc->MakeKeyWhere(g, Query, op, '`', kr))
			return true;

    if (To_CondFil) {
			if (To_CondFil->Idx != hc->active_index) {
				To_CondFil->Idx = hc->active_index;
				To_CondFil->Body= (char*)PlugSubAlloc(g, NULL, 0);
				*To_CondFil->Body= 0;

				if ((To_CondFil = hc->CheckCond(g, To_CondFil, Cond)))
					PlugSubAlloc(g, NULL, strlen(To_CondFil->Body) + 1);

				} // endif active_index

			if (To_CondFil)
				if (Query->Append(" AND ") || Query->Append(To_CondFil->Body)) {
				  strcpy(g->Message, "Readkey: Out of memory");
					return true;
					} // endif Append

			} // endif To_Condfil

		Mode = MODE_READ;
	} // endif's op

	if (trace(33))
		htrc("MYSQL ReadKey: Query=%s\n", Query->GetStr());

	m_Rc = Myc.ExecSQL(g, Query->GetStr());
  Query->Truncate(oldlen);
  return (m_Rc == RC_FX) ? true : false;
} // end of ReadKey

/***********************************************************************/
/*  Data Base read routine for MYSQL access method.                    */
/***********************************************************************/
int TDBMYSQL::ReadDB(PGLOBAL g)
  {
  int rc;

  if (trace(2))
    htrc("MySQL ReadDB: R%d Mode=%d\n", GetTdb_No(), Mode);

  if (Mode == MODE_UPDATE || Mode == MODE_DELETE)
    return SendCommand(g);

  /*********************************************************************/
  /*  Now start the reading process.                                   */
  /*  Here is the place to fetch the line.                             */
  /*********************************************************************/
  N++;
  Fetched = ((rc = Myc.Fetch(g, -1)) == RC_OK);

  if (trace(2))
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
  uint len = Query->GetLength();
  char buf[64];

  // Make the Insert command value list
  for (PCOL colp = Columns; colp; colp = colp->GetNext()) {
    if (!colp->GetValue()->IsNull()) {
      if (colp->GetResultType() == TYPE_STRING ||
          colp->GetResultType() == TYPE_DATE)
        Query->Append_quoted(colp->GetValue()->GetCharString(buf));
      else
        Query->Append(colp->GetValue()->GetCharString(buf));
  
    } else
      Query->Append("NULL");
  
    Query->Append(',');
    } // endfor colp

  if (unlikely(Query->IsTruncated())) {
    strcpy(g->Message, "WriteDB: Out of memory");
    rc = RC_FX;
  } else {
    Query->RepLast(')');
    Myc.m_Rows = -1;          // To execute the query
    rc = Myc.ExecSQL(g, Query->GetStr());
    Query->Truncate(len);     // Restore query
  } // endif Query

  return (rc == RC_NF) ? RC_OK : rc;      // RC_NF is Ok
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete all routine for MYSQL access methods.             */
/***********************************************************************/
int TDBMYSQL::DeleteDB(PGLOBAL g, int irc)
  {
  if (irc == RC_FX)
    // Send the DELETE (all) command to the remote table
    return (SendCommand(g) == RC_FX) ? RC_FX : RC_OK;
  else
    return RC_OK;                 // Ignore

  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for MySQL access method.                   */
/***********************************************************************/
void TDBMYSQL::CloseDB(PGLOBAL g)
  {
  if (Myc.Connected()) {
    if (Mode == MODE_INSERT) {
      char cmd[64];
      int  w;
      PDBUSER dup = PlgGetUser(g);

      dup->Step = "Enabling indexes";
      sprintf(cmd, "ALTER TABLE `%s` ENABLE KEYS", TableName);
      Myc.m_Rows = -1;      // To execute the query
      m_Rc = Myc.ExecSQL(g, cmd, &w);  // May fail for some engines
      } // endif m_Rc

    Myc.Close();
    } // endif Myc

  if (trace(1))
    htrc("MySQL CloseDB: closing %s rc=%d\n", Name, m_Rc);

  } // end of CloseDB

// ------------------------ MYSQLCOL functions --------------------------

/***********************************************************************/
/*  MYSQLCOL public constructor.                                       */
/***********************************************************************/
MYSQLCOL::MYSQLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
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
  Precision = Long = cdp->GetLong();
  Bind = NULL;
  To_Val = NULL;
  Slen = 0;
  Rank = -1;            // Not known yet

  if (trace(1))
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of MYSQLCOL constructor

/***********************************************************************/
/*  MYSQLCOL public constructor.                                       */
/***********************************************************************/
MYSQLCOL::MYSQLCOL(MYSQL_FIELD *fld, PTDB tdbp, int i, PCSZ am)
        : COLBLK(NULL, tdbp, i)
  {
  const char *chset = get_charset_name(fld->charsetnr);
//char  v = (!strcmp(chset, "binary")) ? 'B' : 0;
	char  v = 0;

  Name = fld->name;
  Opt = 0;
  Precision = Long = fld->length;
  Buf_Type = MYSQLtoPLG(fld->type, &v);
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

  if (trace(1))
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
    return true;
  } else if (Buf_Type == value->GetType()) {
    // Values are of the (good) column type
    if (Buf_Type == TYPE_DATE) {
      // If any of the date values is formatted
      // output format must be set for the receiving table
      if (GetDomain() || ((DTVAL *)value)->IsFormatted())
        goto newval;          // This will make a new value;

    } else if (Buf_Type == TYPE_DOUBLE)
      // Float values must be written with the correct (column) precision
      // Note: maybe this should be forced by ShowValue instead of this ?
      value->SetPrec(GetScale());

    Value = value;            // Directly access the external value
  } else {
    // Values are not of the (good) column type
    if (check) {
      sprintf(g->Message, MSG(TYPE_VALUE_ERR), Name,
              GetTypeName(Buf_Type), GetTypeName(value->GetType()));
      return true;
      } // endif check

 newval:
    if (InitValue(g))         // Allocate the matching value block
      return true;

  } // endif's Value, Buf_Type

  // Because Colblk's have been made from a copy of the original TDB in
  // case of Update, we must reset them to point to the original one.
  if (To_Tdb->GetOrig())
    To_Tdb = (PTDB)To_Tdb->GetOrig();

  // Set the Column
  Status = (ok) ? BUF_EMPTY : BUF_NO;
  return false;
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
  char  *p, *buf, tim[20];
  int    rc;
  PTDBMY tdbp = (PTDBMY)To_Tdb;

  /*********************************************************************/
  /*  If physical fetching of the line was deferred, do it now.        */
  /*********************************************************************/
  if (!tdbp->Fetched)
    if ((rc = tdbp->Myc.Fetch(g, tdbp->N)) != RC_OK) {
      if (rc == RC_EF)
        sprintf(g->Message, MSG(INV_DEF_READ), rc);

			throw 11;
		} else
      tdbp->Fetched = true;

  if ((buf = ((PTDBMY)To_Tdb)->Myc.GetCharField(Rank))) {
    if (trace(2))
      htrc("MySQL ReadColumn: name=%s buf=%s\n", Name, buf);

    // TODO: have a true way to differenciate temporal values
    if (Buf_Type == TYPE_DATE && strlen(buf) == 8)
      // This is a TIME value
      p = strcat(strcpy(tim, "1970-01-01 "), buf);
    else
      p = buf;

    if (Value->SetValue_char(p, strlen(p))) {
      sprintf(g->Message, "Out of range value for column %s at row %d",
              Name, tdbp->RowNumber(g));
      PushWarning(g, tdbp);
      } // endif SetValue_char

  } else {
    if (Nullable)
      Value->SetNull(true);

    Value->Reset();              // Null value
  } // endif buf

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: make sure the bind buffer is updated.                 */
/***********************************************************************/
void MYSQLCOL::WriteColumn(PGLOBAL)
  {
  /*********************************************************************/
  /*  Do convert the column value if necessary.                        */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, false);   // Convert the inserted value

#if defined(MYSQL_PREPARED_STATEMENTS)
  if (((PTDBMY)To_Tdb)->Prep) {
    if (Buf_Type == TYPE_DATE) {
      Value->ShowValue((char *)Bind->buffer, (int)Bind->buffer_length);
      Slen = strlen((char *)Bind->buffer);
    } else if (IsTypeChar(Buf_Type))
      Slen = strlen(Value->GetCharValue());

    } // endif Prep
#endif   // MYSQL_PREPARED_STATEMENTS

  } // end of WriteColumn

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBMYEXC class.                              */
/***********************************************************************/
TDBMYEXC::TDBMYEXC(PMYDEF tdp) : TDBMYSQL(tdp)
{
  Cmdlist = NULL;
  Cmdcol = NULL;
  Shw = false;
  Havew = false;
  Isw = false;
  Warnings = 0;
  Mxr = tdp->Maxerr;
  Nerr = 0;
} // end of TDBMYEXC constructor

TDBMYEXC::TDBMYEXC(PTDBMYX tdbp) : TDBMYSQL(tdbp)
{
  Cmdlist = tdbp->Cmdlist;
  Cmdcol = tdbp->Cmdcol;
  Shw = tdbp->Shw;
  Havew = tdbp->Havew;
  Isw = tdbp->Isw;
  Mxr = tdbp->Mxr;
  Nerr = tdbp->Nerr;
} // end of TDBMYEXC copy constructor

// Is this really useful ???
PTDB TDBMYEXC::Clone(PTABS t)
  {
  PTDB    tp;
  PCOL    cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBMYEXC(this);

  for (cp1 = Columns; cp1; cp1 = cp1->GetNext()) {
    cp2 = new(g) MYXCOL((PMYXCOL)cp1, tp);

    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

/***********************************************************************/
/*  Allocate MYSQL column description block.                           */
/***********************************************************************/
PCOL TDBMYEXC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  PMYXCOL colp = new(g) MYXCOL(cdp, this, cprec, n);

  if (!colp->Flag)
    Cmdcol = colp->GetName();

  return colp;
  } // end of MakeCol

/***********************************************************************/
/*  MakeCMD: make the SQL statement to send to MYSQL connection.       */
/***********************************************************************/
PCMD TDBMYEXC::MakeCMD(PGLOBAL g)
  {
  PCMD xcmd = NULL;

  if (To_CondFil) {
    if (Cmdcol) {
      if (!stricmp(Cmdcol, To_CondFil->Body) &&
          (To_CondFil->Op == OP_EQ || To_CondFil->Op == OP_IN)) {
        xcmd = To_CondFil->Cmds;
      } else
        strcpy(g->Message, "Invalid command specification filter");

    } else
      strcpy(g->Message, "No command column in select list");

  } else if (!Srcdef)
    strcpy(g->Message, "No Srcdef default command");
  else
    xcmd = new(g) CMD(g, Srcdef);

  return xcmd;
  } // end of MakeCMD

/***********************************************************************/
/*  EXC GetMaxSize: returns the maximum number of rows in the table.   */
/***********************************************************************/
int TDBMYEXC::GetMaxSize(PGLOBAL)
  {
  if (MaxSize < 0) {
    MaxSize = 10;                 // a guess
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  MySQL Exec Access Method opening routine.                          */
/***********************************************************************/
bool TDBMYEXC::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    strcpy(g->Message, "Multiple execution is not allowed");
    return true;
    } // endif use

  /*********************************************************************/
  /*  Open a MySQL connection for this table.                          */
  /*  Note: this may not be the proper way to do. Perhaps it is better */
  /*  to test whether a connection is already open for this server     */
  /*  and if so to allocate just a new result set. But this only for   */
  /*  servers allowing concurency in getting results ???               */
  /*********************************************************************/
  if (!Myc.Connected())
    if (Myc.Open(g, Host, Schema, User, Pwd, Port))
      return true;

  Use = USE_OPEN;       // Do it now in case we are recursively called

  if (Mode != MODE_READ && Mode != MODE_READX) {
    strcpy(g->Message, "No INSERT/DELETE/UPDATE of MYSQL EXEC tables");
    return true;
    } // endif Mode

  /*********************************************************************/
  /*  Get the command to execute.                                      */
  /*********************************************************************/
  if (!(Cmdlist = MakeCMD(g))) {
		// Next lines commented out because of CHECK TABLE
		//Myc.Close();
    //return true;
    } // endif Cmdlist

  return false;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for MYSQL access method.                    */
/***********************************************************************/
int TDBMYEXC::ReadDB(PGLOBAL g)
  {
  if (Havew) {
    // Process result set from SHOW WARNINGS
    if (Myc.Fetch(g, -1) != RC_OK) {
      Myc.FreeResult();
      Havew = Isw = false;
    } else {
      N++;
      Isw = true;
      return RC_OK;
    } // endif Fetch

    } // endif m_Res

  if (Cmdlist) {
    // Process query to send
    int rc;

    do {
      if (Query)
        Query->Set(Cmdlist->Cmd);
      else
        Query = new(g) STRING(g, 0, Cmdlist->Cmd);

      switch (rc = Myc.ExecSQLcmd(g, Query->GetStr(), &Warnings)) {
        case RC_NF:
          AftRows = Myc.m_Afrw;
          strcpy(g->Message, "Affected rows");
          break;
        case RC_OK:
          AftRows = Myc.m_Fields;
          strcpy(g->Message, "Result set columns");
          break;
        case RC_FX:
          AftRows = Myc.m_Afrw;
          Nerr++;
          break;
        case RC_INFO:
          Shw = true;
        } // endswitch rc

      Cmdlist = (Nerr > Mxr) ? NULL : Cmdlist->Next;
      } while (rc == RC_INFO);

    if (Shw && Warnings)
      Havew = (Myc.ExecSQL(g, "SHOW WARNINGS") == RC_OK);

    ++N;
    return RC_OK;
	} else {
		PushWarning(g, this, 1);
		return RC_EF;
	}	// endif Cmdlist

  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for Exec MYSQL access methods.    */
/***********************************************************************/
int TDBMYEXC::WriteDB(PGLOBAL g)
  {
  strcpy(g->Message, "EXEC MYSQL tables are read only");
  return RC_FX;
  } // end of WriteDB

// ------------------------- MYXCOL functions ---------------------------

/***********************************************************************/
/*  MYXCOL public constructor.                                         */
/***********************************************************************/
MYXCOL::MYXCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
      : MYSQLCOL(cdp, tdbp, cprec, i, am)
  {
  // Set additional EXEC MYSQL access method information for column.
  Flag = cdp->GetOffset();
  } // end of MYSQLCOL constructor

/***********************************************************************/
/*  MYSQLCOL public constructor.                                       */
/***********************************************************************/
MYXCOL::MYXCOL(MYSQL_FIELD *fld, PTDB tdbp, int i, PCSZ am)
      : MYSQLCOL(fld, tdbp, i, am)
  {
  if (trace(1))
    htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

  } // end of MYSQLCOL constructor

/***********************************************************************/
/*  MYXCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
MYXCOL::MYXCOL(MYXCOL *col1, PTDB tdbp) : MYSQLCOL(col1, tdbp)
  {
  Flag = col1->Flag;
  } // end of MYXCOL copy constructor

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void MYXCOL::ReadColumn(PGLOBAL g)
  {
  PTDBMYX tdbp = (PTDBMYX)To_Tdb;

  if (tdbp->Isw) {
    char *buf = NULL;

    if (Flag < 3) {
      buf = tdbp->Myc.GetCharField(Flag);
      Value->SetValue_psz(buf);
    } else
      Value->Reset();

  } else
    switch (Flag) {
      case  0: Value->SetValue_psz(tdbp->Query->GetStr()); break;
      case  1: Value->SetValue(tdbp->AftRows);             break;
      case  2: Value->SetValue_psz(g->Message);            break;
      case  3: Value->SetValue(tdbp->Warnings);            break;
      default: Value->SetValue_psz("Invalid Flag");        break;
      } // endswitch Flag

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: should never be called.                               */
/***********************************************************************/
void MYXCOL::WriteColumn(PGLOBAL)
  {
  assert(false);
  } // end of WriteColumn

/* ---------------------------TDBMCL class --------------------------- */

/***********************************************************************/
/*  TDBMCL class constructor.                                          */
/***********************************************************************/
TDBMCL::TDBMCL(PMYDEF tdp) : TDBCAT(tdp)
  {
  Host = tdp->Hostname;
  Db   = tdp->Tabschema;
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
  return MyColumns(g, NULL, Host, Db, User, Pwd, Tab, NULL, Port, false);
  } // end of GetResult
