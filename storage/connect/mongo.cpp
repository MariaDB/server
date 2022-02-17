/************** mongo C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: mongo     Version 1.1                                 */
/*  (C) Copyright to the author Olivier BERTRAND          2021         */
/*  These programs are the MGODEF class execution routines.            */
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
#include "tabext.h"
#include "filter.h"
#if defined(CMGO_SUPPORT)
#include "tabcmg.h"
#endif   // CMGO_SUPPORT
#if defined(JAVA_SUPPORT)
#include "tabjmg.h"
#endif   // JAVA_SUPPORT
#include "resource.h"

/***********************************************************************/
/*  This should be an option.                                          */
/***********************************************************************/
#define MAXCOL          200        /* Default max column nb in result  */
#define TYPE_UNKNOWN     12        /* Must be greater than other types */

bool MakeSelector(PGLOBAL g, PFIL fp, PSTRG s);
bool IsNum(PSZ s);
int  GetDefaultDepth(void);
bool JsonAllPath(void);

/***********************************************************************/
/*  Make selector json representation for Mongo tables.                */
/***********************************************************************/
bool MakeSelector(PGLOBAL g, PFIL fp, PSTRG s)
{
	OPVAL opc = fp->GetOpc();

	s->Append('{');

	if (opc == OP_AND || opc == OP_OR) {
		if (fp->GetArgType(0) != TYPE_FILTER || fp->GetArgType(1) != TYPE_FILTER)
			return true;

		s->Append("\"$");
		s->Append(opc == OP_AND ? "and" : "or");
		s->Append("\":[");

		if (MakeSelector(g, (PFIL)fp->Arg(0), s))
			return true;

		s->Append(',');

		if (MakeSelector(g, (PFIL)fp->Arg(1), s))
			return true;

		s->Append(']');
	} else {
		if (fp->GetArgType(0) != TYPE_COLBLK)
			return true;

		s->Append('"');
		s->Append(((PCOL)fp->Arg(0))->GetJpath(g, false));
		s->Append("\":{\"$");

		switch (opc) {
			case OP_EQ:
				s->Append("eq");
				break;
			case OP_NE:
				s->Append("ne");
				break;
			case OP_GT:
				s->Append("gt");
				break;
			case OP_GE:
				s->Append("gte");
				break;
			case OP_LT:
				s->Append("lt");
				break;
			case OP_LE:
				s->Append("lte");
				break;
			case OP_NULL:
			case OP_LIKE:
			case OP_EXIST:
			default:
				return true;
		} // endswitch Opc

		s->Append("\":");

		if (fp->GetArgType(1) == TYPE_COLBLK) {
			s->Append("\"$");
			s->Append(((PEXTCOL)fp->Arg(1))->GetJpath(g, false));
			s->Append('"');
		} else {
			char buf[501];

			fp->Arg(1)->Prints(g, buf, 500);
			s->Append(buf);
		} // endif Type

		s->Append('}');
	} // endif opc

	s->Append('}');
	return false;
} // end of MakeSelector

/***********************************************************************/
/*  MGOColumns: construct the result blocks containing the description */
/*  of all the columns of a document contained inside MongoDB.         */
/***********************************************************************/
PQRYRES MGOColumns(PGLOBAL g, PCSZ db, PCSZ uri, PTOS topt, bool info)
{
	static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING, TYPE_INT,
													TYPE_INT, TYPE_SHORT, TYPE_SHORT, TYPE_STRING};
	static XFLD fldtyp[] = {FLD_NAME, FLD_TYPE, FLD_TYPENAME, FLD_PREC,
													FLD_LENGTH, FLD_SCALE, FLD_NULL, FLD_FORMAT};
	unsigned int length[] = {0, 6, 8, 10, 10, 6, 6, 0};
	int      ncol = sizeof(buftyp) / sizeof(int);
	int      i, n = 0;
	PCSZ     drv;
	PBCOL    bcp;
	MGODISC *cmgd = NULL;
	PQRYRES  qrp;
	PCOLRES  crp;

	if (info) {
		length[0] = 128;
		length[7] = 256;
		goto skipit;
	} // endif info

	/*********************************************************************/
	/*  Open MongoDB.                                                    */
	/*********************************************************************/
	drv = GetStringTableOption(g, topt, "Driver", NULL);

	if (drv && toupper(*drv) == 'C') {
#if defined(CMGO_SUPPORT)
		cmgd = new(g) CMGDISC(g, (int*)length);
#else
		sprintf(g->Message, "Mongo %s Driver not available", "C");
		goto err;
#endif
	} else if (drv && toupper(*drv) == 'J') {
#if defined(JAVA_SUPPORT)
		cmgd = new(g) JMGDISC(g, (int*)length);
#else
		sprintf(g->Message, "Mongo %s Driver not available", "Java");
		goto err;
#endif
	} else {						 // Driver not specified
#if defined(CMGO_SUPPORT)
		cmgd = new(g) CMGDISC(g, (int*)length);
#else
		cmgd = new(g) JMGDISC(g, (int*)length);
#endif
	}	// endif drv

	if ((n = cmgd->GetColumns(g, db, uri, topt)) < 0)
		goto err;

