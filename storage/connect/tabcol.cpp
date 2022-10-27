/************* TabCol C++ Functions Source Code File (.CPP) ************/
/*  Name: TABCOL.CPP  Version 2.7                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2015    */
/*                                                                     */
/*  This file contains the PlugDB++ XTAB, COLUMN and XORDER methods.   */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h   is header containing all global Plug declarations.      */
/*  plgdbsem.h is header containing the DB applic. declarations.       */
/*  tabcol.h   is header containing XTAB, and XORDER declares.         */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "tabcol.h"

/***********************************************************************/
/*  XTAB public constructor.                                           */
/***********************************************************************/
XTAB::XTAB(LPCSTR name, LPCSTR srcdef) : Name(name)
  {
  Next = NULL;
  To_Tdb = NULL;
  Srcdef = srcdef;
  Schema = NULL;
  Qualifier = NULL;

  if (trace(1))
    htrc("XTAB: making new TABLE %s %s\n", Name, Srcdef);

  } // end of XTAB constructor

/***********************************************************************/
/*  XTAB public constructor as a copy of another table.                */
/***********************************************************************/
XTAB::XTAB(PTABLE tp) : Name(tp->Name)
  {
  Next = NULL;
  To_Tdb = NULL;
  Srcdef = tp->Srcdef;
  Schema = tp->Schema;
  Qualifier = tp->Qualifier;

  if (trace(1))
    htrc(" making copy TABLE %s %s\n", Name, SVP(Srcdef));

  } // end of XTAB constructor

/***********************************************************************/
/*  Link the tab2 tables to the tab1(this) table chain.                */
/***********************************************************************/
PTABLE XTAB::Link(PTABLE tab2)
  {
  PTABLE tabp;

  if (trace(1))
    htrc("Linking tables %s... to %s\n", Name, tab2->Name);

  for (tabp = this; tabp->Next; tabp = tabp->Next) ;

  tabp->Next = tab2;
  return (this);
  } /* end of Link */

/***********************************************************************/
/*  Make file output of XTAB contents.                                 */
/***********************************************************************/
void XTAB::Printf(PGLOBAL g, FILE *f, uint n)
  {
  char  m[64];

  memset(m, ' ', n);                             /* Make margin string */
  m[n] = '\0';

  for (PTABLE tp = this; tp; tp = tp->Next) {
    fprintf(f, "%sTABLE: %s.%s %s\n",
            m, SVP(tp->Schema), tp->Name, SVP(tp->Srcdef));
    PlugPutOut(g, f, TYPE_TDB, tp->To_Tdb, n + 2);
    } /* endfor tp */

  } /* end of Printf */

/***********************************************************************/
/*  Make string output of XTAB contents.                               */
/***********************************************************************/
void XTAB::Prints(PGLOBAL, char *ps, uint z)
  {
  char buf[128];
  int  i, n = (int)z - 1;

  *ps = '\0';

  for (PTABLE tp = this; tp && n > 0; tp = tp->Next) {
    i = sprintf(buf, "TABLE: %s.%s %s To_Tdb=%p ",
                SVP(tp->Schema), tp->Name, SVP(tp->Srcdef), tp->To_Tdb);
    strncat(ps, buf, n);
    n -= i;
    } // endif tp

  } /* end of Prints */


/***********************************************************************/
/*  COLUMN public constructor.                                         */
/***********************************************************************/
COLUMN::COLUMN(LPCSTR name) : Name(name)
  {
  To_Table = NULL;
  To_Col = NULL;
  Qualifier = NULL;

  if (trace(1))
    htrc(" making new COLUMN %s\n", Name);

  } // end of COLUMN constructor

/***********************************************************************/
/*  COLUMN SetFormat: should never be called.                          */
/***********************************************************************/
bool COLUMN::SetFormat(PGLOBAL g, FORMAT&)
  {
  strcpy(g->Message, MSG(NO_FORMAT_COL));
  return true;
  } // end of SetFormat

/***********************************************************************/
/*  Make file output of COLUMN contents.                               */
/***********************************************************************/
void COLUMN::Printf(PGLOBAL g, FILE *f, uint n)
  {
  char  m[64];

  memset(m, ' ', n);      // Make margin string
  m[n] = '\0';

  if (Name)
    fprintf(f, "%sCOLUMN: %s.%s\n", m,
      ((!Qualifier) ? (PSZ)"?" : Qualifier), Name);
  else         // LNA
    fprintf(f, "%sC%d\n", m, (!Qualifier) ? 0 : *(int *)Qualifier);

  PlugPutOut(g, f, TYPE_TABLE, To_Table, n + 2);
  PlugPutOut(g, f, TYPE_XOBJECT, To_Col, n + 2);
  } /* end of Printf */

/***********************************************************************/
/*  Make string output of COLUMN contents.                             */
/***********************************************************************/
void COLUMN::Prints(PGLOBAL, char *ps, uint z)
  {
  char buf[80];

  if (Name)
    sprintf(buf, "COLUMN: %s.%s table=%p col=%p",
      ((!Qualifier) ? (PSZ)"?" : Qualifier), Name, To_Table, To_Col);
  else         // LNA
    sprintf(buf, "C%d", (!Qualifier) ? 0 : *(int *)Qualifier);

  strncpy(ps, buf, z);
  ps[z - 1] = '\0';
  } /* end of Prints */
