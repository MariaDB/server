/************* Array C++ Functions Source Code File (.CPP) *************/
/*  Name: ARRAY.CPP  Version 2.3                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2019    */
/*                                                                     */
/*  This file contains the XOBJECT derived class ARRAY functions.      */
/*  ARRAY is used for elaborate type of processing, such as sorting    */
/*  and dichotomic search (Find). This new version does not use sub    */
/*  classes anymore for the different types but relies entirely on the */
/*  functionalities provided by the VALUE and VALBLK classes.          */
/*  Currently the only supported types are STRING, SHORT, int, DATE,  */
/*  TOKEN, DOUBLE, and Compressed Strings.                             */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#include "sql_class.h"
//#include "sql_time.h"

#if defined(_WIN32)
//#include <windows.h>
#else   // !_WIN32
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>      // for uintprt_h
#endif  // !_WIN32

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/*  xobject.h   is header containing XOBJECT derived classes declares. */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "array.h"
//#include "select.h"
//#include "query.h"
//#include "token.h"

/***********************************************************************/
/*  Macro definitions.                                                 */
/***********************************************************************/
#if defined(_DEBUG)
#define ASSERT(B)      assert(B);
#else
#define ASSERT(B)
#endif

/***********************************************************************/
/*  DB static external variables.                                      */
/***********************************************************************/
extern MBLOCK Nmblk;                /* Used to initialize MBLOCK's     */

/***********************************************************************/
/*  External functions.                                                */
/***********************************************************************/
BYTE OpBmp(PGLOBAL g, OPVAL opc);
void EncodeValue(int *lp, char *strp, int n);
PARRAY MakeValueArray(PGLOBAL g, PPARM pp);  // avoid gcc warning

/***********************************************************************/
/*  MakeValueArray: Makes a value array from a value list.             */
/***********************************************************************/
PARRAY MakeValueArray(PGLOBAL g, PPARM pp)
{
  int    n, valtyp = 0;
  size_t len = 0;
  PARRAY par;
  PPARM  parmp;

  if (!pp)
    return NULL;

  /*********************************************************************/
  /*  New version with values coming in a list.                        */
  /*********************************************************************/
  if ((valtyp = pp->Type) != TYPE_STRING)
    len = 1;

 	xtrc(1, "valtyp=%d len=%d\n", valtyp, len);
  		
  /*********************************************************************/
  /*  Firstly check the list and count the number of values in it.     */
  /*********************************************************************/
  for (n = 0, parmp = pp; parmp; n++, parmp = parmp->Next)
    if (parmp->Type != valtyp) {
      sprintf(g->Message, MSG(BAD_PARAM_TYPE), "MakeValueArray", parmp->Type);
      return NULL;
    } else if (valtyp == TYPE_STRING)
      len = MY_MAX(len, strlen((char*)parmp->Value));

  /*********************************************************************/
  /*  Make an array object with one block of the proper size.          */
  /*********************************************************************/
  par = new(g) ARRAY(g, valtyp, n, (int)len);

  if (par->GetResultType() == TYPE_ERROR)
    return NULL;          // Memory allocation error in ARRAY

  /*********************************************************************/
  /*  All is right now, fill the array block.                          */
  /*********************************************************************/
  for (parmp = pp; parmp; parmp = parmp->Next)
    switch (valtyp) {
      case TYPE_STRING:
        par->AddValue(g, (PSZ)parmp->Value);
        break;
      case TYPE_SHORT:
        par->AddValue(g, *(short*)parmp->Value);
        break;
      case TYPE_INT:
        par->AddValue(g, *(int*)parmp->Value);
        break;
      case TYPE_DOUBLE:
        par->AddValue(g, *(double*)parmp->Value);
        break;
      case TYPE_PCHAR:
        par->AddValue(g, parmp->Value);
        break;
      case TYPE_VOID:
        // Integer stored inside pp->Value
        par->AddValue(g, parmp->Intval);
        break;
    } // endswitch valtyp

  /*********************************************************************/
  /*  Send back resulting array.                                       */
  /*********************************************************************/
  return par;
} // end of MakeValueArray

/* -------------------------- Class ARRAY ---------------------------- */

