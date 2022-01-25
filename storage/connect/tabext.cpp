/************* Tabext C++ Functions Source Code File (.CPP) ************/
/*  Name: TABEXT.CPP  Version 1.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017 - 2019  */
/*                                                                     */
/*  This file contains the TBX, TDB and OPJOIN classes functions.      */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#define MYSQL_SERVER 1
#include "my_global.h"
#include "sql_class.h"
#include "sql_servers.h"
#include "sql_string.h"
#if !defined(_WIN32)
#include "osutil.h"
#endif

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/*  xobject.h   is header containing XOBJECT derived classes declares. */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "tabext.h"
#include "ha_connect.h"

/* -------------------------- Class CONDFIL -------------------------- */

/***********************************************************************/
/*  CONDFIL Constructor.                                               */
/***********************************************************************/
CONDFIL::CONDFIL(uint idx, AMT type)
{
//Cond = cond; 
	Idx = idx; 
	Type = type;
	Op = OP_XX;
	Cmds = NULL;
	Alist = NULL;
	All = true;
	Bd = false;
	Hv = false;
	Body = NULL, 
	Having = NULL;
}	// end of CONDFIL constructor

/***********************************************************************/
/*  Make and allocate the alias list.                                  */
/***********************************************************************/
int CONDFIL::Init(PGLOBAL g, PHC hc)
{
	PTOS  options = hc->GetTableOptionStruct();
	char *p, *cn, *cal, *alt = NULL;
	int   rc = RC_OK;
	bool  h;

	if (options)
    alt = (char*)GetListOption(g, "Alias", options->oplist, NULL);

	while (alt) {
		if (!(p = strchr(alt, '='))) {
			strcpy(g->Message, "Invalid alias list");
			rc = RC_FX;
			break;
		}	// endif !p

		cal = alt;				 // Alias
		*p++ = 0;

		if ((h = *p == '*')) {
			rc = RC_INFO;
			p++;
		}	// endif h

		cn = p;						// Remote column name

		if ((alt = strchr(p, ';')))
			*alt++ = 0;

		if (*cn == 0)
			cn = alt;

		Alist = new(g) ALIAS(Alist, cn, cal, h);
	}	// endwhile alt

	return rc;
}	// end of Init

/***********************************************************************/
/*  Make and allocate the alias list.                                  */
/***********************************************************************/
const char *CONDFIL::Chk(const char *fln, bool *h)
{
	for (PAL pal = Alist; pal; pal = pal->Next)
		if (!stricmp(fln, pal->Alias)) {
			*h = pal->Having;
			return pal->Name;
		}	// endif fln

	*h = false;
	return fln;
}	// end of Chk

/* --------------------------- Class EXTDEF -------------------------- */

/***********************************************************************/
/*  EXTDEF Constructor.                                                */
/***********************************************************************/
EXTDEF::EXTDEF(void)
{
	Tabname = Tabschema = Username = Password = Tabcat = Tabtyp = NULL;
	Colpat = Srcdef = Qchar = Qrystr = Sep = Phpos = NULL;
	Options = Cto = Qto = Quoted = Maxerr = Maxres = Memory = 0;
	Scrollable = Xsrc = false;
} // end of EXTDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool EXTDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
	if (g->Createas) {
		strcpy(g->Message,
			"Multiple-table UPDATE/DELETE commands are not supported");
		return true;
	}	// endif multi

	Desc = NULL;
	Tabname = GetStringCatInfo(g, "Name",
		(Catfunc & (FNC_TABLE | FNC_COL)) ? NULL : Name);
	Tabname = GetStringCatInfo(g, "Tabname", Tabname);
	Tabschema = GetStringCatInfo(g, "Dbname", NULL);
	Tabschema = GetStringCatInfo(g, "Schema", Tabschema);
	Tabcat = GetStringCatInfo(g, "Qualifier", NULL);
	Tabcat = GetStringCatInfo(g, "Catalog", Tabcat);
	Username = GetStringCatInfo(g, "User", NULL);
	Password = GetStringCatInfo(g, "Password", NULL);

	// Memory was Boolean, it is now integer
	if (!(Memory = GetIntCatInfo("Memory", 0)))
		Memory = GetBoolCatInfo("Memory", false) ? 1 : 0;

	if ((Srcdef = GetStringCatInfo(g, "Srcdef", NULL))) {
		Read_Only = true;
		if (Memory == 2) Memory = 1;
	}	// endif Srcdef

	Qrystr = GetStringCatInfo(g, "Query_String", "?");
	Sep = GetStringCatInfo(g, "Separator", NULL);
