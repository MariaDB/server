/************* tdbvir C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: tdbvir.cpp  Version 1.2                               */
/*  (C) Copyright to the author Olivier BERTRAND          2014-2017    */
/*  This program are the VIR classes DB execution routines.            */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  xtable.h    is header containing the TDBASE declarations.          */
/*  tdbvir.h    is header containing the VIR classes declarations.     */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "filter.h"
#include "xtable.h"
//#include "reldef.h"
#include "colblk.h"
#include "mycat.h"                           // for FNC_COL
#include "tabvir.h"
#include "resource.h"                        // for IDS_COLUMNS

/***********************************************************************/
/*  Return the unique column definition to MariaDB.                    */
/***********************************************************************/
PQRYRES VirColumns(PGLOBAL g, bool info)
  {
  int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING,
                   TYPE_INT, TYPE_STRING, TYPE_STRING};
  XFLD fldtyp[] = {FLD_NAME, FLD_TYPE, FLD_TYPENAME, 
                   FLD_PREC, FLD_KEY, FLD_EXTRA};
  unsigned int length[] = {8, 4, 16, 4, 16, 16};
  int     i, n, ncol = sizeof(buftyp) / sizeof(int);
  PQRYRES qrp;
  PCOLRES crp;

  n = (info) ? 0 : 1;

  /**********************************************************************/
  /*  Allocate the structures used to refer to the result set.          */
  /**********************************************************************/
  if (!(qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
                             buftyp, fldtyp, length, false, true)))
    return NULL;

  // Some columns must be renamed before info
  for (i = 0, crp = qrp->Colresp; crp; crp = crp->Next)
    switch (++i) {
      case 5: crp->Name = "Key";   break;
      case 6: crp->Name = "Extra"; break;
      } // endswitch i

  if (info)
    return qrp;

  /**********************************************************************/
  /*  Now get the results into blocks.                                  */
  /**********************************************************************/
  // Set column name
  crp = qrp->Colresp;                    // Column_Name
  crp->Kdata->SetValue("n", 0);

  // Set type, type name, precision
  crp = crp->Next;                       // Data_Type
  crp->Kdata->SetValue(TYPE_INT, 0);

  crp = crp->Next;                       // Type_Name
  crp->Kdata->SetValue(GetTypeName(TYPE_INT), 0);

  crp = crp->Next;                       // Precision
  crp->Kdata->SetValue(11, 0);

  crp = crp->Next;                       // Key
  crp->Kdata->SetValue("KEY", 0);

  crp = crp->Next;                       // Extra
  crp->Kdata->SetValue("SPECIAL=ROWID", 0);

  qrp->Nblin = 1;

  /**********************************************************************/
  /*  Return the result pointer for use by discovery routines.          */
  /**********************************************************************/
  return qrp;
  } // end of VirColumns

/* --------------------------- Class VIRDEF --------------------------- */

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB VIRDEF::GetTable(PGLOBAL g, MODE)
  {
  //  Column blocks will be allocated only when needed.
  if (Catfunc == FNC_COL)
    return new(g) TDBVICL(this);
  else
    return new(g) TDBVIR(this);

  } // end of GetTable

/* ------------------------ TDBVIR functions ------------------------- */

/***********************************************************************/
/*  Implementation of the TDBVIR class.                                */
/***********************************************************************/
TDBVIR::TDBVIR(PVIRDEF tdp) : TDBASE(tdp)
  {
	Size = (tdp->GetElemt()) ? tdp->GetElemt() : 1;
	N = -1;
  } // end of TDBVIR constructor

