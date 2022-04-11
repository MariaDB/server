/* Copyright (C) Olivier Bertrand 2004 - 2017
   Copyright (C) MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/***********************************************************************/
/*  Author Olivier BERTRAND  bertrandop@gmail.com         2004-2019    */
/*                                                                     */
/*  WHAT THIS PROGRAM DOES:                                            */
/*  -----------------------                                            */
/*  This program are the CONNECT general purpose semantic routines.    */
/***********************************************************************/
#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

/***********************************************************************/
/*  Include application header files                                   */
/*                                                                     */
/*  global.h     is header containing all global declarations.         */
/*  plgdbsem.h   is header containing the DB applic. declarations.     */
/***********************************************************************/
#define DONT_DEFINE_VOID
#include <my_global.h>
#include "handler.h"
#undef  OFFSET

#include "global.h"
#include "plgdbsem.h"
#include "xobject.h"
#include "connect.h"
#include "tabcol.h"
#include "catalog.h"
#include "ha_connect.h"

#define my_strupr(p) my_caseup_str(default_charset_info, (p));
#define my_strlwr(p) my_casedn_str(default_charset_info, (p));
#define my_stricmp(a, b) my_strcasecmp(default_charset_info, (a), (b))

/***********************************************************************/
/*  Routines called internally by semantic routines.                   */
/***********************************************************************/
void  CntEndDB(PGLOBAL);
RCODE EvalColumns(PGLOBAL g, PTDB tdbp, bool reset, bool mrr= false);

/***********************************************************************/
/*  MySQL routines called externally by semantic routines.             */
/***********************************************************************/
int rename_file_ext(const char *from, const char *to,const char *ext);

/***********************************************************************/
/*  CntExit: CONNECT termination routine.                              */
/***********************************************************************/
PGLOBAL CntExit(PGLOBAL g)
  {
  if (g) {
    CntEndDB(g);

		if (g->Activityp) {
			delete g->Activityp;
			g->Activityp = NULL;
		}	// endif Activityp

    g= PlugExit(g);
    } // endif g

  return g;
  } // end of CntExit

/***********************************************************************/
/*  CntEndDB: DB termination semantic routine.                         */
/***********************************************************************/
void CntEndDB(PGLOBAL g)
{
  PDBUSER dbuserp= PlgGetUser(g);

  if (dbuserp) {
    if (dbuserp->Catalog)
      delete dbuserp->Catalog;

    free(dbuserp);

		if (trace(1))
			htrc("CntEndDB: Freeing Dup\n");

		g->Activityp->Aptr = NULL;
	} // endif dbuserp

} // end of CntEndDB

/***********************************************************************/
/*  CntCheckDB: Initialize a DB application session.                   */
/*  Note: because MySQL does not call a storage handler when a user    */
/*  executes a use db command, a check must be done before an SQL      */
/*  command is executed to check whether we are still working on the   */
/*  current database, and if not to load the newly used database.      */
/***********************************************************************/
bool CntCheckDB(PGLOBAL g, PHC handler, const char *pathname)
  {
  bool    rc= false;
  PDBUSER dbuserp= PlgGetUser(g);

  if (trace(1)) {
    printf("CntCheckDB: dbuserp=%p\n", dbuserp);
    } // endif trace

  if (!dbuserp || !handler)
    return true;

  if (trace(1))
    printf("cat=%p oldhandler=%p newhandler=%p\n", dbuserp->Catalog,
    (dbuserp->Catalog) ? ((MYCAT*)dbuserp->Catalog)->GetHandler() : NULL,
           handler);

  // Set the database path for this table
	if (handler->SetDataPath(g, pathname))
		return true;

  if (dbuserp->Catalog) {
    return false;                       // Nothing else to do
    } // endif Catalog

  // Copy new database name in dbuser block
  strncpy(dbuserp->Name, "???", sizeof(dbuserp->Name) - 1);

  dbuserp->Vtdbno= 0;                      // Init of TDB numbers

  /*********************************************************************/
  /*  Now allocate and initialize the Database Catalog.                */
  /*********************************************************************/
  dbuserp->Step= MSG(READY);

  if (!(dbuserp->Catalog= new MYCAT(handler)))
    return true;

  /*********************************************************************/
  /*  All is correct.                                                  */
  /*********************************************************************/
  sprintf(g->Message, MSG(DATABASE_LOADED), "???");

  if (trace(1))
    printf("msg=%s\n", g->Message);

  return rc;
  } // end of CntCheckDB

