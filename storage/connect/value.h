/**************** Value H Declares Source Code File (.H) ***************/
/*  Name: VALUE.H    Version 1.7                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2001-2013    */
/*                                                                     */
/*  This file contains the VALUE and derived classes declares.         */
/***********************************************************************/

/***********************************************************************/
/*  Include required application header files                          */
/*  assert.h     is header required when using the assert function.    */
/*  block.h      is header containing Block    global declarations.    */
/***********************************************************************/
#ifndef __VALUE__H__
#define __VALUE__H__
#include "assert.h"
#include "block.h"

#if defined(WIN32)
#define strtoll _strtoi64
#define atoll(S) strtoll(S, NULL, 10)
#endif   // WIN32

/***********************************************************************/
/*  Types used in some class definitions.                              */
/***********************************************************************/
enum CONV {CNV_ANY     =   0,         /* Convert to any type           */
           CNV_CHAR    =   1,         /* Convert to character type     */
           CNV_NUM     =   2};        /* Convert to numeric type       */

/***********************************************************************/
/*  Types used in some class definitions.                              */
/***********************************************************************/
class CONSTANT;                       // For friend setting
typedef struct _datpar *PDTP;         // For DTVAL


/***********************************************************************/
/*  Utilities used to test types and to allocated values.              */
/***********************************************************************/
int   GetPLGType(int);
PVAL  AllocateValue(PGLOBAL, void *, short);

// Exported functions
DllExport PSZ   GetTypeName(int);
DllExport int   GetTypeSize(int, int);
#ifdef ODBC_SUPPORT
/* This function is exported for use in EOM table type DLLs */
DllExport int   TranslateSQLType(int stp, int prec, int& len);
#endif
DllExport char *GetFormatType(int);
DllExport int   GetFormatType(char);
DllExport int   GetDBType(int);
DllExport bool  IsTypeChar(int type);
DllExport bool  IsTypeNum(int type);
DllExport int   ConvertType(int, int, CONV, bool match = false);
DllExport PVAL  AllocateValue(PGLOBAL, PVAL, int = TYPE_VOID);
DllExport PVAL  AllocateValue(PGLOBAL, int, int len = 0, int prec = 2,
                              PSZ dom = NULL, PCATLG cat = NULL);

/***********************************************************************/
/*  Class VALUE represents a constant or variable of any valid type.   */
/***********************************************************************/
class DllExport VALUE : public BLOCK {
  friend class CONSTANT; // The only object allowed to use SetConstFormat
 public:
  // Constructors

  // Implementation
  virtual bool   IsTypeNum(void) = 0;
  virtual bool   IsZero(void) = 0;
  virtual bool   IsCi(void) {return false;}
  virtual void   Reset(void) = 0;
  virtual int    GetSize(void) = 0;
  virtual int    GetValLen(void) = 0;
  virtual int    GetValPrec(void) = 0;
  virtual int    GetLength(void) {return 1;}
  virtual PSZ    GetCharValue(void) {assert(false); return NULL;}
  virtual short  GetShortValue(void) {assert(false); return 0;}
  virtual int    GetIntValue(void) = 0;
  virtual longlong GetBigintValue(void) = 0;
  virtual double GetFloatValue(void) = 0;
  virtual void  *GetTo_Val(void) = 0;
          int    GetType(void) {return Type;}
          int    GetClen(void) {return Clen;}
          void   SetGlobal(PGLOBAL g) {Global = g;}

