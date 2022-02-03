/************* tabjson C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: tabjson     Version 1.9                               */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2021  */
/*  This program are the JSON class DB execution routines.             */
/***********************************************************************/
#undef BSON_SUPPORT

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>
#include <mysqld.h>
#include <sql_error.h>

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
//#define TYPE_UNKNOWN     12        /* Must be greater than other types */

/***********************************************************************/
/*  External functions.                                                */
/***********************************************************************/
USETEMP UseTemp(void);
bool    JsonAllPath(void);
int     GetDefaultDepth(void);
char   *GetJsonNull(void);
bool    Stringified(PCSZ, char*);

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
  } // endif Multiple

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
  crp->Name = PlugDup(g, "Nullable");
  crp->Next->Name = PlugDup(g, "Jpath");

  if (info || !qrp)
    return qrp;

  qrp->Nblin = n;

  /*********************************************************************/
  /*  Now get the results into blocks.                                 */
  /*********************************************************************/
  for (i = 0, jcp = pjdc->fjcp; jcp; i++, jcp = jcp->Next) {
    if (jcp->Type == TYPE_UNKNOWN)
      jcp->Type = TYPE_STRG;               // Void column

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
  tdp = NULL;
  tjnp = NULL;
  jpp = NULL;
  tjsp = NULL;
  jsp = NULL;
  row = NULL;
  sep = NULL;
  strfy = NULL;
  i = n = bf = ncol = lvl = sz = limit = 0;
  all = false;
} // end of JSONDISC constructor

int JSONDISC::GetColumns(PGLOBAL g, PCSZ db, PCSZ dsn, PTOS topt)
{
  char    filename[_MAX_PATH];
  size_t  reclg = 0;
  bool    mgo = (GetTypeID(topt->type) == TAB_MONGO);
  PGLOBAL G = NULL;

	lvl = GetIntegerTableOption(g, topt, "Level", GetDefaultDepth());
	lvl = GetIntegerTableOption(g, topt, "Depth", lvl);
	sep = GetStringTableOption(g, topt, "Separator", ".");
  strfy = GetStringTableOption(g, topt, "Stringify", NULL);
  sz = GetIntegerTableOption(g, topt, "Jsize", 1024);
  limit = GetIntegerTableOption(g, topt, "Limit", 50);

  /*********************************************************************/
  /*  Open the input file.                                             */
  /*********************************************************************/
  tdp = new(g) JSONDEF;
#if defined(ZIP_SUPPORT)
  tdp->Entry = GetStringTableOption(g, topt, "Entry", NULL);
  tdp->Zipped = GetBooleanTableOption(g, topt, "Zipped", false);
#endif   // ZIP_SUPPORT
  tdp->Fn = GetStringTableOption(g, topt, "Filename", NULL);

  if (!tdp->Fn && topt->http) {
    tdp->Fn = GetStringTableOption(g, topt, "Subtype", NULL);
    topt->subtype = NULL;
  } // endif fn

  if (!(tdp->Database = SetPath(g, db)))
    return 0;

  if ((tdp->Objname = GetStringTableOption(g, topt, "Object", NULL))) {
    if (*tdp->Objname == '$') tdp->Objname++;
    if (*tdp->Objname == '.') tdp->Objname++;
  } // endif Objname

  tdp->Base = GetIntegerTableOption(g, topt, "Base", 0) ? 1 : 0;
  tdp->Pretty = GetIntegerTableOption(g, topt, "Pretty", 2);
  tdp->Xcol = GetStringTableOption(g, topt, "Expand", NULL);
  tdp->Accept = GetBooleanTableOption(g, topt, "Accept", false);
  tdp->Uri = (dsn && *dsn ? dsn : NULL);

  if (!tdp->Fn && !tdp->Uri) {
    strcpy(g->Message, MSG(MISSING_FNAME));
    return 0;
  } else
    topt->subtype = NULL;

  if (tdp->Fn) {
    //  We used the file name relative to recorded datapath
    PlugSetPath(filename, tdp->Fn, tdp->GetPath());
    tdp->Fn = PlugDup(g, filename);
  } // endif Fn

  if (trace(1))
    htrc("File %s objname=%s pretty=%d lvl=%d\n",
      tdp->Fn, tdp->Objname, tdp->Pretty, lvl);

  if (tdp->Uri) {
#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
    tdp->Collname = GetStringTableOption(g, topt, "Tabname", NULL);
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
  } // endif Uri

  if (tdp->Pretty == 2) {
    if (tdp->Zipped) {
#if defined(ZIP_SUPPORT)
      tjsp = new(g) TDBJSON(tdp, new(g) UNZFAM(tdp));
#else   // !ZIP_SUPPORT
      sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
      return 0;
#endif  // !ZIP_SUPPORT
    } else
      tjsp = new(g) TDBJSON(tdp, new(g) MAPFAM(tdp));

    if (tjsp->MakeDocument(g))
      return 0;

    jsp = (tjsp->GetDoc()) ? tjsp->GetDoc()->GetArrayValue(0) : NULL;
  } else {
    if (!(tdp->Lrecl = GetIntegerTableOption(g, topt, "Lrecl", 0)))
    {
      if (!mgo && !tdp->Uri) {
        sprintf(g->Message, "LRECL must be specified for pretty=%d", tdp->Pretty);
        return 0;
      } else
        tdp->Lrecl = 8192;       // Should be enough
    }

    tdp->Ending = GetIntegerTableOption(g, topt, "Ending", CRLF);

    if (tdp->Zipped) {
#if defined(ZIP_SUPPORT)
      tjnp = new(g)TDBJSN(tdp, new(g) UNZFAM(tdp));
#else   // !ZIP_SUPPORT
      sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
      return NULL;
#endif  // !ZIP_SUPPORT
    } else if (tdp->Uri) {
      if (tdp->Driver && toupper(*tdp->Driver) == 'C') {
#if defined(CMGO_SUPPORT)
        tjnp = new(g) TDBJSN(tdp, new(g) CMGFAM(tdp));
#else
        sprintf(g->Message, "Mongo %s Driver not available", "C");
        return 0;
#endif
      } else if (tdp->Driver && toupper(*tdp->Driver) == 'J') {
#if defined(JAVA_SUPPORT)
        tjnp = new(g) TDBJSN(tdp, new(g) JMGFAM(tdp));
#else
        sprintf(g->Message, "Mongo %s Driver not available", "Java");
        return 0;
#endif
      } else {             // Driver not specified
#if defined(CMGO_SUPPORT)
        tjnp = new(g) TDBJSN(tdp, new(g) CMGFAM(tdp));
#elif defined(JAVA_SUPPORT)
        tjnp = new(g) TDBJSN(tdp, new(g) JMGFAM(tdp));
#else
        sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
        return 0;
#endif
      } // endif Driver

    } else if (tdp->Pretty >= 0)
      tjnp = new(g) TDBJSN(tdp, new(g) DOSFAM(tdp));
		else
			tjnp = new(g) TDBJSN(tdp, new(g) BINFAM(tdp));

    tjnp->SetMode(MODE_READ);

    // Allocate the parse work memory
    G = PlugInit(NULL, (size_t)tdp->Lrecl * (tdp->Pretty >= 0 ? 10 : 2));
    tjnp->SetG(G);

    if (tjnp->OpenDB(g))
      return 0;

    switch (tjnp->ReadDB(g)) {
    case RC_EF:
      strcpy(g->Message, "Void json table");
    case RC_FX:
      goto err;
    default:
      if (tdp->Pretty != 2)
        reclg = strlen(tjnp->To_Line);

      jsp = tjnp->Row;
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
  } // endif Uri

  /*********************************************************************/
  /*  Analyse the JSON tree and define columns.                        */
  /*********************************************************************/
  for (i = 1; ; i++) {
    for (jpp = row->GetFirst(); jpp; jpp = jpp->Next) {
      strncpy(colname, jpp->Key, 64);
      fmt[bf] = 0;

      if (Find(g, jpp->Val, colname, MY_MIN(lvl, 0)))
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
        if (tdp->Pretty != 2 && reclg < strlen(tjnp->To_Line))
          reclg = strlen(tjnp->To_Line);

        jsp = tjnp->Row;
      } // endswitch ReadDB

    } else
      jsp = tjsp->GetDoc()->GetArrayValue(i);

    if (!(row = (jsp) ? jsp->GetObject() : NULL))
      break;

  } // endfor i

  if (tdp->Pretty != 2) {
    if (!topt->lrecl)
      topt->lrecl = reclg + 10;

    tjnp->CloseDB(g);
  } // endif Pretty

  return n;

err:
  if (tdp->Pretty != 2)
    tjnp->CloseDB(g);

  return 0;
} // end of GetColumns