/***********************************************************************/
/*  CntInfo: Get table info.                                           */
/*  Returns valid: true if this is a table info.                       */
/***********************************************************************/
bool CntInfo(PGLOBAL g, PTDB tp, PXF info)
{
  if (tp) {
		bool    b = (tp->GetFtype() == RECFM_NAF);
		PTDBDOS tdbp = b ? NULL : (PTDBDOS)tp;

		info->data_file_length = (b) ? 0 : (ulonglong)tdbp->GetFileLength(g);

    if (b || info->data_file_length)
      info->records= (unsigned)tp->Cardinality(g);
//    info->records= (unsigned)tp->GetMaxSize(g);
    else
      info->records= 0;

//  info->mean_rec_length= tdbp->GetLrecl();
    info->mean_rec_length= 0;
    info->data_file_name= (b) ? NULL : (char*)tdbp->GetFile(g);
    return true;
  } else {
    info->data_file_length= 0;
    info->records= 0;
    info->mean_rec_length= 0;
    info->data_file_name= NULL;
    return false;
  } // endif tdbp

} // end of CntInfo

/***********************************************************************/
/*  GetTDB: Get the table description block of a CONNECT table.        */
/***********************************************************************/
PTDB CntGetTDB(PGLOBAL g, LPCSTR name, MODE mode, PHC h)
{
	PTDB    tdbp = NULL;
	PTABLE  tabp;
	PDBUSER dup = PlgGetUser(g);
	volatile PCATLG  cat = (dup) ? dup->Catalog : NULL;  // Safe over throw

	if (trace(1))
		printf("CntGetTDB: name=%s mode=%d cat=%p\n", name, mode, cat);

	if (!cat)
		return NULL;

	try {
		// Get table object from the catalog
		tabp = new(g) XTAB(name);

		if (trace(1))
			printf("CntGetTDB: tabp=%p\n", tabp);

		// Perhaps this should be made thread safe
		((MYCAT*)cat)->SetHandler(h);

		if (!(tdbp = cat->GetTable(g, tabp, mode)))
			printf("CntGetTDB: %s\n", g->Message);

	} catch (int n) {
		if (trace(1))
			htrc("Exception %d: %s\n", n, g->Message);
  } catch (const char *msg) {
		strcpy(g->Message, msg);
	}	// end catch

  if (trace(1))
    printf("Returning tdbp=%p mode=%d\n", tdbp, mode);

  return tdbp;
} // end of CntGetTDB

