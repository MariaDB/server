/************* tabjson C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: tabjson     Version 1.5                               */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2017  */
/*  This program are the JSON class DB execution routines.             */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  tdbdos.h    is header containing the TDBDOS declarations.          */
/*  json.h      is header containing the JSON classes declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
//#include "xtable.h"
#include "maputil.h"
#include "filamtxt.h"
#include "tabdos.h"
//#include "resource.h"                        // for IDS_COLUMNS
#include "tabjson.h"
#include "filamap.h"
#if defined(GZ_SUPPORT)
#include "filamgz.h"
#endif   // GZ_SUPPORT
#if defined(ZIP_SUPPORT)
#include "filamzip.h"
#endif   // ZIP_SUPPORT
#if defined(JAVA_SUPPORT)
#include "jmgfam.h"
#endif   // JAVA_SUPPORT
#if defined(CMGO_SUPPORT)
#include "cmgfam.h"
#endif   // CMGO_SUPPORT
#include "tabmul.h"
#include "checklvl.h"
#include "resource.h"
#include "mycat.h"                             // for FNC_COL

/***********************************************************************/
/*  This should be an option.                                          */
/***********************************************************************/
#define MAXCOL          200        /* Default max column nb in result  */
#define TYPE_UNKNOWN     12        /* Must be greater than other types */

/***********************************************************************/
/*  External functions.                                                */
/***********************************************************************/
USETEMP UseTemp(void);
char   *GetJsonNull(void);

//typedef struct _jncol {
//  struct _jncol *Next;
//  char *Name;
//  char *Fmt;
//  int   Type;
//  int   Len;
//  int   Scale;
//  bool  Cbn;
//  bool  Found;
//} JCOL, *PJCL;

/***********************************************************************/
/* JSONColumns: construct the result blocks containing the description */
/* of all the columns of a table contained inside a JSON file.         */
/***********************************************************************/
PQRYRES JSONColumns(PGLOBAL g, PCSZ db, PCSZ dsn, PTOS topt, bool info)
{
  static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING, TYPE_INT, 
                          TYPE_INT, TYPE_SHORT, TYPE_SHORT, TYPE_STRING};
  static XFLD fldtyp[] = {FLD_NAME, FLD_TYPE, FLD_TYPENAME, FLD_PREC, 
                          FLD_LENGTH, FLD_SCALE, FLD_NULL, FLD_FORMAT};
  static unsigned int length[] = {0, 6, 8, 10, 10, 6, 6, 0};
	int     i, n = 0;
  int     ncol = sizeof(buftyp) / sizeof(int);
	PJCL    jcp;
	JSONDISC *pjdc = NULL;
  PQRYRES qrp;
  PCOLRES crp;

  if (info) {
    length[0] = 128;
    length[7] = 256;
    goto skipit;
    } // endif info

	if (GetIntegerTableOption(g, topt, "Multiple", 0)) {
		strcpy(g->Message, "Cannot find column definition for multiple table");
		return NULL;
	}	// endif Multiple

	pjdc = new(g) JSONDISC(g, length);

	if (!(n = pjdc->GetColumns(g, db, dsn, topt)))
		return NULL;

 skipit:
  if (trace(1))
    htrc("JSONColumns: n=%d len=%d\n", n, length[0]);

  /*********************************************************************/
  /*  Allocate the structures used to refer to the result set.         */
  /*********************************************************************/
  qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
                          buftyp, fldtyp, length, false, false);

  crp = qrp->Colresp->Next->Next->Next->Next->Next->Next;
  crp->Name = "Nullable";
  crp->Next->Name = "Jpath";

  if (info || !qrp)
    return qrp;

  qrp->Nblin = n;

  /*********************************************************************/
  /*  Now get the results into blocks.                                 */
  /*********************************************************************/
  for (i = 0, jcp = pjdc->fjcp; jcp; i++, jcp = jcp->Next) {
		if (jcp->Type == TYPE_UNKNOWN)
			jcp->Type = TYPE_STRING;             // Void column

    crp = qrp->Colresp;                    // Column Name
    crp->Kdata->SetValue(jcp->Name, i);
    crp = crp->Next;                       // Data Type
    crp->Kdata->SetValue(jcp->Type, i);
    crp = crp->Next;                       // Type Name
    crp->Kdata->SetValue(GetTypeName(jcp->Type), i);
    crp = crp->Next;                       // Precision
    crp->Kdata->SetValue(jcp->Len, i);
    crp = crp->Next;                       // Length
    crp->Kdata->SetValue(jcp->Len, i);
    crp = crp->Next;                       // Scale (precision)
    crp->Kdata->SetValue(jcp->Scale, i);
    crp = crp->Next;                       // Nullable
    crp->Kdata->SetValue(jcp->Cbn ? 1 : 0, i);
    crp = crp->Next;                       // Field format

    if (crp->Kdata)
      crp->Kdata->SetValue(jcp->Fmt, i);

    } // endfor i

  /*********************************************************************/
  /*  Return the result pointer.                                       */
  /*********************************************************************/
  return qrp;
  } // end of JSONColumns

/* -------------------------- Class JSONDISC ------------------------- */

/***********************************************************************/
/*  Class used to get the columns of a JSON table.                     */
/***********************************************************************/
JSONDISC::JSONDISC(PGLOBAL g, uint *lg)
{
	length = lg;
	jcp = fjcp = pjcp = NULL;
	tjnp = NULL;
	jpp = NULL;
	tjsp = NULL;
	jsp = NULL;
	row = NULL;
	sep = NULL;
	i = n = bf = ncol = lvl = 0;
	all = false;
}	// end of JSONDISC constructor