/***********************************************************************/
/*  ARRAY public constructor.                                          */
/***********************************************************************/
ARRAY::ARRAY(PGLOBAL g, int type, int size, int length, int prec)
     : CSORT(false)
  {
  Nval = 0;
  Ndif = 0;
  Bot = 0;
  Top = 0;
  Size = size;
  Type = type;
  Xsize = -1;
  Len = 1;
	X = 0;
	Inf = 0;
	Sup = 0;

  switch (type) {
    case TYPE_STRING:
      Len = length;
      /* fall through */
    case TYPE_SHORT:
    case TYPE_INT:
    case TYPE_DOUBLE:
    case TYPE_PCHAR:
      Type = type;
      break;
    case TYPE_VOID:
      Type = TYPE_INT;
      break;
#if 0
    case TYPE_TOKEN:
      break;
    case TYPE_LIST:
      Len = 0;
      prec = length;
      break;
#endif // 0
    default:  // This is illegal an causes an ill formed array building
      sprintf(g->Message, MSG(BAD_ARRAY_TYPE), type);
      Type = TYPE_ERROR;
      return;
    } // endswitch type

  Valblk = new(g) MBVALS;

  if (!(Vblp = Valblk->Allocate(g, Type, Len, prec, Size)))
    Type = TYPE_ERROR;
  else if (!Valblk->GetMemp() && Type != TYPE_LIST)
    // The error message was built by PlgDBalloc
    Type = TYPE_ERROR;
  else if (type != TYPE_PCHAR)
    Value = AllocateValue(g, type, Len, prec);

  Constant = true;
  } // end of ARRAY constructor

#if 0
/***********************************************************************/
/*  ARRAY public constructor from a QUERY.                             */
/***********************************************************************/
ARRAY::ARRAY(PGLOBAL g, PQUERY qryp) : CSORT(false)
  {
  Type = qryp->GetColType(0);
  Nval = qryp->GetNblin();
  Ndif = 0;
  Bot = 0;
  Top = 0;
  Size = Nval;
  Xsize = -1;
  Len = qryp->GetColLength(0);
  X = Inf = Sup = 0;
  Correlated = false;

  switch (Type) {
    case TYPE_STRING:
    case TYPE_SHORT:
    case TYPE_INT:
    case TYPE_DATE:
    case TYPE_DOUBLE:
//  case TYPE_TOKEN:
//  case TYPE_LIST:
//    Valblk = qryp->GetCol(0)->Result;
//    Vblp = qryp->GetColBlk(0);
//    Value = qryp->GetColValue(0);
//    break;
    default:  // This is illegal an causes an ill formed array building
      sprintf(g->Message, MSG(BAD_ARRAY_TYPE), Type);
      Type = TYPE_ERROR;
    } // endswitch type

  if (!Valblk || (!Valblk->GetMemp() && Type != TYPE_LIST))
    // The error message was built by ???
    Type = TYPE_ERROR;

  Constant = true;
  } // end of ARRAY constructor

/***********************************************************************/
/*  ARRAY constructor from a TYPE_LIST subarray.                       */
/***********************************************************************/
ARRAY::ARRAY(PGLOBAL g, PARRAY par, int k) : CSORT(false)
  {
  int     prec;
  LSTBLK *lp;

  if (par->Type != TYPE_LIST) {
    Type = TYPE_ERROR;
    return;
    } // endif Type

  lp = (LSTBLK*)par->Vblp;

  Nval = par->Nval;
  Ndif = 0;
  Bot = 0;
  Top = 0;
  Size = par->Size;
  Xsize = -1;

  Valblk = lp->Mbvk[k];
  Vblp = Valblk->Vblk;
  Type = Vblp->GetType();
  Len = (Type == TYPE_STRING) ? Vblp->GetVlen() : 0;
  prec = (Type == TYPE_FLOAT) ? 2 : 0;
  Value = AllocateValue(g, Type, Len, prec, NULL);
  Constant = true;
  } // end of ARRAY constructor

/***********************************************************************/
/*  Empty: reset the array for a new use (correlated queries).         */
/*  Note: this is temporary as correlated queries will not use arrays  */
/*  anymore with future optimized algorithms.                          */
/***********************************************************************/
void ARRAY::Empty(void)
  {
  assert(Correlated);
  Nval = Ndif = 0;
  Bot = Top = X = Inf = Sup = 0;
  } // end of Empty
#endif // 0

/***********************************************************************/
/*  Add a string element to an array.                                  */
/***********************************************************************/
bool ARRAY::AddValue(PGLOBAL g, PSZ strp)
{
  if (Type != TYPE_STRING) {
    sprintf(g->Message, MSG(ADD_BAD_TYPE), GetTypeName(Type), "CHAR");
    return true;
  } // endif Type

  xtrc(1, " adding string(%d): '%s'\n", Nval, strp);
  Vblp->SetValue(strp, Nval++);
  return false;
} // end of AddValue

