/*************** Valblk H Declares Source Code File (.H) ***************/
/*  Name: VALBLK.H    Version 1.7                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2013    */
/*                                                                     */
/*  This file contains the VALBLK and derived classes declares.        */
/***********************************************************************/

/***********************************************************************/
/*  Include required application header files                          */
/*  assert.h     is header required when using the assert function.    */
/*  block.h      is header containing Block    global declarations.    */
/***********************************************************************/
#ifndef __VALBLK__H__
#define __VALBLK__H__
#include "value.h"

/***********************************************************************/
/*  Utility used to allocate value blocks.                             */
/***********************************************************************/
DllExport PVBLK AllocValBlock(PGLOBAL, void*, int, int, int, int, bool, bool);

/***********************************************************************/
/*  Class VALBLK represent a base class for variable blocks.           */
/***********************************************************************/
class VALBLK : public BLOCK {
//friend void SemColData(PGLOBAL g, PSEM semp);
 public:
  // Constructors
  VALBLK(void *mp, int type, int nval);

  // Implementation
          int    GetNval(void) {return Nval;}
          void   SetNval(int n) {Nval = n;}
          void  *GetValPointer(void) {return Blkp;}
          void   SetValPointer(void *mp) {Blkp = mp;}
          int    GetType(void) {return Type;}
          void   SetCheck(bool b) {Check = b;}
          void   MoveNull(int i, int j)
                  {if (To_Nulls) To_Nulls[j] = To_Nulls[j];}
  virtual void   SetNull(int n, bool b)
                  {if (To_Nulls) {To_Nulls[n] = (b) ? '*' : 0;}}
  virtual bool   IsNull(int n) {return To_Nulls && To_Nulls[n];}
  virtual void   SetNullable(bool b);
  virtual void   Init(PGLOBAL g, bool check) = 0;
  virtual int    GetVlen(void) = 0;
  virtual PSZ    GetCharValue(int n);
  virtual short  GetShortValue(int n) = 0;
  virtual int    GetIntValue(int n) = 0;
  virtual longlong GetBigintValue(int n) = 0;
  virtual double GetFloatValue(int n) = 0;
  virtual void   ReAlloc(void *mp, int n) {Blkp = mp; Nval = n;}
  virtual void   Reset(int n) = 0;
  virtual bool   SetFormat(PGLOBAL g, PSZ fmt, int len, int year = 0);
  virtual void   SetPrec(int p) {}
  virtual bool   IsCi(void) {return false;}

  // Methods
  virtual void   SetValue(short sval, int n) {assert(false);}
  virtual void   SetValue(int lval, int n) {assert(false);}
  virtual void   SetValue(longlong lval, int n) {assert(false);}
  virtual void   SetValue(PSZ sp, int n) {assert(false);}
  virtual void   SetValue(PVAL valp, int n) = 0;
  virtual void   SetValue(PVBLK pv, int n1, int n2) = 0;
#if 0
  virtual void   SetMin(PVAL valp, int n) = 0;
  virtual void   SetMax(PVAL valp, int n) = 0;
  virtual void   SetValues(PVBLK pv, int i, int n) = 0;
  virtual void   AddMinus1(PVBLK pv, int n1, int n2) {assert(false);}
#endif // 0
  virtual void   Move(int i, int j) = 0;
  virtual int    CompVal(PVAL vp, int n) = 0;
  virtual int    CompVal(int i1, int i2) = 0;
  virtual void  *GetValPtr(int n) = 0;
  virtual void  *GetValPtrEx(int n) = 0;
  virtual int    Find(PVAL vp) = 0;
  virtual int    GetMaxLength(void) = 0;
          bool   Locate(PVAL vp, int& i);

 protected:
#if defined(_DEBUG) || defined(DEBTRACE)
  void ChkIndx(int n);
  void ChkPrm(PVAL v, int n);
  void ChkTyp(PVAL v);
  void ChkTyp(PVBLK vb);
#endif   // _DEBUG) ||         DEBTRACE