//Alias = GetStringCatInfo(g, "Alias", NULL);
	Phpos = GetStringCatInfo(g, "Phpos", NULL);
	Xsrc = GetBoolCatInfo("Execsrc", FALSE);
	Maxerr = GetIntCatInfo("Maxerr", 0);
	Maxres = GetIntCatInfo("Maxres", 0);
	Quoted = GetIntCatInfo("Quoted", 0);
	Options = 0;
	Cto = 0;
	Qto = 0;

	if ((Scrollable = GetBoolCatInfo("Scrollable", false)) && !Elemt)
		Elemt = 1;     // Cannot merge SQLFetch and SQLExtendedFetch

	if (Catfunc == FNC_COL)
		Colpat = GetStringCatInfo(g, "Colpat", NULL);

	if (Catfunc == FNC_TABLE)
		Tabtyp = GetStringCatInfo(g, "Tabtype", NULL);

	Pseudo = 2;    // FILID is Ok but not ROWID
	return false;
} // end of DefineAM

/* ---------------------------TDBEXT class --------------------------- */

/***********************************************************************/
/*  Implementation of the TDBEXT class.                                */
/***********************************************************************/
TDBEXT::TDBEXT(EXTDEF *tdp) : TDB(tdp)
{
	Qrp = NULL;

	if (tdp) {
		TableName = tdp->Tabname;
		Schema = tdp->Tabschema;
		User = tdp->Username;
		Pwd = tdp->Password;
		Catalog = tdp->Tabcat;
		Srcdef = tdp->Srcdef;
		Qrystr = tdp->Qrystr;
		Sep = tdp->GetSep();
		Options = tdp->Options;
		Cto = tdp->Cto;
		Qto = tdp->Qto;
		Quoted = MY_MAX(0, tdp->GetQuoted());
		Rows = tdp->GetElemt();
		Memory = tdp->Memory;
		Scrollable = tdp->Scrollable;
	} else {
		TableName = NULL;
		Schema = NULL;
		User = NULL;
		Pwd = NULL;
		Catalog = NULL;
		Srcdef = NULL;
		Qrystr = NULL;
		Sep = 0;
		Options = 0;
		Cto = 0;
		Qto = 0;
		Quoted = 0;
		Rows = 0;
		Memory = 0;
		Scrollable = false;
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
	Nparm = 0;
	Ncol = 0;
	Placed = false;
} // end of TDBEXT constructor

TDBEXT::TDBEXT(PTDBEXT tdbp) : TDB(tdbp)
{
	Qrp = tdbp->Qrp;
	TableName = tdbp->TableName;
	Schema = tdbp->Schema;
	User = tdbp->User;
	Pwd = tdbp->Pwd;
	Catalog = tdbp->Catalog;
	Srcdef = tdbp->Srcdef;
	Qrystr = tdbp->Qrystr;
	Sep = tdbp->Sep;
	Options = tdbp->Options;
	Cto = tdbp->Cto;
	Qto = tdbp->Qto;
	Quoted = tdbp->Quoted;
	Rows = tdbp->Rows;
	Memory = tdbp->Memory;
	Scrollable = tdbp->Scrollable;
	Quote = tdbp->Quote;
	Query = tdbp->Query;
	Count = tdbp->Count;
	//Where = tdbp->Where;
	MulConn = tdbp->MulConn;
	DBQ = tdbp->DBQ;
	Fpos = 0;
	Curpos = 0;
	AftRows = 0;
	CurNum = 0;
	Rbuf = 0;
	BufSize = tdbp->BufSize;
	Nparm = tdbp->Nparm;
	Ncol = tdbp->Ncol;
	Placed = false;
} // end of TDBEXT copy constructor

/******************************************************************/
/*  Convert an UTF-8 string to latin characters.                  */
/******************************************************************/
int TDBEXT::Decode(PCSZ txt, char *buf, size_t n)
{
	uint   dummy_errors;
	uint32 len = copy_and_convert(buf, n, &my_charset_latin1,
		txt, strlen(txt),
		&my_charset_utf8_general_ci,
		&dummy_errors);
	buf[len] = '\0';
	return 0;
} // end of Decode

/*
  Count number of %s placeholders in string.
  Returns -1 if other sprintf placeholders are found, .g %d
*/
static int count_placeholders(const char *fmt)
{
  int cnt= 0;
  for (const char *p=fmt; *p; p++)
  {
    if (*p == '%')
    {
      switch (p[1])
      {
      case 's':
        /* %s found */
        cnt++;
        p++;
        break;
      case '%':
        /* masking char for % found */
        p++;
        break;
      default:
        /* some other placeholder found */
        return -1;
      }
    }
  }
  return cnt;
}

/***********************************************************************/
/*  MakeSrcdef: make the SQL statement from SRDEF option.              */
/***********************************************************************/
bool TDBEXT::MakeSrcdef(PGLOBAL g)
{
	char *catp = strstr(Srcdef, "%s");

	if (catp) {
		char *fil1 = 0, *fil2;
		PCSZ  ph = ((EXTDEF*)To_Def)->Phpos;

		if (!ph)
			ph = (strstr(catp + 2, "%s")) ? "WH" : "W";

		if (stricmp(ph, "H")) {
			fil1 = (To_CondFil && *To_CondFil->Body)
				? To_CondFil->Body : PlugDup(g, "1=1");
		} // endif ph

		if (stricmp(ph, "W")) {
			fil2 = (To_CondFil && To_CondFil->Having && *To_CondFil->Having)
				? To_CondFil->Having : PlugDup(g, "1=1");
		} // endif ph

		int n_placeholders = count_placeholders(Srcdef);
		if (n_placeholders < 0)
		{
			strcpy(g->Message, "MakeSQL: Wrong place holders specification");
			return true;
		}

		if (!stricmp(ph, "W") && n_placeholders <= 1) {
			Query = new(g)STRING(g, strlen(Srcdef) + strlen(fil1));
			Query->SetLength(sprintf(Query->GetStr(), Srcdef, fil1));
		}
		else if (!stricmp(ph, "WH") && n_placeholders <= 2)
		{
			Query = new(g)STRING(g, strlen(Srcdef) + strlen(fil1) + strlen(fil2));
			Query->SetLength(sprintf(Query->GetStr(), Srcdef, fil1, fil2));
		}
		else if (!stricmp(ph, "H") && n_placeholders <= 1)
		{
			Query = new(g)STRING(g, strlen(Srcdef) + strlen(fil2));
			Query->SetLength(sprintf(Query->GetStr(), Srcdef, fil2));
		}
		else if (!stricmp(ph, "HW") && n_placeholders <= 2)
		{
			Query = new(g)STRING(g, strlen(Srcdef) + strlen(fil1) + strlen(fil2));
			Query->SetLength(sprintf(Query->GetStr(), Srcdef, fil2, fil1));
		} else {
			strcpy(g->Message, "MakeSQL: Wrong place holders specification");
			return true;
		} // endif's ph

	} else
		Query = new(g)STRING(g, 0, Srcdef);

	return false;
} // end of MakeSrcdef

	/***********************************************************************/
	/*  MakeSQL: make the SQL statement use with remote connection.        */
	/*  TODO: when implementing remote filtering, column only used in      */
	/*  local filter should be removed from column list.                   */
	/***********************************************************************/
bool TDBEXT::MakeSQL(PGLOBAL g, bool cnt)
{
	PCSZ   schmp = NULL;
	char  *catp = NULL, buf[NAM_LEN * 3];
	int    len;
	bool   first = true;
	PTABLE tablep = To_Table;
	PCOL   colp;

	if (Srcdef)
		return MakeSrcdef(g);

	// Allocate the string used to contain the Query
	Query = new(g)STRING(g, 1023, "SELECT ");

	if (!cnt) {
		if (Columns) {
			// Normal SQL statement to retrieve results
			for (colp = Columns; colp; colp = colp->GetNext())
				if (!colp->IsSpecial()) {
					if (!first)
						Query->Append(", ");
					else
						first = false;

					// Column name can be encoded in UTF-8
					Decode(colp->GetName(), buf, sizeof(buf));

					if (Quote) {
						// Put column name between identifier quotes in case in contains blanks
						Query->Append(Quote);
						Query->Append(buf);
						Query->Append(Quote);
					} else
						Query->Append(buf);

					((PEXTCOL)colp)->SetRank(++Ncol);
				} // endif colp

		} else
			// !Columns can occur for queries such that sql count(*) from...
			// for which we will count the rows from sql * from...
			Query->Append('*');

	} else
		// SQL statement used to retrieve the size of the result
		Query->Append("count(*)");

	Query->Append(" FROM ");

	if (Catalog && *Catalog)
		catp = Catalog;

	//if (tablep->GetSchema())
	//	schmp = (char*)tablep->GetSchema();
	//else 
	if (Schema && *Schema)
		schmp = Schema;

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

	// Table name can be encoded in UTF-8
	Decode(TableName, buf, sizeof(buf));

	if (Quote) {
		// Put table name between identifier quotes in case in contains blanks
		Query->Append(Quote);
		Query->Append(buf);
		Query->Append(Quote);
	} else
		Query->Append(buf);

	len = Query->GetLength();

	if (To_CondFil) {
		if (Mode == MODE_READ) {
			Query->Append(" WHERE ");
			Query->Append(To_CondFil->Body);
			len = Query->GetLength() + 1;
		} else
			len += (strlen(To_CondFil->Body) + 256);

	} else
		len += ((Mode == MODE_READX) ? 256 : 1);

	if (Query->IsTruncated()) {
		strcpy(g->Message, "MakeSQL: Out of memory");
		return true;
	} else
		Query->Resize(len);

	if (trace(33))
		htrc("Query=%s\n", Query->GetStr());

	return false;
} // end of MakeSQL

/***********************************************************************/
/*  Remove the NAME_CONST functions that are added by procedures.      */
/***********************************************************************/
void TDBEXT::RemoveConst(PGLOBAL g, char *stmt)
{
	char *p, *p2;
	char  val[1025], nval[1025];
	int   n, nc;

	while ((p = strstr(stmt, "NAME_CONST")))
		if ((n = sscanf(p, "%*[^,],%1024[^)])%n", val, &nc))) {
			if (trace(33))
				htrc("p=%s\nn=%d val=%s nc=%d\n", p, n, val, nc);

			*p = 0;

			if ((p2 = strstr(val, "'"))) {
				if ((n = sscanf(p2, "%*['\\]%1024[^'\\]", nval))) {
					if (trace(33))
						htrc("p2=%s\nn=%d nval=%s\n", p2, n, nval);

					strcat(strcat(strcat(strcat(stmt, "'"), nval), "'"), p + nc);
				} else
					break;

			} else
				strcat(strcat(strcat(strcat(stmt, "("), val), ")"), p + nc);

			if (trace(33))
				htrc("stmt=%s\n", stmt);

		} else
			break;

		return;
} // end of RemoveConst

/***********************************************************************/
/*  MakeCommand: make the Update or Delete statement to send to the    */
/*  MySQL server. Limited to remote values and filtering.              */
/***********************************************************************/
bool TDBEXT::MakeCommand(PGLOBAL g)
{
	PCSZ  schmp = NULL;
	char *p, *stmt, name[132], *body = NULL;
	char *qrystr = (char*)PlugSubAlloc(g, NULL, strlen(Qrystr) + 1);
	bool  qtd = Quoted > 0;
	char  q = qtd ? *Quote : ' ';
	int   i = 0, k = 0;

	// Make a lower case copy of the originale query and change
	// back ticks to the data source identifier quoting character
	do {
		qrystr[i] = (Qrystr[i] == '`') ? q : tolower(Qrystr[i]);
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

	if (strstr(" update delete low_priority ignore quick from ", name)) {
		if (Quote) {
			strlwr(strcat(strcat(strcpy(name, Quote), Name), Quote));
			k += 2;
		} else {
			strcpy(g->Message, "Quoted must be specified");
			return true;
		}	// endif Quote

	} else
		strlwr(strcpy(name, Name));     // Not a keyword

	if ((p = strstr(qrystr, name))) {
		for (i = 0; i < p - qrystr; i++)
			stmt[i] = (Qrystr[i] == '`') ? q : Qrystr[i];

		stmt[i] = 0;

		k += i + (int)strlen(Name);

		if (Schema && *Schema)
			schmp = Schema;

		if (qtd && *(p - 1) == ' ') {
			if (schmp)
				strcat(strcat(stmt, schmp), ".");

			strcat(strcat(strcat(stmt, Quote), TableName), Quote);
		} else {
			if (schmp) {
				if (qtd && *(p - 1) != ' ') {
					stmt[i - 1] = 0;
					strcat(strcat(strcat(stmt, schmp), "."), Quote);
				} else
					strcat(strcat(stmt, schmp), ".");

			}	// endif schmp

			strcat(stmt, TableName);
		} // endif's

		i = (int)strlen(stmt);

		do {
			stmt[i++] = (Qrystr[k] == '`') ? q : Qrystr[k];
		} while (Qrystr[k++]);

		RemoveConst(g, stmt);

		if (body)
			strcat(stmt, body);

	} else {
		sprintf(g->Message, "Cannot use this %s command",
			(Mode == MODE_UPDATE) ? "UPDATE" : "DELETE");
		return true;
	} // endif p

	if (trace(33))
		htrc("Command=%s\n", stmt);

	Query = new(g)STRING(g, 0, stmt);
	return (!Query->GetSize());
} // end of MakeCommand

/***********************************************************************/
/*  GetRecpos: return the position of last read record.                */
/***********************************************************************/
int TDBEXT::GetRecpos(void)
{
	return Fpos;
} // end of GetRecpos

/***********************************************************************/
/*  ODBC GetMaxSize: returns table size estimate in number of lines.   */
/***********************************************************************/
int TDBEXT::GetMaxSize(PGLOBAL g)
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

/***********************************************************************/
/*  Return max size value.                                             */
/***********************************************************************/
int TDBEXT::GetProgMax(PGLOBAL g)
{
	return GetMaxSize(g);
} // end of GetProgMax

/* ---------------------------EXTCOL class --------------------------- */

/***********************************************************************/
/*  EXTCOL public constructor.                                         */
/***********************************************************************/
EXTCOL::EXTCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
	: COLBLK(cdp, tdbp, i)
{
	if (cprec) {
		Next = cprec->GetNext();
		cprec->SetNext(this);
	} else {
		Next = tdbp->GetColumns();
		tdbp->SetColumns(this);
	} // endif cprec

	if (trace(1))
		htrc(" making new %sCOL C%d %s at %p\n", am, Index, Name, this);

	// Set additional remote access method information for column.
	Crp = NULL;
	Long = Precision;
	To_Val = NULL;
	Bufp = NULL;
	Blkp = NULL;
	Rank = 0;           // Not known yet
} // end of JDBCCOL constructor

/***********************************************************************/
/*  EXTCOL private constructor.                                        */
/***********************************************************************/
EXTCOL::EXTCOL(void) : COLBLK()
{
	Crp = NULL;
	Buf_Type = TYPE_INT;     // This is a count(*) column

	// Set additional Dos access method information for column.
	Long = sizeof(int);
	To_Val = NULL;
	Bufp = NULL;
	Blkp = NULL;
	Rank = 1;
} // end of EXTCOL constructor

/***********************************************************************/
/*  EXTCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
EXTCOL::EXTCOL(PEXTCOL col1, PTDB tdbp) : COLBLK(col1, tdbp)
{
	Crp = col1->Crp;
	Long = col1->Long;
	To_Val = col1->To_Val;
	Bufp = col1->Bufp;
	Blkp = col1->Blkp;
	Rank = col1->Rank;
} // end of JDBCCOL copy constructor

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool EXTCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
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