skipit:
	if (trace(1))
		htrc("MGOColumns: n=%d len=%d\n", n, length[0]);

	/*********************************************************************/
	/*  Allocate the structures used to refer to the result set.         */
	/*********************************************************************/
	qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
		buftyp, fldtyp, length, false, false);

	crp = qrp->Colresp->Next->Next->Next->Next->Next->Next;
	crp->Name = "Nullable";
	crp->Next->Name = "Bpath";

	if (info || !qrp)
		return qrp;

	qrp->Nblin = n;

	/*********************************************************************/
	/*  Now get the results into blocks.                                 */
	/*********************************************************************/
	for (i = 0, bcp = cmgd->fbcp; bcp; i++, bcp = bcp->Next) {
		if (bcp->Type == TYPE_UNKNOWN)            // Void column
			bcp->Type = TYPE_STRING;

		crp = qrp->Colresp;                    // Column Name
		crp->Kdata->SetValue(bcp->Name, i);
		crp = crp->Next;                       // Data Type
		crp->Kdata->SetValue(bcp->Type, i);
		crp = crp->Next;                       // Type Name
		crp->Kdata->SetValue(GetTypeName(bcp->Type), i);
		crp = crp->Next;                       // Precision
		crp->Kdata->SetValue(bcp->Len, i);
		crp = crp->Next;                       // Length
		crp->Kdata->SetValue(bcp->Len, i);
		crp = crp->Next;                       // Scale (precision)
		crp->Kdata->SetValue(bcp->Scale, i);
		crp = crp->Next;                       // Nullable
		crp->Kdata->SetValue(bcp->Cbn ? 1 : 0, i);
		crp = crp->Next;                       // Field format

		if (crp->Kdata)
			crp->Kdata->SetValue(bcp->Fmt, i);

	} // endfor i

	/*********************************************************************/
	/*  Return the result pointer.                                       */
	/*********************************************************************/
	return qrp;

err:
	if (cmgd && cmgd->tmgp)
		cmgd->tmgp->CloseDB(g);

	return NULL;
} // end of MGOColumns

/***********************************************************************/
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
MGODISC::MGODISC(PGLOBAL g, int *lg) {
	length = lg;
	fbcp = NULL;
	pbcp = NULL;
	tmgp = NULL;
	drv = NULL;
	i = ncol = lvl = 0;
	all = false;
}	// end of MGODISC constructor

/***********************************************************************/
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
int MGODISC::GetColumns(PGLOBAL g, PCSZ db, PCSZ uri, PTOS topt)
{
	PMGODEF tdp;

	lvl = GetIntegerTableOption(g, topt, "Level", GetDefaultDepth());
	lvl = GetIntegerTableOption(g, topt, "Depth", lvl);
	all = GetBooleanTableOption(g, topt, "Fullarray", false);

	/*********************************************************************/
	/*  Open the MongoDB collection.                                     */
	/*********************************************************************/
	tdp = new(g) MGODEF;
	tdp->Uri = (uri && *uri) ? uri : "mongodb://localhost:27017";
	tdp->Driver = drv;
	tdp->Tabname = GetStringTableOption(g, topt, "Name", NULL);
	tdp->Tabname = GetStringTableOption(g, topt, "Tabname", tdp->Tabname);
	tdp->Tabschema = GetStringTableOption(g, topt, "Dbname", db);
	tdp->Base = GetIntegerTableOption(g, topt, "Base", 0) ? 1 : 0;
	tdp->Colist = GetStringTableOption(g, topt, "Colist", "all");
	tdp->Filter = GetStringTableOption(g, topt, "Filter", NULL);
	tdp->Pipe = GetBooleanTableOption(g, topt, "Pipeline", false);
	tdp->Version = GetIntegerTableOption(g, topt, "Version", 3);
	tdp->Wrapname = (PSZ)GetStringTableOption(g, topt, "Wrapper",
		(tdp->Version == 2) ? "Mongo2Interface" : "Mongo3Interface");

	if (trace(1))
		htrc("Uri %s coll=%s db=%s colist=%s filter=%s lvl=%d\n",
			tdp->Uri, tdp->Tabname, tdp->Tabschema, tdp->Colist, tdp->Filter, lvl);

	tmgp = tdp->GetTable(g, MODE_READ);
	tmgp->SetMode(MODE_READ);

	if (tmgp->OpenDB(g))
		return -1;

	bcol.Next = NULL;
	bcol.Name = bcol.Fmt = NULL;
	bcol.Type = TYPE_UNKNOWN;
	bcol.Len = bcol.Scale = 0;
	bcol.Found = true;
	bcol.Cbn = false;

	if (Init(g))
		return -1;

	/*********************************************************************/
	/*  Analyse the BSON tree and define columns.                        */
	/*********************************************************************/
	for (i = 1; ; i++) {
		switch (tmgp->ReadDB(g)) {
			case RC_EF:
				return ncol;
			case RC_FX:
				return -1;
			default:
				GetDoc();
		} // endswitch ReadDB

		if (Find(g))
			return -1;

		// Missing columns can be null
		for (bcp = fbcp; bcp; bcp = bcp->Next) {
			bcp->Cbn |= !bcp->Found;
			bcp->Found = false;
		} // endfor bcp

	} // endfor i

	return ncol;
} // end of GetColumns