  // Members
  PGLOBAL Global;           // Used for messages and allocation
  char   *To_Nulls;         // Null values array
  void   *Blkp;             // To value block
  int     Type;             // Type of individual values
  int     Nval;             // Max number of values in block
  bool    Check;            // If true SetValue types must match
  bool    Nullable;         // True if values can be null
  }; // end of class VALBLK

/***********************************************************************/
/*  Class CHRBLK: represent a block of fixed length strings.           */
/***********************************************************************/
class CHRBLK : public VALBLK {
 public:
  // Constructors
  CHRBLK(void *mp, int size, int len, int prec, bool b);

  // Implementation
  virtual void   Init(PGLOBAL g, bool check);
  virtual int    GetVlen(void) {return Long;}
  virtual PSZ    GetCharValue(int n);
  virtual short  GetShortValue(int n);
  virtual int    GetIntValue(int n);
  virtual longlong GetBigintValue(int n);
  virtual double GetFloatValue(int n);
  virtual void   Reset(int n);
  virtual void   SetPrec(int p) {Ci = (p != 0);}
  virtual bool   IsCi(void) {return Ci;}

  // Methods
  virtual void   SetValue(PSZ sp, int n);
  virtual void   SetValue(PVAL valp, int n);
  virtual void   SetValue(PVBLK pv, int n1, int n2);
#if 0
  virtual void   SetMin(PVAL valp, int n);
  virtual void   SetMax(PVAL valp, int n);
  virtual void   SetValues(PVBLK pv, int k, int n);
#endif // 0
  virtual void   Move(int i, int j);
  virtual int    CompVal(PVAL vp, int n);
  virtual int    CompVal(int i1, int i2);
  virtual void  *GetValPtr(int n);
  virtual void  *GetValPtrEx(int n);
  virtual int    Find(PVAL vp);
  virtual int    GetMaxLength(void);

 protected:
  // Members
  char* const &Chrp;             // Pointer to char buffer
  PSZ   Valp;                    // Used to make a zero ended value
  bool  Blanks;                  // True for right filling with blanks
  bool  Ci;                      // True if case insensitive
  int   Long;                    // Length of each string
  }; // end of class CHRBLK

/***********************************************************************/
/*  Class STRBLK: represent a block of string pointers.                */
/*  Currently this class is used only by the DECODE scalar function    */
/*  and by the MyColumn function to store date formats.                */
/***********************************************************************/
class STRBLK : public VALBLK {
 public:
  // Constructors
  STRBLK(PGLOBAL g, void *mp, int size);

  // Implementation
  virtual void   SetNull(int n, bool b) {if (b) {Strp[n] = NULL;}}
  virtual bool   IsNull(int n) {return Strp[n] == NULL;}
  virtual void   SetNullable(bool b) {}    // Always nullable
  virtual void   Init(PGLOBAL g, bool check);
  virtual int    GetVlen(void) {return sizeof(PSZ);}
  virtual PSZ    GetCharValue(int n) {return Strp[n];}
  virtual short  GetShortValue(int n) {return (short)atoi(Strp[n]);}
  virtual int    GetIntValue(int n) {return atol(Strp[n]);}
  virtual longlong GetBigintValue(int n) {return atoll(Strp[n]);}
  virtual double GetFloatValue(int n) {return atof(Strp[n]);}
  virtual void   Reset(int n) {Strp[n] = NULL;}

  // Methods
  virtual void   SetValue(PSZ sp, int n);
  virtual void   SetValue(PVAL valp, int n);
  virtual void   SetValue(PVBLK pv, int n1, int n2);
#if 0
  virtual void   SetMin(PVAL valp, int n);
  virtual void   SetMax(PVAL valp, int n);
  virtual void   SetValues(PVBLK pv, int k, int n);
#endif // 0
  virtual void   Move(int i, int j);
  virtual int    CompVal(PVAL vp, int n);
  virtual int    CompVal(int i1, int i2);
  virtual void  *GetValPtr(int n);
  virtual void  *GetValPtrEx(int n);
  virtual int    Find(PVAL vp);
  virtual int    GetMaxLength(void);

 protected:
  // Members
  PSZ* const &Strp;              // Pointer to PSZ buffer
  }; // end of class STRBLK

