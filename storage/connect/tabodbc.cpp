/************* Tabodbc C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: TABODBC                                               */
/* -------------                                                       */
/*  Version 3.2                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2018    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the TABODBC class DB execution routines.          */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*    TABODBC.CPP    - Source code                                     */
/*    PLGDBSEM.H     - DB application declaration file                 */
/*    TABODBC.H      - TABODBC classes declaration file                */
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
/*  tabodbc.h   is header containing the TABODBC class declarations.   */
/*  odbconn.h   is header containing ODBC connection declarations.     */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "mycat.h"
#include "xtable.h"
#include "tabext.h"
#include "odbccat.h"
#include "tabodbc.h"
#include "tabmul.h"
//#include "reldef.h"
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

/* -------------------------- Class ODBCDEF -------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
ODBCDEF::ODBCDEF(void)
{
  Connect = NULL;
  Catver = 0;
  UseCnc = false;
}  // end of ODBCDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool ODBCDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
  Desc = Connect = GetStringCatInfo(g, "Connect", NULL);

  if (!Connect && !Catfunc) {
    sprintf(g->Message, "Missing connection for ODBC table %s", Name);
    return true;
  } // endif Connect

	if (EXTDEF::DefineAM(g, am, poff))
		return true;

  Catver = GetIntCatInfo("Catver", 2);
  Options = ODBConn::noOdbcDialog;
//Options = ODBConn::noOdbcDialog | ODBConn::useCursorLib;
  Cto= GetIntCatInfo("ConnectTimeout", DEFAULT_LOGIN_TIMEOUT);
  Qto= GetIntCatInfo("QueryTimeout", DEFAULT_QUERY_TIMEOUT);
	UseCnc = GetBoolCatInfo("UseDSN", false);
  return false;
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB ODBCDEF::GetTable(PGLOBAL g, MODE m)
{
  PTDB tdbp = NULL;

  /*********************************************************************/
  /*  Allocate a TDB of the proper type.                               */
  /*  Column blocks will be allocated only when needed.                */
  /*********************************************************************/
  if (Xsrc)
    tdbp = new(g) TDBXDBC(this);
  else switch (Catfunc) {
    case FNC_COL:
      tdbp = new(g) TDBOCL(this);
      break;
    case FNC_TABLE:
      tdbp = new(g) TDBOTB(this);
      break;
    case FNC_DSN:
      tdbp = new(g) TDBSRC(this);
      break;
    case FNC_DRIVER:
      tdbp = new(g) TDBDRV(this);
      break;
    default:
      tdbp = new(g) TDBODBC(this);
  
      if (Multiple == 1)
        tdbp = new(g) TDBMUL(tdbp);
      else if (Multiple == 2)
        strcpy(g->Message, MSG(NO_ODBC_MUL));
  } // endswitch Catfunc

  return tdbp;
} // end of GetTable

/* -------------------------- Class TDBODBC -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBODBC class.                               */
/***********************************************************************/
TDBODBC::TDBODBC(PODEF tdp) : TDBEXT(tdp)
{
  Ocp = NULL;
  Cnp = NULL;

  if (tdp) {
    Connect = tdp->Connect;
    Ops.User = tdp->Username;
    Ops.Pwd = tdp->Password;
    Ops.Cto = tdp->Cto;
    Ops.Qto = tdp->Qto;
    Catver = tdp->Catver;
    Ops.UseCnc = tdp->UseCnc;
  } else {
    Connect = NULL;
    Ops.User = NULL;
    Ops.Pwd = NULL;
    Ops.Cto = DEFAULT_LOGIN_TIMEOUT;
    Ops.Qto = DEFAULT_QUERY_TIMEOUT;
    Catver = 0;
    Ops.UseCnc = false;
  } // endif tdp

} // end of TDBODBC standard constructor

TDBODBC::TDBODBC(PTDBODBC tdbp) : TDBEXT(tdbp)
{
  Ocp = tdbp->Ocp;            // is that right ?
  Cnp = tdbp->Cnp;
  Connect = tdbp->Connect;
  Ops = tdbp->Ops;
} // end of TDBODBC copy constructor

// Method
PTDB TDBODBC::Clone(PTABS t)
{
  PTDB     tp;
  PODBCCOL cp1, cp2;
  PGLOBAL  g = t->G;        // Is this really useful ???

  tp = new(g) TDBODBC(this);

  for (cp1 = (PODBCCOL)Columns; cp1; cp1 = (PODBCCOL)cp1->GetNext()) {
    cp2 = new(g) ODBCCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
  } // endfor cp1

  return tp;
} // end of CopyOne

