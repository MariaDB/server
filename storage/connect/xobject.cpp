/************ Xobject C++ Functions Source Code File (.CPP) ************/
/*  Name: XOBJECT.CPP  Version 2.4                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2014    */
/*                                                                     */
/*  This file contains base XOBJECT class functions.                   */
/*  Also here is the implementation of the CONSTANT class.             */
/***********************************************************************/

/***********************************************************************/
/*  Include mariaDB header file.                                       */
/***********************************************************************/
#include "my_global.h"
#include "m_string.h"

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
/*  Convert a constant to the given type.                              */
/***********************************************************************/
void CONSTANT::Convert(PGLOBAL g, int newtype)
  {
  if (Value->GetType() != newtype)
    if (!(Value = AllocateValue(g, Value, newtype)))
      longjmp(g->jumper[g->jump_level], TYPE_CONST);

  } // end of Convert

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

/* -------------------------- Class STRING --------------------------- */

/***********************************************************************/
/*  STRING public constructor for new char values. Alloc Size must be  */
/*  calculated because PlugSubAlloc rounds up size to multiple of 8.   */
/***********************************************************************/
STRING::STRING(PGLOBAL g, uint n, char *str)
{
  G = g;
  Length = (str) ? strlen(str) : 0;

  if ((Strp = (PSZ)PlgDBSubAlloc(g, NULL, MY_MAX(n, Length) + 1))) {
    if (str)
      strcpy(Strp, str);
    else
      *Strp = 0;

    Next = GetNext();
    Size = Next - Strp;
  } else {
    // This should normally never happen
    Next = NULL;
    Size = 0;
  } // endif Strp

} // end of STRING constructor

/***********************************************************************/
/*  Reallocate the string memory and return the (new) position.        */
/*  If Next is equal to GetNext() this means that no new suballocation */
/*  has been done. Then we can just increase the size of the current   */
/*  allocation and the Strp will remain pointing to the same memory.   */
/***********************************************************************/
char *STRING::Realloc(uint len)
{
  char *p;
  bool  b = (Next == GetNext());
  
  p = (char*)PlgDBSubAlloc(G, NULL, b ? len - Size : len);

  if (!p) {
    // No more room in Sarea; this is very unlikely
    strcpy(G->Message, "No more room in work area");
    return NULL;
    } // endif p

  if (b)
    p = Strp;

  Next = GetNext();
  Size = Next - p;
  return p;
} // end of Realloc

/***********************************************************************/
/*  Set a STRING new PSZ value.                                        */
/***********************************************************************/
bool STRING::Set(PSZ s)
{
  if (!s)
    return false;

  uint len = strlen(s) + 1;

  if (len > Size) {
    char *p = Realloc(len);
    
    if (!p)
      return true;
    else
      Strp = p;

    } // endif n

 	strcpy(Strp, s);
  Length = len - 1;
  return false;
} // end of Set

/***********************************************************************/
/*  Set a STRING new PSZ value.                                        */
/***********************************************************************/
bool STRING::Set(char *s, uint n)
{
  if (!s)
    return false;

  uint len = strnlen(s, n) + 1;

  if (len > Size) {
    char *p = Realloc(len);
    
    if (!p)
      return true;
    else
      Strp = p;

    } // endif n

 	strncpy(Strp, s, n);
  Length = len - 1;
  return false;
} // end of Set

/***********************************************************************/
/*  Append a char* to a STRING.                                        */
/***********************************************************************/
bool STRING::Append(const char *s, uint ln, bool nq)
{
  if (!s)
    return false;

  uint i, len = Length + ln + 1;

  if (len > Size) {
    char *p = Realloc(len);
    
    if (!p)
      return true;
    else if (p != Strp) {
      strcpy(p, Strp);
      Strp = p;
      } // endif p

    } // endif n

	if (nq) {
		for (i = 0; i < ln; i++)
			switch (s[i]) {
			case '\\':   Strp[Length++] = '\\'; Strp[Length++] = '\\'; break;
			case '\0':   Strp[Length++] = '\\'; Strp[Length++] = '0';  break;
			case '\'':   Strp[Length++] = '\\'; Strp[Length++] = '\''; break;
			case '\n':   Strp[Length++] = '\\'; Strp[Length++] = 'n';  break;
			case '\r':   Strp[Length++] = '\\'; Strp[Length++] = 'r';  break;
			case '\032': Strp[Length++] = '\\'; Strp[Length++] = 'Z';  break;
			default:     Strp[Length++] = s[i];
			}	// endswitch s[i]

	} else
		for (i = 0; i < ln && s[i]; i++)
			Strp[Length++] = s[i];

  Strp[Length] = 0;
  return false;
} // end of Append

/***********************************************************************/
/*  Append a PSZ to a STRING.                                          */
/***********************************************************************/
bool STRING::Append(PSZ s)
{
  if (!s)
    return false;

  uint len = Length + strlen(s) + 1;

  if (len > Size) {
    char *p = Realloc(len);
    
    if (!p)
      return true;
    else if (p != Strp) {
      strcpy(p, Strp);
      Strp = p;
      } // endif p

    } // endif n

  strcpy(Strp + Length, s);
  Length = len - 1;
  return false;
} // end of Append

/***********************************************************************/
/*  Append a STRING to a STRING.                                       */
/***********************************************************************/
bool STRING::Append(STRING &str)
{
  return Append(str.GetStr());
} // end of Append

/***********************************************************************/
/*  Append a char to a STRING.                                         */
/***********************************************************************/
bool STRING::Append(char c)
{
  if (Length + 2 > Size) {
    char *p = Realloc(Length + 2);
    
    if (!p)
      return true;
    else if (p != Strp) {
      strcpy(p, Strp);
      Strp = p;
      } // endif p

    } // endif n

  Strp[Length++] = c;
  Strp[Length] = 0;
  return false;
} // end of Append

/***********************************************************************/
/*  Append a quoted PSZ to a STRING.                                   */
/***********************************************************************/
bool STRING::Append_quoted(PSZ s)
{
  bool b = Append('\'');

  if (s) for (char *p = s; !b && *p; p++)
    switch (*p) {
      case '\'':
      case '\\':
      case '\t':
      case '\n':
      case '\r':
      case '\b':
      case '\f': b |= Append('\\');
        // passthru
      default:
        b |= Append(*p);
        break;
      } // endswitch *p

  return (b |= Append('\''));
} // end of Append_quoted

/***********************************************************************/
/*  Resize to given length but only when last suballocated.            */
/*  New size should be greater than string length.                     */
/***********************************************************************/
bool STRING::Resize(uint newsize)
{
  if (Next == GetNext() && newsize > Length) {
    uint        nsz = (((signed)newsize + 7) / 8) * 8;
    int         diff = (signed)Size - (signed)nsz;
    PPOOLHEADER pp = (PPOOLHEADER)G->Sarea;

    if ((signed)pp->FreeBlk + diff < 0)
      return true;      // Out of memory

    pp->To_Free -= diff;
    pp->FreeBlk += diff;
    Size = nsz;
    return false;
  } else
    return newsize > Size;

} // end of Resize

