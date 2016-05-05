/************* TabJDBC C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: TABJDBC                                               */
/* -------------                                                       */
/*  Version 1.0                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2016         */
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
#include "my_global.h"
#include "sql_class.h"
#if defined(__WIN__)
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
#include "jdbccat.h"
#include "tabjdbc.h"
#include "tabmul.h"
#include "reldef.h"
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

/* -------------------------- Class JDBCDEF -------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
JDBCDEF::JDBCDEF(void)
{
	Jpath = Driver = Url = Tabname = Tabschema = Username = NULL;
	Password = Tabcat = Tabtype = Srcdef = Qchar = Qrystr = Sep = NULL;
	Options = Quoted = Maxerr = Maxres = Memory = 0;
	Scrollable = Xsrc = false;
}  // end of JDBCDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from JDBC file.          */
/***********************************************************************/
bool JDBCDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
	Jpath = GetStringCatInfo(g, "Jpath", "");
	Driver = GetStringCatInfo(g, "Driver", NULL);
	Desc = Url = GetStringCatInfo(g, "Url", NULL);

	if (!Url && !Catfunc) {
		sprintf(g->Message, "Missing URL for JDBC table %s", Name);
		return true;
	} // endif Connect

	Tabname = GetStringCatInfo(g, "Name",
		(Catfunc & (FNC_TABLE | FNC_COL)) ? NULL : Name);
	Tabname = GetStringCatInfo(g, "Tabname", Tabname);
	Tabschema = GetStringCatInfo(g, "Dbname", NULL);
	Tabschema = GetStringCatInfo(g, "Schema", Tabschema);
	Tabcat = GetStringCatInfo(g, "Qualifier", NULL);
	Tabcat = GetStringCatInfo(g, "Catalog", Tabcat);
	Tabtype = GetStringCatInfo(g, "Tabtype", NULL);
	Username = GetStringCatInfo(g, "User", NULL);
	Password = GetStringCatInfo(g, "Password", NULL);

	if ((Srcdef = GetStringCatInfo(g, "Srcdef", NULL)))
		Read_Only = true;

	Qrystr = GetStringCatInfo(g, "Query_String", "?");
	Sep = GetStringCatInfo(g, "Separator", NULL);
	Xsrc = GetBoolCatInfo("Execsrc", FALSE);
	Maxerr = GetIntCatInfo("Maxerr", 0);
	Maxres = GetIntCatInfo("Maxres", 0);
	Quoted = GetIntCatInfo("Quoted", 0);