/***********************************************************************/
/*  Add a char pointer element to an array.                            */
/***********************************************************************/
bool ARRAY::AddValue(PGLOBAL g, void *p)
{
  if (Type != TYPE_PCHAR) {
    sprintf(g->Message, MSG(ADD_BAD_TYPE), GetTypeName(Type), "PCHAR");
    return true;
  } // endif Type

  xtrc(1, " adding pointer(%d): %p\n", Nval, p);
  Vblp->SetValue((PSZ)p, Nval++);
  return false;
} // end of AddValue

/***********************************************************************/
/*  Add a short integer element to an array.                           */
/***********************************************************************/
bool ARRAY::AddValue(PGLOBAL g, short n)
{
  if (Type != TYPE_SHORT) {
    sprintf(g->Message, MSG(ADD_BAD_TYPE), GetTypeName(Type), "SHORT");
    return true;
  } // endif Type

  xtrc(1, " adding SHORT(%d): %hd\n", Nval, n);
  Vblp->SetValue(n, Nval++);
  return false;
} // end of AddValue

/***********************************************************************/
/*  Add an integer element to an array.                                */
/***********************************************************************/
bool ARRAY::AddValue(PGLOBAL g, int n)
{
  if (Type != TYPE_INT) {
    sprintf(g->Message, MSG(ADD_BAD_TYPE), GetTypeName(Type), "INTEGER");
    return true;
  } // endif Type

  xtrc(1, " adding int(%d): %d\n", Nval, n);
  Vblp->SetValue(n, Nval++);
  return false;
} // end of AddValue

/***********************************************************************/
/*  Add a double float element to an array.                            */
/***********************************************************************/
bool ARRAY::AddValue(PGLOBAL g, double d)
{
  if (Type != TYPE_DOUBLE) {
    sprintf(g->Message, MSG(ADD_BAD_TYPE), GetTypeName(Type), "DOUBLE");
    return true;
  } // endif Type

  xtrc(1, " adding float(%d): %lf\n", Nval, d);
  Value->SetValue(d);
  Vblp->SetValue(Value, Nval++);
  return false;
} // end of AddValue

/***********************************************************************/
/*  Add the value of a XOBJECT block to an array.                      */
/***********************************************************************/
bool ARRAY::AddValue(PGLOBAL g, PXOB xp)
{
	if (Type != xp->GetResultType()) {
		sprintf(g->Message, MSG(ADD_BAD_TYPE),
			GetTypeName(xp->GetResultType()), GetTypeName(Type));
		return true;
	} // endif Type

	xtrc(1, " adding (%d) from xp=%p\n", Nval, xp);
	Vblp->SetValue(xp->GetValue(), Nval++);
	return false;
} // end of AddValue

/***********************************************************************/
/*  Add a value to an array.                                           */
/***********************************************************************/
bool ARRAY::AddValue(PGLOBAL g, PVAL vp)
{
  if (Type != vp->GetType()) {
    sprintf(g->Message, MSG(ADD_BAD_TYPE),
            GetTypeName(vp->GetType()), GetTypeName(Type));
    return true;
  } // endif Type

  xtrc(1, " adding (%d) from vp=%p\n", Nval, vp);
  Vblp->SetValue(vp, Nval++);
  return false;
} // end of AddValue

/***********************************************************************/
/*  Retrieve the nth value of the array.                               */
/***********************************************************************/
void ARRAY::GetNthValue(PVAL valp, int n)
  {
  valp->SetValue_pvblk(Vblp, n);
  } // end of GetNthValue

#if 0
/***********************************************************************/
/*  Retrieve the nth subvalue of a list array.                         */
/***********************************************************************/
bool ARRAY::GetSubValue(PGLOBAL g, PVAL valp, int *kp)
  {
  PVBLK vblp;

  if (Type != TYPE_LIST) {
    sprintf(g->Message, MSG(NO_SUB_VAL), Type);
    return true;
    } // endif Type

  vblp = ((LSTBLK*)Vblp)->Mbvk[kp[0]]->Vblk;
  valp->SetValue_pvblk(vblp, kp[1]);
  return false;
  } // end of GetSubValue
#endif // 0

/***********************************************************************/
/*  Return the nth value of an integer array.                          */
/***********************************************************************/
int ARRAY::GetIntValue(int n)
  {
  assert (Type == TYPE_INT);
  return Vblp->GetIntValue(n);
  } // end of GetIntValue

