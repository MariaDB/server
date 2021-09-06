/************* TabFmt C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABFMT                                                */
/* -------------                                                       */
/*  Version 3.9.3                                                      */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2001 - 2019  */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the TABFMT classes DB execution routines.         */
/*  The base class CSV is comma separated files.                       */
/*  FMT (Formatted) files are those having a complex internal record   */
/*  format described in the Format keyword of their definition.        */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <errno.h>
#include <locale.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#include "osutil.h"
#else
#if defined(UNIX)
#include <errno.h>
#include <unistd.h>
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
/*  tabdos.h    is header containing the TABDOS class declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "mycat.h"
#include "filamap.h"
#if defined(GZ_SUPPORT)
#include "filamgz.h"
#endif   // GZ_SUPPORT
#if defined(ZIP_SUPPORT)
#include "filamzip.h"
#endif   // ZIP_SUPPORT
#include "tabfmt.h"
#include "tabmul.h"
#define  NO_FUNC
#include "plgcnx.h"                       // For DB types
#include "resource.h"

/***********************************************************************/
/*  This should be an option.                                          */
/***********************************************************************/
#define MAXCOL          200        /* Default max column nb in result  */
#define TYPE_UNKNOWN     12        /* Must be greater than other types */

/***********************************************************************/
/*  External function.                                                 */
/***********************************************************************/
USETEMP UseTemp(void);

/***********************************************************************/
/* CSVColumns: constructs the result blocks containing the description */
/* of all the columns of a CSV file that will be retrieved by #GetData.*/
/* Note: the algorithm to set the type is based on the internal values */
/* of types (TYPE_STRING < TYPE_DOUBLE < TYPE_INT) (1 < 2 < 7).        */
/* If these values are changed, this will have to be revisited.        */
/***********************************************************************/
PQRYRES CSVColumns(PGLOBAL g, PCSZ dp, PTOS topt, bool info)
  {
  static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING,
                          TYPE_INT,   TYPE_INT, TYPE_SHORT};
  static XFLD fldtyp[] = {FLD_NAME, FLD_TYPE,   FLD_TYPENAME,
                          FLD_PREC, FLD_LENGTH, FLD_SCALE};
  static unsigned int length[] = {6, 6, 8, 10, 10, 6};
	const char *fn;
	char    sep, q;
	int     rc, mxr;
	bool    hdr;
  char   *p, *colname[MAXCOL], dechar, buf[8];
  int     i, imax, hmax, n, nerr, phase, blank, digit, dec, type;
  int     ncol = sizeof(buftyp) / sizeof(int);
  int     num_read = 0, num_max = 10000000;     // Statistics
  int     len[MAXCOL], typ[MAXCOL], prc[MAXCOL];
	PCSVDEF tdp;
	PTDBCSV tcvp;
	PTDBASE tdbp;
	PQRYRES qrp;
  PCOLRES crp;

  if (info) {
    imax = hmax = 0;
    length[0] = 128;
    goto skipit;
    } // endif info

	//if (GetIntegerTableOption(g, topt, "Multiple", 0)) {
	//	strcpy(g->Message, "Cannot find column definition for multiple table");
	//	return NULL;
	//}	// endif Multiple

//      num_max = atoi(p+1);             // Max num of record to test
  imax = hmax = nerr = 0;

  for (i = 0; i < MAXCOL; i++) {
    colname[i] = NULL;
    len[i] = 0;
    typ[i] = TYPE_UNKNOWN;
    prc[i] = 0;
    } // endfor i

  /*********************************************************************/
  /*  Get the CSV table description block.                             */
  /*********************************************************************/
	tdp = new(g) CSVDEF;
	tdp->Database = dp;

	if ((tdp->Zipped = GetBooleanTableOption(g, topt, "Zipped", false))) {
#if defined(ZIP_SUPPORT)
		tdp->Entry = GetStringTableOption(g, topt, "Entry", NULL);
		tdp->Mulentries = (tdp->Entry)
			              ? strchr(tdp->Entry, '*') || strchr(tdp->Entry, '?')
			              : GetBooleanTableOption(g, topt, "Mulentries", false);
#else   // !ZIP_SUPPORT
		strcpy(g->Message, "ZIP not supported by this version");
		return NULL;
#endif  // !ZIP_SUPPORT
	} // endif // Zipped

	fn = tdp->Fn = GetStringTableOption(g, topt, "Filename", NULL);

	if (!tdp->Fn) {
		strcpy(g->Message, MSG(MISSING_FNAME));
		return NULL;
	} // endif Fn

	if (!(tdp->Lrecl = GetIntegerTableOption(g, topt, "Lrecl", 0)))
		tdp->Lrecl = 4096;

	tdp->Multiple = GetIntegerTableOption(g, topt, "Multiple", 0);
	p = (char*)GetStringTableOption(g, topt, "Separator", ",");
	tdp->Sep = (strlen(p) == 2 && p[0] == '\\' && p[1] == 't') ? '\t' : *p;

#if defined(_WIN32)
	if (tdp->Sep == ',' || strnicmp(setlocale(LC_NUMERIC, NULL), "French", 6))
		dechar = '.';
	else
		dechar = ',';
#else   // !_WIN32
	dechar = '.';
#endif  // !_WIN32

	sep = tdp->Sep;
	tdp->Quoted = GetIntegerTableOption(g, topt, "Quoted", -1);
	p = (char*)GetStringTableOption(g, topt, "Qchar", "");
	tdp->Qot = *p;

	if (tdp->Qot && tdp->Quoted < 0)
		tdp->Quoted = 0;
	else if (!tdp->Qot && tdp->Quoted >= 0)
		tdp->Qot = '"';

	q = tdp->Qot;
	hdr = GetBooleanTableOption(g, topt, "Header", false);
	tdp->Maxerr = GetIntegerTableOption(g, topt, "Maxerr", 0);
	tdp->Accept = GetBooleanTableOption(g, topt, "Accept", false);

	if (tdp->Accept && tdp->Maxerr == 0)
		tdp->Maxerr = INT_MAX32;       // Accept all bad lines

	mxr = MY_MAX(0, tdp->Maxerr);

	if (trace(1))
		htrc("File %s Sep=%c Qot=%c Header=%d maxerr=%d\n",
		SVP(tdp->Fn), tdp->Sep, tdp->Qot, tdp->Header, tdp->Maxerr);