//Options = JDBConn::noJDBCDialog;
//Options = JDBConn::noJDBCDialog | JDBConn::useCursorLib;
//Cto= GetIntCatInfo("ConnectTimeout", DEFAULT_LOGIN_TIMEOUT);
//Qto= GetIntCatInfo("QueryTimeout", DEFAULT_QUERY_TIMEOUT);
	Scrollable = GetBoolCatInfo("Scrollable", false);
	Memory = GetIntCatInfo("Memory", 0);
	Pseudo = 2;      // FILID is Ok but not ROWID
	return false;
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB JDBCDEF::GetTable(PGLOBAL g, MODE m)
{
	PTDBASE tdbp = NULL;

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
TDBJDBC::TDBJDBC(PJDBCDEF tdp) : TDBASE(tdp)
{
	Jcp = NULL;
	Cnp = NULL;

	if (tdp) {
		Jpath = tdp->Jpath;
		Ops.Driver = tdp->Driver;
		Ops.Url = tdp->Url;
		TableName = tdp->Tabname;
		Schema = tdp->Tabschema;
		Ops.User = tdp->Username;
		Ops.Pwd = tdp->Password;
		Catalog = tdp->Tabcat;
		Srcdef = tdp->Srcdef;
		Qrystr = tdp->Qrystr;
		Sep = tdp->GetSep();
		Options = tdp->Options;
//	Ops.Cto = tdp->Cto;
//	Ops.Qto = tdp->Qto;
		Quoted = MY_MAX(0, tdp->GetQuoted());
		Rows = tdp->GetElemt();
		Memory = tdp->Memory;
		Ops.Scrollable = tdp->Scrollable;
	} else {
		Jpath = NULL;
		TableName = NULL;
		Schema = NULL;
		Ops.Driver = NULL;
		Ops.Url = NULL;
		Ops.User = NULL;
		Ops.Pwd = NULL;
		Catalog = NULL;
		Srcdef = NULL;
		Qrystr = NULL;
		Sep = 0;
		Options = 0;
//	Ops.Cto = DEFAULT_LOGIN_TIMEOUT;
//	Ops.Qto = DEFAULT_QUERY_TIMEOUT;
		Quoted = 0;
		Rows = 0;
		Memory = 0;
		Ops.Scrollable = false;
	} // endif tdp

	Quote = NULL;
	Query = NULL;
	Count = NULL;
//Where = NULL;
	MulConn = NULL;
	DBQ = NULL;
	Qrp = NULL;
	Fpos = 0;
	Curpos = 0;
	AftRows = 0;
	CurNum = 0;
	Rbuf = 0;
	BufSize = 0;
	Ncol = 0;
	Nparm = 0;
	Placed = false;
	Werr = false;
	Rerr = false;
	Ops.Fsize = Ops.CheckSize(Rows);
} // end of TDBJDBC standard constructor

TDBJDBC::TDBJDBC(PTDBJDBC tdbp) : TDBASE(tdbp)
{
	Jcp = tdbp->Jcp;            // is that right ?
	Cnp = tdbp->Cnp;
	Jpath = tdbp->Jpath;
	TableName = tdbp->TableName;
	Schema = tdbp->Schema;
	Ops = tdbp->Ops;
	Catalog = tdbp->Catalog;
	Srcdef = tdbp->Srcdef;
	Qrystr = tdbp->Qrystr;
	Memory = tdbp->Memory;
//Scrollable = tdbp->Scrollable;
	Quote = tdbp->Quote;
	Query = tdbp->Query;
	Count = tdbp->Count;
//Where = tdbp->Where;
	MulConn = tdbp->MulConn;
	DBQ = tdbp->DBQ;
	Options = tdbp->Options;
	Quoted = tdbp->Quoted;
	Rows = tdbp->Rows;
	Fpos = 0;
	Curpos = 0;
	AftRows = 0;
	CurNum = 0;
	Rbuf = 0;
	BufSize = tdbp->BufSize;
	Nparm = tdbp->Nparm;
	Qrp = tdbp->Qrp;
	Placed = false;
} // end of TDBJDBC copy constructor

// Method
PTDB TDBJDBC::CopyOne(PTABS t)
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
} // end of CopyOne

/***********************************************************************/
/*  Allocate JDBC column description block.                            */
/***********************************************************************/
PCOL TDBJDBC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
	return new(g)JDBCCOL(cdp, this, cprec, n);
} // end of MakeCol

/******************************************************************/
/*  Convert an UTF-8 string to latin characters.                  */
/******************************************************************/
int TDBJDBC::Decode(char *txt, char *buf, size_t n)
{
	uint dummy_errors;
	uint32 len= copy_and_convert(buf, n, &my_charset_latin1,
		txt, strlen(txt),
		&my_charset_utf8_general_ci,
		&dummy_errors);
	buf[len]= '\0';
	return 0;
} // end of Decode