/***********************************************************************/
/*  Allocate ODBC column description block.                            */
/***********************************************************************/
PCOL TDBODBC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
  return new(g) ODBCCOL(cdp, this, cprec, n);
} // end of MakeCol

/***********************************************************************/
/*  Extract the filename from connect string and return it.            */
/*  This used for Multiple(1) tables. Also prepare a connect string    */
/*  with a place holder to be used by SetFile.                         */
/***********************************************************************/
PCSZ TDBODBC::GetFile(PGLOBAL g)
{
  if (Connect) {
    char  *p1, *p2;
		int    i;
    size_t n;

		if (!(p1 = strstr(Connect, "DBQ="))) {
			char *p, *lc = strlwr(PlugDup(g, Connect));

			if ((p = strstr(lc, "database=")))
				p1 = Connect + (p - lc);

			i = 9;
		} else
			i = 4;

    if (p1) {
      p1 += i;                        // Beginning of file name
      p2 = strchr(p1, ';');           // End of file path/name

      // Make the File path/name from the connect string
      n = (p2) ? p2 - p1 : strlen(p1);
      DBQ = (PSZ)PlugSubAlloc(g, NULL, n + 1);
      memcpy(DBQ, p1, n);
      DBQ[n] = '\0';

      // Make the Format used to re-generate Connect (3 = "%s" + 1)
      MulConn = (char*)PlugSubAlloc(g, NULL, strlen(Connect) - n + 3);
      memcpy(MulConn, Connect, p1 - Connect);
      MulConn[p1 - Connect] = '\0';
      strcat(strcat(MulConn, "%s"), (p2) ? p2 : ";");
    } // endif p1

  } // endif Connect

  return (DBQ) ? DBQ : (PSZ)"???";
} // end of GetFile

/***********************************************************************/
/*  Set DBQ and get the new file name into the connect string.         */
/***********************************************************************/
void TDBODBC::SetFile(PGLOBAL g, PCSZ fn)
{
  if (MulConn) {
    int n = strlen(MulConn) + strlen(fn) - 1;

    if (n > BufSize) {
      // Allocate a buffer larger than needed so the chance
      // of having to reallocate it is reduced.
      BufSize = n + 6;
      Connect = (char*)PlugSubAlloc(g, NULL, BufSize);
    } // endif n

    // Make the complete connect string
    sprintf(Connect, MulConn, fn);
  } // endif MultConn

  DBQ = PlugDup(g, fn);
} // end of SetFile

/***********************************************************************/
/*  MakeInsert: make the Insert statement used with ODBC connection.   */
/***********************************************************************/
bool TDBODBC::MakeInsert(PGLOBAL g)
{
	PCSZ   schmp = NULL;
	char  *catp = NULL, buf[NAM_LEN * 3];
	int    len = 0;
	bool   oom, b = false;
	PCOL   colp;

  for (colp = Columns; colp; colp = colp->GetNext())
    if (colp->IsSpecial()) {
      strcpy(g->Message, MSG(NO_ODBC_SPECOL));
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

	if (Schema && *Schema)
		schmp = Schema;

	if (schmp)
		len += strlen(schmp) + 1;

	// Table name can be encoded in UTF-8
	Decode(TableName, buf, sizeof(buf));
	len += (strlen(buf) + 32);
	Query = new(g) STRING(g, len, "INSERT INTO ");

	if (catp) {
		Query->Append(catp);

		if (schmp) {
			Query->Append('.');
			Query->Append(schmp);
		} // endif schmp

		Query->Append('.');
	}	else if (schmp) {
		Query->Append(schmp);
		Query->Append('.');
	} // endif schmp

	if (Quote) {
		// Put table name between identifier quotes in case in contains blanks
		Query->Append(Quote);
		Query->Append(buf);
		Query->Append(Quote);
	}	else
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
		}	else
			Query->Append(buf);

	} // endfor colp

	Query->Append(") VALUES (");

	for (int i = 0; i < Nparm; i++)
		Query->Append("?,");

	if ((oom = Query->IsTruncated()))
		strcpy(g->Message, "MakeInsert: Out of memory");
	else
		Query->RepLast(')');

  return oom;
} // end of MakeInsert

