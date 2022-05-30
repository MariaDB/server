/***************** Filter C++ Class Filter Code (.CPP) *****************/
/*  Name: FILTER.CPP  Version 4.0                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2017    */
/*                                                                     */
/*  This file contains the class FILTER function code.                 */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
//#include "sql_class.h"
//#include "sql_time.h"

#if defined(_WIN32)
//#include <windows.h>
#else   // !_WIN32
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif  // !_WIN32


/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/*  xobject.h   is header containing the XOBJECT derived classes dcls. */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "tabcol.h"
#include "xtable.h"
#include "array.h"
#include "filter.h"
#include "xindex.h"

/***********************************************************************/
/*  Utility routines.                                                  */
/***********************************************************************/
void  PlugConvertConstant(PGLOBAL, void* &, short&);
//void *PlugCopyDB(PTABS, void*, INT);
void  NewPointer(PTABS, void*, void*);
void  AddPointer(PTABS, void*);

static PPARM MakeParm(PGLOBAL g, PXOB xp)
  {
  PPARM pp = (PPARM)PlugSubAlloc(g, NULL, sizeof(PARM));
  pp->Type = TYPE_XOBJECT;
  pp->Value = xp;
  pp->Domain = 0;
  pp->Next = NULL;
  return pp;
  } // end of MakeParm

/***********************************************************************/
/*  Routines called internally/externally by FILTER functions.         */
/***********************************************************************/
bool   PlugEvalLike(PGLOBAL, LPCSTR, LPCSTR, bool);
//bool  ReadSubQuery(PGLOBAL, PSUBQ);
//PSUBQ OpenSubQuery(PGLOBAL, PSQL);
//void  PlugCloseDB(PGLOBAL, PSQL);
BYTE   OpBmp(PGLOBAL g, OPVAL opc);
PARRAY MakeValueArray(PGLOBAL g, PPARM pp);

/***********************************************************************/
/*  Returns the bitmap representing the conditions that must not be    */
/*  met when returning from TestValue for a given operator.            */
/*  Bit one is EQ, bit 2 is LT, and bit 3 is GT.                       */
/***********************************************************************/
BYTE OpBmp(PGLOBAL g, OPVAL opc)
  {
  BYTE bt;

  switch (opc) {
    case OP_IN:
    case OP_EQ: bt = 0x06; break;
    case OP_NE: bt = 0x01; break;
    case OP_GT: bt = 0x03; break;
    case OP_GE: bt = 0x02; break;
    case OP_LT: bt = 0x05; break;
    case OP_LE: bt = 0x04; break;
    case OP_EXIST: bt = 0x00; break;
    default:
      sprintf(g->Message, MSG(BAD_FILTER_OP), opc);
			throw (int)TYPE_FILTER;
	} // endswitch opc

  return bt;
  } // end of OpBmp

/***********************************************************************/
/*  Routines called externally by CondFilter.                          */
/***********************************************************************/
PFIL MakeFilter(PGLOBAL g, PFIL fp1, OPVAL vop, PFIL fp2)
  {
  PFIL filp = new(g) FILTER(g, vop);

  filp->Arg(0) = fp1;
  filp->Arg(1) = (fp2) ? fp2 : pXVOID;

  if (filp->Convert(g, false))
    return NULL;

  return filp;
  } // end of MakeFilter

PFIL MakeFilter(PGLOBAL g, PCOL *colp, POPER pop, PPARM pfirst, bool neg)
{
  PPARM parmp, pp[2];
  PFIL  fp1, fp2, filp = NULL;

  if (pop->Val == OP_IN) {
    PARRAY par = MakeValueArray(g, pfirst);

    if (par) {
      pp[0] = MakeParm(g, colp[0]);
      pp[1] = MakeParm(g, par);
      fp1 = new(g) FILTER(g, pop, pp);

      if (fp1->Convert(g, false))
        return NULL;

      filp = (neg) ? MakeFilter(g, fp1, OP_NOT, NULL) : fp1;
      } // endif par

  } else if (pop->Val == OP_XX) {    // BETWEEN
    if (pfirst && pfirst->Next) {
      pp[0] = MakeParm(g, colp[0]);
      pp[1] = pfirst;
      fp1 = new(g) FILTER(g, neg ? OP_LT : OP_GE, pp);

      if (fp1->Convert(g, false))
        return NULL;

      pp[1] = pfirst->Next;
      fp2 = new(g) FILTER(g, neg ? OP_GT : OP_LE, pp);

      if (fp2->Convert(g, false))
        return NULL;

      filp = MakeFilter(g, fp1, neg ? OP_OR : OP_AND, fp2);
      } // endif parmp

  } else {
    parmp = pfirst;

    for (int i = 0; i < 2; i++)
      if (colp[i]) {
        pp[i] = MakeParm(g, colp[i]);
      } else {
        if (!parmp || parmp->Domain != i)
          return NULL;        // Logical error, should never happen

        pp[i] = parmp;
        parmp = parmp->Next;
      } // endif colp

    filp = new(g) FILTER(g, pop, pp);

    if (filp->Convert(g, false))
      return NULL;

  } // endif's Val

  return filp;
} // end of MakeFilter

/* --------------------------- Class FILTER -------------------------- */                      

/***********************************************************************/
/*  FILTER public constructors.                                        */
/***********************************************************************/
FILTER::FILTER(PGLOBAL g, POPER pop, PPARM *tp)
  {
  Constr(g, pop->Val, pop->Mod, tp);
  } // end of FILTER constructor

FILTER::FILTER(PGLOBAL g, OPVAL opc, PPARM *tp)
  {
  Constr(g, opc, 0, tp);
  } // end of FILTER constructor

void FILTER::Constr(PGLOBAL g, OPVAL opc, int opm, PPARM *tp)
  {
  Next = NULL;
  Opc = opc;
  Opm = opm;
  Bt = 0x00;

  for (int i = 0; i < 2; i++) {
    Test[i].B_T = TYPE_VOID;

    if (tp && tp[i]) {
      PlugConvertConstant(g, tp[i]->Value, tp[i]->Type);
#if defined(_DEBUG)
      assert(tp[i]->Type == TYPE_XOBJECT);
#endif
      Arg(i) = (PXOB)tp[i]->Value;
    } else
      Arg(i) = pXVOID;

    Val(i) = NULL;
    Test[i].Conv = FALSE;
    } // endfor i

  } // end of Constr

/***********************************************************************/
/*  FILTER copy constructor.                                           */
/***********************************************************************/
FILTER::FILTER(PFIL fil1)
  {
  Next = NULL;
  Opc = fil1->Opc;
  Opm = fil1->Opm;
  Test[0] = fil1->Test[0];
  Test[1] = fil1->Test[1];
  } // end of FILTER copy constructor