/***********************************************************************/
/*  OPENTAB: Open a Table.                                             */
/***********************************************************************/
bool CntOpenTable(PGLOBAL g, PTDB tdbp, MODE mode, char *c1, char *c2,
                                        bool del, PHC)
  {
  char   *p;
  int     i, n;
  bool    rcop= true;
  PCOL    colp;
//PCOLUMN cp;
  PDBUSER dup= PlgGetUser(g);

  if (trace(1))
    printf("CntOpenTable: tdbp=%p mode=%d\n", tdbp, mode);

  if (!tdbp) {
    strcpy(g->Message, "Null tdbp");
    printf("CntOpenTable: %s\n", g->Message);
    return true;
    } // endif tdbp

	try {
		if (!c1) {
//		if (mode == MODE_INSERT)	 or CHECK TABLE
				// Allocate all column blocks for that table
				tdbp->ColDB(g, NULL, 0);

		} else for (p = c1; *p; p += n) {
			// Allocate only used column blocks
			if (trace(1))
				printf("Allocating column %s\n", p);

			g->Message[0] = 0;    // To check whether ColDB made an error message
			colp = tdbp->ColDB(g, p, 0);

			if (!colp && !(mode == MODE_INSERT && tdbp->IsSpecial(p))) {
				if (g->Message[0] == 0)
					sprintf(g->Message, MSG(COL_ISNOT_TABLE), p, tdbp->GetName());

				throw 1;
			} // endif colp

			n = strlen(p) + 1;
		} // endfor p

		for (i = 0, colp = tdbp->GetColumns(); colp; i++, colp = colp->GetNext()) {
			if (colp->InitValue(g))
				throw 2;

			if (mode == MODE_INSERT)
				// Allow type conversion
				if (colp->SetBuffer(g, colp->GetValue(), true, false))
					throw 3;

			colp->AddColUse(U_P);           // For PLG tables
		} // endfor colp

		/*******************************************************************/
		/* In Update mode, the updated column blocks must be distinct from */
		/* the read column blocks. So make a copy of the TDB and allocate  */
		/* its column blocks in mode write (required by XML tables).       */
		/*******************************************************************/
		if (mode == MODE_UPDATE) {
			PTDB utp;

			if (!(utp = tdbp->Duplicate(g))) {
        /* If table type is of type virtual retrieve global parameter as it was.*/
        if (tdbp->GetAmType() == TYPE_AM_VIR) {
          if (tdbp->OpenDB(g)) {
            printf("%s\n", g->Message);
            throw 7;
          }
        }
				sprintf(g->Message, MSG(INV_UPDT_TABLE), tdbp->GetName());
				throw 4;
			} // endif tp

			if (!c2)
				// Allocate all column blocks for that table
				utp->ColDB(g, NULL, 0);
			else for (p = c2; *p; p += n) {
				// Allocate only used column blocks
				colp = utp->ColDB(g, p, 0);
				n = strlen(p) + 1;
			} // endfor p

			for (i = 0, colp = utp->GetColumns(); colp; i++, colp = colp->GetNext()) {
				if (colp->InitValue(g))
					throw 5;

				if (colp->SetBuffer(g, colp->GetValue(), true, false))
					throw 6;

			} // endfor colp

		// Attach the updated columns list to the main table
			tdbp->SetSetCols(utp->GetColumns());
		} else if (tdbp && mode == MODE_INSERT)
			tdbp->SetSetCols(tdbp->GetColumns());

		// Now do open the physical table
		if (trace(1))
			printf("Opening table %s in mode %d tdbp=%p\n",
				tdbp->GetName(), mode, tdbp);

		//tdbp->SetMode(mode);

		if (del/* && (tdbp->GetFtype() != RECFM_NAF*/) {
			// To avoid erasing the table when doing a partial delete
			// make a fake Next
//    PDOSDEF ddp= new(g) DOSDEF;
//    PTDB tp= new(g) TDBDOS(ddp, NULL);
			tdbp->SetNext((PTDB)1);
			dup->Check &= ~CHK_DELETE;
		} // endif del


		if (trace(1))
			printf("About to open the table: tdbp=%p\n", tdbp);

		if (mode != MODE_ANY && mode != MODE_ALTER) {
			if (tdbp->OpenDB(g)) {
				printf("%s\n", g->Message);
				throw 7;
			} else
				tdbp->SetNext(NULL);

		} // endif mode

		rcop = false;
	} catch (int n) {
		if (trace(1))
			htrc("Exception %d: %s\n", n, g->Message);
	} catch (const char *msg) {
		strcpy(g->Message, msg);
	} // end catch

  return rcop;
  } // end of CntOpenTable

/***********************************************************************/
/*  Rewind a table by reopening it.                                    */
/***********************************************************************/
bool CntRewindTable(PGLOBAL g, PTDB tdbp)
{
  if (!tdbp)
    return true;

  tdbp->OpenDB(g);
  return false;
} // end of CntRewindTable