/***********************************************************************/
/*  MakeSQL: make the SQL statement use with JDBC connection.          */
/*  TODO: when implementing EOM filtering, column only used in local   */
/*  filter should be removed from column list.                         */
/***********************************************************************/
bool TDBJDBC::MakeSQL(PGLOBAL g, bool cnt)
{
	char  *schmp = NULL, *catp = NULL, buf[NAM_LEN * 3];
	int    len;
	bool   oom = false, first = true;
	PTABLE tablep = To_Table;
	PCOL   colp;

	if (Srcdef) {
		Query = new(g)STRING(g, 0, Srcdef);
		return false;
	} // endif Srcdef

	// Allocate the string used to contain the Query
	Query = new(g)STRING(g, 1023, "SELECT ");

	if (!cnt) {
		if (Columns) {
			// Normal SQL statement to retrieve results
			for (colp = Columns; colp; colp = colp->GetNext())
				if (!colp->IsSpecial()) {
					if (!first)
						oom |= Query->Append(", ");
					else
						first = false;

					// Column name can be encoded in UTF-8
					Decode(colp->GetName(), buf, sizeof(buf));

					if (Quote) {
						// Put column name between identifier quotes in case in contains blanks
						oom |= Query->Append(Quote);
						oom |= Query->Append(buf);
						oom |= Query->Append(Quote);
					} else
						oom |= Query->Append(buf);

					((PJDBCCOL)colp)->Rank = ++Ncol;
				} // endif colp

		} else
			// !Columns can occur for queries such that sql count(*) from...
			// for which we will count the rows from sql * from...
			oom |= Query->Append('*');

	} else
		// SQL statement used to retrieve the size of the result
		oom |= Query->Append("count(*)");

	oom |= Query->Append(" FROM ");

	if (Catalog && *Catalog)
		catp = Catalog;

	if (tablep->GetSchema())
		schmp = (char*)tablep->GetSchema();
	else if (Schema && *Schema)
		schmp = Schema;

	if (catp) {
		oom |= Query->Append(catp);

		if (schmp) {
			oom |= Query->Append('.');
			oom |= Query->Append(schmp);
		} // endif schmp

		oom |= Query->Append('.');
	} else if (schmp) {
		oom |= Query->Append(schmp);
		oom |= Query->Append('.');
	} // endif schmp

	// Table name can be encoded in UTF-8
	Decode(TableName, buf, sizeof(buf));

	if (Quote) {
		// Put table name between identifier quotes in case in contains blanks
		oom |= Query->Append(Quote);
		oom |= Query->Append(buf);
		oom |= Query->Append(Quote);
	} else
		oom |= Query->Append(buf);

	len = Query->GetLength();

	if (To_CondFil) {
		if (Mode == MODE_READ) {
			oom |= Query->Append(" WHERE ");
			oom |= Query->Append(To_CondFil->Body);
			len = Query->GetLength() + 1;
		} else
			len += (strlen(To_CondFil->Body) + 256);

	} else
		len += ((Mode == MODE_READX) ? 256 : 1);

	if (oom || Query->Resize(len)) {
		strcpy(g->Message, "MakeSQL: Out of memory");
		return true;
	} // endif oom

	if (trace)
		htrc("Query=%s\n", Query->GetStr());

	return false;
} // end of MakeSQL

