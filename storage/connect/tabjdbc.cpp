/************* TabJDBC C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: TABJDBC                                               */
/* -------------                                                       */
/*  Version 1.3                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2016-2019    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the TABJDBC class DB execution routines.          */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*    TABJDBC.CPP    - Source code                                     */
/*    PLGDBSEM.H     - DB application declaration file                 */
/*    TABJDBC.H      - TABJDBC classes declaration file                */
/*    GLOBAL.H       - Global declaration file                         */
/*                                                                     */
/*  REQUIRED LIBRARIES:                                                */
/*  -------------------                                                */
/*    Large model C library                                            */
/*                                                                     */
/*  REQUIRED PROGRAMS:                                                 */
/*  ------------------                                                 */
/*    IBM, Borland, GNU or Microsoft C++ Compiler and Linker           */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#define MYSQL_SERVER 1
#include "my_global.h"
#include "sql_class.h"
#include "sql_servers.h"
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#include <sqltypes.h>
#else
#if defined(UNIX)
#include <errno.h>
#define NODW
#include "osutil.h"
#else
#include <io.h>
#endif
#include <fcntl.h>
#endif

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  kindex.h    is kindex header that also includes tabdos.h.          */
/*  tabJDBC.h   is header containing the TABJDBC class declarations.   */
/*  JDBConn.h   is header containing JDBC connection declarations.     */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "mycat.h"
#include "xtable.h"
#include "tabext.h"
#include "tabjdbc.h"
#include "tabmul.h"
#include "tabcol.h"
#include "valblk.h"
#include "ha_connect.h"

#include "sql_string.h"

/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
//     int num_read, num_there, num_eq[2], num_nf;        // Statistics
extern int num_read, num_there, num_eq[2];                // Statistics

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
bool ExactInfo(void);
#if defined(DEVELOPMENT)
extern char *GetUserVariable(PGLOBAL g, const uchar *varname);
#endif  // DEVELOPMENT

/* -------------------------- Class JDBCDEF -------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
JDBCDEF::JDBCDEF(void)
{
	Driver = Url = Wrapname = NULL;
}  // end of JDBCDEF constructor

/***********************************************************************/
/*  Called on table construction.                                      */
/***********************************************************************/
bool JDBCDEF::SetParms(PJPARM sjp)
{
	sjp->Url= Url;
	sjp->User= Username;
	sjp->Pwd= Password;
//sjp->Properties = Prop;
	return true;
}  // end of SetParms

/***********************************************************************/
/* Parse connection string                                             */
/*                                                                     */
/* SYNOPSIS                                                            */
/*   ParseURL()                                                        */
/*   Url                 The connection string to parse                */
/*                                                                     */
/* DESCRIPTION                                                         */
/*   This is used to set the Url in case a wrapper server as been      */
/*   specified. This is rather experimental yet.                       */
/*                                                                     */
/* RETURN VALUE                                                        */
/*   RC_OK       Url was a true URL                                    */
/*   RC_NF       Url was a server name/table                           */
/*   RC_FX       Error                                                 */
/*                                                                     */
/***********************************************************************/
int JDBCDEF::ParseURL(PGLOBAL g, char *url, bool b)
{
	if (strncmp(url, "jdbc:", 5)) {
		PSZ p;

		// No "jdbc:" in connection string. Must be a straight
		// "server" or "server/table"
		// ok, so we do a little parsing, but not completely!
		if ((p = strchr(url, '/'))) {
			// If there is a single '/' in the connection string,
			// this means the user is specifying a table name
			*p++= '\0';

			// there better not be any more '/'s !
			if (strchr(p, '/'))
				return RC_FX;

			Tabname = p;
		} // endif

		if (trace(1))
			htrc("server: %s Tabname: %s", url, Tabname);

		// Now make the required URL
		FOREIGN_SERVER *server, server_buffer;

		// get_server_by_name() clones the server if exists
		if (!(server= get_server_by_name(current_thd->mem_root, url, &server_buffer))) {
			sprintf(g->Message, "Server %s does not exist!", url);
			return RC_FX;
		} // endif server

#if defined(DEVELOPMENT)
		if (*server->host == '@') {
			Url = GetUserVariable(g, (const uchar*)&server->host[1]);
		} else
#endif // 0
		if (strncmp(server->host, "jdbc:", 5)) {
			// Now make the required URL
			Url = (PSZ)PlugSubAlloc(g, NULL, 0);
			strcat(strcpy(Url, "jdbc:"), server->scheme);
			strcat(strcat(Url, "://"), server->host);

			if (server->port) {
				char buf[16];

				sprintf(buf, "%ld", server->port);
				strcat(strcat(Url, ":"), buf);
			} // endif port

			if (server->db)
				strcat(strcat(Url, "/"), server->db);

			PlugSubAlloc(g, NULL, strlen(Url) + 1);
		} else		 // host is a URL
			Url = PlugDup(g, server->host);

		if (!Tabschema && server->db)
			Tabschema = PlugDup(g, server->db);

		if (!Username && server->username)
			Username = PlugDup(g, server->username);

		if (!Password && server->password)
			Password = PlugDup(g, server->password);

		Driver = PlugDup(g, GetListOption(g, "Driver", server->owner, NULL));
		Wrapname = PlugDup(g, GetListOption(g, "Wrapper", server->owner, NULL));
		Memory = atoi(GetListOption(g, "Memory", server->owner, "0"));
		return RC_NF;
	} // endif

	// Url was a JDBC URL, nothing to do
	return RC_OK;
} // end of ParseURL