#if 0
/***********************************************************************/
/*  Linearize:  Does the linearization of the filter tree:             */
/*    Independent filters (not implied in OR/NOT) will be separated    */
/*    from others and filtering operations will be automated by        */
/*    making a list of filter operations in polish operation style.    */
/*  Returned value points to the first filter of the list, which ends  */
/*  with the filter that was pointed by the first call argument,       */
/*  except for separators, in which case a loop is needed to find it.  */
/*  Note: a loop is used now in all cases (was not for OP_NOT) to be   */
/*    able to handle the case of filters whose arguments are already   */
/*    linearized, as it is done in LNA semantic routines. Indeed for   */
/*    already linearized chains, the first filter is never an OP_AND,  */
/*    OP_OR or OP_NOT filter, so this function just returns 'this'.    */
/***********************************************************************/
PFIL FILTER::Linearize(bool nosep)
  {
  int  i;
  PFIL lfp[2], ffp[2] = {NULL,NULL};

  switch (Opc) {
    case OP_NOT:
      if (GetArgType(0) == TYPE_FILTER) {
        lfp[0] = (PFIL)Arg(0);
        ffp[0] = lfp[0]->Linearize(TRUE);
        } /* endif */

      if (!ffp[0])
        return NULL;

      while (lfp[0]->Next)         // See Note above
        lfp[0] = lfp[0]->Next;

      Arg(0) = lfp[0];
      lfp[0]->Next = this;
      break;
    case OP_OR:
      nosep = TRUE;
    case OP_AND:
      for (i = 0; i < 2; i++) {
        if (GetArgType(i) == TYPE_FILTER) {
          lfp[i] = (PFIL)Arg(i);
          ffp[i] = lfp[i]->Linearize(nosep);
          } /* endif */

        if (!ffp[i])
          return NULL;

        while (lfp[i]->Next)
          lfp[i] = lfp[i]->Next;

        Arg(i) = lfp[i];
        } /* endfor i */

      if (nosep) {
        lfp[0]->Next = ffp[1];
        lfp[1]->Next = this;
      } else {
        lfp[0]->Next = this;
        Opc = OP_SEP;
        Arg(1) = pXVOID;
        Next = ffp[1];
      } /* endif */

      break;
    default:
      ffp[0] = this;
    } /* endswitch */

  return (ffp[0]);
  } // end of Linearize

/***********************************************************************/
/*  Link the fil2 filter chain to the fil1(this) filter chain.         */
/***********************************************************************/
PFIL FILTER::Link(PGLOBAL g, PFIL fil2)
  {
  PFIL fil1;

  if (trace(1))
    htrc("Linking filter %p with op=%d... to filter %p with op=%d\n",
                  this, Opc, fil2, (fil2) ? fil2->Opc : 0);

  for (fil1 = this; fil1->Next; fil1 = fil1->Next) ;

  if (fil1->Opc == OP_SEP)
    fil1->Next = fil2;              // Separator already exists
  else {
    // Create a filter separator and insert it between the chains
    PFIL filp = new(g) FILTER(g, OP_SEP);

    filp->Arg(0) = fil1;
    filp->Next = fil2;
    fil1->Next = filp;
    } // endelse

  return (this);
  } // end of Link

/***********************************************************************/
/*  Remove eventual last separator from a filter chain.                */
/***********************************************************************/
PFIL FILTER::RemoveLastSep(void)
  {
  PFIL filp, gfp = NULL;

  // Find last filter block (filp) and previous one (gfp).
  for (filp = this; filp->Next; filp = filp->Next)
    gfp = filp;

  // If last filter is a separator, remove it
  if (filp->Opc == OP_SEP)
    if (gfp)
      gfp->Next = NULL;
    else
      return NULL;       // chain is now empty

  return this;
  } // end of RemoveLastSep

/***********************************************************************/
/*  CheckColumn: Checks references to Columns in the filter and change */
/*  them into references to Col Blocks.                                */
/*  Returns the number of column references or -1 in case of column    */
/*  not found and -2 in case of unrecoverable error.                   */
/*  WHERE  filters are called with *aggreg == AGG_NO.                  */
/*  HAVING filters are called with *aggreg == AGG_ANY.                 */
/***********************************************************************/
int FILTER::CheckColumn(PGLOBAL g, PSQL sqlp, PXOB &p, int &ag)
  {
  char errmsg[MAX_STR] = "";
  int  agg, k, n = 0;

  if (trace(1))
    htrc("FILTER CheckColumn: sqlp=%p ag=%d\n", sqlp, ag);

  switch (Opc) {
    case OP_SEP:
    case OP_AND:
    case OP_OR:
    case OP_NOT:
      return 0;  // This because we are called for a linearized filter
    default:
      break;
    } // endswitch Opc

  // Check all arguments even in case of error for when we are called
  // from CheckHaving, where references to an alias raise an error but
  // we must have all other arguments to be set.
  for (int i = 0; i < 2; i++) {
    if (GetArgType(i) == TYPE_FILTER)      // Should never happen in
      return 0;                            // current implementation

    agg = ag;

    if ((k = Arg(i)->CheckColumn(g, sqlp, Arg(i), agg)) < -1) {
      return k;
    } else if (k < 0) {
      if (!*errmsg)                        // Keep first error message
        strcpy(errmsg, g->Message);

    } else
      n += k;

    } // endfor i

  if (*errmsg) {
    strcpy(g->Message, errmsg);
    return -1;
  } else
    return n;

  } // end of CheckColumn

/***********************************************************************/
/*  RefNum: Find the number of references correlated sub-queries make  */
/*  to the columns of the outer query (pointed by sqlp).               */
/***********************************************************************/
int FILTER::RefNum(PSQL sqlp)
  {
  int n = 0;

  for (int i = 0; i < 2; i++)
    n += Arg(i)->RefNum(sqlp);

  return n;
  } // end of RefNum

/***********************************************************************/
/*  CheckSubQuery: see SUBQUERY::CheckSubQuery for comment.            */
/***********************************************************************/
PXOB FILTER::CheckSubQuery(PGLOBAL g, PSQL sqlp)
  {
  switch (Opc) {
    case OP_SEP:
    case OP_AND:
    case OP_OR:
    case OP_NOT:
      break;
    default:
      for (int i = 0; i < 2; i++)
        if (!(Arg(i) = (PXOB)Arg(i)->CheckSubQuery(g, sqlp)))
          return NULL;

      break;
    } // endswitch Opc

  return this;
  } // end of CheckSubQuery

