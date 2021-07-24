/************** tabjmg C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: tabjmg     Version 1.3                                */
/*  (C) Copyright to the author Olivier BERTRAND          2021         */
/*  This file contains the MongoDB classes using the Java Driver.      */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "maputil.h"
#include "filamtxt.h"
#include "tabext.h"
#include "tabjmg.h"
#include "tabmul.h"
#include "checklvl.h"
#include "resource.h"
#include "mycat.h"                             // for FNC_COL
#include "filter.h"

#define nullptr 0

PQRYRES MGOColumns(PGLOBAL g, PCSZ db, PCSZ uri, PTOS topt, bool info);
bool    Stringified(PCSZ, char*);

/* -------------------------- Class JMGDISC -------------------------- */

/***********************************************************************/
/*  Constructor																												 */
/***********************************************************************/
JMGDISC::JMGDISC(PGLOBAL g, int *lg) : MGODISC(g, lg)
{
	drv = "Java"; Jcp = NULL; columnid = nullptr; bvnameid = nullptr;
}	// end of JMGDISC constructor

/***********************************************************************/
/*  Initialyze.                                                        */
/***********************************************************************/
bool JMGDISC::Init(PGLOBAL g)
{
	if (!(Jcp = ((TDBJMG*)tmgp)->Jcp)) {
		strcpy(g->Message, "Init: Jcp is NULL");
		return true;
	}	else if (Jcp->gmID(g, columnid, "ColumnDesc",
		                  "(Ljava/lang/Object;I[II)Ljava/lang/Object;"))
		return true;
	else if (Jcp->gmID(g, bvnameid, "ColDescName", "()Ljava/lang/String;"))
	  return true;

	return false;
}	// end of Init

/***********************************************************************/
/*  Analyse passed document.                                           */
/***********************************************************************/
bool JMGDISC::Find(PGLOBAL g)
{
	return ColDesc(g, nullptr, NULL, NULL, Jcp->m_Ncol, 0);
}	// end of Find

/***********************************************************************/
/*  Analyse passed document.                                           */
/***********************************************************************/
bool JMGDISC::ColDesc(PGLOBAL g, jobject obj, char *pcn, char *pfmt,
											int ncol, int k)
{
	const char *key, *utf;
	char    colname[65];
	char    fmt[129];
	bool    rc = true;
	size_t  z;
	jint   *n = nullptr;
	jstring jkey;
	jobject jres;

	// Build the java int array
	jintArray val = Jcp->env->NewIntArray(5);

	if (val == nullptr) {
		strcpy(g->Message, "Cannot allocate jint array");
		return true;
	} else if (!ncol)
		n = Jcp->env->GetIntArrayElements(val, 0);

	for (int i = 0; i < ncol; i++) {
		jres = Jcp->env->CallObjectMethod(Jcp->job, columnid, obj, i, val, lvl - k);
		n = Jcp->env->GetIntArrayElements(val, 0);

		if (Jcp->Check(n[0])) {
			sprintf(g->Message, "ColDesc: %s", Jcp->Msg);
			goto err;
		} else if (!n[0])
			continue;

		jkey = (jstring)Jcp->env->CallObjectMethod(Jcp->job, bvnameid);
		utf = Jcp->env->GetStringUTFChars(jkey, nullptr);
		key = PlugDup(g, utf);
		Jcp->env->ReleaseStringUTFChars(jkey, utf);
		Jcp->env->DeleteLocalRef(jkey);

		if (pcn) {
			strncpy(colname, pcn, 64);
			colname[64] = 0;
			z = 65 - strlen(colname);
			strncat(strncat(colname, "_", z), key, z - 1);
		} else
			strcpy(colname, key);

		if (pfmt) {
			strncpy(fmt, pfmt, 128);
			fmt[128] = 0;
			z = 129 - strlen(fmt);
			strncat(strncat(fmt, ".", z), key, z - 1);
		} else
			strcpy(fmt, key);

		if (!jres) {
			bcol.Type = n[0];
			bcol.Len = n[1];
			bcol.Scale = n[2];
			bcol.Cbn = n[3];
			AddColumn(g, colname, fmt, k);
		} else {
			if (n[0] == 2 && !all)
				n[4] = MY_MIN(n[4], 1);

			if (ColDesc(g, jres, colname, fmt, n[4], k + 1))
				goto err;

		}	// endif jres

	} // endfor i

	rc = false;

 err:
	Jcp->env->ReleaseIntArrayElements(val, n, 0);
	return rc;
}	// end of ColDesc

