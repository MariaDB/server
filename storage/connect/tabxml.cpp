/************* Tabxml C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABXML                                                */
/* -------------                                                       */
/*  Version 3.0                                                        */
/*                                                                     */
/*  Author Olivier BERTRAND          2007 - 2020                       */
/*                                                                     */
/*  This program are the XML tables classes using MS-DOM or libxml2.   */
/***********************************************************************/

/***********************************************************************/
/*  Include required compiler header files.                            */
/***********************************************************************/
#include "my_global.h"
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#if defined(_WIN32)
#include <io.h>
#include <winsock2.h>
//#include <windows.h>
#include <comdef.h>
#else   // !_WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
//#include <ctype.h>
#include "osutil.h"
#define _O_RDONLY O_RDONLY
#endif  // !_WIN32
#include "resource.h"                        // for IDS_COLUMNS

#define INCLUDE_TDBXML
#define NODE_TYPE_LIST

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  tabdos.h    is header containing the TABDOS class declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
//#include "reldef.h"
#include "xtable.h"
#include "colblk.h"
#include "mycat.h"
#include "xindex.h"
#include "plgxml.h"
#include "tabxml.h"
#include "tabmul.h"

extern "C" char version[];

#if defined(_WIN32) && defined(DOMDOC_SUPPORT)
#define XMLSUP "MS-DOM"
#else   // !_WIN32
#define XMLSUP "libxml2"
#endif  // !_WIN32

#define TYPE_UNKNOWN     12        /* Must be greater than other types */
#define XLEN(M)  sizeof(M) - strlen(M) - 1	       /* To avoid overflow*/

int GetDefaultDepth(void);

/***********************************************************************/
/* Class and structure used by XMLColumns.                             */
/***********************************************************************/
typedef class XMCOL *PXCL;

class XMCOL : public BLOCK {
 public:
  // Constructors
  XMCOL(void) {Next = NULL; 
               Name[0] = 0; 
               Fmt = NULL; 
               Type = 1;
               Len = Scale = 0;
               Cbn = false; 
               Found = true;}
  XMCOL(PGLOBAL g, PXCL xp, char *fmt, int i) {
               Next = NULL; 
               strcpy(Name, xp->Name);
               Fmt = (*fmt) ? PlugDup(g, fmt) : NULL; 
               Type = xp->Type;
               Len = xp->Len;
               Scale = xp->Scale;
               Cbn = (xp->Cbn || i > 1); 
               Found = true;}

  // Members
  PXCL  Next;
  char  Name[64];
  char *Fmt;
  int   Type;
  int   Len;
  int   Scale;
  bool  Cbn;
  bool  Found;
}; // end of class XMCOL

typedef struct LVL {
  PXNODE  pn;
  PXLIST  nl;
  PXATTR  atp;
  bool    b;
  long    k;
  int     m, n;
} *PLVL;

/***********************************************************************/
/* XMLColumns: construct the result blocks containing the description  */
/* of all the columns of a table contained inside an XML file.         */
/***********************************************************************/
PQRYRES XMLColumns(PGLOBAL g, char *db, char *tab, PTOS topt, bool info)
{
  static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING, TYPE_INT, 
                          TYPE_INT, TYPE_SHORT, TYPE_SHORT, TYPE_STRING};
  static XFLD fldtyp[] = {FLD_NAME, FLD_TYPE, FLD_TYPENAME, FLD_PREC, 
                          FLD_LENGTH, FLD_SCALE, FLD_NULL, FLD_FORMAT};
  static unsigned int length[] = {0, 6, 8, 10, 10, 6, 6, 0};
  char    colname[65], fmt[129], buf[512];
  int     i, j, lvl, n = 0;
  int     ncol = sizeof(buftyp) / sizeof(int);
  bool    ok = true;
	PCSZ    fn, op;
  PXCL    xcol, xcp, fxcp = NULL, pxcp = NULL;
  PLVL   *lvlp, vp;
  PXNODE  node = NULL;
  PXMLDEF tdp;
  PTDBXML txmp;
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

	/*********************************************************************/
  /*  Open the input file.                                             */
  /*********************************************************************/
  if (!(fn = GetStringTableOption(g, topt, "Filename", NULL))) {
    if (topt->http) // REST table can have default filename
      fn = GetStringTableOption(g, topt, "Subtype", NULL);
    
    if (!fn) {
      strcpy(g->Message, MSG(MISSING_FNAME));
      return NULL;
    } else
      topt->subtype = NULL;

  } // endif fn

  lvl = GetIntegerTableOption(g, topt, "Level", GetDefaultDepth());
  lvl = GetIntegerTableOption(g, topt, "Depth", lvl);
  lvl = (lvl < 0) ? 0 : (lvl > 16) ? 16 : lvl;

  if (trace(1))
    htrc("File %s lvl=%d\n", topt->filename, lvl);

  tdp = new(g) XMLDEF;
  tdp->Fn = fn;

	if (!(tdp->Database = SetPath(g, db)))
		return NULL;

  tdp->Tabname = tab;
	tdp->Tabname = (char*)GetStringTableOption(g, topt, "Tabname", tab);
	tdp->Rowname = (char*)GetStringTableOption(g, topt, "Rownode", NULL);
	tdp->Zipped = GetBooleanTableOption(g, topt, "Zipped", false);
	tdp->Entry = GetStringTableOption(g, topt, "Entry", NULL);
	tdp->Skip = GetBooleanTableOption(g, topt, "Skipnull", false);

  if (!(op = GetStringTableOption(g, topt, "Xmlsup", NULL)))
#if defined(_WIN32)
    tdp->Usedom = true;
#else   // !_WIN32
    tdp->Usedom = false;