int JSONDISC::GetColumns(PGLOBAL g, PCSZ db, PCSZ dsn, PTOS topt)
{
	bool mgo = (GetTypeID(topt->type) == TAB_MONGO);
	PCSZ level = GetStringTableOption(g, topt, "Level", NULL);

	if (level) {
		lvl = atoi(level);
		lvl = (lvl > 16) ? 16 : lvl;
	}	else
		lvl = 0;

	sep = GetStringTableOption(g, topt, "Separator", ".");

	/*********************************************************************/
	/*  Open the input file.                                             */
	/*********************************************************************/
	tdp = new(g) JSONDEF;
#if defined(ZIP_SUPPORT)
	tdp->Entry = GetStringTableOption(g, topt, "Entry", NULL);
	tdp->Zipped = GetBooleanTableOption(g, topt, "Zipped", false);
#endif   // ZIP_SUPPORT
	tdp->Fn = GetStringTableOption(g, topt, "Filename", NULL);

	if (!(tdp->Database = SetPath(g, db)))
		return 0;

	tdp->Objname = GetStringTableOption(g, topt, "Object", NULL);
	tdp->Base = GetIntegerTableOption(g, topt, "Base", 0) ? 1 : 0;
	tdp->Pretty = GetIntegerTableOption(g, topt, "Pretty", 2);
	tdp->Xcol = GetStringTableOption(g, topt, "Expand", NULL);
	tdp->Accept = GetBooleanTableOption(g, topt, "Accept", false);
	tdp->Uri = (dsn && *dsn ? dsn : NULL);

	if (!tdp->Fn && !tdp->Uri) {
		strcpy(g->Message, MSG(MISSING_FNAME));
		return 0;
	} // endif Fn

	if (trace(1))
		htrc("File %s objname=%s pretty=%d lvl=%d\n",
			tdp->Fn, tdp->Objname, tdp->Pretty, lvl);

	if (tdp->Uri) {
#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
		tdp->Collname = GetStringTableOption(g, topt, "Name", NULL);
		tdp->Collname = GetStringTableOption(g, topt, "Tabname", tdp->Collname);
		tdp->Schema = GetStringTableOption(g, topt, "Dbname", "test");
		tdp->Options = (PSZ)GetStringTableOption(g, topt, "Colist", "all");
		tdp->Pipe = GetBooleanTableOption(g, topt, "Pipeline", false);
		tdp->Driver = (PSZ)GetStringTableOption(g, topt, "Driver", NULL);
		tdp->Version = GetIntegerTableOption(g, topt, "Version", 3);
		tdp->Wrapname = (PSZ)GetStringTableOption(g, topt, "Wrapper",
			(tdp->Version == 2) ? "Mongo2Interface" : "Mongo3Interface");
		tdp->Pretty = 0;
#else   // !MONGO_SUPPORT
		sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
		return 0;
#endif  // !MONGO_SUPPORT
	}	// endif Uri

	if (tdp->Pretty == 2) {
		if (tdp->Zipped) {
#if defined(ZIP_SUPPORT)
			tjsp = new(g) TDBJSON(tdp, new(g) UNZFAM(tdp));
#else   // !ZIP_SUPPORT
			sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
			return 0;
#endif  // !ZIP_SUPPORT
		}	else
			tjsp = new(g) TDBJSON(tdp, new(g) MAPFAM(tdp));

		if (tjsp->MakeDocument(g))
			return 0;

		jsp = (tjsp->GetDoc()) ? tjsp->GetDoc()->GetValue(0) : NULL;
	}	else {
		if (!(tdp->Lrecl = GetIntegerTableOption(g, topt, "Lrecl", 0)))
			if (!mgo) {
				sprintf(g->Message, "LRECL must be specified for pretty=%d", tdp->Pretty);
				return 0;
			}	else
				tdp->Lrecl = 8192;			 // Should be enough

		tdp->Ending = GetIntegerTableOption(g, topt, "Ending", CRLF);

		if (tdp->Zipped) {
#if defined(ZIP_SUPPORT)
			tjnp = new(g)TDBJSN(tdp, new(g) UNZFAM(tdp));
#else   // !ZIP_SUPPORT
			sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
			return NULL;
#endif  // !ZIP_SUPPORT
		}	else if (tdp->Uri) {
			if (tdp->Driver && toupper(*tdp->Driver) == 'C') {
#if defined(CMGO_SUPPORT)
				tjnp = new(g) TDBJSN(tdp, new(g) CMGFAM(tdp));
#else
				sprintf(g->Message, "Mongo %s Driver not available", "C");
				return 0;
#endif
			}	else if (tdp->Driver && toupper(*tdp->Driver) == 'J') {
#if defined(JAVA_SUPPORT)
				tjnp = new(g) TDBJSN(tdp, new(g) JMGFAM(tdp));
#else
				sprintf(g->Message, "Mongo %s Driver not available", "Java");
				return 0;
#endif
			}	else {						 // Driver not specified
#if defined(CMGO_SUPPORT)
				tjnp = new(g) TDBJSN(tdp, new(g) CMGFAM(tdp));
#elif defined(JAVA_SUPPORT)
				tjnp = new(g) TDBJSN(tdp, new(g) JMGFAM(tdp));
#else
				sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
				return 0;
#endif
			}	// endif Driver

		}	else
			tjnp = new(g) TDBJSN(tdp, new(g) DOSFAM(tdp));

		tjnp->SetMode(MODE_READ);

		// Allocate the parse work memory
		PGLOBAL G = (PGLOBAL)PlugSubAlloc(g, NULL, sizeof(GLOBAL));
		memset(G, 0, sizeof(GLOBAL));
		G->Sarea_Size = tdp->Lrecl * 10;
		G->Sarea = PlugSubAlloc(g, NULL, G->Sarea_Size);
		PlugSubSet(G->Sarea, G->Sarea_Size);
		G->jump_level = 0;
		tjnp->SetG(G);

		if (tjnp->OpenDB(g))
			return 0;

		switch (tjnp->ReadDB(g)) {
		case RC_EF:
			strcpy(g->Message, "Void json table");
		case RC_FX:
			goto err;
		default:
			jsp = tjnp->GetRow();
		} // endswitch ReadDB

	} // endif pretty

	if (!(row = (jsp) ? jsp->GetObject() : NULL)) {
		strcpy(g->Message, "Can only retrieve columns from object rows");
		goto err;
	} // endif row

	all = GetBooleanTableOption(g, topt, "Fullarray", false);
	jcol.Name = jcol.Fmt = NULL;
	jcol.Next = NULL;
	jcol.Found = true;
	colname[0] = 0;

	if (!tdp->Uri) {
		fmt[0] = '$';
		fmt[1] = '.';
		bf = 2;
	}	// endif Uri

	/*********************************************************************/
	/*  Analyse the JSON tree and define columns.                        */
	/*********************************************************************/
	for (i = 1; ; i++) {
		for (jpp = row->GetFirst(); jpp; jpp = jpp->GetNext()) {
			strncpy(colname, jpp->GetKey(), 64);
			fmt[bf] = 0;

			if (Find(g, jpp->GetVal(), MY_MIN(lvl, 0)))
				goto err;

		} // endfor jpp

		// Missing column can be null
		for (jcp = fjcp; jcp; jcp = jcp->Next) {
			jcp->Cbn |= !jcp->Found;
			jcp->Found = false;
		} // endfor jcp

		if (tdp->Pretty != 2) {
			// Read next record
			switch (tjnp->ReadDB(g)) {
			case RC_EF:
				jsp = NULL;
				break;
			case RC_FX:
				goto err;
			default:
				jsp = tjnp->GetRow();
			} // endswitch ReadDB

		}	else
			jsp = tjsp->GetDoc()->GetValue(i);

		if (!(row = (jsp) ? jsp->GetObject() : NULL))
			break;

	} // endfor i

	if (tdp->Pretty != 2)
		tjnp->CloseDB(g);

	return n;

err:
	if (tdp->Pretty != 2)
		tjnp->CloseDB(g);

	return 0;
}	// end of GetColumns

bool JSONDISC::Find(PGLOBAL g, PJVAL jvp, int j)
{
	char *p, *pc = colname + strlen(colname);
	int   ars;
	PJOB  job;
	PJAR  jar;

	if ((valp = jvp ? jvp->GetValue() : NULL)) {
		jcol.Type = valp->GetType();
		jcol.Len = valp->GetValLen();
		jcol.Scale = valp->GetValPrec();
		jcol.Cbn = valp->IsNull();
	} else if (!jvp || jvp->IsNull()) {
		jcol.Type = TYPE_UNKNOWN;
		jcol.Len = jcol.Scale = 0;
		jcol.Cbn = true;
	} else  if (j < lvl) {
		if (!fmt[bf])
			strcat(fmt, colname);

		p = fmt + strlen(fmt);
		jsp = jvp->GetJson();

		switch (jsp->GetType()) {
			case TYPE_JOB:
				job = (PJOB)jsp;

				for (PJPR jrp = job->GetFirst(); jrp; jrp = jrp->GetNext()) {
					if (*jrp->GetKey() != '$') {
						strncat(strncat(fmt, sep, 128), jrp->GetKey(), 128);
						strncat(strncat(colname, "_", 64), jrp->GetKey(), 64);
					} // endif Key

					if (Find(g, jrp->GetVal(), j + 1))
						return true;

					*p = *pc = 0;
				} // endfor jrp

				return false;
			case TYPE_JAR:
				jar = (PJAR)jsp;

				if (all || (tdp->Xcol && !stricmp(tdp->Xcol, colname)))
					ars = jar->GetSize(false);
				else
					ars = MY_MIN(jar->GetSize(false), 1);

				for (int k = 0; k < ars; k++) {
					if (!tdp->Xcol || stricmp(tdp->Xcol, colname)) {
						sprintf(buf, "%d", k);

						if (tdp->Uri)
							strncat(strncat(fmt, sep, 128), buf, 128);
						else
							strncat(strncat(strncat(fmt, "[", 128), buf, 128), "]", 128);

						if (all)
							strncat(strncat(colname, "_", 64), buf, 64);

					} else
						strncat(fmt, (tdp->Uri ? sep : "[*]"), 128);

					if (Find(g, jar->GetValue(k), j))
						return true;

					*p = *pc = 0;
				} // endfor k

				return false;
			default:
				sprintf(g->Message, "Logical error after %s", fmt);
				return true;
		} // endswitch Type

	} else if (lvl >= 0) {
		jcol.Type = TYPE_STRING;
		jcol.Len = 256;
		jcol.Scale = 0;
		jcol.Cbn = true;
	} else
		return false;

	AddColumn(g);
	return false;
}	// end of Find

void JSONDISC::AddColumn(PGLOBAL g)
{
	bool b = fmt[bf] != 0;		 // True if formatted

	// Check whether this column was already found
	for (jcp = fjcp; jcp; jcp = jcp->Next)
		if (!strcmp(colname, jcp->Name))
			break;

	if (jcp) {
		if (jcp->Type != jcol.Type) {
			if (jcp->Type == TYPE_UNKNOWN)
				jcp->Type = jcol.Type;
			else if (jcol.Type != TYPE_UNKNOWN)
				jcp->Type = TYPE_STRING;

		} // endif Type

		if (b && (!jcp->Fmt || strlen(jcp->Fmt) < strlen(fmt))) {
			jcp->Fmt = PlugDup(g, fmt);
			length[7] = MY_MAX(length[7], strlen(fmt));
		} // endif fmt

		jcp->Len = MY_MAX(jcp->Len, jcol.Len);
		jcp->Scale = MY_MAX(jcp->Scale, jcol.Scale);
		jcp->Cbn |= jcol.Cbn;
		jcp->Found = true;
	} else if (jcol.Type != TYPE_UNKNOWN || tdp->Accept) {
		// New column
		jcp = (PJCL)PlugSubAlloc(g, NULL, sizeof(JCOL));
		*jcp = jcol;
		jcp->Cbn |= (i > 1);
		jcp->Name = PlugDup(g, colname);
		length[0] = MY_MAX(length[0], strlen(colname));

		if (b) {
			jcp->Fmt = PlugDup(g, fmt);
			length[7] = MY_MAX(length[7], strlen(fmt));
		} else
			jcp->Fmt = NULL;

		if (pjcp) {
			jcp->Next = pjcp->Next;
			pjcp->Next = jcp;
		} else
			fjcp = jcp;

		n++;
	} // endif jcp

	pjcp = jcp;
} // end of AddColumn