/***********************************************************************/
/*  Evaluate all columns after a record is read.                       */
/***********************************************************************/
RCODE EvalColumns(PGLOBAL g, PTDB tdbp, bool reset, bool mrr)
{
  RCODE rc= RC_OK;
  PCOL  colp;

	try {
		for (colp = tdbp->GetColumns(); rc == RC_OK && colp;
			colp = colp->GetNext()) {
			xtrc(2, "Going to read column %s of table %s\n",
				colp->GetName(), tdbp->GetName());

			if (reset)
				colp->Reset();

			// Virtual columns are computed by MariaDB
			if (!colp->GetColUse(U_VIRTUAL) && (!mrr || colp->GetKcol()))
				if (colp->Eval(g))
					rc = RC_FX;

		} // endfor colp

	} catch (int n) {
		if (trace(1))
			printf("Error %d reading columns: %s\n", n, g->Message);

		rc = RC_FX;
	} catch (const char *msg) {
		strcpy(g->Message, msg);
		rc = RC_NF;
	} // end catch

  return rc;
} // end of EvalColumns

/***********************************************************************/
/*  ReadNext: Read next record sequentially.                           */
/***********************************************************************/
RCODE CntReadNext(PGLOBAL g, PTDB tdbp)
{
  RCODE rc;

  if (!tdbp)
    return RC_FX;
  else if (tdbp->GetKindex()) {
    // Reading sequencially an indexed table. This happens after the
    // handler function records_in_range was called and MySQL decides
    // to quit using the index (!!!) Drop the index.
//  for (PCOL colp= tdbp->GetColumns(); colp; colp= colp->GetNext())
//    colp->SetKcol(NULL);

    ((PTDBASE)tdbp)->ResetKindex(g, NULL);
    } // endif index

	try {
		// Do it now to avoid double eval when filtering
		for (PCOL colp = tdbp->GetColumns(); colp; colp = colp->GetNext())
			colp->Reset();

		do {
			if ((rc = (RCODE)tdbp->ReadDB(g)) == RC_OK)
				if (!ApplyFilter(g, tdbp->GetFilter()))
					rc = RC_NF;

		} while (rc == RC_NF);

		if (rc == RC_OK)
			rc = EvalColumns(g, tdbp, false);

	} catch (int) {
	  rc = RC_FX;
  } catch (const char *msg) {
	  strcpy(g->Message, msg);
		rc = RC_FX;
  } // end catch

  return rc;
} // end of CntReadNext

/***********************************************************************/
/*  WriteRow: Insert a new row into a table.                           */
/***********************************************************************/
RCODE  CntWriteRow(PGLOBAL g, PTDB tdbp)
{
	RCODE   rc;
	PCOL    colp;
	//PTDBASE tp= (PTDBASE)tdbp;

	if (!tdbp)
		return RC_FX;

	try {
		// Store column values in table write buffer(s)
		for (colp = tdbp->GetSetCols(); colp; colp = colp->GetNext())
			if (!colp->GetColUse(U_VIRTUAL))
				colp->WriteColumn(g);

		if (tdbp->IsIndexed())
			// Index values must be sorted before updating
			rc = (RCODE)((PTDBDOS)tdbp)->GetTxfp()->StoreValues(g, true);
		else
			// Return result code from write operation
			rc = (RCODE)tdbp->WriteDB(g);

	} catch (int n) {
		printf("Exception %d: %s\n", n, g->Message);
		rc = RC_FX;
	} catch (const char *msg) {
		strcpy(g->Message, msg);
		rc = RC_FX;
	} // end catch

	return rc;
} // end of CntWriteRow