/***********************************************************************/
/*  SortJoin: function that places ahead of the list the 'good' groups */
/*  for join filtering.  These are groups with only one filter that    */
/*  specify equality between two different table columns, at least     */
/*  one is a table key column.  Doing so the join filter will be in    */
/*  general compatible with linearization of the joined table tree.    */
/*  This function has been added a further sorting on column indexing. */
/***********************************************************************/
PFIL FILTER::SortJoin(PGLOBAL g)
  {
  int     k;
  PCOL    cp1, cp2;
  PTDBASE tp1, tp2;
  PFIL    fp, filp, gfp, filstart = this, filjoin = NULL, lfp = NULL;
  bool    join = TRUE, key = TRUE;

  // This routine requires that the chain ends with a separator
  // So check for it and eventually add one if necessary
  for (filp = this; filp->Next; filp = filp->Next) ;

  if (filp->Opc != OP_SEP)
    filp->Next = new(g) FILTER(g, OP_SEP);

 again:
  for (k = (key) ? 0 : MAX_MULT_KEY; k <= MAX_MULT_KEY; k++)
    for (gfp = NULL, fp = filp = filstart; filp; filp = filp->Next)
      switch (filp->Opc) {
        case OP_SEP:
          if (join) {
            // Put this filter group into the join filter group list.
            if (!lfp)
              filjoin = fp;
            else
              lfp->Next = fp;

            if (!gfp)
              filstart = filp->Next;
            else
              gfp->Next = filp->Next;

            lfp = filp;              // last block of join filter list
          } else
            gfp = filp;              // last block of bad  filter list

          join = TRUE;
          fp = filp->Next;
          break;
        case OP_LOJ:
        case OP_ROJ:
        case OP_DTJ:
          join &= TRUE;
          break;
        case OP_EQ:
          if (join && k > 0       // So specific join operators come first
                   &&  filp->GetArgType(0) == TYPE_COLBLK
                   &&  filp->GetArgType(1) == TYPE_COLBLK) {
            cp1 = (PCOL)filp->Arg(0);
            cp2 = (PCOL)filp->Arg(1);
            tp1 = (PTDBASE)cp1->GetTo_Tdb();
            tp2 = (PTDBASE)cp2->GetTo_Tdb();

            if (tp1->GetTdb_No() != tp2->GetTdb_No()) {
              if (key)
                join &= (cp1->GetKey() == k || cp2->GetKey() == k);
              else
                join &= (tp1->GetColIndex(cp1) || tp2->GetColIndex(cp2));

            } else
              join = FALSE;

          } else
            join = FALSE;

          break;
        default:
          join = FALSE;
        } // endswitch filp->Opc

  if (key) {
    key = FALSE;
    goto again;
    } // endif key

  if (filjoin) {
    lfp->Next = filstart;
    filstart = filjoin;
    } // endif filjoin

  // Removing last separator is perhaps unuseful, but it was so
  return filstart->RemoveLastSep();
  } // end of SortJoin

/***********************************************************************/
/*  Check that this filter is a good join filter.                      */
/*  If so the opj block will be set accordingly.                       */
/*  opj points to the join block, fprec to the filter block to which   */
/*  the rest of the chain must be linked in case of success.           */
/*  teq, tek and tk2 indicates the severity of the tests:              */
/*  tk2 == TRUE means both columns must be primary keys.               */
/*  tc2 == TRUE means both args must be columns (not expression).      */
/*  tek == TRUE means at least one column must be a primary key.       */
/*  teq == TRUE means the filter operator must be OP_EQ.               */
/*  tix == TRUE means at least one column must be a simple index key.  */
/*  thx == TRUE means at least one column must be a leading index key. */
/***********************************************************************/
bool FILTER::FindJoinFilter(POPJOIN opj, PFIL fprec, bool teq, bool tek,
                            bool tk2, bool tc2, bool tix, bool thx)
  {
  if (trace(1))
    htrc("FindJoinFilter: opj=%p fprec=%p tests=(%d,%d,%d,%d)\n",
                          opj, fprec, teq, tek, tk2, tc2);

  // Firstly check that this filter is an independent filter
  // meaning that it is the only one in its own group.
  if (Next && Next->Opc != OP_SEP)
    return (Opc < 0);

  // Keep only equi-joins and specific joins (Outer and Distinct)
  // Normally specific join operators comme first because they have
  // been placed first by SortJoin.
  if (teq && Opc > OP_EQ)
    return FALSE;

  // We have a candidate for join filter, now check that it
  // fulfil the requirement about its operands, to point to
  // columns of respectively the two TDB's of that join.
  int    col1 = 0, col2 = 0;
  bool   key = tk2;
  bool   idx = FALSE, ihx = FALSE;
  PIXDEF pdx;

  for (int i = 0; i < 2; i++)
    if (GetArgType(i) == TYPE_COLBLK) {
      PCOL colp = (PCOL)Arg(i);

      if (tk2)
        key &= (colp->IsKey());
      else
        key |= (colp->IsKey());

      pdx = ((PTDBASE)colp->GetTo_Tdb())->GetColIndex(colp);
      idx |= (pdx && pdx->GetNparts() == 1);
      ihx |= (pdx != NULL);

      if      (colp->VerifyColumn(opj->GetTbx1()))
        col1 = i + 1;
      else if (colp->VerifyColumn(opj->GetTbx2()))
        col2 = i + 1;

    } else if (!tc2 && GetArgType(i) != TYPE_CONST) {
      PXOB xp = Arg(i);

      if      (xp->VerifyColumn(opj->GetTbx1()))
        col1 = i + 1;
      else if (xp->VerifyColumn(opj->GetTbx2()))
        col2 = i + 1;

    } else
      return (Opc < 0);

  if (col1 == 0 || col2 == 0)
    return (Opc < 0);

  if (((tek && !key) || (tix && !idx) || (thx && !ihx)) && Opc != OP_DTJ)
    return FALSE;

  // This is the join filter, set the join block.
  if (col1 == 1) {
    opj->SetCol1(Arg(0));
    opj->SetCol2(Arg(1));
  } else {
    opj->SetCol1(Arg(1));
    opj->SetCol2(Arg(0));

    switch (Opc) {
//    case OP_GT:  Opc = OP_LT;  break;
//    case OP_LT:  Opc = OP_GT;  break;
//    case OP_GE:  Opc = OP_LE;  break;
//    case OP_LE:  Opc = OP_GE;  break;
      case OP_LOJ:
      case OP_ROJ:
      case OP_DTJ:
        // For expended join operators, the filter must indicate
        // the way the join should be done, and not the order of
        // appearance of tables in the table list (which is kept
        // because tables are sorted in AddTdb). Therefore the
        // join is inversed, not the filter.
        opj->InverseJoin();
      default: break;
      } // endswitch Opc

  } // endif col1

  if (Opc < 0) {
    // For join operators, special processing is needed
    int  knum = 0;
    PFIL fp;

    switch (Opc) {
      case OP_LOJ:
        opj->SetJtype(JT_LEFT);
        knum = opj->GetCol2()->GetKey();
        break;
      case OP_ROJ:
        opj->SetJtype(JT_RIGHT);
        knum = opj->GetCol1()->GetKey();
        break;
      case OP_DTJ:
        for (knum = 1, fp = this->Next; fp; fp = fp->Next)
          if (fp->Opc == OP_DTJ)
            knum++;
          else if (fp->Opc != OP_SEP)
            break;

        opj->SetJtype(JT_DISTINCT);
        opj->GetCol2()->SetKey(knum);
        break;
      default:
        break;
      } // endswitch Opc

    if (knum > 1) {
      // Lets take care of a multiple key join
      // We do a minimum of checking here as it will done later
      int   k = 1;
      OPVAL op;
      BYTE  tmp[sizeof(Test[0])];

      for (fp = this->Next; k < knum && fp; fp = fp->Next) {
        switch (op = fp->Opc) {
          case OP_SEP:
            continue;
          case OP_LOJ:
            if (Opc == OP_ROJ) {
              op = Opc;
              memcpy(tmp, &fp->Test[0], sizeof(Test[0]));
              fp->Test[0] = fp->Test[1];
              memcpy(&fp->Test[1], tmp, sizeof(Test[0]));
              } // endif Opc

            k++;
            break;
          case OP_ROJ:
            if (Opc == OP_LOJ) {
              op = Opc;
              memcpy(tmp, &fp->Test[0], sizeof(Test[0]));
              fp->Test[0] = fp->Test[1];
              memcpy(&fp->Test[1], tmp, sizeof(Test[0]));
              } // endif Opc

            k++;
            break;
          case OP_DTJ:
            if (op == Opc && fp->GetArgType(1) == TYPE_COLBLK)
              ((PCOL)fp->Arg(1))->SetKey(knum);

            k++;
            break;
          default:
            break;
          } // endswitch op

        if (op != Opc)
          return TRUE;

        fp->Opc = OP_EQ;
        } // endfor fp

      } // endif k

    Opc = OP_EQ;
    } // endif Opc

  // Set the join filter operator
  opj->SetOpc(Opc);

  // Now mark the columns involved in the join filter because
  // this information will be used by the linearize program.
  // Note: this should be replaced in the future by something
  // enabling to mark tables as Parent or Child.
  opj->GetCol1()->MarkCol(U_J_EXT);
  opj->GetCol2()->MarkCol(U_J_EXT);

  // Remove the filter from the filter chain.  If the filter is
  // not last in the chain, also remove the SEP filter after it.
  if (Next)                 // Next->Opc == OP_SEP
    Next = Next->Next;

  if (!fprec)
    opj->SetFilter(Next);
  else
    fprec->Next = Next;

  return FALSE;
  } // end of FindJoinFilter