/***********************************************************************/
/*  MakeInsert: make the Insert statement used with JDBC connection.   */
/***********************************************************************/
bool TDBJDBC::MakeInsert(PGLOBAL g)
{
	char  *schmp = NULL, *catp = NULL, buf[NAM_LEN * 3];
	int    len = 0;
	uint   pos;
	bool   b = false, oom = false;
	PTABLE tablep = To_Table;
	PCOL   colp;

	for (colp = Columns; colp; colp = colp->GetNext())
		if (colp->IsSpecial()) {
			strcpy(g->Message, "No JDBC special columns");
			return true;
		} else {
			// Column name can be encoded in UTF-8
			Decode(colp->GetName(), buf, sizeof(buf));
			len += (strlen(buf) + 6);	 // comma + quotes + valist
			((PJDBCCOL)colp)->Rank = ++Nparm;
		} // endif colp

	// Below 32 is enough to contain the fixed part of the query
	if (Catalog && *Catalog)
		catp = Catalog;

	if (catp)
		len += strlen(catp) + 1;

	if (tablep->GetSchema())
		schmp = (char*)tablep->GetSchema();
	else if (Schema && *Schema)
		schmp = Schema;

	if (schmp)
		len += strlen(schmp) + 1;

	// Table name can be encoded in UTF-8
	Decode(TableName, buf, sizeof(buf));
	len += (strlen(buf) + 32);
	Query = new(g)STRING(g, len, "INSERT INTO ");

	if (catp) {
		oom |= Query->Append(catp);

		if (schmp) {
			oom |= Query->Append('.');
			oom |= Query->Append(schmp);
		} // endif schmp

		oom |= Query->Append('.');
	} else if (schmp) {
		oom |= Query->Append(schmp);
		oom |= Query->Append('.');
	} // endif schmp

	if (Quote) {
		// Put table name between identifier quotes in case in contains blanks
		oom |= Query->Append(Quote);
		oom |= Query->Append(buf);
		oom |= Query->Append(Quote);
	} else
		oom |= Query->Append(buf);

	oom |= Query->Append('(');

	for (colp = Columns; colp; colp = colp->GetNext()) {
		if (b)
			oom |= Query->Append(", ");
		else
			b = true;

		// Column name can be in UTF-8 encoding
		Decode(colp->GetName(), buf, sizeof(buf));

		if (Quote) {
			// Put column name between identifier quotes in case in contains blanks
			oom |= Query->Append(Quote);
			oom |= Query->Append(buf);
			oom |= Query->Append(Quote);
		} else
			oom |= Query->Append(buf);

	} // endfor colp

	if ((oom |= Query->Append(") VALUES ("))) {
		strcpy(g->Message, "MakeInsert: Out of memory");
		return true;
	} else // in case prepared statement fails
		pos = Query->GetLength();

	// Make prepared statement
	for (int i = 0; i < Nparm; i++)
		oom |= Query->Append("?,");

	if (oom) {
		strcpy(g->Message, "MakeInsert: Out of memory");
		return true;
	} else
		Query->RepLast(')');

	// Now see if we can use prepared statement
	if (Jcp->PrepareSQL(Query->GetStr()))
		Query->Truncate(pos);     // Restore query to not prepared
	else
		Prepared = true;

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
/*  MakeCommand: make the Update or Delete statement to send to the    */
/*  MySQL server. Limited to remote values and filtering.              */
/***********************************************************************/
bool TDBJDBC::MakeCommand(PGLOBAL g)
{
	char *p, *stmt, name[68], *body = NULL, *qc = Jcp->GetQuoteChar();
	char *qrystr = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 1);
	bool  qtd = Quoted > 0;
	int   i = 0, k = 0;

	// Make a lower case copy of the originale query and change
	// back ticks to the data source identifier quoting character
	do {
		qrystr[i] = (Qrystr[i] == '`') ? *qc : tolower(Qrystr[i]);
	} while (Qrystr[i++]);

	if (To_CondFil && (p = strstr(qrystr, " where "))) {
		p[7] = 0;           // Remove where clause
		Qrystr[(p - qrystr) + 7] = 0;
		body = To_CondFil->Body;
		stmt = (char*)PlugSubAlloc(g, NULL, strlen(qrystr)
			+ strlen(body) + 64);
	} else
		stmt = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 64);

	// Check whether the table name is equal to a keyword
	// If so, it must be quoted in the original query
	strlwr(strcat(strcat(strcpy(name, " "), Name), " "));

	if (!strstr(" update delete low_priority ignore quick from ", name))
		strlwr(strcpy(name, Name));     // Not a keyword
	else
		strlwr(strcat(strcat(strcpy(name, qc), Name), qc));

	if ((p = strstr(qrystr, name))) {
		for (i = 0; i < p - qrystr; i++)
			stmt[i] = (Qrystr[i] == '`') ? *qc : Qrystr[i];

		stmt[i] = 0;
		k = i + (int)strlen(Name);

		if (qtd && *(p-1) == ' ')
			strcat(strcat(strcat(stmt, qc), TableName), qc);
		else
			strcat(stmt, TableName);

		i = (int)strlen(stmt);

		do {
			stmt[i++] = (Qrystr[k] == '`') ? *qc : Qrystr[k];
		} while (Qrystr[k++]);

		if (body)
			strcat(stmt, body);

	} else {
		sprintf(g->Message, "Cannot use this %s command",
			(Mode == MODE_UPDATE) ? "UPDATE" : "DELETE");
		return NULL;
	} // endif p

	Query = new(g)STRING(g, 0, stmt);
	return (!Query->GetSize());
} // end of MakeCommand

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

		if (jcp->Open(Jpath, &Ops) == RC_FX)
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
/*  JDBC GetMaxSize: returns table size estimate in number of lines.   */
/***********************************************************************/
int TDBJDBC::GetMaxSize(PGLOBAL g)
{
	if (MaxSize < 0) {
		if (Mode == MODE_DELETE)
			// Return 0 in mode DELETE in case of delete all.
			MaxSize = 0;
		else if (!Cardinality(NULL))
			MaxSize = 10;   // To make MySQL happy
		else if ((MaxSize = Cardinality(g)) < 0)
			MaxSize = 12;   // So we can see an error occured

	} // endif MaxSize

	return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  Return max size value.                                             */
/***********************************************************************/
int TDBJDBC::GetProgMax(PGLOBAL g)
{
	return GetMaxSize(g);
} // end of GetProgMax

/***********************************************************************/
/*  JDBC Access Method opening routine.                                */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBJDBC::OpenDB(PGLOBAL g)
{
	bool rc = true;

	if (trace)
		htrc("JDBC OpenDB: tdbp=%p tdb=R%d use=%d mode=%d\n",
		     this, Tdb_No, Use, Mode);

	if (Use == USE_OPEN) {
		/*******************************************************************/
		/*  Table already open, just replace it at its beginning.          */
		/*******************************************************************/
		if (Memory == 1) {
			if ((Qrp = Jcp->AllocateResult(g)))
				Memory = 2;            // Must be filled
			else
				Memory = 0;            // Allocation failed, don't use it

		} else if (Memory == 2)
			Memory = 3;              // Ok to use memory result

		if (Memory < 3) {
			// Method will depend on cursor type
			if ((Rbuf = Jcp->Rewind(Query->GetStr())) < 0)
				if (Mode != MODE_READX) {
					Jcp->Close();
				  return true;
			  } else
				  Rbuf = 0;

		} else
			Rbuf = Qrp->Nblin;

		CurNum = 0;
		Fpos = 0;
		Curpos = 1;
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
		Jcp = new(g)JDBConn(g, this);
	else if (Jcp->IsOpen())
		Jcp->Close();

	if (Jcp->Open(Jpath, &Ops) == RC_FX)
		return true;
	else if (Quoted)
		Quote = Jcp->GetQuoteChar();

	Use = USE_OPEN;       // Do it now in case we are recursively called

	/*********************************************************************/
	/*  Make the command and allocate whatever is used for getting results.                   */
	/*********************************************************************/
	if (Mode == MODE_READ || Mode == MODE_READX) {
		if (Memory > 1 && !Srcdef) {
			int n;

			if (!MakeSQL(g, true)) {
				// Allocate a Count(*) column
				Cnp = new(g)JDBCCOL;
				Cnp->InitValue(g);

				if ((n = Jcp->GetResultSize(Query->GetStr(), Cnp)) < 0) {
					sprintf(g->Message, "Cannot get result size rc=%d", n);
					return true;
				} else if (n) {
					Jcp->m_Rows = n;

					if ((Qrp = Jcp->AllocateResult(g)))
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

/***********************************************************************/
/*  GetRecpos: return the position of last read record.                */
/***********************************************************************/
int TDBJDBC::GetRecpos(void)
{
	return Fpos;
} // end of GetRecpos

/***********************************************************************/
/*  SetRecpos: set the position of next read record.                   */
/***********************************************************************/
bool TDBJDBC::SetRecpos(PGLOBAL g, int recpos)
{
	if (Jcp->m_Full) {
		Fpos = 0;
//	CurNum = 0;
		CurNum = 1;
	} else if (Memory == 3) {
//	Fpos = recpos;
//	CurNum = -1;
		Fpos = 0;
		CurNum = recpos;
	} else if (Ops.Scrollable) {
		// Is new position in the current row set?
//	if (recpos >= Curpos && recpos < Curpos + Rbuf) {
//		CurNum = recpos - Curpos;
//		Fpos = 0;
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

				if ((To_CondFil = hc->CheckCond(g, To_CondFil, To_CondFil->Cond)))
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

	if (trace)
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

	if (trace > 1)
		htrc("JDBC ReadDB: R%d Mode=%d key=%p link=%p Kindex=%p\n",
		GetTdb_No(), Mode, To_Key_Col, To_Link, To_Kindex);

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

	if (To_Kindex) {
		// Direct access of JDBC tables is not implemented
		strcpy(g->Message, "No JDBC direct access");
		return RC_FX;
	} // endif To_Kindex

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

	if (trace > 1)
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
	bool oom = false;

	// Make the Insert command value list
	for (PCOL colp = Columns; colp; colp = colp->GetNext()) {
		if (!colp->GetValue()->IsNull()) {
			char *s = colp->GetValue()->GetCharString(buf);

			if (colp->GetResultType() == TYPE_STRING)
				oom |= Query->Append_quoted(s);
			else if (colp->GetResultType() == TYPE_DATE) {
				DTVAL *dtv = (DTVAL*)colp->GetValue();

				if (dtv->IsFormatted())
					oom |= Query->Append_quoted(s);
				else
					oom |= Query->Append(s);

			} else
				oom |= Query->Append(s);

		} else
			oom |= Query->Append("NULL");

		oom |= Query->Append(',');
	} // endfor colp

	if (unlikely(oom)) {
		strcpy(g->Message, "WriteDB: Out of memory");
		return RC_FX;
	} // endif oom

	Query->RepLast(')');
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

			if (trace)
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
	//if (To_Kindex) {
	//  To_Kindex->Close();
	//  To_Kindex = NULL;
	//  } // endif

	if (Jcp)
		Jcp->Close();

	if (trace)
		htrc("JDBC CloseDB: closing %s\n", Name);

	if (!Werr && 
		(Mode == MODE_INSERT || Mode == MODE_UPDATE || Mode == MODE_DELETE)) {
		sprintf(g->Message, "%s: %d affected rows", TableName, AftRows);

		if (trace)
			htrc("%s\n", g->Message);

		PushWarning(g, this, 0);    // 0 means a Note
	}	// endif Mode

	Prepared = false;
} // end of CloseDB

/* --------------------------- JDBCCOL ------------------------------- */

/***********************************************************************/
/*  JDBCCOL public constructor.                                        */
/***********************************************************************/
JDBCCOL::JDBCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
	: COLBLK(cdp, tdbp, i)
{
	if (cprec) {
		Next = cprec->GetNext();
		cprec->SetNext(this);
	} else {
		Next = tdbp->GetColumns();
		tdbp->SetColumns(this);
	} // endif cprec

	// Set additional JDBC access method information for column.
	Crp = NULL;
	//Long = cdp->GetLong();
	Long = Precision;
	//strcpy(F_Date, cdp->F_Date);
	To_Val = NULL;
//Slen = 0;
//StrLen = &Slen;
//Sqlbuf = NULL;
	Bufp = NULL;
	Blkp = NULL;
	Rank = 0;           // Not known yet

	if (trace)
		htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

} // end of JDBCCOL constructor

/***********************************************************************/
/*  JDBCCOL private constructor.                                       */
/***********************************************************************/
JDBCCOL::JDBCCOL(void) : COLBLK()
{
	Crp = NULL;
	Buf_Type = TYPE_INT;     // This is a count(*) column
	// Set additional Dos access method information for column.
	Long = sizeof(int);
	To_Val = NULL;
//Slen = 0;
//StrLen = &Slen;
//Sqlbuf = NULL;
	Bufp = NULL;
	Blkp = NULL;
	Rank = 1;
} // end of JDBCCOL constructor

/***********************************************************************/
/*  JDBCCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
JDBCCOL::JDBCCOL(JDBCCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
{
	Crp = col1->Crp;
	Long = col1->Long;
	//strcpy(F_Date, col1->F_Date);
	To_Val = col1->To_Val;
//Slen = col1->Slen;
//StrLen = col1->StrLen;
//Sqlbuf = col1->Sqlbuf;
	Bufp = col1->Bufp;
	Blkp = col1->Blkp;
	Rank = col1->Rank;
} // end of JDBCCOL copy constructor

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool JDBCCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
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
/*  ReadColumn: when SQLFetch is used there is nothing to do as the    */
/*  column buffer was bind to the record set. This is also the case    */
/*  when calculating MaxSize (Bufp is NULL even when Rows is not).     */
/***********************************************************************/
void JDBCCOL::ReadColumn(PGLOBAL g)
{
	PTDBJDBC tdbp = (PTDBJDBC)To_Tdb;
	int i = tdbp->Fpos - 1, n = tdbp->CurNum;

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

#if 0
/***********************************************************************/
/*  AllocateBuffers: allocate the extended buffer for SQLExtendedFetch */
/*  or Fetch.  Note: we use Long+1 here because JDBC must have space   */
/*  for the ending null character.                                     */
/***********************************************************************/
void JDBCCOL::AllocateBuffers(PGLOBAL g, int rows)
{
	if (Buf_Type == TYPE_DATE)
		Sqlbuf = (TIMESTAMP_STRUCT*)PlugSubAlloc(g, NULL,
		sizeof(TIMESTAMP_STRUCT));

	if (!rows)
		return;

	if (Buf_Type == TYPE_DATE)
		Bufp = PlugSubAlloc(g, NULL, rows * sizeof(TIMESTAMP_STRUCT));
	else {
		Blkp = AllocValBlock(g, NULL, Buf_Type, rows, GetBuflen(),
			GetScale(), true, false, false);
		Bufp = Blkp->GetValPointer();
	} // endelse

	if (rows > 1)
		StrLen = (SQLLEN *)PlugSubAlloc(g, NULL, rows * sizeof(SQLLEN));

} // end of AllocateBuffers

/***********************************************************************/
/*  Returns the buffer to use for Fetch or Extended Fetch.             */
/***********************************************************************/
void *JDBCCOL::GetBuffer(DWORD rows)
{
	if (rows && To_Tdb) {
		assert(rows == (DWORD)((TDBJDBC*)To_Tdb)->Rows);
		return Bufp;
	} else
		return (Buf_Type == TYPE_DATE) ? Sqlbuf : Value->GetTo_Val();

} // end of GetBuffer

/***********************************************************************/
/*  Returns the buffer length to use for Fetch or Extended Fetch.      */
/***********************************************************************/
SWORD JDBCCOL::GetBuflen(void)
{
	SWORD flen;

	switch (Buf_Type) {
	case TYPE_DATE:
		flen = (SWORD)sizeof(TIMESTAMP_STRUCT);
		break;
	case TYPE_STRING:
	case TYPE_DECIM:
		flen = (SWORD)Value->GetClen() + 1;
		break;
	default:
		flen = (SWORD)Value->GetClen();
	} // endswitch Buf_Type

	return flen;
} // end of GetBuflen
#endif // 0

/***********************************************************************/
/*  WriteColumn: make sure the bind buffer is updated.                 */
/***********************************************************************/
void JDBCCOL::WriteColumn(PGLOBAL g)
{
	/*********************************************************************/
	/*  Do convert the column value if necessary.                        */
	/*********************************************************************/
	if (Value != To_Val)
		Value->SetValue_pval(To_Val, FALSE);   // Convert the inserted value

#if 0
	if (Buf_Type == TYPE_DATE) {
		struct tm tm, *dbtime = ((DTVAL*)Value)->GetGmTime(&tm);

		Sqlbuf->second = dbtime->tm_sec;
		Sqlbuf->minute = dbtime->tm_min;
		Sqlbuf->hour   = dbtime->tm_hour;
		Sqlbuf->day    = dbtime->tm_mday;
		Sqlbuf->month  = dbtime->tm_mon + 1;
		Sqlbuf->year   = dbtime->tm_year + 1900;
		Sqlbuf->fraction = 0;
	} else if (Buf_Type == TYPE_DECIM) {
		// Some data sources require local decimal separator
		char *p, sep = ((PTDBJDBC)To_Tdb)->Sep;

		if (sep && (p = strchr(Value->GetCharValue(), '.')))
			*p = sep;

	} // endif Buf_Type

	if (Nullable)
		*StrLen = (Value->IsNull()) ? SQL_NULL_DATA :
		(IsTypeChar(Buf_Type)) ? SQL_NTS : 0;
#endif // 0
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

#if 0
/***********************************************************************/
/*  JDBC Bind Parameter function.                                      */
/***********************************************************************/
bool TDBXJDC::BindParameters(PGLOBAL g)
{
	PJDBCCOL colp;

	for (colp = (PJDBCCOL)Columns; colp; colp = (PJDBCCOL)colp->Next) {
		colp->AllocateBuffers(g, 0);

		if (Jcp->BindParam(colp))
			return true;

	} // endfor colp

	return false;
} // end of BindParameters
#endif // 0

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
	bool rc = false;

	if (trace)
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
		Jcp = new(g) JDBConn(g, this);
	} else if (Jcp->IsOpen())
		Jcp->Close();

	if (Jcp->Open(Jpath, &Ops) == RC_FX)
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
		Jcp->Close();
		return true;
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

		if ((rc = Jcp->ExecSQLcommand(Query->GetStr())) == RC_FX)
			Nerr++;

		if (rc == RC_NF)
			AftRows = Jcp->m_Aff;
		else if (rc == RC_OK)
			AftRows = Jcp->m_Ncol;

		Fpos++;                // Used for progress info
		Cmdlist = (Nerr > Mxr) ? NULL : Cmdlist->Next;
		return RC_OK;
	} else
		return RC_EF;

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
JSRCCOL::JSRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
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
	return JDBCDrivers(g, Jpath, Maxres, false);
} // end of GetResult

/* ---------------------------TDBJTB class --------------------------- */

/***********************************************************************/
/*  TDBJTB class constructor.                                          */
/***********************************************************************/
TDBJTB::TDBJTB(PJDBCDEF tdp) : TDBJDRV(tdp)
{
	Jpath = tdp->Jpath;
	Schema = tdp->Tabschema;
	Tab = tdp->Tabname;
	Tabtype = tdp->Tabtype;
	Ops.Driver = tdp->Driver;
	Ops.Url = tdp->Url;
	Ops.User = tdp->Username;
	Ops.Pwd = tdp->Password;
} // end of TDBJTB constructor

/***********************************************************************/
/*  GetResult: Get the list of JDBC tables.                            */
/***********************************************************************/
PQRYRES TDBJTB::GetResult(PGLOBAL g)
{
	return JDBCTables(g, Jpath, Schema, Tab, Tabtype, Maxres, false, &Ops);
} // end of GetResult

/* --------------------------TDBJDBCL class -------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of JDBC table columns.                     */
/***********************************************************************/
PQRYRES TDBJDBCL::GetResult(PGLOBAL g)
{
	return JDBCColumns(g, Jpath, Schema, Tab, NULL, Maxres, false, &Ops);
} // end of GetResult

#if 0
/* ---------------------------TDBJSRC class -------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of JDBC data sources.                      */
/***********************************************************************/
PQRYRES TDBJSRC::GetResult(PGLOBAL g)
{
	return JDBCDataSources(g, Maxres, false);
} // end of GetResult

/* ------------------------ End of TabJDBC --------------------------- */
#endif // 0
