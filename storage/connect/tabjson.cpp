/************* tabjson C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: tabjson     Version 1.1                               */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2015  */
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
#if defined(ZIP_SUPPORT)
#include "filamzip.h"
#endif   // ZIP_SUPPORT
#include "tabmul.h"
#include "checklvl.h"
#include "resource.h"
#include "mycat.h"                             // for FNC_COL

/***********************************************************************/
/*  This should be an option.                                          */
/***********************************************************************/
#define MAXCOL          200        /* Default max column nb in result  */
#define TYPE_UNKNOWN     12        /* Must be greater than other types */
#define USE_G             1        /* Use recoverable memory if 1      */

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
USETEMP UseTemp(void);

typedef struct _jncol {
  struct _jncol *Next;
  char *Name;
  char *Fmt;
  int   Type;
  int   Len;
  int   Scale;
  bool  Cbn;
  bool  Found;
} JCOL, *PJCL;

/***********************************************************************/
/* JSONColumns: construct the result blocks containing the description */
/* of all the columns of a table contained inside a JSON file.         */
/***********************************************************************/
PQRYRES JSONColumns(PGLOBAL g, char *db, PTOS topt, bool info)
{
  static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING, TYPE_INT, 
                          TYPE_INT, TYPE_SHORT, TYPE_SHORT, TYPE_STRING};
  static XFLD fldtyp[] = {FLD_NAME, FLD_TYPE, FLD_TYPENAME, FLD_PREC, 
                          FLD_LENGTH, FLD_SCALE, FLD_NULL, FLD_FORMAT};
  static unsigned int length[] = {0, 6, 8, 10, 10, 6, 6, 0};
  char   *fn, colname[65], fmt[129];
  int     i, j, lvl, n = 0;
  int     ncol = sizeof(buftyp) / sizeof(int);
  PVAL    valp;
  JCOL    jcol;
  PJCL    jcp, fjcp = NULL, pjcp = NULL;
  PJPR   *jrp, jpp;
  PJSON   jsp;
  PJVAL   jvp;
  PJOB    row;
  PJDEF   tdp;
  TDBJSN *tjnp = NULL;
  PJTDB   tjsp = NULL;
  PQRYRES qrp;
  PCOLRES crp;

  jcol.Name = jcol.Fmt = NULL;

  if (info) {
    length[0] = 128;
    length[7] = 256;
    goto skipit;
    } // endif info

  /*********************************************************************/
  /*  Open the input file.                                             */
  /*********************************************************************/
  if (!(fn = GetStringTableOption(g, topt, "Filename", NULL))) {
    strcpy(g->Message, MSG(MISSING_FNAME));
    return NULL;
  } else {
    lvl = GetIntegerTableOption(g, topt, "Level", 0);
    lvl = (lvl < 0) ? 0 : (lvl > 16) ? 16 : lvl;
  } // endif fn

  tdp = new(g) JSONDEF;
  tdp->Fn = fn;
  tdp->Database = SetPath(g, db);
  tdp->Objname = GetStringTableOption(g, topt, "Object", NULL);
  tdp->Base = GetIntegerTableOption(g, topt, "Base", 0) ? 1 : 0;
  tdp->Pretty = GetIntegerTableOption(g, topt, "Pretty", 2);

  if (trace)
    htrc("File %s objname=%s pretty=%d lvl=%d\n", 
          tdp->Fn, tdp->Objname, tdp->Pretty, lvl);

  if (tdp->Pretty == 2) {
    tjsp = new(g) TDBJSON(tdp, new(g) MAPFAM(tdp));

    if (tjsp->MakeDocument(g))
      return NULL;

    jsp = (tjsp->GetDoc()) ? tjsp->GetDoc()->GetValue(0) : NULL;
  } else {
    if (!(tdp->Lrecl = GetIntegerTableOption(g, topt, "Lrecl", 0))) {
      sprintf(g->Message, "LRECL must be specified for pretty=%d", tdp->Pretty);
      return NULL;
      } // endif lrecl

    tdp->Ending = GetIntegerTableOption(g, topt, "Ending", CRLF);
    tjnp = new(g) TDBJSN(tdp, new(g) DOSFAM(tdp));
    tjnp->SetMode(MODE_READ);

    if (tjnp->OpenDB(g))
      return NULL;

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

  jcol.Next = NULL;
  jcol.Found = true;
  colname[64] = 0;
  fmt[128] = 0;
  jrp = (PJPR*)PlugSubAlloc(g, NULL, sizeof(PJPR) * lvl);

  /*********************************************************************/
  /*  Analyse the JSON tree and define columns.                        */
  /*********************************************************************/
  for (i = 1; ; i++) {
    for (jpp = row->GetFirst(); jpp; jpp = jpp->GetNext()) {
      for (j = 0; j < lvl; j++)
        jrp[j] = NULL;

     more:
      strncpy(colname, jpp->GetKey(), 64);
      *fmt = 0;
      j = 0;
      jvp = jpp->GetVal();

     retry:
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
        if (!*fmt)
          strcpy(fmt, colname);

        jsp = jvp->GetJson();

        switch (jsp->GetType()) {
          case TYPE_JOB:
            if (!jrp[j])
              jrp[j] = jsp->GetFirst();

            strncat(strncat(fmt, ":", 128), jrp[j]->GetKey(), 128);
            strncat(strncat(colname, "_", 64), jrp[j]->GetKey(), 64);
            jvp = jrp[j]->GetVal();
            j++;
            break;
          case TYPE_JAR:
            strncat(fmt, ":", 128);
            jvp = jsp->GetValue(0);
            break;
          default:
            sprintf(g->Message, "Logical error after %s", fmt);
            goto err;
          } // endswitch jsp

        goto retry;
      } else {
        jcol.Type = TYPE_STRING;
        jcol.Len = 256;
        jcol.Scale = 0;
        jcol.Cbn = true;
      } // endif's

      // Check whether this column was already found
      for (jcp = fjcp; jcp; jcp = jcp->Next)
        if (!strcmp(colname, jcp->Name))
          break;
  
      if (jcp) {
        if (jcp->Type != jcol.Type)
          jcp->Type = TYPE_STRING;

        if (*fmt && (!jcp->Fmt || strlen(jcp->Fmt) < strlen(fmt))) {
          jcp->Fmt = PlugDup(g, fmt);
          length[7] = MY_MAX(length[7], strlen(fmt));
          } // endif *fmt

        jcp->Len = MY_MAX(jcp->Len, jcol.Len);
        jcp->Scale = MY_MAX(jcp->Scale, jcol.Scale);
        jcp->Cbn |= jcol.Cbn;
        jcp->Found = true;
      } else {
        // New column
        jcp = (PJCL)PlugSubAlloc(g, NULL, sizeof(JCOL));
        *jcp = jcol;
        jcp->Cbn |= (i > 1);
        jcp->Name = PlugDup(g, colname);
        length[0] = MY_MAX(length[0], strlen(colname));
  
        if (*fmt) {
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

      for (j = lvl - 1; j >= 0; j--)
        if (jrp[j] && (jrp[j] = jrp[j]->GetNext()))
          goto more;

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

    } else
      jsp = tjsp->GetDoc()->GetValue(i);

    if (!(row = (jsp) ? jsp->GetObject() : NULL))
      break;

    } // endor i

  if (tdp->Pretty != 2)
    tjnp->CloseDB(g);

 skipit:
  if (trace)
    htrc("CSVColumns: n=%d len=%d\n", n, length[0]);

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
  for (i = 0, jcp = fjcp; jcp; i++, jcp = jcp->Next) {
    if (jcp->Type == TYPE_UNKNOWN)            // Void column
      jcp->Type = TYPE_STRING;

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

err:
  if (tdp->Pretty != 2)
    tjnp->CloseDB(g);

  return NULL;
  } // end of JSONColumns

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
} // end of JSONDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool JSONDEF::DefineAM(PGLOBAL g, LPCSTR, int poff)
{
  Jmode = (JMODE)GetIntCatInfo("Jmode", MODE_OBJECT);
  Objname = GetStringCatInfo(g, "Object", NULL);
  Xcol = GetStringCatInfo(g, "Expand", NULL);
  Pretty = GetIntCatInfo("Pretty", 2);
  Limit = GetIntCatInfo("Limit", 10);
  Base = GetIntCatInfo("Base", 0) ? 1 : 0;
  return DOSDEF::DefineAM(g, "DOS", poff);
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

    if (Compressed) {
#if defined(ZIP_SUPPORT)
      if (Compressed == 1)
        txfp = new(g) ZIPFAM(this);
      else
        txfp = new(g) ZLBFAM(this);
#else   // !ZIP_SUPPORT
      sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
      return NULL;
#endif  // !ZIP_SUPPORT
    } else if (map)
      txfp = new(g) MAPFAM(this);
    else
      txfp = new(g) DOSFAM(this);

    // Txfp must be set for TDBDOS
    tdbp = new(g) TDBJSN(this, txfp);

#if USE_G
		// Allocate the parse work memory
		PGLOBAL G = (PGLOBAL)PlugSubAlloc(g, NULL, sizeof(GLOBAL));
		memset(G, 0, sizeof(GLOBAL));
		G->Sarea_Size = Lrecl * 10;
		G->Sarea = PlugSubAlloc(g, NULL, G->Sarea_Size);
		PlugSubSet(G, G->Sarea, G->Sarea_Size);
		G->jump_level = -1;
		((TDBJSN*)tdbp)->G = G;
#else
		((TDBJSN*)tdbp)->G = g;
#endif
	} else {
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
    Strict = tdp->Strict;
  } else {
    Jmode = MODE_OBJECT;
    Objname = NULL;
    Xcol = NULL;
    Limit = 1;
    Pretty = 0;
    B = 0;
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
  Pretty = tdbp->Pretty;
  Strict = tdbp->Strict;
  Comma = tdbp->Comma;
  } // end of TDBJSN copy constructor

// Used for update
PTDB TDBJSN::CopyOne(PTABS t)
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
  } // end of CopyOne

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
    if ((p = strchr(objpath, ':')))
      *p++ = 0;

    if (*objpath != '[') {         // objpass is a key
      val = (jsp->GetType() == TYPE_JOB) ?
             jsp->GetObject()->GetValue(objpath) : NULL;
    } else if (objpath[strlen(objpath)-1] == ']') {
      val = (jsp->GetType() == TYPE_JAR) ?
        jsp->GetArray()->GetValue(atoi(objpath+1) - B) : NULL;
    } else
      val = NULL;

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

  return TDBDOS::OpenDB(g);
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