  // Methods
  virtual bool   SetValue_pval(PVAL valp, bool chktype = false) = 0;
  virtual void   SetValue_char(char *p, int n) = 0;
  virtual void   SetValue_psz(PSZ s) = 0;
  virtual void   SetValue_bool(bool b) {assert(false);}
  virtual void   SetValue(short i) {assert(false);}
  virtual void   SetValue(int n) {assert(false);}
  virtual void   SetValue(longlong n) {assert(false);}
  virtual void   SetValue(double f) {assert(false);}
  virtual void   SetValue_pvblk(PVBLK blk, int n) = 0;
  virtual void   SetBinValue(void *p) = 0;
  virtual bool   GetBinValue(void *buf, int buflen, bool go) = 0;
  virtual void   GetBinValue(void *buf, int len) = 0;
  virtual bool   IsEqual(PVAL vp, bool chktype) = 0;
  virtual int    CompareValue(PVAL vp) = 0;
  virtual BYTE   TestValue(PVAL vp);
  virtual void   Divide(int cnt) {assert(false);}
  virtual void   StdVar(PVAL vp, int cnt, bool b) {assert(false);}
  virtual void   Add(int lv) {assert(false);}
  virtual void   Add(PVAL vp) {assert(false);}
  virtual void   Add(PVBLK vbp, int i) {assert(false);}
  virtual void   Add(PVBLK vbp, int j, int k) {assert(false);}
  virtual void   Add(PVBLK vbp, int *x, int j, int k) {assert(false);}
  virtual void   AddSquare(PVAL vp) {assert(false);}
  virtual void   AddSquare(PVBLK vbp, int i) {assert(false);}
  virtual void   AddSquare(PVBLK vbp, int j, int k) {assert(false);}
  virtual void   Times(PVAL vp) {assert(false);}
  virtual void   SetMin(PVAL vp) = 0;
  virtual void   SetMin(PVBLK vbp, int i) = 0;
  virtual void   SetMin(PVBLK vbp, int j, int k) = 0;
  virtual void   SetMin(PVBLK vbp, int *x, int j, int k) = 0;
  virtual void   SetMax(PVAL vp) = 0;
  virtual void   SetMax(PVBLK vbp, int i) = 0;
  virtual void   SetMax(PVBLK vbp, int j, int k) = 0;
  virtual void   SetMax(PVBLK vbp, int *x, int j, int k) = 0;
  virtual char  *ShowValue(char *buf, int len = 0) = 0;
  virtual char  *GetCharString(char *p) = 0;
  virtual char  *GetShortString(char *p, int n) {return "#####";}
  virtual char  *GetIntString(char *p, int n) = 0;
  virtual char  *GetBigintString(char *p, int n) = 0;
  virtual char  *GetFloatString(char *p, int n, int prec) = 0;
  virtual bool   Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op) = 0;
  virtual int    GetTime(PGLOBAL g, PVAL *vp, int np) = 0;
  virtual bool   FormatValue(PVAL vp, char *fmt) = 0;
          char  *ShowTypedValue(PGLOBAL g, char *buf, int typ, int n, int p);
 protected:
  virtual bool   SetConstFormat(PGLOBAL, FORMAT&) = 0;

  // Constructor used by derived classes
  VALUE(int type) : Type(type) {}

  // Members
  PGLOBAL Global;                   // To reduce arglist
//const int   Type;                 // The value type
  int     Type;                     // The value type
  int     Clen;                     // Internal value length
  }; // end of class VALUE

/***********************************************************************/
/*  Class STRING: represents zero terminated strings.                  */
/***********************************************************************/
class STRING : public VALUE {
  friend class SFROW;
 public:
  // Constructors
  STRING(PSZ s);
  STRING(PGLOBAL g, PSZ s, int n, int c = 0);
  STRING(PGLOBAL g, short i);
  STRING(PGLOBAL g, int n);
  STRING(PGLOBAL g, longlong n);
  STRING(PGLOBAL g, double f);

  // Implementation
  virtual bool   IsTypeNum(void) {return false;}
  virtual bool   IsZero(void) {return (Strp) ? strlen(Strp) == 0 : true;}
  virtual bool   IsCi(void) {return Ci;}
  virtual void   Reset(void) {*Strp = '\0';}
  virtual int    GetValLen(void) {return Len;}
  virtual int    GetValPrec() {return (Ci) ? 1 : 0;}
  virtual int    GetLength(void) {return Len;}
  virtual int    GetSize(void) {return (Strp) ? strlen(Strp) : 0;}
  virtual PSZ    GetCharValue(void) {return Strp;}
  virtual short  GetShortValue(void) {return (short)atoi(Strp);}
  virtual int    GetIntValue(void) {return atol(Strp);}
  virtual longlong GetBigintValue(void) {return atoll(Strp);}
  virtual double GetFloatValue(void) {return atof(Strp);}
  virtual void  *GetTo_Val(void) {return Strp;}