/***********************************************************************/
/*  Class SHRBLK: represents a block of int integer values.           */
/***********************************************************************/
class SHRBLK : public VALBLK {
 public:
  // Constructors
  SHRBLK(void *mp, int size);

  // Implementation
  virtual void   Init(PGLOBAL g, bool check);
  virtual int    GetVlen(void) {return sizeof(short);}
//virtual PSZ    GetCharValue(int n);
  virtual short  GetShortValue(int n) {return Shrp[n];}
  virtual int    GetIntValue(int n) {return (int)Shrp[n];}
  virtual longlong GetBigintValue(int n) {return (longlong)Shrp[n];}
  virtual double GetFloatValue(int n) {return (double)Shrp[n];}
  virtual void   Reset(int n) {Shrp[n] = 0;}

  // Methods
  virtual void   SetValue(PSZ sp, int n);
  virtual void   SetValue(short sval, int n)
                  {Shrp[n] = sval; SetNull(n, false);}
  virtual void   SetValue(int lval, int n)
                  {Shrp[n] = (short)lval; SetNull(n, false);}
  virtual void   SetValue(longlong lval, int n)
                  {Shrp[n] = (short)lval; SetNull(n, false);}
  virtual void   SetValue(PVAL valp, int n);
  virtual void   SetValue(PVBLK pv, int n1, int n2);
#if 0
  virtual void   SetMin(PVAL valp, int n);
  virtual void   SetMax(PVAL valp, int n);
  virtual void   SetValues(PVBLK pv, int k, int n);
  virtual void   AddMinus1(PVBLK pv, int n1, int n2);
#endif // 0
  virtual void   Move(int i, int j);
  virtual int    CompVal(PVAL vp, int n);
  virtual int    CompVal(int i1, int i2);
  virtual void  *GetValPtr(int n);
  virtual void  *GetValPtrEx(int n);
  virtual int    Find(PVAL vp);
  virtual int    GetMaxLength(void);

 protected:
  // Members
  short* const &Shrp;
  }; // end of class SHRBLK

/***********************************************************************/
/*  Class LNGBLK: represents a block of int integer values.           */
/***********************************************************************/
class LNGBLK : public VALBLK {
 public:
  // Constructors
  LNGBLK(void *mp, int size);

  // Implementation
  virtual void   Init(PGLOBAL g, bool check);
  virtual int    GetVlen(void) {return sizeof(int);}
//virtual PSZ    GetCharValue(int n);
  virtual short  GetShortValue(int n) {return (short)Lngp[n];}
  virtual int    GetIntValue(int n) {return Lngp[n];}
  virtual longlong GetBigintValue(int n) {return (longlong)Lngp[n];}
  virtual double GetFloatValue(int n) {return (double)Lngp[n];}
  virtual void   Reset(int n) {Lngp[n] = 0;}

  // Methods
  virtual void   SetValue(PSZ sp, int n);
  virtual void   SetValue(short sval, int n)
                  {Lngp[n] = (int)sval; SetNull(n, false);}
  virtual void   SetValue(int lval, int n)
                  {Lngp[n] = lval; SetNull(n, false);}
  virtual void   SetValue(longlong lval, int n)
                  {Lngp[n] = (int)lval; SetNull(n, false);}
  virtual void   SetValue(PVAL valp, int n);
  virtual void   SetValue(PVBLK pv, int n1, int n2);
#if 0
  virtual void   SetMin(PVAL valp, int n);
  virtual void   SetMax(PVAL valp, int n);
  virtual void   SetValues(PVBLK pv, int k, int n);
  virtual void   AddMinus1(PVBLK pv, int n1, int n2);
#endif // 0
  virtual void   Move(int i, int j);
  virtual int    CompVal(PVAL vp, int n);
  virtual int    CompVal(int i1, int i2);
  virtual void  *GetValPtr(int n);
  virtual void  *GetValPtrEx(int n);
  virtual int    Find(PVAL vp);
  virtual int    GetMaxLength(void);

 protected:
  // Members
  int* const &Lngp;
  }; // end of class LNGBLK

/***********************************************************************/
/*  Class DATBLK: represents a block of time stamp values.             */
/***********************************************************************/
class DATBLK : public LNGBLK {
 public:
  // Constructor
  DATBLK(void *mp, int size);