/* -------------------------- Class JSONDEF -------------------------- */

JSONDEF::JSONDEF(void)
{
  Jmode = MODE_OBJECT;
  Objname = NULL;
  Xcol = NULL;
  Pretty = 2;
  Limit = 1;
  Base = 0;
  Strict = false;
	Sep = '.';
	Uri = NULL;
	Collname = Options = Filter = NULL;
	Pipe = false;
	Driver = NULL;
	Version = 0;
	Wrapname = NULL;
} // end of JSONDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool JSONDEF::DefineAM(PGLOBAL g, LPCSTR, int poff)
{
	Schema = GetStringCatInfo(g, "DBname", Schema);
	Jmode = (JMODE)GetIntCatInfo("Jmode", MODE_OBJECT);
  Objname = GetStringCatInfo(g, "Object", NULL);
  Xcol = GetStringCatInfo(g, "Expand", NULL);
  Pretty = GetIntCatInfo("Pretty", 2);
  Limit = GetIntCatInfo("Limit", 10);
  Base = GetIntCatInfo("Base", 0) ? 1 : 0;
	Sep = *GetStringCatInfo(g, "Separator", ".");
	Accept = GetBoolCatInfo("Accept", false);

	if (Uri = GetStringCatInfo(g, "Connect", NULL)) {
#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
		Collname = GetStringCatInfo(g, "Name",
			(Catfunc & (FNC_TABLE | FNC_COL)) ? NULL : Name);
		Collname = GetStringCatInfo(g, "Tabname", Collname);
		Options = GetStringCatInfo(g, "Colist", NULL);
		Filter = GetStringCatInfo(g, "Filter", NULL);
		Pipe = GetBoolCatInfo("Pipeline", false);
		Driver = GetStringCatInfo(g, "Driver", NULL);
		Version = GetIntCatInfo("Version", 3);
		Pretty = 0;
#if defined(JAVA_SUPPORT)
		if (Version == 2)
			Wrapname = GetStringCatInfo(g, "Wrapper", "Mongo2Interface");
		else
			Wrapname = GetStringCatInfo(g, "Wrapper", "Mongo3Interface");
#endif   // JAVA_SUPPORT
#else   // !MONGO_SUPPORT
		sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
		return true;
#endif  // !MONGO_SUPPORT
	}	// endif Uri

	return DOSDEF::DefineAM(g, (Uri ? "XMGO" : "DOS"), poff);
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB JSONDEF::GetTable(PGLOBAL g, MODE m)
{
  if (Catfunc == FNC_COL)
    return new(g)TDBJCL(this);

  PTDBASE tdbp;
  PTXF    txfp = NULL;

  // JSN not used for pretty=1 for insert or delete
  if (!Pretty || (Pretty == 1 && (m == MODE_READ || m == MODE_UPDATE))) {
    USETEMP tmp = UseTemp();
    bool    map = Mapped && m != MODE_INSERT &&
                !(tmp != TMP_NO && m == MODE_UPDATE) &&
                !(tmp == TMP_FORCE &&
                (m == MODE_UPDATE || m == MODE_DELETE));

		if (Uri) {
			if (Driver && toupper(*Driver) == 'C') {
#if defined(CMGO_SUPPORT)
			txfp = new(g) CMGFAM(this);
#else
			sprintf(g->Message, "Mongo %s Driver not available", "C");
			return NULL;
#endif
			} else if (Driver && toupper(*Driver) == 'J') {
#if defined(JAVA_SUPPORT)
				txfp = new(g) JMGFAM(this);
#else
				sprintf(g->Message, "Mongo %s Driver not available", "Java");
				return NULL;
#endif
			} else {						 // Driver not specified
#if defined(CMGO_SUPPORT)
				txfp = new(g) CMGFAM(this);
#elif defined(JAVA_SUPPORT)
				txfp = new(g) JMGFAM(this);
#else		// !MONGO_SUPPORT
				sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
				return NULL;
#endif  // !MONGO_SUPPORT
			}	// endif Driver

		} else if (Zipped) {
#if defined(ZIP_SUPPORT)
			if (m == MODE_READ || m == MODE_ANY || m == MODE_ALTER) {
				txfp = new(g) UNZFAM(this);
			} else if (m == MODE_INSERT) {
				txfp = new(g) ZIPFAM(this);
			} else {
				strcpy(g->Message, "UPDATE/DELETE not supported for ZIP");
				return NULL;
			}	// endif's m
#else   // !ZIP_SUPPORT
			sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
			return NULL;
#endif  // !ZIP_SUPPORT
		} else if (Compressed) {
#if defined(GZ_SUPPORT)
      if (Compressed == 1)
        txfp = new(g) GZFAM(this);
      else
        txfp = new(g) ZLBFAM(this);
#else   // !GZ_SUPPORT
      sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "GZ");
      return NULL;
#endif  // !GZ_SUPPORT
		} else if (map)
      txfp = new(g) MAPFAM(this);
    else
      txfp = new(g) DOSFAM(this);

    // Txfp must be set for TDBDOS
    tdbp = new(g) TDBJSN(this, txfp);

		if (Lrecl) {
			// Allocate the parse work memory
			PGLOBAL G = (PGLOBAL)PlugSubAlloc(g, NULL, sizeof(GLOBAL));
			memset(G, 0, sizeof(GLOBAL));
			G->Sarea_Size = Lrecl * 10;
			G->Sarea = PlugSubAlloc(g, NULL, G->Sarea_Size);
			PlugSubSet(G->Sarea, G->Sarea_Size);
			G->jump_level = 0;
			((TDBJSN*)tdbp)->G = G;
		} else {
			strcpy(g->Message, "LRECL is not defined");
			return NULL;
		}	// endif Lrecl

	} else {
		if (Zipped)	{
#if defined(ZIP_SUPPORT)
			if (m == MODE_READ || m == MODE_ANY || m == MODE_ALTER) {
				txfp = new(g) UNZFAM(this);
			} else if (m == MODE_INSERT) {
				strcpy(g->Message, "INSERT supported only for zipped JSON when pretty=0");
				return NULL;
			} else {
				strcpy(g->Message, "UPDATE/DELETE not supported for ZIP");
				return NULL;
			}	// endif's m
#else   // !ZIP_SUPPORT
			sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
			return NULL;
#endif  // !ZIP_SUPPORT
		} else
			txfp = new(g) MAPFAM(this);

    tdbp = new(g) TDBJSON(this, txfp);
		((TDBJSON*)tdbp)->G = g;
  } // endif Pretty

  if (Multiple)
    tdbp = new(g) TDBMUL(tdbp);

  return tdbp;
} // end of GetTable

/* --------------------------- Class TDBJSN -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBJSN class.                                */
/***********************************************************************/
TDBJSN::TDBJSN(PJDEF tdp, PTXF txfp) : TDBDOS(tdp, txfp)
  {
  Top = NULL;
  Row = NULL;
  Val = NULL;
  Colp = NULL;

  if (tdp) {
    Jmode = tdp->Jmode;
    Objname = tdp->Objname;
    Xcol = tdp->Xcol;
    Limit = tdp->Limit;
    Pretty = tdp->Pretty;
    B = tdp->Base ? 1 : 0;
		Sep = tdp->Sep;
    Strict = tdp->Strict;
  } else {
    Jmode = MODE_OBJECT;
    Objname = NULL;
		Xcol = NULL;
    Limit = 1;
    Pretty = 0;
    B = 0;
		Sep = '.';
    Strict = false;
  } // endif tdp

  Fpos = -1;
  N = M = 0;
  NextSame = 0;
  SameRow = 0;
  Xval = -1;
  Comma = false;
  } // end of TDBJSN standard constructor

TDBJSN::TDBJSN(TDBJSN *tdbp) : TDBDOS(NULL, tdbp)
  {
	G = NULL;
  Top = tdbp->Top;
  Row = tdbp->Row;
  Val = tdbp->Val;
  Colp = tdbp->Colp;
  Jmode = tdbp->Jmode;
  Objname = tdbp->Objname;
  Xcol = tdbp->Xcol;
  Fpos = tdbp->Fpos;
  N = tdbp->N;
  M = tdbp->M;
  Limit = tdbp->Limit;
  NextSame = tdbp->NextSame;
  SameRow = tdbp->SameRow;
  Xval = tdbp->Xval;
  B = tdbp->B;
	Sep = tdbp->Sep;
  Pretty = tdbp->Pretty;
  Strict = tdbp->Strict;
  Comma = tdbp->Comma;
  } // end of TDBJSN copy constructor

