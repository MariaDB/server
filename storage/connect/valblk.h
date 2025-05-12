/*************** Valblk H Declares Source Code File (.H) ***************/
/*  Name: VALBLK.H    Version 2.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2014    */
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
DllExport PVBLK AllocValBlock(PGLOBAL, void*, int, int, int, int,
                                              bool, bool, bool);
const char *GetFmt(int type, bool un = false);

/***********************************************************************/
/*  DB static external variables.                                      */
/***********************************************************************/
extern MBLOCK Nmblk;                /* Used to initialize MBLOCK's     */

/***********************************************************************/
/*  Class MBVALS is a utility class for (re)allocating VALBLK's.       */
/***********************************************************************/
class MBVALS : public BLOCK {
//friend class LSTBLK;
  friend class ARRAY;
 public:
  // Constructors
  MBVALS(void) {Vblk = NULL; Mblk = Nmblk;}

  // Methods
  void  *GetMemp(void) {return Mblk.Memp;}
  PVBLK  Allocate(PGLOBAL g, int type, int len, int prec,
                             int n, bool sub = false);
  bool   ReAllocate(PGLOBAL g, int n);
  void   Free(void);

 protected:
  // Members
  PVBLK  Vblk;                    // Pointer to VALBLK
  MBLOCK Mblk;                    // The memory block
  }; // end of class MBVALS

typedef class MBVALS *PMBV;

/***********************************************************************/
/*  Class VALBLK represent a base class for variable blocks.           */
/***********************************************************************/
class VALBLK : public BLOCK {
 public:
  // Constructors
  VALBLK(void *mp, int type, int nval, bool un = false);

  // Implementation
          int    GetNval(void) {return Nval;}
          void   SetNval(int n) {Nval = n;}
          void  *GetValPointer(void) {return Blkp;}
          void   SetValPointer(void *mp) {Blkp = mp;}
          int    GetType(void) {return Type;}
          int    GetPrec(void) {return Prec;}
          void   SetCheck(bool b) {Check = b;}
          void   MoveNull(int i, int j)
                  {if (To_Nulls) To_Nulls[j] = To_Nulls[i];}
  virtual void   SetNull(int n, bool b)
                  {if (To_Nulls) {To_Nulls[n] = (b) ? '*' : 0;}}
  virtual bool   IsNull(int n) {return To_Nulls && To_Nulls[n];}
	virtual bool   IsNullable(void) {return Nullable;}
	virtual void   SetNullable(bool b);
  virtual bool   IsUnsigned(void) {return Unsigned;}
  virtual bool   Init(PGLOBAL g, bool check) = 0;
  virtual int    GetVlen(void) = 0;
  virtual PSZ    GetCharValue(int n);
  virtual char   GetTinyValue(int n) = 0;
  virtual uchar  GetUTinyValue(int n) = 0;
  virtual short  GetShortValue(int n) = 0;
  virtual ushort GetUShortValue(int n) = 0;
  virtual int    GetIntValue(int n) = 0;
  virtual uint   GetUIntValue(int n) = 0;
  virtual longlong GetBigintValue(int n) = 0;
  virtual ulonglong GetUBigintValue(int n) = 0;
  virtual double GetFloatValue(int n) = 0;
  virtual char  *GetCharString(char *p, int n) = 0;
  virtual void   ReAlloc(void *mp, int n) {Blkp = mp; Nval = n;}
  virtual void   Reset(int n) = 0;
  virtual bool   SetFormat(PGLOBAL g, PCSZ fmt, int len, int year = 0);
  virtual void   SetPrec(int p) {}
  virtual bool   IsCi(void) {return false;}

  // Methods
  virtual void   SetValue(short, int) {assert(false);}
  virtual void   SetValue(ushort, int) {assert(false);}
  virtual void   SetValue(int, int) {assert(false);}
  virtual void   SetValue(uint, int) {assert(false);}
  virtual void   SetValue(longlong, int) {assert(false);}
  virtual void   SetValue(ulonglong, int) {assert(false);}
  virtual void   SetValue(double, int) {assert(false);}
  virtual void   SetValue(char, int) {assert(false);}
  virtual void   SetValue(uchar, int) {assert(false);}
  virtual void   SetValue(PCSZ, int) {assert(false);}
  virtual void   SetValue(const char *, uint, int) {assert(false);}
  virtual void   SetValue(PVAL valp, int n) = 0;
  virtual void   SetValue(PVBLK pv, int n1, int n2) = 0;
  virtual void   SetMin(PVAL valp, int n) = 0;
  virtual void   SetMax(PVAL valp, int n) = 0;
  virtual void   Move(int i, int j) = 0;
  virtual int    CompVal(PVAL vp, int n) = 0;
  virtual int    CompVal(int i1, int i2) = 0;
  virtual void  *GetValPtr(int n) = 0;
  virtual void  *GetValPtrEx(int n) = 0;
  virtual int    Find(PVAL vp) = 0;
  virtual int    GetMaxLength(void) = 0;
          bool   Locate(PVAL vp, int& i);

