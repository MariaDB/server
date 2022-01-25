/************* tabbson C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: tabbson     Version 1.2                               */
/*  (C) Copyright to the author Olivier BERTRAND          2020 - 2021  */
/*  This program are the BSON class DB execution routines.             */
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
#include "maputil.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "tabbson.h"
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
/* BSONColumns: construct the result blocks containing the description */
/* of all the columns of a table contained inside a JSON file.         */
/***********************************************************************/
PQRYRES BSONColumns(PGLOBAL g, PCSZ db, PCSZ dsn, PTOS topt, bool info)
{
  static int  buftyp[] = { TYPE_STRING, TYPE_SHORT, TYPE_STRING, TYPE_INT,
                          TYPE_INT, TYPE_SHORT, TYPE_SHORT, TYPE_STRING };
  static XFLD fldtyp[] = { FLD_NAME, FLD_TYPE, FLD_TYPENAME, FLD_PREC,
                          FLD_LENGTH, FLD_SCALE, FLD_NULL, FLD_FORMAT };
  static unsigned int length[] = { 0, 6, 8, 10, 10, 6, 6, 0 };
  int     i, n = 0;
  int     ncol = sizeof(buftyp) / sizeof(int);
  PJCL    jcp;
  BSONDISC* pjdc = NULL;
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

  pjdc = new(g) BSONDISC(g, length);

  if (!(n = pjdc->GetColumns(g, db, dsn, topt)))
    return NULL;

skipit:
  if (trace(1))
    htrc("BSONColumns: n=%d len=%d\n", n, length[0]);

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
} // end of BSONColumns

/* -------------------------- Class BSONDISC ------------------------- */

/***********************************************************************/
/*  Class used to get the columns of a JSON table.                     */
/***********************************************************************/
BSONDISC::BSONDISC(PGLOBAL g, uint* lg)
{
  length = lg;
  jcp = fjcp = pjcp = NULL;
  tdp = NULL;
  tjnp = NULL;
  jpp = NULL;
  tjsp = NULL;
  jsp = NULL;
  bp = NULL;
  row = NULL;
  sep = NULL;
  strfy = NULL;
  i = n = bf = ncol = lvl = sz = limit = 0;
  all = false;
} // end of BSONDISC constructor

int BSONDISC::GetColumns(PGLOBAL g, PCSZ db, PCSZ dsn, PTOS topt)
{
  char  filename[_MAX_PATH];
  bool  mgo = (GetTypeID(topt->type) == TAB_MONGO);
  PBVAL bdp = NULL;

  lvl = GetIntegerTableOption(g, topt, "Level", GetDefaultDepth());
  lvl = GetIntegerTableOption(g, topt, "Depth", lvl);
  sep = GetStringTableOption(g, topt, "Separator", ".");
  sz = GetIntegerTableOption(g, topt, "Jsize", 1024);
  limit = GetIntegerTableOption(g, topt, "Limit", 50);
  strfy = GetStringTableOption(g, topt, "Stringify", NULL);

  /*********************************************************************/
  /*  Open the input file.                                             */
  /*********************************************************************/
  tdp = new(g) BSONDEF;
  tdp->G = NULL;
#if defined(ZIP_SUPPORT)
  tdp->Entry = GetStringTableOption(g, topt, "Entry", NULL);
  tdp->Zipped = GetBooleanTableOption(g, topt, "Zipped", false);
#endif   // ZIP_SUPPORT
  tdp->Fn = GetStringTableOption(g, topt, "Filename", NULL);

  if (!tdp->Fn && topt->http)
    tdp->Fn = GetStringTableOption(g, topt, "Subtype", NULL);

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
    tdp->G = g;

    if (tdp->Zipped) {
#if defined(ZIP_SUPPORT)
      tjsp = new(g) TDBBSON(g, tdp, new(g) UNZFAM(tdp));
#else   // !ZIP_SUPPORT
      sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
      return 0;
#endif  // !ZIP_SUPPORT
    } else
      tjsp = new(g) TDBBSON(g, tdp, new(g) MAPFAM(tdp));

    if (tjsp->MakeDocument(g))
      return 0;

    bp = tjsp->Bp;
//  bdp = tjsp->GetDoc() ? bp->GetBson(tjsp->GetDoc()) : NULL;
    bdp = tjsp->GetDoc();
    jsp = bdp ? bp->GetArrayValue(bdp, 0) : NULL;
  } else {
    if (!((tdp->Lrecl = GetIntegerTableOption(g, topt, "Lrecl", 0)))) {
      if (!mgo) {
        sprintf(g->Message, "LRECL must be specified for pretty=%d", tdp->Pretty);
        return 0;
      } else
        tdp->Lrecl = 8192;       // Should be enough

    } // endif Lrecl

    // Allocate the parse work memory
    tdp->G = PlugInit(NULL, (size_t)tdp->Lrecl * (tdp->Pretty >= 0 ? 4 : 2));
    tdp->Ending = GetIntegerTableOption(g, topt, "Ending", CRLF);

    if (tdp->Zipped) {
#if defined(ZIP_SUPPORT)
      tjnp = new(g)TDBBSN(g, tdp, new(g) UNZFAM(tdp));
#else   // !ZIP_SUPPORT
      sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
      return NULL;
#endif  // !ZIP_SUPPORT
    } else if (tdp->Uri) {
      if (tdp->Driver && toupper(*tdp->Driver) == 'C') {
#if defined(CMGO_SUPPORT)
        tjnp = new(g) TDBBSN(g, tdp, new(g) CMGFAM(tdp));
#else
        sprintf(g->Message, "Mongo %s Driver not available", "C");
        return 0;
#endif
      } else if (tdp->Driver && toupper(*tdp->Driver) == 'J') {
#if defined(JAVA_SUPPORT)
        tjnp = new(g) TDBBSN(g, tdp, new(g) JMGFAM(tdp));
#else
        sprintf(g->Message, "Mongo %s Driver not available", "Java");
        return 0;
#endif
      } else {             // Driver not specified
#if defined(CMGO_SUPPORT)
        tjnp = new(g) TDBBSN(g, tdp, new(g) CMGFAM(tdp));
#elif defined(JAVA_SUPPORT)
        tjnp = new(g) TDBBSN(g, tdp, new(g) JMGFAM(tdp));
#else
        sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
        return 0;
#endif
      } // endif Driver

    } else if (tdp->Pretty >= 0)
      tjnp = new(g) TDBBSN(g, tdp, new(g) DOSFAM(tdp));
    else
      tjnp = new(g) TDBBSN(g, tdp, new(g) BINFAM(tdp));

    tjnp->SetMode(MODE_READ);
    bp = tjnp->Bp;

    if (tjnp->OpenDB(g))
      return 0;

    switch (tjnp->ReadDB(g)) {
    case RC_EF:
      strcpy(g->Message, "Void json table");
    case RC_FX:
      goto err;
    default:
      jsp = tjnp->Row;
    } // endswitch ReadDB

  } // endif pretty

  if (!(row = (jsp) ? bp->GetObject(jsp) : NULL)) {
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
    for (jpp = row; jpp; jpp = bp->GetNext(jpp)) {
      strncpy(colname, bp->GetKey(jpp), 64);
      fmt[bf] = 0;

      if (Find(g, bp->GetVlp(jpp), colname, MY_MIN(lvl, 0)))
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
        jsp = tjnp->Row;
      } // endswitch ReadDB

    } else
      jsp = bp->GetNext(jsp);

    if (!(row = (jsp) ? bp->GetObject(jsp) : NULL))
      break;

  } // endfor i

  if (tdp->Pretty != 2)
    tjnp->CloseDB(g);

  return n;

