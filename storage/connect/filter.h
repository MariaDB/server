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
  int    GetType(void) override {return TYPE_FILTER;}
  int    GetResultType(void) override {return TYPE_INT;}
  int    GetLength(void) override {return 1;}
  int    GetLengthEx(void) override {assert(FALSE); return 0;}
  int    GetScale() override {return 0;};
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
  void   Reset(void) override;
  bool   Compare(PXOB) override {return FALSE;}          // Not used yet
  bool   Init(PGLOBAL) override;
  bool   Eval(PGLOBAL) override;
  bool   SetFormat(PGLOBAL, FORMAT&) override {return TRUE;}      // NUY
//virtual int    CheckColumn(PGLOBAL g, PSQL sqlp, PXOB &xp, int &ag);
//virtual int    RefNum(PSQL);
//virtual PXOB   SetSelect(PGLOBAL, PSQL, bool) {return NULL;}   // NUY
//virtual PXOB   CheckSubQuery(PGLOBAL, PSQL);
//virtual bool   CheckLocal(PTDB);
//virtual int    CheckSpcCol(PTDB tdbp, int n);
	void   Printf(PGLOBAL g, FILE *f, uint n) override;
  void   Prints(PGLOBAL g, char *ps, uint z) override;
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
  FILTER(void) = default;       // Standard constructor not to be used
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
  bool Eval(PGLOBAL) override = 0; // just to prevent direct FILTERX use

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
  FILTERCMP(PGLOBAL, OPVAL);

  // Methods
  bool Eval(PGLOBAL) override;
  }; // end of class FILTEREQ

/***********************************************************************/
/*  Derived class FILTERAND: OP_AND, no conversion and Xobject args.   */
/***********************************************************************/
class FILTERAND : public FILTERX {
 public:
  // Methods
  bool Eval(PGLOBAL) override;
  }; // end of class FILTERAND

/***********************************************************************/
/*  Derived class FILTEROR: OP_OR, no conversion and Xobject args.     */
/***********************************************************************/
class FILTEROR : public FILTERX {
 public:
  // Methods
  bool Eval(PGLOBAL) override;
  }; // end of class FILTEROR

/***********************************************************************/
/*  Derived class FILTERNOT: OP_NOT, no conversion and Xobject args.   */
/***********************************************************************/
class FILTERNOT : public FILTERX {
 public:
  // Methods
  bool Eval(PGLOBAL) override;
  }; // end of class FILTERNOT

/***********************************************************************/
/*  Derived class FILTERIN: OP_IN, no conversion and Array 2nd arg.    */
/***********************************************************************/
class FILTERIN : public FILTERX {
 public:
  // Methods
  bool Eval(PGLOBAL) override;
  }; // end of class FILTERIN

/***********************************************************************/
/*  Derived class FILTERTRUE: Always returns TRUE.                     */
/***********************************************************************/
class FILTERTRUE : public FILTERX {
 public:
  // Constructor
  FILTERTRUE(PVAL valp) {Value = valp; Value->SetValue_bool(TRUE);}

  // Methods
  void Reset(void) override;
  bool Eval(PGLOBAL) override;
  }; // end of class FILTERTRUE

#endif // __FILTER__