#if defined(ZIP_SUPPORT)
	if (tdp->Zipped)
		tcvp = new(g)TDBCSV(tdp, new(g)UNZFAM(tdp));
	else
#endif   // ZIP_SUPPORT
		tcvp = new(g) TDBCSV(tdp, new(g) DOSFAM(tdp));

	tcvp->SetMode(MODE_READ);

	if (tdp->Multiple) {
		tdbp = new(g)TDBMUL(tcvp);
		tdbp->SetMode(MODE_READ);
	} else
	  tdbp = tcvp;

	/*********************************************************************/
	/*  Open the CSV file.                                               */
	/*********************************************************************/
	if (tdbp->OpenDB(g))
		return NULL;

  if (hdr) {
    /*******************************************************************/
    /*  Make the column names from the first line.                     */
    /*******************************************************************/
    phase = 0;

    if ((rc = tdbp->ReadDB(g)) == RC_OK) {
			p = PlgDBDup(g, tcvp->To_Line);

      //skip leading blanks
      for (; *p == ' '; p++) ;

      if (q && *p == q) {
        // Header is quoted
        p++;
        phase = 1;
        } // endif q

      colname[0] = p;
    } else if (rc == RC_EF) {
      sprintf(g->Message, MSG(FILE_IS_EMPTY), fn);
      goto err;
		} else
			goto err;

    for (i = 1; *p; p++)
      if (phase == 1 && *p == q) {
        *p = '\0';
        phase = 0;
      } else if (*p == sep && !phase) {
        *p = '\0';

        //skip leading blanks
        for (; *(p+1) == ' '; p++) ;

        if (q && *(p+1) == q) {
          // Header is quoted
          p++;
          phase = 1;
          } // endif q

        colname[i++] = p + 1;
        } // endif sep

    num_read++;
    imax = hmax = i;

    for (i = 0; i < hmax; i++)
      length[0] = MY_MAX(length[0], strlen(colname[i]));

		tcvp->Header = true;			// In case of multiple table
    } // endif hdr

  for (num_read++; num_read <= num_max; num_read++) {
    /*******************************************************************/
    /*  Now start the reading process. Read one line.                  */
    /*******************************************************************/
		if ((rc = tdbp->ReadDB(g)) == RC_OK) {
    } else if (rc == RC_EF) {
      sprintf(g->Message, MSG(EOF_AFTER_LINE), num_read -1);
      break;
    } else {
      sprintf(g->Message, MSG(ERR_READING_REC), num_read, fn);
      goto err;
    } // endif's

    /*******************************************************************/
    /*  Make the test for field lengths.                               */
    /*******************************************************************/
    i = n = phase = blank = digit = dec = 0;

    for (p = tcvp->To_Line; *p; p++)
      if (*p == sep) {
        if (phase != 1) {
          if (i == MAXCOL - 1) {
            sprintf(g->Message, MSG(TOO_MANY_FIELDS), num_read, fn);
            goto err;
            } // endif i

          if (n) {
            len[i] = MY_MAX(len[i], n);
            type = (digit || (dec && n == 1)) ? TYPE_STRING
                 : (dec) ? TYPE_DOUBLE : TYPE_INT;
            typ[i] = MY_MIN(type, typ[i]);
            prc[i] = MY_MAX((typ[i] == TYPE_DOUBLE) ? (dec - 1) : 0, prc[i]);
            } // endif n

          i++;
          n = phase = blank = digit = dec = 0;
        } else          // phase == 1
          n++;

      } else if (*p == ' ') {
        if (phase < 2)
          n++;

        if (blank)
          digit = 1;

      } else if (*p == q) {
        if (phase == 0) {
          if (blank) {
            if (++nerr > mxr) {
              sprintf(g->Message, MSG(MISPLACED_QUOTE), num_read);
              goto err;
            } else
              goto skip;
          }

          n = 0;
          phase = digit = 1;
        } else if (phase == 1) {
          if (*(p+1) == q) {
            // This is currently not implemented for CSV tables
//          if (++nerr > mxr) {
//            sprintf(g->Message, MSG(QUOTE_IN_QUOTE), num_read);
//            goto err;
//          } else
//            goto skip;

            p++;
            n++;
          } else
            phase = 2;

        } else if (++nerr > mxr) {      // phase == 2
          sprintf(g->Message, MSG(MISPLACED_QUOTE), num_read);
          goto err;
        } else
          goto skip;

      } else {
        if (phase == 2) {
          if (++nerr > mxr) {
            sprintf(g->Message, MSG(MISPLACED_QUOTE), num_read);
            goto err;
          } else
            goto skip;
        }

        // isdigit cannot be used here because of debug assert
        if (!strchr("0123456789", *p)) {
          if (!digit && *p == dechar)
            dec = 1;                    // Decimal point found
          else if (blank || !(*p == '-' || *p == '+'))
            digit = 1;

        } else if (dec)
          dec++;                        // More decimals

        n++;
        blank = 1;
      } // endif's *p

    if (phase == 1) {
      if (++nerr > mxr) {
        sprintf(g->Message, MSG(UNBALANCE_QUOTE), num_read);
        goto err;
      } else
        goto skip;
    }

    if (n) {
      len[i] = MY_MAX(len[i], n);
      type = (digit || n == 0 || (dec && n == 1)) ? TYPE_STRING
           : (dec) ? TYPE_DOUBLE : TYPE_INT;
      typ[i] = MY_MIN(type, typ[i]);
      prc[i]  = MY_MAX((typ[i] == TYPE_DOUBLE) ? (dec - 1) : 0, prc[i]);
      } // endif n

    imax = MY_MAX(imax, i+1);
   skip: ;                  // Skip erroneous line
    } // endfor num_read

  if (trace(1)) {
    htrc("imax=%d Lengths:", imax);

    for (i = 0; i < imax; i++)
      htrc(" %d", len[i]);

    htrc("\n");
  } // endif trace

	tdbp->CloseDB(g);

 skipit:
  if (trace(1))
    htrc("CSVColumns: imax=%d hmax=%d len=%d\n",
                      imax, hmax, length[0]);

  /*********************************************************************/
  /*  Allocate the structures used to refer to the result set.         */
  /*********************************************************************/
  qrp = PlgAllocResult(g, ncol, imax, IDS_COLUMNS + 3,
                          buftyp, fldtyp, length, false, false);
  if (info || !qrp)
    return qrp;

  qrp->Nblin = imax;

  /*********************************************************************/
  /*  Now get the results into blocks.                                 */
  /*********************************************************************/
  for (i = 0; i < imax; i++) {
    if (i >= hmax) {
      sprintf(buf, "COL%.3d", i+1);
      p = buf;
    } else
      p = colname[i];

    if (typ[i] == TYPE_UNKNOWN)            // Void column
      typ[i] = TYPE_STRING;

    crp = qrp->Colresp;                    // Column Name
    crp->Kdata->SetValue(p, i);
    crp = crp->Next;                       // Data Type
    crp->Kdata->SetValue(typ[i], i);
    crp = crp->Next;                       // Type Name
    crp->Kdata->SetValue(GetTypeName(typ[i]), i);
    crp = crp->Next;                       // Precision
    crp->Kdata->SetValue(len[i], i);
    crp = crp->Next;                       // Length
    crp->Kdata->SetValue(len[i], i);
    crp = crp->Next;                       // Scale (precision)
    crp->Kdata->SetValue(prc[i], i);
    } // endfor i

  /*********************************************************************/
  /*  Return the result pointer for use by GetData routines.           */
  /*********************************************************************/
  return qrp;

 err:
  tdbp->CloseDB(g);
  return NULL;
  } // end of CSVCColumns