/***********************************************************************/
/*  UpdateRow: Update a row into a table.                              */
/***********************************************************************/
RCODE CntUpdateRow(PGLOBAL g, PTDB tdbp)
  {
  if (!tdbp || tdbp->GetMode() != MODE_UPDATE)
    return RC_FX;

  // Return result code from write operation
  return CntWriteRow(g, tdbp);
  } // end of CntUpdateRow

/***********************************************************************/
/*  DeleteRow: Delete a row from a table.                              */
/***********************************************************************/
RCODE  CntDeleteRow(PGLOBAL g, PTDB tdbp, bool all)
  {
  RCODE   rc;
//PTDBASE tp= (PTDBASE)tdbp;

  if (!tdbp || tdbp->GetMode() != MODE_DELETE)
    return RC_FX;
  else if (tdbp->IsReadOnly())
    return RC_NF;

  if (all) {
    if (tdbp->GetDef()->Indexable())
      ((PTDBDOS)tdbp)->Cardinal= 0;

    // Note: if all, this call will be done when closing the table
    rc= (RCODE)tdbp->DeleteDB(g, RC_FX);
//} else if (tdbp->GetKindex() && !((PTDBASE)tdbp)->GetKindex()->IsSorted() &&
//           ((PTDBASE)tdbp)->Txfp->GetAmType() != TYPE_AM_DBF) {
  } else if(tdbp->IsIndexed()) {
    // Index values must be sorted before updating
    rc= (RCODE)((PTDBDOS)tdbp)->GetTxfp()->StoreValues(g, false);
  } else // Return result code from delete operation
    rc= (RCODE)tdbp->DeleteDB(g, RC_OK);

  return rc;
  } // end of CntDeleteRow

/***********************************************************************/
/*  CLOSETAB: Close a table.                                           */
/***********************************************************************/
int CntCloseTable(PGLOBAL g, PTDB tdbp, bool nox, bool abort)
{
	int     rc = RC_OK;
	//TDBASE *tbxp= (PTDBASE)tdbp;

	if (!tdbp)
		return rc;                           // Nothing to do
	else if (tdbp->GetUse() != USE_OPEN) {
		if (tdbp->GetAmType() == TYPE_AM_XML)
			tdbp->CloseDB(g);                  // Opened by GetMaxSize

		return rc;
	} // endif !USE_OPEN

	if (trace(1))
		printf("CntCloseTable: tdbp=%p mode=%d nox=%d abort=%d\n",
			tdbp, tdbp->GetMode(), nox, abort);

	if (tdbp->GetMode() == MODE_DELETE && tdbp->GetUse() == USE_OPEN) {
		if (tdbp->IsIndexed())
			rc = ((PTDBDOS)tdbp)->GetTxfp()->DeleteSortedRows(g);

		if (!rc)
			rc = tdbp->DeleteDB(g, RC_EF);    // Specific A.M. delete routine

	} else if (tdbp->GetMode() == MODE_UPDATE && tdbp->IsIndexed())
		rc = ((PTDBDOS)tdbp)->GetTxfp()->UpdateSortedRows(g);

	switch (rc) {
		case RC_FX:
			abort = true;
			break;
		case RC_INFO:
			PushWarning(g, tdbp);
			break;
	} // endswitch rc

	try {
		//  This will close the table file(s) and also finalize write
		//  operations such as Insert, Update, or Delete.
		tdbp->SetAbort(abort);
		tdbp->CloseDB(g);
		tdbp->SetAbort(false);

		if (trace(2))
			printf("Table %s closed\n", tdbp->GetName());

		if (!nox && tdbp->GetMode() != MODE_READ && tdbp->GetMode() != MODE_ANY) {
			if (trace(2))
				printf("About to reset opt\n");

			if (!tdbp->IsRemote()) {
				// Make all the eventual indexes
				PTDBASE tbxp = (PTDBASE)tdbp;
				tbxp->ResetKindex(g, NULL);
				tbxp->SetKey_Col(NULL);
				rc = tbxp->ResetTableOpt(g, true, tbxp->GetDef()->Indexable() == 1);
			} // endif remote

		} // endif nox

	} catch (int) {
		rc = RC_FX;
	} catch (const char *msg) {
		strcpy(g->Message, msg);
		rc = RC_FX;
	} // end catch

	if (trace(2))
		htrc("Done rc=%d\n", rc);

	return (rc == RC_OK || rc == RC_INFO) ? 0 : rc;
} // end of CntCloseTable