  // Implementation
  virtual bool SetFormat(PGLOBAL g, PSZ fmt, int len, int year = 0);

  // Methods
  virtual void SetValue(PSZ sp, int n);

 protected:
  // Members
  PVAL Dvalp;                    // Date value used to convert string
  }; // end of class DATBLK

/***********************************************************************/
/*  Class BIGBLK: represents a block of big integer values.            */
/***********************************************************************/
class BIGBLK : public VALBLK {
 public:
  // Constructors
  BIGBLK(void *mp, int size);

  // Implementation
  virtual void   Init(PGLOBAL g, bool check);
  virtual int    GetVlen(void) {return sizeof(longlong);}
//virtual PSZ    GetCharValue(int n);
  virtual short  GetShortValue(int n) {return (short)Lngp[n];}
  virtual int    GetIntValue(int n) {return (int)Lngp[n];}
  virtual longlong GetBigintValue(int n) {return Lngp[n];}
  virtual double GetFloatValue(int n) {return (double)Lngp[n];}
  virtual void   Reset(int n) {Lngp[n] = 0LL;}

  // Methods
  virtual void   SetValue(PSZ sp, int n);
  virtual void   SetValue(short sval, int n)
                  {Lngp[n] = (longlong)sval; SetNull(n, false);}
  virtual void   SetValue(int lval, int n)
                  {Lngp[n] = (longlong)lval; SetNull(n, false);}
  virtual void   SetValue(longlong lval, int n)
                  {Lngp[n] = lval; SetNull(n, false);}
  virtual void   SetValue(PVAL valp, int n);
  virtual void   SetValue(PVBLK pv, int n1, int n2);
#if 0
  virtual void   SetMin(PVAL valp, int n);
  virtual void   SetMax(PVAL valp, int n);
  virtual void   SetValues(PVBLK pv, int k, int n);
  virtual void   AddMinus1(PVBLK pv, int n1, int n2);
#endif // 0
  virtual void   Move(int i, int j);
  virtual int    CompVal(PVAL vp, int n);
  virtual int    CompVal(int i1, int i2);
  virtual void  *GetValPtr(int n);
  virtual void  *GetValPtrEx(int n);
  virtual int    Find(PVAL vp);
  virtual int    GetMaxLength(void);

 protected:
  // Members
  longlong* const &Lngp;
  }; // end of class BIGBLK

/***********************************************************************/
/*  Class DBLBLK: represents a block of double float values.           */
/***********************************************************************/
class DBLBLK : public VALBLK {
 public:
  // Constructors
  DBLBLK(void *mp, int size, int prec);

  // Implementation
  virtual void   Init(PGLOBAL g, bool check);
  virtual int    GetVlen(void) {return sizeof(double);}
//virtual PSZ    GetCharValue(int n);
  virtual short  GetShortValue(int n) {return (short)Dblp[n];}
  virtual int    GetIntValue(int n) {return (int)Dblp[n];}
  virtual longlong GetBigintValue(int n) {return (longlong)Dblp[n];}
  virtual double GetFloatValue(int n) {return Dblp[n];}
  virtual void   Reset(int n) {Dblp[n] = 0.0;}
  virtual void   SetPrec(int p) {Prec = p;}

  // Methods
  virtual void   SetValue(PSZ sp, int n);
  virtual void   SetValue(PVAL valp, int n);
  virtual void   SetValue(PVBLK pv, int n1, int n2);
#if 0
  virtual void   SetMin(PVAL valp, int n);
  virtual void   SetMax(PVAL valp, int n);
  virtual void   SetValues(PVBLK pv, int k, int n);
#endif // 0
  virtual void   Move(int i, int j);
  virtual int    CompVal(PVAL vp, int n);
  virtual int    CompVal(int i1, int i2);
  virtual void  *GetValPtr(int n);
  virtual void  *GetValPtrEx(int n);
  virtual int    Find(PVAL vp);
  virtual int    GetMaxLength(void);

 protected:
  // Members
  double* const &Dblp;
  int      Prec;
  }; // end of class DBLBLK

#endif // __VALBLK__H__