/***********************************************************************/
/*  DefineAM: define specific AM block values from JDBC file.          */
/***********************************************************************/
bool JDBCDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
	int rc = RC_OK;

	if (EXTDEF::DefineAM(g, am, poff))
		return true;

	Desc = Url = GetStringCatInfo(g, "Connect", NULL);

	if (!Url && !Catfunc) {
		// Look in the option list (deprecated)
		Url = GetStringCatInfo(g, "Url", NULL);

		if (!Url) {
			sprintf(g->Message, "Missing URL for JDBC table %s", Name);
			return true;
		} // endif Url

	} // endif Connect

	if (Url)
		if ((rc = ParseURL(g, Url)) == RC_FX) {
			sprintf(g->Message, "Wrong JDBC URL %s", Url);
			return true;
		} // endif rc

	// Default values may have been set in ParseURL
	Memory = GetIntCatInfo("Memory", Memory);
	Driver = GetStringCatInfo(g, "Driver", Driver);
	Wrapname = GetStringCatInfo(g, "Wrapper", Wrapname);
	return false;
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB JDBCDEF::GetTable(PGLOBAL g, MODE m)
{
	PTDB tdbp = NULL;

	/*********************************************************************/
	/*  Allocate a TDB of the proper type.                               */
	/*  Column blocks will be allocated only when needed.                */
	/*********************************************************************/
	if (Xsrc)
		tdbp = new(g)TDBXJDC(this);
	else switch (Catfunc) {
		case FNC_COL:
			tdbp = new(g)TDBJDBCL(this);
			break;
#if 0
		case FNC_DSN:
			tdbp = new(g)TDBJSRC(this);
			break;
#endif // 0
		case FNC_TABLE:
			tdbp = new(g)TDBJTB(this);
			break;
		case FNC_DRIVER:
			tdbp = new(g)TDBJDRV(this);
			break;
		default:
			tdbp = new(g)TDBJDBC(this);

			if (Multiple == 1)
				tdbp = new(g)TDBMUL(tdbp);
			else if (Multiple == 2)
				strcpy(g->Message, "NO_JDBC_MUL");

		} // endswitch Catfunc

	return tdbp;
} // end of GetTable

/***********************************************************************/
/*  The MySQL and MariaDB JDBC drivers return by default a result set  */
/*  containing the entire result of the executed query. This can be an */
/*  issue for big tables and memory error can occur. An alternative is */
/*  to use streaming (reading one row at a time) but to specify this,  */
/*  a fech size of the integer min value must be send to the driver.   */
/***********************************************************************/
int JDBCPARM::CheckSize(int rows)
{
	if (Url && rows == 1) {
		// Are we connected to a MySQL JDBC connector?
		bool b = (!strncmp(Url, "jdbc:mysql:", 11) ||
			        !strncmp(Url, "jdbc:mariadb:", 13));
		return b ? INT_MIN32 : rows;
	} else
		return rows;

} // end of CheckSize

/* -------------------------- Class TDBJDBC -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBJDBC class.                               */
/***********************************************************************/
TDBJDBC::TDBJDBC(PJDBCDEF tdp) : TDBEXT(tdp)
{
	Jcp = NULL;
	Cnp = NULL;

	if (tdp) {
		Ops.Driver = tdp->Driver;
		Ops.Url = tdp->Url;
		Wrapname = tdp->Wrapname;
		Ops.User = tdp->Username;
		Ops.Pwd = tdp->Password;
		Ops.Scrollable = tdp->Scrollable;
	} else {
		Wrapname = NULL;
		Ops.Driver = NULL;
		Ops.Url = NULL;
		Ops.User = NULL;
		Ops.Pwd = NULL;
		Ops.Scrollable = false;
	} // endif tdp

	Prepared = false;
	Werr = false;
	Rerr = false;
	Ops.Fsize = Ops.CheckSize(Rows);
} // end of TDBJDBC standard constructor