// Used for update
PTDB TDBJSN::Clone(PTABS t)
  {
	G = NULL;
  PTDB    tp;
  PJCOL   cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBJSN(this);

  for (cp1 = (PJCOL)Columns; cp1; cp1 = (PJCOL)cp1->GetNext()) {
    cp2 = new(g) JSONCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

/***********************************************************************/
/*  Allocate JSN column description block.                             */
/***********************************************************************/
PCOL TDBJSN::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  PJCOL colp = new(g) JSONCOL(g, cdp, this, cprec, n);

  return (colp->ParseJpath(g)) ? NULL : colp;
  } // end of MakeCol

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBJSN::InsertSpecialColumn(PCOL colp)
  {
  if (!colp->IsSpecial())
    return NULL;

//if (Xcol && ((SPCBLK*)colp)->GetRnm())
//  colp->SetKey(0);               // Rownum is no more a key

  colp->SetNext(Columns);
  Columns = colp;
  return colp;
  } // end of InsertSpecialColumn

/***********************************************************************/
/*  JSON Cardinality: returns table size in number of rows.            */
/***********************************************************************/
int TDBJSN::Cardinality(PGLOBAL g)
  {
  if (!g)
    return 0;
  else if (Cardinal < 0)
    Cardinal = TDBDOS::Cardinality(g);

  return Cardinal;
  } // end of Cardinality

/***********************************************************************/
/*  JSON GetMaxSize: returns file size estimate in number of lines.    */
/***********************************************************************/
int TDBJSN::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0)
    MaxSize = TDBDOS::GetMaxSize(g) * ((Xcol) ? Limit : 1);

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  Find the row in the tree structure.                                */
/***********************************************************************/
PJSON TDBJSN::FindRow(PGLOBAL g)
{
  char *p, *objpath;
  PJSON jsp = Row;
  PJVAL val = NULL;

	for (objpath = PlugDup(g, Objname); jsp && objpath; objpath = p) {
		if ((p = strchr(objpath, Sep)))
			*p++ = 0;

		if (*objpath != '[' && !IsNum(objpath)) {	// objpass is a key
			val = (jsp->GetType() == TYPE_JOB) ?
				jsp->GetObject()->GetValue(objpath) : NULL;
		} else {
			if (*objpath == '[') {
				if (objpath[strlen(objpath) - 1] == ']')
					objpath++;
				else
					return NULL;
			} // endif [

			val = (jsp->GetType() == TYPE_JAR) ?
				jsp->GetArray()->GetValue(atoi(objpath) - B) : NULL;
		} // endif objpath

		jsp = (val) ? val->GetJson() : NULL;
	} // endfor objpath

  return jsp;
} // end of FindRow

/***********************************************************************/
/*  OpenDB: Data Base open routine for JSN access method.              */									 
/***********************************************************************/
bool TDBJSN::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open replace it at its beginning.                */
    /*******************************************************************/
    Fpos= -1;
    NextSame = 0;
    SameRow = 0;
  } else {
    /*******************************************************************/
    /*  First opening.                                                 */
    /*******************************************************************/
    if (Mode == MODE_INSERT)
      switch (Jmode) {
        case MODE_OBJECT: Row = new(g) JOBJECT; break;
        case MODE_ARRAY:  Row = new(g) JARRAY;  break;
        case MODE_VALUE:  Row = new(g) JVALUE;  break;
        default:
          sprintf(g->Message, "Invalid Jmode %d", Jmode);
          return true;
        } // endswitch Jmode

	} // endif Use

	if (TDBDOS::OpenDB(g))
		return true;

	if (Xcol)
		To_Filter = NULL;							 // Imcompatible

	return false;
} // end of OpenDB

/***********************************************************************/
/*  SkipHeader: Physically skip first header line if applicable.       */
/*  This is called from TDBDOS::OpenDB and must be executed before     */
/*  Kindex construction if the file is accessed using an index.        */
/***********************************************************************/
bool TDBJSN::SkipHeader(PGLOBAL g)
  {
  int  len = GetFileLength(g);
  bool rc = false;

#if defined(_DEBUG)
  if (len < 0)
    return true;
#endif   // _DEBUG

#if defined(__WIN__)
#define  Ending  2
#else   // !__WIN__
#define  Ending  1
#endif  // !__WIN__

  if (Pretty == 1) {
    if (Mode == MODE_INSERT || Mode == MODE_DELETE) {
      // Mode Insert and delete are no more handled here
      assert(false);
    } else if (len) // !Insert && !Delete
      rc = (Txfp->SkipRecord(g, false) == RC_FX || Txfp->RecordPos(g));

    } // endif Pretty

  return rc;
  } // end of SkipHeader

