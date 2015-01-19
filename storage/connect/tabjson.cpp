/************* tabjson C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: tabxjson     Version 1.0                              */
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
//#include "mycat.h"                           // for FNC_COL
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

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
USETEMP UseTemp(void);

/* -------------------------- Class JSONDEF -------------------------- */

JSONDEF::JSONDEF(void)
{
  Jmode = MODE_OBJECT;
  Objname = NULL;
  Xcol = NULL;
  Limit = 1;
  ReadMode = 0;
} // end of JSONDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool JSONDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
  Jmode = (JMODE)GetIntCatInfo("Jmode", MODE_OBJECT);
  Objname = GetStringCatInfo(g, "Object", NULL);
  Xcol = GetStringCatInfo(g, "Expand", NULL);
  Pretty = GetIntCatInfo("Pretty", 2);
  Limit = GetIntCatInfo("Limit", 10);
  return DOSDEF::DefineAM(g, "DOS", poff);
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB JSONDEF::GetTable(PGLOBAL g, MODE m)
{
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
  } else {
    txfp = new(g) DOSFAM(this);
    tdbp = new(g) TDBJSON(this, txfp);
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
  Row = NULL;
  Colp = NULL;
  Jmode = tdp->Jmode;
  Xcol = tdp->Xcol;
  Fpos = -1;
  Spos = N = 0;
  Limit = tdp->Limit;
  Pretty = tdp->Pretty;
  Strict = tdp->Strict;
  NextSame = false;
  Comma = false;
  SameRow = 0;
  Xval = -1;
  } // end of TDBJSN standard constructor

TDBJSN::TDBJSN(TDBJSN *tdbp) : TDBDOS(NULL, tdbp)
  {
  Row = tdbp->Row;
  Colp = tdbp->Colp;
  Jmode = tdbp->Jmode;
  Xcol = tdbp->Xcol;
  Fpos = tdbp->Fpos;
  Spos = tdbp->Spos;
  N = tdbp->N;
  Limit = tdbp->Limit;
  Pretty = tdbp->Pretty;
  Strict = tdbp->Strict;
  NextSame = tdbp->NextSame;
  Comma = tdbp->Comma;
  SameRow = tdbp->SameRow;
  Xval = tdbp->Xval;
  } // end of TDBJSN copy constructor

// Used for update
PTDB TDBJSN::CopyOne(PTABS t)
  {
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
PCOL TDBJSN::InsertSpecialColumn(PGLOBAL g, PCOL colp)
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
/*  OpenDB: Data Base open routine for JSN access method.              */
/***********************************************************************/
bool TDBJSN::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open replace it at its beginning.                */
    /*******************************************************************/
    for (PJCOL cp = (PJCOL)Columns; cp; cp = (PJCOL)cp->GetNext()) {
      cp->Nx = 0;
      cp->Arp = NULL;
      } // endfor cp

    Fpos= -1;
    Spos = 0;
    NextSame = false;
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

#if defined(WIN32)
#define  Ending  2
#else   // !WIN32
#define  Ending  1
#endif  // !WIN32

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
    SameRow++;
    return RC_OK;
  } else if ((rc = TDBDOS::ReadDB(g)) == RC_OK)
    if (!IsRead() && ((rc = ReadBuffer(g)) != RC_OK)) {
      // Deferred reading failed
    } else if (!(Row = ParseJson(g, To_Line, 
                             strlen(To_Line), Pretty, &Comma))) {
      rc = (Pretty == 1 && !strcmp(To_Line, "]")) ? RC_EF : RC_FX;
    } else {
      SameRow = 0;
      Fpos++;
      rc = RC_OK;
    } // endif's

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  PrepareWriting: Prepare the line for WriteDB.                      */
/***********************************************************************/
  bool TDBJSN::PrepareWriting(PGLOBAL g)
  {
  PSZ s = Serialize(g, Row, NULL, Pretty);

  if (s) {
    if (Comma)
      strcat(s, ",");

    if ((signed)strlen(s) > Lrecl) {
      sprintf(g->Message, "Line would be truncated (lrecl=%d)", Lrecl);
      return true;
    } else
      strcpy(To_Line, s);

    Row->Clear();
    return false;
  } else
    return true;

  } // end of PrepareWriting

/* ----------------------------- JSNCOL ------------------------------- */

