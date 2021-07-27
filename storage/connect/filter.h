/*************** Filter H Declares Source Code File (.H) ***************/
/*  Name: FILTER.H    Version 1.3                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2010-2017    */
/*                                                                     */
/*  This file contains the FILTER and derived classes declares.        */
/***********************************************************************/
#ifndef __FILTER__
#define __FILTER__

/***********************************************************************/
/*  Include required application header files                          */
/***********************************************************************/
#include "xobject.h"

/***********************************************************************/
/*  Utilities for WHERE condition building.                            */
/***********************************************************************/
PFIL MakeFilter(PGLOBAL g, PFIL filp, OPVAL vop, PFIL fp);
PFIL MakeFilter(PGLOBAL g, PCOL *colp, POPER pop, PPARM pfirst, bool neg);

/***********************************************************************/
/*  Definition of class FILTER with all its method functions.          */
/*  Note: Most virtual implementation functions are not in use yet     */
/*  but could be in future system evolution.                           */
/***********************************************************************/
class DllExport FILTER : public XOBJECT { /* Filter description block  */
//friend PFIL PrepareFilter(PGLOBAL, PFIL, bool);
  friend DllExport bool ApplyFilter(PGLOBAL, PFIL);
 public:
  // Constructors
  FILTER(PGLOBAL g, POPER pop, PPARM *tp = NULL);
  FILTER(PGLOBAL g, OPVAL opc, PPARM *tp = NULL);
  FILTER(PFIL fil1);

  // Implementation
  virtual int    GetType(void) {return TYPE_FILTER;}
  virtual int    GetResultType(void) {return TYPE_INT;}
  virtual int    GetLength(void) {return 1;}
  virtual int    GetLengthEx(void) {assert(FALSE); return 0;}
  virtual int    GetScale() {return 0;};
          PFIL   GetNext(void) {return Next;}
          OPVAL  GetOpc(void) {return Opc;}
          int    GetOpm(void) {return Opm;}
          int    GetArgType(int i) {return Arg(i)->GetType();}
          bool   GetResult(void) {return Value->GetIntValue() != 0;}
          PXOB  &Arg(int i) {return Test[i].Arg;}
          PVAL  &Val(int i) {return Test[i].Value;}
          bool  &Conv(int i) {return Test[i].Conv;}
          void   SetNext(PFIL filp) {Next = filp;}

  // Methods
  virtual void   Reset(void);
  virtual bool   Compare(PXOB) {return FALSE;}          // Not used yet
  virtual bool   Init(PGLOBAL);
  virtual bool   Eval(PGLOBAL);
  virtual bool   SetFormat(PGLOBAL, FORMAT&) {return TRUE;}      // NUY
//virtual int    CheckColumn(PGLOBAL g, PSQL sqlp, PXOB &xp, int &ag);
//virtual int    RefNum(PSQL);
//virtual PXOB   SetSelect(PGLOBAL, PSQL, bool) {return NULL;}   // NUY
//virtual PXOB   CheckSubQuery(PGLOBAL, PSQL);
//virtual bool   CheckLocal(PTDB);
//virtual int    CheckSpcCol(PTDB tdbp, int n);
	virtual void   Printf(PGLOBAL g, FILE *f, uint n);
  virtual void   Prints(PGLOBAL g, char *ps, uint z);
//        PFIL   Linearize(bool nosep);
//        PFIL   Link(PGLOBAL g, PFIL fil2);
//        PFIL   RemoveLastSep(void);
//        PFIL   SortJoin(PGLOBAL g);
//        bool   FindJoinFilter(POPJOIN opj, PFIL fprec, bool teq,
//               bool tek, bool tk2, bool tc2, bool tix, bool thx);
//        bool   CheckHaving(PGLOBAL g, PSQL sqlp);
          bool   Convert(PGLOBAL g, bool having);
//        int    SplitFilter(PFIL *fp);
//        int    SplitFilter(PFIL *fp, PTDB tp, int n);
//        PFIL   LinkFilter(PGLOBAL g, PFIL fp2);
//        PFIL   Copy(PTABS t);

 protected:
  FILTER(void) {}       // Standard constructor not to be used
  void Constr(PGLOBAL g, OPVAL opc, int opm, PPARM *tp);

  // Members
  PFIL  Next;           // Used for linearization
  OPVAL Opc;            // Comparison operator
  int   Opm;            // Modificator
  BYTE  Bt;             // Operator bitmap
  struct {
    int   B_T;          // Buffer type
    PXOB  Arg;          // Points to argument
    PVAL  Value;        // Points to argument value
    bool  Conv;         // TRUE if argument must be converted
    } Test[2];
  }; // end of class FILTER

/***********************************************************************/
/*  Derived class FILTERX: used to replace a filter by a derived class */
/*  using an Eval method optimizing the filtering evaluation.          */
/*  Note: this works only if the members of the derived class are the  */
/*  same than the ones of the original class (NO added members).       */
/***********************************************************************/
class FILTERX : public FILTER {
 public:
  // Methods
  virtual bool Eval(PGLOBAL) = 0; // just to prevent direct FILTERX use

  // Fake operator new used to change a filter into a derived filter
  void * operator new(size_t, PFIL filp) {return filp;}
#if defined(_WIN32)
  // Avoid warning C4291 by defining a matching dummy delete operator
  void operator delete(void *, PFIL) {}
#else
  void operator delete(void *) {}
#endif
  }; // end of class FILTERX

/***********************************************************************/
/*  Derived class FILTEREQ: OP_EQ, no conversion and Xobject args.     */
/***********************************************************************/
class FILTERCMP : public FILTERX {
 public:
  // Constructor
  FILTERCMP(PGLOBAL g);

  // Methods
  virtual bool Eval(PGLOBAL);
  }; // end of class FILTEREQ

/***********************************************************************/
/*  Derived class FILTERAND: OP_AND, no conversion and Xobject args.   */
/***********************************************************************/
class FILTERAND : public FILTERX {
 public:
  // Methods
  virtual bool Eval(PGLOBAL);
  }; // end of class FILTERAND

/***********************************************************************/
/*  Derived class FILTEROR: OP_OR, no conversion and Xobject args.     */
/***********************************************************************/
class FILTEROR : public FILTERX {
 public:
  // Methods
  virtual bool Eval(PGLOBAL);
  }; // end of class FILTEROR

/***********************************************************************/
/*  Derived class FILTERNOT: OP_NOT, no conversion and Xobject args.   */
/***********************************************************************/
class FILTERNOT : public FILTERX {
 public:
  // Methods
  virtual bool Eval(PGLOBAL);
  }; // end of class FILTERNOT

/***********************************************************************/
/*  Derived class FILTERIN: OP_IN, no conversion and Array 2nd arg.    */
/***********************************************************************/
class FILTERIN : public FILTERX {
 public:
  // Methods
  virtual bool Eval(PGLOBAL);
  }; // end of class FILTERIN

/***********************************************************************/
/*  Derived class FILTERTRUE: Always returns TRUE.                     */
/***********************************************************************/
class FILTERTRUE : public FILTERX {
 public:
  // Constructor
  FILTERTRUE(PVAL valp) {Value = valp; Value->SetValue_bool(TRUE);}

  // Methods
  virtual void Reset(void);
  virtual bool Eval(PGLOBAL);
  }; // end of class FILTERTRUE

#endif // __FILTER__