/***********************************************************************/
/*  ReadDB: Data Base read routine for JSN access method.              */
/***********************************************************************/
int TDBJSN::ReadDB(PGLOBAL g)
  {
  int   rc;

  N++;

  if (NextSame) {
    SameRow = NextSame;
    NextSame = 0;
    M++;
    return RC_OK;
	} else if ((rc = TDBDOS::ReadDB(g)) == RC_OK) {
		if (!IsRead() && ((rc = ReadBuffer(g)) != RC_OK))
			// Deferred reading failed
			return rc;

		// Recover the memory used for parsing
		PlugSubSet(G->Sarea, G->Sarea_Size);

		if ((Row = ParseJson(G, To_Line, strlen(To_Line), &Pretty, &Comma))) {
			Row = FindRow(g);
			SameRow = 0;
			Fpos++;
			M = 1;
			rc = RC_OK;
		} else if (Pretty != 1 || strcmp(To_Line, "]")) {
			strcpy(g->Message, G->Message);
			rc = RC_FX;
		} else
			rc = RC_EF;

	} // endif ReadDB

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  Make the top tree from the object path.                            */
/***********************************************************************/
int TDBJSN::MakeTopTree(PGLOBAL g, PJSON jsp)
{
	if (Objname) {
		if (!Val) {
			// Parse and allocate Objname item(s)
			char *p;
			char *objpath = PlugDup(g, Objname);
			int   i;
			PJOB  objp;
			PJAR  arp;
			PJVAL val = NULL;

			Top = NULL;

			for (; objpath; objpath = p) {
				if ((p = strchr(objpath, Sep)))
					*p++ = 0;

				if (*objpath != '[' && !IsNum(objpath)) {
					objp = new(g) JOBJECT;

					if (!Top)
						Top = objp;

					if (val)
						val->SetValue(objp);

					val = new(g) JVALUE;
					objp->SetValue(g, val, objpath);
				} else {
					if (*objpath == '[') {
						// Old style
						if (objpath[strlen(objpath) - 1] != ']') {
							sprintf(g->Message, "Invalid Table path %s", Objname);
							return RC_FX;
						} else
							objpath++;

					} // endif objpath

					arp = new(g) JARRAY;

					if (!Top)
						Top = arp;

					if (val)
						val->SetValue(arp);

					val = new(g) JVALUE;
					i = atoi(objpath) - B;
					arp->SetValue(g, val, i);
					arp->InitArray(g);
				} // endif objpath

			} // endfor p

			Val = val;
		} // endif Val

		Val->SetValue(jsp);
	} else
		Top = jsp;

	return RC_OK;
} // end of MakeTopTree

/***********************************************************************/
/*  PrepareWriting: Prepare the line for WriteDB.                      */
/***********************************************************************/
  bool TDBJSN::PrepareWriting(PGLOBAL g)
  {
  PSZ s;

  if (MakeTopTree(g, Row))
    return true;

  if ((s = Serialize(G, Top, NULL, Pretty))) {
    if (Comma)
      strcat(s, ",");

    if ((signed)strlen(s) > Lrecl) {
      strncpy(To_Line, s, Lrecl);
      sprintf(g->Message, "Line truncated (lrecl=%d)", Lrecl);
      return PushWarning(g, this);
    } else
      strcpy(To_Line, s);

    return false;
  } else
    return true;

  } // end of PrepareWriting

/***********************************************************************/
/*  WriteDB: Data Base write routine for JSON access method.           */
/***********************************************************************/
int TDBJSN::WriteDB(PGLOBAL g)
{
	int rc = TDBDOS::WriteDB(g);

	PlugSubSet(G->Sarea, G->Sarea_Size);
	Row->Clear();
	return rc;
} // end of WriteDB

/* ---------------------------- JSONCOL ------------------------------ */

/***********************************************************************/
/*  JSONCOL public constructor.                                        */
/***********************************************************************/
JSONCOL::JSONCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
       : DOSCOL(g, cdp, tdbp, cprec, i, "DOS")
{
  Tjp = (TDBJSN *)(tdbp->GetOrig() ? tdbp->GetOrig() : tdbp);
	G = Tjp->G;
  Jpath = cdp->GetFmt();
  MulVal = NULL;
  Nodes = NULL;
  Nod = 0;
	Sep = Tjp->Sep;
  Xnod = -1;
  Xpd = false;
  Parsed = false;
} // end of JSONCOL constructor

/***********************************************************************/
/*  JSONCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
JSONCOL::JSONCOL(JSONCOL *col1, PTDB tdbp) : DOSCOL(col1, tdbp)
{
	G = col1->G;
  Tjp = col1->Tjp;
  Jpath = col1->Jpath;
  MulVal = col1->MulVal;
  Nodes = col1->Nodes;
  Nod = col1->Nod;
	Sep = col1->Sep;
	Xnod = col1->Xnod;
  Xpd = col1->Xpd;
  Parsed = col1->Parsed;
} // end of JSONCOL copy constructor

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool JSONCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
  {
  if (DOSCOL::SetBuffer(g, value, ok, check))
    return true;

  // Parse the json path
  if (ParseJpath(g))
    return true;

  Tjp = (TDBJSN*)To_Tdb;
	G = Tjp->G;
  return false;
  } // end of SetBuffer

/***********************************************************************/
/*  Check whether this object is expanded.                             */
/***********************************************************************/
bool JSONCOL::CheckExpand(PGLOBAL g, int i, PSZ nm, bool b)
  {
  if ((Tjp->Xcol && nm && !strcmp(nm, Tjp->Xcol) &&
      (Tjp->Xval < 0 || Tjp->Xval == i)) || Xpd) {
    Xpd = true;              // Expandable object
    Nodes[i].Op = OP_EXP;
  } else if (b) {
    strcpy(g->Message, "Cannot expand more than one branch");
    return true;
  } // endif Xcol

  return false;
  } // end of CheckExpand

/***********************************************************************/
/*  Analyse array processing options.                                  */
/***********************************************************************/
bool JSONCOL::SetArrayOptions(PGLOBAL g, char *p, int i, PSZ nm)
{
	int    n;
	bool   dg = true, b = false;
	PJNODE jnp = &Nodes[i];

	//if (*p == '[') p++;		 // Old syntax .[	or :[
	n = (int)strlen(p);

	if (*p) {
		if (p[n - 1] == ']') {
			p[--n] = 0;
		} else if (!IsNum(p)) {
			// Wrong array specification
			sprintf(g->Message, "Invalid array specification %s for %s", p, Name);
			return true;
		} // endif p

	} else
		b = true;

	// To check whether a numeric Rank was specified
	dg = IsNum(p);

	if (!n) {
		// Default specifications
		if (CheckExpand(g, i, nm, false))
			return true;
		else if (jnp->Op != OP_EXP) {
			if (b) {
				// Return 1st value (B is the index base)
				jnp->Rank = Tjp->B;
				jnp->Op = OP_EQ;
			} else if (!Value->IsTypeNum()) {
				jnp->CncVal = AllocateValue(g, (void*)", ", TYPE_STRING);
				jnp->Op = OP_CNC;
			} else
				jnp->Op = OP_ADD;

		} // endif OP

	} else if (dg) {
		// Return nth value
		jnp->Rank = atoi(p) - Tjp->B;
		jnp->Op = OP_EQ;
	} else if (n == 1) {
		// Set the Op value;
		if (Sep == ':')
			switch (*p) {
				case '*': *p = 'x'; break;
				case 'x':
				case 'X': *p = '*'; break; // Expand this array
				default: break;
			} // endswitch p

		switch (*p) {
			case '+': jnp->Op = OP_ADD;  break;
			case 'x': jnp->Op = OP_MULT; break;
			case '>': jnp->Op = OP_MAX;  break;
			case '<': jnp->Op = OP_MIN;  break;
			case '!': jnp->Op = OP_SEP;  break; // Average
			case '#': jnp->Op = OP_NUM;  break;
			case '*': // Expand this array
				if (!Tjp->Xcol && nm) {
					Xpd = true;
					jnp->Op = OP_EXP;
					Tjp->Xval = i;
					Tjp->Xcol = nm;
				} else if (CheckExpand(g, i, nm, true))
					return true;

				break;
			default:
				sprintf(g->Message,
					"Invalid function specification %c for %s", *p, Name);
				return true;
		} // endswitch *p

	} else if (*p == '"' && p[n - 1] == '"') {
		// This is a concat specification
		jnp->Op = OP_CNC;

		if (n > 2) {
			// Set concat intermediate string
			p[n - 1] = 0;
			jnp->CncVal = AllocateValue(g, p + 1, TYPE_STRING);
		} // endif n

	} else {
		sprintf(g->Message, "Wrong array specification for %s", Name);
		return true;
	} // endif's

	// For calculated arrays, a local Value must be used
	switch (jnp->Op) {
		case OP_NUM:
			jnp->Valp = AllocateValue(g, TYPE_INT);
			break;
		case OP_ADD:
		case OP_MULT:
		case OP_SEP:
			if (!IsTypeChar(Buf_Type))
				jnp->Valp = AllocateValue(g, Buf_Type, 0, GetPrecision());
			else
				jnp->Valp = AllocateValue(g, TYPE_DOUBLE, 0, 2);

			break;
		case OP_MIN:
		case OP_MAX:
			jnp->Valp = AllocateValue(g, Buf_Type, Long, GetPrecision());
			break;
		case OP_CNC:
			if (IsTypeChar(Buf_Type))
				jnp->Valp = AllocateValue(g, TYPE_STRING, Long, GetPrecision());
			else
				jnp->Valp = AllocateValue(g, TYPE_STRING, 512);

			break;
		default:
			break;
	} // endswitch Op

	if (jnp->Valp)
		MulVal = AllocateValue(g, jnp->Valp);

	return false;
} // end of SetArrayOptions

/***********************************************************************/
/*  Parse the eventual passed Jpath information.                       */
/*  This information can be specified in the Fieldfmt column option    */
/*  when creating the table. It permits to indicate the position of    */
/*  the node corresponding to that column.                             */
/***********************************************************************/
bool JSONCOL::ParseJpath(PGLOBAL g)
{
	char *p, *p1 = NULL, *p2 = NULL, *pbuf = NULL;
	int   i;
	bool  a, mul = false;

	if (Parsed)
		return false;                       // Already done
	else if (InitValue(g))
		return true;
	else if (!Jpath)
		Jpath = Name;

	if (To_Tdb->GetOrig()) {
		// This is an updated column, get nodes from origin
		for (PJCOL colp = (PJCOL)Tjp->GetColumns(); colp;
			         colp = (PJCOL)colp->GetNext())
			if (!stricmp(Name, colp->GetName())) {
				Nod = colp->Nod;
				Nodes = colp->Nodes;
				Xpd = colp->Xpd;
				goto fin;
			} // endif Name

		sprintf(g->Message, "Cannot parse updated column %s", Name);
		return true;
	} // endif To_Orig

	pbuf = PlugDup(g, Jpath);
	if (*pbuf == '$') pbuf++;
	if (*pbuf == Sep) pbuf++;
	if (*pbuf == '[') p1 = pbuf++;

	// Estimate the required number of nodes
	for (i = 0, p = pbuf; (p = NextChr(p, Sep)); i++, p++)
		Nod++;                         // One path node found

	Nodes = (PJNODE)PlugSubAlloc(g, NULL, (++Nod) * sizeof(JNODE));
	memset(Nodes, 0, (Nod) * sizeof(JNODE));

	// Analyze the Jpath for this column
	for (i = 0, p = pbuf; p && i < Nod; i++, p = (p2 ? p2 : NULL)) {
		a = (p1 != NULL);
		p1 = strchr(p, '[');
		p2 = strchr(p, Sep);

		if (!p2)
			p2 = p1;
		else if (p1) {
			if (p1 < p2)
				p2 = p1;
			else if (p1 == p2 + 1)
				*p2++ = 0;		 // Old syntax .[	or :[
			else
				p1 = NULL;

		}	// endif p1

		if (p2)
			*p2++ = 0;

		// Jpath must be explicit
		if (a || *p == 0 || *p == '[' || IsNum(p)) {
			// Analyse intermediate array processing
			if (SetArrayOptions(g, p, i, Nodes[i - 1].Key))
				return true;

		} else if (*p == '*') {
			// Return JSON
			Nodes[i].Op = OP_XX;
		} else {
			Nodes[i].Key = p;
			Nodes[i].Op = OP_EXIST;
		} // endif's

	} // endfor i, p

	Nod = i;

fin:
	MulVal = AllocateValue(g, Value);
	Parsed = true;
	return false;
} // end of ParseJpath

/***********************************************************************/
/*  Get Jpath converted to Mongo path.                                 */
/***********************************************************************/
PSZ JSONCOL::GetJpath(PGLOBAL g, bool proj)
{
	if (Jpath) {
		char *p1, *p2, *mgopath;
		int   i = 0;

		if (strcmp(Jpath, "*")) {
			p1 = Jpath;
			if (*p1 == '$') p1++;
			if (*p1 == '.')	p1++;
			mgopath = PlugDup(g, p1);
		} else
			return NULL;

		for (p1 = p2 = mgopath; *p1; p1++)
			if (i) {								 // Inside []
				if (isdigit(*p1)) {
					if (!proj)
					  *p2++ = *p1;

				} else if (*p1 == ']' && i == 1) {
					if (proj && p1[1] == '.')
						p1++;

					i = 0;
				} else if (*p1 == '.' && i == 2) {
					if (!proj)
						*p2++ = '.';

					i = 0;
				} else if (!proj)
					return NULL;
			
			} else switch (*p1) {
				case ':':
				case '.':
					if (isdigit(p1[1]))
						i = 2;

					*p2++ = '.';
					break;
				case '[':
					if (*(p2 - 1) != '.')
						*p2++ = '.';

					i = 1;
					break;
				case '*':
					if (*(p2 - 1) == '.' && !*(p1 + 1)) {
						p2--;							 // Suppress last :*
						break;
					} // endif p2

				default:
					*p2++ = *p1;
					break;
		  } // endswitch p1;

			*p2 = 0;
			return mgopath;
	} else
		return NULL;

} // end of GetJpath

/***********************************************************************/
/*  MakeJson: Serialize the json item and set value to it.             */
/***********************************************************************/
PVAL JSONCOL::MakeJson(PGLOBAL g, PJSON jsp)
{
	if (Value->IsTypeNum()) {
    strcpy(g->Message, "Cannot make Json for a numeric column");
    Value->Reset();
  } else
    Value->SetValue_psz(Serialize(g, jsp, NULL, 0));

  return Value;
  } // end of MakeJson

/***********************************************************************/
/*  SetValue: Set a value from a JVALUE contains.                      */
/***********************************************************************/
void JSONCOL::SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val, int n)
  {
  if (val) {
		vp->SetNull(false);

    switch (val->GetValType()) {
      case TYPE_STRG:
      case TYPE_INTG:
			case TYPE_BINT:
			case TYPE_DBL:
			case TYPE_DTM:
				vp->SetValue_pval(val->GetValue());
        break;
      case TYPE_BOOL:
        if (vp->IsTypeNum())
          vp->SetValue(val->GetInteger() ? 1 : 0);
        else
          vp->SetValue_psz((PSZ)(val->GetInteger() ? "true" : "false"));
    
        break;
      case TYPE_JAR:
        SetJsonValue(g, vp, val->GetArray()->GetValue(0), n);
        break;
      case TYPE_JOB:
//      if (!vp->IsTypeNum() || !Strict) {
          vp->SetValue_psz(val->GetObject()->GetText(g, NULL));
          break;
//        } // endif Type
     
      default:
				vp->Reset();
				vp->SetNull(true);
		} // endswitch Type

	} else {
		vp->Reset();
		vp->SetNull(true);
	} // endif val

  } // end of SetJsonValue

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void JSONCOL::ReadColumn(PGLOBAL g)
  {
  if (!Tjp->SameRow || Xnod >= Tjp->SameRow)
    Value->SetValue_pval(GetColumnValue(g, Tjp->Row, 0));

	if (Xpd && Value->IsNull() && !((PJDEF)Tjp->To_Def)->Accept)
		throw("Null expandable JSON value");

  // Set null when applicable
  if (!Nullable)
    Value->SetNull(false);

  } // end of ReadColumn