/* --------------------------- Class CSVDEF -------------------------- */

/***********************************************************************/
/*  CSVDEF constructor.                                                */
/***********************************************************************/
CSVDEF::CSVDEF(void)
  {
  Fmtd = Header = false;
//Maxerr = 0;
  Quoted = -1;
  Sep = ',';
  Qot = '\0';
  }  // end of CSVDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool CSVDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  char   buf[8];

  // Double check correctness of offset values
  if (Catfunc == FNC_NO)
    for (PCOLDEF cdp = To_Cols; cdp; cdp = cdp->GetNext())
      if (cdp->GetOffset() < 1 && !cdp->IsSpecial()) {
        strcpy(g->Message, MSG(BAD_OFFSET_VAL));
        return true;
        } // endif Offset

  // Call DOSDEF DefineAM with am=CSV so FMT is not confused with FIX
  if (DOSDEF::DefineAM(g, "CSV", poff))
    return true;

	Recfm = RECFM_CSV;
  GetCharCatInfo("Separator", ",", buf, sizeof(buf));
  Sep = (strlen(buf) == 2 && buf[0] == '\\' && buf[1] == 't') ? '\t' : *buf;
  Quoted = GetIntCatInfo("Quoted", -1);
  GetCharCatInfo("Qchar", "", buf, sizeof(buf));
  Qot = *buf;

  if (Qot && Quoted < 0)
    Quoted = 0;
  else if (!Qot && Quoted >= 0)
    Qot = '"';

  Fmtd = (!Sep || (am && (*am == 'F' || *am == 'f')));
  Header = GetBoolCatInfo("Header", false);
  Maxerr = GetIntCatInfo("Maxerr", 0);
  Accept = GetBoolCatInfo("Accept", false);

  if (Accept && Maxerr == 0)
    Maxerr = INT_MAX32;       // Accept all bad lines

  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB CSVDEF::GetTable(PGLOBAL g, MODE mode)
  {
  PTDBASE tdbp;

  if (Catfunc != FNC_COL) {
    USETEMP tmp = UseTemp();
    bool    map = Mapped && mode != MODE_INSERT &&
                  !(tmp != TMP_NO && mode == MODE_UPDATE) &&
                  !(tmp == TMP_FORCE &&
                  (mode == MODE_UPDATE || mode == MODE_DELETE));
    PTXF    txfp;

    /*******************************************************************/
    /*  Allocate a file processing class of the proper type.           */
    /*******************************************************************/
		if (Zipped) {
#if defined(ZIP_SUPPORT)
			if (mode == MODE_READ || mode == MODE_ANY || mode == MODE_ALTER) {
				txfp = new(g) UNZFAM(this);
			} else if (mode == MODE_INSERT) {
				txfp = new(g) ZIPFAM(this);
			} else {
				strcpy(g->Message, "UPDATE/DELETE not supported for ZIP");
				return NULL;
			}	// endif's mode
#else   // !ZIP_SUPPORT
			strcpy(g->Message, "ZIP not supported");
			return NULL;
#endif  // !ZIP_SUPPORT
		} else if (map) {
      // Should be now compatible with UNIX
      txfp = new(g) MAPFAM(this);
    } else if (Compressed) {
#if defined(GZ_SUPPORT)
      if (Compressed == 1)
        txfp = new(g) GZFAM(this);
      else
        txfp = new(g) ZLBFAM(this);

#else   // !GZ_SUPPORT
        strcpy(g->Message, "Compress not supported");
        return NULL;
#endif  // !GZ_SUPPORT
    } else
      txfp = new(g) DOSFAM(this);

    /*******************************************************************/
    /*  Allocate a TDB of the proper type.                             */
    /*  Column blocks will be allocated only when needed.              */
    /*******************************************************************/
    if (!Fmtd)
      tdbp = new(g) TDBCSV(this, txfp);
    else
      tdbp = new(g) TDBFMT(this, txfp);

    if (Multiple)
      tdbp = new(g) TDBMUL(tdbp);
    else
      /*****************************************************************/
      /*  For block tables, get eventually saved optimization values.  */
      /*****************************************************************/
      if (tdbp->GetBlockValues(g)) {
        PushWarning(g, tdbp);
//      return NULL;          // causes a crash when deleting index
      } else {
        if (IsOptimized()) {
          if (map) {
            txfp = new(g) MBKFAM(this);
          } else if (Compressed) {
#if defined(GZ_SUPPORT)
            if (Compressed == 1)
              txfp = new(g) ZBKFAM(this);
            else {
              txfp->SetBlkPos(To_Pos);
              ((PZLBFAM)txfp)->SetOptimized(To_Pos != NULL);
              } // endelse
#else
            sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "GZ");
            return NULL;
#endif
          } else
            txfp = new(g) BLKFAM(this);

          ((PTDBDOS)tdbp)->SetTxfp(txfp);
          } // endif Optimized

      } // endelse

  } else
    tdbp = new(g)TDBCCL(this);

  return tdbp;
  } // end of GetTable

/* -------------------------- Class TDBCSV --------------------------- */