/***********************************************************************/
/*  Return the nth value of a STRING array.                            */
/***********************************************************************/
char *ARRAY::GetStringValue(int n)
  {
  assert (Type == TYPE_STRING || Type == TYPE_PCHAR);
  return Vblp->GetCharValue(n);
  } // end of GetStringValue

/***********************************************************************/
/*  Find whether a value is in an array.                               */
/*  Provide a conversion limited to the Value limitation.              */
/***********************************************************************/
bool ARRAY::Find(PVAL valp)
  {
  int n;
  PVAL     vp;

  if (Type != valp->GetType()) {
    Value->SetValue_pval(valp);
    vp = Value;
  } else
    vp = valp;

  Inf = Bot, Sup = Top;

  while (Sup - Inf > 1) {
    X = (Inf + Sup) >> 1;
    n = Vblp->CompVal(vp, X);

    if (n < 0)
      Sup = X;
    else if (n > 0)
      Inf = X;
    else
      return true;

    } // endwhile

  return false;
  } // end of Find

/***********************************************************************/
/*  ARRAY: Compare routine for a list of values.                       */
/***********************************************************************/
BYTE ARRAY::Vcompare(PVAL vp, int n)
  {
  Value->SetValue_pvblk(Vblp, n);
  return vp->TestValue(Value);
  } // end of Vcompare

/***********************************************************************/
/*  Test a filter condition on an array depending on operator and mod. */
/*  Modificator values are 1: ANY (or SOME) and 2: ALL.                */
/***********************************************************************/
bool ARRAY::FilTest(PGLOBAL g, PVAL valp, OPVAL opc, int opm)
  {
  int  i;
  PVAL vp;
  BYTE bt = OpBmp(g, opc);
  int top = Nval - 1;

  if (top < 0)              // Array is empty
    // Return true for ALL because it means that there are no item that
    // does not verify the condition, which is true indeed.
    // Return false for ANY because true means that there is at least
    // one item that verifies the condition, which is false.
    return opm == 2;

  if (valp) {
    if (Type != valp->GetType()) {
      Value->SetValue_pval(valp);
      vp = Value;
    } else
      vp = valp;

  } else if (opc != OP_EXIST) {
		sprintf(g->Message, MSG(MISSING_ARG), opc);
		throw (int)TYPE_ARRAY;
  } else    // OP_EXIST
    return Nval > 0;

  if (opc == OP_IN || (opc == OP_EQ && opm == 1))
    return Find(vp);
  else if (opc == OP_NE && opm == 2)
    return !Find(vp);
  else if (opc == OP_EQ && opm == 2)
    return (Ndif == 1) ? !(Vcompare(vp, 0) & bt) : false;
  else if (opc == OP_NE && opm == 1)
    return (Ndif == 1) ? !(Vcompare(vp, 0) & bt) : true;

  if (Type != TYPE_LIST) {
    if (opc == OP_GT || opc == OP_GE)
      return !(Vcompare(vp, (opm == 1) ? 0 : top) & bt);
    else
      return !(Vcompare(vp, (opm == 2) ? 0 : top) & bt);

    } // endif Type

  // Case of TYPE_LIST
  if (opm == 2) {
    for (i = 0; i < Nval; i++)
      if (Vcompare(vp, i) & bt)
        return false;

    return true;
  } else { // opm == 1
    for (i = 0; i < Nval; i++)
      if (!(Vcompare(vp, i) & bt))
        return true;

    return false;
  } // endif opm

  } // end of FilTest

/***********************************************************************/
/*  Test whether this array can be converted to TYPE_SHORT.            */
/*  Must be called after the array is sorted.                          */
/***********************************************************************/
bool ARRAY::CanBeShort(void)
  {
  int* To_Val = (int*)Valblk->GetMemp();

  if (Type != TYPE_INT || !Ndif)
    return false;

  // Because the array is sorted, this is true if all the array
  // int values are in the range of SHORT values
  return (To_Val[0] >= -32768 && To_Val[Nval-1] < 32768);
  } // end of CanBeShort

