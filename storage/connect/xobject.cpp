/************ Xobject C++ Functions Source Code File (.CPP) ************/
/*  Name: XOBJECT.CPP  Version 2.2                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2012    */
/*                                                                     */
/*  This file contains base XOBJECT class functions.                   */
/*  Also here is the implementation of the CONSTANT class.             */
/***********************************************************************/

/***********************************************************************/
/*  Include mariaDB header file.                                       */
/***********************************************************************/
#include "my_global.h"

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xobject.h"

/***********************************************************************/
/*  Macro definitions.                                                 */
/***********************************************************************/
#if defined(_DEBUG) || defined(DEBTRACE)
#define ASSERT(B)      assert(B);
#else
#define ASSERT(B)
#endif

/***********************************************************************/
/*  The one and only needed void object.                               */
/***********************************************************************/
XVOID Xvoid;
PXOB const pXVOID = &Xvoid;       // Pointer used by other classes

/* ------------------------- Class XOBJECT --------------------------- */

/***********************************************************************/
/*  GetCharValue: returns the Result value as a char string.           */
/*  Using GetCharValue provides no conversion from numeric types.      */
/***********************************************************************/
PSZ XOBJECT::GetCharValue(void)
  {
  ASSERT(Value)
  return Value->GetCharValue();
  } // end of GetCharValue()

/***********************************************************************/
/*  GetShortValue: returns the Result value as a short integer.        */
/***********************************************************************/
short XOBJECT::GetShortValue(void)
  {
  ASSERT(Value)
  return Value->GetShortValue();
  } // end of GetShortValue

/***********************************************************************/
/*  GetIntValue: returns the Result value as a int integer.            */
/***********************************************************************/
int XOBJECT::GetIntValue(void)
  {
  ASSERT(Value)
  return Value->GetIntValue();
  } // end of GetIntValue

/***********************************************************************/
/*  GetFloatValue: returns the Result value as a double float.         */
/***********************************************************************/
double XOBJECT::GetFloatValue(void)
  {
  ASSERT(Value)
  return Value->GetFloatValue();
  } // end of GetFloatValue

/* ------------------------- Class CONSTANT -------------------------- */

/***********************************************************************/
/*  CONSTANT public constructor.                                       */
/***********************************************************************/
CONSTANT::CONSTANT(PGLOBAL g, void *value, short type)
  {
  if (!(Value = AllocateValue(g, value, (int)type)))
    longjmp(g->jumper[g->jump_level], TYPE_CONST);

  Constant = true;
  } // end of CONSTANT constructor

/***********************************************************************/
/*  CONSTANT public constructor.                                       */
/***********************************************************************/
CONSTANT::CONSTANT(PGLOBAL g, int n)
  {
  if (!(Value = AllocateValue(g, &n, TYPE_INT)))
    longjmp(g->jumper[g->jump_level], TYPE_CONST);

  Constant = true;
  } // end of CONSTANT constructor

/***********************************************************************/
/*  GetLengthEx: returns an evaluation of the constant string length.  */
/*  Note: When converting from token to string, length has to be       */
/*    specified but we need the domain length, not the value length.   */
/***********************************************************************/
int CONSTANT::GetLengthEx(void)
  {
  return Value->GetValLen();
  } // end of GetLengthEx

/***********************************************************************/
/*  Compare: returns true if this object is equivalent to xp.          */
/***********************************************************************/
bool CONSTANT::Compare(PXOB xp)
  {
  if (this == xp)
    return true;
  else if (xp->GetType() != TYPE_CONST)
    return false;
  else
    return Value->IsEqual(xp->GetValue(), true);

  } // end of Compare

#if 0
/***********************************************************************/
/*  Rephrase: temporary implementation used by PlugRephraseSQL.        */
/***********************************************************************/
bool CONSTANT::Rephrase(PGLOBAL g, PSZ work)
  {
  switch (Value->GetType()) {
    case TYPE_STRING:
      sprintf(work + strlen(work), "'%s'", Value->GetCharValue());
      break;
    case TYPE_SHORT:
      sprintf(work + strlen(work), "%hd", Value->GetShortValue());
      break;
    case TYPE_INT:
    case TYPE_DATE:
      sprintf(work + strlen(work), "%d", Value->GetIntValue());
      break;
    case TYPE_DOUBLE:
      sprintf(work + strlen(work), "%lf", Value->GetFloatValue());
      break;
    case TYPE_BIGINT:
      sprintf(work + strlen(work), "%lld", Value->GetBigintValue());
      break;
    case TYPE_TINY:
      sprintf(work + strlen(work), "%d", Value->GetTinyValue());
      break;
    default:
      sprintf(g->Message, MSG(BAD_CONST_TYPE), Value->GetType());
      return false;
    } // endswitch

  return false;
  } // end of Rephrase
#endif // 0

/***********************************************************************/
/*  Make file output of a constant object.                             */
/***********************************************************************/
void CONSTANT::Print(PGLOBAL g, FILE *f, uint n)
  {
  Value->Print(g, f, n);
  } /* end of Print */

/***********************************************************************/
/*  Make string output of a constant object.                           */
/***********************************************************************/
void CONSTANT::Print(PGLOBAL g, char *ps, uint z)
  {
  Value->Print(g, ps, z);
  } /* end of Print */
