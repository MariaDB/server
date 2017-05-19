/**************** Array H Declares Source Code File (.H) ***************/
/*  Name: ARRAY.H    Version 3.1                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2017    */
/*                                                                     */
/*  This file contains the ARRAY and VALBASE derived classes declares. */
/***********************************************************************/
#ifndef __ARRAY_H
#define __ARRAY_H


/***********************************************************************/
/*  Include required application header files                          */
/***********************************************************************/
#include "xobject.h"
#include "valblk.h"
#include "csort.h"

typedef class ARRAY *PARRAY;

/***********************************************************************/
/*  Definition of class ARRAY  with all its method functions.          */
/*  Note: This is not a general array class that could be defined as   */
/*  a template class, but rather a specific object containing a list   */
/*  of values to be processed by the filter IN operator.               */
/*  In addition it must act as a metaclass by being able to give back  */
/*  the type of values it contains.                                    */
/*  It must also be able to convert itself from some type to another.  */
/***********************************************************************/
class DllExport ARRAY : public XOBJECT, public CSORT { // Array descblock
  friend class MULAR;
//friend class VALLST;
//friend class SFROW;
 public:
  // Constructors
  ARRAY(PGLOBAL g, int type, int size, int len = 1, int prec = 0);
//ARRAY(PGLOBAL g, PQUERY qryp);
//ARRAY(PGLOBAL g, PARRAY par, int k);

  // Implementation
  virtual int   GetType(void) {return TYPE_ARRAY;}
  virtual int   GetResultType(void) {return Type;}
  virtual int   GetLength(void) {return Len;}
  virtual int   GetLengthEx(void) {return Len;}
  virtual int   GetScale() {return 0;}
          int   GetNval(void) {return Nval;}
          int   GetSize(void) {return Size;}
//        PVAL  GetValp(void) {return Valp;}
          void  SetType(int atype) {Type = atype;}
//        void  SetCorrel(bool b) {Correlated = b;}

  // Methods
  using XOBJECT::GetIntValue;
  virtual void  Reset(void) {Bot = -1;}
  virtual int   Qcompare(int *, int *);
  virtual bool  Compare(PXOB) {assert(false); return false;}
  virtual bool  SetFormat(PGLOBAL, FORMAT&) {assert(false); return false;}
//virtual int   CheckSpcCol(PTDB, int) {return 0;}
  virtual void  Printf(PGLOBAL g, FILE *f, uint n);
  virtual void  Prints(PGLOBAL g, char *ps, uint z);
//        void  Empty(void);
          void  SetPrecision(PGLOBAL g, int p);
          bool  AddValue(PGLOBAL g, PSZ sp);
          bool  AddValue(PGLOBAL g, void *p);
          bool  AddValue(PGLOBAL g, short n);
          bool  AddValue(PGLOBAL g, int n);
          bool  AddValue(PGLOBAL g, double f);
          bool  AddValue(PGLOBAL g, PXOB xp);
          bool  AddValue(PGLOBAL g, PVAL vp);
          void  GetNthValue(PVAL valp, int n);
          int   GetIntValue(int n);
          char *GetStringValue(int n);
          BYTE  Vcompare(PVAL vp, int n);
          void  Save(int);
          void  Restore(int);
          void  Move(int, int);
          bool  Sort(PGLOBAL g);
          void *GetSortIndex(PGLOBAL g);
          bool  Find(PVAL valp);
          bool  FilTest(PGLOBAL g, PVAL valp, OPVAL opc, int opm);
          int   Convert(PGLOBAL g, int k, PVAL vp = NULL);
          int   BlockTest(PGLOBAL g, int opc, int opm,
                          void *minp, void *maxp, bool s);
          PSZ   MakeArrayList(PGLOBAL g);
          bool  CanBeShort(void);
          bool  GetSubValue(PGLOBAL g, PVAL valp, int *kp);

 protected:
  // Members
  PMBV   Valblk;        // To the MBVALS class
  PVBLK  Vblp;          // To Valblock of the data array
//PVAL   Valp;          // The value used for Save and Restore is Value
  int    Size;          // Size of value array
  int    Nval;          // Total number of items in array
  int    Ndif;          // Total number of distinct items in array
  int    Xsize;         // Size of Index (used for correlated arrays)
  int    Type;          // Type of individual values in the array
  int    Len;           // Length of character string
  int    Bot;           // Bottom of research index
  int    Top;           // Top    of research index
  int    X, Inf, Sup;   // Used for block optimization
//bool   Correlated;    // -----------> Temporary
  }; // end of class ARRAY

/***********************************************************************/
/*  Definition of class MULAR with all its method functions.           */
/*  This class is used when constructing the arrays of constants used  */
/*  for indexing. Its only purpose is to provide a way to sort, reduce */
/*  and reorder the arrays of multicolumn indexes as one block. Indeed */
/*  sorting the arrays independantly would break the correspondance of */
/*  column values.                                                     */
/***********************************************************************/
class MULAR : public CSORT, public BLOCK {   // No need to be an XOBJECT
 public:
  // Constructor
  MULAR(PGLOBAL g, int n);

  // Implementation
  void SetPars(PARRAY par, int i) {Pars[i] = par;}

  // Methods
  virtual int Qcompare(int *i1, int *i2);   // Sort compare routine
          bool Sort(PGLOBAL g);

 protected:
  // Members
  int     Narray;         // The number of sub-arrays
  PARRAY *Pars;           // To the block of real arrays
  }; // end of class ARRAY

#endif // __ARRAY_H