/***********************************************************************/
/*  ODBC Bind Parameter function.                                      */
/***********************************************************************/
bool TDBODBC::BindParameters(PGLOBAL g)
{
	PODBCCOL colp;

	for (colp = (PODBCCOL)Columns; colp; colp = (PODBCCOL)colp->Next) {
		colp->AllocateBuffers(g, 0);

		if (Ocp->BindParam(colp))
			return true;

	} // endfor colp

	return false;
} // end of BindParameters

#if 0
/***********************************************************************/
/*  MakeUpdate: make the SQL statement to send to ODBC connection.     */
/***********************************************************************/
char *TDBODBC::MakeUpdate(PGLOBAL g)
{
  char *qc, *stmt = NULL, cmd[8], tab[96], end[1024];

  stmt = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 64);
  memset(end, 0, sizeof(end));

  if (sscanf(Qrystr, "%s `%[^`]`%1023c", cmd, tab, end) > 2 ||
      sscanf(Qrystr, "%s \"%[^\"]\"%1023c", cmd, tab, end) > 2)
    qc = Ocp->GetQuoteChar();
  else if (sscanf(Qrystr, "%s %s%1023c", cmd, tab, end) > 2)
    qc = (Quoted) ? Quote : "";
  else {
    strcpy(g->Message, "Cannot use this UPDATE command");
    return NULL;
  } // endif sscanf

  assert(!stricmp(cmd, "update"));
  strcat(strcat(strcat(strcpy(stmt, "UPDATE "), qc), TableName), qc);

  for (int i = 0; end[i]; i++)
    if (end[i] == '`')
      end[i] = *qc;

  strcat(stmt, end);
  return stmt;
} // end of MakeUpdate

/***********************************************************************/
/*  MakeDelete: make the SQL statement to send to ODBC connection.     */
/***********************************************************************/
char *TDBODBC::MakeDelete(PGLOBAL g)
{
	char *qc, *stmt = NULL, cmd[8], from[8], tab[96], end[512];

	stmt = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 64);
	memset(end, 0, sizeof(end));

	if (sscanf(Qrystr, "%s %s `%[^`]`%511c", cmd, from, tab, end) > 2 ||
		  sscanf(Qrystr, "%s %s \"%[^\"]\"%511c", cmd, from, tab, end) > 2)
		qc = Ocp->GetQuoteChar();
	else if (sscanf(Qrystr, "%s %s %s%511c", cmd, from, tab, end) > 2)
		qc = (Quoted) ? Quote : "";
	else {
		strcpy(g->Message, "Cannot use this DELETE command");
		return NULL;
	} // endif sscanf

	assert(!stricmp(cmd, "delete") && !stricmp(from, "from"));
	strcat(strcat(strcat(strcpy(stmt, "DELETE FROM "), qc), TableName), qc);

	if (*end) {
		for (int i = 0; end[i]; i++)
			if (end[i] == '`')
				end[i] = *qc;

		strcat(stmt, end);
	} // endif end

	return stmt;
} // end of MakeDelete
#endif // 0

/***********************************************************************/
/*  ResetSize: call by TDBMUL when calculating size estimate.          */
/***********************************************************************/
void TDBODBC::ResetSize(void)
{
  MaxSize = -1;

  if (Ocp && Ocp->IsOpen())
    Ocp->Close();

} // end of ResetSize

/***********************************************************************/
/*  ODBC Cardinality: returns table size in number of rows.            */
/***********************************************************************/
int TDBODBC::Cardinality(PGLOBAL g)
{
  if (!g)
    return (Mode == MODE_ANY && !Srcdef) ? 1 : 0;

  if (Cardinal < 0 && Mode == MODE_ANY && !Srcdef && ExactInfo()) {
    // Info command, we must return the exact table row number
    char     qry[96], tbn[64];
    ODBConn *ocp = new(g) ODBConn(g, this);

    if (ocp->Open(Connect, &Ops, Options) < 1)
      return -1;

    // Table name can be encoded in UTF-8
    Decode(TableName, tbn, sizeof(tbn));
    strcpy(qry, "SELECT COUNT(*) FROM ");

    if (Quote)
      strcat(strcat(strcat(qry, Quote), tbn), Quote);
    else
      strcat(qry, tbn);

    // Allocate a Count(*) column (must not use the default constructor)
    Cnp = new(g) ODBCCOL;
    Cnp->InitValue(g);

    if ((Cardinal = ocp->GetResultSize(qry, Cnp)) < 0)
      return -3;

    ocp->Close();
  } else
    Cardinal = 10;    // To make MySQL happy

  return Cardinal;
} // end of Cardinality