/***********************************************************************/
/*  Analyze the filter and reset the size limit accordingly.           */
/*  This is possible when a filter contains predicates implying the    */
/*  special column ROWID. Here we just test for when no more good      */
/*  records can be met in the remaining of the table.                  */
/***********************************************************************/
int TDBVIR::TestFilter(PFIL filp, bool nop)
  {
  int  i, op = filp->GetOpc(), n = 0, type[2] = {0,0};
	int l1 = 0, l2, limit = Size;
  PXOB arg[2] = {NULL,NULL};

  if (op == OP_GT || op == OP_GE || op == OP_LT || op == OP_LE) {
    for (i = 0; i < 2; i++) {
      arg[i] = filp->Arg(i);

      switch (filp->GetArgType(i)) {
        case TYPE_CONST:
          if ((l1 = arg[i]->GetIntValue()) >= 0)
	          type[i] = 1;

          break;
        case TYPE_COLBLK:
          if (((PCOL)arg[i])->GetTo_Tdb() == this &&
              ((PCOL)arg[i])->GetAmType() == TYPE_AM_ROWID)
						type[i] = 2;

					break;
        default:
          break;
        } // endswitch ArgType

      if (!type[i])
        break;

			n += type[i];
      } // endfor i

    if (n == 3) {
			// If true it will be ok to delete the filter
			BOOL ok = (filp == To_Filter);

      if (type[0] == 1)
        // Make it always a Column-op-Value
        switch (op) {
          case OP_GT:	op = OP_LT;	break;
          case OP_GE:	op = OP_LE;	break;
          case OP_LT:	op = OP_GT;	break;
          case OP_LE:	op = OP_GE;	break;
          } // endswitch op

			if (!nop) switch (op) {
		    case OP_LT:	l1--; /* fall through */
				case OP_LE: limit = l1;   break;
				default: ok = false;
				} // endswitch op

			else switch (op) {
		    case OP_GE:	l1--; /* fall through */
				case OP_GT: limit = l1;   break;
				default: ok = false;
				} // endswitch op

			limit = MY_MIN(MY_MAX(0, limit), Size);

			// Just one where clause such as Rowid < limit;
			if (ok)
				To_Filter = NULL;

    } else
			limit = Size;

	} else if ((op == OP_AND && !nop) || (op == OP_OR && nop)) {
    l1 = TestFilter((PFIL)filp->Arg(0), nop);
    l2 = TestFilter((PFIL)filp->Arg(1), nop);
		limit = MY_MIN(l1, l2);
  } else if (op == OP_NOT)
    limit = TestFilter((PFIL)filp->Arg(0), !nop);

  return limit;
  } // end of TestFilter

/***********************************************************************/
/*  Allocate source column description block.                          */
/***********************************************************************/
PCOL TDBVIR::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
	{
  PCOL colp = NULL;

  if (cdp->IsVirtual()) {
    colp = new(g) VIRCOL(cdp, this, cprec, n);
  } else strcpy(g->Message, 
    "Virtual tables accept only special or virtual columns");

	return colp;
	} // end of MakeCol

/***********************************************************************/
/*  VIR Access Method opening routine.                                 */
/***********************************************************************/
bool TDBVIR::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    // Table already open
		N = -1;
    return false;
    } // endif use

  if (Mode != MODE_READ) {
    strcpy(g->Message, "Virtual tables are read only");
    return true;
    } // endif Mode

  /*********************************************************************/
  /*  Analyze the filter and refine Size accordingly.                  */
  /*********************************************************************/
	if (To_Filter)
	  Size = TestFilter(To_Filter, false);

	return false;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for the VIR access method.                  */
/***********************************************************************/
int TDBVIR::ReadDB(PGLOBAL)
  {
  return (++N >= Size) ? RC_EF : RC_OK;
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for the VIR access methods.       */
/***********************************************************************/
int TDBVIR::WriteDB(PGLOBAL g)
  {
  sprintf(g->Message, MSG(VIR_READ_ONLY), To_Def->GetType());
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for the VIR access methods.          */
/***********************************************************************/
int TDBVIR::DeleteDB(PGLOBAL g, int)
  {
  sprintf(g->Message, MSG(VIR_NO_DELETE), To_Def->GetType());
  return RC_FX;
  } // end of DeleteDB

/* ---------------------------- VIRCOL ------------------------------- */

/***********************************************************************/
/*  VIRCOL public constructor.                                         */
/***********************************************************************/
VIRCOL::VIRCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ)
      : COLBLK(cdp, tdbp, i)
  {
  if (cprec) {
    Next = cprec->GetNext();
    cprec->SetNext(this);
  } else {
    Next = tdbp->GetColumns();
    tdbp->SetColumns(this);
  } // endif cprec

  } // end of VIRCOL constructor

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void VIRCOL::ReadColumn(PGLOBAL g)
  {
  // This should never be called
  sprintf(g->Message, "ReadColumn: Column %s is not virtual", Name);
	throw (int)TYPE_COLBLK;
} // end of ReadColumn

/* ---------------------------TDBVICL class -------------------------- */

/***********************************************************************/
/*  GetResult: Get the list the VIRTUAL table columns.                 */
/***********************************************************************/
PQRYRES TDBVICL::GetResult(PGLOBAL g)
  {
  return VirColumns(g, false);
	} // end of GetResult

/* ------------------------- End of Virtual -------------------------- */