/***********************************************************************/
/*  CheckHaving: check and process a filter of an HAVING clause.       */
/*  Check references to Columns and Functions in the filter.           */
/*  All these references can correspond to items existing in the       */
/*  SELECT list, else if it is a function, allocate a SELECT block     */
/*  to be added to the To_Sel list (non projected blocks).             */
/***********************************************************************/
bool FILTER::CheckHaving(PGLOBAL g, PSQL sqlp)
  {
  int  agg = AGG_ANY;
  PXOB xp;

//sqlp->SetOk(TRUE);  // Ok to look into outer queries for filters

  switch (Opc) {
    case OP_SEP:
    case OP_AND:
    case OP_OR:
    case OP_NOT:
      return FALSE;
    default:
      if (CheckColumn(g, sqlp, xp, agg) < -1)
        return TRUE;       // Unrecovable error

      break;
    } // endswitch Opc

  sqlp->SetOk(TRUE);  // Ok to look into outer queries for filters

  for (int i = 0; i < 2; i++)
    if (!(xp = Arg(i)->SetSelect(g, sqlp, TRUE)))
      return TRUE;
    else if (xp != Arg(i)) {
      Arg(i) = xp;
      Val(i) = Arg(i)->GetValue();
      } // endif

  sqlp->SetOk(FALSE);
  return FALSE;
  } // end of CheckHaving

/***********************************************************************/
/*  Used while building a table index. This function split the filter  */
/*  attached to the tdbp table into the local and not local part.      */
/*  The local filter is used to restrict the size of the index and the */
/*  not local part remains to be executed later. This has been added   */
/*  recently and not only to improve the performance but chiefly to    */
/*  avoid loosing rows when processing distinct joins.                 */
/*  Returns:                                                           */
/*    0: the whole filter is local (both arguments are)                */
/*    1: the whole filter is not local                                 */
/*    2: the filter was split in local (attached to fp[0]) and         */
/*                           not local (attached to fp[1]).            */
/***********************************************************************/
int FILTER::SplitFilter(PFIL *fp)
  {
  int i, rc[2];

  if (Opc == OP_AND) {
    for (i = 0; i < 2; i++)
      rc[i] = ((PFIL)Arg(i))->SplitFilter(fp);

    // Filter first argument should never be split because of the
    // algorithm used to de-linearize the filter.
    assert(rc[0] != 2);

    if (rc[0] != rc[1]) {
      // Splitting to be done
      if (rc[1] == 2) {
        // 2nd argument already split, add 1st to the proper filter
        assert(fp[*rc]);
        Arg(1) = fp[*rc];
        Val(1) = fp[*rc]->GetValue();
        fp[*rc] = this;
      } else for (i = 0; i < 2; i++) {
        //  Split the filter arguments
        assert(!fp[rc[i]]);
        fp[rc[i]] = (PFIL)Arg(i);
        } // endfor i

      *rc = 2;
      } // endif rc

  } else
    *rc = (CheckLocal(NULL)) ? 0 : 1;

  return *rc;
  } // end of SplitFilter

/***********************************************************************/
/*  This function is called when making a Kindex after the filter was  */
/*  split in local and nolocal part in the case of many to many joins. */
/*  Indeed the whole filter must be reconstructed to take care of next */
/*  same values when doing the explosive join. In addition, the link   */
/*  must be done respecting the way filters are de-linearized, no AND  */
/*  filter in the first argument of an AND filter, because this is     */
/*  expected to be true if SplitFilter is used again on this filter.   */
/***********************************************************************/
PFIL FILTER::LinkFilter(PGLOBAL g, PFIL fp2)
  {
  PFIL fp1, filp, filand = NULL;

  assert(fp2);           // Test must be made by caller

  // Find where the new AND filter must be attached
  for (fp1 = this; fp1->Opc == OP_AND; fp1 = (PFIL)fp1->Arg(1))
    filand = fp1;

  filp = new(g) FILTER(g, OP_AND);
  filp->Arg(0) = fp1;
  filp->Val(0) = fp1->GetValue();
  filp->Test[0].B_T = TYPE_INT;
  filp->Test[0].Conv = FALSE;
  filp->Arg(1) = fp2;
  filp->Val(1) = fp2->GetValue();
  filp->Test[1].B_T = TYPE_INT;
  filp->Test[1].Conv = FALSE;
  filp->Value = AllocateValue(g, TYPE_INT);

  if (filand) {
    // filp must be inserted here
    filand->Arg(1) = filp;
    filand->Val(1) = filp->GetValue();
    filp = this;
    } // endif filand

  return filp;
  } // end of LinkFilter

/***********************************************************************/
/*  Checks whether filter contains reference to a previous table that  */
/*  is not logically joined to the currently opened table, or whether */
/*  it is a Sub-Select filter.  In any case, local is set to FALSE.    */
/*  Note: This function is now applied to de-linearized filters.       */
/***********************************************************************/
bool FILTER::CheckLocal(PTDB tdbp)
  {
  bool local = TRUE;

  if (trace(1)) {
    if (tdbp)
      htrc("CheckLocal: filp=%p R%d\n", this, tdbp->GetTdb_No());
    else
      htrc("CheckLocal: filp=%p\n", this);
    } // endif trace

  for (int i = 0; local && i < 2; i++)
    local = Arg(i)->CheckLocal(tdbp);

  if (trace(1))
    htrc("FCL: returning %d\n", local);

  return (local);
  } // end of CheckLocal