#endif  // !_WIN32
  else
    tdp->Usedom = (toupper(*op) == 'M' || toupper(*op) == 'D');

  txmp = new(g) TDBXML(tdp);

  if (txmp->Initialize(g))
    goto err;

  xcol = new(g) XMCOL;
  colname[64] = 0;
  fmt[128] = 0;
  lvlp = (PLVL*)PlugSubAlloc(g, NULL, sizeof(PLVL) * (lvl + 1));

  for (j = 0; j <= lvl; j++)
    lvlp[j] = (PLVL)PlugSubAlloc(g, NULL, sizeof(LVL));

  /*********************************************************************/
  /*  Analyse the XML tree and define columns.                         */
  /*********************************************************************/
  for (i = 1; ; i++) {
    // Get next row
    switch (txmp->ReadDB(g)) {
      case RC_EF:
        vp = NULL;
        break;
      case RC_FX:
        goto err;
      default:
        vp = lvlp[0];
        vp->pn = txmp->RowNode;
        vp->atp = vp->pn->GetAttribute(g, NULL);
        vp->nl = vp->pn->GetChildElements(g);
        vp->b = true;
        vp->k = 0;
        vp->m = vp->n = 0;
        j = 0;
      } // endswitch ReadDB

    if (!vp)
      break;

    while (true) {
      if (!vp->atp &&
				!(node = (vp->nl) ? vp->nl->GetItem(g, vp->k++, tdp->Usedom ? node : NULL)
				                  : NULL))
        if (j) {
          vp = lvlp[--j];

          if (!tdp->Usedom)    // nl was destroyed
            vp->nl = vp->pn->GetChildElements(g);

          if (!lvlp[j+1]->b) {
            vp->k--;
            ok = false;
            } // endif b

          continue;
        } else
          break;

      xcol->Name[vp->n] = 0;
      fmt[vp->m] = 0;

     more:
      if (vp->atp) {
				size_t z = sizeof(colname) - 1;
        strncpy(colname, vp->atp->GetName(g), z);
				colname[z] = 0;
				strncat(xcol->Name, colname, XLEN(xcol->Name));

        switch (vp->atp->GetText(g, buf, sizeof(buf))) {
          case RC_INFO:
            PushWarning(g, txmp);
          case RC_OK:
            strncat(fmt, "@", XLEN(fmt));
            break;
          default:
            goto err;
          } // enswitch rc

        if (j)
          strncat(fmt, colname, XLEN(fmt));

      } else {
        if (tdp->Usedom && node->GetType() != 1)
          continue;

        strncpy(colname, node->GetName(g), sizeof(colname));
				strncat(xcol->Name, colname, XLEN(xcol->Name));

        if (j)
          strncat(fmt, colname, XLEN(fmt));

        if (j < lvl && ok) {
          vp = lvlp[j+1];
          vp->k = 0;
					vp->pn = node;
					vp->atp = node->GetAttribute(g, NULL);
          vp->nl = node->GetChildElements(g);

          if (tdp->Usedom && vp->nl->GetLength() == 1) {
            node = vp->nl->GetItem(g, 0, node);
            vp->b = (node->GetType() == 1);   // Must be ab element
          } else
            vp->b = (vp->nl && vp->nl->GetLength());

          if (vp->atp || vp->b) {
            if (!vp->atp)
							node = vp->nl->GetItem(g, vp->k++, tdp->Usedom ? node : NULL);

						if (!j)
							strncat(fmt, colname, XLEN(fmt));

						strncat(fmt, "/", XLEN(fmt));
						strncat(xcol->Name, "_", XLEN(xcol->Name));
						j++;
            vp->n = (int)strlen(xcol->Name);
            vp->m = (int)strlen(fmt);
            goto more;
          } else {
            vp = lvlp[j];

            if (!tdp->Usedom)    // nl was destroyed
              vp->nl = vp->pn->GetChildElements(g);

          } // endif vp

        } else
          ok = true;

        switch (node->GetContent(g, buf, sizeof(buf))) {
          case RC_INFO:
            PushWarning(g, txmp);
          case RC_OK:
						xcol->Cbn = !strlen(buf);
            break;
          default:
            goto err;
          } // enswitch rc

      } // endif atp;

      xcol->Len = strlen(buf);

      // Check whether this column was already found
      for (xcp = fxcp; xcp; xcp = xcp->Next)
        if (!strcmp(xcol->Name, xcp->Name))
          break;
  
      if (xcp) {
        if (xcp->Type != xcol->Type)
          xcp->Type = TYPE_STRING;

        if (*fmt && (!xcp->Fmt || strlen(xcp->Fmt) < strlen(fmt))) {
          xcp->Fmt = PlugDup(g, fmt);
          length[7] = MY_MAX(length[7], strlen(fmt));
          } // endif *fmt

        xcp->Len = MY_MAX(xcp->Len, xcol->Len);
        xcp->Scale = MY_MAX(xcp->Scale, xcol->Scale);
        xcp->Cbn |= (xcol->Cbn || !xcol->Len);
        xcp->Found = true;
      } else if(xcol->Len || !tdp->Skip) {
        // New column
        xcp = new(g) XMCOL(g, xcol, fmt, i);
        length[0] = MY_MAX(length[0], strlen(xcol->Name));
        length[7] = MY_MAX(length[7], strlen(fmt));
  
        if (pxcp) {
          xcp->Next = pxcp->Next;
          pxcp->Next = xcp;
        } else
          fxcp = xcp;

        n++;
      } // endif xcp

			if (xcp)
				pxcp = xcp;

      if (vp->atp)
        vp->atp = vp->atp->GetNext(g);

      } // endwhile

    // Missing column can be null
    for (xcp = fxcp; xcp; xcp = xcp->Next) {
      xcp->Cbn |= !xcp->Found;
      xcp->Found = false;
      } // endfor xcp

    } // endor i

  txmp->CloseDB(g);

 skipit:
  if (trace(1))
    htrc("XMLColumns: n=%d len=%d\n", n, length[0]);

  /*********************************************************************/
  /*  Allocate the structures used to refer to the result set.         */
  /*********************************************************************/
  qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
                          buftyp, fldtyp, length, false, false);

  crp = qrp->Colresp->Next->Next->Next->Next->Next->Next;
  crp->Name = "Nullable";
  crp->Next->Name = "Xpath";

  if (info || !qrp)
    return qrp;

  qrp->Nblin = n;

  /*********************************************************************/
  /*  Now get the results into blocks.                                 */
  /*********************************************************************/
  for (i = 0, xcp = fxcp; xcp; i++, xcp = xcp->Next) {
    if (xcp->Type == TYPE_UNKNOWN)            // Void column
      xcp->Type = TYPE_STRING;

    crp = qrp->Colresp;                    // Column Name
    crp->Kdata->SetValue(xcp->Name, i);
    crp = crp->Next;                       // Data Type
    crp->Kdata->SetValue(xcp->Type, i);
    crp = crp->Next;                       // Type Name
    crp->Kdata->SetValue(GetTypeName(xcp->Type), i);
    crp = crp->Next;                       // Precision
    crp->Kdata->SetValue(xcp->Len, i);
    crp = crp->Next;                       // Length
    crp->Kdata->SetValue(xcp->Len, i);
    crp = crp->Next;                       // Scale (precision)
    crp->Kdata->SetValue(xcp->Scale, i);
    crp = crp->Next;                       // Nullable
    crp->Kdata->SetValue(xcp->Cbn ? 1 : 0, i);
    crp = crp->Next;                       // Field format

    if (crp->Kdata)
      crp->Kdata->SetValue(xcp->Fmt, i);

    } // endfor i

  /*********************************************************************/
  /*  Return the result pointer.                                       */
  /*********************************************************************/
  return qrp;

err:
  txmp->CloseDB(g);
  return NULL;
  } // end of XMLColumns

/* -------------- Implementation of the XMLDEF class  ---------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
XMLDEF::XMLDEF(void)
  {
  Pseudo = 3;
  Fn = NULL;
  Encoding = NULL;
  Tabname = NULL;
  Rowname = NULL;
  Colname = NULL;
  Mulnode = NULL;
  XmlDB = NULL;
  Nslist = NULL;
  DefNs = NULL;
  Attrib = NULL;
  Hdattr = NULL;
	Entry = NULL;
  Coltype = 1;
  Limit = 0;
  Header = 0;
  Xpand = false;
  Usedom = false;
	Zipped = false;
	Mulentries = false;
	Skip = false;
  } // end of XMLDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool XMLDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
	PCSZ defrow, defcol;
	char buf[10];

  Fn = GetStringCatInfo(g, "Filename", NULL);
  Encoding = GetStringCatInfo(g, "Encoding", "UTF-8");

  if (*Fn == '?') {
    strcpy(g->Message, MSG(MISSING_FNAME));
    return true;
    } // endif fn

  if ((signed)GetIntCatInfo("Flag", -1) != -1) {
    strcpy(g->Message, MSG(DEPREC_FLAG));
    return true;
    } // endif flag

  defrow = defcol = NULL;
  GetCharCatInfo("Coltype", "", buf, sizeof(buf));

  switch (toupper(*buf)) {
    case 'A':                          // Attribute
    case '@':
    case '0':
      Coltype = 0;
      break;
    case '\0':                         // Default
    case 'T':                          // Tag
    case 'N':                          // Node
    case '1':
      Coltype = 1;
      break;
    case 'C':                          // Column
    case 'P':                          // Position
    case 'H':                          // HTML
    case '2':
      Coltype = 2;
      defrow = "TR";
      defcol = "TD";
      break;
    default:
      sprintf(g->Message, MSG(INV_COL_TYPE), buf);
      return true;
    } // endswitch typname

  Tabname = GetStringCatInfo(g, "Name", Name);  // Deprecated
  Tabname = GetStringCatInfo(g, "Table_name", Tabname); // Deprecated
  Tabname = GetStringCatInfo(g, "Tabname", Tabname);
  Rowname = GetStringCatInfo(g, "Rownode", defrow);
  Colname = GetStringCatInfo(g, "Colnode", defcol);
  Mulnode = GetStringCatInfo(g, "Mulnode", NULL);
  XmlDB = GetStringCatInfo(g, "XmlDB", NULL);
  Nslist = GetStringCatInfo(g, "Nslist", NULL);
  DefNs = GetStringCatInfo(g, "DefNs", NULL);
  Limit = GetIntCatInfo("Limit", 50);
  Xpand = GetBoolCatInfo("Expand", false);
  Header = GetIntCatInfo("Header", 0);
  GetCharCatInfo("Xmlsup", "*", buf, sizeof(buf));

  // Note that if no support is specified, the default is MS-DOM
  // on Windows and libxml2 otherwise
  if (*buf == '*')
#if defined(_WIN32)
    Usedom = true;
#else   // !_WIN32
    Usedom = false;
#endif  // !_WIN32
  else
    Usedom = (toupper(*buf) == 'M' || toupper(*buf) == 'D');

  // Get eventual table node attribute
  Attrib = GetStringCatInfo(g, "Attribute", NULL);
  Hdattr = GetStringCatInfo(g, "HeadAttr", NULL);

	// Specific for zipped files
	if ((Zipped = GetBoolCatInfo("Zipped", false)))
		Mulentries = ((Entry = GetStringCatInfo(g, "Entry", NULL)))
		? strchr(Entry, '*') || strchr(Entry, '?')
		: GetBoolCatInfo("Mulentries", false);

	return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB XMLDEF::GetTable(PGLOBAL g, MODE m)
  {
  if (Catfunc == FNC_COL)
    return new(g) TDBXCT(this);

	if (Zipped && !(m == MODE_READ || m == MODE_ANY)) {
		strcpy(g->Message, "ZIpped XML tables are read only");
		return NULL;
	}	// endif Zipped

  PTDBASE tdbp = new(g) TDBXML(this);

  if (Multiple)
    tdbp = new(g) TDBMUL(tdbp);

  return tdbp;
  } // end of GetTable

/* ------------------------- TDBXML Class ---------------------------- */

