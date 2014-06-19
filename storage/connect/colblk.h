/*************** Colblk H Declares Source Code File (.H) ***************/
/*  Name: COLBLK.H    Version 1.7                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2014    */
/*                                                                     */
/*  This file contains the COLBLK and derived classes declares.        */
/***********************************************************************/
#ifndef __COLBLK__H
#define  __COLBLK__H

/***********************************************************************/
/*  Include required application header files                          */
/***********************************************************************/
#include "xobject.h"
#include "reldef.h"

/***********************************************************************/
/*  Class COLBLK: Base class for table column descriptors.             */
/***********************************************************************/
class DllExport COLBLK : public XOBJECT {
  friend class TDBPIVOT;
 protected:
  // Default constructors used by derived classes
  COLBLK(PCOLDEF cdp = NULL, PTDB tdbp = NULL, int i = 0);
  COLBLK(PCOL colp, PTDB tdbp = NULL);     // Used in copy process
  COLBLK(int n) {}     // Used when changing a column class in TDBXML

 public:
  // Implementation
  virtual int     GetType(void) {return TYPE_COLBLK;}
  virtual int     GetResultType(void) {return Buf_Type;}
  virtual int     GetScale(void) {return Format.Prec;}
  virtual int     GetPrecision(void) {return Precision;}
  virtual int     GetLength(void) {return Long;}
  virtual int     GetLengthEx(void);
  virtual int     GetAmType() {return TYPE_AM_ERROR;}
  virtual void    SetOk(void) {Status |= BUF_EMPTY;}
  virtual PTDB    GetTo_Tdb(void) {return To_Tdb;}
          PCOL    GetNext(void) {return Next;}
          PSZ     GetName(void) {return Name;}
          int     GetIndex(void) {return Index;}
          ushort  GetColUse(void) {return ColUse;}
          ushort  GetColUse(ushort u) {return (ColUse & u);}
          ushort  GetStatus(void) {return Status;}
          ushort  GetStatus(ushort u) {return (Status & u);}
          void    SetColUse(ushort u) {ColUse = u;}
          void    SetStatus(ushort u) {Status = u;}
          void    AddColUse(ushort u) {ColUse |= u;}
          void    AddStatus(ushort u) {Status |= u;}
          void    SetNext(PCOL cp) {Next = cp;}
          void    SetKcol(PXCOL kcp) {To_Kcol = kcp;}
          PCOLDEF GetCdp(void) {return Cdp;}
          PSZ     GetDomain(void) {return (Cdp) ? Cdp->Decode : NULL;}
          PSZ     GetDesc(void) {return (Cdp) ? Cdp->Desc : NULL;}
          PSZ     GetFmt(void) {return (Cdp) ? Cdp->Fmt : NULL;}
          bool    IsUnsigned(void) {return Unsigned;}
          bool    IsNullable(void) {return Nullable;}
          bool    IsVirtual(void) {return Cdp->IsVirtual();}
          void    SetNullable(bool b) {Nullable = b;}
                  
  // Methods      
  virtual void    Reset(void);
  virtual bool    Compare(PXOB xp);
  virtual bool    SetFormat(PGLOBAL, FORMAT&);
  virtual bool    IsSpecial(void) {return false;}
  virtual bool    Eval(PGLOBAL g);
  virtual bool    SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
  virtual void    SetTo_Val(PVAL valp) {}
  virtual void    ReadColumn(PGLOBAL g);
  virtual void    WriteColumn(PGLOBAL g);
  virtual void    Print(PGLOBAL g, FILE *, uint);
  virtual void    Print(PGLOBAL g, char *, uint);
          bool    InitValue(PGLOBAL g);

 protected:
  // Members
  PCOL    Next;                // Next column in table
  PSZ     Name;                // Column name
  PCOLDEF Cdp;                 // To column definition block
  PTDB    To_Tdb;              // Points to Table Descriptor Block
  PXCOL   To_Kcol;             // Points to Xindex matching column
  bool    Nullable;            // True if nullable
  bool    Unsigned;            // True if unsigned
  int     Index;               // Column number in table
  int     Buf_Type;            // Data type
  int     Long;                // Internal length in table
  int     Precision;           // Column length (as for ODBC)
  FORMAT  Format;              // Output format
  ushort  ColUse;              // Column usage
  ushort  Status;              // Column read status
  }; // end of class COLBLK

/***********************************************************************/
/*  Class SPCBLK: Base class for special column descriptors.           */
/***********************************************************************/
class DllExport SPCBLK : public COLBLK {
 public:
  // Constructor
  SPCBLK(PCOLUMN cp);

  // Implementation
  virtual int  GetAmType(void) = 0;
  virtual  bool GetRnm(void) {return false;}

  // Methods
  virtual bool IsSpecial(void) {return true;}
  virtual void ReadColumn(PGLOBAL g) = 0;
  virtual void WriteColumn(PGLOBAL g);

 protected:
  // Default constructor not to be used
  SPCBLK(void) : COLBLK(1) {}
  }; // end of class SPCBLK

/***********************************************************************/
/*  Class RIDBLK: ROWID special column descriptor.                     */
/***********************************************************************/
class DllExport RIDBLK : public SPCBLK {
 public:
  // Constructor
  RIDBLK(PCOLUMN cp, bool rnm);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_ROWID;}
  virtual  bool GetRnm(void) {return Rnm;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);

 protected:
  bool Rnm;                         // False for RowID, True for RowNum
  }; // end of class RIDBLK

/***********************************************************************/
/*  Class FIDBLK: FILEID special column descriptor.                    */
/***********************************************************************/
class DllExport FIDBLK : public SPCBLK {
 public:
  // Constructor
  FIDBLK(PCOLUMN cp);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_FILID;}

  // Methods
  virtual void Reset(void) {}       // This is a pseudo constant column
  virtual void ReadColumn(PGLOBAL g);

 protected:
  PSZ  Fn;                         // The current To_File of the table
  }; // end of class FIDBLK

/***********************************************************************/
/*  Class TIDBLK: TABID special column descriptor.                     */
/***********************************************************************/
class DllExport TIDBLK : public SPCBLK {
 public:
  // Constructor
  TIDBLK(PCOLUMN cp);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_TABID;}

  // Methods
  virtual void Reset(void) {}       // This is a pseudo constant column
  virtual void ReadColumn(PGLOBAL g);

 protected:
  // Default constructor not to be used
  TIDBLK(void) {}

  // Members
  PSZ  Tname;                       // The current table name
  }; // end of class TIDBLK

/***********************************************************************/
/*  Class SIDBLK: SERVID special column descriptor.                    */
/***********************************************************************/
class DllExport SIDBLK : public SPCBLK {
 public:
  // Constructor
  SIDBLK(PCOLUMN cp);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_SRVID;}

  // Methods
  virtual void Reset(void) {}       // This is a pseudo constant column
  virtual void ReadColumn(PGLOBAL g);

 protected:
  // Default constructor not to be used
  SIDBLK(void) {}

  // Members
  PSZ  Sname;                       // The current server name
  }; // end of class SIDBLK

#endif // __COLBLK__H