/***********************************************************************/
/*  Convert an array to new numeric type k.                            */
/*  Note: conversion is always made in ascending order from STRING to  */
/*  short to int to double so no precision is lost in the conversion.  */
/*  One exception is converting from int to short compatible arrays.   */
/***********************************************************************/
int ARRAY::Convert(PGLOBAL g, int k, PVAL vp)
  {
  int   i, prec = 0;
  bool  b = false;
  PMBV  ovblk = Valblk;
  PVBLK ovblp = Vblp;

  Type = k;                    // k is the new type
  Valblk = new(g) MBVALS;

  switch (Type) {
    case TYPE_DOUBLE:
      prec = 2;
      /* fall through */
    case TYPE_SHORT:
    case TYPE_INT:
    case TYPE_DATE:
      Len = 1;
      break;
    default:
      sprintf(g->Message, MSG(BAD_CONV_TYPE), Type);
      return TYPE_ERROR;
    } // endswitch k

  Size = Nval;
  Nval = 0;
  Vblp = Valblk->Allocate(g, Type, Len, prec, Size);

  if (!Valblk->GetMemp())
    // The error message was built by PlgDBalloc
    return TYPE_ERROR;
  else
    Value = AllocateValue(g, Type, Len, prec);

  /*********************************************************************/
  /*  Converting STRING to DATE can be done according to date format.  */
  /*********************************************************************/
  if (Type == TYPE_DATE && ovblp->GetType() == TYPE_STRING && vp)
  {
    if (((DTVAL*)Value)->SetFormat(g, vp))
      return TYPE_ERROR;
    else
      b = true;  // Sort the new array on date internal values
  }

  /*********************************************************************/
  /*  Do the actual conversion.                                        */
  /*********************************************************************/
  for (i = 0; i < Size; i++) {
    Value->SetValue_pvblk(ovblp, i);

    if (AddValue(g, Value))
      return TYPE_ERROR;

    } // endfor i

  /*********************************************************************/
  /*  For sorted arrays, get the initial find values.                  */
  /*********************************************************************/
  if (b)
    Sort(g);

  ovblk->Free();
  return Type;
  } // end of Convert

/***********************************************************************/
/*  ARRAY Save: save value at i (used while rordering).                */
/***********************************************************************/
void ARRAY::Save(int i)
  {
  Value->SetValue_pvblk(Vblp, i);
  } // end of Save

/***********************************************************************/
/*  ARRAY Restore: restore value to j (used while rordering).          */
/***********************************************************************/
void ARRAY::Restore(int j)
  {
  Vblp->SetValue(Value, j);
  } // end of Restore

/***********************************************************************/
/*  ARRAY Move: move value from k to j (used while rordering).         */
/***********************************************************************/
void ARRAY::Move(int j, int k)
  {
  Vblp->Move(k, j);      // VALBLK does the opposite !!!
  } // end of Move

/***********************************************************************/
/*  ARRAY: Compare routine for one LIST value (ascending only).        */
/***********************************************************************/
int ARRAY::Qcompare(int *i1, int *i2)
  {
  return Vblp->CompVal(*i1, *i2);
  } // end of Qcompare

/***********************************************************************/
/*  Mainly meant to set the character arrays case sensitiveness.       */
/***********************************************************************/
void ARRAY::SetPrecision(PGLOBAL g, int p)
  {
  if (Vblp == NULL) {
    strcpy(g->Message, MSG(PREC_VBLP_NULL));
		throw (int)TYPE_ARRAY;
    } // endif Vblp

  bool was = Vblp->IsCi();

  if (was && !p) {
    strcpy(g->Message, MSG(BAD_SET_CASE));
		throw (int)TYPE_ARRAY;
	} // endif Vblp

  if (was || !p)
    return;
  else
    Vblp->SetPrec(p);

  if (!was && Type == TYPE_STRING)
    // Must be resorted to eliminate duplicate strings
    if (Sort(g))
			throw (int)TYPE_ARRAY;

  } // end of SetPrecision