/***********************************************************************/
/*  Implementation of the TDBCSV class.                                */
/***********************************************************************/
TDBCSV::TDBCSV(PCSVDEF tdp, PTXF txfp) : TDBDOS(tdp, txfp)
  {
#if defined(_DEBUG)
  assert (tdp);
#endif
  Field  = NULL;
  Offset = NULL;
  Fldlen = NULL;
  Fields = 0;
  Nerr = 0;
  Quoted = tdp->Quoted;
  Maxerr = tdp->Maxerr;
  Accept = tdp->Accept;
  Header = tdp->Header;
  Sep = tdp->GetSep();
  Qot = tdp->GetQot();
  } // end of TDBCSV standard constructor

TDBCSV::TDBCSV(PGLOBAL g, PTDBCSV tdbp) : TDBDOS(g, tdbp)
  {
  Fields = tdbp->Fields;

  if (Fields) {
    if (tdbp->Offset)
      Offset = (int*)PlugSubAlloc(g, NULL, sizeof(int) * Fields);

    if (tdbp->Fldlen)
      Fldlen = (int*)PlugSubAlloc(g, NULL, sizeof(int) * Fields);

    Field = (PSZ *)PlugSubAlloc(g, NULL, sizeof(PSZ) * Fields);

    for (int i = 0; i < Fields; i++) {
      if (Offset)
        Offset[i] = tdbp->Offset[i];

      if (Fldlen)
        Fldlen[i] = tdbp->Fldlen[i];

      if (Field) {
        assert (Fldlen);
        Field[i] = (PSZ)PlugSubAlloc(g, NULL, Fldlen[i] + 1);
        Field[i][Fldlen[i]] = '\0';
        } // endif Field

      } // endfor i

  } else {
    Field  = NULL;
    Offset = NULL;
    Fldlen = NULL;
  } // endif Fields

  Nerr = tdbp->Nerr;
  Maxerr = tdbp->Maxerr;
  Quoted = tdbp->Quoted;
  Accept = tdbp->Accept;
  Header = tdbp->Header;
  Sep = tdbp->Sep;
  Qot = tdbp->Qot;
  } // end of TDBCSV copy constructor