/***********************************************************************/
/*  Implementation of the TDBXML constuctor.                           */
/***********************************************************************/
TDBXML::TDBXML(PXMLDEF tdp) : TDBASE(tdp)
  {
  Docp = NULL;
  Root = NULL;
  Curp = NULL;
  DBnode = NULL;
  TabNode = NULL;
  RowNode = NULL;
  ColNode = NULL;
  Nlist = NULL;
  Clist = NULL;
  To_Xb = NULL;
  Colp = NULL;
  Xfile = tdp->Fn;
  Enc = tdp->Encoding;
  Tabname = tdp->Tabname;
#if 0 // why all these?
  Rowname = (tdp->Rowname) ? tdp->Rowname : NULL;
  Colname = (tdp->Colname) ? tdp->Colname : NULL;
  Mulnode = (tdp->Mulnode) ? tdp->Mulnode : NULL;
  XmlDB = (tdp->XmlDB) ? tdp->XmlDB : NULL;
  Nslist = (tdp->Nslist) ? tdp->Nslist : NULL;
  DefNs = (tdp->DefNs) ? tdp->DefNs : NULL;
  Attrib = (tdp->Attrib) ? tdp->Attrib : NULL;
  Hdattr = (tdp->Hdattr) ? tdp->Hdattr : NULL;
#endif // 0
	Rowname = tdp->Rowname;
	Colname = tdp->Colname;
	Mulnode = tdp->Mulnode;
	XmlDB = tdp->XmlDB;
	Nslist = tdp->Nslist;
	DefNs = tdp->DefNs;
	Attrib = tdp->Attrib;
	Hdattr = tdp->Hdattr;
	Entry = tdp->Entry;
	Coltype = tdp->Coltype;
  Limit = tdp->Limit;
  Xpand = tdp->Xpand;
	Zipped = tdp->Zipped;
	Mulentries = tdp->Mulentries;
	Changed = false;
  Checked = false;
  NextSame = false;
  NewRow = false;
  Hasnod = false;
  Write = false;
  Bufdone = false;
  Nodedone = false;
  Void = false;
  Usedom = tdp->Usedom;
  Header = tdp->Header;
  Multiple = tdp->Multiple;
  Nrow = -1;
  Irow = Header - 1;
  Nsub = 0;
  N = 0;
  } // end of TDBXML constructor

TDBXML::TDBXML(PTDBXML tdbp) : TDBASE(tdbp)
  {
  Docp = tdbp->Docp;
  Root = tdbp->Root;
  Curp = tdbp->Curp;
  DBnode = tdbp->DBnode;
  TabNode = tdbp->TabNode;
  RowNode = tdbp->RowNode;
  ColNode = tdbp->ColNode;
  Nlist = tdbp->Nlist;
  Clist = tdbp->Clist;
  To_Xb = tdbp->To_Xb;
  Colp = tdbp->Colp;
  Xfile = tdbp->Xfile;
  Enc = tdbp->Enc;
  Tabname = tdbp->Tabname;
  Rowname = tdbp->Rowname;
  Colname = tdbp->Colname;
  Mulnode = tdbp->Mulnode;
  XmlDB = tdbp->XmlDB;
  Nslist = tdbp->Nslist;
  DefNs = tdbp->DefNs;
  Attrib = tdbp->Attrib;
  Hdattr = tdbp->Hdattr;
	Entry = tdbp->Entry;
	Coltype = tdbp->Coltype;
  Limit = tdbp->Limit;
  Xpand = tdbp->Xpand;
	Zipped = tdbp->Zipped;
	Mulentries = tdbp->Mulentries;
	Changed = tdbp->Changed;
  Checked = tdbp->Checked;
  NextSame = tdbp->NextSame;
  NewRow = tdbp->NewRow;
  Hasnod = tdbp->Hasnod;
  Write = tdbp->Write;
  Void = tdbp->Void;
  Usedom = tdbp->Usedom;
  Header = tdbp->Header;
  Multiple = tdbp->Multiple;
  Nrow = tdbp->Nrow;
  Irow = tdbp->Irow;
  Nsub = tdbp->Nsub;
  N = tdbp->N;
  } // end of TDBXML copy constructor