  // Methods
  virtual bool   SetValue_pval(PVAL valp, bool chktype);
  virtual void   SetValue_char(char *p, int n);
  virtual void   SetValue_psz(PSZ s);
  virtual void   SetValue_pvblk(PVBLK blk, int n);
  virtual void   SetValue(short i);
  virtual void   SetValue(int n);
  virtual void   SetValue(longlong n);
  virtual void   SetValue(double f);
  virtual void   SetBinValue(void *p);
  virtual bool   GetBinValue(void *buf, int buflen, bool go);
  virtual void   GetBinValue(void *buf, int len);
  virtual char  *ShowValue(char *buf, int);
  virtual char  *GetCharString(char *p);
  virtual char  *GetShortString(char *p, int n);
  virtual char  *GetIntString(char *p, int n);
  virtual char  *GetBigintString(char *p, int n);
  virtual char  *GetFloatString(char *p, int n, int prec = -1);
  virtual bool   IsEqual(PVAL vp, bool chktype);
  virtual int    CompareValue(PVAL vp);
  virtual BYTE   TestValue(PVAL vp);
  virtual void   SetMin(PVAL vp);
  virtual void   SetMin(PVBLK vbp, int i);
  virtual void   SetMin(PVBLK vbp, int j, int k);
  virtual void   SetMin(PVBLK vbp, int *x, int j, int k);
  virtual void   SetMax(PVAL vp);
  virtual void   SetMax(PVBLK vbp, int i);
  virtual void   SetMax(PVBLK vbp, int j, int k);
  virtual void   SetMax(PVBLK vbp, int *x, int j, int k);
  virtual bool   SetConstFormat(PGLOBAL, FORMAT&);
  virtual bool   Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op);
  virtual int    GetTime(PGLOBAL g, PVAL *vp, int np);
  virtual bool   FormatValue(PVAL vp, char *fmt);
  virtual void   Print(PGLOBAL g, FILE *, uint);
  virtual void   Print(PGLOBAL g, char *, uint);

 protected:
  // Default constructor not to be used
  STRING(void) : VALUE(TYPE_ERROR) {}

  // Members
  PSZ     Strp;
  int     Len;
  bool    Ci;                        // true if case insensitive
  }; // end of class STRING

/***********************************************************************/
/*  Class SHVAL: represents short integer values.                      */
/***********************************************************************/
class SHVAL : public VALUE {
 public:
  // Constructors
  SHVAL(PSZ s);
  SHVAL(short n);
  SHVAL(int n);
  SHVAL(longlong n);
  SHVAL(double f);

  // Implementation
  virtual bool   IsTypeNum(void) {return true;}
  virtual bool   IsZero(void) {return Sval == 0;}
  virtual void   Reset(void) {Sval = 0;}
  virtual int    GetValLen(void);
  virtual int    GetValPrec() {return 0;}
  virtual int    GetSize(void) {return sizeof(short);}
//virtual PSZ    GetCharValue(void) {}
  virtual short  GetShortValue(void) {return Sval;}
  virtual int    GetIntValue(void) {return (int)Sval;}
  virtual longlong GetBigintValue(void) {return (longlong)Sval;}
  virtual double GetFloatValue(void) {return (double)Sval;}
  virtual void  *GetTo_Val(void) {return &Sval;}