/***********************************************************************/
/*  Load and initialize the use of an index.                           */
/*  This is the condition(s) for doing indexing.                       */
/*  Note: FIX table are not reset here to Nrec= 1.                     */
/***********************************************************************/
int CntIndexInit(PGLOBAL g, PTDB ptdb, int id, bool sorted)
  {
  PIXDEF  xdp;
  PTDBDOS tdbp;
  DOSDEF *dfp;

  if (!ptdb)
    return -1;
  else if (!ptdb->GetDef()->Indexable()) {
    sprintf(g->Message, MSG(TABLE_NO_INDEX), ptdb->GetName());
    return 0;
  } else if (ptdb->GetDef()->Indexable() == 3) {
    return 1;
  } else
    tdbp= (PTDBDOS)ptdb;

  dfp= (DOSDEF*)tdbp->GetDef();

//if (!(k= colp->GetKey()))
//  if (colp->GetOpt() >= 2) {
//    strcpy(g->Message, "Not a valid indexed column");
//    return -1;
//  } else
      // This is a pseudo indexed sorted block optimized column
//    return 0;

  if (tdbp->GetKindex())
  {
    if (((XXBASE*)tdbp->GetKindex())->GetID() == id) {
      tdbp->GetKindex()->Reset();                // Same index
      return (tdbp->GetKindex()->IsMul()) ? 2 : 1;
    } else {
      tdbp->GetKindex()->Close();
      tdbp->SetKindex(NULL);
    } // endif colp
  }

  for (xdp= dfp->GetIndx(); xdp; xdp= xdp->GetNext())
    if (xdp->GetID() == id)
      break;

  if (!xdp) {
    sprintf(g->Message, "Wrong index ID %d", id);
    return 0;
    } // endif xdp

#if 0
  if (xdp->IsDynamic()) {
    // This is a dynamically created index (KINDEX)
    // It should not be created now, if called by index range
    tdbp->SetXdp(xdp);
    return (xdp->IsUnique()) ? 1 : 2;
    } // endif dynamic
#endif // 0

  // Static indexes must be initialized now for records_in_range
  if (tdbp->InitialyzeIndex(g, xdp, sorted))
    return 0;

  return (tdbp->GetKindex()->IsMul()) ? 2 : 1;
  } // end of CntIndexInit

#if defined(WORDS_BIGENDIAN)
/***********************************************************************/
/*  Swap bytes of the key that are written in little endian order.     */
/***********************************************************************/
static void SetSwapValue(PVAL valp, char *kp)
{
  if (valp->IsTypeNum() && valp->GetType() != TYPE_DECIM) {
    uchar buf[8];
    int   i, k= valp->GetClen();

    for (i = 0; k > 0;)
      buf[i++]= kp[--k];



    valp->SetBinValue((void*)buf);
  } else
    valp->SetBinValue((void*)kp);

} // end of SetSwapValue
#endif   // WORDS_BIGENDIAN