/***********************************************************************/
/*  JSNCOL public constructor.                                         */
/***********************************************************************/
JSONCOL::JSONCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
       : DOSCOL(g, cdp, tdbp, cprec, i, "DOS")
  {
  Tjp = (TDBJSN *)(tdbp->GetOrig() ? tdbp->GetOrig() : tdbp);
  Arp = NULL;
  Jpath = cdp->GetFmt();
  MulVal = NULL;
  Nodes = NULL;
  Nod = Nx =0;
  Ival = -1;
  Xpd = false;
  Parsed = false;
  } // end of JSONCOL constructor

/***********************************************************************/
/*  JSONCOL constructor used for copying columns.                      */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
JSONCOL::JSONCOL(JSONCOL *col1, PTDB tdbp) : DOSCOL(col1, tdbp)
  {
  Tjp = col1->Tjp;
  Arp = col1->Arp;
  Jpath = col1->Jpath;
  MulVal = col1->MulVal;
  Nodes = col1->Nodes;
  Nod = col1->Nod;
  Ival = col1->Ival;
  Nx = col1->Nx;
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
  return false;
  } // end of SetBuffer

/***********************************************************************/
/*  Analyse array processing options.                                  */
/***********************************************************************/
bool JSONCOL::CheckExpand(PGLOBAL g, int i, PSZ nm, bool b)
  {
  if (Tjp->Xcol && nm && !strcmp(nm, Tjp->Xcol) &&
     (Tjp->Xval < 0 || Tjp->Xval == i)) {
    Xpd = true;              // Expandable object
    Nodes[i].Op = OP_XX;
    Tjp->Xval = i;
  } else if (b) {
    strcpy(g->Message, "Cannot expand more than one array");
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
  bool   dg = true;
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

    } // endif *p 

  // To check whether a numeric Rank was specified
  for (int k = 0; dg && p[k]; k++)
    dg = isdigit(p[k]) > 0;

  if (!n) {
    // Default specifications
    if (CheckExpand(g, i, nm, false))
      return true;
    else if (jnp->Op != OP_XX)
      if (!Value->IsTypeNum()) {
        jnp->CncVal = AllocateValue(g, ", ", TYPE_STRING);
        jnp->Op = OP_CNC;
      } else
        jnp->Op = OP_ADD;

  } else if (dg) {
    if (atoi(p) > 0) {
      // Return nth value
      jnp->Rank = atoi(p);
      jnp->Op = OP_EQ;
    } else // Ignore array
      jnp->Op = OP_NULL;

  } else if (n == 1) {
    // Set the Op value;
    switch (*p) {
      case '+': jnp->Op = OP_ADD;  break;
      case '*': jnp->Op = OP_MULT; break;
      case '>': jnp->Op = OP_MAX;  break;
      case '<': jnp->Op = OP_MIN;  break;
      case '#': jnp->Op = OP_NUM;  break;
      case '!': jnp->Op = OP_SEP;  break; // Average
      case 'x':
      case 'X': // Expand this array
        if (!Tjp->Xcol && nm) {  
          Xpd = true;
          jnp->Op = OP_XX;
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

  pbuf = (char*)PlugSubAlloc(g, NULL, strlen(Jpath) + 1);
  strcpy(pbuf, Jpath);

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

    } else {
      Nodes[i].Key = p;
      Nodes[i].Op = OP_EXIST;
    } // endif's

    } // endfor i, p

  MulVal = AllocateValue(g, Value);
  Parsed = true;
  return false;
  } // end of ParseJpath

/***********************************************************************/
/*  SetValue: Set a value from a JVALUE contains.                      */
/***********************************************************************/
void JSONCOL::SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val, int n)
  {
  if (val) {
    if (Nodes[n].Op == OP_NUM)
      vp->SetValue(1);
    else {
 again:
      switch (val->GetValType()) {
        case TYPE_STRING:
        case TYPE_INT:
        case TYPE_DOUBLE:
          vp->SetValue_pval(val->GetValue());
          break;
        case TYPE_TINY:
          if (vp->IsTypeNum())
            vp->SetValue(val->GetInteger() ? 1 : 0);
          else
            vp->SetValue_psz(val->GetInteger() ? "true" : "false");
     
          break;
        case TYPE_JAR:
          val = val->GetArray()->GetValue(0);
          goto again;
        case TYPE_JOB:
          if (!vp->IsTypeNum()) {
            vp->SetValue_psz(val->GetObject()->GetText(g));
            break;
            } // endif Type
     
        default:
          vp->Reset();
        } // endswitch Type

      } // endelse

  } else
    vp->Reset();

  } // end of SetJsonValue