bool JSONDISC::Find(PGLOBAL g, PJVAL jvp, PCSZ key, int j)
{
  char  *p, *pc = colname + strlen(colname);
  int    ars;
	size_t n;
  PJOB   job;
  PJAR   jar;

  if (jvp && jvp->DataType != TYPE_JSON) {
		if (JsonAllPath() && !fmt[bf])                                  
			strcat(fmt, colname);

		jcol.Type = jvp->DataType;

    switch (jvp->DataType) {
      case TYPE_STRG:
      case TYPE_DTM:
        jcol.Len = (int)strlen(jvp->Strp);
        break;
      case TYPE_INTG:
      case TYPE_BINT:
        jcol.Len = (int)strlen(jvp->GetString(g));
        break;
      case TYPE_DBL:
        jcol.Len = (int)strlen(jvp->GetString(g));
        jcol.Scale = jvp->Nd;
        break;
      case TYPE_BOOL:
        jcol.Len = 1;
        break;
      default:
        jcol.Len = 0;
        break;
    } // endswitch Type

    jcol.Scale = jvp->Nd;
    jcol.Cbn = jvp->DataType == TYPE_NULL;
  } else if (!jvp || jvp->IsNull()) {
    jcol.Type = TYPE_UNKNOWN;
    jcol.Len = jcol.Scale = 0;
    jcol.Cbn = true;
  } else if (j < lvl && !Stringified(strfy, colname)) {
    if (!fmt[bf])
      strcat(fmt, colname);

    p = fmt + strlen(fmt);
    jsp = jvp->GetJson();

    switch (jsp->GetType()) {
      case TYPE_JOB:
        job = (PJOB)jsp;

        for (PJPR jrp = job->GetFirst(); jrp; jrp = jrp->Next) {
          PCSZ k = jrp->Key;

          if (*k != '$') {
						n = sizeof(fmt) - strlen(fmt) -1;
						strncat(strncat(fmt, sep, n), k, n - strlen(sep));
						n = sizeof(colname) - strlen(colname) - 1;
						strncat(strncat(colname, "_", n), k, n - 1);
          } // endif Key

          if (Find(g, jrp->Val, k, j + 1))
            return true;

          *p = *pc = 0;
        } // endfor jrp

        return false;
      case TYPE_JAR:
        jar = (PJAR)jsp;

        if (all || (tdp->Xcol && !stricmp(tdp->Xcol, key)))
          ars = MY_MIN(jar->GetSize(false), limit);
        else
          ars = MY_MIN(jar->GetSize(false), 1);

        for (int k = 0; k < ars; k++) {
					n = sizeof(fmt) - (strlen(fmt) + 1);

					if (!tdp->Xcol || stricmp(tdp->Xcol, key)) {
            sprintf(buf, "%d", k);

						if (tdp->Uri) {
							strncat(strncat(fmt, sep, n), buf, n - strlen(sep));
						} else {
							strncat(strncat(fmt, "[", n), buf, n - 1);
							strncat(fmt, "]", n - (strlen(buf) + 1));
						} // endif uri

						if (all) {
							n = sizeof(colname) - (strlen(colname) + 1);
							strncat(strncat(colname, "_", n), buf, n - 1);
						} // endif all

					} else
						strncat(fmt, (tdp->Uri ? sep : "[*]"), n);

          if (Find(g, jar->GetArrayValue(k), "", j))
            return true;

          *p = *pc = 0;
        } // endfor k

        return false;
      default:
        sprintf(g->Message, "Logical error after %s", fmt);
        return true;
    } // endswitch Type

  } else if (lvl >= 0) {
    if (Stringified(strfy, colname)) {
			if (!fmt[bf])
				strcat(fmt, colname);

			strcat(fmt, ".*");
		}	else if (JsonAllPath() && !fmt[bf])
			strcat(fmt, colname);

		jcol.Type = TYPE_STRG;
    jcol.Len = sz;
    jcol.Scale = 0;
    jcol.Cbn = true;
  } else
    return false;

  AddColumn(g);
  return false;
} // end of Find