/***********************************************************************/
/*  IndexRead: fetch a record having the index value.                  */
/***********************************************************************/
RCODE CntIndexRead(PGLOBAL g, PTDB ptdb, OPVAL op,
                   const key_range *kr, bool mrr)
  {
  int     n, x;
  RCODE   rc;
  XXBASE *xbp;
	PTDBDOS tdbp;

  if (!ptdb)
    return RC_FX;
  else
    x= ptdb->GetDef()->Indexable();

  if (!x) {
    sprintf(g->Message, MSG(TABLE_NO_INDEX), ptdb->GetName());
    return RC_FX;
  } else if (x == 2) {
    // Remote index. Only used in read mode
    if ((ptdb->GetMode() == MODE_READ || ptdb->GetMode() == MODE_READX)
			               && op != OP_SAME && ptdb->ReadKey(g, op, kr))
      return RC_FX;

    goto rnd;
  } else if (x == 3) {
    if (kr)
      ((PTDBASE)ptdb)->SetRecpos(g, *(int*)kr->key);

    if (op == OP_SAME)
      return RC_NF;

    goto rnd;
  } else
    tdbp= (PTDBDOS)ptdb;

  // Set reference values and index operator
  if (!tdbp->GetLink() || !tdbp->GetKindex()) {
//  if (!tdbp->To_Xdp) {
      sprintf(g->Message, "Index not initialized for table %s", tdbp->GetName());
      return RC_FX;
#if 0
      } // endif !To_Xdp
    // Now it's time to make the dynamic index
    if (tdbp->InitialyzeIndex(g, NULL, false)) {
      sprintf(g->Message, "Fail to make dynamic index %s", 
                          tdbp->To_Xdp->GetName());
      return RC_FX;
      } // endif MakeDynamicIndex
#endif // 0
    } // endif !To_Kindex

  xbp= (XXBASE*)tdbp->GetKindex();

  if (kr) {
		char   *kp= (char*)kr->key;
		int     len= kr->length;
		short   lg;
		bool    rcb;
		PVAL    valp;
		PCOL    colp;

    for (n= 0; n < tdbp->GetKnum(); n++) {
      colp= (PCOL)tdbp->Key(n);

      if (colp->GetColUse(U_NULLS))
        kp++;                   // Skip null byte

      valp= tdbp->Link(n)->GetValue();

      if (!valp->IsTypeNum()) {
        if (colp->GetColUse(U_VAR)) {
#if defined(WORDS_BIGENDIAN)
          ((char*)&lg)[0]= ((char*)kp)[1];
          ((char*)&lg)[1]= ((char*)kp)[0];
#else   // !WORDS_BIGENDIAN
          lg= *(short*)kp;
#endif   //!WORDS_BIGENDIAN
          kp+= sizeof(short);
          rcb= valp->SetValue_char(kp, (int)lg);
        } else
          rcb= valp->SetValue_char(kp, valp->GetClen());

        if (rcb) {
          if (tdbp->RowNumber(g))
            sprintf(g->Message, "Out of range value for column %s at row %d",
                    colp->GetName(), tdbp->RowNumber(g));
          else
            sprintf(g->Message, "Out of range value for column %s",
                    colp->GetName());

          PushWarning(g, tdbp);
          } // endif b

      } else
#if defined(WORDS_BIGENDIAN)
        SetSwapValue(valp, kp);
#else   // !WORDS_BIGENDIAN
        valp->SetBinValue((void*)kp);
#endif   //!WORDS_BIGENDIAN

      kp+= valp->GetClen();

      if (len == kp - (char*)kr->key) {
        n++;
        break;
      } else if (len < kp - (char*)kr->key) {
        strcpy(g->Message, "Key buffer is too small");
        return RC_FX;
      } // endif len

      } // endfor n

    xbp->SetNval(n);
    } // endif key

  xbp->SetOp(op);
  xbp->SetNth(0);

 rnd:
  if ((rc= (RCODE)ptdb->ReadDB(g)) == RC_OK)
    rc= EvalColumns(g, ptdb, true, mrr);

  return rc;
  } // end of CntIndexRead