/***********************************************************************/
/*  Sort and eliminate distinct values from an array.                  */
/*  Note: this is done by making a sorted index on distinct values.    */
/*  Returns false if Ok or true in case of error.                      */
/***********************************************************************/
bool ARRAY::Sort(PGLOBAL g)
  {
  int   i, j, k;

  // This is to avoid multiply allocating for correlated subqueries
  if (Nval > Xsize) {
    if (Xsize >= 0) {
      // Was already allocated
      PlgDBfree(Index);
      PlgDBfree(Offset);
      } // endif Xsize

    // Prepare non conservative sort with offet values
    Index.Size = Nval * sizeof(int);

    if (!PlgDBalloc(g, NULL, Index))
      goto error;

    Offset.Size = (Nval + 1) * sizeof(int);

    if (!PlgDBalloc(g, NULL, Offset))
      goto error;

    Xsize = Nval;
    } // endif Nval

  // Call the sort program, it returns the number of distinct values
  Ndif = Qsort(g, Nval);

  if (Ndif < 0)
    goto error;

  // Use the sort index to reorder the data in storage so it will
  // be physically sorted and Index can be removed.
  for (i = 0; i < Nval; i++) {
    if (Pex[i] == i || Pex[i] == Nval)
      // Already placed or already moved
      continue;

    Save(i);

    for (j = i;; j = k) {
      k = Pex[j];
      Pex[j] = Nval;           // Mark position as set

      if (k == i) {
        Restore(j);
        break;                 // end of loop
      } else
        Move(j, k);

      } // endfor j

    } // endfor i

  // Reduce the size of the To_Val array if Ndif < Nval
  if (Ndif < Nval) {
    for (i = 1; i < Ndif; i++)
      if (i != Pof[i])
        break;

    for (; i < Ndif; i++)
      Move(i, Pof[i]);

    Nval = Ndif;
    } // endif ndif

//if (!Correlated) {
    if (Size > Nval) {
      Size = Nval;
      Valblk->ReAllocate(g, Size);
      } // endif Size

    // Index and Offset are not used anymore
    PlgDBfree(Index);
    PlgDBfree(Offset);
    Xsize = -1;
//  } // endif Correlated

  Bot = -1;                // For non optimized search
  Top = Ndif;              //   Find searches the whole array.
  return false;

 error:
  Nval = Ndif = 0;
  Valblk->Free();
  PlgDBfree(Index);
  PlgDBfree(Offset);
  return true;
  } // end of Sort

/***********************************************************************/
/*  Sort and return the sort index.                                    */
/*  Note: This is meant if the array contains unique values.           */
/*  Returns Index.Memp if Ok or NULL in case of error.                 */
/***********************************************************************/
void *ARRAY::GetSortIndex(PGLOBAL g)
  {
  // Prepare non conservative sort with offet values
  Index.Size = Nval * sizeof(int);

  if (!PlgDBalloc(g, NULL, Index))
    goto error;

  Offset.Size = (Nval + 1) * sizeof(int);

  if (!PlgDBalloc(g, NULL, Offset))
    goto error;

  // Call the sort program, it returns the number of distinct values
  Ndif = Qsort(g, Nval);

  if (Ndif < 0)
    goto error;

  if (Ndif < Nval)
    goto error;

  PlgDBfree(Offset);
  return Index.Memp;

 error:
  Nval = Ndif = 0;
  Valblk->Free();
  PlgDBfree(Index);
  PlgDBfree(Offset);
  return NULL;
  } // end of GetSortIndex