/* --------------------------- Class TDBJMG -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBJMG class.                                */
/***********************************************************************/
TDBJMG::TDBJMG(PMGODEF tdp) : TDBEXT(tdp)
{
	Jcp = NULL;
//Cnp = NULL;

	if (tdp) {
		Ops.Driver = tdp->Tabschema;
		Ops.Url = tdp->Uri;
		Ops.Version = tdp->Version;
		Uri = tdp->Uri;
		Db_name = tdp->Tabschema;
		Wrapname = tdp->Wrapname;
		Coll_name = tdp->Tabname;
		Options = tdp->Colist;
		Filter = tdp->Filter;
		Strfy = tdp->Strfy;
		B = tdp->Base ? 1 : 0;
		Pipe = tdp->Pipe && Options != NULL;
	} else {
		Ops.Driver = NULL;
		Ops.Url = NULL;
		Ops.Version = 0;
		Uri = NULL;
		Db_name = NULL;
		Coll_name = NULL;
		Options = NULL;
		Filter = NULL;
		Strfy = NULL;
		B = 0;
		Pipe = false;
	} // endif tdp

	Ops.User = NULL;
	Ops.Pwd = NULL;
	Ops.Scrollable = false;
	Ops.Fsize = 0;
	Fpos = -1;
	N = 0;
	Done = false;
} // end of TDBJMG standard constructor

TDBJMG::TDBJMG(TDBJMG *tdbp) : TDBEXT(tdbp)
{
	Uri = tdbp->Uri;
	Db_name = tdbp->Db_name;;
	Coll_name = tdbp->Coll_name;
	Options = tdbp->Options;
	Filter = tdbp->Filter;
	Strfy = tdbp->Strfy;
	B = tdbp->B;
	Fpos = tdbp->Fpos;
	N = tdbp->N;
	Done = tdbp->Done;
	Pipe = tdbp->Pipe;
} // end of TDBJMG copy constructor

// Used for update
PTDB TDBJMG::Clone(PTABS t)
{
	PTDB    tp;
	PJMGCOL cp1, cp2;
	PGLOBAL g = t->G;

	tp = new(g) TDBJMG(this);

	for (cp1 = (PJMGCOL)Columns; cp1; cp1 = (PJMGCOL)cp1->GetNext())
		if (!cp1->IsSpecial()) {
			cp2 = new(g) JMGCOL(cp1, tp);  // Make a copy
			NewPointer(t, cp1, cp2);
		} // endif cp1

	return tp;
} // end of Clone

/***********************************************************************/
/*  Allocate JSN column description block.                             */
/***********************************************************************/
PCOL TDBJMG::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
	return new(g) JMGCOL(g, cdp, this, cprec, n);
} // end of MakeCol

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBJMG::InsertSpecialColumn(PCOL colp)
{
	if (!colp->IsSpecial())
		return NULL;

	colp->SetNext(Columns);
	Columns = colp;
	return colp;
} // end of InsertSpecialColumn

/***********************************************************************/
/*  MONGO Cardinality: returns table size in number of rows.           */
/***********************************************************************/
int TDBJMG::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;
	else if (Cardinal < 0)
		Cardinal = (!Init(g)) ? Jcp->CollSize(g) : 0;

	return Cardinal;
} // end of Cardinality