/***********************************************************************/
/*  This routine is used to split the filter attached to the tdbp      */
/*  table into the local and not local part where "local" means that   */
/*  it applies "locally" to the FILEID special column with crit = 2    */
/*  and to the SERVID and/or TABID special columns with crit = 3.      */
/*  Returns:                                                           */
/*    0: the whole filter is local (both arguments are)                */
/*    1: the whole filter is not local                                 */
/*    2: the filter was split in local (attached to fp[0]) and         */
/*                           not local (attached to fp[1]).            */
/*  Note: "Locally" means that the "local" filter can be evaluated     */
/*  before opening the table. This implies that the special column be  */
/*  compared only with constants and that this filter not to be or'ed  */
/*  with a non "local" filter.                                         */
/***********************************************************************/
int FILTER::SplitFilter(PFIL *fp, PTDB tp, int crit)
  {
  int i, rc[2];

  if (Opc == OP_AND) {
    for (i = 0; i < 2; i++)
      rc[i] = ((PFIL)Arg(i))->SplitFilter(fp, tp, crit);

    // Filter first argument should never be split because of the
    // algorithm used to de-linearize the filter.
    assert(rc[0] != 2);

    if (rc[0] != rc[1]) {
      // Splitting to be done
      if (rc[1] == 2) {
        // 2nd argument already split, add 1st to the proper filter
        assert(fp[*rc]);
        Arg(1) = fp[*rc];
        Val(1) = fp[*rc]->GetValue();
        fp[*rc] = this;
      } else for (i = 0; i < 2; i++) {
        //  Split the filter arguments
        assert(!fp[rc[i]]);
        fp[rc[i]] = (PFIL)Arg(i);
        } // endfor i

      *rc = 2;
      } // endif rc

  } else
    *rc = (CheckSpcCol(tp, crit) == 1) ? 0 : 1;

  return *rc;
  } // end of SplitFilter

/***********************************************************************/
/*  Checks whether filter contains only references to FILEID, SERVID,  */
/*  or TABID with constants or pseudo constants.                       */
/***********************************************************************/
int FILTER::CheckSpcCol(PTDB tdbp, int n)
  {
  int n1 = Arg(0)->CheckSpcCol(tdbp, n);
  int n2 = Arg(1)->CheckSpcCol(tdbp, n);

  return max(n1, n2);
  } // end of CheckSpcCol
#endif // 0

/***********************************************************************/
/*  Reset the filter arguments to non evaluated yet.                   */
/***********************************************************************/
void FILTER::Reset(void)
  {
  for (int i = 0; i < 2; i++)
    Arg(i)->Reset();

  } // end of Reset

/***********************************************************************/
/*  Init: called when reinitializing a query (Correlated subqueries)   */
/***********************************************************************/
bool FILTER::Init(PGLOBAL g)
  {
  for (int i = 0; i < 2; i++)
    Arg(i)->Init(g);

  return FALSE;
  } // end of Init