/***********************************************************************/
/*  GetColumnValue:                                                    */
/***********************************************************************/
PVAL JSONCOL::GetColumnValue(PGLOBAL g, PJSON row, int i)
  {
  int   n = Nod - 1;
  bool  expd = false;
  PJAR  arp;
  PJVAL val = NULL;

  for (; i < Nod && row; i++) {
    if (Nodes[i].Op == OP_NUM) {
      Value->SetValue(row->GetType() == TYPE_JAR ? row->size() : 1);
      return(Value);
    } else if (Nodes[i].Op == OP_XX) {
      return MakeJson(G, row);
    } else switch (row->GetType()) {
      case TYPE_JOB:
        if (!Nodes[i].Key) {
          // Expected Array was not there, wrap the value
          if (i < Nod-1)
            continue;
          else
            val = new(G) JVALUE(row);

        } else
          val = ((PJOB)row)->GetValue(Nodes[i].Key);

        break;
      case TYPE_JAR:
        arp = (PJAR)row;

        if (!Nodes[i].Key) {
          if (Nodes[i].Op == OP_EQ)
            val = arp->GetValue(Nodes[i].Rank);
          else if (Nodes[i].Op == OP_EXP)
            return ExpandArray(g, arp, i);
          else
            return CalculateArray(g, arp, i);

				} else {
					// Unexpected array, unwrap it as [0]
					val = arp->GetValue(0);
					i--;
				}	// endif's

        break;
      case TYPE_JVAL:
        val = (PJVAL)row;
        break;
      default:
        sprintf(g->Message, "Invalid row JSON type %d", row->GetType());
        val = NULL;
      } // endswitch Type

    if (i < Nod-1)
      row = (val) ? val->GetJson() : NULL;

    } // endfor i

  SetJsonValue(g, Value, val, n);
  return Value;
  } // end of GetColumnValue

/***********************************************************************/
/*  ExpandArray:                                                       */
/***********************************************************************/
PVAL JSONCOL::ExpandArray(PGLOBAL g, PJAR arp, int n)
  {
  int    ars = MY_MIN(Tjp->Limit, arp->size());
  PJVAL  jvp;
  JVALUE jval;

	if (!ars) {
		Value->Reset();
		Value->SetNull(true);
		Tjp->NextSame = 0;
		return Value;
	} // endif ars

  if (!(jvp = arp->GetValue((Nodes[n].Rx = Nodes[n].Nx)))) {
    strcpy(g->Message, "Logical error expanding array");
		throw 666;
	} // endif jvp

  if (n < Nod - 1 && jvp->GetJson()) {
    jval.SetValue(GetColumnValue(g, jvp->GetJson(), n + 1));
    jvp = &jval;
    } // endif n

  if (n >= Tjp->NextSame) {
    if (++Nodes[n].Nx == ars) {
      Nodes[n].Nx = 0;
      Xnod = 0;
    } else
      Xnod = n;

    Tjp->NextSame = Xnod;
    } // endif NextSame 

  SetJsonValue(g, Value, jvp, n);
  return Value;
  } // end of ExpandArray

/***********************************************************************/
/*  CalculateArray:                                                    */
/***********************************************************************/
PVAL JSONCOL::CalculateArray(PGLOBAL g, PJAR arp, int n)
  {
	int    i, ars, nv = 0, nextsame = Tjp->NextSame;
	bool   err;
  OPVAL  op = Nodes[n].Op;
  PVAL   val[2], vp = Nodes[n].Valp;
  PJVAL  jvrp, jvp;
  JVALUE jval;

  vp->Reset();
	ars = MY_MIN(Tjp->Limit, arp->size());

	if (trace(1))
		htrc("CalculateArray: size=%d op=%d nextsame=%d\n",
			ars, op, nextsame);

	for (i = 0; i < ars; i++) {
		jvrp = arp->GetValue(i);

		if (trace(1))
			htrc("i=%d nv=%d\n", i, nv);

		if (!jvrp->IsNull() || (op == OP_CNC && GetJsonNull())) do {
			if (jvrp->IsNull()) {
				jvrp->Value = AllocateValue(g, GetJsonNull(), TYPE_STRING);
				jvp = jvrp;
			} else if (n < Nod - 1 && jvrp->GetJson()) {
        Tjp->NextSame = nextsame;
        jval.SetValue(GetColumnValue(g, jvrp->GetJson(), n + 1));
        jvp = &jval;
      } else
        jvp = jvrp;
  
			if (trace(1))
				htrc("jvp=%s null=%d\n",
					jvp->GetString(g), jvp->IsNull() ? 1 : 0);

			if (!nv++) {
        SetJsonValue(g, vp, jvp, n);
        continue;
      } else
        SetJsonValue(g, MulVal, jvp, n);

			if (!MulVal->IsNull()) {
				switch (op) {
          case OP_CNC:
            if (Nodes[n].CncVal) {
              val[0] = Nodes[n].CncVal;
              err = vp->Compute(g, val, 1, op);
              } // endif CncVal
  
            val[0] = MulVal;
            err = vp->Compute(g, val, 1, op);
						break;
//        case OP_NUM:
          case OP_SEP:
            val[0] = Nodes[n].Valp;
            val[1] = MulVal;
            err = vp->Compute(g, val, 2, OP_ADD);
            break;
          default:
            val[0] = Nodes[n].Valp;
            val[1] = MulVal;
            err = vp->Compute(g, val, 2, op);
          } // endswitch Op

        if (err)
          vp->Reset();
    
				if (trace(1)) {
					char buf(32);

					htrc("vp='%s' err=%d\n",
						vp->GetCharString(&buf), err ? 1 : 0);

				} // endif trace

			} // endif Null

      } while (Tjp->NextSame > nextsame);

    } // endfor i

  if (op == OP_SEP) {
    // Calculate average
    MulVal->SetValue(nv);
    val[0] = vp;
    val[1] = MulVal;

    if (vp->Compute(g, val, 2, OP_DIV))
      vp->Reset();

    } // endif Op

  Tjp->NextSame = nextsame;
  return vp;
  } // end of CalculateArray