 protected:
  bool AllocBuff(PGLOBAL g, size_t size);
  void ChkIndx(int n);
  void ChkTyp(PVAL v);
  void ChkTyp(PVBLK vb);

  // Members
  PGLOBAL Global;           // Used for messages and allocation
  MBLOCK  Mblk;             // Used to allocate buffer
  char   *To_Nulls;         // Null values array
  void   *Blkp;             // To value block
  bool    Check;            // If true SetValue types must match
  bool    Nullable;         // True if values can be null
  bool    Unsigned;         // True if values are unsigned
  int     Type;             // Type of individual values
  int     Nval;             // Max number of values in block
  int     Prec;             // Precision of float values
  }; // end of class VALBLK

/***********************************************************************/
/*  Class TYPBLK: represents a block of typed values.                  */
/***********************************************************************/
template <class TYPE>
class TYPBLK : public VALBLK {
 public:
  // Constructors
  TYPBLK(void *mp, int size, int type, int prec = 0, bool un = false);

  // Implementation
  bool   Init(PGLOBAL g, bool check) override;
  int    GetVlen(void) override {return sizeof(TYPE);}

  char   GetTinyValue(int n) override {return (char)UnalignedRead(n);}
  uchar  GetUTinyValue(int n) override {return (uchar)UnalignedRead(n);}
  short  GetShortValue(int n) override {return (short)UnalignedRead(n);}
  ushort GetUShortValue(int n) override {return (ushort)UnalignedRead(n);}
  int    GetIntValue(int n) override {return (int)UnalignedRead(n);}
  uint   GetUIntValue(int n) override {return (uint)UnalignedRead(n);}
  longlong GetBigintValue(int n) override {return (longlong)UnalignedRead(n);}
  ulonglong GetUBigintValue(int n) override {return (ulonglong)UnalignedRead(n);}
  double GetFloatValue(int n) override {return (double)UnalignedRead(n);}
  char  *GetCharString(char *p, int n) override;
  void   Reset(int n) override {UnalignedWrite(n, 0);}

  // Methods
  using VALBLK::SetValue;
  void   SetValue(PCSZ sp, int n) override;
  void   SetValue(const char *sp, uint len, int n) override;
  void   SetValue(short sval, int n) override
                  {UnalignedWrite(n, (TYPE)sval); SetNull(n, false);}
  void   SetValue(ushort sval, int n) override
                  {UnalignedWrite(n, (TYPE)sval); SetNull(n, false);}
  void   SetValue(int lval, int n) override
                  {UnalignedWrite(n, (TYPE)lval); SetNull(n, false);}
  void   SetValue(uint lval, int n) override
                  {UnalignedWrite(n, (TYPE)lval); SetNull(n, false);}
  void   SetValue(longlong lval, int n) override
                  {UnalignedWrite(n, (TYPE)lval); SetNull(n, false);}
  void   SetValue(ulonglong lval, int n) override
                  {UnalignedWrite(n, (TYPE)lval); SetNull(n, false);}
  void   SetValue(double fval, int n) override
                  {UnalignedWrite(n, (TYPE)fval); SetNull(n, false);}
  void   SetValue(char cval, int n) override
                  {UnalignedWrite(n, (TYPE)cval); SetNull(n, false);}
  void   SetValue(uchar cval, int n) override
                  {UnalignedWrite(n, (TYPE)cval); SetNull(n, false);}
  void   SetValue(PVAL valp, int n) override;
  void   SetValue(PVBLK pv, int n1, int n2) override;
  void   SetMin(PVAL valp, int n) override;
  void   SetMax(PVAL valp, int n) override;
  void   Move(int i, int j) override;
  int    CompVal(PVAL vp, int n) override;
  int    CompVal(int i1, int i2) override;
  void  *GetValPtr(int n) override;
  void  *GetValPtrEx(int n) override;
  int    Find(PVAL vp) override;
  int    GetMaxLength(void) override;

 protected:
  // Specialized functions
  static ulonglong MaxVal(void);
  TYPE GetTypedValue(PVAL vp);
  TYPE GetTypedValue(PVBLK blk, int n);

  // Members
  TYPE* const &Typp;
  const char  *Fmt;

  // Unaligned access
  TYPE UnalignedRead(int n) const {
    TYPE result;
    memcpy(&result, Typp + n, sizeof(TYPE));
    return result;
  }

  void UnalignedWrite(int n, TYPE value) {
    memcpy(Typp + n, &value, sizeof(TYPE));
  }
  }; // end of class TYPBLK

/***********************************************************************/
/*  Class CHRBLK: represent a block of fixed length strings.           */
/***********************************************************************/
class CHRBLK : public VALBLK {
 public:
  // Constructors
  CHRBLK(void *mp, int size, int type, int len, int prec, bool b);

