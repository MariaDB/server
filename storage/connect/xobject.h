/*************** Xobject H Declares Source Code File (.H) **************/
/*  Name: XOBJECT.H    Version 2.4                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2014    */
/*                                                                     */
/*  This file contains the XOBJECT and derived classes declares.       */
/***********************************************************************/

#ifndef __XOBJECT__H
#define __XOBJECT__H

/***********************************************************************/
/*  Include required application header files                          */
/*  block.h      is header containing Block    global declarations.    */
/***********************************************************************/
#include "block.h"
#include "valblk.h"             // includes value.h

/***********************************************************************/
/*  Types used in some class definitions.                              */
/***********************************************************************/
typedef class STRING *PSTRG;

/***********************************************************************/
/*  The pointer to the one and only needed void object.                */
/***********************************************************************/
extern PXOB const pXVOID;

/***********************************************************************/
/*  Class XOBJECT is the base class for all classes that can be used   */
/*  in evaluation operations: FILTER, EXPRESSION, SCALF, FNC, COLBLK,  */
/*  SELECT, FILTER as well as all the constant object types.           */
/***********************************************************************/
class DllExport XOBJECT : public BLOCK {
 public:
  XOBJECT(void) {Value = NULL; Constant = false;}

  // Implementation
          PVAL   GetValue(void) {return Value;}
          bool   IsConstant(void) {return Constant;}
  virtual int    GetType(void) {return TYPE_XOBJECT;}
  virtual int    GetResultType(void) {return TYPE_VOID;}
  virtual int    GetKey(void) {return 0;}
#if defined(_DEBUG)
  virtual void   SetKey(int) {assert(false);}
#else    // !_DEBUG
  virtual void   SetKey(int) {}     // Only defined for COLBLK
#endif  // !_DEBUG
  virtual int    GetLength(void) = 0;
  virtual int    GetLengthEx(void) = 0;
  virtual PSZ    GetCharValue(void);
  virtual short  GetShortValue(void);
  virtual int    GetIntValue(void);
  virtual double GetFloatValue(void);
  virtual int    GetScale(void) = 0;

  // Methods
  virtual void   Reset(void) {}
  virtual bool   Compare(PXOB) = 0;
  virtual bool   Init(PGLOBAL) {return false;}
  virtual bool   Eval(PGLOBAL) {return false;}
  virtual bool   SetFormat(PGLOBAL, FORMAT&) = 0;

 protected:
  PVAL Value;    // The current value of the object.
  bool Constant; // true for an object having a constant value.
  }; // end of class XOBJECT

/***********************************************************************/
/*  Class XVOID: represent a void (null) object.                       */
/*  Used to represent a void parameter for count(*) or for a filter.   */
/***********************************************************************/
class DllExport XVOID : public XOBJECT {
 public:
  XVOID(void) {Constant = true;}

  // Implementation
  virtual int    GetType(void) {return TYPE_VOID;}
  virtual int    GetLength(void) {return 0;}
  virtual int    GetLengthEx(void) {return 0;}
  virtual PSZ    GetCharValue(void) {return NULL;}
  virtual int    GetIntValue(void) {return 0;}
  virtual double GetFloatValue(void) {return 0.0;}
  virtual int    GetScale() {return 0;}

  // Methods
  virtual bool   Compare(PXOB xp) {return xp->GetType() == TYPE_VOID;}
  virtual bool   SetFormat(PGLOBAL, FORMAT&) {return true;}
  }; // end of class XVOID


/***********************************************************************/
/*  Class CONSTANT: represents a constant XOBJECT of any value type.   */
/*  Note that the CONSTANT class is a friend of the VALUE class;       */
/***********************************************************************/
class DllExport CONSTANT : public XOBJECT {
 public:
  CONSTANT(PGLOBAL g, void *value, short type);
  CONSTANT(PGLOBAL g, int n);
  CONSTANT(PVAL valp) {Value = valp; Constant = true;}

  // Implementation
  virtual int    GetType(void) {return TYPE_CONST;}
  virtual int    GetResultType(void) {return Value->Type;}
  virtual int    GetLength(void) {return Value->GetValLen();}
  virtual int    GetScale() {return Value->GetValPrec();}
  virtual int    GetLengthEx(void);

  // Methods
  virtual bool   Compare(PXOB xp);
  virtual bool   SetFormat(PGLOBAL g, FORMAT& fmt)
                 {return Value->SetConstFormat(g, fmt);}
          void   Convert(PGLOBAL g, int newtype);
          void   SetValue(PVAL vp) {Value = vp;}
  virtual void   Printf(PGLOBAL g, FILE *, uint);
  virtual void   Prints(PGLOBAL g, char *, uint);
  }; // end of class CONSTANT

/***********************************************************************/
/*  Class STRING handles variable length char strings.                 */
/*  It is mainly used to avoid buffer overrun.                         */
/***********************************************************************/
class DllExport STRING : public BLOCK {
 public:
  // Constructor
  STRING(PGLOBAL g, uint n, PCSZ str = NULL);

  // Implementation
  inline int    GetLength(void) {return (int)Length;}
	inline void   SetLength(uint n) {Length = n;}
	inline PSZ    GetStr(void) {return Strp;}
  inline uint32 GetSize(void) {return Size;}
	inline char   GetLastChar(void) {return Length ? Strp[Length - 1] : 0;}
	inline bool   IsTruncated(void) {return Trc;}

  // Methods
  inline void   Reset(void) {*Strp = 0;}
         bool   Set(PCSZ s);
         bool   Set(char *s, uint n);
         bool   Append(const char *s, uint ln, bool nq = false);
         bool   Append(PCSZ s);
         bool   Append(STRING &str);
         bool   Append(char c);
         bool   Resize(uint n);
         bool   Append_quoted(PCSZ s);
  inline void   Trim(void) {(void)Resize(Length + 1);}
  inline void   Chop(void) {if (Length) Strp[--Length] = 0;}
  inline void   RepLast(char c) {if (Length) Strp[Length-1] = c;}
  inline void   Truncate(uint n) {if (n < Length) {Strp[n] = 0; Length = n;}}

 protected:
         char  *Realloc(uint len);
  inline char  *GetNext(void)
         {return ((char*)G->Sarea)+((PPOOLHEADER)G->Sarea)->To_Free;}

  // Members
  PGLOBAL G;                   // To avoid parameter
  PSZ     Strp;                // The char string
  uint    Length;              // String length
  uint    Size;                // Allocated size
	bool    Trc;								 // When truncated
  char   *Next;                // Next alloc position
  }; // end of class STRING

#endif