/***********************************************************************/
/*  Convert:  does all filter setting and conversions.                 */
/*   (having = TRUE for Having Clauses, FALSE for Where Clauses)       */
/*      Note: hierarchy of types is implied by the ConvertType         */
/*      function, currently FLOAT, int, STRING and TOKEN.              */
/*  Returns FALSE if successful or TRUE in case of error.              */
/*  Note on result type for filters:                                   */
/*    Currently the result type is of TYPE_INT (should be TYPE_BOOL).  */
/*    This avoids to introduce a new type and perhaps will permit      */
/*    conversions. However the boolean operators will result in a      */
/*    boolean int result, meaning that result shall be only 0 or 1  .  */
/***********************************************************************/
bool FILTER::Convert(PGLOBAL g, bool having)
  {
  int i, comtype = TYPE_ERROR;

  if (trace(1))
    htrc("converting(?) %s %p opc=%d\n",
          (having) ? "having" : "filter", this, Opc);

  for (i = 0; i < 2; i++) {
    switch (GetArgType(i)) {
      case TYPE_COLBLK:
        if (((PCOL)Arg(i))->InitValue(g))
          return TRUE;

        break;
      case TYPE_ARRAY:
        if ((Opc != OP_IN && !Opm) || i == 0) {
          strcpy(g->Message, MSG(BAD_ARRAY_OPER));
          return TRUE;
          } // endif

        if (((PARRAY)Arg(i))->Sort(g))  // Sort the array
          return TRUE;                  // Error

        break;
      case TYPE_VOID:
        if (i == 1) {
          Val(0) = Arg(0)->GetValue();
          goto TEST;             // Filter has only one argument
          } // endif i

        strcpy(g->Message, MSG(VOID_FIRST_ARG));
        return TRUE;
      } // endswitch

    if (trace(1))
      htrc("Filter(%d): Arg type=%d\n", i, GetArgType(i));

    // Set default values
    Test[i].B_T = Arg(i)->GetResultType();
    Test[i].Conv = FALSE;

    // Special case of the LIKE operator.
    if (Opc == OP_LIKE) {
      if (!IsTypeChar((int)Test[i].B_T)) {
        sprintf(g->Message, MSG(BAD_TYPE_LIKE), i, Test[i].B_T);
        return TRUE;
        } // endif

      comtype = TYPE_STRING;
    } else {
      // Set the common type for both (eventually converted) arguments
      int argtyp = Test[i].B_T;

      if (GetArgType(i) == TYPE_CONST && argtyp == TYPE_INT) {
        // If possible, downcast the type to smaller types to avoid
        // convertion as much as possible.
        int n = Arg(i)->GetValue()->GetIntValue();

        if (n >= INT_MIN8 && n <= INT_MAX8)
          argtyp = TYPE_TINY;
        else if (n >= INT_MIN16 && n <= INT_MAX16)
          argtyp = TYPE_SHORT;

      } else if (GetArgType(i) == TYPE_ARRAY) {
        // If possible, downcast int arrays target type to TYPE_SHORT
        // to take  care of filters written like shortcol in (34,35,36).
        if (((PARRAY)Arg(i))->CanBeShort())
          argtyp = TYPE_SHORT;

      } // endif TYPE_CONST

      comtype = ConvertType(comtype, argtyp, CNV_ANY);
    } // endif Opc

    if (comtype == TYPE_ERROR) {
      strcpy(g->Message, MSG(ILL_FILTER_CONV));
      return TRUE;
      } // endif

    if (trace(1))
      htrc(" comtype=%d, B_T(%d)=%d Val(%d)=%p\n",
             comtype, i, Test[i].B_T, i, Val(i));

    } // endfor i

  // Set or allocate the filter argument values and buffers
  for (i = 0; i < 2; i++) {
    if (trace(1))
      htrc(" conv type %d ? i=%d B_T=%d comtype=%d\n",
            GetArgType(i), i, Test[i].B_T, comtype);

    if (Test[i].B_T == comtype) {
      // No conversion, set Value to argument Value
      Val(i) = Arg(i)->GetValue();
#if defined(_DEBUG)
      assert (Val(i) && Val(i)->GetType() == Test[i].B_T);
#endif
    } else {
      //  Conversion between filter arguments to be done.
      //  Note that the argument must be converted, not only the
      //  buffer and buffer type, so GetArgType() returns the new type.
      switch (GetArgType(i)) {
        case TYPE_CONST:
          if (comtype == TYPE_DATE && Test[i].B_T == TYPE_STRING) {
            // Convert according to the format of the other argument
            Val(i) = AllocateValue(g, comtype, Arg(i)->GetLength());

            if (((DTVAL*)Val(i))->SetFormat(g, Val(1-i)))
              return TRUE;

            Val(i)->SetValue_psz(Arg(i)->GetValue()->GetCharValue());
          } else {
            ((PCONST)Arg(i))->Convert(g, comtype);
            Val(i) = Arg(i)->GetValue();
          } // endif comtype

          break;
        case TYPE_ARRAY:
          // Conversion PSZ or int array to int or double FLOAT.
          if (((PARRAY)Arg(i))->Convert(g, comtype, Val(i-1)) == TYPE_ERROR)
            return TRUE;

          break;
        case TYPE_FILTER:
          strcpy(g->Message, MSG(UNMATCH_FIL_ARG));
          return TRUE;
        default:
          // Conversion from Column, Select/Func, Expr, Scalfnc...
          // The argument requires conversion during Eval
          // A separate Value block must be allocated.
          // Note: the test on comtype is to prevent unnecessary
          // domain initialization and get the correct length in
          // case of Token -> numeric conversion.
          Val(i) = AllocateValue(g, comtype, (comtype == TYPE_STRING)
                 ? Arg(i)->GetLengthEx() : Arg(i)->GetLength());

          if (comtype == TYPE_DATE && Test[i].B_T == TYPE_STRING)
            // Convert according to the format of the other argument
            if (((DTVAL*)Val(i))->SetFormat(g, Val(1 - i)))
              return TRUE;

          Test[i].Conv = TRUE;
          break;
        } // endswitch GetType

      Test[i].B_T = comtype;
    } // endif comtype

    } // endfor i

  //  Last check to be sure all is correct.
  if (Test[0].B_T != Test[1].B_T) {
    sprintf(g->Message, MSG(BAD_FILTER_CONV), Test[0].B_T, Test[1].B_T);
    return TRUE;
//} else if (Test[0].B_T == TYPE_LIST &&
//          ((LSTVAL*)Val(0))->GetN() != ((LSTVAL*)Val(1))->GetN()) {
//  sprintf(g->Message, MSG(ROW_ARGNB_ERR),
//          ((LSTVAL*)Val(0))->GetN(), ((LSTVAL*)Val(1))->GetN());
//  return TRUE;
  } // endif's B_T


 TEST: // Test for possible Eval optimization

  if (trace(1))
    htrc("Filp %p op=%d argtypes=(%d,%d)\n",
          this, Opc, GetArgType(0), GetArgType(1));

  // Check whether we have a "simple" filter and in that case
  // change its class so an optimized Eval function will be used
  if (!Test[0].Conv && !Test[1].Conv) {
    if (Opm) switch (Opc) {
      case OP_EQ:
      case OP_NE:
      case OP_GT:
      case OP_GE:
      case OP_LT:
      case OP_LE:
        if (GetArgType(1) != TYPE_ARRAY)
          break;      // On subquery, do standard processing

        // Change the FILTER class to FILTERIN
        new(this) FILTERIN;
        break;
      default:
        break;
      } // endswitch Opc

    else switch (Opc) {
#if 0
      case OP_EQ:  new(this) FILTEREQ;  break;
      case OP_NE:  new(this) FILTERNE;  break;
      case OP_GT:  new(this) FILTERGT;  break;
      case OP_GE:  new(this) FILTERGE;  break;
      case OP_LT:  new(this) FILTERLT;  break;
      case OP_LE:  new(this) FILTERLE;  break;
#endif // 0
      case OP_EQ:
      case OP_NE:
      case OP_GT:
      case OP_GE:
      case OP_LT:
      case OP_LE:  new(this) FILTERCMP(g); break;
      case OP_AND: new(this) FILTERAND; break;
      case OP_OR:  new(this) FILTEROR;  break;
      case OP_NOT: new(this) FILTERNOT; break;
      case OP_EXIST:
        if (GetArgType(1) == TYPE_VOID) {
          // For EXISTS it is the first argument that should be null
          Arg(1) = Arg(0);
          Arg(0) = pXVOID;
          } // endif void

        // fall through
      case OP_IN:
        // For IN operator do optimize if operand is an array
        if (GetArgType(1) != TYPE_ARRAY)
          break;      // IN on subquery, do standard processing

        // Change the FILTER class to FILTERIN
        new(this) FILTERIN;
        break;
      default:
        break;
      } // endswitch Opc

    } // endif Conv

  // The result value (should be TYPE_BOOL ???)
  Value = AllocateValue(g, TYPE_INT);
  return FALSE;
  } // end of Convert