  // Methods
  virtual bool   SetValue_pval(PVAL valp, bool chktype);
  virtual void   SetValue_char(char *p, int n);
  virtual void   SetValue_psz(PSZ s);
  virtual void   SetValue_bool(bool b) {Sval = (b) ? 1 : 0;}
  virtual void   SetValue(short i) {Sval = i;}
  virtual void   SetValue(int n) {Sval = (short)n;}
  virtual void   SetValue(longlong n) {Sval = (short)n;}
  virtual void   SetValue_pvblk(PVBLK blk, int n);
  virtual void   SetBinValue(void *p);
  virtual bool   GetBinValue(void *buf, int buflen, bool go);
  virtual void   GetBinValue(void *buf, int len);
  virtual char  *ShowValue(char *buf, int);
  virtual char  *GetCharString(char *p);
  virtual char  *GetShortString(char *p, int n);
  virtual char  *GetIntString(char *p, int n);
  virtual char  *GetBigintString(char *p, int n);
  virtual char  *GetFloatString(char *p, int n, int prec = -1);
  virtual bool   IsEqual(PVAL vp, bool chktype);
  virtual int    CompareValue(PVAL vp);
  virtual void   Divide(int cnt);
  virtual void   StdVar(PVAL vp, int cnt, bool b);
  virtual void   Add(int lv) {Sval += (short)lv;}
  virtual void   Add(PVAL vp);
  virtual void   Add(PVBLK vbp, int i);
  virtual void   Add(PVBLK vbp, int j, int k);
  virtual void   Add(PVBLK vbp, int *x, int j, int k);
  virtual void   AddSquare(PVAL vp);
  virtual void   AddSquare(PVBLK vbp, int i);
  virtual void   AddSquare(PVBLK vbp, int j, int k);
  virtual void   Times(PVAL vp);
  virtual void   SetMin(PVAL vp);
  virtual void   SetMin(PVBLK vbp, int i);
  virtual void   SetMin(PVBLK vbp, int j, int k);
  virtual void   SetMin(PVBLK vbp, int *x, int j, int k);
  virtual void   SetMax(PVAL vp);
  virtual void   SetMax(PVBLK vbp, int i);
  virtual void   SetMax(PVBLK vbp, int j, int k);
  virtual void   SetMax(PVBLK vbp, int *x, int j, int k);
  virtual bool   SetConstFormat(PGLOBAL, FORMAT&);
  virtual bool   Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op);
  virtual int    GetTime(PGLOBAL g, PVAL *vp, int np) {return 0;}
  virtual bool   FormatValue(PVAL vp, char *fmt);
  virtual void   Print(PGLOBAL g, FILE *, uint);
  virtual void   Print(PGLOBAL g, char *, uint);

 protected:
          short  SafeAdd(short n1, short n2);
          short  SafeMult(short n1, short n2);
  // Default constructor not to be used
  SHVAL(void) : VALUE(TYPE_ERROR) {}

  // Members
  short   Sval;
  }; // end of class SHVAL

/***********************************************************************/
/*  Class INTVAL: represents int integer values.                       */
/***********************************************************************/
class DllExport INTVAL : public VALUE {
 public:
  // Constructors
  INTVAL(PSZ s);
  INTVAL(short i);
  INTVAL(int n);
  INTVAL(longlong n);
  INTVAL(double f);

  // Implementation
  virtual bool   IsTypeNum(void) {return true;}
  virtual bool   IsZero(void) {return Ival == 0;}
  virtual void   Reset(void) {Ival = 0;}
  virtual int    GetValLen(void);
  virtual int    GetValPrec() {return 0;}
  virtual int    GetSize(void) {return sizeof(int);}
//virtual PSZ    GetCharValue(void) {}
  virtual short  GetShortValue(void) {return (short)Ival;}
  virtual int    GetIntValue(void) {return Ival;}
  virtual longlong GetBigintValue(void) {return (longlong)Ival;}
  virtual double GetFloatValue(void) {return (double)Ival;}
  virtual void  *GetTo_Val(void) {return &Ival;}