/***********************************************************************/
/*  GetRow: Get the object containing this column.                     */
/***********************************************************************/
PJSON JSONCOL::GetRow(PGLOBAL g)
  {
  PJVAL val = NULL;
  PJAR  arp;
  PJSON nwr, row = Tjp->Row;

	for (int i = 0; i < Nod && row; i++) {
		if (Nodes[i+1].Op == OP_XX)
      break;
    else switch (row->GetType()) {
      case TYPE_JOB:
        if (!Nodes[i].Key)
          // Expected Array was not there, wrap the value
          continue;

        val = ((PJOB)row)->GetValue(Nodes[i].Key);
        break;
      case TYPE_JAR:
				arp = (PJAR)row;

				if (!Nodes[i].Key) {
          if (Nodes[i].Op == OP_EQ)
            val = arp->GetValue(Nodes[i].Rank);
          else
            val = arp->GetValue(Nodes[i].Rx);

        } else {
					// Unexpected array, unwrap it as [0]
					val = arp->GetValue(0);
					i--;
				} // endif Nodes

        break;
      case TYPE_JVAL:
        val = (PJVAL)row;
        break;
      default:
        sprintf(g->Message, "Invalid row JSON type %d", row->GetType());
        val = NULL;
      } // endswitch Type

    if (val) {
      row = val->GetJson();
    } else {
      // Construct missing objects
      for (i++; row && i < Nod; i++) {
        if (Nodes[i].Op == OP_XX)
          break;
        else if (!Nodes[i].Key)
          // Construct intermediate array
          nwr = new(G) JARRAY;
        else
          nwr = new(G) JOBJECT;

        if (row->GetType() == TYPE_JOB) {
          ((PJOB)row)->SetValue(G, new(G) JVALUE(nwr), Nodes[i-1].Key);
        } else if (row->GetType() == TYPE_JAR) {
          ((PJAR)row)->AddValue(G, new(G) JVALUE(nwr));
          ((PJAR)row)->InitArray(G);
        } else {
          strcpy(g->Message, "Wrong type when writing new row");
          nwr = NULL;
        } // endif's

        row = nwr;
        } // endfor i

      break;
    } // endelse

    } // endfor i

  return row;
  } // end of GetRow

/***********************************************************************/
/*  WriteColumn:                                                       */
/***********************************************************************/
void JSONCOL::WriteColumn(PGLOBAL g)
  {
	if (Xpd && Tjp->Pretty < 2) {
		strcpy(g->Message, "Cannot write expanded column when Pretty is not 2");
		throw 666;
	}	// endif Xpd

  /*********************************************************************/
  /*  Check whether this node must be written.                         */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, FALSE);    // Convert the updated value

  /*********************************************************************/
  /*  On INSERT Null values are represented by no node.                */
  /*********************************************************************/
  if (Value->IsNull() && Tjp->Mode == MODE_INSERT)
    return;

  char *s;
  PJOB  objp = NULL;
  PJAR  arp = NULL;
  PJVAL jvp = NULL;
  PJSON jsp, row = GetRow(g);

  switch (row->GetType()) {
    case TYPE_JOB:  objp = (PJOB)row;  break;
    case TYPE_JAR:  arp  = (PJAR)row;  break;
    case TYPE_JVAL: jvp  = (PJVAL)row; break;
    default: row = NULL;     // ???????????????????????????
    } // endswitch Type

  if (row) switch (Buf_Type) {
    case TYPE_STRING:
      if (Nodes[Nod-1].Op == OP_XX) {
        s = Value->GetCharValue();

        if (!(jsp = ParseJson(G, s, (int)strlen(s)))) {
          strcpy(g->Message, s);
					throw 666;
				} // endif jsp

        if (arp) {
          if (Nod > 1 && Nodes[Nod-2].Op == OP_EQ)
            arp->SetValue(G, new(G) JVALUE(jsp), Nodes[Nod-2].Rank);
          else
            arp->AddValue(G, new(G) JVALUE(jsp));

          arp->InitArray(G);
        } else if (objp) {
          if (Nod > 1 && Nodes[Nod-2].Key)
            objp->SetValue(G, new(G) JVALUE(jsp), Nodes[Nod-2].Key);

        } else if (jvp)
          jvp->SetValue(jsp);

        break;
        } // endif Op

      // fall through
    case TYPE_DATE:
    case TYPE_INT:
		case TYPE_TINY:
		case TYPE_SHORT:
		case TYPE_BIGINT:
		case TYPE_DOUBLE:
      if (arp) {
        if (Nodes[Nod-1].Op == OP_EQ)
          arp->SetValue(G, new(G) JVALUE(G, Value), Nodes[Nod-1].Rank);
        else
          arp->AddValue(G, new(G) JVALUE(G, Value));

        arp->InitArray(G);
      } else if (objp) {
        if (Nodes[Nod-1].Key)
          objp->SetValue(G, new(G) JVALUE(G, Value), Nodes[Nod-1].Key);

      } else if (jvp)
        jvp->SetValue(Value);

      break;
    default:                  // ??????????
      sprintf(g->Message, "Invalid column type %d", Buf_Type);
    } // endswitch Type

  } // end of WriteColumn

/* -------------------------- Class TDBJSON -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBJSON class.                               */
/***********************************************************************/
TDBJSON::TDBJSON(PJDEF tdp, PTXF txfp) : TDBJSN(tdp, txfp)
  {
  Doc = NULL;
  Multiple = tdp->Multiple;
  Done = Changed = false;
  } // end of TDBJSON standard constructor

TDBJSON::TDBJSON(PJTDB tdbp) : TDBJSN(tdbp)
  {
  Doc = tdbp->Doc;
  Multiple = tdbp->Multiple;
  Done = tdbp->Done;
  Changed = tdbp->Changed;
  } // end of TDBJSON copy constructor