TDBJDBC::TDBJDBC(PTDBJDBC tdbp) : TDBEXT(tdbp)
{
	Jcp = tdbp->Jcp;            // is that right ?
	Cnp = tdbp->Cnp;
	Wrapname = tdbp->Wrapname;
	Ops = tdbp->Ops;
	Prepared = tdbp->Prepared;
	Werr = tdbp->Werr;
	Rerr = tdbp->Rerr;
} // end of TDBJDBC copy constructor

// Method
PTDB TDBJDBC::Clone(PTABS t)
{
	PTDB     tp;
	PJDBCCOL cp1, cp2;
	PGLOBAL  g = t->G;        // Is this really useful ???

	tp = new(g)TDBJDBC(this);

	for (cp1 = (PJDBCCOL)Columns; cp1; cp1 = (PJDBCCOL)cp1->GetNext()) {
		cp2 = new(g)JDBCCOL(cp1, tp);  // Make a copy
		NewPointer(t, cp1, cp2);
	} // endfor cp1

	return tp;
} // end of Clone

/***********************************************************************/
/*  Allocate JDBC column description block.                            */
/***********************************************************************/
PCOL TDBJDBC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
	return new(g)JDBCCOL(cdp, this, cprec, n);
} // end of MakeCol

/***********************************************************************/
/*  MakeInsert: make the Insert statement used with JDBC connection.   */
/***********************************************************************/
bool TDBJDBC::MakeInsert(PGLOBAL g)
{
	PCSZ   schmp = NULL;
	char  *catp = NULL, buf[NAM_LEN * 3];
	int    len = 0;
	uint   pos;
	bool   b = false;
	// PTABLE tablep = To_Table;
	PCOL   colp;

	for (colp = Columns; colp; colp = colp->GetNext())
		if (colp->IsSpecial()) {
			strcpy(g->Message, "No JDBC special columns");
			return true;
		} else {
			// Column name can be encoded in UTF-8
			Decode(colp->GetName(), buf, sizeof(buf));
			len += (strlen(buf) + 6);	 // comma + quotes + valist
			((PEXTCOL)colp)->SetRank(++Nparm);
		} // endif colp

	// Below 32 is enough to contain the fixed part of the query
	if (Catalog && *Catalog)
		catp = Catalog;

	if (catp)
		len += strlen(catp) + 1;

	//if (tablep->GetSchema())
	//	schmp = (char*)tablep->GetSchema();
	//else
	if (Schema && *Schema)
		schmp = Schema;

	if (schmp)
		len += strlen(schmp) + 1;

	// Table name can be encoded in UTF-8
	Decode(TableName, buf, sizeof(buf));
	len += (strlen(buf) + 32);
	Query = new(g)STRING(g, len, "INSERT INTO ");

	if (catp) {
		Query->Append(catp);

		if (schmp) {
			Query->Append('.');
			Query->Append(schmp);
		} // endif schmp

		Query->Append('.');
	} else if (schmp) {
		Query->Append(schmp);
		Query->Append('.');
	} // endif schmp

	if (Quote) {
		// Put table name between identifier quotes in case in contains blanks
		Query->Append(Quote);
		Query->Append(buf);
		Query->Append(Quote);
	} else
		Query->Append(buf);

	Query->Append('(');

	for (colp = Columns; colp; colp = colp->GetNext()) {
		if (b)
			Query->Append(", ");
		else
			b = true;

		// Column name can be in UTF-8 encoding
		Decode(colp->GetName(), buf, sizeof(buf));

		if (Quote) {
			// Put column name between identifier quotes in case in contains blanks
			Query->Append(Quote);
			Query->Append(buf);
			Query->Append(Quote);
		} else
			Query->Append(buf);

	} // endfor colp

	if ((Query->Append(") VALUES ("))) {
		strcpy(g->Message, "MakeInsert: Out of memory");
		return true;
	} else // in case prepared statement fails
		pos = Query->GetLength();

	// Make prepared statement
	for (int i = 0; i < Nparm; i++)
		Query->Append("?,");

	if (Query->IsTruncated()) {
		strcpy(g->Message, "MakeInsert: Out of memory");
		return true;
	} else
		Query->RepLast(')');

	// Now see if we can use prepared statement
	if (Jcp->PrepareSQL(Query->GetStr()))
		Query->Truncate(pos);     // Restore query to not prepared
	else
		Prepared = true;

	if (trace(33))
		htrc("Insert=%s\n", Query->GetStr());

	return false;
} // end of MakeInsert

/***********************************************************************/
/*  JDBC Set Parameter function.                                       */
/***********************************************************************/
bool TDBJDBC::SetParameters(PGLOBAL g)
{
	PJDBCCOL colp;

	for (colp = (PJDBCCOL)Columns; colp; colp = (PJDBCCOL)colp->Next)
		if (Jcp->SetParam(colp))
			return true;

	return false;
} // end of SetParameters