  // Methods
  virtual bool   SetValue_pval(PVAL valp, bool chktype);
  virtual void   SetValue_char(char *p, int n);
  virtual void   SetValue_psz(PSZ s);
  virtual void   SetValue_bool(bool b) {Ival = (b) ? 1 : 0;}
  virtual void   SetValue(short i) {Ival = (int)i;}
  virtual void   SetValue(int n) {Ival = n;}
  virtual void   SetValue(longlong n) {Ival = (int)n;}
  virtual void   SetValue(double f) {Ival = (int)f;}
  virtual void   SetValue_pvblk(PVBLK blk, int n);
  virtual void   SetBinValue(void *p);
  virtual bool   GetBinValue(void *buf, int buflen, bool go);
  virtual void   GetBinValue(void *buf, int len);
  virtual char  *ShowValue(char *buf, int);
  virtual char  *GetCharString(char *p);
  virtual char  *GetShortString(char *p, int n);
  virtual char  *GetIntString(char *p, int n);
  virtual char  *GetBigintString(char *p, int n);
  virtual char  *GetFloatString(char *p, int n, int prec = -1);
  virtual bool   IsEqual(PVAL vp, bool chktype);
  virtual int    CompareValue(PVAL vp);
  virtual void   Divide(int cnt);
  virtual void   StdVar(PVAL vp, int cnt, bool b);
  virtual void   Add(int lv) {Ival += lv;}
  virtual void   Add(PVAL vp);
  virtual void   Add(PVBLK vbp, int i);
  virtual void   Add(PVBLK vbp, int j, int k);
  virtual void   Add(PVBLK vbp, int *x, int j, int k);
  virtual void   AddSquare(PVAL vp);
  virtual void   AddSquare(PVBLK vbp, int i);
  virtual void   AddSquare(PVBLK vbp, int j, int k);
  virtual void   Times(PVAL vp);
  virtual void   SetMin(PVAL vp);
  virtual void   SetMin(PVBLK vbp, int i);
  virtual void   SetMin(PVBLK vbp, int j, int k);
  virtual void   SetMin(PVBLK vbp, int *x, int j, int k);
  virtual void   SetMax(PVAL vp);
  virtual void   SetMax(PVBLK vbp, int i);
  virtual void   SetMax(PVBLK vbp, int j, int k);
  virtual void   SetMax(PVBLK vbp, int *x, int j, int k);
  virtual bool   SetConstFormat(PGLOBAL, FORMAT&);
  virtual bool   Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op);
  virtual int    GetTime(PGLOBAL g, PVAL *vp, int np);
  virtual bool   FormatValue(PVAL vp, char *fmt);
  virtual void   Print(PGLOBAL g, FILE *, uint);
  virtual void   Print(PGLOBAL g, char *, uint);

 protected:
          int   SafeAdd(int n1, int n2);
          int   SafeMult(int n1, int n2);
  // Default constructor not to be used
  INTVAL(void) : VALUE(TYPE_ERROR) {}

  // Members
  int    Ival;
  }; // end of class INTVAL

/***********************************************************************/
/*  Class DTVAL: represents a time stamp value.                        */
/***********************************************************************/
class DllExport DTVAL : public INTVAL {
 public:
  // Constructors
  DTVAL(PGLOBAL g, int n, int p, PSZ fmt);
  DTVAL(PGLOBAL g, PSZ s, int n);
  DTVAL(PGLOBAL g, short i);
  DTVAL(PGLOBAL g, int n);
  DTVAL(PGLOBAL g, longlong n);
  DTVAL(PGLOBAL g, double f);

  // Implementation
  virtual bool   SetValue_pval(PVAL valp, bool chktype);
  virtual void   SetValue_char(char *p, int n);
  virtual void   SetValue_psz(PSZ s);
  virtual void   SetValue_pvblk(PVBLK blk, int n);
  virtual char  *GetCharString(char *p);
  virtual char  *ShowValue(char *buf, int);
  virtual bool   Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op);
  virtual int    GetTime(PGLOBAL g, PVAL *vp, int np);
  virtual bool   FormatValue(PVAL vp, char *fmt);
          bool   SetFormat(PGLOBAL g, PSZ fmt, int len, int year = 0);
          bool   SetFormat(PGLOBAL g, PVAL valp);
          bool   IsFormatted(void) {return Pdtp != NULL;}
          bool   GetTmMember(OPVAL op, int& mval);
          bool   DateDiff(DTVAL *dtp, OPVAL op, int& tdif);
          bool   MakeTime(struct tm *ptm);
  static  void   SetTimeShift(void);
  static  int    GetShift(void) {return Shift;}

  // Methods
          bool   MakeDate(PGLOBAL g, int *val, int nval);
          bool   WeekNum(PGLOBAL g, int& nval);

  struct  tm    *GetGmTime(void);

 protected:
  // Default constructor not to be used
  DTVAL(void) : INTVAL() {}

  // Members
  static int    Shift;        // Time zone shift in seconds
  PDTP          Pdtp;         // To the DATPAR structure
  char         *Sdate;        // Utility char buffer
//struct tm    *DateTime;     // Utility (not used yet)
  int           DefYear;      // Used by ExtractDate
  int           Len;          // Used by CHAR scalar function
  }; // end of class DTVAL