/***********************************************************************/
/*  MONGO GetMaxSize: returns collection size estimate.                */
/***********************************************************************/
int TDBJMG::GetMaxSize(PGLOBAL g)
{
	if (MaxSize < 0)
		MaxSize = Cardinality(g);

	return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  Init: initialize MongoDB processing.                               */
/***********************************************************************/
bool TDBJMG::Init(PGLOBAL g)
{
	if (Done)
		return false;

	/*********************************************************************/
	/*  Open an JDBC connection for this table.                          */
	/*  Note: this may not be the proper way to do. Perhaps it is better */
	/*  to test whether a connection is already open for this datasource */
	/*  and if so to allocate just a new result set. But this only for   */
	/*  drivers allowing concurency in getting results ???               */
	/*********************************************************************/
	if (!Jcp)
		Jcp = new(g) JMgoConn(g, Coll_name, Wrapname);
	else if (Jcp->IsOpen())
		Jcp->Close();

	if (Jcp->Connect(&Ops))
		return true;

	Done = true;
	return false;
} // end of Init

/***********************************************************************/
/*  OpenDB: Data Base open routine for MONGO access method.            */
/***********************************************************************/
bool TDBJMG::OpenDB(PGLOBAL g)
{
	if (Use == USE_OPEN) {
		/*******************************************************************/
		/*  Table already open replace it at its beginning.                */
		/*******************************************************************/
		if (Jcp->Rewind())
			return true;

		Fpos = -1;
		return false;
	} // endif Use

	/*********************************************************************/
	/*  First opening.                                                   */
	/*********************************************************************/
	if (Pipe && Mode != MODE_READ) {
		strcpy(g->Message, "Pipeline tables are read only");
		return true;
	}	// endif Pipe

	Use = USE_OPEN;       // Do it now in case we are recursively called

	if (Init(g))
		return true;

	if (Jcp->GetMethodId(g, Mode))
		return true;

	if (Mode == MODE_DELETE && !Next) {
	// Delete all documents
		if (!Jcp->MakeCursor(g, this, "all", Filter, false))
			if (Jcp->DocDelete(g, true) == RC_OK)
				return false;

		return true;
	}	// endif Mode

	if (Mode == MODE_INSERT)
		Jcp->MakeColumnGroups(g, this);

	if (Mode != MODE_UPDATE)
		return Jcp->MakeCursor(g, this, Options, Filter, Pipe);

	return false;
} // end of OpenDB

/***********************************************************************/
/*  Data Base indexed read routine for ODBC access method.             */
/***********************************************************************/
bool TDBJMG::ReadKey(PGLOBAL g, OPVAL op, const key_range *kr)
{
	strcpy(g->Message, "MONGO tables are not indexable");
	return true;
} // end of ReadKey

/***********************************************************************/
/*  ReadDB: Get next document from a collection.                       */
/***********************************************************************/
int TDBJMG::ReadDB(PGLOBAL g)
{
	int rc = RC_OK;

	if (!N && Mode == MODE_UPDATE)
		if (Jcp->MakeCursor(g, this, Options, Filter, Pipe))
			return RC_FX;

	if (++CurNum >= Rbuf) {
		Rbuf = Jcp->Fetch();
		Curpos = Fpos + 1;
		CurNum = 0;
		N++;
	} // endif CurNum

	rc = (Rbuf > 0) ? RC_OK : (Rbuf == 0) ? RC_EF : RC_FX;

	return rc;
} // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for DOS access method.            */
/***********************************************************************/
int TDBJMG::WriteDB(PGLOBAL g)
{
	int rc = RC_OK;

	if (Mode == MODE_INSERT) {
		rc = Jcp->DocWrite(g, NULL);
	} else if (Mode == MODE_DELETE) {
		rc = Jcp->DocDelete(g, false);
	} else if (Mode == MODE_UPDATE) {
		rc = Jcp->DocUpdate(g, this);
	}	// endif Mode

	return rc;
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for ODBC access method.              */
/***********************************************************************/
int TDBJMG::DeleteDB(PGLOBAL g, int irc)
{
	return (irc == RC_OK) ? WriteDB(g) : RC_OK;
} // end of DeleteDB

/***********************************************************************/
/*  Table close routine for MONGO tables.                              */
/***********************************************************************/
void TDBJMG::CloseDB(PGLOBAL g)
{
	Jcp->Close();
	Done = false;
} // end of CloseDB

/* ----------------------------- JMGCOL ------------------------------ */

/***********************************************************************/
/*  JMGCOL public constructor.                                         */
/***********************************************************************/
JMGCOL::JMGCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
	: EXTCOL(cdp, tdbp, cprec, i, "MGO")
{
	Tmgp = (PTDBJMG)(tdbp->GetOrig() ? tdbp->GetOrig() : tdbp);
	Sgfy = Stringified(Tmgp->Strfy, Name);

	if ((Jpath = cdp->GetFmt())) {
		int n = strlen(Jpath);

		if (n && Jpath[n - 1] == '*') {
			Jpath = PlugDup(g, cdp->GetFmt());

			if (--n) {
				if (Jpath[n - 1] == '.') n--;
				Jpath[n] = 0;
			}	// endif n

			Sgfy = true;
		}	// endif Jpath

	}	else
		Jpath = cdp->GetName();

} // end of JMGCOL constructor

/***********************************************************************/
/*  JMGCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
JMGCOL::JMGCOL(JMGCOL *col1, PTDB tdbp) : EXTCOL(col1, tdbp)
{
	Tmgp = col1->Tmgp;
	Jpath = col1->Jpath;
	Sgfy = col1->Sgfy;
} // end of JMGCOL copy constructor

/***********************************************************************/
/*  Get path when proj is false or projection path when proj is true.  */
/***********************************************************************/
PSZ JMGCOL::GetJpath(PGLOBAL g, bool proj)
{
	if (Jpath) {
		if (proj) {
			char* p1, * p2, * projpath = PlugDup(g, Jpath);
			int   i = 0;

			for (p1 = p2 = projpath; *p1; p1++)
				if (*p1 == '.') {
					if (!i)
						*p2++ = *p1;

					i = 1;
				} else if (i) {
					if (!isdigit(*p1)) {
						*p2++ = *p1;
						i = 0;
					} // endif p1

				} else
					*p2++ = *p1;

			if (*(p2 - 1) == '.')
				p2--;

			*p2 = 0;
			return projpath;
		} else
			return Jpath;

	} else
		return Name;

} // end of GetJpath

#if 0
/***********************************************************************/
/*  Mini: used to suppress blanks to json strings.                     */
/***********************************************************************/
char *JMGCOL::Mini(PGLOBAL g, const bson_t *bson, bool b)
{
	char *s, *str = NULL;
	int   i, k = 0;
	bool  ok = true;

	if (b)
		s = str = bson_array_as_json(bson, NULL);
	else
		s = str = bson_as_json(bson, NULL);

	for (i = 0; i < Long && s[i]; i++) {
		switch (s[i]) {
			case ' ':
				if (ok) continue;
				break;
			case '"':
				ok = !ok;
			default:
				break;
		} // endswitch s[i]

		Mbuf[k++] = s[i];
	} // endfor i

	bson_free(str);

	if (i >= Long) {
		sprintf(g->Message, "Value too long for column %s", Name);
		throw (int)TYPE_AM_MGO;
	}	// endif i

	Mbuf[k] = 0;
	return Mbuf;
} // end of Mini
#endif // 0

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void JMGCOL::ReadColumn(PGLOBAL g)
{
	Value->SetValue_psz(Tmgp->Jcp->GetColumnValue(Jpath));
} // end of ReadColumn

/***********************************************************************/
/*  WriteColumn:                                                       */
/***********************************************************************/
void JMGCOL::WriteColumn(PGLOBAL g)
{
	// Check whether this node must be written
	if (Value != To_Val)
		Value->SetValue_pval(To_Val, FALSE);    // Convert the updated value

} // end of WriteColumn

#if 0
/***********************************************************************/
/*  AddValue: Add column value to the document to insert or update.    */
/***********************************************************************/
bool JMGCOL::AddValue(PGLOBAL g, bson_t *doc, char *key, bool upd)
{
	bool rc = false;

	if (Value->IsNull()) {
		if (upd)
			rc = BSON_APPEND_NULL(doc, key);
		else
			return false;

	} else switch (Buf_Type) {
		case TYPE_STRING:
			rc = BSON_APPEND_UTF8(doc, key, Value->GetCharValue());
			break;
		case TYPE_INT:
		case TYPE_SHORT:
			rc = BSON_APPEND_INT32(doc, key, Value->GetIntValue());
			break;
		case TYPE_TINY:
			rc = BSON_APPEND_BOOL(doc, key, Value->GetIntValue());
			break;
		case TYPE_BIGINT:
			rc = BSON_APPEND_INT64(doc, key, Value->GetBigintValue());
			break;
		case TYPE_DOUBLE:
			rc = BSON_APPEND_DOUBLE(doc, key, Value->GetFloatValue());
			break;
		case TYPE_DECIM:
		{bson_decimal128_t dec;

		if (bson_decimal128_from_string(Value->GetCharValue(), &dec))
			rc = BSON_APPEND_DECIMAL128(doc, key, &dec);

		} break;
		case TYPE_DATE:
			rc = BSON_APPEND_DATE_TIME(doc, key, Value->GetBigintValue() * 1000);
			break;
		default:
			sprintf(g->Message, "Type %d not supported yet", Buf_Type);
			return true;
	} // endswitch Buf_Type

	if (!rc) {
		strcpy(g->Message, "Adding value failed");
		return true;
	} else
		return false;

} // end of AddValue
#endif // 0

/* -------------------------- TDBJGL class --------------------------- */

/***********************************************************************/
/*  TDBJGL class constructor.                                          */
/***********************************************************************/
TDBJGL::TDBJGL(PMGODEF tdp) : TDBCAT(tdp)
{
	Topt = tdp->GetTopt();
	Uri = tdp->Uri;
	Db = tdp->GetTabschema();
} // end of TDBJCL constructor

/***********************************************************************/
/*  GetResult: Get the list the MongoDB collection columns.            */
/***********************************************************************/
PQRYRES TDBJGL::GetResult(PGLOBAL g)
{
	return MGOColumns(g, Db, Uri, Topt, false);
} // end of GetResult

/* -------------------------- End of mongo --------------------------- */
