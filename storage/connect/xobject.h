/*************** Xobject H Declares Source Code File (.H) **************/
/*  Name: XOBJECT.H    Version 2.3                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2012    */
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
//typedef struct _tabdesc  *PTABD;        // For friend setting

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
  virtual void   SetKey(int k) {assert(false);}
#else    // !_DEBUG
  virtual void   SetKey(int k) {}   // Only defined for COLBLK
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
          void   SetValue(PVAL vp) {Value = vp;}
  virtual void   Print(PGLOBAL g, FILE *, uint);
  virtual void   Print(PGLOBAL g, char *, uint);
  }; // end of class CONSTANT

#endif