/***********************************************************************/
/*  Class BIGVAL: represents bigint integer values.                    */
/***********************************************************************/
class DllExport BIGVAL : public VALUE {
 public:
  // Constructors
  BIGVAL(PSZ s);
  BIGVAL(short i);
  BIGVAL(int n);
  BIGVAL(longlong n);
  BIGVAL(double f);

  // Implementation
  virtual bool   IsTypeNum(void) {return true;}
  virtual bool   IsZero(void) {return Lval == 0LL;}
  virtual void   Reset(void) {Lval = 0LL;}
  virtual int    GetValLen(void);
  virtual int    GetValPrec() {return 0;}
  virtual int    GetSize(void) {return sizeof(longlong);}
//virtual PSZ    GetCharValue(void) {}
  virtual short  GetShortValue(void) {return (short)Lval;}
  virtual int    GetIntValue(void) {return (int)Lval;}
  virtual longlong GetBigintValue(void) {return Lval;}
  virtual double GetFloatValue(void) {return (double)Lval;}
  virtual void  *GetTo_Val(void) {return &Lval;}

  // Methods
  virtual bool   SetValue_pval(PVAL valp, bool chktype);
  virtual void   SetValue_char(char *p, int n);
  virtual void   SetValue_psz(PSZ s);
  virtual void   SetValue_bool(bool b) {Lval = (b) ? 1LL : 0LL;}
  virtual void   SetValue(short i) {Lval = (longlong)i;}
  virtual void   SetValue(int n) {Lval = (longlong)n;}
  virtual void   SetValue(longlong n) {Lval = n;}
  virtual void   SetValue(double f) {Lval = (longlong)f;}
  virtual void   SetValue_pvblk(PVBLK blk, int n);
  virtual void   SetBinValue(void *p);
  virtual bool   GetBinValue(void *buf, int buflen, bool go);
  virtual void   GetBinValue(void *buf, int len);
  virtual char  *ShowValue(char *buf, int);
  virtual char  *GetCharString(char *p);
  virtual char  *GetShortString(char *p, int n);
  virtual char  *GetIntString(char *p, int n);
  virtual char  *GetBigintString(char *p, int n);
  virtual char  *GetFloatString(char *p, int n, int prec = -1);
  virtual bool   IsEqual(PVAL vp, bool chktype);
  virtual int    CompareValue(PVAL vp);
  virtual void   Divide(int cnt);
  virtual void   StdVar(PVAL vp, int cnt, bool b);
  virtual void   Add(int lv) {Lval += (longlong)lv;}
  virtual void   Add(PVAL vp);
  virtual void   Add(PVBLK vbp, int i);
  virtual void   Add(PVBLK vbp, int j, int k);
  virtual void   Add(PVBLK vbp, int *x, int j, int k);
  virtual void   AddSquare(PVAL vp);
  virtual void   AddSquare(PVBLK vbp, int i);
  virtual void   AddSquare(PVBLK vbp, int j, int k);
  virtual void   Times(PVAL vp);
  virtual void   SetMin(PVAL vp);
  virtual void   SetMin(PVBLK vbp, int i);
  virtual void   SetMin(PVBLK vbp, int j, int k);
  virtual void   SetMin(PVBLK vbp, int *x, int j, int k);
  virtual void   SetMax(PVAL vp);
  virtual void   SetMax(PVBLK vbp, int i);
  virtual void   SetMax(PVBLK vbp, int j, int k);
  virtual void   SetMax(PVBLK vbp, int *x, int j, int k);
  virtual bool   SetConstFormat(PGLOBAL, FORMAT&);
  virtual bool   Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op);
  virtual int    GetTime(PGLOBAL g, PVAL *vp, int np) {return 0;}
  virtual bool   FormatValue(PVAL vp, char *fmt);
  virtual void   Print(PGLOBAL g, FILE *, uint);
  virtual void   Print(PGLOBAL g, char *, uint);

 protected:
          longlong SafeAdd(longlong n1, longlong n2);
          longlong SafeMult(longlong n1, longlong n2);
  // Default constructor not to be used
  BIGVAL(void) : VALUE(TYPE_ERROR) {}

  // Members
  longlong Lval;
  }; // end of class BIGVAL