void JSONDISC::AddColumn(PGLOBAL g)
{
  bool b = fmt[bf] != 0;     // True if formatted

  // Check whether this column was already found
  for (jcp = fjcp; jcp; jcp = jcp->Next)
    if (!strcmp(colname, jcp->Name))
      break;

  if (jcp) {
    if (jcp->Type != jcol.Type) {
      if (jcp->Type == TYPE_UNKNOWN || jcp->Type == TYPE_NULL)
        jcp->Type = jcol.Type;
//    else if (jcol.Type != TYPE_UNKNOWN && jcol.Type != TYPE_VOID)
//      jcp->Type = TYPE_STRING;
      else if (jcp->Type != TYPE_STRG)
        switch (jcol.Type) {
        case TYPE_STRG:
        case TYPE_DBL:
          jcp->Type = jcol.Type;
          break;
        case TYPE_BINT:
          if (jcp->Type == TYPE_INTG || jcp->Type == TYPE_BOOL)
            jcp->Type = jcol.Type;

          break;
        case TYPE_INTG:
          if (jcp->Type == TYPE_BOOL)
            jcp->Type = jcol.Type;

          break;
        default:
          break;
        } // endswith Type

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

  if (jcp)
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
bool JSONDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
  Schema = GetStringCatInfo(g, "DBname", Schema);
  Jmode = (JMODE)GetIntCatInfo("Jmode", MODE_OBJECT);

  if ((Objname = GetStringCatInfo(g, "Object", NULL))) {
    if (*Objname == '$') Objname++;
    if (*Objname == '.') Objname++;
  } // endif Objname

  Xcol = GetStringCatInfo(g, "Expand", NULL);
  Pretty = GetIntCatInfo("Pretty", 2);
  Limit = GetIntCatInfo("Limit", 50);
  Base = GetIntCatInfo("Base", 0) ? 1 : 0;
  Sep = *GetStringCatInfo(g, "Separator", ".");
  Accept = GetBoolCatInfo("Accept", false);

  // Don't use url as MONGO uri when called from REST
  if (stricmp(am, "REST") && (Uri = GetStringCatInfo(g, "Connect", NULL))) {
#if defined(JAVA_SUPPORT) || defined(CMGO_SUPPORT)
    Collname = GetStringCatInfo(g, "Name",
      (Catfunc & (FNC_TABLE | FNC_COL)) ? NULL : Name);
    Collname = GetStringCatInfo(g, "Tabname", Collname);
    Options = GetStringCatInfo(g, "Colist", Xcol ? "all" : NULL);
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
  } // endif Uri

  return DOSDEF::DefineAM(g, (Uri ? "XMGO" : "DOS"), poff);
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB JSONDEF::GetTable(PGLOBAL g, MODE m)
{
  if (trace(1))
    htrc("JSON GetTable Pretty=%d Uri=%s\n", Pretty, SVP(Uri));

  if (Catfunc == FNC_COL)
    return new(g)TDBJCL(this);

  PTDBASE tdbp;
  PTXF    txfp = NULL;

  // JSN not used for pretty=1 for insert or delete
  if (Pretty <= 0 || (Pretty == 1 && (m == MODE_READ || m == MODE_UPDATE))) {
    USETEMP tmp = UseTemp();
    bool    map = Mapped && Pretty >= 0 && m != MODE_INSERT &&
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
      } else {             // Driver not specified
#if defined(CMGO_SUPPORT)
        txfp = new(g) CMGFAM(this);
#elif defined(JAVA_SUPPORT)
        txfp = new(g) JMGFAM(this);
#else   // !MONGO_SUPPORT
        sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
        return NULL;
#endif  // !MONGO_SUPPORT
      } // endif Driver

      Pretty = 4;   // Not a file
    } else if (Zipped) {
#if defined(ZIP_SUPPORT)
      if (m == MODE_READ || m == MODE_ANY || m == MODE_ALTER) {
        txfp = new(g) UNZFAM(this);
      } else if (m == MODE_INSERT) {
        txfp = new(g) ZIPFAM(this);
      } else {
        strcpy(g->Message, "UPDATE/DELETE not supported for ZIP");
        return NULL;
      } // endif's m
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
		else if (Pretty < 0)	 // BJsonfile
			txfp = new(g) BINFAM(this);
		else
      txfp = new(g) DOSFAM(this);

    // Txfp must be set for TDBJSN
    tdbp = new(g) TDBJSN(this, txfp);

    if (Lrecl) {
      // Allocate the parse work memory
#if 0
      PGLOBAL G = (PGLOBAL)PlugSubAlloc(g, NULL, sizeof(GLOBAL));
      memset(G, 0, sizeof(GLOBAL));
      G->Sarea_Size = (size_t)Lrecl * 10;
      G->Sarea = PlugSubAlloc(g, NULL, G->Sarea_Size);
      PlugSubSet(G->Sarea, G->Sarea_Size);
      G->jump_level = 0;
      ((TDBJSN*)tdbp)->G = G;
#endif // 0
      ((TDBJSN*)tdbp)->G = PlugInit(NULL, (size_t)Lrecl * (Pretty >= 0 ? 12 : 4));
    } else {
      strcpy(g->Message, "LRECL is not defined");
      return NULL;
    } // endif Lrecl

  } else {
    if (Zipped) {
#if defined(ZIP_SUPPORT)
      if (m == MODE_READ || m == MODE_ANY || m == MODE_ALTER) {
        txfp = new(g) UNZFAM(this);
      } else if (m == MODE_INSERT) {
        strcpy(g->Message, "INSERT supported only for zipped JSON when pretty=0");
        return NULL;
      } else {
        strcpy(g->Message, "UPDATE/DELETE not supported for ZIP");
        return NULL;
      } // endif's m
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
/*  Implementation of the TDBJSN class (Pretty < 2)                    */
/***********************************************************************/
TDBJSN::TDBJSN(PJDEF tdp, PTXF txfp) : TDBDOS(tdp, txfp)
{
	G = NULL;
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

TDBJSN::TDBJSN(TDBJSN* tdbp) : TDBDOS(NULL, tdbp)
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
}  // end of TDBJSN copy constructor

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

#if 0
/***********************************************************************/
/*  JSON Cardinality: returns table size in number of rows.            */
/***********************************************************************/
int TDBJSN::Cardinality(PGLOBAL g)
{
  if (!g)
    return 0;
	else if (Cardinal < 0) {
		Cardinal = TDBDOS::Cardinality(g);

	}	// endif Cardinal

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
#endif // 0

/***********************************************************************/
/*  JSON EstimatedLength. Returns an estimated minimum line length.    */
/***********************************************************************/
int TDBJSN::EstimatedLength(void)
{
	if (AvgLen <= 0)
		return (Lrecl ? Lrecl : 1024) / 8;		// TODO: make it better
	else
		return AvgLen;

} // end of Estimated Length

/***********************************************************************/
/*  Find the row in the tree structure.                                */
/***********************************************************************/
PJSON TDBJSN::FindRow(PGLOBAL g)
{
  char *p, *objpath = PlugDup(g, Objname);
  char *sep = (char*)(Sep == ':' ? ":[" : ".[");
  bool  bp = false, b = false;
  PJSON jsp = Row;
  PJVAL val = NULL;

  for (; jsp && objpath; objpath = p, bp = b) {
    if ((p = strpbrk(objpath + 1, sep))) {
      b = (*p == '[');
      *p++ = 0;
    } // endif p

    if (!bp && *objpath != '[' && !IsNum(objpath)) { // objpass is a key
      val = (jsp->GetType() == TYPE_JOB) ?
        jsp->GetObject()->GetKeyValue(objpath) : NULL;
    } else {
      if (bp || *objpath == '[') {
        if (objpath[strlen(objpath) - 1] != ']') {
          sprintf(g->Message, "Invalid Table path %s", Objname);
          return NULL;
        } else if (!bp)
          objpath++;

      } // endif [

      val = (jsp->GetType() == TYPE_JAR) ?
        jsp->GetArray()->GetArrayValue(atoi(objpath) - B) : NULL;
    } // endif objpath

    jsp = (val) ? val->GetJson() : NULL;
  } // endfor objpath

  if (jsp && jsp->GetType() != TYPE_JOB) {
    if (jsp->GetType() == TYPE_JAR) {
      jsp = jsp->GetArray()->GetArrayValue(B);

      if (jsp->GetType() != TYPE_JOB)
        jsp = NULL;

    } else
      jsp = NULL;

  } // endif Type

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

	if (Pretty < 0) {
		/*******************************************************************/
		/*  Binary BJSON table.                                            */
		/*******************************************************************/
		xtrc(1, "JSN OpenDB: tdbp=%p tdb=R%d use=%d mode=%d\n",
						this, Tdb_No, Use, Mode);

		if (Use == USE_OPEN) {
			/*******************************************************************/
			/*  Table already open, just replace it at its beginning.          */
			/*******************************************************************/
			if (!To_Kindex) {
				Txfp->Rewind();       // see comment in Work.log
			} else // Table is to be accessed through a sorted index table
				To_Kindex->Reset();

			return false;
		} // endif use

		/*********************************************************************/
		/*  Open according to logical input/output mode required.            */
		/*  Use conventionnal input/output functions.                        */
		/*********************************************************************/
		if (Txfp->OpenTableFile(g))
			return true;

		Use = USE_OPEN;       // Do it now in case we are recursively called

		/*********************************************************************/
		/*  Lrecl is Ok.                      															 */
		/*********************************************************************/
    MODE   mode = Mode;

    // Buffer must be allocated in g->Sarea
    Mode = MODE_ANY;
    Txfp->AllocateBuffer(g);
    Mode = mode;

    //To_Line = (char*)PlugSubAlloc(g, NULL, linelen);
		//memset(To_Line, 0, linelen);
		To_Line = Txfp->GetBuf();
		xtrc(1, "OpenJSN: R%hd mode=%d To_Line=%p\n", Tdb_No, Mode, To_Line);
		return false;
	} else if (TDBDOS::OpenDB(g))
    return true;

  if (Xcol)
    To_Filter = NULL;              // Imcompatible

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

  if (Pretty == 1) {
    if (Mode == MODE_INSERT || Mode == MODE_DELETE) {
      // Mode Insert and delete are no more handled here
      DBUG_ASSERT(false);
    } else if (len > 0) // !Insert && !Delete
      rc = (Txfp->SkipRecord(g, false) == RC_FX || Txfp->RecordPos(g));

  } // endif Pretty

  return rc;
} // end of SkipHeader

/***********************************************************************/
/*  ReadDB: Data Base read routine for JSN access method.              */
/***********************************************************************/
int TDBJSN::ReadDB(PGLOBAL g) {
	int   rc;

	N++;

	if (NextSame) {
		SameRow = NextSame;
		NextSame = 0;
		M++;
		return RC_OK;
	} else if ((rc = TDBDOS::ReadDB(g)) == RC_OK) {
		if (!IsRead() && ((rc = ReadBuffer(g)) != RC_OK))
			return rc;	// Deferred reading failed

		if (Pretty >= 0) {
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

		} else {
			// Here we get a movable Json binary tree
			PJSON jsp;
			SWAP* swp;

			jsp = (PJSON)To_Line;
			swp = new(g) SWAP(G, jsp);
			swp->SwapJson(jsp, false);		// Restore pointers from offsets
			Row = jsp;
			Row = FindRow(g);
			SameRow = 0;
			Fpos++;
			M = 1;
			rc = RC_OK;
		}	// endif Pretty

	} // endif ReadDB

	return rc;
} // end of ReadDB

/***********************************************************************/
/*  Make the top tree from the object path.                            */
/***********************************************************************/
bool TDBJSN::MakeTopTree(PGLOBAL g, PJSON jsp)
{
  if (Objname) {
    if (!Val) {
      // Parse and allocate Objname item(s)
      char *p, *objpath = PlugDup(g, Objname);
      char *sep = (char*)(Sep == ':' ? ":[" : ".[");
      int   i;
      bool  bp = false, b = false;
      PJOB  objp;
      PJAR  arp;
      PJVAL val = NULL;

      Top = NULL;

      for (; objpath; objpath = p, bp = b) {
        if ((p = strpbrk(objpath + 1, sep))) {
          b = (*p == '[');
          *p++ = 0;
        } // endif p

        if (!bp && *objpath != '[' && !IsNum(objpath)) {
          objp = new(g) JOBJECT;

          if (!Top)
            Top = objp;

          if (val)
            val->SetValue(objp);

          val = new(g) JVALUE;
          objp->SetKeyValue(g, val, objpath);
        } else {
          if (bp || *objpath == '[') {
            // Old style
            if (objpath[strlen(objpath) - 1] != ']') {
              sprintf(g->Message, "Invalid Table path %s", Objname);
              return true;
            } else if (!bp)
              objpath++;

          } // endif bp

          arp = new(g) JARRAY;

          if (!Top)
            Top = arp;

          if (val)
            val->SetValue(arp);

          val = new(g) JVALUE;
          i = atoi(objpath) - B;
          arp->SetArrayValue(g, val, i);
          arp->InitArray(g);
        } // endif objpath

      } // endfor p

      Val = val;
    } // endif Val

    Val->SetValue(jsp);
  } else
    Top = jsp;

  return false;
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

/***********************************************************************/
/*  Data Base close routine for JSON access method.                    */
/***********************************************************************/
void TDBJSN::CloseDB(PGLOBAL g)
{
  TDBDOS::CloseDB(g);
  G = PlugExit(G);
} // end of CloseDB

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
  Warned = false;
  Sgfy = false;
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
  Warned = col1->Warned;
  Sgfy = col1->Sgfy;
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

  //if (*p == '[') p++;    // Old syntax .[ or :[
  n = (int)strlen(p);

  if (*p) {
    if (p[n - 1] == ']') {
      p[--n] = 0;
    } else if (!IsNum(p)) {
      // Wrong array specification
      snprintf(g->Message, sizeof(g->Message), "Invalid array specification %s for %s", p, Name);
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
  bool  a;

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
        *p2++ = 0;     // Old syntax .[ or :[
      else
        p1 = NULL;

    } // endif p1

    if (p2)
      *p2++ = 0;

    // Jpath must be explicit
    if (a || *p == 0 || *p == '[' || IsNum(p)) {
      // Analyse intermediate array processing
      if (SetArrayOptions(g, p, i, Nodes[i - 1].Key))
        return true;
      else if (Xpd && Tjp->Mode == MODE_DELETE) {
        strcpy(g->Message, "Cannot delete expanded columns");
        return true;
      } // endif Xpd

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
      if (*p1 == '.') p1++;
      mgopath = PlugDup(g, p1);
    } else {
      Sgfy = true;
      return NULL;
    } // endif

    for (p1 = p2 = mgopath; *p1; p1++)
    {
      if (i) {                 // Inside []
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
            p2--;              // Suppress last :*
            Sgfy = true;
            break;
          } // endif p2
          /* falls through */
        default:
          *p2++ = *p1;
          break;
      } // endswitch p1;
    }

      if (*(p2 - 1) == '.')
        p2--;

      *p2 = 0;
      return mgopath;
  } else
    return NULL;

} // end of GetJpath

/***********************************************************************/
/*  MakeJson: Serialize the json item and set value to it.             */
/***********************************************************************/
PVAL JSONCOL::MakeJson(PGLOBAL g, PJSON jsp, int n)
{
  if (Value->IsTypeNum()) {
    strcpy(g->Message, "Cannot make Json for a numeric column");

    if (!Warned) {
      PushWarning(g, Tjp);
      Warned = true;
    } // endif Warned

    Value->Reset();
    return Value;
#if 0
	} else if (Value->GetType() == TYPE_BIN) {
		if ((unsigned)Value->GetClen() >= sizeof(BSON)) {
			ulong len = Tjp->Lrecl ? Tjp->Lrecl : 500;
			PBSON bsp = JbinAlloc(g, NULL, len, jsp);

			strcat(bsp->Msg, " column");
			((BINVAL*)Value)->SetBinValue(bsp, sizeof(BSON));
		} else {
			strcpy(g->Message, "Column size too small");
			Value->SetValue_char(NULL, 0);
		} // endif Clen
#endif // 0
	}	else if (n < Nod - 1) {
    if (jsp->GetType() == TYPE_JAR) {
      int    ars = jsp->GetSize(false);
      PJNODE jnp = &Nodes[n];
      PJAR   jvp = new(g) JARRAY;

      for (jnp->Rank = 0; jnp->Rank < ars; jnp->Rank++)
        jvp->AddArrayValue(g, GetRowValue(g, jsp, n));

      jnp->Rank = 0;
      jvp->InitArray(g);
      jsp = jvp;
    } else if (jsp->Type == TYPE_JOB) {
      PJOB jvp = new(g) JOBJECT;

      for (PJPR prp = ((PJOB)jsp)->GetFirst(); prp; prp = prp->Next)
        jvp->SetKeyValue(g, GetRowValue(g, prp->Val, n + 1), prp->Key);

      jsp = jvp;
    } // endif Type

  } // endif

  Value->SetValue_psz(Serialize(g, jsp, NULL, 0));
  return Value;
} // end of MakeJson

/***********************************************************************/
/*  GetRowValue:                                                       */
/***********************************************************************/
PJVAL JSONCOL::GetRowValue(PGLOBAL g, PJSON row, int i)
{
  PJVAL val = NULL;

  for (; i < Nod && row; i++) {
    switch (row->GetType()) {
      case TYPE_JOB:
        val = (Nodes[i].Key) ? ((PJOB)row)->GetKeyValue(Nodes[i].Key) : NULL;
        break;
      case TYPE_JAR:
        val = ((PJAR)row)->GetArrayValue(Nodes[i].Rank);
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

  return val;
} // end of GetRowValue

/***********************************************************************/
/*  SetValue: Set a value from a JVALUE contains.                      */
/***********************************************************************/
void JSONCOL::SetJsonValue(PGLOBAL g, PVAL vp, PJVAL jvp)
{
  if (jvp) {
    vp->SetNull(false);

    switch (jvp->GetValType()) {
      case TYPE_STRG:
      case TYPE_INTG:
      case TYPE_BINT:
      case TYPE_DBL:
      case TYPE_DTM:
				switch (vp->GetType()) {
				case TYPE_STRING:
					vp->SetValue_psz(jvp->GetString(g));
					break;
				case TYPE_INT:
				case TYPE_SHORT:
				case TYPE_TINY:
					vp->SetValue(jvp->GetInteger());
					break;
				case TYPE_BIGINT:
					vp->SetValue(jvp->GetBigint());
					break;
				case TYPE_DOUBLE:
					vp->SetValue(jvp->GetFloat());

					if (jvp->GetValType() == TYPE_DBL)
						vp->SetPrec(jvp->Nd);

					break;
        case TYPE_DATE:
          if (jvp->GetValType() == TYPE_STRG) {
            PSZ dat = jvp->GetString(g);

            if (!IsNum(dat)) {
              if (!((DTVAL*)vp)->IsFormatted())
                ((DTVAL*)vp)->SetFormat(g, "YYYY-MM-DDThh:mm:ssZ", 20, 0);

              vp->SetValue_psz(dat);
            } else
              vp->SetValue(atoi(dat));

          } else
            vp->SetValue(jvp->GetInteger());

          break;
        default:
					sprintf(g->Message, "Unsupported column type %d\n", vp->GetType());
					throw 888;
				} // endswitch Type

        break;
      case TYPE_BOOL:
        if (vp->IsTypeNum())
          vp->SetValue(jvp->GetInteger() ? 1 : 0);
        else
          vp->SetValue_psz((PSZ)(jvp->GetInteger() ? "true" : "false"));

        break;
      case TYPE_JAR:
//      SetJsonValue(g, vp, val->GetArray()->GetValue(0));
				vp->SetValue_psz(jvp->GetArray()->GetText(g, NULL));
				break;
      case TYPE_JOB:
//      if (!vp->IsTypeNum() || !Strict) {
          vp->SetValue_psz(jvp->GetObject()->GetText(g, NULL));
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

//  if (Xpd && Value->IsNull() && !((PJDEF)Tjp->To_Def)->Accept)
//    throw("Null expandable JSON value");

  // Set null when applicable
  if (!Nullable)
    Value->SetNull(false);

} // end of ReadColumn

/***********************************************************************/
/*  GetColumnValue:                                                    */
/***********************************************************************/
PVAL JSONCOL::GetColumnValue(PGLOBAL g, PJSON row, int i)
{
  PJAR  arp;
  PJVAL val = NULL;

  for (; i < Nod && row; i++) {
    if (Nodes[i].Op == OP_NUM) {
      Value->SetValue(row->GetType() == TYPE_JAR ? ((PJAR)row)->size() : 1);
      return(Value);
    } else if (Nodes[i].Op == OP_XX) {
      return MakeJson(G, row, i);
    } else switch (row->GetType()) {
      case TYPE_JOB:
        if (!Nodes[i].Key) {
          // Expected Array was not there, wrap the value
          if (i < Nod-1)
            continue;
          else
            val = new(G) JVALUE(row);

        } else
          val = ((PJOB)row)->GetKeyValue(Nodes[i].Key);

        break;
      case TYPE_JAR:
        arp = (PJAR)row;

        if (!Nodes[i].Key) {
          if (Nodes[i].Op == OP_EQ)
            val = arp->GetArrayValue(Nodes[i].Rank);
          else if (Nodes[i].Op == OP_EXP)
            return ExpandArray(g, arp, i);
          else
            return CalculateArray(g, arp, i);

        } else {
          // Unexpected array, unwrap it as [0]
          val = arp->GetArrayValue(0);
          i--;
        } // endif's

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

  SetJsonValue(g, Value, val);
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

  if (!(jvp = arp->GetArrayValue((Nodes[n].Rx = Nodes[n].Nx)))) {
    strcpy(g->Message, "Logical error expanding array");
    throw 666;
  } // endif jvp

  if (n < Nod - 1 && jvp->GetJson()) {
    jval.SetValue(g, GetColumnValue(g, jvp->GetJson(), n + 1));
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

  SetJsonValue(g, Value, jvp);
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
    jvrp = arp->GetArrayValue(i);

    if (trace(1))
      htrc("i=%d nv=%d\n", i, nv);

    if (!jvrp->IsNull() || (op == OP_CNC && GetJsonNull())) do {
      if (jvrp->IsNull()) {
				jvrp->Strp = PlugDup(g, GetJsonNull());
        jvrp->DataType = TYPE_STRG;
        jvp = jvrp;
      } else if (n < Nod - 1 && jvrp->GetJson()) {
        Tjp->NextSame = nextsame;
        jval.SetValue(g, GetColumnValue(g, jvrp->GetJson(), n + 1));
        jvp = &jval;
      } else
        jvp = jvrp;

      if (trace(1))
        htrc("jvp=%s null=%d\n",
          jvp->GetString(g), jvp->IsNull() ? 1 : 0);

      if (!nv++) {
        SetJsonValue(g, vp, jvp);
        continue;
      } else
        SetJsonValue(g, MulVal, jvp);

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
    if (i < Nod-1 && Nodes[i+1].Op == OP_XX)
      break;
    else switch (row->GetType()) {
      case TYPE_JOB:
        if (!Nodes[i].Key)
          // Expected Array was not there, wrap the value
          continue;

        val = ((PJOB)row)->GetKeyValue(Nodes[i].Key);
        break;
      case TYPE_JAR:
        arp = (PJAR)row;

        if (!Nodes[i].Key) {
          if (Nodes[i].Op == OP_EQ)
            val = arp->GetArrayValue(Nodes[i].Rank);
          else
            val = arp->GetArrayValue(Nodes[i].Rx);

        } else {
          // Unexpected array, unwrap it as [0]
          val = arp->GetArrayValue(0);
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
          ((PJOB)row)->SetKeyValue(G, new(G) JVALUE(nwr), Nodes[i-1].Key);
        } else if (row->GetType() == TYPE_JAR) {
          ((PJAR)row)->AddArrayValue(G, new(G) JVALUE(nwr));
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
  } // endif Xpd

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

        if (s && *s) {
          if (!(jsp = ParseJson(G, s, strlen(s)))) {
            strcpy(g->Message, s);
            throw 666;
          } // endif jsp

        } else
          jsp = NULL;

        if (arp) {
          if (Nod > 1 && Nodes[Nod-2].Op == OP_EQ)
            arp->SetArrayValue(G, new(G) JVALUE(jsp), Nodes[Nod-2].Rank);
          else
            arp->AddArrayValue(G, new(G) JVALUE(jsp));

          arp->InitArray(G);
        } else if (objp) {
          if (Nod > 1 && Nodes[Nod-2].Key)
            objp->SetKeyValue(G, new(G) JVALUE(jsp), Nodes[Nod-2].Key);

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
          arp->SetArrayValue(G, new(G) JVALUE(G, Value), Nodes[Nod-1].Rank);
        else
          arp->AddArrayValue(G, new(G) JVALUE(G, Value));

        arp->InitArray(G);
      } else if (objp) {
        if (Nodes[Nod-1].Key)
          objp->SetKeyValue(G, new(G) JVALUE(G, Value), Nodes[Nod-1].Key);

      } else if (jvp)
        jvp->SetValue(g, Value);

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
  char   *p, *p1, *p2, *memory, *objpath, *key = NULL;
  int     i = 0;
	size_t  len;
	my_bool a;
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
		p1 = (*objpath == '[') ? objpath++ : NULL;

    /*********************************************************************/
    /*  Find the table in the tree structure.                            */
    /*********************************************************************/
    for (p = objpath; jsp && p; p = (p2 ? p2 : NULL)) {
			a = (p1 != NULL);
			p1 = strchr(p, '[');
			p2 = strchr(p, '.');

			if (!p2)
				p2 = p1;
			else if (p1) {
				if (p1 < p2)
					p2 = p1;
				else if (p1 == p2 + 1)
					*p2++ = 0;		 // Old syntax .[
				else
					p1 = NULL;

			}	// endif p1

			if (p2)
				*p2++ = 0;

      if (!a && *p && *p != '[' && !IsNum(p)) {
        // obj is a key
        if (jsp->GetType() != TYPE_JOB) {
          strcpy(g->Message, "Table path does not match the json file");
          return RC_FX;
        } // endif Type

        key = p;
        objp = jsp->GetObject();
        arp = NULL;
        val = objp->GetKeyValue(key);

        if (!val || !(jsp = val->GetJson())) {
          sprintf(g->Message, "Cannot find object key %s", key);
          return RC_FX;
        } // endif val

      } else {
        if (*p == '[') {
          // Old style
          if (p[strlen(p) - 1] != ']') {
            sprintf(g->Message, "Invalid Table path near %s", p);
            return RC_FX;
          } else
            p++;

        } // endif p

        if (jsp->GetType() != TYPE_JAR) {
          strcpy(g->Message, "Table path does not match the json file");
          return RC_FX;
        } // endif Type

        arp = jsp->GetArray();
        objp = NULL;
        i = atoi(p) - B;
        val = arp->GetArrayValue(i);

        if (!val) {
          sprintf(g->Message, "Cannot find array value %d", i);
          return RC_FX;
        } // endif val

      } // endif

      jsp = val->GetJson();
    } // endfor p

  } // endif objpath

  if (jsp && jsp->GetType() == TYPE_JAR)
    Doc = jsp->GetArray();
  else {
    // The table is void or is just one object or one value
    Doc = new(g) JARRAY;

    if (val) {
      Doc->AddArrayValue(g, val);
      Doc->InitArray(g);
    } else if (jsp) {
      Doc->AddArrayValue(g, new(g) JVALUE(jsp));
      Doc->InitArray(g);
    } // endif val

    if (objp)
      objp->SetKeyValue(g, new(g) JVALUE(Doc), key);
    else if (arp)
      arp->SetArrayValue(g, new(g) JVALUE(Doc), i);
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
  {
    if (!Multiple) {
      if (MakeDocument(g) == RC_OK)
        Cardinal = Doc->size();

    } else
      return 10;
  }
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
    To_Filter = NULL;              // Imcompatible

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
    Row = Doc->GetArrayValue(Fpos);

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
      Doc->AddArrayValue(g, vp);
      Row = new(g) JOBJECT;
    } else
      Doc->SetArrayValue(g, vp, Fpos);

  } else if (Jmode == MODE_ARRAY) {
    PJVAL vp = new(g) JVALUE(Row);

    if (Mode == MODE_INSERT) {
      Doc->AddArrayValue(g, vp);
      Row = new(g) JARRAY;
    } else
      Doc->SetArrayValue(g, vp, Fpos);

  } else { // if (Jmode == MODE_VALUE)
    if (Mode == MODE_INSERT) {
      Doc->AddArrayValue(g, (PJVAL)Row);
      Row = new(g) JVALUE;
    } else
      Doc->SetArrayValue(g, (PJVAL)Row, Fpos);

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