/***********************************************************************/
/*  Eval: Compute filter result value.                                 */
/*  New algorithm: evaluation is now done from the root for each group */
/*  so Eval is now a recursive process for FILTER operands.            */
/***********************************************************************/
bool FILTER::Eval(PGLOBAL g)
  {
  int     i; // n = 0;
//PSUBQ   subp = NULL;
  PARRAY  ap = NULL;

  (void) PlgGetUser(g);

  if (Opc <= OP_XX)
  {
    for (i = 0; i < 2; i++)
    {
      // Evaluate the object and eventually convert it.
      if (Arg(i)->Eval(g))
        return TRUE;
      else if (Test[i].Conv)
        Val(i)->SetValue_pval(Arg(i)->GetValue());
    }
  }

  if (trace(1))
    htrc(" Filter: op=%d type=%d %d B_T=%d %d val=%p %p\n",
          Opc, GetArgType(0), GetArgType(1), Test[0].B_T, Test[1].B_T,
          Val(0), Val(1));

  // Main switch on filtering according to operator type.
  switch (Opc) {
    case OP_EQ:
    case OP_NE:
    case OP_GT:
    case OP_GE:
    case OP_LT:
    case OP_LE:
      if (!Opm) {
        //  Comparison boolean operators.
#if defined(_DEBUG)
        if (Val(0)->GetType() != Val(1)->GetType())
          goto FilterError;
#endif
        // Compare the two arguments
        // New algorithm to take care of TYPE_LIST
        Bt = OpBmp(g, Opc);
        Value->SetValue_bool(!(Val(0)->TestValue(Val(1)) & Bt));
        break;
        } // endif Opm

      // For modified operators, pass thru
      /* fall through */
    case OP_IN:
    case OP_EXIST:
      // For IN operations, special processing is done here
      switch (GetArgType(1)) {
        case TYPE_ARRAY:
          ap = (PARRAY)Arg(1);
          break;
        default:
          strcpy(g->Message, MSG(IN_WITHOUT_SUB));
          goto FilterError;
        } // endswitch Type

      if (trace(1)) {
        htrc(" IN filtering: ap=%p\n", ap);

        if (ap)
          htrc(" Array: type=%d size=%d other_type=%d\n",
                ap->GetType(), ap->GetSize(), Test[0].B_T);

        } // endif trace

      /*****************************************************************/
      /*  Implementation note: The Find function is now able to do a   */
      /*  conversion but limited to SHORT, int, and FLOAT arrays.     */
      /*****************************************************************/
//    Value->SetValue_bool(ap->Find(g, Val(0)));

      if (ap)
        Value->SetValue_bool(ap->FilTest(g, Val(0), Opc, Opm));

      break;

    case OP_LIKE:
#if defined(_DEBUG)
      if (!IsTypeChar((int)Test[0].B_T) || !IsTypeChar((int)Test[1].B_T))
        goto FilterError;
#endif
      if (Arg(0)->Eval(g))
        return TRUE;

      Value->SetValue_bool(PlugEvalLike(g, Val(0)->GetCharValue(),
                                           Val(1)->GetCharValue(),
                                           Val(0)->IsCi()));
      break;

    case OP_AND:
#if defined(_DEBUG)
      if (Test[0].B_T != TYPE_INT || Test[1].B_T != TYPE_INT)
        goto FilterError;
#endif

      if (Arg(0)->Eval(g))
        return TRUE;

      Value->SetValue(Val(0)->GetIntValue());

      if (!Value->GetIntValue())
        return FALSE;   // No need to evaluate 2nd argument

      if (Arg(1)->Eval(g))
        return TRUE;

      Value->SetValue(Val(1)->GetIntValue());
      break;

    case OP_OR:
#if defined(_DEBUG)
      if (Test[0].B_T != TYPE_INT || Test[1].B_T != TYPE_INT)
        goto FilterError;
#endif

      if (Arg(0)->Eval(g))
        return TRUE;

      Value->SetValue(Val(0)->GetIntValue());

      if (Value->GetIntValue())
        return FALSE;   // No need to evaluate 2nd argument

      if (Arg(1)->Eval(g))
        return TRUE;

      Value->SetValue(Val(1)->GetIntValue());
      break;

    case OP_NOT:
#if defined(_DEBUG)
      if (Test[0].B_T != TYPE_INT)      // Should be type bool ???
        goto FilterError;
#endif

      if (Arg(0)->Eval(g))
        return TRUE;

      Value->SetValue_bool(!Val(0)->GetIntValue());
      break;

    case OP_SEP:   // No more used while evaluating
    default:
      goto FilterError;
    } // endswitch Opc

  if (trace(1))
    htrc("Eval: filter %p Opc=%d result=%d\n",
                this, Opc, Value->GetIntValue());

  return FALSE;

 FilterError:
  sprintf(g->Message, MSG(BAD_FILTER),
          Opc, Test[0].B_T, Test[1].B_T, GetArgType(0), GetArgType(1));
  return TRUE;
  } // end of Eval

#if 0
/***********************************************************************/
/*  Called by PlugCopyDB to make a copy of a (linearized) filter chain.*/
/***********************************************************************/
PFIL FILTER::Copy(PTABS t)
  {
  int  i;
  PFIL fil1, fil2, newfilchain = NULL, fprec = NULL;

  for (fil1 = this; fil1; fil1 = fil1->Next) {
    fil2 = new(t->G) FILTER(fil1);

    if (!fprec)
      newfilchain = fil2;
    else
      fprec->Next = fil2;

    NewPointer(t, fil1, fil2);

    for (i = 0; i < 2; i++)
      if (fil1->GetArgType(i) == TYPE_COLBLK ||
          fil1->GetArgType(i) == TYPE_FILTER)
        AddPointer(t, &fil2->Arg(i));

    fprec = fil2;
    } /* endfor fil1 */

  return newfilchain;
  } // end of Copy
#endif // 0

/*********************************************************************/
/*  Make file output of FILTER contents.                             */
/*********************************************************************/
void FILTER::Printf(PGLOBAL g, FILE *f, uint n)
  {
  char m[64];

  memset(m, ' ', n);                    // Make margin string
  m[n] = '\0';

  bool lin = (Next != NULL);            // lin == TRUE if linearized

  for (PFIL fp = this; fp; fp = fp->Next) {
    fprintf(f, "%sFILTER: at %p opc=%d lin=%d result=%d\n",
            m, fp, fp->Opc, lin,
            (Value) ? Value->GetIntValue() : 0);

    for (int i = 0; i < 2; i++) {
      fprintf(f, "%s Arg(%d) type=%d value=%p B_T=%d val=%p\n",
              m, i, fp->GetArgType(i), fp->Arg(i),
                    fp->Test[i].B_T, fp->Val(i));

      if (lin && fp->GetArgType(i) == TYPE_FILTER)
        fprintf(f, "%s  Filter at %p\n", m, fp->Arg(i));
      else
        fp->Arg(i)->Printf(g, f, n + 2);

      } // endfor i

    } // endfor fp

  } // end of Printf

/***********************************************************************/
/*  Make string output of TABLE contents (z should be checked).        */
/***********************************************************************/
void FILTER::Prints(PGLOBAL g, char *ps, uint z)
  {
  #define FLEN 100

  typedef struct _bc {
    struct _bc *Next;
    char   Cold[FLEN+1];
    } BC, *PBC;

  char *p;
  int   n;
  PFIL  fp;
  PBC   bxp, bcp = NULL;

  *ps = '\0';

  for (fp = this; fp && z > 0; fp = fp->Next) {
    if (fp->Opc < OP_CNC || fp->Opc == OP_IN || fp->Opc == OP_NULL
                         || fp->Opc == OP_LIKE || fp->Opc == OP_EXIST) {
      if (!(bxp = new BC)) {
        strncat(ps, "Filter(s)", z);
        return;
        } /* endif */

      bxp->Next = bcp;
      bcp = bxp;
      p = bcp->Cold;
      n = FLEN;
      fp->Arg(0)->Prints(g, p, n);
      n = FLEN - strlen(p);

      switch (fp->Opc) {
        case OP_EQ:
          strncat(bcp->Cold, "=", n);
          break;
        case OP_NE:
          strncat(bcp->Cold, "!=", n);
          break;
        case OP_GT:
          strncat(bcp->Cold, ">", n);
          break;
        case OP_GE:
          strncat(bcp->Cold, ">=", n);
          break;
        case OP_LT:
          strncat(bcp->Cold, "<", n);
          break;
        case OP_LE:
          strncat(bcp->Cold, "<=", n);
          break;
        case OP_IN:
          strncat(bcp->Cold, " in ", n);
          break;
        case OP_NULL:
          strncat(bcp->Cold, " is null", n);
          break;
        case OP_LIKE:
          strncat(bcp->Cold, " like ", n);
          break;
        case OP_EXIST:
          strncat(bcp->Cold, " exists ", n);
          break;
        case OP_AND:
          strncat(bcp->Cold, " and ", n);
          break;
        case OP_OR:
          strncat(bcp->Cold, " or ", n);
          break;
        default:
          strncat(bcp->Cold, "?", n);
        } // endswitch Opc

      n = FLEN - strlen(p);
      p += strlen(p);
      fp->Arg(1)->Prints(g, p, n);
    } else
      if (!bcp) {
        strncat(ps, "???", z);
        z -= 3;
      } else
        switch (fp->Opc) {
          case OP_SEP:                    // Filter list separator
            strncat(ps, bcp->Cold, z);
            z -= strlen(bcp->Cold);
            strncat(ps, ";", z--);
            bxp = bcp->Next;
            delete bcp;
            bcp = bxp;
            break;
          case OP_NOT:                    // Filter NOT operator
            for (n = MY_MIN((int)strlen(bcp->Cold), FLEN-3); n >= 0; n--)
              bcp->Cold[n+2] = bcp->Cold[n];
            bcp->Cold[0] = '^';
            bcp->Cold[1] = '(';
            strcat(bcp->Cold, ")");
            break;
          default:
            for (n = MY_MIN((int)strlen(bcp->Cold), FLEN-4); n >= 0; n--)
              bcp->Cold[n+3] = bcp->Cold[n];
            bcp->Cold[0] = ')';
            switch (fp->Opc) {
              case OP_AND: bcp->Cold[1] = '&'; break;
              case OP_OR:  bcp->Cold[1] = '|'; break;
              default: bcp->Cold[1] = '?';
              } // endswitch
            bcp->Cold[2] = '(';
            strcat(bcp->Cold, ")");
            bxp = bcp->Next;
            for (n = MY_MIN((int)strlen(bxp->Cold), FLEN-1); n >= 0; n--)
              bxp->Cold[n+1] = bxp->Cold[n];
            bxp->Cold[0] = '(';
            strncat(bxp->Cold, bcp->Cold, FLEN-strlen(bxp->Cold));
            delete bcp;
            bcp = bxp;
          } // endswitch

    } // endfor fp

  n = 0;

  if (!bcp)
    strncat(ps, "Null-Filter", z);
  else do {
    if (z > 0) {
      if (n++ > 0) {
        strncat(ps, "*?*", z);
        z = MY_MAX(0, (int)z-3);
        } // endif
      strncat(ps, bcp->Cold, z);
      z -= strlen(bcp->Cold);
      } // endif

    bxp = bcp->Next;
    delete bcp;
    bcp = bxp;
    } while (bcp); // enddo

  } // end of Prints