// Method
PTDB TDBCSV::Clone(PTABS t)
  {
  PTDB    tp;
  PCSVCOL cp1, cp2;
  PGLOBAL g = t->G;        // Is this really useful ???

  tp = new(g) TDBCSV(g, this);

  for (cp1 = (PCSVCOL)Columns; cp1; cp1 = (PCSVCOL)cp1->GetNext()) {
    cp2 = new(g) CSVCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

/***********************************************************************/
/*  Allocate CSV column description block.                             */
/***********************************************************************/
PCOL TDBCSV::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) CSVCOL(g, cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  Check whether the number of errors is greater than the maximum.    */
/***********************************************************************/
bool TDBCSV::CheckErr(void)
  {
  return (++Nerr) > Maxerr;
  } // end of CheckErr

/***********************************************************************/
/*  CSV EstimatedLength. Returns an estimated minimum line length.     */
/***********************************************************************/
int TDBCSV::EstimatedLength(void)
  {
  int     n = 0;
  PCOLDEF cdp;

  if (trace(1))
    htrc("EstimatedLength: Fields=%d Columns=%p\n", Fields, Columns);

  for (cdp = To_Def->GetCols(); cdp; cdp = cdp->GetNext())
    if (!cdp->IsSpecial() && !cdp->IsVirtual())  // A true column
      n++;

  return --n;   // Number of separators if all fields are null
  } // end of Estimated Length

#if 0
/***********************************************************************/
/*  CSV tables needs the use temporary files for Update.               */
/***********************************************************************/
bool TDBCSV::IsUsingTemp(PGLOBAL g)
  {
  return (Use_Temp == TMP_YES || Use_Temp == TMP_FORCE ||
         (Use_Temp == TMP_AUTO && Mode == MODE_UPDATE));
  } // end of IsUsingTemp
#endif // 0  (Same as TDBDOS one)

/***********************************************************************/
/*  CSV Access Method opening routine.                                 */
/*  First allocate the Offset and Fldlen arrays according to the       */
/*  greatest field used in that query. Then call the DOS opening fnc.  */
/***********************************************************************/
bool TDBCSV::OpenDB(PGLOBAL g)
  {
  bool    rc = false;
  PCOLDEF cdp;
  PDOSDEF tdp = (PDOSDEF)To_Def;

  if (Use != USE_OPEN && (Columns || Mode == MODE_UPDATE)) {
    // Allocate the storage used to read (or write) records
    int     i, len;
    PCSVCOL colp;

    if (!Fields) {            // May have been set in TABFMT::OpenDB
      if (Mode != MODE_UPDATE && Mode != MODE_INSERT) {
        for (colp = (PCSVCOL)Columns; colp; colp = (PCSVCOL)colp->Next)
          if (!colp->IsSpecial() && !colp->IsVirtual())
            Fields = MY_MAX(Fields, (int)colp->Fldnum);

        if (Columns)
          Fields++;           // Fldnum was 0 based

      } else
        for (cdp = tdp->GetCols(); cdp; cdp = cdp->GetNext())
          if (!cdp->IsSpecial() && !cdp->IsVirtual())
            Fields++;
    }

    Offset = (int*)PlugSubAlloc(g, NULL, sizeof(int) * Fields);
    Fldlen = (int*)PlugSubAlloc(g, NULL, sizeof(int) * Fields);

    if (Mode == MODE_INSERT || Mode == MODE_UPDATE) {
      Field = (PSZ*)PlugSubAlloc(g, NULL, sizeof(PSZ) * Fields);
      Fldtyp = (bool*)PlugSubAlloc(g, NULL, sizeof(bool) * Fields);
      } // endif Mode

    for (i = 0; i < Fields; i++) {
      Offset[i] = 0;
      Fldlen[i] = 0;

      if (Field) {
        Field[i] = NULL;
        Fldtyp[i] = false;
        } // endif Field

      } // endfor i

    if (Field) {
      // Prepare writing fields
      if (Mode != MODE_UPDATE) {
        for (colp = (PCSVCOL)Columns; colp; colp = (PCSVCOL)colp->Next)
          if (!colp->IsSpecial() && !colp->IsVirtual()) {
            i = colp->Fldnum;
            len = colp->GetLength();
            Field[i] = (PSZ)PlugSubAlloc(g, NULL, len + 1);
            Field[i][len] = '\0';
            Fldlen[i] = len;
            Fldtyp[i] = IsTypeNum(colp->GetResultType());
            } // endif colp

      } else     // MODE_UPDATE
        for (cdp = tdp->GetCols(); cdp; cdp = cdp->GetNext())
          if (!cdp->IsSpecial() && !cdp->IsVirtual()) {
            i = cdp->GetOffset() - 1;
            len = cdp->GetLength();
            Field[i] = (PSZ)PlugSubAlloc(g, NULL, len + 1);
            Field[i][len] = '\0';
            Fldlen[i] = len;
            Fldtyp[i] = IsTypeNum(cdp->GetType());
            } // endif cdp
    }
  } // endif Use

  if (Header) {
    // Check that the Lrecl is at least equal to the header line length
    int     headlen = 0;
    PCOLDEF cdp;
    PDOSDEF tdp = (PDOSDEF)To_Def;

    for (cdp = tdp->GetCols(); cdp; cdp = cdp->GetNext())
      headlen += strlen(cdp->GetName()) + 3;  // 3 if names are quoted

    if (headlen > Lrecl) {
      Lrecl = headlen;
      Txfp->Lrecl = headlen;
      } // endif headlen

    } // endif Header

  Nerr = 0;
  rc = TDBDOS::OpenDB(g);

  if (!rc && Mode == MODE_UPDATE && To_Kindex)
    // Because KINDEX::Init is executed in mode READ, we must restore
    // the Fldlen array that was modified when reading the table file.
    for (cdp = tdp->GetCols(); cdp; cdp = cdp->GetNext())
      Fldlen[cdp->GetOffset() - 1] = cdp->GetLength();

  return rc;
  } // end of OpenDB

/***********************************************************************/
/*  SkipHeader: Physically skip first header line if applicable.       */
/*  This is called from TDBDOS::OpenDB and must be executed before     */
/*  Kindex construction if the file is accessed using an index.        */
/***********************************************************************/
bool TDBCSV::SkipHeader(PGLOBAL g)
  {
  int len = GetFileLength(g);
  bool rc = false;

#if defined(_DEBUG)
  if (len < 0)
    return true;
#endif   // _DEBUG

  if (Header) {
    if (Mode == MODE_INSERT) {
      if (!len) {
        // New file, the header line must be constructed and written
        int     i, n = 0;
        int    hlen = 0;
        bool    q = Qot && Quoted > 0;
        PCOLDEF cdp;

        // Estimate the length of the header list
        for (cdp = To_Def->GetCols(); cdp; cdp = cdp->GetNext()) {
          hlen += (1 + strlen(cdp->GetName()));
          hlen += ((q) ? 2 : 0);
          n++;            // Calculate the number of columns
          } // endfor cdp

        if (hlen > Lrecl) {
          sprintf(g->Message, MSG(LRECL_TOO_SMALL), hlen);
          return true;
          } // endif hlen

        // File is empty, write a header record
        memset(To_Line, 0, Lrecl);

        // The column order in the file is given by the offset value
        for (i = 1; i <= n; i++)
          for (cdp = To_Def->GetCols(); cdp; cdp = cdp->GetNext())
            if (cdp->GetOffset() == i) {
              if (q)
                To_Line[strlen(To_Line)] = Qot;

              strcat(To_Line, cdp->GetName());

              if (q)
                To_Line[strlen(To_Line)] = Qot;

              if (i < n)
                To_Line[strlen(To_Line)] = Sep;

              } // endif Offset

        rc = (Txfp->WriteBuffer(g) == RC_FX);
        } // endif !FileLength

    } else if (Mode == MODE_DELETE) {
      if (len)
        rc = (Txfp->SkipRecord(g, true) == RC_FX);

    } else if (len) // !Insert && !Delete
      rc = (Txfp->SkipRecord(g, false) == RC_FX || Txfp->RecordPos(g));

    } // endif Header

  return rc;
  } // end of SkipHeader

/***********************************************************************/
/*  ReadBuffer: Physical read routine for the CSV access method.       */
/***********************************************************************/
int TDBCSV::ReadBuffer(PGLOBAL g)
  {
  //char *p1, *p2, *p = NULL;
	char *p2, *p = NULL;
	int   i, n, len, rc = Txfp->ReadBuffer(g);
  bool  bad = false;

  if (trace(2))
    htrc("CSV: Row is '%s' rc=%d\n", To_Line, rc);

  if (rc != RC_OK || !Fields)
    return rc;
  else
    p2 = To_Line;

  // Find the offsets and lengths of the columns for this row
  for (i = 0; i < Fields; i++) {
    if (!bad) {
      if (Qot && *p2 == Qot) {                // Quoted field
        //for (n = 0, p1 = ++p2; (p = strchr(p1, Qot)); p1 = p + 2)
        //  if (*(p + 1) == Qot)
        //    n++;                              // Doubled internal quotes
        //  else
        //    break;                            // Final quote

				for (n = 0, p = ++p2; p; p++)
					if (*p == Qot || *p == '\\') {
						if (*(++p) == Qot)
							n++;														// Escaped internal quotes
						else if (*(p - 1) == Qot)
							break;													// Final quote
					}	// endif *p

        if (p) {
          //len = p++ - p2;
					len = (int)(p - p2 - 1);

//        if (Sep != ' ')
//          for (; *p == ' '; p++) ;          // Skip blanks

          if (*p != Sep && i != Fields - 1) { // Should be the separator
            if (CheckErr()) {
              sprintf(g->Message, MSG(MISSING_FIELD),
                                  i+1, Name, RowNumber(g));
              return RC_FX;
            } else if (Accept)
              bad = true;
            else
              return RC_NF;

            } // endif p

          if (n) {
            int j, k;

            // Suppress the escape of internal quotes
            for (j = k = 0; j < len; j++, k++) {
              if (p2[j] == Qot || (p2[j] == '\\' && p2[j + 1] == Qot))
                j++;                          // skip escape char
							else if (p2[j] == '\\')
								p2[k++] = p2[j++];						// avoid \\Qot

              p2[k] = p2[j];
              } // endfor i, j

            len -= n;
            } // endif n

        } else if (CheckErr()) {
          sprintf(g->Message, MSG(BAD_QUOTE_FIELD),
                              Name, i+1, RowNumber(g));
          return RC_FX;
        } else if (Accept) {
          len = strlen(p2);
          bad = true;
        } else
          return RC_NF;

      } else if ((p = strchr(p2, Sep)))
        len = (int)(p - p2);
      else if (i == Fields - 1)
        len = strlen(p2);
      else if (Accept && Maxerr == 0) {
        len = strlen(p2);
        bad = true;
      } else if (CheckErr()) {
        sprintf(g->Message, MSG(MISSING_FIELD), i+1, Name, RowNumber(g));
        return RC_FX;
      } else if (Accept) {
        len = strlen(p2);
        bad = true;
      } else
        return RC_NF;

    } else
      len = 0;

    Offset[i] = (int)(p2 - To_Line);

    if (Mode != MODE_UPDATE)
      Fldlen[i] = len;
    else if (len > Fldlen[i]) {
      sprintf(g->Message, MSG(FIELD_TOO_LONG), i+1, RowNumber(g));
      return RC_FX;
    } else {
      strncpy(Field[i], p2, len);
      Field[i][len] = '\0';
    } // endif Mode

    if (p)
      p2 = p + 1;

    } // endfor i

  return rc;
  } // end of ReadBuffer

/***********************************************************************/
/*  Prepare the line to write.                                         */
/***********************************************************************/
bool TDBCSV::PrepareWriting(PGLOBAL g)
  {
  char sep[2], qot[2];
  int  i, nlen, oldlen = strlen(To_Line);

  if (trace(2))
    htrc("CSV WriteDB: R%d Mode=%d key=%p link=%p\n",
          Tdb_No, Mode, To_Key_Col, To_Link);

  // Before writing the line we must check its length
  if ((nlen = CheckWrite(g)) < 0)
    return true;

  // Before writing the line we must make it
  sep[0] = Sep;
  sep[1] = '\0';
  qot[0] = Qot;
  qot[1] = '\0';
  *To_Line = '\0';

  for (i = 0; i < Fields; i++) {
    if (i)
      strcat(To_Line, sep);

    if (Field[i]) {
      if (!strlen(Field[i])) {
        // Generally null fields are not quoted
        if (Quoted > 2)
          // Except if explicitely required
          strcat(strcat(To_Line, qot), qot);

      } else if (Qot && (strchr(Field[i], Sep) || *Field[i] == Qot
              || Quoted > 1 || (Quoted == 1 && !Fldtyp[i]))) {
        if (strchr(Field[i], Qot)) {
          // Field contains quotes that must be doubled
          int j, k = strlen(To_Line), n = strlen(Field[i]);

          To_Line[k++] = Qot;

          for (j = 0; j < n; j++) {
            if (Field[i][j] == Qot)
              To_Line[k++] = Qot;

            To_Line[k++] = Field[i][j];
            } // endfor j

          To_Line[k++] = Qot;
          To_Line[k] = '\0';
        } else
          strcat(strcat(strcat(To_Line, qot), Field[i]), qot);
      }

      else
        strcat(To_Line, Field[i]);
    }
  } // endfor i

#if defined(_DEBUG)
  assert ((unsigned)nlen == strlen(To_Line));
#endif

  if (Mode == MODE_UPDATE && nlen < oldlen
                          && !((PDOSFAM)Txfp)->GetUseTemp()) {
    // In Update mode with no temp file, line length must not change
    To_Line[nlen] = Sep;

    for (nlen++; nlen < oldlen; nlen++)
      To_Line[nlen] = ' ';

    To_Line[nlen] = '\0';
    } // endif

  if (trace(2))
    htrc("Write: line is=%s", To_Line);

  return false;
  } // end of PrepareWriting

/***********************************************************************/
/*  Data Base write routine CSV file access method.                    */
/***********************************************************************/
int TDBCSV::WriteDB(PGLOBAL g)
  {
  // Before writing the line we must check and prepare it
  if (PrepareWriting(g))
    return RC_FX;

  /*********************************************************************/
  /*  Now start the writing process.                                   */
  /*********************************************************************/
  return Txfp->WriteBuffer(g);
  } // end of WriteDB

/***********************************************************************/
/*  Check whether a new line fit in the file lrecl size.               */
/***********************************************************************/
int TDBCSV::CheckWrite(PGLOBAL g)
  {
  int maxlen, n, nlen = (Fields - 1);

  if (trace(2))
    htrc("CheckWrite: R%d Mode=%d\n", Tdb_No, Mode);

  // Before writing the line we must check its length
  maxlen = (Mode == MODE_UPDATE && !Txfp->GetUseTemp())
         ? strlen(To_Line) : Lrecl;

  // Check whether record is too int
  for (int i = 0; i < Fields; i++)
  {
    if (Field[i]) {
      if (!(n = strlen(Field[i])))
        n += (Quoted > 2 ? 2 : 0);
      else if (strchr(Field[i], Sep) || (Qot && *Field[i] == Qot)
          || Quoted > 1 || (Quoted == 1 && !Fldtyp[i]))
      {
        if (!Qot) {
          sprintf(g->Message, MSG(SEP_IN_FIELD), i + 1);
          return -1;
        } else {
          // Quotes inside a quoted field must be doubled
          char *p1, *p2;

          for (p1 = Field[i]; (p2 = strchr(p1, Qot)); p1 = p2 + 1)
            n++;

          n += 2;        // Outside quotes
        } // endif
      }
      if ((nlen += n) > maxlen) {
        strcpy(g->Message, MSG(LINE_TOO_LONG));
        return -1;
        } // endif nlen

      } // endif Field
  }
  return nlen;
  } // end of CheckWrite

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBFMT class.                                */
/***********************************************************************/
TDBFMT::TDBFMT(PGLOBAL g, PTDBFMT tdbp) : TDBCSV(g, tdbp)
  {
  FldFormat = tdbp->FldFormat;
  To_Fld = tdbp->To_Fld;
  FmtTest = tdbp->FmtTest;
  Linenum = tdbp->Linenum;
  } // end of TDBFMT copy constructor

// Method
PTDB TDBFMT::Clone(PTABS t)
  {
  PTDB    tp;
  PCSVCOL cp1, cp2;
//PFMTCOL cp1, cp2;
  PGLOBAL g = t->G;        // Is this really useful ???

  tp = new(g) TDBFMT(g, this);

  for (cp1 = (PCSVCOL)Columns; cp1; cp1 = (PCSVCOL)cp1->GetNext()) {
//for (cp1 = (PFMTCOL)Columns; cp1; cp1 = (PFMTCOL)cp1->GetNext()) {
    cp2 = new(g) CSVCOL(cp1, tp);  // Make a copy
//  cp2 = new(g) FMTCOL(cp1, tp);  // Make a copy
    NewPointer(t, cp1, cp2);
    } // endfor cp1

  return tp;
  } // end of Clone

/***********************************************************************/
/*  Allocate FMT column description block.                             */
/***********************************************************************/
PCOL TDBFMT::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) CSVCOL(g, cdp, this, cprec, n);
//return new(g) FMTCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  FMT EstimatedLength. Returns an estimated minimum line length.     */
/*  The big problem here is how can we astimated that minimum ?        */
/***********************************************************************/
int TDBFMT::EstimatedLength(void)
  {
  // This is rather stupid !!!
  return ((PDOSDEF)To_Def)->GetEnding() + (int)((Lrecl / 10) + 1);
  } // end of EstimatedLength

/***********************************************************************/
/*  FMT Access Method opening routine.                                 */
/***********************************************************************/
bool TDBFMT::OpenDB(PGLOBAL g)
  {
  Linenum = 0;

  if (Mode == MODE_INSERT || Mode == MODE_UPDATE) {
    sprintf(g->Message, MSG(FMT_WRITE_NIY), "FMT");
    return true;                    // NIY
    } // endif Mode

  if (Use != USE_OPEN && Columns) {
    // Make the formats used to read records
    PSZ     pfm;
    int     i, n;
    PCSVCOL colp;
    PCOLDEF cdp;
    PDOSDEF tdp = (PDOSDEF)To_Def;

    for (colp = (PCSVCOL)Columns; colp; colp = (PCSVCOL)colp->Next)
      if (!colp->IsSpecial() && !colp->IsVirtual())  // a true column
        Fields = MY_MAX(Fields, (int)colp->Fldnum);

    if (Columns)
      Fields++;                // Fldnum was 0 based

    To_Fld = PlugSubAlloc(g, NULL, Lrecl + 1);
    FldFormat = (PSZ*)PlugSubAlloc(g, NULL, sizeof(PSZ) * Fields);
    memset(FldFormat, 0, sizeof(PSZ) * Fields);
    FmtTest = (int*)PlugSubAlloc(g, NULL, sizeof(int) * Fields);
    memset(FmtTest, 0, sizeof(int) * Fields);

    // Get the column formats
    for (cdp = tdp->GetCols(); cdp; cdp = cdp->GetNext())
      if (!cdp->IsSpecial() && !cdp->IsVirtual() 
                            && (i = cdp->GetOffset() - 1) < Fields) {
        if (!(pfm = cdp->GetFmt())) {
          sprintf(g->Message, MSG(NO_FLD_FORMAT), i + 1, Name);
          return true;
          } // endif pfm

        // Roughly check the Fmt format
        if ((n = strlen(pfm) - 2) < 4) {
          sprintf(g->Message, MSG(BAD_FLD_FORMAT), i + 1, Name);
          return true;
          } // endif n

        FldFormat[i] = (PSZ)PlugSubAlloc(g, NULL, n + 5);
        strcpy(FldFormat[i], pfm);

        if (!strcmp(pfm + n, "%m")) {
          // This is a field that can be missing. Flag it so it can
          // be handled with special processing.
          FldFormat[i][n+1] = 'n';  // To have sscanf normal processing
          FmtTest[i] = 2;
        } else if (i+1 < Fields && strcmp(pfm + n, "%n")) {
          // There are trailing characters after the field contents
          // add a marker for the next field start position.
          strcat(FldFormat[i], "%n");
          FmtTest[i] = 1;
        } // endif's

        } // endif i

    } // endif Use

  return TDBCSV::OpenDB(g);
  } // end of OpenDB

/***********************************************************************/
/*  ReadBuffer: Physical read routine for the FMT access method.       */
/***********************************************************************/
int TDBFMT::ReadBuffer(PGLOBAL g)
  {
  int  i, len, n, deb, fin, nwp, pos = 0, rc;
  bool bad = false;

  if ((rc = Txfp->ReadBuffer(g)) != RC_OK || !Fields)
    return rc;
  else
    ++Linenum;

  if (trace(2))
    htrc("FMT: Row %d is '%s' rc=%d\n", Linenum, To_Line, rc);

  // Find the offsets and lengths of the columns for this row
  for (i = 0; i < Fields; i++) {
    if (!bad) {
      deb = fin = -1;

      if (!FldFormat[i]) {
        n = 0;
      } else if (FmtTest[i] == 1) {
        nwp = -1;
        n = sscanf(To_Line + pos, FldFormat[i], &deb, To_Fld, &fin, &nwp);
      } else {
        n = sscanf(To_Line + pos, FldFormat[i], &deb, To_Fld, &fin);

        if (n != 1 && (deb >= 0 || i == Fields - 1) && FmtTest[i] == 2) {
          // Missing optional field, not an error
          n = 1;

          if (i == Fields - 1)
            fin = deb = 0;
          else
            fin = deb;

          } // endif n

        nwp = fin;
      } // endif i

      if (n != 1 || deb < 0 || fin < 0 || nwp < 0) {
        // This is to avoid a very strange sscanf bug occuring
        // with fields that ends with a null character.
        // This bug causes subsequent sscanf to return in error,
        // so next lines are not parsed correctly.
        sscanf("a", "%*c");       // Seems to reset things Ok

        if (CheckErr()) {
          sprintf(g->Message, MSG(BAD_LINEFLD_FMT), Linenum, i + 1, Name);
          return RC_FX;
        } else if (Accept)
          bad = true;
        else
          return RC_NF;

        } // endif n...

      } // endif !bad

    if (!bad) {
      Offset[i] = pos + deb;
      len = fin - deb;
    } else {
      nwp = 0;
      Offset[i] = pos;
      len = 0;
    } // endif bad

//  if (Mode != MODE_UPDATE)
      Fldlen[i] = len;
//  else if (len > Fldlen[i]) {
//    sprintf(g->Message, MSG(FIELD_TOO_LONG), i+1, To_Tdb->RowNumber(g));
//    return RC_FX;
//  } else {
//    strncpy(Field[i], To_Line + pos, len);
//    Field[i][len] = '\0';
//  } // endif Mode

    pos += nwp;
    } // endfor i

  if (bad)
    Nerr++;
  else
    sscanf("a", "%*c");             // Seems to reset things Ok

  return rc;
  } // end of ReadBuffer

/***********************************************************************/
/*  Data Base write routine FMT file access method.                    */
/***********************************************************************/
int TDBFMT::WriteDB(PGLOBAL g)
  {
  sprintf(g->Message, MSG(FMT_WRITE_NIY), "FMT");
  return RC_FX;                    // NIY
  } // end of WriteDB

// ------------------------ CSVCOL functions ----------------------------

/***********************************************************************/
/*  CSVCOL public constructor                                          */
/***********************************************************************/
CSVCOL::CSVCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
  : DOSCOL(g, cdp, tdbp, cprec, i, "CSV")
  {
  Fldnum = Deplac - 1;
  Deplac = 0;
  } // end of CSVCOL constructor

/***********************************************************************/
/*  CSVCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
CSVCOL::CSVCOL(CSVCOL *col1, PTDB tdbp) : DOSCOL(col1, tdbp)
  {
  Fldnum = col1->Fldnum;
  } // end of CSVCOL copy constructor

/***********************************************************************/
/*  VarSize: This function tells UpdateDB whether or not the block     */
/*  optimization file must be redone if this column is updated, even   */
/*  it is not sorted or clustered. This applies to a blocked table,    */
/*  because if it is updated using a temporary file, the block size    */
/*  may be modified.                                                   */
/***********************************************************************/
bool CSVCOL::VarSize(void)
  {
  PTXF txfp = ((PTDBCSV)To_Tdb)->Txfp;

  if (txfp->IsBlocked() && txfp->GetUseTemp())
    // Blocked table using a temporary file
    return true;
  else
    return false;

  } // end VarSize

/***********************************************************************/
/*  ReadColumn: call DOSCOL::ReadColumn after having set the offet     */
/*  and length of the field to read as calculated by TDBCSV::ReadDB.   */
/***********************************************************************/
void CSVCOL::ReadColumn(PGLOBAL g)
  {
  int     rc;
  PTDBCSV tdbp = (PTDBCSV)To_Tdb;

  /*********************************************************************/
  /*  If physical reading of the line was deferred, do it now.         */
  /*********************************************************************/
  if (!tdbp->IsRead())
    if ((rc = tdbp->ReadBuffer(g)) != RC_OK) {
      if (rc == RC_EF)
        sprintf(g->Message, MSG(INV_DEF_READ), rc);

			throw 34;
		} // endif

  if (tdbp->Mode != MODE_UPDATE) {
    int colen = Long;                    // Column length

    // Set the field offset and length for this row
    Deplac = tdbp->Offset[Fldnum];       // Field offset
    Long   = tdbp->Fldlen[Fldnum];       // Field length

    if (trace(2))
      htrc("CSV ReadColumn %s Fldnum=%d offset=%d fldlen=%d\n",
            Name, Fldnum, Deplac, Long);

    if (Long > colen && tdbp->CheckErr()) {
      Long = colen;                       // Restore column length
      sprintf(g->Message, MSG(FLD_TOO_LNG_FOR),
              Fldnum + 1, Name, To_Tdb->RowNumber(g), tdbp->GetFile(g));
			throw 34;
		} // endif Long

    // Now do the reading
    DOSCOL::ReadColumn(g);

    // Restore column length
    Long = colen;
  } else {         // Mode Update
    // Field have been copied in TDB Field array
    PSZ fp = tdbp->Field[Fldnum];

    if (Dsp)
      for (int i = 0; fp[i]; i++)
        if (fp[i] == Dsp)
          fp[i] = '.';

    Value->SetValue_psz(fp);

    // Set null when applicable
    if (Nullable)
      Value->SetNull(Value->IsZero());

  } // endif Mode

  } // end of ReadColumn

/***********************************************************************/
/*  WriteColumn: The column is written in TDBCSV matching Field.       */
/***********************************************************************/
void CSVCOL::WriteColumn(PGLOBAL g)
  {
  char   *p;
  int     n, flen;
  PTDBCSV tdbp = (PTDBCSV)To_Tdb;

  if (trace(2))
    htrc("CSV WriteColumn: col %s R%d coluse=%.4X status=%.4X\n",
          Name, tdbp->GetTdb_No(), ColUse, Status);

  flen = GetLength();

  if (trace(2))
    htrc("Lrecl=%d Long=%d field=%d coltype=%d colval=%p\n",
          tdbp->Lrecl, Long, flen, Buf_Type, Value);

  /*********************************************************************/
  /*  Check whether the new value has to be converted to Buf_Type.     */
  /*********************************************************************/
  if (Value != To_Val)
    Value->SetValue_pval(To_Val, false);    // Convert the updated value

  /*********************************************************************/
  /*  Get the string representation of the column value.               */
  /*********************************************************************/
  p = Value->GetCharString(Buf);
	n = strlen(p);

  if (trace(2))
    htrc("new length(%p)=%d\n", p, n);

  if (n > flen) {
    sprintf(g->Message, MSG(BAD_FLD_LENGTH), Name, p, n,
                        tdbp->RowNumber(g), tdbp->GetFile(g));
		throw 34;
	} else if (Dsp)
    for (int i = 0; p[i]; i++)
      if (p[i] == '.')
        p[i] = Dsp; 

  if (trace(2))
    htrc("buffer=%s\n", p);

  /*********************************************************************/
  /*  Updating must be done also during the first pass so writing the  */
  /*  updated record can be checked for acceptable record length.      */
  /*********************************************************************/
  if (Fldnum < 0) {
    // This can happen for wrong offset value in XDB files
    sprintf(g->Message, MSG(BAD_FIELD_RANK), Fldnum + 1, Name);
		throw 34;
	} else
    strncpy(tdbp->Field[Fldnum], p, flen);

  if (trace(2))
    htrc(" col written: '%s'\n", p);

  } // end of WriteColumn

/* ---------------------------TDBCCL class --------------------------- */

/***********************************************************************/
/*  TDBCCL class constructor.                                          */
/***********************************************************************/
TDBCCL::TDBCCL(PCSVDEF tdp) : TDBCAT(tdp)
{
	Topt = tdp->GetTopt();
} // end of TDBCCL constructor

/***********************************************************************/
/*  GetResult: Get the list the CSV file columns.                      */
/***********************************************************************/
PQRYRES TDBCCL::GetResult(PGLOBAL g)
  {
  return CSVColumns(g, ((PTABDEF)To_Def)->GetPath(), Topt, false);
  } // end of GetResult

/* ------------------------ End of TabFmt ---------------------------- */