/***********************************************************************/
/*  ResetSize: call by TDBMUL when calculating size estimate.          */
/***********************************************************************/
void TDBJDBC::ResetSize(void)
{
	MaxSize = -1;

	if (Jcp && Jcp->IsOpen())
		Jcp->Close();

} // end of ResetSize

/***********************************************************************/
/*  JDBC Cardinality: returns table size in number of rows.            */
/***********************************************************************/
int TDBJDBC::Cardinality(PGLOBAL g)
{
	if (!g)
		return (Mode == MODE_ANY && !Srcdef) ? 1 : 0;

#if 0
	if (Cardinal < 0 && Mode == MODE_ANY && !Srcdef && ExactInfo()) {
		// Info command, we must return the exact table row number
		char     qry[96], tbn[64];
		JDBConn *jcp = new(g)JDBConn(g, this);

		if (jcp->Open(&Ops) == RC_FX)
			return -1;

		// Table name can be encoded in UTF-8
		Decode(TableName, tbn, sizeof(tbn));
		strcpy(qry, "SELECT COUNT(*) FROM ");

		if (Quote)
			strcat(strcat(strcat(qry, Quote), tbn), Quote);
		else
			strcat(qry, tbn);

		// Allocate a Count(*) column (must not use the default constructor)
		Cnp = new(g)JDBCCOL;
		Cnp->InitValue(g);

		if ((Cardinal = jcp->GetResultSize(qry, Cnp)) < 0)
			return -3;

		jcp->Close();
	} else
#endif // 0
		Cardinal = 10;    // To make MariaDB happy

	return Cardinal;
} // end of Cardinality

/***********************************************************************/
/*  JDBC Access Method opening routine.                                */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBJDBC::OpenDB(PGLOBAL g)
{
	bool rc = true;

	if (trace(1))
		htrc("JDBC OpenDB: tdbp=%p tdb=R%d use=%d mode=%d\n",
		     this, Tdb_No, Use, Mode);

	if (Use == USE_OPEN) {
		if (Mode == MODE_READ || Mode == MODE_READX) {
			/*****************************************************************/
			/*  Table already open, just replace it at its beginning.        */
			/*****************************************************************/
			if (Memory == 1) {
				if ((Qrp = Jcp->AllocateResult(g, this)))
					Memory = 2;            // Must be filled
				else
					Memory = 0;            // Allocation failed, don't use it

			} else if (Memory == 2)
				Memory = 3;              // Ok to use memory result

			if (Memory < 3) {
				// Method will depend on cursor type
				if ((Rbuf = Query ? Jcp->Rewind(Query->GetStr()) : 0) < 0)
                                {
					if (Mode != MODE_READX) {
						Jcp->Close();
						return true;
					} else
						Rbuf = 0;
                                }

			} else
				Rbuf = Qrp->Nblin;

			CurNum = 0;
			Fpos = 0;
			Curpos = 1;
		} else if (Mode == MODE_UPDATE || Mode == MODE_DELETE) {
			// new update coming from a trigger or procedure
			Query = NULL;
			SetCondFil(NULL);
			Qrystr = To_Def->GetStringCatInfo(g, "Query_String", "?");
		} else {  //if (Mode == MODE_INSERT)
		} // endif Mode

		return false;
	} // endif use

	/*********************************************************************/
	/*  Open an JDBC connection for this table.                          */
	/*  Note: this may not be the proper way to do. Perhaps it is better */
	/*  to test whether a connection is already open for this datasource */
	/*  and if so to allocate just a new result set. But this only for   */
	/*  drivers allowing concurency in getting results ???               */
	/*********************************************************************/
	if (!Jcp)
		Jcp = new(g)JDBConn(g, Wrapname);
	else if (Jcp->IsOpen())
		Jcp->Close();

	if (Jcp->Connect(&Ops))
		return true;
	else if (Quoted)
		Quote = Jcp->GetQuoteChar();

	if (Mode != MODE_READ && Mode != MODE_READX)
		if (Jcp->SetUUID(g, this))
			PushWarning(g, this, 1);

	Use = USE_OPEN;       // Do it now in case we are recursively called

	/*********************************************************************/
	/* Make the command and allocate whatever is used for getting results*/
	/*********************************************************************/
	if (Mode == MODE_READ || Mode == MODE_READX) {
		if (Memory > 1 && !Srcdef) {
			int n;

			if (!MakeSQL(g, true)) {
				// Allocate a Count(*) column
				Cnp = new(g)JDBCCOL;
				Cnp->InitValue(g);

				if ((n = Jcp->GetResultSize(Query->GetStr(), Cnp)) < 0) {
					char* msg = PlugDup(g, g->Message);

					sprintf(g->Message, "Get result size: %s (rc=%d)", msg, n);
					return true;
				} else if (n) {
					Jcp->m_Rows = n;

					if ((Qrp = Jcp->AllocateResult(g, this)))
						Memory = 2;            // Must be filled
					else {
						strcpy(g->Message, "Result set memory allocation failed");
						return true;
					} // endif n

				} else				 // Void result
					Memory = 0;

				Jcp->m_Rows = 0;
			} else
				return true;

		} // endif Memory

		if (!(rc = MakeSQL(g, false))) {
//		for (PJDBCCOL colp = (PJDBCCOL)Columns; colp; colp = (PJDBCCOL)colp->GetNext())
//			if (!colp->IsSpecial())
//				colp->AllocateBuffers(g, Rows);

			rc = (Mode == MODE_READ)
				? (Jcp->ExecuteQuery(Query->GetStr()) != RC_OK)
				: false;
		} // endif rc

	} else if (Mode == MODE_INSERT) {
#if 0
		if (!(rc = MakeInsert(g))) {
			if (Nparm != Jcp->PrepareSQL(Query->GetStr())) {
				strcpy(g->Message, MSG(PARM_CNT_MISS));
				rc = true;
			} else
				rc = BindParameters(g);

		} // endif rc
#endif // 0
		rc = MakeInsert(g);
	} else if (Mode == MODE_UPDATE || Mode == MODE_DELETE) {
		rc = false;  // wait for CheckCond before calling MakeCommand(g);
	} else
		sprintf(g->Message, "Invalid mode %d", Mode);

	if (rc) {
		Jcp->Close();
		return true;
	} // endif rc

	/*********************************************************************/
	/*  Reset statistics values.                                         */
	/*********************************************************************/
	num_read = num_there = num_eq[0] = num_eq[1] = 0;
	return false;
} // end of OpenDB