/* -------------------- Derived Classes Functions -------------------- */

/***********************************************************************/
/*  FILTERCMP constructor.                                             */
/***********************************************************************/
FILTERCMP::FILTERCMP(PGLOBAL g)
  {
  Bt = OpBmp(g, Opc);
  } // end of FILTERCMP constructor

/***********************************************************************/
/*  Eval: Compute result value for comparison operators.               */
/***********************************************************************/
bool FILTERCMP::Eval(PGLOBAL g)
  {
  if (Arg(0)->Eval(g) || Arg(1)->Eval(g))
    return TRUE;

  Value->SetValue_bool(!(Val(0)->TestValue(Val(1)) & Bt));
  return FALSE;
  } // end of Eval

/***********************************************************************/
/*  Eval: Compute result value for AND filters.                        */
/***********************************************************************/
bool FILTERAND::Eval(PGLOBAL g)
  {
  if (Arg(0)->Eval(g))
    return TRUE;

  Value->SetValue(Val(0)->GetIntValue());

  if (!Value->GetIntValue())
    return FALSE;   // No need to evaluate 2nd argument

  if (Arg(1)->Eval(g))
    return TRUE;

  Value->SetValue(Val(1)->GetIntValue());
  return FALSE;
  } // end of Eval

/***********************************************************************/
/*  Eval: Compute result value for OR filters.                         */
/***********************************************************************/
bool FILTEROR::Eval(PGLOBAL g)
  {
  if (Arg(0)->Eval(g))
    return TRUE;

  Value->SetValue(Val(0)->GetIntValue());

  if (Value->GetIntValue())
    return FALSE;   // No need to evaluate 2nd argument

  if (Arg(1)->Eval(g))
    return TRUE;

  Value->SetValue(Val(1)->GetIntValue());
  return FALSE;
  } // end of Eval

/***********************************************************************/
/*  Eval: Compute result value for NOT filters.                        */
/***********************************************************************/
bool FILTERNOT::Eval(PGLOBAL g)
  {
  if (Arg(0)->Eval(g))
    return TRUE;

  Value->SetValue_bool(!Val(0)->GetIntValue());
  return FALSE;
  } // end of Eval

/***********************************************************************/
/*  Eval: Compute result value for IN filters.                         */
/***********************************************************************/
bool FILTERIN::Eval(PGLOBAL g)
  {
  if (Arg(0)->Eval(g))
    return TRUE;

  Value->SetValue_bool(((PARRAY)Arg(1))->FilTest(g, Val(0), Opc, Opm));
  return FALSE;
  } // end of Eval

/***********************************************************************/
/*  FILTERTRUE does nothing and returns TRUE.                          */
/***********************************************************************/
void FILTERTRUE::Reset(void)
  {
  } // end of Reset

bool FILTERTRUE::Eval(PGLOBAL)
  {
  return FALSE;
  } // end of Eval

/* ------------------------- Friend Functions ------------------------ */

#if 0
/***********************************************************************/
/*  Prepare: prepare a filter for execution. This implies two things:  */
/*  1) de-linearize the filter to be able to evaluate it recursively.  */
/*     This permit to conditionally evaluate only the first argument   */
/*     of OP_OR and OP_AND filters without having to pass by an        */
/*     intermediate Apply function (as this has a performance cost).   */
/*  2) do all the necessary conversion for all filter block arguments. */
/***********************************************************************/
PFIL PrepareFilter(PGLOBAL g, PFIL fp, bool having)
  {
  PFIL filp = NULL;

  if (trace(1))
    htrc("PrepareFilter: fp=%p having=%d\n", fp, having);

  while (fp) {
    if (fp->Opc == OP_SEP)
      // If separator is not last transform it into an AND filter
      if (fp->Next) {
        filp = PrepareFilter(g, fp->Next, having);
        fp->Arg(1) = filp;
        fp->Opc = OP_AND;
        fp->Next = NULL;     // This will end the loop
      } else
        break;  // Remove eventual ending separator(s)

//  if (fp->Convert(g, having))
//			throw (int)TYPE_FILTER;

    filp = fp;
    fp = fp->Next;
    filp->Next = NULL;
    } // endwhile

  if (trace(1))
    htrc(" returning filp=%p\n", filp);

  return filp;
  } // end of PrepareFilter
#endif // 0

/***********************************************************************/
/*  ApplyFilter: Apply filtering for a table (where or having clause). */
/*  New algorithm: evaluate from the root a de-linearized filter so    */
/*  AND/OR clauses can be optimized throughout the whole tree.         */
/***********************************************************************/
DllExport bool ApplyFilter(PGLOBAL g, PFIL filp)
  {
  if (!filp)
    return TRUE;

  // Must be done for null tables
  filp->Reset();

//if (tdbp && tdbp->IsNull())
//  return TRUE;

  if (filp->Eval(g))
		throw (int)TYPE_FILTER;

  if (trace(2))
    htrc("PlugFilter filp=%p result=%d\n",
                     filp, filp->GetResult());

  return filp->GetResult();
  } // end of ApplyFilter