/***********************************************************************/
/*  Block filter testing for IN operator on Column/Array operands.     */
/*  Here we call Find that returns true if the value is in the array   */
/*  with X equal to the index of the found value in the array, or      */
/*  false if the value is not in the array with Inf and Sup being the  */
/*  indexes of the array values that are immediately below and over    */
/*  the not found value. This enables to restrict the array to the     */
/*  values that are between the min and max block values and to return */
/*  the indication of whether the Find will be always true, always not */
/*  true or other.                                                     */
/***********************************************************************/
int ARRAY::BlockTest(PGLOBAL, int opc, int opm,
                     void *minp, void *maxp, bool s)
  {
  bool bin, bax, pin, pax, veq, all = (opm == 2);

  if (Ndif == 0)              // Array is empty
    // Return true for ALL because it means that there are no item that
    // does not verify the condition, which is true indeed.
    // Return false for ANY because true means that there is at least
    // one item that verifies the condition, which is false.
    return (all) ? 2 : -2;
  else if (opc == OP_EQ && all && Ndif > 1)
    return -2;
  else if (opc == OP_NE && !all && Ndif > 1)
    return 2;
//  else if (Ndif == 1)
//    all = false;

  // veq is true when all values in the block are equal
  switch (Type) {
    case TYPE_STRING: veq = (Vblp->IsCi())
                      ? !stricmp((char*)minp, (char*)maxp)
                      : !strcmp((char*)minp, (char*)maxp);     break;
    case TYPE_SHORT:  veq = *(short*)minp == *(short*)maxp;    break;
    case TYPE_INT:    veq = *(int*)minp == *(int*)maxp;        break;
    case TYPE_DOUBLE: veq = *(double*)minp == *(double*)maxp;  break;
    default: veq = false;   // Error ?
    } // endswitch type

  if (!s)
    Bot = -1;

  Top = Ndif;        // Reset Top at top of list
  Value->SetBinValue(maxp);
  Top = (bax = Find(Value)) ? X + 1 : Sup;

  if (bax) {
    if (opc == OP_EQ)
      return (veq) ? 1 : 0;
    else if (opc == OP_NE)
      return (veq) ? -1 : 0;

    if (X == 0) switch (opc) {
      // Max value is equal to min list value
      case OP_LE: return  1;             break;
      case OP_LT: return (veq) ? -1 : 0; break;
      case OP_GE: return (veq) ?  1 : 0; break;
      case OP_GT: return -1;             break;
      } // endswitch opc

    pax = (opc == OP_GE) ? (X < Ndif - 1) : true;
  } else if (Inf == Bot) {
    // Max value is smaller than min list value
    return (opc == OP_LT || opc == OP_LE || opc == OP_NE) ? 1 : -1;
  } else
    pax = (Sup < Ndif);  // True if max value is inside the list value

  if (!veq) {
    Value->SetBinValue(minp);
    bin = Find(Value);
  } else
    bin = bax;

  Bot = (bin) ? X - 1 : Inf;

  if (bin) {
    if (opc == OP_EQ || opc == OP_NE)
      return 0;

    if (X == Ndif - 1) switch (opc) {
      case OP_GE: return (s) ?  2 : 1;   break;
      case OP_GT: return (veq) ? -1 : 0; break;
      case OP_LE: return (veq) ?  1 : 0; break;
      case OP_LT: return (s) ? -2 : -1;  break;
      } // endswitch opc

    pin = (opc == OP_LE) ? (X > 0) : true;
  } else if (Sup == Ndif) {
    // Min value is greater than max list value
    if (opc == OP_GT || opc == OP_GE || opc == OP_NE)
      return (s) ? 2 : 1;
    else
      return (s) ? -2 : -1;

  } else
    pin = (Inf >= 0);    // True if min value is inside the list value

  if (Top - Bot <= 1) {
    // No list item between min and max value
#if defined(_DEBUG)
    assert (!bin && !bax);
#endif
    switch (opc) {
      case OP_EQ: return -1;          break;
      case OP_NE: return  1;          break;
      default: return (all) ? -1 : 1; break;
      } // endswitch opc

    } // endif

#if defined(_DEBUG)
  assert (Ndif > 1);    // if Ndif = 1 we should have returned already
#endif

  // At this point, if there are no logical errors in the algorithm,
  // the only possible overlaps between the array and the block are:
  // Array:    +-------+      +-------+       +-------+      +-----+
  // Block:  +-----+            +---+            +------+   +--------+
  // true:        pax          pin pax          pin
  if (all) switch (opc) {
    case OP_GT:
    case OP_GE: return (pax) ? -1 : 0; break;
    case OP_LT:
    case OP_LE: return (pin) ? -1 : 0; break;
    } // endswitch opc

  return 0;
  } // end of BlockTest

/***********************************************************************/
/*  MakeArrayList: Makes a value list from an SQL IN array (in work).  */
/***********************************************************************/
PSZ ARRAY::MakeArrayList(PGLOBAL g)
{
  char   *p, *tp;
  int     i;
  size_t  z, len = 2;

  if (Type == TYPE_LIST)
    return (PSZ)("(?" "?" "?)");             // To be implemented

  z = MY_MAX(24, GetTypeSize(Type, Len) + 4);
  tp = (char*)PlugSubAlloc(g, NULL, z);

  for (i = 0; i < Nval; i++) {
    Value->SetValue_pvblk(Vblp, i);
    Value->Prints(g, tp, z);
    len += strlen(tp);
  } // enfor i

  xtrc(1, "Arraylist: len=%d\n", len);
  p = (char *)PlugSubAlloc(g, NULL, len);
  strcpy(p, "(");

  for (i = 0; i < Nval;) {
    Value->SetValue_pvblk(Vblp, i);
    Value->Prints(g, tp, z);
    strcat(p, tp);
    strcat(p, (++i == Nval) ? ")" : ",");
  } // enfor i

  xtrc(1, "Arraylist: newlen=%d\n", strlen(p));
  return p;
} // end of MakeArrayList