#if 0
/***********************************************************************/
/*  GetRecpos: return the position of last read record.                */
/***********************************************************************/
int TDBJDBC::GetRecpos(void)
{
	return Fpos;
} // end of GetRecpos
#endif // 0

/***********************************************************************/
/*  SetRecpos: set the position of next read record.                   */
/***********************************************************************/
bool TDBJDBC::SetRecpos(PGLOBAL g, int recpos)
{
	if (Jcp->m_Full) {
		Fpos = 0;
		CurNum = 1;
	} else if (Memory == 3) {
		Fpos = 0;
		CurNum = recpos;
	} else if (Ops.Scrollable) {
		// Is new position in the current row set?
		if (recpos > 0 && recpos <= Rbuf) {
		  CurNum = recpos;
			Fpos = recpos;
		} else {
			strcpy(g->Message, "Scrolling out of row set NIY");
			return true;
		} // endif recpos

	} else {
		strcpy(g->Message, "This action requires a scrollable cursor");
		return true;
	} // endif's

	// Indicate the table position was externally set
	Placed = true;
	return false;
} // end of SetRecpos

/***********************************************************************/
/*  Data Base indexed read routine for JDBC access method.             */
/***********************************************************************/
bool TDBJDBC::ReadKey(PGLOBAL g, OPVAL op, const key_range *kr)
{
	char c = Quote ? *Quote : 0;
	int  rc, oldlen = Query->GetLength();
	PHC  hc = To_Def->GetHandler();

	if (!(kr || hc->end_range) || op == OP_NEXT ||
		     Mode == MODE_UPDATE || Mode == MODE_DELETE) {
		if (!kr && Mode == MODE_READX) {
			// This is a false indexed read
			rc = Jcp->ExecuteQuery((char*)Query->GetStr());
			Mode = MODE_READ;
			Rows = 1;												 // ???
			return (rc != RC_OK);
		} // endif key

		return false;
	} else {
		if (hc->MakeKeyWhere(g, Query, op, c, kr))
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
		htrc("JDBC ReadKey: Query=%s\n", Query->GetStr());

	rc = Jcp->ExecuteQuery((char*)Query->GetStr());
	Query->Truncate(oldlen);
	Rows = 1;                        // ???
	return (rc != RC_OK);
} // end of ReadKey

/***********************************************************************/
/*  Data Base read routine for JDBC access method.                     */
/***********************************************************************/
int TDBJDBC::ReadDB(PGLOBAL g)
{
	int  rc;

	if (trace(2))
		htrc("JDBC ReadDB: R%d Mode=%d\n", GetTdb_No(), Mode);

	if (Mode == MODE_UPDATE || Mode == MODE_DELETE) {
		if (!Query && MakeCommand(g))
			return RC_FX;

		// Send the UPDATE/DELETE command to the remote table
		rc = Jcp->ExecuteUpdate(Query->GetStr());

		if (rc == RC_OK) {
			AftRows = Jcp->m_Aff;
			return RC_EF;               // Nothing else to do
		} else {
			Werr = true;
			return RC_FX;
		} // endif rc

	} // endif Mode

	/*********************************************************************/
	/*  Now start the reading process.                                   */
	/*  Here is the place to fetch the line(s).                          */
	/*********************************************************************/
	if (Placed) {
		if (Fpos && CurNum >= 0)
			Rbuf = Jcp->Fetch((Curpos = Fpos));
		else
			Fpos = CurNum;

		rc = (Rbuf > 0) ? RC_OK : (Rbuf == 0) ? RC_EF : RC_FX;
		Placed = false;
	} else {
		if (Memory != 3) {
			if (++CurNum >= Rbuf) {
				Rbuf = Jcp->Fetch();
				Curpos = Fpos + 1;
				CurNum = 0;
			} // endif CurNum

			rc = (Rbuf > 0) ? RC_OK : (Rbuf == 0) ? RC_EF : RC_FX;
		} else                 // Getting result from memory
			rc = (Fpos < Qrp->Nblin) ? RC_OK : RC_EF;

		if (rc == RC_OK) {
			if (Memory == 2)
				Qrp->Nblin++;

			Fpos++;                // Used for memory and pos
		} // endif rc

	} // endif placed

	if (trace(2))
		htrc(" Read: Rbuf=%d rc=%d\n", Rbuf, rc);

	return rc;
} // end of ReadDB

/***********************************************************************/
/*  Data Base Insert write routine for JDBC access method.             */
/***********************************************************************/
int TDBJDBC::WriteDB(PGLOBAL g)
{
	int  rc;

	if (Prepared) {
		if (SetParameters(g)) {
			Werr = true;
			rc = RC_FX;
		} else if ((rc = Jcp->ExecuteSQL()) == RC_OK)
			AftRows += Jcp->m_Aff;
		else
			Werr = true;

		return rc;
	} // endif  Prepared

	// Statement was not prepared, we must construct and execute
	// an insert query for each line to insert
	uint len = Query->GetLength();
	char buf[64];

	// Make the Insert command value list
	for (PCOL colp = Columns; colp; colp = colp->GetNext()) {
		if (!colp->GetValue()->IsNull()) {
			char *s = colp->GetValue()->GetCharString(buf);

			if (colp->GetResultType() == TYPE_STRING)
				Query->Append_quoted(s);
			else if (colp->GetResultType() == TYPE_DATE) {
				DTVAL *dtv = (DTVAL*)colp->GetValue();

				if (dtv->IsFormatted())
					Query->Append_quoted(s);
				else
					Query->Append(s);

			} else
				Query->Append(s);

		} else
			Query->Append("NULL");

		Query->Append(',');
	} // endfor colp

	if (unlikely(Query->IsTruncated())) {
		strcpy(g->Message, "WriteDB: Out of memory");
		return RC_FX;
	} // endif Query

	Query->RepLast(')');

	if (trace(2))
		htrc("Inserting: %s\n", Query->GetStr());

	rc = Jcp->ExecuteUpdate(Query->GetStr());
	Query->Truncate(len);     // Restore query

	if (rc == RC_OK)
		AftRows += Jcp->m_Aff;
	else
		Werr = true;

	return rc;
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for JDBC access method.              */
/***********************************************************************/
int TDBJDBC::DeleteDB(PGLOBAL g, int irc)
{
	if (irc == RC_FX) {
		if (!Query && MakeCommand(g))
			return RC_FX;

		// Send the DELETE (all) command to the remote table
		if (Jcp->ExecuteUpdate(Query->GetStr()) == RC_OK) {
			AftRows = Jcp->m_Aff;
			sprintf(g->Message, "%s: %d affected rows", TableName, AftRows);

			if (trace(1))
				htrc("%s\n", g->Message);

			PushWarning(g, this, 0);    // 0 means a Note
			return RC_OK;               // This is a delete all
		} else
			return RC_FX;               // Error

	} else
		return RC_OK;                 // Ignore

} // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for JDBC access method.                    */
/***********************************************************************/
void TDBJDBC::CloseDB(PGLOBAL g)
{
	if (Jcp)
		Jcp->Close();

	if (trace(1))
		htrc("JDBC CloseDB: closing %s\n", Name);

	if (!Werr && 
		(Mode == MODE_INSERT || Mode == MODE_UPDATE || Mode == MODE_DELETE)) {
		sprintf(g->Message, "%s: %d affected rows", TableName, AftRows);

		if (trace(1))
			htrc("%s\n", g->Message);

		PushWarning(g, this, 0);    // 0 means a Note
	}	// endif Mode

	Prepared = false;
} // end of CloseDB

/* --------------------------- JDBCCOL ------------------------------- */

/***********************************************************************/
/*  JDBCCOL public constructor.                                        */
/***********************************************************************/
JDBCCOL::JDBCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
	     : EXTCOL(cdp, tdbp, cprec, i, am)
{
	uuid = false;
} // end of JDBCCOL constructor

/***********************************************************************/
/*  JDBCCOL private constructor.                                       */
/***********************************************************************/
JDBCCOL::JDBCCOL(void) : EXTCOL()
{
	uuid = false;
} // end of JDBCCOL constructor

/***********************************************************************/
/*  JDBCCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
JDBCCOL::JDBCCOL(JDBCCOL *col1, PTDB tdbp) : EXTCOL(col1, tdbp)
{
	uuid = col1->uuid;
} // end of JDBCCOL copy constructor

/***********************************************************************/
/*  ReadColumn: retrieve the column value via the JDBC driver.         */
/***********************************************************************/
void JDBCCOL::ReadColumn(PGLOBAL g)
{
	PTDBJDBC tdbp = (PTDBJDBC)To_Tdb;
	int i = tdbp->Fpos - 1;

	if (tdbp->Memory == 3) {
		// Get the value from the stored memory
		if (Crp->Nulls && Crp->Nulls[i] == '*') {
			Value->Reset();
			Value->SetNull(true);
		} else {
			Value->SetValue_pvblk(Crp->Kdata, i);
			Value->SetNull(false);
		} // endif Nulls

		return;
	} // endif Memory

	/*********************************************************************/
	/*  Get the column value.                                            */
	/*********************************************************************/
	tdbp->Jcp->SetColumnValue(Rank, Name, Value);

	if (tdbp->Memory != 2)
		return;

	/*********************************************************************/
	/*  Fill the allocated result structure.                             */
	/*********************************************************************/
	if (Value->IsNull()) {
		if (Crp->Nulls)
			Crp->Nulls[i] = '*';           // Null value

		Crp->Kdata->Reset(i);
	} else
		Crp->Kdata->SetValue(Value, i);

} // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: Convert if necessary.                                 */
/***********************************************************************/
void JDBCCOL::WriteColumn(PGLOBAL g)
{
	/*********************************************************************/
	/*  Do convert the column value if necessary.                        */
	/*********************************************************************/
	if (Value != To_Val)
		Value->SetValue_pval(To_Val, FALSE);   // Convert the inserted value

} // end of WriteColumn

/* -------------------------- Class TDBXJDC -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBXJDC class.                               */
/***********************************************************************/
TDBXJDC::TDBXJDC(PJDBCDEF tdp) : TDBJDBC(tdp)
{
	Cmdlist = NULL;
	Cmdcol = NULL;
	Mxr = tdp->Maxerr;
	Nerr = 0;
} // end of TDBXJDC constructor

/***********************************************************************/
/*  Allocate XSRC column description block.                            */
/***********************************************************************/
PCOL TDBXJDC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
	PJSRCCOL colp = new(g)JSRCCOL(cdp, this, cprec, n);

	if (!colp->Flag)
		Cmdcol = colp->GetName();

	return colp;
} // end of MakeCol

/***********************************************************************/
/*  MakeCMD: make the SQL statement to send to JDBC connection.        */
/***********************************************************************/
PCMD TDBXJDC::MakeCMD(PGLOBAL g)
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
/*  XDBC GetMaxSize: returns table size (not always one row).          */
/***********************************************************************/
int TDBXJDC::GetMaxSize(PGLOBAL g)
{
	if (MaxSize < 0)
		MaxSize = 2;             // Just a guess

	return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  JDBC Access Method opening routine.                                */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBXJDC::OpenDB(PGLOBAL g)
{
	if (trace(1))
		htrc("JDBC OpenDB: tdbp=%p tdb=R%d use=%d mode=%d\n",
		this, Tdb_No, Use, Mode);

	if (Use == USE_OPEN) {
		strcpy(g->Message, "Multiple execution is not allowed");
		return true;
	} // endif use

	/*********************************************************************/
	/*  Open an JDBC connection for this table.                          */
	/*  Note: this may not be the proper way to do. Perhaps it is better */
	/*  to test whether a connection is already open for this datasource */
	/*  and if so to allocate just a new result set. But this only for   */
	/*  drivers allowing concurency in getting results ???               */
	/*********************************************************************/
	if (!Jcp) {
		Jcp = new(g) JDBConn(g, Wrapname);
	} else if (Jcp->IsOpen())
		Jcp->Close();

	if (Jcp->Connect(&Ops))
		return true;

	Use = USE_OPEN;       // Do it now in case we are recursively called

	if (Mode != MODE_READ && Mode != MODE_READX) {
		strcpy(g->Message, "No INSERT/DELETE/UPDATE of XJDBC tables");
		return true;
	} // endif Mode

	/*********************************************************************/
	/*  Get the command to execute.                                      */
	/*********************************************************************/
	if (!(Cmdlist = MakeCMD(g))) {
		// Next lines commented out because of CHECK TABLE
		//Jcp->Close();
		//return true;
	} // endif Query

	Rows = 1;
	return false;
} // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for xdbc access method.             */
/***********************************************************************/
int TDBXJDC::ReadDB(PGLOBAL g)
{
	if (Cmdlist) {
		int rc;

		if (!Query)
			Query = new(g) STRING(g, 0, Cmdlist->Cmd);
		else
			Query->Set(Cmdlist->Cmd);

		if ((rc = Jcp->ExecuteCommand(Query->GetStr())) == RC_FX)
			Nerr++;

		if (rc == RC_NF)
			AftRows = Jcp->m_Aff;
		else if (rc == RC_OK)
			AftRows = Jcp->m_Ncol;

		Fpos++;                // Used for progress info
		Cmdlist = (Nerr > Mxr) ? NULL : Cmdlist->Next;
		return RC_OK;
	} else {
		PushWarning(g, this, 1);
		return RC_EF;
	}	// endif Cmdlist

} // end of ReadDB

/***********************************************************************/
/*  Data Base write line routine for JDBC access method.               */
/***********************************************************************/
int TDBXJDC::WriteDB(PGLOBAL g)
{
	strcpy(g->Message, "Execsrc tables are read only");
	return RC_FX;
} // end of DeleteDB

/***********************************************************************/
/*  Data Base delete line routine for JDBC access method.              */
/***********************************************************************/
int TDBXJDC::DeleteDB(PGLOBAL g, int irc)
{
	strcpy(g->Message, "NO_XJDBC_DELETE");
	return RC_FX;
} // end of DeleteDB

/* --------------------------- JSRCCOL ------------------------------- */

/***********************************************************************/
/*  JSRCCOL public constructor.                                        */
/***********************************************************************/
JSRCCOL::JSRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
	     : JDBCCOL(cdp, tdbp, cprec, i, am)
{
	// Set additional JDBC access method information for column.
	Flag = cdp->GetOffset();
} // end of JSRCCOL constructor

/***********************************************************************/
/*  ReadColumn: set column value according to Flag.                    */
/***********************************************************************/
void JSRCCOL::ReadColumn(PGLOBAL g)
{
	PTDBXJDC tdbp = (PTDBXJDC)To_Tdb;

	switch (Flag) {
	case  0: Value->SetValue_psz(tdbp->Query->GetStr()); break;
	case  1: Value->SetValue(tdbp->AftRows);             break;
	case  2: Value->SetValue_psz(g->Message);            break;
	default: Value->SetValue_psz("Invalid Flag");        break;
	} // endswitch Flag

} // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: Should never be called.                               */
/***********************************************************************/
void JSRCCOL::WriteColumn(PGLOBAL g)
{
	// Should never be called
} // end of WriteColumn

/* ---------------------------TDBJDRV class -------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of JDBC drivers.                           */
/***********************************************************************/
PQRYRES TDBJDRV::GetResult(PGLOBAL g)
{
	return JDBCDrivers(g, Maxres, false);
} // end of GetResult

/* ---------------------------TDBJTB class --------------------------- */

/***********************************************************************/
/*  TDBJTB class constructor.                                          */
/***********************************************************************/
TDBJTB::TDBJTB(PJDBCDEF tdp) : TDBJDRV(tdp)
{
	Schema = tdp->Tabschema;
	Tab = tdp->Tabname;
	Tabtype = tdp->Tabtyp;
	Ops.Driver = tdp->Driver;
	Ops.Url = tdp->Url;
	Ops.User = tdp->Username;
	Ops.Pwd = tdp->Password;
	Ops.Fsize = 0;
	Ops.Scrollable = false;
} // end of TDBJTB constructor

/***********************************************************************/
/*  GetResult: Get the list of JDBC tables.                            */
/***********************************************************************/
PQRYRES TDBJTB::GetResult(PGLOBAL g)
{
	return JDBCTables(g, Schema, Tab, Tabtype, Maxres, false, &Ops);
} // end of GetResult

/* --------------------------TDBJDBCL class -------------------------- */

/***********************************************************************/
/*  TDBJDBCL class constructor.                                        */
/***********************************************************************/
TDBJDBCL::TDBJDBCL(PJDBCDEF tdp) : TDBJTB(tdp)
{
	Colpat = tdp->Colpat;
} // end of TDBJDBCL constructor

/***********************************************************************/
/*  GetResult: Get the list of JDBC table columns.                     */
/***********************************************************************/
PQRYRES TDBJDBCL::GetResult(PGLOBAL g)
{
	return JDBCColumns(g, Schema, Tab, Colpat, Maxres, false, &Ops);
} // end of GetResult