  // Implementation
  bool   Init(PGLOBAL g, bool check) override;
  int    GetVlen(void) override {return Long;}
  PSZ    GetCharValue(int n) override;
  char   GetTinyValue(int n) override;
  uchar  GetUTinyValue(int n) override;
  short  GetShortValue(int n) override;
  ushort GetUShortValue(int n) override;
  int    GetIntValue(int n) override;
  uint   GetUIntValue(int n) override;
  longlong GetBigintValue(int n) override;
  ulonglong GetUBigintValue(int n) override;
  double GetFloatValue(int n) override;
  char  *GetCharString(char *p, int n) override;
  void   Reset(int n) override;
  void   SetPrec(int p) override {Ci = (p != 0);}
  bool   IsCi(void) override {return Ci;}

  // Methods
  using VALBLK::SetValue;
  void   SetValue(PCSZ sp, int n) override;
  void   SetValue(const char *sp, uint len, int n) override;
  void   SetValue(PVAL valp, int n) override;
  void   SetValue(PVBLK pv, int n1, int n2) override;
  void   SetMin(PVAL valp, int n) override;
  void   SetMax(PVAL valp, int n) override;
  void   Move(int i, int j) override;
  int    CompVal(PVAL vp, int n) override;
  int    CompVal(int i1, int i2) override;
  void  *GetValPtr(int n) override;
  void  *GetValPtrEx(int n) override;
  int    Find(PVAL vp) override;
  int    GetMaxLength(void) override;

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
  STRBLK(PGLOBAL g, void *mp, int size, int type);

  // Implementation
  void   SetNull(int n, bool b) override {if (b) {Strp[n] = NULL;}}
  bool   IsNull(int n) override {return Strp[n] == NULL;}
  void   SetNullable(bool) override {}      // Always nullable
  bool   Init(PGLOBAL g, bool check) override;
  int    GetVlen(void) override {return sizeof(PSZ);}
  PSZ    GetCharValue(int n) override {return Strp[n];}
  char   GetTinyValue(int n) override;
  uchar  GetUTinyValue(int n) override;
  short  GetShortValue(int n) override;
  ushort GetUShortValue(int n) override;
  int    GetIntValue(int n) override;
  uint   GetUIntValue(int n) override;
  longlong GetBigintValue(int n) override;
  ulonglong GetUBigintValue(int n) override;
  double GetFloatValue(int n) override {return atof(Strp[n]);}
  char  *GetCharString(char *, int n) override {return Strp[n];}
  void   Reset(int n) override {Strp[n] = NULL;}

  // Methods
  using VALBLK::SetValue;
  void   SetValue(PCSZ sp, int n) override;
  void   SetValue(const char *sp, uint len, int n) override;
  void   SetValue(PVAL valp, int n) override;
  void   SetValue(PVBLK pv, int n1, int n2) override;
  void   SetMin(PVAL valp, int n) override;
  void   SetMax(PVAL valp, int n) override;
  void   Move(int i, int j) override;
  int    CompVal(PVAL vp, int n) override;
  int    CompVal(int i1, int i2) override;
  void  *GetValPtr(int n) override;
  void  *GetValPtrEx(int n) override;
  int    Find(PVAL vp) override;
  int    GetMaxLength(void) override;

  // Specific
          void   SetSorted(bool b) {Sorted = b;}

 protected:
  // Members
  PSZ* const &Strp;              // Pointer to PSZ buffer
  bool        Sorted;            // Values are (semi?) sorted
  }; // end of class STRBLK

/***********************************************************************/
/*  Class DATBLK: represents a block of time stamp values.             */
/***********************************************************************/
class DATBLK : public TYPBLK<int> {
 public:
  // Constructor
  DATBLK(void *mp, int size);

  // Implementation
  bool  SetFormat(PGLOBAL g, PCSZ fmt, int len, int year = 0) override;
  char *GetCharString(char *p, int n) override;

  // Methods
  using TYPBLK<int>::SetValue;
  void  SetValue(PCSZ sp, int n) override;

 protected:
  // Members
  PVAL Dvalp;                    // Date value used to convert string
  }; // end of class DATBLK

/***********************************************************************/
/*  Class PTRBLK: represent a block of char pointers.                  */
/*  Currently this class is used only by the ARRAY class to make and   */
/*  sort a list of char pointers.                                      */
/***********************************************************************/
class PTRBLK : public STRBLK {
  friend class ARRAY;
  friend PVBLK AllocValBlock(PGLOBAL, void *, int, int, int, int,
                                              bool, bool, bool);
 protected:
  // Constructors
  PTRBLK(PGLOBAL g, void *mp, int size) : STRBLK(g, mp, size, TYPE_PCHAR) {}

  // Implementation

  // Methods
  using STRBLK::SetValue;
  using STRBLK::CompVal;
  void   SetValue(PCSZ p, int n) override {Strp[n] = (char*)p;}
  int    CompVal(int i1, int i2) override;

 protected:
  // Members
  }; // end of class PTRBLK

#endif // __VALBLK__H__