// Used for update
PTDB TDBJSON::Clone(PTABS t)
  {
  PTDB    tp;
  PJCOL   cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBJSON(this);

  for (cp1 = (PJCOL)Columns; cp1; cp1 = (PJCOL)cp1->GetNext()) {
    cp2 = new(g) JSONCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

/***********************************************************************/
/*  Make the document tree from the object path.                       */
/***********************************************************************/
int TDBJSON::MakeNewDoc(PGLOBAL g)
  {
  // Create a void table that will be populated
  Doc = new(g) JARRAY;

  if (MakeTopTree(g, Doc))
    return RC_FX;

  Done = true;
  return RC_OK;
  } // end of MakeNewDoc

/***********************************************************************/
/*  Make the document tree from a file.                                */
/***********************************************************************/
int TDBJSON::MakeDocument(PGLOBAL g)
  {
  char   *p, *memory, *objpath, *key = NULL;
  int     len, i = 0;
  MODE    mode = Mode;
  PJSON   jsp;
  PJOB    objp = NULL;
  PJAR    arp = NULL;
  PJVAL   val = NULL;

  if (Done)
    return RC_OK;

  /*********************************************************************/
	/*  Create the mapping file object in mode read.                     */
  /*********************************************************************/
  Mode = MODE_READ;

  if (!Txfp->OpenTableFile(g)) {
    PFBLOCK fp = Txfp->GetTo_Fb();

    if (fp) {
      len = fp->Length;
      memory = fp->Memory;
    } else {
      Mode = mode;         // Restore saved Mode
      return MakeNewDoc(g);
    } // endif fp

  } else
    return RC_FX;

  /*********************************************************************/
  /*  Parse the json file and allocate its tree structure.             */
  /*********************************************************************/
  g->Message[0] = 0;
  jsp = Top = ParseJson(g, memory, len, &Pretty);
  Txfp->CloseTableFile(g, false);
  Mode = mode;             // Restore saved Mode

  if (!jsp && g->Message[0])
    return RC_FX;

	if ((objpath = PlugDup(g, Objname))) {
		if (*objpath == '$') objpath++;
		if (*objpath == '.') objpath++;

		/*********************************************************************/
		/*  Find the table in the tree structure.                            */
		/*********************************************************************/
		for (; jsp && objpath; objpath = p) {
			if ((p = strchr(objpath, Sep)))
				*p++ = 0;

			if (*objpath != '[' && !IsNum(objpath)) {
				// objpass is a key
				if (jsp->GetType() != TYPE_JOB) {
					strcpy(g->Message, "Table path does not match the json file");
					return RC_FX;
				} // endif Type

				key = objpath;
				objp = jsp->GetObject();
				arp = NULL;
				val = objp->GetValue(key);

				if (!val || !(jsp = val->GetJson())) {
					sprintf(g->Message, "Cannot find object key %s", key);
					return RC_FX;
				} // endif val

			} else {
				if (*objpath == '[') {
					// Old style
					if (objpath[strlen(objpath) - 1] != ']') {
						sprintf(g->Message, "Invalid Table path %s", Objname);
						return RC_FX;
					} else
						objpath++;

				} // endif objpath

				if (jsp->GetType() != TYPE_JAR) {
					strcpy(g->Message, "Table path does not match the json file");
					return RC_FX;
				} // endif Type

				arp = jsp->GetArray();
				objp = NULL;
				i = atoi(objpath) - B;
				val = arp->GetValue(i);

				if (!val) {
					sprintf(g->Message, "Cannot find array value %d", i);
					return RC_FX;
				} // endif val

			} // endif

			jsp = val->GetJson();
		} // endfor objpath

	}	// endif objpath

  if (jsp && jsp->GetType() == TYPE_JAR)
    Doc = jsp->GetArray();
  else { 
    // The table is void or is just one object or one value
    Doc = new(g) JARRAY;

    if (val) {
      Doc->AddValue(g, val);
      Doc->InitArray(g);
    } else if (jsp) {
      Doc->AddValue(g, new(g) JVALUE(jsp));
      Doc->InitArray(g);
    } // endif val

    if (objp)
      objp->SetValue(g, new(g) JVALUE(Doc), key);
    else if (arp)
      arp->SetValue(g, new(g) JVALUE(Doc), i);
    else
      Top = Doc;

  } // endif jsp

  Done = true;
  return RC_OK;
  } // end of MakeDocument

/***********************************************************************/
/*  JSON Cardinality: returns table size in number of rows.            */
/***********************************************************************/
int TDBJSON::Cardinality(PGLOBAL g)
  {
  if (!g)
    return (Xcol || Multiple) ? 0 : 1;
  else if (Cardinal < 0)
    if (!Multiple) {
      if (MakeDocument(g) == RC_OK)
        Cardinal = Doc->size();

    } else
      return 10;

  return Cardinal;
  } // end of Cardinality

/***********************************************************************/
/*  JSON GetMaxSize: returns table size estimate in number of rows.    */
/***********************************************************************/
int TDBJSON::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0)
    MaxSize = Cardinality(g) * ((Xcol) ? Limit : 1);

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  ResetSize: call by TDBMUL when calculating size estimate.          */
/***********************************************************************/
void TDBJSON::ResetSize(void)
  {
  MaxSize = Cardinal = -1;
  Fpos = -1;
  N = 0;
  Done = false;
  } // end of ResetSize

/***********************************************************************/
/*  TDBJSON is not indexable.                                          */
/***********************************************************************/
int TDBJSON::MakeIndex(PGLOBAL g, PIXDEF pxdf, bool)
  {
  if (pxdf) {
    strcpy(g->Message, "JSON not indexable when pretty = 2");
    return RC_FX;
  } else
    return RC_OK;

  } // end of MakeIndex 

/***********************************************************************/
/*  Return the position in the table.                                  */
/***********************************************************************/
int TDBJSON::GetRecpos(void)
  {
#if 0
  union {
    uint Rpos;
    BYTE Spos[4];
    };

  Rpos = htonl(Fpos);
  Spos[0] = (BYTE)NextSame;
  return Rpos;
#endif // 0
  return Fpos;
  } // end of GetRecpos

/***********************************************************************/
/*  Set the position in the table.                                  */
/***********************************************************************/
bool TDBJSON::SetRecpos(PGLOBAL, int recpos)
  {
#if 0
  union {
    uint Rpos;
    BYTE Spos[4];
    };

  Rpos = recpos;
  NextSame = Spos[0];
  Spos[0] = 0;
  Fpos = (signed)ntohl(Rpos);

//if (Fpos != (signed)ntohl(Rpos)) {
//  Fpos = ntohl(Rpos);
//  same = false;
//} else
//  same = true;
#endif // 0

  Fpos = recpos - 1;
  return false;
  } // end of SetRecpos

/***********************************************************************/
/*  JSON Access Method opening routine.                                */
/***********************************************************************/
bool TDBJSON::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open replace it at its beginning.                */
    /*******************************************************************/
    Fpos= -1;
    NextSame = false;
    SameRow = 0;
    return false;
    } // endif use

  /*********************************************************************/
  /*  OpenDB: initialize the JSON file processing.                     */
  /*********************************************************************/
  if (MakeDocument(g) != RC_OK)
    return true;

  if (Mode == MODE_INSERT)
    switch (Jmode) {
      case MODE_OBJECT: Row = new(g) JOBJECT; break;
      case MODE_ARRAY:  Row = new(g) JARRAY;  break;
      case MODE_VALUE:  Row = new(g) JVALUE;  break;
      default:
        sprintf(g->Message, "Invalid Jmode %d", Jmode);
        return true;
      } // endswitch Jmode

	if (Xcol)
		To_Filter = NULL;							 // Imcompatible

	Use = USE_OPEN;
	return false;
  } // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for JSON access method.             */
/***********************************************************************/
int TDBJSON::ReadDB(PGLOBAL)
  {
  int rc;

	N++;

  if (NextSame) {
    SameRow = NextSame;
    NextSame = false;
    M++;
    rc = RC_OK;
  } else if (++Fpos < (signed)Doc->size()) {
    Row = Doc->GetValue(Fpos);

    if (Row->GetType() == TYPE_JVAL)
      Row = ((PJVAL)Row)->GetJson();

    SameRow = 0;
    M = 1;
    rc = RC_OK;
  } else
    rc = RC_EF;

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for JSON access method.           */
/***********************************************************************/
int TDBJSON::WriteDB(PGLOBAL g)
  {
  if (Jmode == MODE_OBJECT) {
    PJVAL vp = new(g) JVALUE(Row);

    if (Mode == MODE_INSERT) {
      Doc->AddValue(g, vp);
      Row = new(g) JOBJECT;
    } else if (Doc->SetValue(g, vp, Fpos))
      return RC_FX;

  } else if (Jmode == MODE_ARRAY) {
    PJVAL vp = new(g) JVALUE(Row);

    if (Mode == MODE_INSERT) {
      Doc->AddValue(g, vp);
      Row = new(g) JARRAY;
    } else if (Doc->SetValue(g, vp, Fpos))
      return RC_FX;

  } else { // if (Jmode == MODE_VALUE)
    if (Mode == MODE_INSERT) {
      Doc->AddValue(g, (PJVAL)Row);
      Row = new(g) JVALUE;
    } else if (Doc->SetValue(g, (PJVAL)Row, Fpos))
      return RC_FX;

  } // endif Jmode

  Changed = true;
  return RC_OK;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for JSON access method.              */
/***********************************************************************/
int TDBJSON::DeleteDB(PGLOBAL g, int irc)
  {
  if (irc == RC_OK) {
    // Deleted current row
    if (Doc->DeleteValue(Fpos)) {
      sprintf(g->Message, "Value %d does not exist", Fpos + 1);
      return RC_FX;
      } // endif Delete

    Changed = true;
  } else if (irc == RC_FX)
    // Delete all
    for (int i = 0; i < Doc->size(); i++) {
      Doc->DeleteValue(i);
      Changed = true;
      } // endfor i

  return RC_OK;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for JSON access methods.                   */
/***********************************************************************/
void TDBJSON::CloseDB(PGLOBAL g)
  {
  if (!Changed)
    return;

  // Save the modified document
  char filename[_MAX_PATH];

  Doc->InitArray(g);

  // We used the file name relative to recorded datapath
  PlugSetPath(filename, ((PJDEF)To_Def)->Fn, GetPath());

  // Serialize the modified table
  if (!Serialize(g, Top, filename, Pretty))
    puts(g->Message);

  } // end of CloseDB

/* ---------------------------TDBJCL class --------------------------- */

/***********************************************************************/
/*  TDBJCL class constructor.                                          */
/***********************************************************************/
TDBJCL::TDBJCL(PJDEF tdp) : TDBCAT(tdp)
  {
  Topt = tdp->GetTopt();
  Db = tdp->Schema;
	Dsn = tdp->Uri;
  } // end of TDBJCL constructor

/***********************************************************************/
/*  GetResult: Get the list the JSON file columns.                     */
/***********************************************************************/
PQRYRES TDBJCL::GetResult(PGLOBAL g)
  {
  return JSONColumns(g, Db, Dsn, Topt, false);
  } // end of GetResult

/* --------------------------- End of json --------------------------- */