/***********************************************************************/
/*  Add a new column in the column list.                               */
/***********************************************************************/
void MGODISC::AddColumn(PGLOBAL g, PCSZ colname, PCSZ fmt, int k)
{
	// Check whether this column was already found
	for (bcp = fbcp; bcp; bcp = bcp->Next)
		if (!strcmp(colname, bcp->Name))
			break;

	if (bcp) {
		if (bcp->Type != bcol.Type)
			bcp->Type = TYPE_STRING;

		if (k && *fmt && (!bcp->Fmt || strlen(bcp->Fmt) < strlen(fmt))) {
			bcp->Fmt = PlugDup(g, fmt);
			length[7] = MY_MAX(length[7], (signed)strlen(fmt));
		} // endif *fmt

		bcp->Len = MY_MAX(bcp->Len, bcol.Len);
		bcp->Scale = MY_MAX(bcp->Scale, bcol.Scale);
		bcp->Cbn |= bcol.Cbn;
		bcp->Found = true;
	} else {
		// New column
		bcp = (PBCOL)PlugSubAlloc(g, NULL, sizeof(BCOL));
		*bcp = bcol;
		bcp->Cbn |= (i > 1);
		bcp->Name = PlugDup(g, colname);
		length[0] = MY_MAX(length[0], (signed)strlen(colname));

		if (k || JsonAllPath()) {
			bcp->Fmt = PlugDup(g, fmt);
			length[7] = MY_MAX(length[7], (signed)strlen(fmt));
		} else
			bcp->Fmt = NULL;

		if (pbcp) {
			bcp->Next = pbcp->Next;
			pbcp->Next = bcp;
		} else
			fbcp = bcp;

		ncol++;
	} // endif jcp

	pbcp = bcp;
} // end of AddColumn

/* -------------------------- Class MGODEF --------------------------- */

MGODEF::MGODEF(void)
{
	Driver = NULL;
	Uri = NULL;
	Colist = NULL;
	Filter = NULL;
	Base = 0;
	Version = 0;
	Pipe = false;
} // end of MGODEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool MGODEF::DefineAM(PGLOBAL g, LPCSTR, int poff)
{
	if (EXTDEF::DefineAM(g, "MGO", poff))
		return true;
	else if (!Tabschema)
		Tabschema = GetStringCatInfo(g, "Dbname", "*");

	Driver = GetStringCatInfo(g, "Driver", NULL);
	Uri = GetStringCatInfo(g, "Connect", "mongodb://localhost:27017");
	Colist = GetStringCatInfo(g, "Colist", NULL);
	Filter = GetStringCatInfo(g, "Filter", NULL);
	Strfy = GetStringCatInfo(g, "Stringify", NULL);
	Base = GetIntCatInfo("Base", 0) ? 1 : 0;
	Version = GetIntCatInfo("Version", 3);

	if (Version == 2)
		Wrapname = GetStringCatInfo(g, "Wrapper", "Mongo2Interface");
	else
		Wrapname = GetStringCatInfo(g, "Wrapper", "Mongo3Interface");

	Pipe = GetBoolCatInfo("Pipeline", false);
	return false;
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB MGODEF::GetTable(PGLOBAL g, MODE m)
{
	if (Driver && toupper(*Driver) == 'C') {
#if defined(CMGO_SUPPORT)
		if (Catfunc == FNC_COL)
			return new(g) TDBGOL(this);
		else
			return new(g) TDBCMG(this);
#else
		sprintf(g->Message, "Mongo %s Driver not available", "C");
		return NULL;
#endif
	} else if (Driver && toupper(*Driver) == 'J') {
#if defined(JAVA_SUPPORT)
		if (Catfunc == FNC_COL)
			return new(g) TDBJGL(this);
		else
			return new(g) TDBJMG(this);
#else
		sprintf(g->Message, "Mongo %s Driver not available", "Java");
		return NULL;
#endif
	} else {						 // Driver not specified
#if defined(CMGO_SUPPORT)
		if (Catfunc == FNC_COL)
			return new(g) TDBGOL(this);
		else
			return new(g) TDBCMG(this);
#else
		if (Catfunc == FNC_COL)
			return new(g) TDBJGL(this);
		else
			return new(g) TDBJMG(this);
#endif
	} // endif Driver

} // end of GetTable