#if USE_G
		// Recover the memory used for parsing
		PlugSubSet(G, G->Sarea, G->Sarea_Size);
#endif

		if ((Row = ParseJson(G, To_Line, strlen(To_Line), &Pretty, &Comma))) {
			Row = FindRow(g);
			SameRow = 0;
			Fpos++;
			M = 1;
			rc = RC_OK;
		} else if (Pretty != 1 || strcmp(To_Line, "]")) {
#if USE_G
			strcpy(g->Message, G->Message);
#endif
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
        if ((p = strchr(objpath, ':')))
          *p++ = 0;
  
        if (*objpath != '[') {
          objp = new(g) JOBJECT;
  
          if (!Top)
            Top = objp;
  
          if (val)
            val->SetValue(objp);
  
          val = new(g) JVALUE;
          objp->SetValue(g, val, objpath);
        } else if (objpath[strlen(objpath)-1] == ']') {
          arp = new(g) JARRAY;

          if (!Top)
            Top = arp;
    
          if (val)
            val->SetValue(arp);
    
          val = new(g) JVALUE;
          i = atoi(objpath+1) - B;
          arp->SetValue(g, val, i);
          arp->InitArray(g);
        } else {
          sprintf(g->Message, "Invalid Table path %s", Objname);
          return RC_FX;
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
	/*  WriteDB: Data Base write routine for DOS access method.            */
	/***********************************************************************/
	int TDBJSN::WriteDB(PGLOBAL g)
{
	int rc = TDBDOS::WriteDB(g);

#if USE_G
	if (rc == RC_FX)
		strcpy(g->Message, G->Message);

	PlugSubSet(G, G->Sarea, G->Sarea_Size);
#endif
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
  int    n = (int)strlen(p);
  bool   dg = true, b = false;
  PJNODE jnp = &Nodes[i];

  if (*p) {
    if (p[--n] == ']') {
      p[n--] = 0;
      p++;
    } else {
      // Wrong array specification
      sprintf(g->Message,
        "Invalid array specification %s for %s", p, Name);
      return true;
    } // endif p

  } else
    b = true;

  // To check whether a numeric Rank was specified
  for (int k = 0; dg && p[k]; k++)
    dg = isdigit(p[k]) > 0;

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
    switch (*p) {
      case '+': jnp->Op = OP_ADD;  break;
      case '*': jnp->Op = OP_MULT; break;
      case '>': jnp->Op = OP_MAX;  break;
      case '<': jnp->Op = OP_MIN;  break;
      case '!': jnp->Op = OP_SEP;  break; // Average
      case '#': jnp->Op = OP_NUM;  break;
      case 'x':
      case 'X': // Expand this array
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
  char  *p, *p2 = NULL, *pbuf = NULL;
  int    i; 
  bool   mul = false;

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

  // The Jpath must be analyzed
  for (i = 0, p = pbuf; (p = strchr(p, ':')); i++, p++)
    Nod++;                         // One path node found

  Nodes = (PJNODE)PlugSubAlloc(g, NULL, (++Nod) * sizeof(JNODE));
  memset(Nodes, 0, (Nod) * sizeof(JNODE));

  // Analyze the Jpath for this column
  for (i = 0, p = pbuf; i < Nod; i++, p = (p2 ? p2 + 1 : p + strlen(p))) {
    if ((p2 = strchr(p, ':'))) 
      *p2 = 0;

    // Jpath must be explicit
    if (*p == 0 || *p == '[') {
      // Analyse intermediate array processing
      if (SetArrayOptions(g, p, i, Nodes[i-1].Key))
        return true;

    } else if (*p == '*') {
      // Return JSON
      Nodes[i].Op = OP_XX;
    } else {
      Nodes[i].Key = p;
      Nodes[i].Op = OP_EXIST;
    } // endif's

    } // endfor i, p

 fin:
  MulVal = AllocateValue(g, Value);
  Parsed = true;
  return false;
  } // end of ParseJpath

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
    switch (val->GetValType()) {
      case TYPE_STRG:
      case TYPE_INTG:
			case TYPE_BINT:
			case TYPE_DBL:
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
      } // endswitch Type

  } else
    vp->Reset();

  } // end of SetJsonValue

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void JSONCOL::ReadColumn(PGLOBAL g)
  {
  if (!Tjp->SameRow || Xnod >= Tjp->SameRow)
    Value->SetValue_pval(GetColumnValue(g, Tjp->Row, 0));

  // Set null when applicable
  if (Nullable)
    Value->SetNull(Value->IsZero());

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
  int    ars;
  PJVAL  jvp;
  JVALUE jval;

  ars = MY_MIN(Tjp->Limit, arp->size());

  if (!(jvp = arp->GetValue((Nodes[n].Rx = Nodes[n].Nx)))) {
    strcpy(g->Message, "Logical error expanding array");
    longjmp(g->jumper[g->jump_level], 666);
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

  for (i = 0; i < ars; i++) {
    jvrp = arp->GetValue(i);

    do {
      if (n < Nod - 1 && jvrp->GetJson()) {
        Tjp->NextSame = nextsame;
        jval.SetValue(GetColumnValue(g, jvrp->GetJson(), n + 1));
        jvp = &jval;
      } else
        jvp = jvrp;
  
      if (!nv++) {
        SetJsonValue(g, vp, jvp, n);
        continue;
      } else
        SetJsonValue(g, MulVal, jvp, n);
  
      if (!MulVal->IsZero()) {
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
    
        } // endif Zero

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
		longjmp(g->jumper[g->jump_level], 666);
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
          longjmp(g->jumper[g->jump_level], 666);
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

      // Passthru
    case TYPE_DATE:
    case TYPE_INT:
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
PTDB TDBJSON::CopyOne(PTABS t)
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
  } // end of CopyOne

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

  objpath = PlugDup(g, Objname);

  /*********************************************************************/
  /*  Find the table in the tree structure.                            */
  /*********************************************************************/
  for (; jsp && objpath; objpath = p) {
    if ((p = strchr(objpath, ':')))
      *p++ = 0;

    if (*objpath != '[') {         // objpass is a key
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

    } else if (objpath[strlen(objpath)-1] == ']') {
      if (jsp->GetType() != TYPE_JAR) {
        strcpy(g->Message, "Table path does not match the json file");
        return RC_FX;
        } // endif Type

      arp = jsp->GetArray();
      objp = NULL;
      i = atoi(objpath+1) - B;
      val = arp->GetValue(i);

      if (!val) {
        sprintf(g->Message, "Cannot find array value %d", i);
        return RC_FX;
        } // endif val

    } else {
      sprintf(g->Message, "Invalid Table path %s", Objname);
      return RC_FX;
    } // endif objpath

    jsp = val->GetJson();
    } // endfor objpath

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
  Db = (char*)tdp->GetDB();
  } // end of TDBJCL constructor

/***********************************************************************/
/*  GetResult: Get the list the JSON file columns.                     */
/***********************************************************************/
PQRYRES TDBJCL::GetResult(PGLOBAL g)
  {
  return JSONColumns(g, Db, Topt, false);
  } // end of GetResult

/* --------------------------- End of json --------------------------- */