/***********************************************************************/
/*  Make file output of ARRAY  contents.                               */
/***********************************************************************/
void ARRAY::Printf(PGLOBAL g, FILE *f, uint n)
{
  char m[64];
  int  lim = MY_MIN(Nval,10);

  memset(m, ' ', n);     // Make margin string
  m[n] = '\0';
  fprintf(f, "%sARRAY: type=%d\n", m, Type);
  memset(m, ' ', n + 2);     // Make margin string
  m[n] = '\0';

  if (Type != TYPE_LIST) {
    fprintf(f, "%sblock=%p numval=%d\n", m, Valblk->GetMemp(), Nval);

    if (Vblp)
      for (int i = 0; i < lim; i++) {
        Value->SetValue_pvblk(Vblp, i);
        Value->Printf(g, f, n+4);
        } // endfor i

  } else
    fprintf(f, "%sVALLST: numval=%d\n", m, Nval);

} // end of Printf

/***********************************************************************/
/*  Make string output of ARRAY  contents.                             */
/***********************************************************************/
void ARRAY::Prints(PGLOBAL, char *ps, uint z)
{
  if (z < 16)
    return;

  sprintf(ps, "ARRAY: type=%d\n", Type);
  // More to be implemented later
} // end of Prints

/* -------------------------- Class MULAR ---------------------------- */

/***********************************************************************/
/*  MULAR public constructor.                                          */
/***********************************************************************/
MULAR::MULAR(PGLOBAL g, int n) : CSORT(false)
  {
  Narray = n;
  Pars = (PARRAY*)PlugSubAlloc(g, NULL, n * sizeof(PARRAY));
  } // end of MULAR constructor

/***********************************************************************/
/*  MULAR: Compare routine multiple arrays.                            */
/***********************************************************************/
int MULAR::Qcompare(int *i1, int *i2)
  {
  int i, n = 0;

  for (i = 0; i < Narray; i++)
    if ((n = Pars[i]->Qcompare(i1, i2)))
      break;

  return n;
  } // end of Qcompare

/***********************************************************************/
/*  Sort and eliminate distinct values from multiple arrays.           */
/*  Note: this is done by making a sorted index on distinct values.    */
/*  Returns false if Ok or true in case of error.                      */
/***********************************************************************/
bool MULAR::Sort(PGLOBAL g)
  {
  int i, j, k, n, nval, ndif;

  // All arrays must have the same number of values
  nval = Pars[0]->Nval;

  for (n = 1; n < Narray; n++)
    if (Pars[n]->Nval != nval) {
      strcpy(g->Message, MSG(BAD_ARRAY_VAL));
      return true;
      } // endif nval

  // Prepare non conservative sort with offet values
  Index.Size = nval * sizeof(int);

  if (!PlgDBalloc(g, NULL, Index))
    goto error;

  Offset.Size = (nval + 1) * sizeof(int);

  if (!PlgDBalloc(g, NULL, Offset))
    goto error;

  // Call the sort program, it returns the number of distinct values
  ndif = Qsort(g, nval);

  if (ndif < 0)
    goto error;

  // Use the sort index to reorder the data in storage so it will
  // be physically sorted and Index can be removed.
  for (i = 0; i < nval; i++) {
    if (Pex[i] == i || Pex[i] == nval)
      // Already placed or already moved
      continue;

    for (n = 0; n < Narray; n++)
      Pars[n]->Save(i);

    for (j = i;; j = k) {
      k = Pex[j];
      Pex[j] = nval;           // Mark position as set

      if (k == i) {
        for (n = 0; n < Narray; n++)
          Pars[n]->Restore(j);

        break;                 // end of loop
      } else
        for (n = 0; n < Narray; n++)
          Pars[n]->Move(j, k);

      } // endfor j

    } // endfor i

  // Reduce the size of the To_Val array if ndif < nval
  if (ndif < nval) {
    for (i = 1; i < ndif; i++)
      if (i != Pof[i])
        break;

    for (; i < ndif; i++)
      for (n = 0; n < Narray; n++)
        Pars[n]->Move(i, Pof[i]);

    for (n = 0; n < Narray; n++) {
      Pars[n]->Nval = ndif;
      Pars[n]->Size = ndif;
      Pars[n]->Valblk->ReAllocate(g, ndif);
      } // endfor n

    } // endif ndif

  // Index and Offset are not used anymore
  PlgDBfree(Index);
  PlgDBfree(Offset);

  for (n = 0; n < Narray; n++) {
    Pars[n]->Bot = -1;        // For non optimized search
    Pars[n]->Top = ndif;      //   Find searches the whole array.
    } // endfor n

  return false;

 error:
  PlgDBfree(Index);
  PlgDBfree(Offset);
  return true;
  } // end of Sort