/***********************************************************************/
/*  Return the number of rows matching given values.                   */
/***********************************************************************/
int CntIndexRange(PGLOBAL g, PTDB ptdb, const uchar* *key, uint *len,
                   bool *incl, key_part_map *kmap)
  {
  const uchar *p, *kp;
  int     i, n, x, k[2];
  short   lg;
  bool    b, rcb;
  PVAL    valp;
  PCOL    colp;
  PTDBDOS tdbp;
  XXBASE *xbp;

  if (!ptdb)
    return -1;

  x= ptdb->GetDef()->Indexable();

  if (!x) {
    sprintf(g->Message, MSG(TABLE_NO_INDEX), ptdb->GetName());
    DBUG_PRINT("Range", ("%s", g->Message));
    return -1;
  } else if (x == 2) {
    // Remote index
    return 2;
  } else if (x == 3) {
    // Virtual index
    for (i= 0; i < 2; i++)
      if (key[i])
        k[i] = *(int*)key[i] + (incl[i] ? 0 : 1 - 2 * i);
      else
        k[i] = (i) ? ptdb->Cardinality(g) : 1;

    return k[1] - k[0] + 1;
  } else
    tdbp= (PTDBDOS)ptdb;

  if (!tdbp->GetKindex() || !tdbp->GetLink()) {
    if (!tdbp->GetXdp()) {
      sprintf(g->Message, "Index not initialized for table %s", tdbp->GetName());
      DBUG_PRINT("Range", ("%s", g->Message));
      return -1;
    } else       // Dynamic index
      return tdbp->GetXdp()->GetMaxSame();     // TODO a better estimate

  } else
    xbp= (XXBASE*)tdbp->GetKindex();

  for (b= false, i= 0; i < 2; i++) {
    p= kp= key[i];

    if (kp) {
      for (n= 0; n < tdbp->GetKnum(); n++) {
        if (kmap[i] & (key_part_map)(1 << n)) {
          if (b == true)
            // Cannot do indexing with missing intermediate key
            return -1;

          colp= (PCOL)tdbp->Key(n);

          if (colp->GetColUse(U_NULLS))
            p++;                   // Skip null byte  ???

          valp= tdbp->Link(n)->GetValue();

          if (!valp->IsTypeNum()) {
            if (colp->GetColUse(U_VAR)) {
#if defined(WORDS_BIGENDIAN)
              ((char*)&lg)[0]= ((char*)p)[1];
              ((char*)&lg)[1]= ((char*)p)[0];
#else   // !WORDS_BIGENDIAN
              lg= *(short*)p;
#endif   //!WORDS_BIGENDIAN
              p+= sizeof(short);
              rcb= valp->SetValue_char((char*)p, (int)lg);
            } else
              rcb= valp->SetValue_char((char*)p, valp->GetClen());

          if (rcb) {
            if (tdbp->RowNumber(g))
              sprintf(g->Message,
                      "Out of range value for column %s at row %d",
                      colp->GetName(), tdbp->RowNumber(g));
            else
              sprintf(g->Message, "Out of range value for column %s",
                      colp->GetName());

            PushWarning(g, tdbp);
            } // endif b

          } else
#if defined(WORDS_BIGENDIAN)
            SetSwapValue(valp, (char*)p);
#else   // !WORDS_BIGENDIAN
            valp->SetBinValue((void*)p);
#endif  // !WORDS_BIGENDIAN

          if (trace(1)) {
            char bf[32];
            printf("i=%d n=%d key=%s\n", i, n, valp->GetCharString(bf));
            } // endif trace

          p+= valp->GetClen();

          if (len[i] == (unsigned)(p - kp)) {
            n++;
            break;
          } else if (len[i] < (unsigned)(p - kp)) {
            strcpy(g->Message, "Key buffer is too small");
            return -1;
          } // endif len

        } else
          b= true;

        } // endfor n

      xbp->SetNval(n);

      if (trace(1))
        printf("xbp=%p Nval=%d i=%d incl=%d\n", xbp, n, i, incl[i]);

      k[i]= xbp->Range(g, i + 1, incl[i]);
    } else
      k[i]= (i) ? xbp->GetNum_K() : 0;

    } // endfor i

  if (trace(1))
    printf("k1=%d k0=%d\n", k[1], k[0]);

  return k[1] - k[0];
  } // end of CntIndexRange