/***********************************************************************/
/*  Class DFVAL: represents double float values.                       */
/***********************************************************************/
class DFVAL : public VALUE {
 public:
  // Constructors
  DFVAL(PSZ s, int prec = 2);
  DFVAL(short i, int prec = 2);
  DFVAL(int n, int prec = 2);
  DFVAL(longlong n, int prec = 2);
  DFVAL(double f, int prec = 2);

  // Implementation
  virtual bool   IsTypeNum(void) {return true;}
  virtual bool   IsZero(void) {return Fval == 0.0;}
  virtual void   Reset(void) {Fval = 0.0;}
  virtual int    GetValLen(void);
  virtual int    GetValPrec() {return Prec;}
  virtual int    GetSize(void) {return sizeof(double);}
//virtual PSZ    GetCharValue(void) {}
  virtual short  GetShortValue(void) {return (short)Fval;}
  virtual int    GetIntValue(void) {return (int)Fval;}
  virtual longlong GetBigintValue(void) {return (longlong)Fval;}
  virtual double GetFloatValue(void) {return Fval;}
  virtual void  *GetTo_Val(void) {return &Fval;}
          void   SetPrec(int prec) {Prec = prec;}

  // Methods
  virtual bool   SetValue_pval(PVAL valp, bool chktype);
  virtual void   SetValue_char(char *p, int n);
  virtual void   SetValue_psz(PSZ s);
  virtual void   SetValue(short i) {Fval = (double)i;}
  virtual void   SetValue(int n) {Fval = (double)n;}
  virtual void   SetValue(longlong n) {Fval = (double)n;}
  virtual void   SetValue(double f) {Fval = f;}
  virtual void   SetValue_pvblk(PVBLK blk, int n);
  virtual void   SetBinValue(void *p);
  virtual bool   GetBinValue(void *buf, int buflen, bool go);
  virtual void   GetBinValue(void *buf, int len);
  virtual char  *ShowValue(char *buf, int);
  virtual char  *GetCharString(char *p);
  virtual char  *GetShortString(char *p, int n);
  virtual char  *GetIntString(char *p, int n);
  virtual char  *GetBigintString(char *p, int n);
  virtual char  *GetFloatString(char *p, int n, int prec = -1);
  virtual bool   IsEqual(PVAL vp, bool chktype);
  virtual int    CompareValue(PVAL vp);
  virtual void   Divide(int cnt);
  virtual void   StdVar(PVAL vp, int cnt, bool b);
  virtual void   Add(PVAL vp);
  virtual void   Add(PVBLK vbp, int i);
  virtual void   Add(PVBLK vbp, int j, int k);
  virtual void   Add(PVBLK vbp, int *x, int j, int k);
  virtual void   AddSquare(PVAL vp);
  virtual void   AddSquare(PVBLK vbp, int i);
  virtual void   AddSquare(PVBLK vbp, int j, int k);
  virtual void   Times(PVAL vp);
  virtual void   SetMin(PVAL vp);
  virtual void   SetMin(PVBLK vbp, int i);
  virtual void   SetMin(PVBLK vbp, int j, int k);
  virtual void   SetMin(PVBLK vbp, int *x, int j, int k);
  virtual void   SetMax(PVAL vp);
  virtual void   SetMax(PVBLK vbp, int i);
  virtual void   SetMax(PVBLK vbp, int j, int k);
  virtual void   SetMax(PVBLK vbp, int *x, int j, int k);
  virtual bool   SetConstFormat(PGLOBAL, FORMAT&);
  virtual bool   Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op);
  virtual int    GetTime(PGLOBAL g, PVAL *vp, int np);
  virtual bool   FormatValue(PVAL vp, char *fmt);
  virtual void   Print(PGLOBAL g, FILE *, uint);
  virtual void   Print(PGLOBAL g, char *, uint);

  // Specific function
          void   Divide(double div) {Fval /= div;}

 protected:
  // Default constructor not to be used
  DFVAL(void) : VALUE(TYPE_ERROR) {}

  // Members
  double  Fval;
  int     Prec;
  }; // end of class DFVAL

#endif