// Used for update
PTDB TDBXML::Clone(PTABS t)
  {
  PTDB    tp;
  PXMLCOL cp1, cp2;
  PGLOBAL g = t->G;

  tp = new(g) TDBXML(this);

  for (cp1 = (PXMLCOL)Columns; cp1; cp1 = (PXMLCOL)cp1->GetNext()) {
    cp2 = new(g) XMLCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

/***********************************************************************/
/*  Must not be in tabxml.h because of OEM tables                      */
/***********************************************************************/
const CHARSET_INFO *TDBXML::data_charset()
{
	return &my_charset_utf8_general_ci;
}	// end of data_charset

/***********************************************************************/
/*  Allocate XML column description block.                             */
/***********************************************************************/
PCOL TDBXML::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  if (trace(1))
    htrc("TDBXML: MakeCol %s n=%d\n", (cdp) ? cdp->GetName() : "<null>", n);

  return new(g) XMLCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBXML::InsertSpecialColumn(PCOL colp)
  {
  if (!colp->IsSpecial())
    return NULL;

//if (Xpand && ((SPCBLK*)colp)->GetRnm())
//  colp->SetKey(0);               // Rownum is no more a key

  colp->SetNext(Columns);
  Columns = colp;
  return colp;
  } // end of InsertSpecialColumn

/***********************************************************************/
/*  LoadTableFile: Load and parse an XML file.                         */
/***********************************************************************/
int TDBXML::LoadTableFile(PGLOBAL g, char *filename)
  {
  int     rc = RC_OK, type = (Usedom) ? TYPE_FB_XML : TYPE_FB_XML2;
  PFBLOCK fp = NULL;
  PDBUSER dup = (PDBUSER)g->Activityp->Aptr;

  if (Docp)
    return rc;               // Already done

  if (trace(1))
    htrc("TDBXML: loading %s\n", filename);

  /*********************************************************************/
  /*  Firstly we check whether this file have been already loaded.     */
  /*********************************************************************/
  if ((Mode == MODE_READ || Mode == MODE_ANY) && !Zipped) 
    for (fp = dup->Openlist; fp; fp = fp->Next)
      if (fp->Type == type && fp->Length && fp->Count)
        if (!stricmp(fp->Fname, filename))
          break;

  if (fp) {
    /*******************************************************************/
    /*  File already loaded. Just increment use count and get pointer. */
    /*******************************************************************/
    fp->Count++;
    Docp = (Usedom) ? GetDomDoc(g, Nslist, DefNs, Enc, fp)
                    : GetLibxmlDoc(g, Nslist, DefNs, Enc, fp);
  } else {
    /*******************************************************************/
    /*  Parse the XML file.                                            */
    /*******************************************************************/
    if (!(Docp = (Usedom) ? GetDomDoc(g, Nslist, DefNs, Enc)
                          : GetLibxmlDoc(g, Nslist, DefNs, Enc)))
      return RC_FX;

    // Initialize the implementation
    if (Docp->Initialize(g, Entry, Zipped)) {
      sprintf(g->Message, MSG(INIT_FAILED), (Usedom) ? "DOM" : "libxml2");
      return RC_FX;
      } // endif init

    if (trace(1))
      htrc("TDBXML: parsing %s rc=%d\n", filename, rc);

    // Parse the XML file
    if (Docp->ParseFile(g, filename)) {
      // Does the file exist?
      int h= global_open(g, MSGID_NONE, filename, _O_RDONLY);

      if (h != -1) {
        rc = (!_filelength(h)) ? RC_EF : RC_INFO;
        close(h);
      } else
        rc = (errno == ENOENT) ? RC_NF : RC_INFO;

      // Cannot make a Xblock until document is made
      return rc;
      } // endif Docp

    /*******************************************************************/
    /*  Link a Xblock. This make possible to reuse already opened docs */
    /*  and also to automatically close them in case of error g->jump. */
    /*******************************************************************/
    fp = Docp->LinkXblock(g, Mode, rc, filename);
  } // endif xp

  To_Xb = fp;                                  // Useful when closing
  return rc;
  } // end of LoadTableFile

/***********************************************************************/
/*  Initialize the processing of the XML file.                         */
/*  Note: this function can be called several times, eventally before  */
/*  the columns are known (from TBL for instance)                      */
/***********************************************************************/
bool TDBXML::Initialize(PGLOBAL g)
  {
  int     rc;
  PXMLCOL colp;

  if (Void)
    return false;

  if (Columns) {
    // Allocate the buffers that will contain node values
    for (colp = (PXMLCOL)Columns; colp; colp = (PXMLCOL)colp->GetNext())
			if (!colp->IsSpecial()) {            // Not a pseudo column
				if (!Bufdone && colp->AllocBuf(g, Mode == MODE_INSERT))
					return true;

				colp->Nx = colp->Sx = -1;
			} // endif Special

    Bufdone = true;
    } // endif Bufdone

#if !defined(UNIX)
	if (!Root) try {
#else
	if (!Root) {
#endif
		char tabpath[64], filename[_MAX_PATH];

		//  We used the file name relative to recorded datapath
		PlugSetPath(filename, Xfile, GetPath());

		// Load or re-use the table file
		rc = LoadTableFile(g, filename);

		if (rc == RC_OK) {
			// Get root node
			if (!(Root = Docp->GetRoot(g))) {
				// This should never happen as load should have failed
				strcpy(g->Message, MSG(EMPTY_DOC));
				goto error;
			} // endif Root

		// If tabname is not an Xpath,
		// construct one that will find it anywhere
			if (!strchr(Tabname, '/'))
				strcat(strcpy(tabpath, "//"), Tabname);
			else
				strcpy(tabpath, Tabname);

			// Evaluate table xpath
			if ((TabNode = Root->SelectSingleNode(g, tabpath))) {
				if (TabNode->GetType() != XML_ELEMENT_NODE) {
					sprintf(g->Message, MSG(BAD_NODE_TYPE), TabNode->GetType());
					goto error;
				} // endif Type

			} else if (Mode == MODE_INSERT && XmlDB) {
				// We are adding a new table to a multi-table file

				// If XmlDB is not an Xpath,
				// construct one that will find it anywhere
				if (!strchr(XmlDB, '/'))
					strcat(strcpy(tabpath, "//"), XmlDB);
				else
					strcpy(tabpath, XmlDB);

				if (!(DBnode = Root->SelectSingleNode(g, tabpath))) {
					// DB node does not exist yet; we cannot create it
					// because we don't know where it should be placed
					sprintf(g->Message, MSG(MISSING_NODE), XmlDB, Xfile);
					goto error;
				} // endif DBnode

				if (!(TabNode = DBnode->AddChildNode(g, Tabname))) {
					sprintf(g->Message, MSG(FAIL_ADD_NODE), Tabname);
					goto error;
				} // endif TabNode

				DBnode->AddText(g, "\n");
			} else {
				TabNode = Root;              // Try this ?
				Tabname = TabNode->GetName(g);
			} // endif's

		} else if (rc == RC_NF || rc == RC_EF) {
			// The XML file does not exist or is void
			if (Mode == MODE_INSERT) {
				// New Document
				char buf[64];

				// Create the XML node
				if (Docp->NewDoc(g, "1.0")) {
					strcpy(g->Message, MSG(NEW_DOC_FAILED));
					goto error;
				} // endif NewDoc

			//  Now we can link the Xblock
				To_Xb = Docp->LinkXblock(g, Mode, rc, filename);

				// Add a CONNECT comment node
				strcpy(buf, " Created by the MariaDB CONNECT Storage Engine");
				Docp->AddComment(g, buf);

				if (XmlDB) {
					// This is a multi-table file
					DBnode = Root = Docp->NewRoot(g, XmlDB);
					DBnode->AddText(g, "\n");
					TabNode = DBnode->AddChildNode(g, Tabname);
					DBnode->AddText(g, "\n");
				} else
					TabNode = Root = Docp->NewRoot(g, Tabname);

				if (TabNode == NULL || Root == NULL) {
					strcpy(g->Message, MSG(XML_INIT_ERROR));
					goto error;
				} else if (SetTabNode(g))
					goto error;

			} else {
				sprintf(g->Message, MSG(FILE_UNFOUND), Xfile);

				if (Mode == MODE_READ) {
					PushWarning(g, this);
					Void = true;
				} // endif Mode

				goto error;
			} // endif Mode

		} else if (rc == RC_INFO) {
			// Loading failed
			sprintf(g->Message, MSG(LOADING_FAILED), Xfile);
			goto error;
		} else // (rc == RC_FX)
			goto error;

		if (!Rowname) {
			for (PXNODE n = TabNode->GetChild(g); n; n = n->GetNext(g))
				if (n->GetType() == XML_ELEMENT_NODE) {
					Rowname = n->GetName(g);
					break;
				} // endif Type

			if (!Rowname)
				Rowname = TabNode->GetName(g);
		} // endif Rowname

			// Get row node list
		if (strcmp(Rowname, Tabname))
			Nlist = TabNode->SelectNodes(g, Rowname);
		else
			Nrow = 1;


		Docp->SetNofree(true);       // For libxml2
#if defined(_WIN32)
	} catch (_com_error e) {
    // We come here if a DOM command threw an error
    char   buf[128];

    rc = WideCharToMultiByte(CP_ACP, 0, e.Description(), -1,
                             buf, sizeof(buf), NULL, NULL);

    if (rc)
      sprintf(g->Message, "%s: %s", MSG(COM_ERROR), buf);
    else
      sprintf(g->Message, "%s hr=%x", MSG(COM_ERROR), e.Error());

    goto error;
#endif   // _WIN32
#if !defined(UNIX)
  } catch(...) {
    // Other errors
    strcpy(g->Message, MSG(XMLTAB_INIT_ERR));
    goto error;
#endif
  } // end of try-catches

  if (Root && Columns && (Multiple || !Nodedone)) {
    // Allocate class nodes to avoid dynamic allocation
    for (colp = (PXMLCOL)Columns; colp; colp = (PXMLCOL)colp->GetNext())
      if (!colp->IsSpecial())            // Not a pseudo column
        colp->AllocNodes(g, Docp);

    Nodedone = true;
    } // endif Nodedone

  if (Nrow < 0)
    Nrow = (Nlist) ? Nlist->GetLength() : 0;
 
  // Init is Ok
  return false;

error:
  if (Docp)
    Docp->CloseDoc(g, To_Xb);

  return !Void;
} // end of Initialize

/***********************************************************************/
/*  Set TabNode attributes or header.                                  */
/***********************************************************************/
bool TDBXML::SetTabNode(PGLOBAL g)
  {
  assert(Mode == MODE_INSERT);

  if (Attrib)
    SetNodeAttr(g, Attrib, TabNode);

  if (Header) {
    PCOLDEF cdp;
    PXNODE  rn, cn;

    if (Rowname) {
      TabNode->AddText(g, "\n\t");
      rn = TabNode->AddChildNode(g, Rowname, NULL);
    } else {
      strcpy(g->Message, MSG(NO_ROW_NODE));
      return true;
    } // endif Rowname

    if (Hdattr)
      SetNodeAttr(g, Hdattr, rn);

    for (cdp = To_Def->GetCols(); cdp; cdp = cdp->GetNext()) {
      rn->AddText(g, "\n\t\t");
      cn = rn->AddChildNode(g, "TH", NULL);
      cn->SetContent(g, (char *)cdp->GetName(), 
                         strlen(cdp->GetName()) + 1);
      } // endfor cdp

    rn->AddText(g, "\n\t");
    } // endif ColType

  return false;
  } // end of SetTabNode

/***********************************************************************/
/*  Set attributes of a table or header node.                          */
/***********************************************************************/
void TDBXML::SetNodeAttr(PGLOBAL g, char *attr, PXNODE node)
  {
  char  *p, *pa, *pn = attr;
  PXATTR an;

  do {
    if ((p = strchr(pn, '='))) {
      pa = pn;
      *p++ = 0;
  
      if ((pn =   strchr(p, ';')))
        *pn++ = 0;
  
      an = node->AddProperty(g, pa, NULL);
      an->SetText(g, p, strlen(p) + 1);
    } else
      break;

    } while (pn);

  } // end of SetNodeAttr

/***********************************************************************/
/*  XML Cardinality: returns table cardinality in number of rows.      */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int TDBXML::Cardinality(PGLOBAL g)
  {
  if (!g)
    return (Multiple || Xpand || Coltype == 2) ? 0 : 1;

  if (Multiple)
    return 10;

  if (Nrow < 0)
    if (Initialize(g))
      return -1;

  return (Void) ? 0 : Nrow - Header;
  } // end of Cardinality

/***********************************************************************/
/*  XML GetMaxSize: returns the number of tables in the database.      */
/***********************************************************************/
int TDBXML::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    if (!Multiple)
      MaxSize = Cardinality(g) * ((Xpand) ? Limit : 1);
    else
      MaxSize = 10;

    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  Return the position in the table.                                  */
/***********************************************************************/
int TDBXML::GetRecpos(void)
  {
  union {
    uint Rpos;
    BYTE Spos[4];
    };

  Rpos = htonl(Irow);
  Spos[0] = (BYTE)Nsub;
  return Rpos;
  } // end of GetRecpos

/***********************************************************************/
/*  RowNumber: return the ordinal number of the current row.           */
/***********************************************************************/
int TDBXML::RowNumber(PGLOBAL g, bool b)
  {
  if (To_Kindex && (Xpand || Coltype == 2) && !b) {
    /*******************************************************************/
    /*  Don't know how to retrieve RowID for expanded XML tables.      */
    /*******************************************************************/
    sprintf(g->Message, MSG(NO_ROWID_FOR_AM),
                        GetAmName(g, GetAmType()));
    return 0;          // Means error
  } else
    return (b || !(Xpand || Coltype == 2)) ? Irow - Header + 1 : N;

  } // end of RowNumber

/***********************************************************************/
/*  XML Access Method opening routine.                                 */
/***********************************************************************/
bool TDBXML::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open replace it at its beginning.                */
    /*******************************************************************/
    if (!To_Kindex) {
      Irow = Header - 1;
      Nsub = 0;
    } else
      /*****************************************************************/
      /*  Table is to be accessed through a sorted index table.        */
      /*****************************************************************/
      To_Kindex->Reset();

    return false;
    } // endif use

  /*********************************************************************/
  /*  OpenDB: initialize the XML file processing.                      */
  /*********************************************************************/
  Write = (Mode == MODE_INSERT || Mode == MODE_UPDATE);

  if (Initialize(g))
    return true;

  NewRow = (Mode == MODE_INSERT);
  Nsub = 0;
  Use = USE_OPEN;       // Do it now in case we are recursively called
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for XML access method.                      */
/***********************************************************************/
int TDBXML::ReadDB(PGLOBAL g)
  {
  bool same;

  if (Void)
    return RC_EF;

  /*********************************************************************/
  /*  Now start the pseudo reading process.                            */
  /*********************************************************************/
  if (To_Kindex) {
    /*******************************************************************/
    /*  Reading is by an index table.                                  */
    /*******************************************************************/
    union {
      uint Rpos;
      BYTE Spos[4];
      };

    int recpos = To_Kindex->Fetch(g);

    switch (recpos) {
      case -1:           // End of file reached
        return RC_EF;
      case -2:           // No match for join
        return RC_NF;
      case -3:           // Same record as last non null one
        same = true;
        return RC_OK;
      default:
        Rpos = recpos;
        Nsub = Spos[0];
        Spos[0] = 0;

        if (Irow != (signed)ntohl(Rpos)) {
          Irow = ntohl(Rpos);
          same = false;
        } else
          same = true;

      } // endswitch recpos

  } else {
    if (trace(1))
      htrc("TDBXML ReadDB: Irow=%d Nrow=%d\n", Irow, Nrow);

    // This is to force the table to be expanded when constructing
    // an index for which the expand column is not specified.
    if (Colp && Irow >= Header) {
      Colp->Eval(g);
      Colp->Reset();
      } // endif Colp

    if (!NextSame) {
      if (++Irow == Nrow)
        return RC_EF;

      same = false;
      Nsub = 0;
    } else {
      // Not sure the multiple column read will be called
      NextSame = false;
      same = true;
      Nsub++;
    } // endif NextSame

    N++;                          // RowID
  } // endif To_Kindex

  if (!same) {
    if (trace(2))
      htrc("TDBXML ReadDB: Irow=%d RowNode=%p\n", Irow, RowNode);

    // Get the new row node
		if (Nlist) {
			if ((RowNode = Nlist->GetItem(g, Irow, RowNode)) == NULL) {
				sprintf(g->Message, MSG(MISSING_ROWNODE), Irow);
				return RC_FX;
			} // endif RowNode

		} else
			RowNode = TabNode;

    if (Colname && Coltype == 2)
      Clist = RowNode->SelectNodes(g, Colname, Clist);

    } // endif same

  return RC_OK;
  } // end of ReadDB

/***********************************************************************/
/*  CheckRow: called On Insert and Update. Must create the Row node    */
/*  if it does not exist (Insert) and update the Clist if called by    */
/*  a column having an Xpath because it can use an existing node that  */
/*  was added while inserting or Updating this row.                    */
/***********************************************************************/
bool TDBXML::CheckRow(PGLOBAL g, bool b)
  {
  if (NewRow && Mode == MODE_INSERT)
    if (Rowname) {
      TabNode->AddText(g, "\n\t");
      RowNode = TabNode->AddChildNode(g, Rowname, RowNode);
    } else {
      strcpy(g->Message, MSG(NO_ROW_NODE));
      return true;
    } // endif Rowname

  if (Colname && (NewRow || b))
    Clist = RowNode->SelectNodes(g, Colname, Clist);

  return NewRow = false;
  } // end of CheckRow

/***********************************************************************/
/*  WriteDB: Data Base write routine for XDB access methods.           */
/***********************************************************************/
int TDBXML::WriteDB(PGLOBAL g)
  {
  if (Mode == MODE_INSERT) {
    if (Hasnod)
      RowNode->AddText(g, "\n\t");

    NewRow = true;
    } // endif Mode

  // Something was changed in the document
  Changed = true;
  return RC_OK;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for XDB access methods.              */
/***********************************************************************/
int TDBXML::DeleteDB(PGLOBAL g, int irc)
  {
	// TODO: Handle null Nlist
  if (irc == RC_FX) {
    // Delete all rows
    for (Irow = 0; Irow < Nrow; Irow++)
      if ((RowNode = Nlist->GetItem(g, Irow, RowNode)) == NULL) {
        sprintf(g->Message, MSG(MISSING_ROWNODE), Irow);
        return RC_FX;
      } else {
        TabNode->DeleteChild(g, RowNode);

        if (Nlist->DropItem(g, Irow))
          return RC_FX;

      } // endif RowNode

    Changed = true;
  } else if (irc != RC_EF) {
    TabNode->DeleteChild(g, RowNode);

    if (Nlist->DropItem(g, Irow))
      return RC_FX;

    Changed = true;
  } // endif's irc

  return RC_OK;
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for XDB access methods.                    */
/***********************************************************************/
void TDBXML::CloseDB(PGLOBAL g)
  {
  if (Docp) {
    if (Changed) {
      char filename[_MAX_PATH];

      // We used the file name relative to recorded datapath
      PlugSetPath(filename, Xfile, GetPath());

      if (Mode == MODE_INSERT)
        TabNode->AddText(g, "\n");

      // Save the modified document
      if (Docp->DumpDoc(g, filename)) {
        PushWarning(g, this);
        Docp->CloseDoc(g, To_Xb);

        // This causes a crash in Diagnostics_area::set_error_status
//				throw (int)TYPE_AM_XML;
			} // endif DumpDoc
      
      } // endif Changed

    // Free the document and terminate XML processing
    Docp->CloseDoc(g, To_Xb);
    } // endif docp

  if (Multiple) {
    // Reset all constants to start a new parse
    Docp = NULL;
    Root = NULL;
    Curp = NULL;
    DBnode = NULL;
    TabNode = NULL;
    RowNode = NULL;
    ColNode = NULL;
    Nlist = NULL;
    Clist = NULL;
    To_Xb = NULL;
    Colp = NULL;
    Changed = false;
    Checked = false;
    NextSame = false;
    NewRow = false;
    Hasnod = false;
    Write = false;
    Nodedone = false;
    Void = false;
    Nrow = -1;
    Irow = Header - 1;
    Nsub = 0;
    N = 0;
    } // endif Multiple

  } // end of CloseDB

// ------------------------ XMLCOL functions ----------------------------

/***********************************************************************/
/*  XMLCOL public constructor.                                        */
/***********************************************************************/
XMLCOL::XMLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
      : COLBLK(cdp, tdbp, i)
  {
  if (cprec) {
    Next = cprec->GetNext();
    cprec->SetNext(this);
  } else {
    Next = tdbp->GetColumns();
    tdbp->SetColumns(this);
  } // endif cprec

  // Set additional XML access method information for column.
  Tdbp = (PTDBXML)tdbp;
  Nl = NULL;
  Nlx = NULL;
  ColNode = NULL;
  ValNode = NULL;
  Cxnp = NULL;
  Vxnp = NULL;
  Vxap = NULL;
  AttNode = NULL;
  Nodes = NULL;
  Nod = 0;
  Inod = -1;
  Mul = false;
  Checked = false;
  Xname = cdp->GetFmt();
  Long = cdp->GetLong();
  Rank = cdp->GetOffset();
  Type = Tdbp->Coltype;
  Nx = -1;
  Sx = -1;
  N = 0;
  Valbuf = NULL;
  To_Val = NULL;
  } // end of XMLCOL constructor

/***********************************************************************/
/*  XMLCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
XMLCOL::XMLCOL(XMLCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  Tdbp = col1->Tdbp;
  Nl = col1->Nl;
  Nlx = col1->Nlx;
  ColNode = col1->ColNode;
  ValNode = col1->ValNode;
  Cxnp = col1->Cxnp;
  Vxnp = col1->Vxnp;
  Vxap = col1->Vxap;
  AttNode = col1->AttNode;
  Nodes = col1->Nodes;
  Nod = col1->Nod;
  Inod = col1->Inod;
  Mul = col1->Mul;
  Checked = col1->Checked;
  Xname = col1->Xname;
  Valbuf = col1->Valbuf;
  Long = col1->Long;
  Rank = col1->Rank;
  Nx = col1->Nx;
  Sx = col1->Sx;
  N = col1->N;
  Type = col1->Type;
  To_Val = col1->To_Val;
  } // end of XMLCOL copy constructor

/***********************************************************************/
/*  Allocate a buffer of the proper size.                              */
/***********************************************************************/
bool XMLCOL::AllocBuf(PGLOBAL g, bool mode)
  {
  if (Valbuf)
    return false;                       // Already done

  return ParseXpath(g, mode);
  } // end of AllocBuf

/***********************************************************************/
/*  Parse the eventual passed Xpath information.                       */
/*  This information can be specified in the Xpath (or Fieldfmt)       */
/*  column option when creating the table. It permits to indicate the  */
/*  position of the node corresponding to that column in a Xpath-like  */
/*  language (but not a truly compliant one).                          */
/***********************************************************************/
bool XMLCOL::ParseXpath(PGLOBAL g, bool mode)
  {
  char *p, *p2, *pbuf = NULL;
  int   i, n = 1, len = strlen(Name);

  len += ((Tdbp->Colname) ? strlen(Tdbp->Colname) : 0);
  len += ((Xname) ? strlen(Xname) : 0);
  pbuf = (char*)PlugSubAlloc(g, NULL, len + 3);
  *pbuf = '\0';

  if (!mode)
    // Take care of an eventual extra column node a la html
    if (Tdbp->Colname) {
      char *p = strstr(Tdbp->Colname, "%d");
      if (p)
        snprintf(pbuf, len + 3, "%.*s%d%s/", (int) (p - Tdbp->Colname), Tdbp->Colname,
            Rank + (Tdbp->Usedom ? 0 : 1), p + 2);
      else
        snprintf(pbuf, len + 3, "%s/", Tdbp->Colname);
    } // endif Colname

  if (Xname) {
    if (Type == 2) {
      sprintf(g->Message, MSG(BAD_COL_XPATH), Name, Tdbp->Name);
      return true;
    } else
      strcat(pbuf, Xname);

    if (trace(1))
      htrc("XMLCOL: pbuf=%s\n", pbuf);

    // For Update or Insert the Xpath must be analyzed
    if (mode) {
      for (i = 0, p = pbuf; (p = strchr(p, '/')); i++, p++)
        Nod++;                       // One path node found

      if (Nod)
        Nodes = (char**)PlugSubAlloc(g, NULL, Nod * sizeof(char*));

      } // endif mode

    // Analyze the Xpath for this column
    for (i = 0, p = pbuf; (p2 = strchr(p, '/')); i++, p = p2 + 1) {
      if (Tdbp->Mulnode && !strncmp(p, Tdbp->Mulnode, p2 - p))
        if (!Tdbp->Xpand && mode) {
          strcpy(g->Message, MSG(CONCAT_SUBNODE));
          return true;
        } else
          Inod = i;                  // Index of multiple node

      if (mode) {
        // For Update or Insert the Xpath must be explicit
        if (strchr("@/.*", *p)) {
          sprintf(g->Message, MSG(XPATH_NOT_SUPP), Name);
          return true;
        } else
          Nodes[i] = p;

        *p2 = '\0';
        } // endif mode

      } // endfor i, p

    if (*p == '/' || *p == '.') {
      sprintf(g->Message, MSG(XPATH_NOT_SUPP), Name);
      return true;
    } else if (*p == '@') {
      p++;                           // Remove the @ if mode
      Type = 0;                      // Column is an attribute
    } else
      Type = 1;                      // Column is a node

    if (!*p)
      strcpy(p, Name);               // Xname is column name

    if (Type && Tdbp->Mulnode && !strcmp(p, Tdbp->Mulnode))
      Inod = Nod;                    // Index of multiple node

    if (mode)                        // Prepare Xname
      pbuf = p;

  } else if (Type == 2) {
    // HTML like table, columns are retrieved by position
    new(this) XPOSCOL(Value);        // Change the class of this column
    Inod = -1;
  } else if (Type == 0 && !mode) {
    strcat(strcat(pbuf, "@"), Name);
  } else {                           // Type == 1
    if (Tdbp->Mulnode && !strcmp(Name, Tdbp->Mulnode))
      Inod = 0;                      // Nod

    strcat(pbuf, Name);
  } // endif,s

  if (Inod >= 0) {
    Tdbp->Colp = this;               // To force expand

    if (Tdbp->Xpand)
      n = Tdbp->Limit;

    new(this) XMULCOL(Value);        // Change the class of this column
    } // endif Inod

  Valbuf = (char*)PlugSubAlloc(g, NULL, n * (Long + 1));

  for (i = 0; i < n; i++)
    Valbuf[Long + (i * (Long + 1))] = '\0';

  if (Type || Nod)
    Tdbp->Hasnod = true;

  if (trace(1))
    htrc("XMLCOL: Xname=%s\n", pbuf);

  // Save the calculated Xpath
  Xname = pbuf;
  return false;
  } // end of ParseXpath

/***********************************************************************/
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool XMLCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
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
  if (To_Tdb->GetOrig()) {
    To_Tdb = (PTDB)To_Tdb->GetOrig();
    Tdbp = (PTDBXML)To_Tdb;   // Specific of XMLCOL

    // Allocate the XML buffer
    if (AllocBuf(g, true))      // In Write mode
      return true;

    } // endif GetOrig

  // Set the Column
  Status = (ok) ? BUF_EMPTY : BUF_NO;
  return false;
  } // end of SetBuffer

/***********************************************************************/
/*  Alloc the nodes that will be used during the whole process.        */
/***********************************************************************/
void XMLCOL::AllocNodes(PGLOBAL g, PXDOC dp)
{
  Cxnp = dp->NewPnode(g);
  Vxnp = dp->NewPnode(g);
  Vxap = dp->NewPattr(g);
} // end of AllocNodes

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the column node    */
/*  from the corresponding table, extract from it the node text and    */
/*  convert it to the column type.                                     */
/***********************************************************************/
void XMLCOL::ReadColumn(PGLOBAL g)
  {
  if (Nx == Tdbp->Irow)
    return;                         // Same row than the last read

  ValNode = Tdbp->RowNode->SelectSingleNode(g, Xname, Vxnp);

  if (ValNode) {
    if (ValNode->GetType() != XML_ELEMENT_NODE &&
        ValNode->GetType() != XML_ATTRIBUTE_NODE) {
      sprintf(g->Message, MSG(BAD_VALNODE), ValNode->GetType(), Name);
			throw (int)TYPE_AM_XML;
		} // endif type

    // Get the Xname value from the XML file
    switch (ValNode->GetContent(g, Valbuf, Long + 1)) {
      case RC_OK:
        break;
      case RC_INFO:
        PushWarning(g, Tdbp);
        break;
      default:
				throw (int)TYPE_AM_XML;
		} // endswitch

    Value->SetValue_psz(Valbuf);
  } else {
    if (Nullable)
      Value->SetNull(true);

    Value->Reset();              // Null value
  } // endif ValNode

  Nx = Tdbp->Irow;
  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: what this routine does is to access the last row of   */
/*  the corresponding table, and rewrite the content corresponding     */
/*  to this column node from the column buffer and type.               */
/***********************************************************************/
void XMLCOL::WriteColumn(PGLOBAL g)
  {
  char  *p, buf[16];
  int    done = 0;
  int   i, n, k = 0;
  PXNODE TopNode = NULL;

  if (trace(2))
    htrc("XML WriteColumn: col %s R%d coluse=%.4X status=%.4X\n",
          Name, Tdbp->GetTdb_No(), ColUse, Status);

  /*********************************************************************/
  /*  Check whether this node must be written.                         */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, false);    // Convert the updated value

  /*********************************************************************/
  /*  If a check pass was done while updating, all node contruction    */
  /*  has been already one.                                            */
  /*********************************************************************/
  if (Status && Tdbp->Checked && !Value->IsNull()) {
    assert (ColNode != NULL);
    assert ((Type ? (void *)ValNode : (void *)AttNode) != NULL);
    goto fin;
    } // endif Checked

  /*********************************************************************/
  /*  On Insert, a Row node must be created for each row;              */
  /*  For columns having an Xpath, the Clist must be updated.          */
  /*********************************************************************/
  if (Tdbp->CheckRow(g, Nod || Tdbp->Colname))
		throw (int)TYPE_AM_XML;

  /*********************************************************************/
  /*  Null values are represented by no node.                          */
  /*********************************************************************/
	if (Value->IsNull())
    return;

  /*********************************************************************/
  /*  Find the column and value nodes to update or insert.             */
  /*********************************************************************/
  if (Tdbp->Clist) {
    n =  Tdbp->Clist->GetLength();
    ColNode = NULL;
  } else {
    n = 1;
    ColNode = Tdbp->RowNode->Clone(g, ColNode);
  } // endif Clist

  ValNode = NULL;

  for (i = 0; i < n; i++) {
    if (Tdbp->Clist)
      ColNode = Tdbp->Clist->GetItem(g, i, Cxnp);

    /*******************************************************************/
    /*  Check whether an Xpath was provided to go to the column node.  */
    /*******************************************************************/
    for (k = 0; k < Nod; k++)
      if ((ColNode = ColNode->SelectSingleNode(g, Nodes[k], Cxnp)))
        TopNode = ColNode;
      else
        break;

    if (ColNode)
      if (Type)
        ValNode = ColNode->SelectSingleNode(g, Xname, Vxnp);
      else
        AttNode = ColNode->GetAttribute(g, Xname, Vxap);

    if (TopNode || ValNode || AttNode)
      break;                      // We found the good column
    else if (Tdbp->Clist)
      ColNode = NULL;

    // refresh CList in case its Listp was freed in SelectSingleNode above
    if (Tdbp->Clist)
      Tdbp->RowNode->SelectNodes(g, Tdbp->Colname, Tdbp->Clist);
    } // endfor i

  /*********************************************************************/
  /*  Create missing nodes.                                            */
  /*********************************************************************/
  if (ColNode == NULL) {
    if (TopNode == NULL)
      if (Tdbp->Clist) {
        Tdbp->RowNode->AddText(g, "\n\t\t");
        ColNode = Tdbp->RowNode->AddChildNode(g, Tdbp->Colname);
        done = 2;
        TopNode = ColNode;
      } else
        TopNode = Tdbp->RowNode;

    for (; k < Nod && TopNode; k++) {
      if (!done) {
        TopNode->AddText(g, "\n\t\t");
        done = 1;
        } // endif done

      ColNode = TopNode->AddChildNode(g, Nodes[k], Cxnp);
      TopNode = ColNode;
      } // endfor k

    if (ColNode == NULL) {
      strcpy(g->Message, MSG(COL_ALLOC_ERR));
			throw (int)TYPE_AM_XML;
		} // endif ColNode

    } // endif ColNode

  if (Type == 1) {
    if (ValNode == NULL) {
      if (done < 2)
        ColNode->AddText(g, "\n\t\t");

      ValNode = ColNode->AddChildNode(g, Xname, Vxnp);
      } // endif ValNode

  } else // (Type == 0)
    if (AttNode == NULL)
      AttNode = ColNode->AddProperty(g, Xname, Vxap);

  if (ValNode == NULL && AttNode == NULL) {
    strcpy(g->Message, MSG(VAL_ALLOC_ERR));
    throw (int)TYPE_AM_XML;
    } // endif ValNode

  /*********************************************************************/
  /*  Get the string representation of Value according to column type. */
  /*********************************************************************/
  p = Value->GetCharString(buf);

  if (strlen(p) > (unsigned)Long) {
    sprintf(g->Message, MSG(VALUE_TOO_LONG), p, Name, Long);
		throw (int)TYPE_AM_XML;
	} else
    strcpy(Valbuf, p);

  /*********************************************************************/
  /*  Updating must be done only when not in checking pass.            */
  /*********************************************************************/
 fin:
  if (Status) {
    if (Type) {
      ValNode->SetContent(g, Valbuf, Long);
    } else
      AttNode->SetText(g, Valbuf, Long);

    } // endif Status

  } // end of WriteColumn

// ------------------------ XMULCOL functions ---------------------------

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the column node    */
/*  from the corresponding table, extract from it the node text and    */
/*  convert it to the column type.                                     */
/***********************************************************************/
void XMULCOL::ReadColumn(PGLOBAL g)
  {
  char *p;
  int   i, len;
  bool  b = Tdbp->Xpand;

  if (Nx != Tdbp->Irow) {                     // New row
    Nl = Tdbp->RowNode->SelectNodes(g, Xname, Nl);

    if ((N = Nl->GetLength())) {
      *(p = Valbuf) = '\0';
      len = Long;

      if (N > Tdbp->Limit) {
        N = Tdbp->Limit;
        sprintf(g->Message, "Multiple values limited to %d", Tdbp->Limit);
        PushWarning(g, Tdbp);
        } // endif N

      for (i = 0; i < N; i++) {
        ValNode = Nl->GetItem(g, i, Vxnp);

        if (ValNode->GetType() != XML_ELEMENT_NODE &&
            ValNode->GetType() != XML_ATTRIBUTE_NODE) {
          sprintf(g->Message, MSG(BAD_VALNODE), ValNode->GetType(), Name);
					throw (int)TYPE_AM_XML;
				} // endif type

        // Get the Xname value from the XML file
        switch (ValNode->GetContent(g, p, (b ? Long : len))) {
          case RC_OK:
            break;
          case RC_INFO:
            PushWarning(g, Tdbp);
            break;
          default:
            throw (int)TYPE_AM_XML;
          } // endswitch

        if (!b) {
          // Concatenate all values
          if (N - i > 1)
            strncat(Valbuf, ", ", len - strlen(p));

          if ((len -= strlen(p)) <= 0)
            break;

          p += strlen(p);
        } else            // Xpand
          p += (Long + 1);

        } // endfor i

      Value->SetValue_psz(Valbuf);
    } else {
      if (Nullable)
        Value->SetNull(true);

      Value->Reset();              // Null value
    } // endif ValNode

  } else if (Sx == Tdbp->Nsub)
    return;                        // Same row
  else                             // Expanded value
    Value->SetValue_psz(Valbuf + (Tdbp->Nsub * (Long + 1)));

  Nx = Tdbp->Irow;
  Sx = Tdbp->Nsub;
  Tdbp->NextSame = (Tdbp->Xpand && N - Sx > 1);
  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: what this routine does is to access the last line     */
/*  read from the corresponding table, and rewrite the field           */
/*  corresponding to this column from the column buffer and type.      */
/***********************************************************************/
void XMULCOL::WriteColumn(PGLOBAL g)
  {
  char  *p, buf[16];
  int    done = 0;
  int   i, n, len, k = 0;
  PXNODE TopNode = NULL;

  if (trace(1))
    htrc("XML WriteColumn: col %s R%d coluse=%.4X status=%.4X\n",
          Name, Tdbp->GetTdb_No(), ColUse, Status);

  /*********************************************************************/
  /*  Check whether this node must be written.                         */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, false);    // Convert the updated value

  if (Value->IsNull())
    return;

  /*********************************************************************/
  /*  If a check pass was done while updating, all node contruction    */
  /*  has been already one.                                            */
  /*********************************************************************/
  if (Status && Tdbp->Checked) {
    assert (ColNode);
    assert ((Type ? (void *)ValNode : (void *)AttNode) != NULL);
    goto fin;
    } // endif Checked

  /*********************************************************************/
  /*  On Insert, a Row node must be created for each row;              */
  /*  For columns having an Xpath, the Clist must be updated.          */
  /*********************************************************************/
  if (Tdbp->CheckRow(g, Nod))
		throw (int)TYPE_AM_XML;

  /*********************************************************************/
  /*  Find the column and value nodes to update or insert.             */
  /*********************************************************************/
  if (Tdbp->Clist) {
    n =  Tdbp->Clist->GetLength();
    ColNode = NULL;
  } else {
    n = 1;
    ColNode = Tdbp->RowNode->Clone(g, ColNode);
  } // endif Clist

  ValNode = NULL;

  for (i = 0; i < n; i++) {
    if (Tdbp->Clist)
      ColNode = Tdbp->Clist->GetItem(g, i, Cxnp);

    /*******************************************************************/
    /*  Check whether an Xpath was provided to go to the column node.  */
    /*******************************************************************/
    for (k = 0; k < Nod; k++) {
      if (k == Inod) {
        // This is the multiple node
        Nlx = ColNode->SelectNodes(g, Nodes[k], Nlx);
        ColNode = Nlx->GetItem(g, Tdbp->Nsub, Cxnp);
      } else
        ColNode = ColNode->SelectSingleNode(g, Nodes[k], Cxnp);

      if (ColNode == NULL)
        break;

      TopNode = ColNode;
      } // endfor k

    if (ColNode)
      if (Inod == Nod) {
        /***************************************************************/
        /*  The node value can be multiple.                            */
        /***************************************************************/
        assert (Type);

        // Get the value Node from the XML list
        Nlx = ColNode->SelectNodes(g, Xname, Nlx);
        len = Nlx->GetLength();

        if (len > 1 && !Tdbp->Xpand) {
          sprintf(g->Message, MSG(BAD_VAL_UPDATE), Name);
					throw (int)TYPE_AM_XML;
				} else
          ValNode = Nlx->GetItem(g, Tdbp->Nsub, Vxnp);

      } else  // Inod != Nod
        if (Type)
          ValNode = ColNode->SelectSingleNode(g, Xname, Vxnp);
        else
          AttNode = ColNode->GetAttribute(g, Xname, Vxap);

    if (TopNode || ValNode || AttNode)
      break;                     // We found the good column
    else if (Tdbp->Clist)
      ColNode = NULL;

    } // endfor i

  /*********************************************************************/
  /*  Create missing nodes.                                            */
  /*********************************************************************/
  if (ColNode == NULL) {
    if (TopNode == NULL)
      if (Tdbp->Clist) {
        Tdbp->RowNode->AddText(g, "\n\t\t");
        ColNode = Tdbp->RowNode->AddChildNode(g, Tdbp->Colname);
        done = 2;
        TopNode = ColNode;
      } else
        TopNode = Tdbp->RowNode;

    for (; k < Nod && TopNode; k++) {
      if (!done) {
        TopNode->AddText(g, "\n\t\t");
        done = 1;
        } // endif done

      ColNode = TopNode->AddChildNode(g, Nodes[k], Cxnp);
      TopNode = ColNode;
      } // endfor k

    if (ColNode == NULL) {
      strcpy(g->Message, MSG(COL_ALLOC_ERR));
			throw (int)TYPE_AM_XML;
		} // endif ColNode

    } // endif ColNode

  if (Type == 1) {
    if (ValNode == NULL) {
      if (done < 2)
        ColNode->AddText(g, "\n\t\t");

      ValNode = ColNode->AddChildNode(g, Xname, Vxnp);
      } // endif ValNode

  } else // (Type == 0)
    if (AttNode == NULL)
      AttNode = ColNode->AddProperty(g, Xname, Vxap);

  if (ValNode == NULL && AttNode == NULL) {
    strcpy(g->Message, MSG(VAL_ALLOC_ERR));
		throw (int)TYPE_AM_XML;
	} // endif ValNode

  /*********************************************************************/
  /*  Get the string representation of Value according to column type. */
  /*********************************************************************/
  p = Value->GetCharString(buf);

  if (strlen(p) > (unsigned)Long) {
    sprintf(g->Message, MSG(VALUE_TOO_LONG), p, Name, Long);
		throw (int)TYPE_AM_XML;
	} else
    strcpy(Valbuf, p);

  /*********************************************************************/
  /*  Updating must be done only when not in checking pass.            */
  /*********************************************************************/
 fin:
  if (Status) {
    if (Type) {
      ValNode->SetContent(g, Valbuf, Long);
    } else
      AttNode->SetText(g, Valbuf, Long);

    } // endif Status

  } // end of WriteColumn

/* ------------------------ XPOSCOL functions ------------------------ */

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the column node    */
/*  from the corresponding table, extract from it the node text and    */
/*  convert it to the column type.                                     */
/***********************************************************************/
void XPOSCOL::ReadColumn(PGLOBAL g)
  {
  if (Nx == Tdbp->Irow)
    return;                         // Same row than the last read

  if (Tdbp->Clist == NULL) {
    strcpy(g->Message, MSG(MIS_TAG_LIST));
		throw (int)TYPE_AM_XML;
	} // endif Clist

  if ((ValNode = Tdbp->Clist->GetItem(g, Rank, Vxnp))) {
    // Get the column value from the XML file
    switch (ValNode->GetContent(g, Valbuf, Long + 1)) {
      case RC_OK:
        break;
      case RC_INFO:
        PushWarning(g, Tdbp);
        break;
      default:
				throw (int)TYPE_AM_XML;
		} // endswitch

    Value->SetValue_psz(Valbuf);
  } else {
    if (Nullable)
      Value->SetNull(true);

    Value->Reset();              // Null value
  } // endif ValNode

  Nx = Tdbp->Irow;
  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: what this routine does is to access the last line     */
/*  read from the corresponding table, and rewrite the field           */
/*  corresponding to this column from the column buffer and type.      */
/***********************************************************************/
void XPOSCOL::WriteColumn(PGLOBAL g)
  {
  char          *p, buf[16];
  int           i, k, n;

  if (trace(1))
    htrc("XML WriteColumn: col %s R%d coluse=%.4X status=%.4X\n",
          Name, Tdbp->GetTdb_No(), ColUse, Status);

  /*********************************************************************/
  /*  Check whether this node must be written.                         */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, false);    // Convert the updated value

  if (Value->IsNull())
    return;

  /*********************************************************************/
  /*  If a check pass was done while updating, all node contruction    */
  /*  has been already one.                                            */
  /*********************************************************************/
  if (Status && Tdbp->Checked) {
    assert (ValNode);
    goto fin;
    } // endif Checked

  /*********************************************************************/
  /*  On Insert, a Row node must be created for each row;              */
  /*  For all columns the Clist must be updated.                       */
  /*********************************************************************/
  if (Tdbp->CheckRow(g, true))
		throw (int)TYPE_AM_XML;

  /*********************************************************************/
  /*  Find the column and value nodes to update or insert.             */
  /*********************************************************************/
  if (Tdbp->Clist == NULL) {
    strcpy(g->Message, MSG(MIS_TAG_LIST));
		throw (int)TYPE_AM_XML;
	} // endif Clist

  n =  Tdbp->Clist->GetLength();
  k = Rank;

  if (!(ValNode = Tdbp->Clist->GetItem(g, k, Vxnp))) {
    /*******************************************************************/
    /*  Create missing column nodes.                                   */
    /*******************************************************************/
    Tdbp->RowNode->AddText(g, "\n\t\t");

    for (i = n; i <= k; i++)
      ValNode = Tdbp->RowNode->AddChildNode(g, Tdbp->Colname, Vxnp);

    assert (ValNode);
    } // endif ValNode

  /*********************************************************************/
  /*  Get the string representation of Value according to column type. */
  /*********************************************************************/
  p = Value->GetCharString(buf);

  if (strlen(p) > (unsigned)Long) {
    sprintf(g->Message, MSG(VALUE_TOO_LONG), p, Name, Long);
		throw (int)TYPE_AM_XML;
	} else
    strcpy(Valbuf, p);

  /*********************************************************************/
  /*  Updating must be done only when not in checking pass.            */
  /*********************************************************************/
 fin:
  if (Status)
    ValNode->SetContent(g, Valbuf, Long);

  } // end of WriteColumn

/* ---------------------------TDBXCT class --------------------------- */

/***********************************************************************/
/*  TDBXCT class constructor.                                          */
/***********************************************************************/
TDBXCT::TDBXCT(PXMLDEF tdp) : TDBCAT(tdp)
  {
  Topt = tdp->GetTopt();
  //Db = (char*)tdp->GetDB();
	Db = (char*)tdp->Schema;
	Tabn = tdp->Tabname;
  } // end of TDBXCT constructor

/***********************************************************************/
/*  GetResult: Get the list the JSON file columns.                     */
/***********************************************************************/
PQRYRES TDBXCT::GetResult(PGLOBAL g)
  {
  return XMLColumns(g, Db, Tabn, Topt, false);
  } // end of GetResult

/* ------------------------ End of Tabxml ---------------------------- */