/***********************************************************************/
/*  GetRow: Get the object containing this column.                     */
/***********************************************************************/
PJSON JSONCOL::GetRow(PGLOBAL g, int mode)
  {
  PJVAL val;
  PJAR  arp;
  PJSON nwr, row = Tjp->Row;

  for (int i = 0; i < Nod-1 && row; i++) {
    switch (row->GetType()) {
      case TYPE_JOB:
        if (!Nodes[i].Key)
          // Expected Array was not there
          continue;

        val = ((PJOB)row)->GetValue(Nodes[i].Key);
        break;
      case TYPE_JAR:
        if (!Nodes[i].Key) {
          if (Nodes[i].Op != OP_NULL) {
            Ival = i;
            arp = (PJAR)row;

            if (mode < 2)     // First pass
              Arp = arp;

            if (Nodes[i].Op != OP_XX) {
              if (Nodes[i].Rank)
                val = arp->GetValue(Nodes[i].Rank - 1);
              else
                val = arp->GetValue(arp == Arp ? Nx : 0);

            } else
              val = arp->GetValue(Tjp->SameRow);
                       
          } else
            val = NULL;

        } else {
          strcpy(g->Message, "Unexpected array");
          val = NULL;          // Not an expected array
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
    } else if (mode == 1) {          // mode write
      // Construct missing objects
      for (i++; row && i < Nod; i++) {
        if (!Nodes[i].Key) {
          // Construct intermediate array
          nwr = new(g) JARRAY;
        } else {
          nwr = new(g) JOBJECT;
        } // endif Nodes

        if (row->GetType() == TYPE_JOB) {
          ((PJOB)row)->SetValue(g, new(g) JVALUE(nwr), Nodes[i-1].Key);
        } else if (row->GetType() == TYPE_JAR) {
          ((PJAR)row)->AddValue(g, new(g) JVALUE(nwr));
          ((PJAR)row)->InitArray(g);
        } else {
          strcpy(g->Message, "Wrong type when writing new row");
          nwr = NULL;
        } // endif's

        row = nwr;
        } // endfor i

      break;
    } else
      row = NULL;

    } // endfor i

  return row;
  } // end of GetRow

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void JSONCOL::ReadColumn(PGLOBAL g)
  {
  int   mode = 0, n = Nod - 1;
  PJSON row;
  PJVAL val = NULL;

 evenmore:
  row = GetRow(g, mode);

 more:
  if (row) switch (row->GetType()) {
    case TYPE_JOB:
      if (Nodes[n].Key)
        val = row->GetValue(Nodes[n].Key);
      else
        val = new(g) JVALUE(row);

      break;
    case TYPE_JAR:
      // Multiple column ?
      if (Nodes[n].Op != OP_NULL) {
        Arp = (PJAR)row;
        val = Arp->GetValue(Nodes[n].Rank > 0 ?
                            Nodes[n].Rank - 1 : 
                            Nodes[n].Op == OP_XX ? Tjp->SameRow : Nx);
        Ival = n;
      } else
        val = NULL;

      break;
    case TYPE_JVAL:
      val = (PJVAL)row;
      break;
    default:
      sprintf(g->Message, "Wrong return value type %d", row->GetType());
      Value->Reset();
      return;
    } // endswitch Type

  if (!Nx /*|| (Xpd)*/)
    SetJsonValue(g, Value, val, n);

  if (Arp) {
    // Multiple column
    int ars = (Nodes[Ival].Rank > 0) ? 1 : MY_MIN(Tjp->Limit, Arp->size());

    if (Nodes[Ival].Op == OP_XX) {
      if (ars > Tjp->SameRow + 1)
        Tjp->NextSame = true;      // More to come
      else {
        Tjp->NextSame = false;
        Arp = NULL;
      } // endelse

    } else {
      if (Nx && val) {
        SetJsonValue(g, MulVal, val, Ival);

        if (!MulVal->IsZero()) {
          PVAL val[2];
          bool err;

          switch (Nodes[Ival].Op) {
            case OP_CNC:
              if (Nodes[Ival].CncVal) {
                val[0] = Nodes[Ival].CncVal;
                err = Value->Compute(g, val, 1, Nodes[Ival].Op);
                } // endif CncVal

              val[0] = MulVal;
              err = Value->Compute(g, val, 1, Nodes[Ival].Op);
              break;
            case OP_NUM:
            case OP_SEP:
              val[0] = Value;
              val[1] = MulVal;
              err = Value->Compute(g, val, 2, OP_ADD);
              break;
            default:
              val[0] = Value;
              val[1] = MulVal;
              err = Value->Compute(g, val, 2, Nodes[Ival].Op);
            } // endswitch Op

          if (err)
            Value->Reset();

          } // endif Zero

        } // endif Nx

      if (ars > ++Nx) {
        if (Ival != n) {
          mode = 2;
          goto evenmore;
        } else
          goto more;

      } else {
        if (Nodes[Ival].Op == OP_SEP) {
          // Calculate average
          PVAL val[2];

          MulVal->SetValue(ars);
          val[0] = Value;
          val[1] = MulVal;

          if (Value->Compute(g, val, 2, OP_DIV))
            Value->Reset();

          } // endif Op

        Arp = NULL;
        Nx = 0;
      } // endif ars

    } // endif Op

    } // endif Arp

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn:                                                       */
/***********************************************************************/
void JSONCOL::WriteColumn(PGLOBAL g)
  {
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

  PJOB  objp = NULL;
  PJAR  arp = NULL;
  PJVAL jvp = NULL;
  PJSON row = GetRow(g, 1);
  JTYP  type = row->GetType();

  switch (row->GetType()) {
    case TYPE_JOB:  objp = (PJOB)row;  break;
    case TYPE_JAR:  arp  = (PJAR)row;  break;
    case TYPE_JVAL: jvp  = (PJVAL)row; break;
    default: row = NULL;     // ???????????????????????????
    } // endswitch Type

  if (row) switch (Buf_Type) {
    case TYPE_STRING:
    case TYPE_DATE:
    case TYPE_INT:
    case TYPE_DOUBLE:
      if (arp) {
        if (Nodes[Nod-1].Rank)
          arp->SetValue(g, new(g) JVALUE(g, Value), Nodes[Nod-1].Rank-1);
        else
          arp->AddValue(g, new(g) JVALUE(g, Value));

        arp->InitArray(g);
      } else if (objp) {
        if (Nodes[Nod-1].Key)
          objp->SetValue(g, new(g) JVALUE(g, Value), Nodes[Nod-1].Key);

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
  Top = NULL;
  Doc = NULL;
  Objname = tdp->Objname;
  Multiple = tdp->Multiple;
  Done = Changed = false;
  } // end of TDBJSON standard constructor

TDBJSON::TDBJSON(PJTDB tdbp) : TDBJSN(tdbp)
  {
  Top = tdbp->Top;
  Doc = tdbp->Doc;
  Objname = tdbp->Objname;
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
/*  Make the document tree from a file.                                */
/***********************************************************************/
int TDBJSON::MakeNewDoc(PGLOBAL g)
  {
  // Create a void table that will be populated
  Doc = new(g) JARRAY;

  if (Objname) {
    // Parse and allocate Objname item(s)
    char *p;
    char *objpath = (char*)PlugSubAlloc(g, NULL, strlen(Objname)+1);
    int   i;
    PJOB  objp;
    PJAR  arp;
    PJVAL val = NULL;

    strcpy(objpath, Objname);
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
        i = atoi(objpath+1) - 1;
        arp->SetValue(g, val, i);
        arp->InitArray(g);
      } else {
        sprintf(g->Message, "Invalid Table path %s", Objname);
        return RC_FX;
      } // endif objpath

      } // endfor p

    val->SetValue(Doc);
  } else
    Top = Doc;

  return RC_OK;
  } // end of MakeNewDoc

/***********************************************************************/
/*  Make the document tree from a file.                                */
/***********************************************************************/
int TDBJSON::MakeDocument(PGLOBAL g)
  {
  char   *p, *memory, *objpath, *key, filename[_MAX_PATH];
  int     i, len;
  HANDLE  hFile;
	MEMMAP  mm;
  PJSON   jsp;
  PJOB    objp = NULL;
  PJAR    arp = NULL;
  PJVAL   val = NULL;

  if (Done)
    return RC_OK;
  else
    Done = true;

  // Now open the JSON file
  PlugSetPath(filename, Txfp->To_File, GetPath());

	/*********************************************************************/
	/*  Create the mapping file object.                                  */
	/*********************************************************************/
	hFile = CreateFileMap(g, filename, &mm, MODE_READ, false);

	if (hFile == INVALID_HANDLE_VALUE) {
    DWORD drc = GetLastError();

    if (drc != ERROR_FILE_NOT_FOUND || Mode != MODE_INSERT) {
      sprintf(g->Message, MSG(OPEN_ERROR), drc, 10, filename);
      FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                    FORMAT_MESSAGE_IGNORE_INSERTS, NULL, drc, 0,
                    (LPTSTR)filename, sizeof(filename), NULL);
      strcat(g->Message, filename);
  	  return RC_FX;
    } else
      return MakeNewDoc(g);

  	} // endif hFile

  /*********************************************************************/
  /*  Get the file size (assuming file is smaller than 4 GB)           */
  /*********************************************************************/
  len = mm.lenL;
	memory = (char *)mm.memory;

	if (!len) {      				// Empty file
    CloseFileHandle(hFile);
    UnmapViewOfFile(memory);

    if (Mode == MODE_INSERT)
  		return MakeNewDoc(g);

		} // endif len

  if (!memory) {
    CloseFileHandle(hFile);
    sprintf(g->Message, MSG(MAP_VIEW_ERROR), filename, GetLastError());
    return RC_FX;
    } // endif Memory

  CloseFileHandle(hFile);                    // Not used anymore
  hFile = INVALID_HANDLE_VALUE;              // For Fblock

  /*********************************************************************/
  /*  Parse the json file and allocate its tree structure.             */
  /*********************************************************************/
  g->Message[0] = 0;
  jsp = Top = ParseJson(g, memory, len, Pretty);
  UnmapViewOfFile(memory);

  if (!jsp && g->Message[0])
    return RC_FX;

  if (Objname) {
    objpath = (char*)PlugSubAlloc(g, NULL, strlen(Objname) + 1);
    strcpy(objpath, Objname);
  } else
    objpath = NULL;

  /*********************************************************************/
  /*  Find the table in the tree structure.                            */
  /*********************************************************************/
  for (; jsp && objpath; objpath = p) {
    if ((p = strchr(objpath, ':')))
      *p++ = 0;

    if (*objpath != '[') {         // objpass is a key
      if (jsp->GetType() != TYPE_JOB) {
        strcpy(g->Message, "Table path does no match json file");
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
        strcpy(g->Message, "Table path does no match json file");
        return RC_FX;
        } // endif Type

      arp = jsp->GetArray();
      objp = NULL;
      i = atoi(objpath+1) - 1;
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
int TDBJSON::MakeIndex(PGLOBAL g, PIXDEF pxdf, bool add)
  {
  if (pxdf) {
    strcpy(g->Message, "JSON not indexable when pretty = 2");
    return RC_FX;
  } else
    return RC_OK;

  } // end of MakeIndex 

/***********************************************************************/
/*  JSON Access Method opening routine.                                */
/***********************************************************************/
bool TDBJSON::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open replace it at its beginning.                */
    /*******************************************************************/
    for (PJCOL cp = (PJCOL)Columns; cp; cp = (PJCOL)cp->GetNext()) {
      cp->Nx = 0;
      cp->Arp = NULL;
      } // endfor cp

    Fpos= -1;
    Spos = 0;
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
int TDBJSON::ReadDB(PGLOBAL g)
  {
  int   rc;

  N++;

  if (NextSame) {
    SameRow++;
    rc = RC_OK;
  } else if (++Fpos < (signed)Doc->size()) {
    Row = Doc->GetValue(Fpos);

    if (Row->GetType() == TYPE_JVAL)
      Row = ((PJVAL)Row)->GetJson();

    SameRow = 0;
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
    if (Mode == MODE_INSERT)
      Doc->AddValue(g, (PJVAL)Row);
    else if (Doc->SetValue(g, (PJVAL)Row, Fpos))
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
  char  filename[_MAX_PATH];
  PSZ   msg;
  FILE *fop;

  Doc->InitArray(g);

  // We used the file name relative to recorded datapath
  PlugSetPath(filename, ((PJDEF)To_Def)->Fn, GetPath());

  if (!(fop = fopen(filename, "wb"))) {
    sprintf(g->Message, MSG(OPEN_MODE_ERROR),
            "w", (int)errno, filename);
    strcat(strcat(g->Message, ": "), strerror(errno));
  } else {
    // Serialize the modified table
    if ((msg = Serialize(g, Top, fop, Pretty)))
      printf(msg);

    fclose(fop);
  } // endif fop

  } // end of CloseDB

/* -------------------------- End of json --------------------------- */