err:
  if (tdp->Pretty != 2)
    tjnp->CloseDB(g);

  return 0;
} // end of GetColumns

bool BSONDISC::Find(PGLOBAL g, PBVAL jvp, PCSZ key, int j)
{
  char  *p, *pc = colname + strlen(colname), buf[32];
  int    ars;
  size_t n;
  PBVAL  job;
  PBVAL  jar;

  if (jvp && !bp->IsJson(jvp)) {
    if (JsonAllPath() && !fmt[bf])
      strcat(fmt, colname);

    jcol.Type = (JTYP)jvp->Type;

    switch (jvp->Type) {
      case TYPE_STRG:
      case TYPE_DTM:
        jcol.Len = (int)strlen(bp->GetString(jvp));
        break;
      case TYPE_INTG:
      case TYPE_BINT:
        jcol.Len = (int)strlen(bp->GetString(jvp, buf));
        break;
      case TYPE_DBL:
      case TYPE_FLOAT:
        jcol.Len = (int)strlen(bp->GetString(jvp, buf));
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
    jcol.Cbn = jvp->Type == TYPE_NULL;
  } else if (!jvp || bp->IsValueNull(jvp)) {
    jcol.Type = TYPE_UNKNOWN;
    jcol.Len = jcol.Scale = 0;
    jcol.Cbn = true;
  } else  if (j < lvl && !Stringified(strfy, colname)) {
    if (!fmt[bf])
      strcat(fmt, colname);

    p = fmt + strlen(fmt);
    jsp = jvp;

    switch (jsp->Type) {
    case TYPE_JOB:
      job = jsp;

      for (PBPR jrp = bp->GetObject(job); jrp; jrp = bp->GetNext(jrp)) {
        PCSZ k = bp->GetKey(jrp);

        if (*k != '$') {
          n = sizeof(fmt) - strlen(fmt) - 1;
          strncat(strncat(fmt, sep, n), k, n - strlen(sep));
          n = sizeof(colname) - strlen(colname) - 1;
          strncat(strncat(colname, "_", n), k, n - 1);
        } // endif Key

        if (Find(g, bp->GetVlp(jrp), k, j + 1))
          return true;

        *p = *pc = 0;
      } // endfor jrp

      return false;
    case TYPE_JAR:
      jar = jsp;

      if (all || (tdp->Xcol && !stricmp(tdp->Xcol, key)))
        ars = MY_MIN(bp->GetArraySize(jar), limit);
      else
        ars = MY_MIN(bp->GetArraySize(jar), 1);

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

        } else {
          strncat(fmt, (tdp->Uri ? sep : "[*]"), n);
        }

        if (Find(g, bp->GetArrayValue(jar, k), "", j))
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
    } else if (JsonAllPath() && !fmt[bf])
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

void BSONDISC::AddColumn(PGLOBAL g)
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

/* -------------------------- Class BTUTIL --------------------------- */

/***********************************************************************/
/*  Find the row in the tree structure.                                */
/***********************************************************************/
PBVAL BTUTIL::FindRow(PGLOBAL g)
{
  char *p, *objpath = PlugDup(g, Tp->Objname);
  char *sep = (char*)(Tp->Sep == ':' ? ":[" : ".[");
  bool  bp = false, b = false;
  PBVAL jsp = Tp->Row;
  PBVAL val = NULL;

  for (; jsp && objpath; objpath = p, bp = b) {
    if ((p = strpbrk(objpath + 1, sep))) {
      b = (*p == '[');
      *p++ = 0;
    } // endif p

    if (!bp && *objpath != '[' && !IsNum(objpath)) { // objpass is a key
      val = (jsp->Type == TYPE_JOB) ?
        GetKeyValue(jsp, objpath) : NULL;
    } else {
      if (bp || *objpath == '[') {                   // Old style
        if (objpath[strlen(objpath) - 1] != ']') {
          sprintf(g->Message, "Invalid Table path %s", Tp->Objname);
          return NULL;
        } else if (!bp)
          objpath++;

      } // endif bp

      val = (jsp->Type == TYPE_JAR) ?
        GetArrayValue(jsp, atoi(objpath) - Tp->B) : NULL;
    } // endif objpath

      //  jsp = (val) ? val->GetJson() : NULL;
    jsp = val;
  } // endfor objpath

  if (jsp && jsp->Type != TYPE_JOB) {
    if (jsp->Type == TYPE_JAR) {
      jsp = GetArrayValue(jsp, Tp->B);

      if (jsp->Type != TYPE_JOB)
        jsp = NULL;

    } else
      jsp = NULL;

  } // endif Type

  return jsp;
} // end of FindRow

/***********************************************************************/
/*  Parse the read line.                                               */
/***********************************************************************/
PBVAL BTUTIL::ParseLine(PGLOBAL g, int prty, bool cma)
{
  pretty = prty;
  comma = cma;
  return ParseJson(g, Tp->To_Line, strlen(Tp->To_Line));
} // end of ParseLine

/***********************************************************************/
/*  Make the top tree from the object path.                            */
/***********************************************************************/
PBVAL BTUTIL::MakeTopTree(PGLOBAL g, int type)
{
  PBVAL top = NULL, val = NULL;

  if (Tp->Objname) {
    if (!Tp->Row) {
      // Parse and allocate Objpath item(s)
      char *p, *objpath = PlugDup(g, Tp->Objname);
      char *sep = (char*)(Tp->Sep == ':' ? ":[" : ".[");
      int   i;
      bool  bp = false, b = false;
      PBVAL objp = NULL;
      PBVAL arp = NULL;

      for (; objpath; objpath = p, bp = b) {
        if ((p = strpbrk(objpath + 1, sep))) {
          b = (*p == '[');
          *p++ = 0;
        } // endif p


        if (!bp && *objpath != '[' && !IsNum(objpath)) {
          // objpass is a key
          objp = NewVal(TYPE_JOB);

          if (!top)
            top = objp;

          if (val)
            SetValueObj(val, objp);

          val = NewVal();
          SetKeyValue(objp, MOF(val), objpath);
        } else {
          if (bp || *objpath == '[') {
            // Old style
            if (objpath[strlen(objpath) - 1] != ']') {
              sprintf(g->Message, "Invalid Table path %s", Tp->Objname);
              return NULL;
            } else if (!bp)
              objpath++;

          } // endif bp

          if (!top)
            top = NewVal(TYPE_JAR);

          if (val)
            SetValueArr(val, arp);

          val = NewVal();
          i = atoi(objpath) - Tp->B;
          SetArrayValue(arp, val, i);
        } // endif objpath

      } // endfor p

    } // endif Val

    Tp->Row = val;
    if (Tp->Row) Tp->Row->Type = type;
  } else
    top = Tp->Row = NewVal(type);

  return top;
} // end of MakeTopTree

PSZ BTUTIL::SerialVal(PGLOBAL g, PBVAL vlp, int pretty)
{
  return Serialize(g, vlp, NULL, pretty);
} // en of SerialTop

/* -------------------------- Class BCUTIL --------------------------- */

/***********************************************************************/
/*  SetValue: Set a value from a BVALUE contains.                      */
/***********************************************************************/
void BCUTIL::SetJsonValue(PGLOBAL g, PVAL vp, PBVAL jvp)
{
  if (jvp) {
    vp->SetNull(false);

    if (Jb) {
      vp->SetValue_psz(Serialize(g, jvp, NULL, 0));
      Jb = false;
    } else switch (jvp->Type) {
    case TYPE_STRG:
    case TYPE_INTG:
    case TYPE_BINT:
    case TYPE_DBL:
    case TYPE_DTM:
    case TYPE_FLOAT:
      switch (vp->GetType()) {
        case TYPE_STRING:
        case TYPE_DECIM:
          vp->SetValue_psz(GetString(jvp));
          break;
        case TYPE_INT:
        case TYPE_SHORT:
        case TYPE_TINY:
          vp->SetValue(GetInteger(jvp));
          break;
        case TYPE_BIGINT:
          vp->SetValue(GetBigint(jvp));
          break;
        case TYPE_DOUBLE:
          vp->SetValue(GetDouble(jvp));

          if (jvp->Type == TYPE_DBL || jvp->Type == TYPE_FLOAT)
            vp->SetPrec(jvp->Nd);

          break;
        case TYPE_DATE:
          if (jvp->Type == TYPE_STRG) {
            PSZ dat = GetString(jvp);

            if (!IsNum(dat)) {
              if (!((DTVAL*)vp)->IsFormatted())
                ((DTVAL*)vp)->SetFormat(g, "YYYY-MM-DDThh:mm:ssZ", 20, 0);

              vp->SetValue_psz(dat);
            } else
              vp->SetValue(atoi(dat));

          } else
            vp->SetValue(GetInteger(jvp));

          break;
        default:
          sprintf(G->Message, "Unsupported column type %d", vp->GetType());
          throw 888;
      } // endswitch Type

      break;
    case TYPE_BOOL:
      if (vp->IsTypeNum())
        vp->SetValue(GetInteger(jvp) ? 1 : 0);
      else
        vp->SetValue_psz((PSZ)(GetInteger(jvp) ? "true" : "false"));

      break;
    case TYPE_JAR:
    case TYPE_JOB:
      //      SetJsonValue(g, vp, val->GetArray()->GetValue(0));
      vp->SetValue_psz(GetValueText(g, jvp, NULL));
      break;
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
/*  MakeJson: Serialize the json item and set value to it.             */
/***********************************************************************/
PBVAL BCUTIL::MakeBson(PGLOBAL g, PBVAL jsp, int n)
{
  PBVAL vlp, jvp = jsp;

  if (n < Cp->Nod - 1) {
    if (jsp->Type == TYPE_JAR) {
      int    ars = GetArraySize(jsp);
      PJNODE jnp = &Cp->Nodes[n];

      jvp = NewVal(TYPE_JAR);
      jnp->Op = OP_EQ;

      for (int i = 0; i < ars; i++) {
        jnp->Rank = i;
        vlp = GetRowValue(g, jsp, n);
        AddArrayValue(jvp,DupVal(vlp));
      } // endfor i

      jnp->Op = OP_XX;
      jnp->Rank = 0;
    } else if (jsp->Type == TYPE_JOB) {
      jvp = NewVal(TYPE_JOB);

      for (PBPR prp = GetObject(jsp); prp; prp = GetNext(prp)) {
        vlp = GetRowValue(g, GetVlp(prp), n + 1);
        SetKeyValue(jvp, vlp, MZP(prp->Key));
      }	// endfor prp

    } // endif Type

  } // endif's

  Jb = true;
  return jvp;
} // end of MakeBson

/***********************************************************************/
/*  GetRowValue:                                                       */
/***********************************************************************/
PBVAL BCUTIL::GetRowValue(PGLOBAL g, PBVAL row, int i)
{
  int    nod = Cp->Nod;
  JNODE *nodes = Cp->Nodes;
  PBVAL  arp;
  PBVAL  bvp = NULL;

  for (; i < nod && row; i++) {
    if (nodes[i].Op == OP_NUM) {
      bvp = NewVal(TYPE_INT);
      bvp->N = (row->Type == TYPE_JAR) ? GetSize(row) : 1;
      return(bvp);
    } else if (nodes[i].Op == OP_XX) {
      return MakeBson(g, row, i);
    } else switch (row->Type) {
    case TYPE_JOB:
      if (!nodes[i].Key) {
        // Expected Array was not there, wrap the value
        if (i < nod - 1)
          continue;
        else
          bvp = row;

      } else
        bvp = GetKeyValue(row, nodes[i].Key);

      break;
    case TYPE_JAR:
      arp = row;

      if (!nodes[i].Key) {
        if (nodes[i].Op == OP_EQ)
          bvp = GetArrayValue(arp, nodes[i].Rank);
        else if (nodes[i].Op == OP_EXP)
          return NewVal(ExpandArray(g, arp, i));
        else
          return NewVal(CalculateArray(g, arp, i));

      } else {
        // Unexpected array, unwrap it as [0]
        bvp = GetArrayValue(arp, 0);
        i--;
      } // endif's

      break;
    case TYPE_JVAL:
      bvp = row;
      break;
    default:
      sprintf(g->Message, "Invalid row JSON type %d", row->Type);
      bvp = NULL;
    } // endswitch Type

    if (i < nod - 1)
      row = bvp;

  } // endfor i

  return bvp;
} // end of GetRowValue

/***********************************************************************/
/*  GetColumnValue:                                                    */
/***********************************************************************/
PVAL BCUTIL::GetColumnValue(PGLOBAL g, PBVAL row, int i)
{
  PVAL  value = Cp->Value;
  PBVAL bvp = GetRowValue(g, row, i);

  SetJsonValue(g, value, bvp);
  return value;
} // end of GetColumnValue

/***********************************************************************/
/*  ExpandArray:                                                       */
/***********************************************************************/
PVAL BCUTIL::ExpandArray(PGLOBAL g, PBVAL arp, int n)
{
  int    nod = Cp->Nod, ars = MY_MIN(Tp->Limit, GetArraySize(arp));
  JNODE *nodes = Cp->Nodes;
  PVAL   value = Cp->Value;
  PBVAL  bvp;
  BVAL   bval;

  if (!ars) {
    value->Reset();
    value->SetNull(true);
    Tp->NextSame = 0;
    return value;
  } // endif ars

  if (!(bvp = GetArrayValue(arp, (nodes[n].Rx = nodes[n].Nx)))) {
    strcpy(g->Message, "Logical error expanding array");
    throw 666;
  } // endif jvp

  if (n < nod - 1 && IsJson(bvp)) {
    SetValue(&bval, GetColumnValue(g, bvp, n + 1));
    bvp = &bval;
  } // endif n

  if (n >= Tp->NextSame) {
    if (++nodes[n].Nx == ars) {
      nodes[n].Nx = 0;
      Cp->Xnod = 0;
    } else
      Cp->Xnod = n;

    Tp->NextSame = Cp->Xnod;
  } // endif NextSame

  SetJsonValue(g, value, bvp);
  return value;
} // end of ExpandArray

/***********************************************************************/
/*  CalculateArray:                                                    */
/***********************************************************************/
PVAL BCUTIL::CalculateArray(PGLOBAL g, PBVAL arp, int n)
{
  int    i, ars, nv = 0, nextsame = Tp->NextSame;
  bool   err;
  int    nod = Cp->Nod;
  JNODE *nodes = Cp->Nodes;
  OPVAL  op = nodes[n].Op;
  PVAL   val[2], vp = nodes[n].Valp, mulval = Cp->MulVal;
  PBVAL  jvrp, jvp;
  BVAL   jval;

  vp->Reset();
  ars = MY_MIN(Tp->Limit, GetArraySize(arp));
  xtrc(1,"CalculateArray: size=%d op=%d nextsame=%d\n", ars, op, nextsame);

  for (i = 0; i < ars; i++) {
    jvrp = GetArrayValue(arp, i);
    xtrc(1, "i=%d nv=%d\n", i, nv);

    if (!IsValueNull(jvrp) || (op == OP_CNC && GetJsonNull())) do {
      if (IsValueNull(jvrp)) {
        SetString(jvrp, PlugDup(G, GetJsonNull()));
        jvp = jvrp;
      } else if (n < nod - 1 && IsJson(jvrp)) {
        Tp->NextSame = nextsame;
        SetValue(&jval, GetColumnValue(g, jvrp, n + 1));
        jvp = &jval;
      } else
        jvp = jvrp;

      xtrc(1, "jvp=%s null=%d\n", GetString(jvp), IsValueNull(jvp) ? 1 : 0);

      if (!nv++) {
        SetJsonValue(g, vp, jvp);
        continue;
      } else
        SetJsonValue(g, mulval, jvp);

      if (!mulval->IsNull()) {
        switch (op) {
        case OP_CNC:
          if (nodes[n].CncVal) {
            val[0] = nodes[n].CncVal;
            err = vp->Compute(g, val, 1, op);
          } // endif CncVal

          val[0] = mulval;
          err = vp->Compute(g, val, 1, op);
          break;
        // case OP_NUM:
        case OP_SEP:
          val[0] = nodes[n].Valp;
          val[1] = mulval;
          err = vp->Compute(g, val, 2, OP_ADD);
          break;
        default:
          val[0] = nodes[n].Valp;
          val[1] = mulval;
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

    } while (Tp->NextSame > nextsame);

  } // endfor i

  if (op == OP_SEP) {
    // Calculate average
    mulval->SetValue(nv);
    val[0] = vp;
    val[1] = mulval;

    if (vp->Compute(g, val, 2, OP_DIV))
      vp->Reset();

  } // endif Op

  Tp->NextSame = nextsame;
  return vp;
} // end of CalculateArray

/***********************************************************************/
/*  GetRow: Get the object containing this column.                     */
/***********************************************************************/
PBVAL BCUTIL::GetRow(PGLOBAL g)
{
  int    nod = Cp->Nod;
  JNODE *nodes = Cp->Nodes;
  PBVAL  val = NULL;
  PBVAL  arp;
  PBVAL  nwr, row = Tp->Row;

  for (int i = 0; i < nod && row; i++) {
    if (i < nod-1 && nodes[i+1].Op == OP_XX)
      break;
    else switch (row->Type) {
    case TYPE_JOB:
      if (!nodes[i].Key)
        // Expected Array was not there, wrap the value
        continue;

      val = GetKeyValue(row, nodes[i].Key);
      break;
    case TYPE_JAR:
      arp = row;

      if (!nodes[i].Key) {
        if (nodes[i].Op == OP_EQ)
          val = GetArrayValue(arp, nodes[i].Rank);
        else
          val = GetArrayValue(arp, nodes[i].Rx);

      } else {
        // Unexpected array, unwrap it as [0]
        val = GetArrayValue(arp, 0);
        i--;
      } // endif Nodes

      break;
    case TYPE_JVAL:
      val = row;
      break;
    default:
      sprintf(g->Message, "Invalid row JSON type %d", row->Type);
      val = NULL;
    } // endswitch Type

    if (val) {
      row = val;
    } else {
      // Construct missing objects
      for (i++; row && i < nod; i++) {
        int type;

        if (nodes[i].Op == OP_XX)
          break;
        else if (!nodes[i].Key)
          // Construct intermediate array
          type = TYPE_JAR;
        else
          type = TYPE_JOB;

        if (row->Type == TYPE_JOB) {
          nwr = AddPair(row, nodes[i - 1].Key, type);
        } else if (row->Type == TYPE_JAR) {
          AddArrayValue(row, (nwr = NewVal(type)));
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


/* -------------------------- Class BSONDEF -------------------------- */

BSONDEF::BSONDEF(void)
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
} // end of BSONDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool BSONDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
  G = g;
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
PTDB BSONDEF::GetTable(PGLOBAL g, MODE m)
{
  if (trace(1))
    htrc("BSON GetTable Pretty=%d Uri=%s\n", Pretty, SVP(Uri));

  if (Catfunc == FNC_COL)
    return new(g)TDBBCL(this);

  PTDBASE tdbp;
  PTXF    txfp = NULL;

  // JSN not used for pretty=1 for insert or delete
  if (Pretty <= 0 || (Pretty == 1 && (m == MODE_READ || m == MODE_UPDATE))) {
    USETEMP tmp = UseTemp();
    bool    map = Mapped && Pretty >= 0 && m != MODE_INSERT &&
      !(tmp != TMP_NO && m == MODE_UPDATE) &&
      !(tmp == TMP_FORCE && (m == MODE_UPDATE || m == MODE_DELETE));

    if (Lrecl) {
      // Allocate the parse work memory
      G = PlugInit(NULL, (size_t)Lrecl * (Pretty < 0 ? 3 : 5));
    } else {
      strcpy(g->Message, "LRECL is not defined");
      return NULL;
    } // endif Lrecl

    if (Pretty < 0) {	 // BJsonfile
      txfp = new(g) BINFAM(this);
    } else if (Uri) {
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
    } else if (map) {
      txfp = new(g) MAPFAM(this);
    } else
      txfp = new(g) DOSFAM(this);

    // Txfp must be set for TDBBSN
    tdbp = new(g) TDBBSN(g, this, txfp);
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

    tdbp = new(g) TDBBSON(g, this, txfp);
  } // endif Pretty

  if (Multiple)
    tdbp = new(g) TDBMUL(tdbp);

  return tdbp;
} // end of GetTable

/* --------------------------- Class TDBBSN -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBBSN class (Pretty < 2)                    */
/***********************************************************************/
TDBBSN::TDBBSN(PGLOBAL g, PBDEF tdp, PTXF txfp) : TDBDOS(tdp, txfp)
{
  Bp = new(g) BTUTIL(tdp->G, this);
  Top = NULL;
  Row = NULL;
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
  Bp->SetPretty(Pretty);
} // end of TDBBSN standard constructor

TDBBSN::TDBBSN(TDBBSN* tdbp) : TDBDOS(NULL, tdbp)
{
  Bp = tdbp->Bp;
  Top = tdbp->Top;
  Row = tdbp->Row;
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
}  // end of TDBBSN copy constructor

// Used for update
PTDB TDBBSN::Clone(PTABS t)
{
  PTDB    tp;
  PBSCOL  cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBBSN(this);

  for (cp1 = (PBSCOL)Columns; cp1; cp1 = (PBSCOL)cp1->GetNext()) {
    cp2 = new(g) BSONCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
  } // endfor cp1

  return tp;
} // end of Clone

/***********************************************************************/
/*  Allocate JSN column description block.                             */
/***********************************************************************/
PCOL TDBBSN::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
  PBSCOL colp = new(g) BSONCOL(g, cdp, this, cprec, n);

  return (colp->ParseJpath(g)) ? NULL : colp;
} // end of MakeCol

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBBSN::InsertSpecialColumn(PCOL colp)
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
int TDBBSN::Cardinality(PGLOBAL g)
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
int TDBBSN::GetMaxSize(PGLOBAL g)
{
  if (MaxSize < 0)
    MaxSize = TDBDOS::GetMaxSize(g) * ((Xcol) ? Limit : 1);

  return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  JSON EstimatedLength. Returns an estimated minimum line length.    */
/***********************************************************************/
int TDBBSN::EstimatedLength(void)
{
  if (AvgLen <= 0)
    return (Lrecl ? Lrecl : 1024) / 8;		// TODO: make it better
  else
    return AvgLen;

} // end of Estimated Length

/***********************************************************************/
/*  OpenDB: Data Base open routine for BSN access method.              */
/***********************************************************************/
bool TDBBSN::OpenDB(PGLOBAL g)
{
  TUSE use = Use;

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open replace it at its beginning.    ???         */
    /*******************************************************************/
    Fpos = -1;
    NextSame = 0;
    SameRow = 0;
  } // endif Use

    /*********************************************************************/
    /*  Open according to logical input/output mode required.            */
    /*********************************************************************/
  if (TDBDOS::OpenDB(g))
    return true;

  if (use == USE_OPEN)
    return false;

  if (Pretty < 0) {
    /*********************************************************************/
    /*  Binary BJSON table.                                              */
    /*********************************************************************/
    xtrc(1, "JSN OpenDB: tdbp=%p tdb=R%d use=%d mode=%d\n",
      this, Tdb_No, Use, Mode);

    // Lrecl is Ok
    size_t linelen = Lrecl;
    MODE   mode = Mode;

    // Buffer must be allocated in G->Sarea
    Mode = MODE_ANY;
    Txfp->AllocateBuffer(Bp->G);
    Mode = mode;

    if (Mode == MODE_INSERT)
      Bp->SubSet(true);
    else
      Bp->MemSave();

    To_Line = Txfp->GetBuf();
    memset(To_Line, 0, linelen);
    xtrc(1, "OpenJSN: R%hd mode=%d To_Line=%p\n", Tdb_No, Mode, To_Line);
  } // endif Pretty

  /***********************************************************************/
  /*  First opening.                                                     */
  /***********************************************************************/
  if (Mode == MODE_INSERT) {
    int type;

    switch (Jmode) {
      case MODE_OBJECT: type = TYPE_JOB;  break;
      case MODE_ARRAY:  type = TYPE_JAR;  break;
      case MODE_VALUE:  type = TYPE_JVAL; break;
      default:
        sprintf(g->Message, "Invalid Jmode %d", Jmode);
        return true;
    } // endswitch Jmode

    Top = Bp->MakeTopTree(g, type);
    Bp->MemSave();
  } // endif Mode

  if (Xcol)
    To_Filter = NULL;              // Not compatible

  return false;
} // end of OpenDB

/***********************************************************************/
/*  SkipHeader: Physically skip first header line if applicable.       */
/*  This is called from TDBDOS::OpenDB and must be executed before     */
/*  Kindex construction if the file is accessed using an index.        */
/***********************************************************************/
bool TDBBSN::SkipHeader(PGLOBAL g)
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
int TDBBSN::ReadDB(PGLOBAL g)
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
      return rc;	// Deferred reading failed

    if (Pretty >= 0) {
      // Recover the memory used for parsing
      Bp->SubSet();

      if ((Row = Bp->ParseLine(g, Pretty, Comma))) {
        Top = Row;
        Row = Bp->FindRow(g);
        SameRow = 0;
        Fpos++;
        M = 1;
        rc = RC_OK;
      } else if (Pretty != 1 || strcmp(To_Line, "]")) {
        Bp->GetMsg(g);
        rc = RC_FX;
      } else
        rc = RC_EF;

    } else { // Here we get a movable Json binary tree
      Bp->MemSet(((BINFAM*)Txfp)->Recsize);  // Useful when updating
      Row = Top = (PBVAL)To_Line;
      Row = Bp->FindRow(g);
      SameRow = 0;
      Fpos++;
      M = 1;
      rc = RC_OK;
    }	// endif Pretty

  } // endif ReadDB

  return rc;
} // end of ReadDB

/***********************************************************************/
/*  PrepareWriting: Prepare the line for WriteDB.                      */
/***********************************************************************/
bool TDBBSN::PrepareWriting(PGLOBAL g)
{
  if (Pretty >= 0) {
    PSZ s;

//  if (!(Top = Bp->MakeTopTree(g, Row->Type)))
//    return true;

    if ((s = Bp->SerialVal(g, Top, Pretty))) {
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
  } else
    ((BINFAM*)Txfp)->Recsize = ((size_t)PlugSubAlloc(Bp->G, NULL, 0)
                              - (size_t)To_Line);
  return false;
} // end of PrepareWriting

/***********************************************************************/
/*  WriteDB: Data Base write routine for JSON access method.           */
/***********************************************************************/
int TDBBSN::WriteDB(PGLOBAL g) {
  int rc = TDBDOS::WriteDB(g);

  Bp->SubSet();
  Bp->Clear(Row);
  return rc;
} // end of WriteDB

/***********************************************************************/
/*  Data Base close routine for JSON access method.                    */
/***********************************************************************/
void TDBBSN::CloseDB(PGLOBAL g)
{
  TDBDOS::CloseDB(g);
  Bp->G = PlugExit(Bp->G);                  
} // end of CloseDB

/* ---------------------------- BSONCOL ------------------------------ */

/***********************************************************************/
/*  BSONCOL public constructor.                                        */
/***********************************************************************/
BSONCOL::BSONCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
        : DOSCOL(g, cdp, tdbp, cprec, i, "DOS")
{
  Tbp = (TDBBSN*)(tdbp->GetOrig() ? tdbp->GetOrig() : tdbp);
  Cp = new(g) BCUTIL(((PBDEF)Tbp->To_Def)->G, this, Tbp);
  Jpath = cdp->GetFmt();
  MulVal = NULL;
  Nodes = NULL;
  Nod = 0;
  Sep = Tbp->Sep;
  Xnod = -1;
  Xpd = false;
  Parsed = false;
  Warned = false;
  Sgfy = false;
} // end of BSONCOL constructor

/***********************************************************************/
/*  BSONCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
BSONCOL::BSONCOL(BSONCOL* col1, PTDB tdbp) : DOSCOL(col1, tdbp)
{
  Tbp = col1->Tbp;
  Cp = col1->Cp;
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
} // end of BSONCOL copy constructor

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool BSONCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
{
  if (DOSCOL::SetBuffer(g, value, ok, check))
    return true;

  // Parse the json path
  if (ParseJpath(g))
    return true;

  Tbp = (TDBBSN*)To_Tdb;
  return false;
} // end of SetBuffer

/***********************************************************************/
/*  Check whether this object is expanded.                             */
/***********************************************************************/
bool BSONCOL::CheckExpand(PGLOBAL g, int i, PSZ nm, bool b)
{
  if ((Tbp->Xcol && nm && !strcmp(nm, Tbp->Xcol) &&
    (Tbp->Xval < 0 || Tbp->Xval == i)) || Xpd) {
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
bool BSONCOL::SetArrayOptions(PGLOBAL g, char* p, int i, PSZ nm)
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
        jnp->Rank = Tbp->B;
        jnp->Op = OP_EQ;
      } else if (!Value->IsTypeNum()) {
        jnp->CncVal = AllocateValue(g, (void*)", ", TYPE_STRING);
        jnp->Op = OP_CNC;
      } else
        jnp->Op = OP_ADD;

    } // endif OP

  } else if (dg) {
    // Return nth value
    jnp->Rank = atoi(p) - Tbp->B;
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
      if (!Tbp->Xcol && nm) {
        Xpd = true;
        jnp->Op = OP_EXP;
        Tbp->Xval = i;
        Tbp->Xcol = nm;
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
bool BSONCOL::ParseJpath(PGLOBAL g)
{
  char* p, * p1 = NULL, * p2 = NULL, * pbuf = NULL;
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
    for (PBSCOL colp = (PBSCOL)Tbp->GetColumns(); colp;
      colp = (PBSCOL)colp->GetNext())
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
      else if (Xpd && Tbp->Mode == MODE_DELETE) {
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
PSZ BSONCOL::GetJpath(PGLOBAL g, bool proj)
{
  if (Jpath) {
    char* p1, * p2, * mgopath;
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

      default:
        *p2++ = *p1;
        break;
      } // endswitch p1;

      if (*(p2 - 1) == '.')
        p2--;

      *p2 = 0;
      return mgopath;
  } else
    return NULL;

} // end of GetJpath

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void BSONCOL::ReadColumn(PGLOBAL g)
{
  if (!Tbp->SameRow || Xnod >= Tbp->SameRow)
    Value->SetValue_pval(Cp->GetColumnValue(g, Tbp->Row, 0));

#if defined(DEVELOPMENT)
  if (Xpd && Value->IsNull() && !((PBDEF)Tbp->To_Def)->Accept)
    htrc("Null expandable JSON value for column %s\n", Name);
#endif   // DEVELOPMENT

  // Set null when applicable
  if (!Nullable)
    Value->SetNull(false);

} // end of ReadColumn

/***********************************************************************/
/*  WriteColumn:                                                       */
/***********************************************************************/
void BSONCOL::WriteColumn(PGLOBAL g)
{
  if (Xpd && Tbp->Pretty < 2) {
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
  if (Value->IsNull() && Tbp->Mode == MODE_INSERT)
    return;

  PBVAL jsp, row = Cp->GetRow(g);

  if (row) switch (Buf_Type) {
  case TYPE_STRING:
  case TYPE_DATE:
  case TYPE_INT:
  case TYPE_TINY:
  case TYPE_SHORT:
  case TYPE_BIGINT:
  case TYPE_DOUBLE:
    if (Buf_Type == TYPE_STRING && Nodes[Nod - 1].Op == OP_XX) {
      char *s = Value->GetCharValue();

      if (!(jsp = Cp->ParseJson(g, s, strlen(s)))) {
        strcpy(g->Message, s);
        throw 666;
      } // endif jsp

      switch (row->Type) {
        case TYPE_JAR:
          if (Nod > 1 && Nodes[Nod - 2].Op == OP_EQ)
            Cp->SetArrayValue(row, jsp, Nodes[Nod - 2].Rank);
          else
            Cp->AddArrayValue(row, jsp);

          break;
        case TYPE_JOB:  
          if (Nod > 1 && Nodes[Nod - 2].Key)
            Cp->SetKeyValue(row, jsp, Nodes[Nod - 2].Key);

          break;
        case TYPE_JVAL:
        default: 
          Cp->SetValueVal(row, jsp);
      } // endswitch Type

      break;
    } else
      jsp = Cp->NewVal(Value);

    switch (row->Type) {
      case TYPE_JAR:
        if (Nodes[Nod - 1].Op == OP_EQ)
          Cp->SetArrayValue(row, jsp, Nodes[Nod - 1].Rank);
        else
          Cp->AddArrayValue(row, jsp);

        break;
      case TYPE_JOB:
        if (Nodes[Nod - 1].Key)
          Cp->SetKeyValue(row, jsp, Nodes[Nod - 1].Key);

        break;
      case TYPE_JVAL:
      default:
        Cp->SetValueVal(row, jsp);
    } // endswitch Type

    break;
  default:                  // ??????????
    sprintf(g->Message, "Invalid column type %d", Buf_Type);
  } // endswitch Type

} // end of WriteColumn

/* -------------------------- Class TDBBSON -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBBSON class.                               */
/***********************************************************************/
TDBBSON::TDBBSON(PGLOBAL g, PBDEF tdp, PTXF txfp) : TDBBSN(g, tdp, txfp)
{
  Docp = NULL;
  Docrow = NULL;
  Multiple = tdp->Multiple;
  Docsize = 0;
  Done = Changed = false;
  Bp->SetPretty(2);
} // end of TDBBSON standard constructor

TDBBSON::TDBBSON(PBTDB tdbp) : TDBBSN(tdbp)
{
  Docp = tdbp->Docp;
  Docrow = tdbp->Docrow;
  Multiple = tdbp->Multiple;
  Docsize = tdbp->Docsize;
  Done = tdbp->Done;
  Changed = tdbp->Changed;
} // end of TDBBSON copy constructor

// Used for update
PTDB TDBBSON::Clone(PTABS t)
{
  PTDB    tp;
  PBSCOL   cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBBSON(this);

  for (cp1 = (PBSCOL)Columns; cp1; cp1 = (PBSCOL)cp1->GetNext()) {
    cp2 = new(g) BSONCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
  } // endfor cp1

  return tp;
} // end of Clone

/***********************************************************************/
/*  Make the document tree from the object path.                       */
/***********************************************************************/
int TDBBSON::MakeNewDoc(PGLOBAL g)
{
  // Create a void table that will be populated
  Docp = Bp->NewVal(TYPE_JAR);

  if (!(Top = Bp->MakeTopTree(g, TYPE_JAR)))
    return RC_FX;

  Docp = Row;
  Done = true;
  return RC_OK;
} // end of MakeNewDoc

/***********************************************************************/
/*  Make the document tree from a file.                                */
/***********************************************************************/
int TDBBSON::MakeDocument(PGLOBAL g)
{
  char   *p, *p1, *p2, *memory, *objpath, *key = NULL;
  int     i = 0;
  size_t  len;
  my_bool a;
  MODE    mode = Mode;
  PBVAL   jsp;
  PBVAL   objp = NULL;
  PBVAL   arp = NULL;
  PBVAL   val = NULL;

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
  jsp = Top = Bp->ParseJson(g, memory, len);
  Txfp->CloseTableFile(g, false);
  Mode = mode;             // Restore saved Mode

  if (!jsp && g->Message[0])
    return RC_FX;

  if ((objpath = PlugDup(g, Objname))) {
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
        if (jsp->Type != TYPE_JOB) {
          strcpy(g->Message, "Table path does not match the json file");
          return RC_FX;
        } // endif Type

        key = p;
        objp = jsp;
        arp = NULL;
        val = Bp->GetKeyValue(objp, key);

        if (!val || !(jsp = Bp->GetBson(val))) {
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

        if (jsp->Type != TYPE_JAR) {
          strcpy(g->Message, "Table path does not match the json file");
          return RC_FX;
        } // endif Type

        arp = jsp;
        objp = NULL;
        i = atoi(p) - B;
        val = Bp->GetArrayValue(arp, i);

        if (!val) {
          sprintf(g->Message, "Cannot find array value %d", i);
          return RC_FX;
        } // endif val

      } // endif

      jsp = val;
    } // endfor p

  } // endif objpath

  if (jsp && jsp->Type == TYPE_JAR)
    Docp = jsp;
  else {
    // The table is void or is just one object or one value
    if (objp) {
      Docp = Bp->GetKeyValue(objp, key);
      Docp->To_Val = Bp->MOF(Bp->DupVal(Docp));
      Docp->Type = TYPE_JAR;
    } else if (arp) {
      Docp = Bp->NewVal(TYPE_JAR);
      Bp->AddArrayValue(Docp, jsp);
      Bp->SetArrayValue(arp, Docp, i);
    } else {
      Top = Docp = Bp->NewVal(TYPE_JAR);
      Bp->AddArrayValue(Docp, jsp);
    } // endif's

  } // endif jsp

  Docsize = Bp->GetSize(Docp);
  Done = true;
  return RC_OK;
} // end of MakeDocument

/***********************************************************************/
/*  JSON Cardinality: returns table size in number of rows.            */
/***********************************************************************/
int TDBBSON::Cardinality(PGLOBAL g)
{
  if (!g)
    return (Xcol || Multiple) ? 0 : 1;
  else if (Cardinal < 0) {
    if (!Multiple) {
      if (MakeDocument(g) == RC_OK)
        Cardinal = Docsize;

    } else
      return 10;

  } // endif Cardinal

  return Cardinal;
} // end of Cardinality

/***********************************************************************/
/*  JSON GetMaxSize: returns table size estimate in number of rows.    */
/***********************************************************************/
int TDBBSON::GetMaxSize(PGLOBAL g)
{
  if (MaxSize < 0)
    MaxSize = Cardinality(g) * ((Xcol) ? Limit : 1);

  return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  ResetSize: call by TDBMUL when calculating size estimate.          */
/***********************************************************************/
void TDBBSON::ResetSize(void)
{
  MaxSize = Cardinal = -1;
  Fpos = -1;
  N = 0;
  Docrow = NULL;
  Done = false;
} // end of ResetSize

/***********************************************************************/
/*  TDBBSON is not indexable.                                          */
/***********************************************************************/
int TDBBSON::MakeIndex(PGLOBAL g, PIXDEF pxdf, bool)
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
int TDBBSON::GetRecpos(void)
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
bool TDBBSON::SetRecpos(PGLOBAL, int recpos)
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
  Docrow = NULL;
  return false;
} // end of SetRecpos

/***********************************************************************/
/*  JSON Access Method opening routine.                                */
/***********************************************************************/
bool TDBBSON::OpenDB(PGLOBAL g)
{
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open replace it at its beginning.                */
    /*******************************************************************/
    Fpos = -1;
    NextSame = false;
    SameRow = 0;
    Docrow = NULL;
    return false;
  } // endif use

/*********************************************************************/
/*  OpenDB: initialize the JSON file processing.                     */
/*********************************************************************/
  if (MakeDocument(g) != RC_OK)
    return true;

  if (Mode == MODE_INSERT)
    switch (Jmode) {
    case MODE_OBJECT: Row = Bp->NewVal(TYPE_JOB);  break;
    case MODE_ARRAY:  Row = Bp->NewVal(TYPE_JAR);  break;
    case MODE_VALUE:  Row = Bp->NewVal(TYPE_JVAL); break;
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
int TDBBSON::ReadDB(PGLOBAL)
{
  int rc;

  N++;

  if (NextSame) {
    SameRow = NextSame;
    NextSame = false;
    M++;
    rc = RC_OK;
  } else if (++Fpos < Docsize) {
    Docrow = (Docrow) ? Bp->GetNext(Docrow) : Bp->GetArrayValue(Docp, Fpos);
    Row = (Docrow->Type == TYPE_JVAL) ? Bp->GetBson(Docrow) : Docrow;
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
int TDBBSON::WriteDB(PGLOBAL g)
{
  if (Mode == MODE_INSERT) {
    Bp->AddArrayValue(Docp, Row);

    switch(Jmode) {
    case MODE_OBJECT: Row = Bp->NewVal(TYPE_JOB); break;
    case MODE_ARRAY:  Row = Bp->NewVal(TYPE_JAR); break;
    default:          Row = Bp->NewVal();         break;
    } // endswitch Jmode

  } else
    Bp->SetArrayValue(Docp, Row, Fpos);

  Changed = true;
  return RC_OK;
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for JSON access method.              */
/***********************************************************************/
int TDBBSON::DeleteDB(PGLOBAL g, int irc)
{
  if (irc == RC_OK)
    // Deleted current row
    Bp->DeleteValue(Docp, Fpos);
  else if (irc == RC_FX)
    // Delete all
    Docp->To_Val = 0;

  Changed = true;
  return RC_OK;
} // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for JSON access methods.                   */
/***********************************************************************/
void TDBBSON::CloseDB(PGLOBAL g)
{
  if (!Changed)
    return;

  // Save the modified document
  char filename[_MAX_PATH];

//Docp->InitArray(g);

  // We used the file name relative to recorded datapath
  PlugSetPath(filename, ((PBDEF)To_Def)->Fn, GetPath());

  // Serialize the modified table
  if (!Bp->Serialize(g, Top, filename, Pretty))
    puts(g->Message);

} // end of CloseDB

/* ---------------------------TDBBCL class --------------------------- */

/***********************************************************************/
/*  TDBBCL class constructor.                                          */
/***********************************************************************/
TDBBCL::TDBBCL(PBDEF tdp) : TDBCAT(tdp) {
  Topt = tdp->GetTopt();
  Db = tdp->Schema;
  Dsn = tdp->Uri;
} // end of TDBBCL constructor

/***********************************************************************/
/*  GetResult: Get the list the JSON file columns.                     */
/***********************************************************************/
PQRYRES TDBBCL::GetResult(PGLOBAL g) {
  return BSONColumns(g, Db, Dsn, Topt, false);
} // end of GetResult

/* --------------------------- End of json --------------------------- */