/***********************************************************************/
/*  ODBC Access Method opening routine.                                */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBODBC::OpenDB(PGLOBAL g)
{
  bool rc = true;

  if (trace(1))
    htrc("ODBC OpenDB: tdbp=%p tdb=R%d use=%dmode=%d\n",
            this, Tdb_No, Use, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
    if (Memory == 1) {
      if ((Qrp = Ocp->AllocateResult(g)))
        Memory = 2;            // Must be filled
      else
        Memory = 0;            // Allocation failed, don't use it

    } else if (Memory == 2)
      Memory = 3;              // Ok to use memory result

    if (Memory < 3) {
      // Method will depend on cursor type
      if ((Rbuf = Ocp->Rewind(Query->GetStr(), (PODBCCOL)Columns)) < 0) {
        if (Mode != MODE_READX) {
          Ocp->Close();
          return true;
        } else {
          Rbuf = 0;
        }
      }
    } else {
      Rbuf = Qrp->Nblin;
    }

    CurNum = 0;
    Fpos = 0;
    Curpos = 1;
    return false;
  } // endif use

  /*********************************************************************/
  /*  Open an ODBC connection for this table.                          */
  /*  Note: this may not be the proper way to do. Perhaps it is better */
  /*  to test whether a connection is already open for this datasource */
  /*  and if so to allocate just a new result set. But this only for   */
  /*  drivers allowing concurency in getting results ???               */
  /*********************************************************************/
  if (!Ocp)
    Ocp = new(g) ODBConn(g, this);
  else if (Ocp->IsOpen())
    Ocp->Close();

  if (Ocp->Open(Connect, &Ops, Options) < 1)
    return true;
  else if (Quoted)
    Quote = Ocp->GetQuoteChar();

  Use = USE_OPEN;       // Do it now in case we are recursively called

  /*********************************************************************/
  /* Make the command and allocate whatever is used for getting results*/
  /*********************************************************************/
  if (Mode == MODE_READ || Mode == MODE_READX) {
    if (Memory > 1 && !Srcdef) {
      int n;

      if (!MakeSQL(g, true)) {
        // Allocate a Count(*) column
        Cnp = new(g) ODBCCOL;
        Cnp->InitValue(g);

        if ((n = Ocp->GetResultSize(Query->GetStr(), Cnp)) < 0) {
					char* msg = PlugDup(g, g->Message);

					sprintf(g->Message, "Get result size: %s (rc=%d)", msg, n);
					return true;
				} else if (n) {
					Ocp->m_Rows = n;

					if ((Qrp = Ocp->AllocateResult(g)))
						Memory = 2;            // Must be filled
					else {
						strcpy(g->Message, "Result set memory allocation failed");
						return true;
					} // endif n

				} else				 // Void result
					Memory = 0;

				Ocp->m_Rows = 0;
			} else
        return true;

    } // endif Memory

    if (!(rc = MakeSQL(g, false))) {
      for (PODBCCOL colp = (PODBCCOL)Columns; colp;
                    colp = (PODBCCOL)colp->GetNext())
        if (!colp->IsSpecial())
          colp->AllocateBuffers(g, Rows);

			rc = (Mode == MODE_READ)
    		 ? ((Rows = Ocp->ExecDirectSQL(Query->GetStr(), (PODBCCOL)Columns)) < 0)
				 : false;
    } // endif rc

  } else if (Mode == MODE_INSERT) {
    if (!(rc = MakeInsert(g))) {
      if (Nparm != Ocp->PrepareSQL(Query->GetStr())) {
        strcpy(g->Message, MSG(PARM_CNT_MISS));
        rc = true;
      } else
        rc = BindParameters(g);

    } // endif rc

  } else if (Mode == MODE_UPDATE || Mode == MODE_DELETE) {
    rc = false;  // wait for CheckCond before calling MakeCommand(g);
  } else
    sprintf(g->Message, "Invalid mode %d", Mode);

  if (rc) {
    Ocp->Close();
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
int TDBODBC::GetRecpos(void)
{
  return Fpos;
} // end of GetRecpos
#endif // 0

/***********************************************************************/
/*  SetRecpos: set the position of next read record.                   */
/***********************************************************************/
bool TDBODBC::SetRecpos(PGLOBAL g, int recpos)
{
  if (Ocp->m_Full) {
    Fpos = 0;
    CurNum = recpos - 1;
  } else if (Memory == 3) {
    Fpos = recpos;
    CurNum = -1;
  } else if (Scrollable) {
    // Is new position in the current row set?
    if (recpos >= Curpos && recpos < Curpos + Rbuf) {
      CurNum = recpos - Curpos;
      Fpos = 0;
    } else {
      Fpos = recpos;
      CurNum = 0;
    } // endif recpos

  } else {
    strcpy(g->Message, 
			"This action requires Memory setting or a scrollable cursor");
    return true;
  } // endif's

  // Indicate the table position was externally set
  Placed = true;
  return false;
} // end of SetRecpos

/***********************************************************************/
/*  Data Base indexed read routine for ODBC access method.             */
/***********************************************************************/
bool TDBODBC::ReadKey(PGLOBAL g, OPVAL op, const key_range *kr)
{
	char c = Quote ? *Quote : 0;
	int  oldlen = Query->GetLength();
	PHC  hc = To_Def->GetHandler();

	if (!(kr || hc->end_range) || op == OP_NEXT ||
  	     Mode == MODE_UPDATE || Mode == MODE_DELETE) {
		if (!kr && Mode == MODE_READX) {
			// This is a false indexed read
			Rows = Ocp->ExecDirectSQL((char*)Query->GetStr(), (PODBCCOL)Columns);
			Mode = MODE_READ;
			return (Rows < 0);
		} // endif key

		return false;
	}	else {
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
		htrc("ODBC ReadKey: Query=%s\n", Query->GetStr());

	Rows = Ocp->ExecDirectSQL((char*)Query->GetStr(), (PODBCCOL)Columns);
	Query->Truncate(oldlen);
	return (Rows < 0);
} // end of ReadKey

/***********************************************************************/
/*  VRDNDOS: Data Base read routine for odbc access method.            */
/***********************************************************************/
int TDBODBC::ReadDB(PGLOBAL g)
{
  int   rc;

  if (trace(2))
		htrc("ODBC ReadDB: R%d Mode=%d\n", GetTdb_No(), Mode);

  if (Mode == MODE_UPDATE || Mode == MODE_DELETE) {
    if (!Query && MakeCommand(g))
      return RC_FX;

    // Send the UPDATE/DELETE command to the remote table
    if (!Ocp->ExecSQLcommand(Query->GetStr())) {
      sprintf(g->Message, "%s: %d affected rows", TableName, AftRows);

      if (trace(1))
        htrc("%s\n", g->Message);

      PushWarning(g, this, 0);    // 0 means a Note
      return RC_EF;               // Nothing else to do
    } else
      return RC_FX;               // Error

  } // endif Mode

  /*********************************************************************/
  /*  Now start the reading process.                                   */
  /*  Here is the place to fetch the line(s).                          */
  /*********************************************************************/
  if (Placed) {
    if (Fpos && CurNum >= 0)
      Rbuf = Ocp->Fetch((Curpos = Fpos));

    rc = (Rbuf > 0) ? RC_OK : (Rbuf == 0) ? RC_EF : RC_FX;
    Placed = false;
  } else {
    if (Memory != 3) {
      if (++CurNum >= Rbuf) {
        Rbuf = Ocp->Fetch();
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

  } // endif Placed

  if (trace(2))
    htrc(" Read: Rbuf=%d rc=%d\n", Rbuf, rc);

  return rc;
} // end of ReadDB

/***********************************************************************/
/*  Data Base Insert write routine for ODBC access method.             */
/***********************************************************************/
int TDBODBC::WriteDB(PGLOBAL g)
{
  int n = Ocp->ExecuteSQL();

  if (n < 0) {
    AftRows = n;
    return RC_FX;
  } else
    AftRows += n;

  return RC_OK;
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for ODBC access method.              */
/***********************************************************************/
int TDBODBC::DeleteDB(PGLOBAL g, int irc)
{
  if (irc == RC_FX) {
    if (!Query && MakeCommand(g))
      return RC_FX;

    // Send the DELETE (all) command to the remote table
    if (!Ocp->ExecSQLcommand(Query->GetStr())) {
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
/*  Data Base close routine for ODBC access method.                    */
/***********************************************************************/
void TDBODBC::CloseDB(PGLOBAL g)
{
  if (Ocp)

    Ocp->Close();

  if (trace(1))
    htrc("ODBC CloseDB: closing %s\n", Name);

} // end of CloseDB

/* --------------------------- ODBCCOL ------------------------------- */

/***********************************************************************/
/*  ODBCCOL public constructor.                                        */
/***********************************************************************/
ODBCCOL::ODBCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
       : EXTCOL(cdp, tdbp, cprec, i, am)
{
  // Set additional ODBC access method information for column.
  Slen = 0;
  StrLen = &Slen;
  Sqlbuf = NULL;
} // end of ODBCCOL constructor

/***********************************************************************/
/*  ODBCCOL private constructor.                                       */
/***********************************************************************/
ODBCCOL::ODBCCOL(void) : EXTCOL()
{
  Slen = 0;
  StrLen = &Slen;
  Sqlbuf = NULL;
} // end of ODBCCOL constructor

/***********************************************************************/
/*  ODBCCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
ODBCCOL::ODBCCOL(ODBCCOL *col1, PTDB tdbp) : EXTCOL(col1, tdbp)
{
  Slen = col1->Slen;
  StrLen = col1->StrLen;
  Sqlbuf = col1->Sqlbuf;
} // end of ODBCCOL copy constructor

/***********************************************************************/
/*  ReadColumn: when SQLFetch is used there is nothing to do as the    */
/*  column buffer was bind to the record set. This is also the case    */
/*  when calculating MaxSize (Bufp is NULL even when Rows is not).     */
/***********************************************************************/
void ODBCCOL::ReadColumn(PGLOBAL g)
{
  PTDBODBC tdbp = (PTDBODBC)To_Tdb;
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

  if (StrLen[n] == SQL_NULL_DATA) {
    // Null value
    if (Nullable)
      Value->SetNull(true);

    Value->Reset();
    goto put;
  } else
    Value->SetNull(false);

  if (Bufp && tdbp->Rows) {
    if (Buf_Type == TYPE_DATE)
      *Sqlbuf = ((TIMESTAMP_STRUCT*)Bufp)[n];
    else
      Value->SetValue_pvblk(Blkp, n);

  } // endif Bufp

  if (Buf_Type == TYPE_DATE) {
    struct tm dbtime;

    memset(&dbtime, 0, sizeof(tm));
    dbtime.tm_sec = (int)Sqlbuf->second;
    dbtime.tm_min = (int)Sqlbuf->minute;
    dbtime.tm_hour = (int)Sqlbuf->hour;
    dbtime.tm_mday = (int)Sqlbuf->day;
    dbtime.tm_mon = (int)Sqlbuf->month - 1;
    dbtime.tm_year = (int)Sqlbuf->year - 1900;
    ((DTVAL*)Value)->MakeTime(&dbtime);
  } else if (Buf_Type == TYPE_DECIM && tdbp->Sep) {
    // Be sure to use decimal point
    char *p = strchr(Value->GetCharValue(), tdbp->Sep);

    if (p)
      *p = '.';

  } // endif Buf_Type

  if (trace(2)) {
    char buf[64];

    htrc("ODBC Column %s: rows=%d buf=%p type=%d value=%s\n",
      Name, tdbp->Rows, Bufp, Buf_Type, Value->GetCharString(buf));
  } // endif trace

 put:
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
/*  AllocateBuffers: allocate the extended buffer for SQLExtendedFetch */
/*  or Fetch.  Note: we use Long+1 here because ODBC must have space   */
/*  for the ending null character.                                     */
/***********************************************************************/
void ODBCCOL::AllocateBuffers(PGLOBAL g, int rows)
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
void *ODBCCOL::GetBuffer(DWORD rows)
{
  if (rows && To_Tdb) {
    assert(rows == (DWORD)((TDBODBC*)To_Tdb)->Rows);
    return Bufp;
  } else
    return (Buf_Type == TYPE_DATE) ? Sqlbuf : Value->GetTo_Val();

} // end of GetBuffer

/***********************************************************************/
/*  Returns the buffer length to use for Fetch or Extended Fetch.      */
/***********************************************************************/
SWORD ODBCCOL::GetBuflen(void)
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

/***********************************************************************/
/*  WriteColumn: make sure the bind buffer is updated.                 */
/***********************************************************************/
void ODBCCOL::WriteColumn(PGLOBAL g)
{
  /*********************************************************************/
  /*  Do convert the column value if necessary.                        */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, FALSE);   // Convert the inserted value

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
    char *p, sep = ((PTDBODBC)To_Tdb)->Sep;

    if (sep && (p = strchr(Value->GetCharValue(), '.')))
      *p = sep;

  } // endif Buf_Type

  if (Nullable)
    *StrLen = (Value->IsNull()) ? SQL_NULL_DATA :
         (IsTypeChar(Buf_Type)) ? SQL_NTS : 0;

} // end of WriteColumn

/* -------------------------- Class TDBXDBC -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBXDBC class.                               */
/***********************************************************************/
TDBXDBC::TDBXDBC(PODEF tdp) : TDBODBC(tdp)
{
  Cmdlist = NULL;
  Cmdcol = NULL;
  Mxr = tdp->Maxerr;
  Nerr = 0;
} // end of TDBXDBC constructor

TDBXDBC::TDBXDBC(PTDBXDBC tdbp) : TDBODBC(tdbp)
{
  Cmdlist = tdbp->Cmdlist;
  Cmdcol = tdbp->Cmdcol;
  Mxr = tdbp->Mxr;
  Nerr = tdbp->Nerr;
} // end of TDBXDBC copy constructor

PTDB TDBXDBC::Clone(PTABS t)
{
  PTDB     tp;
  PXSRCCOL cp1, cp2;
  PGLOBAL  g = t->G;        // Is this really useful ???

  tp = new(g) TDBXDBC(this);

  for (cp1 = (PXSRCCOL)Columns; cp1; cp1 = (PXSRCCOL)cp1->GetNext()) {
    cp2 = new(g) XSRCCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
  } // endfor cp1

  return tp;
} // end of CopyOne

/***********************************************************************/
/*  Allocate XSRC column description block.                            */
/***********************************************************************/
PCOL TDBXDBC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
  PXSRCCOL colp = new(g) XSRCCOL(cdp, this, cprec, n);

  if (!colp->Flag)
    Cmdcol = colp->GetName();

  return colp;
} // end of MakeCol

/***********************************************************************/
/*  MakeCMD: make the SQL statement to send to ODBC connection.        */
/***********************************************************************/
PCMD TDBXDBC::MakeCMD(PGLOBAL g)
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
/*  ODBC Bind Parameter function.                                      */
/***********************************************************************/
bool TDBXDBC::BindParameters(PGLOBAL g)
{
  PODBCCOL colp;

  for (colp = (PODBCCOL)Columns; colp; colp = (PODBCCOL)colp->Next) {
    colp->AllocateBuffers(g, 0);

    if (Ocp->BindParam(colp))
      return true;

    } // endfor colp

  return false;
} // end of BindParameters
#endif // 0

/***********************************************************************/
/*  XDBC GetMaxSize: returns table size (not always one row).          */
/***********************************************************************/
int TDBXDBC::GetMaxSize(PGLOBAL g)
{
  if (MaxSize < 0)
    MaxSize = 10;             // Just a guess

  return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  ODBC Access Method opening routine.                                */
/*  New method now that this routine is called recursively (last table */
/*  first in reverse order): index blocks are immediately linked to    */
/*  join block of next table if it exists or else are discarted.       */
/***********************************************************************/
bool TDBXDBC::OpenDB(PGLOBAL g)
{
  if (trace(1))
    htrc("ODBC OpenDB: tdbp=%p tdb=R%d use=%dmode=%d\n",
            this, Tdb_No, Use, Mode);

  if (Use == USE_OPEN) {
    strcpy(g->Message, "Multiple execution is not allowed");
    return true;
  } // endif use

  /*********************************************************************/
  /*  Open an ODBC connection for this table.                          */
  /*  Note: this may not be the proper way to do. Perhaps it is better */
  /*  to test whether a connection is already open for this datasource */
  /*  and if so to allocate just a new result set. But this only for   */
  /*  drivers allowing concurency in getting results ???               */
  /*********************************************************************/
  if (!Ocp) {
    Ocp = new(g) ODBConn(g, this);
  } else if (Ocp->IsOpen())
    Ocp->Close();

  if (Ocp->Open(Connect, &Ops, Options) < 1)
    return true;

  Use = USE_OPEN;       // Do it now in case we are recursively called

  if (Mode != MODE_READ && Mode != MODE_READX) {
    strcpy(g->Message, "No INSERT/DELETE/UPDATE of XDBC tables");
    return true;
  } // endif Mode

  /*********************************************************************/
  /*  Get the command to execute.                                      */
  /*********************************************************************/
  if (!(Cmdlist = MakeCMD(g))) {
		// Next lines commented out because of CHECK TABLE
		//Ocp->Close();
    //return true;
  } // endif Cmdlist

  Rows = 1;
  return false;
} // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for xdbc access method.             */
/***********************************************************************/
int TDBXDBC::ReadDB(PGLOBAL g)
{
  if (Cmdlist) {
		if (!Query)
			Query = new(g)STRING(g, 0, Cmdlist->Cmd);
		else
			Query->Set(Cmdlist->Cmd);

    if (Ocp->ExecSQLcommand(Query->GetStr()))
      Nerr++;

    Fpos++;                // Used for progress info
    Cmdlist = (Nerr > Mxr) ? NULL : Cmdlist->Next;
    return RC_OK;
	} else {
		PushWarning(g, this, 1);
		return RC_EF;
	}	// endif Cmdlist

} // end of ReadDB

/***********************************************************************/
/*  Data Base write line routine for XDBC access method.               */
/***********************************************************************/
int TDBXDBC::WriteDB(PGLOBAL g)
{
  strcpy(g->Message, "Execsrc tables are read only");
  return RC_FX;
} // end of DeleteDB

/***********************************************************************/
/*  Data Base delete line routine for XDBC access method.              */
/***********************************************************************/
int TDBXDBC::DeleteDB(PGLOBAL g, int irc)
{
  strcpy(g->Message, MSG(NO_ODBC_DELETE));
  return RC_FX;
} // end of DeleteDB

/* --------------------------- XSRCCOL ------------------------------- */

/***********************************************************************/
/*  XSRCCOL public constructor.                                        */
/***********************************************************************/
XSRCCOL::XSRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
       : ODBCCOL(cdp, tdbp, cprec, i, am)
{
  // Set additional ODBC access method information for column.
  Flag = cdp->GetOffset();
} // end of XSRCCOL constructor

/***********************************************************************/
/*  XSRCCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
XSRCCOL::XSRCCOL(XSRCCOL *col1, PTDB tdbp) : ODBCCOL(col1, tdbp)
{
  Flag = col1->Flag;
} // end of XSRCCOL copy constructor

/***********************************************************************/
/*  ReadColumn: set column value according to Flag.                    */
/***********************************************************************/
void XSRCCOL::ReadColumn(PGLOBAL g)
{
  PTDBXDBC tdbp = (PTDBXDBC)To_Tdb;

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
void XSRCCOL::WriteColumn(PGLOBAL g)
{
  // Should never be called
} // end of WriteColumn

/* ---------------------------TDBDRV class --------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of ODBC drivers.                           */
/***********************************************************************/
PQRYRES TDBDRV::GetResult(PGLOBAL g)
{
  return ODBCDrivers(g, Maxres, false);
} // end of GetResult

/* ---------------------------TDBSRC class --------------------------- */

/***********************************************************************/
/*  GetResult: Get the list of ODBC data sources.                      */
/***********************************************************************/
PQRYRES TDBSRC::GetResult(PGLOBAL g)
{
  return ODBCDataSources(g, Maxres, false);
} // end of GetResult

/* ---------------------------TDBOTB class --------------------------- */

/***********************************************************************/
/*  TDBOTB class constructor.                                          */
/***********************************************************************/
TDBOTB::TDBOTB(PODEF tdp) : TDBDRV(tdp)
{
  Dsn = tdp->GetConnect();
  Schema = tdp->GetTabschema();
  Tab = tdp->GetTabname();
	Tabtyp = tdp->Tabtyp;
  Ops.User = tdp->Username;
  Ops.Pwd = tdp->Password;
  Ops.Cto = tdp->Cto;
  Ops.Qto = tdp->Qto;
  Ops.UseCnc = tdp->UseCnc;
} // end of TDBOTB constructor

/***********************************************************************/
/*  GetResult: Get the list of ODBC tables.                            */
/***********************************************************************/
PQRYRES TDBOTB::GetResult(PGLOBAL g)
{
  return ODBCTables(g, Dsn, Schema, Tab, Tabtyp, Maxres, false, &Ops);
} // end of GetResult

/* ---------------------------TDBOCL class --------------------------- */

/***********************************************************************/
/*  TDBOCL class constructor.                                          */
/***********************************************************************/
TDBOCL::TDBOCL(PODEF tdp) : TDBOTB(tdp)
{
	Colpat = tdp->Colpat;
} // end of TDBOTB constructor

/***********************************************************************/
/*  GetResult: Get the list of ODBC table columns.                     */
/***********************************************************************/
PQRYRES TDBOCL::GetResult(PGLOBAL g)
{
  return ODBCColumns(g, Dsn, Schema, Tab, Colpat, Maxres, false, &Ops);
} // end of GetResult

/* ------------------------ End of Tabodbc --------------------------- */
