/*************** RelDef H Declares Source Code File (.H) ***************/
/*  Name: RELDEF.H  Version 1.6                                        */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2004-2016    */
/*                                                                     */
/*  This file contains the DEF classes definitions.                    */
/***********************************************************************/

#ifndef __RELDEF_H
#define __RELDEF_H

#include "block.h"
#include "catalog.h"
//#include "my_sys.h"
#include "mycat.h"

typedef class  INDEXDEF *PIXDEF;
typedef class  ha_connect *PHC;

/***********************************************************************/
/*  Table or View (relation) definition block.                         */
/***********************************************************************/
class DllExport RELDEF : public BLOCK {      // Relation definition block
  friend class CATALOG;
  friend class PLUGCAT;
  friend class MYCAT;
 public:
  RELDEF(void);                        // Constructor

  // Implementation
  PRELDEF GetNext(void) {return Next;}
  PSZ     GetName(void) {return Name;}
  PSZ     GetDB(void) {return (PSZ)Database;}
  PCOLDEF GetCols(void) {return To_Cols;}
  PHC     GetHandler(void) {return Hc;}
  void    SetCols(PCOLDEF pcd) {To_Cols = pcd;}
  PCATLG  GetCat(void) {return Cat;}
  virtual const char *GetType(void) = 0;
  virtual AMT  GetDefType(void) = 0;
  void    SetName(const char *str) { Name=(char*)str; }
  void    SetCat(PCATLG cat) { Cat=cat; }

  // Methods
  PTOS    GetTopt(void);
  bool    GetBoolCatInfo(PCSZ what, bool bdef);
  bool    SetIntCatInfo(PCSZ what, int ival);
  bool    Partitioned(void);
  int     GetIntCatInfo(PCSZ what, int idef);
  int     GetSizeCatInfo(PCSZ what, PCSZ sdef);
  int     GetCharCatInfo(PCSZ what, PCSZ sdef, char *buf, int size);
  char   *GetStringCatInfo(PGLOBAL g, PCSZ what, PCSZ sdef);
  virtual int  Indexable(void) {return 0;}
  virtual bool Define(PGLOBAL g, PCATLG cat, 
		                  LPCSTR name, LPCSTR schema, LPCSTR am) = 0;
  virtual PTDB GetTable(PGLOBAL g, MODE mode) = 0;

 protected:
  PRELDEF Next;                        /* To next definition block     */
  PSZ     Name;                        /* Name of the view             */
  LPCSTR  Database;                    /* Table database               */
  PCOLDEF To_Cols;                     /* To a list of column desc     */
  PCATLG  Cat;                         /* To DB catalog info           */
  PHC     Hc;                          /* The Connect handler          */
  }; // end of RELDEF

/***********************************************************************/
/*  This class corresponds to the data base description for tables     */
/*  of type DOS, FIX, CSV, DBF, BIN, VCT, JSON, XML...                 */
/***********************************************************************/
class DllExport TABDEF : public RELDEF {   /* Logical table descriptor */
  friend class CATALOG;
  friend class PLUGCAT;
  friend class MYCAT;
  friend class TDB;
	friend class TDBEXT;
public:
  // Constructor
  TABDEF(void);                  // Constructor

  // Implementation
  int     GetDegree(void) {return Degree;}
  void    SetDegree(int d) {Degree = d;}
  int     GetElemt(void) {return Elemt;}
  void    SetNext(PTABDEF tdfp) {Next = tdfp;}
  int     GetMultiple(void) {return Multiple;}
  int     GetPseudo(void) {return Pseudo;}
	RECFM   GetRecfm(void) {return Recfm;}
	PCSZ    GetPath(void);
//PSZ     GetPath(void)
//          {return (Database) ? (PSZ)Database : Cat->GetDataPath();}
	RECFM   GetTableFormat(const char* type);
	bool    SepIndex(void) {return GetBoolCatInfo("SepIndex", false);}
  bool    IsReadOnly(void) {return Read_Only;}
  virtual AMT    GetDefType(void) {return TYPE_AM_TAB;}
  virtual PIXDEF GetIndx(void) {return NULL;}
  virtual void   SetIndx(PIXDEF) {}
  virtual bool   IsHuge(void) {return false;}
  const CHARSET_INFO *data_charset() {return m_data_charset;}
	const   char  *GetCsName(void) {return csname;}

  // Methods
          int  GetColCatInfo(PGLOBAL g);
          void SetIndexInfo(void);
          bool DropTable(PGLOBAL g, PSZ name);
	virtual bool Define(PGLOBAL g, PCATLG cat,
						          LPCSTR name, LPCSTR schema, LPCSTR am);
	virtual bool DefineAM(PGLOBAL, LPCSTR, int) = 0;

 protected:
  // Members
  PCSZ    Schema;               /* Table schema (for ODBC)             */
  PCSZ    Desc;                 /* Table description                   */
	RECFM   Recfm;                /* File or table format                */
	uint    Catfunc;              /* Catalog function ID                 */
  int     Card;                 /* (max) number of rows in table       */
  int     Elemt;                /* Number of rows in blocks or rowset  */
  int     Sort;                 /* Table already sorted ???            */
  int     Multiple;             /* 0: No 1: DIR 2: Section 3: filelist */
  int     Degree;               /* Number of columns in the table      */
  int     Pseudo;               /* Bit: 1 ROWID }Ok, 2 FILEID Ok       */
  bool    Read_Only;            /* true for read only tables           */
  const CHARSET_INFO *m_data_charset;
  const char *csname;           /* Table charset name                  */
}; // end of TABDEF

/***********************************************************************/
/*  Externally defined OEM tables.                                     */
/***********************************************************************/
class DllExport OEMDEF : public TABDEF {                  /* OEM table */
  friend class CATALOG;
  friend class PLUGCAT;
  friend class MYCAT;
 public:
  // Constructor
  OEMDEF(void) {Hdll = NULL; Pxdef = NULL; Module = Subtype = NULL;}

  // Implementation
  virtual const char *GetType(void) {return "OEM";}
  virtual AMT  GetDefType(void) {return TYPE_AM_OEM;}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE mode);

 protected:
  PTABDEF GetXdef(PGLOBAL g);

  // Members
#if defined(_WIN32)
  HANDLE  Hdll;               /* Handle to the external DLL            */
#else   // !_WIN32
  void   *Hdll;               /* Handle for the loaded shared library  */
#endif  // !_WIN32
  PTABDEF Pxdef;              /* Pointer to the external TABDEF class  */
  char   *Module;             /* Path/Name of the DLL implenting it    */
  char   *Subtype;            /* The name of the OEM table sub type    */
  }; // end of OEMDEF

/***********************************************************************/
/*  Column definition block used during creation.                      */
/***********************************************************************/
class DllExport COLCRT : public BLOCK { /* Column description block              */
  friend class TABDEF;
 public:
  COLCRT(PSZ name);           // Constructor
  COLCRT(void);               // Constructor (for views)

  // Implementation
  PSZ  GetName(void) {return Name;}
  PSZ  GetDecode(void) {return Decode;}
  PSZ  GetFmt(void) {return Fmt;}
  int  GetOpt(void) {return Opt;}
  int  GetFreq(void) {return Freq;}
  int  GetLong(void) {return Long;}
  int  GetPrecision(void) {return Precision;}
  int  GetOffset(void) {return Offset;}
  void SetOffset(int offset) {Offset = offset;}

 protected:
  PCOLCRT Next;               /* To next block                         */
  PSZ     Name;               /* Column name                           */
  PSZ     Desc;               /* Column description                    */
  PSZ     Decode;             /* Date format                           */
  PSZ     Fmt;                /* Input format for formatted files      */
  int     Offset;             /* Offset of field within record         */
  int     Long;               /* Length of field in file record (!BIN) */
  int     Key;                /* Key (greater than 1 if multiple)      */
  int     Precision;          /* Logical column length                 */
  int     Scale;              /* Decimals for float/decimal values     */
  int     Opt;                /* 0:Not 1:clustered 2:sorted-asc 3:desc */
  int     Freq;               /* Estimated number of different values  */
  char    DataType;           /* Internal data type (C, N, F, T)       */
  }; // end of COLCRT

/***********************************************************************/
/*  Column definition block.                                           */
/***********************************************************************/
class DllExport COLDEF : public COLCRT { /* Column description block   */
  friend class TABDEF;
  friend class COLBLK;
  friend class DBFFAM;
	friend class TDB;
	friend class TDBASE;
	friend class TDBDOS;
public:
  COLDEF(void);                // Constructor

  // Implementation
  PCOLDEF GetNext(void) {return (PCOLDEF)Next;}
  void    SetNext(PCOLDEF pcdf) {Next = pcdf;}
  int     GetLength(void) {return (int)F.Length;}
  int     GetClen(void) {return Clen;}
  int     GetType(void) {return Buf_Type;}
  int     GetPoff(void) {return Poff;}
  void   *GetMin(void) {return To_Min;}
  void    SetMin(void *minp) {To_Min = minp;}
  void   *GetMax(void) {return To_Max;}
  void    SetMax(void *maxp) {To_Max = maxp;}
  bool    GetXdb2(void) {return Xdb2;}
  void    SetXdb2(bool b) {Xdb2 = b;}
  void   *GetBmap(void) {return To_Bmap;}
  void    SetBmap(void *bmp) {To_Bmap = bmp;}
  void   *GetDval(void) {return To_Dval;}
  void    SetDval(void *dvp) {To_Dval = dvp;}
  int     GetNdv(void) {return Ndv;}
  void    SetNdv(int ndv) {Ndv = ndv;}
  int     GetNbm(void) {return Nbm;}
  void    SetNbm(int nbm) {Nbm = nbm;}
  int     Define(PGLOBAL g, void *memp, PCOLINFO cfp, int poff);
  void    Define(PGLOBAL g, PCOL colp);
  bool    IsSpecial(void) {return (Flags & U_SPECIAL) ? true : false;} 
  bool    IsVirtual(void) {return (Flags & U_VIRTUAL) ? true : false;} 

 protected:
  void   *To_Min;              /* Point to array of block min values   */
  void   *To_Max;              /* Point to array of block max values   */
  int    *To_Pos;              /* Point to array of block positions    */
  bool    Xdb2;                /* TRUE if to be optimized by XDB2      */
  void   *To_Bmap;             /* To array of block bitmap values      */
  void   *To_Dval;             /* To array of column distinct values   */
  int     Ndv;                 /* Number of distinct values            */
  int     Nbm;                 /* Number of ULONG in bitmap (XDB2)     */
  int     Buf_Type;            /* Internal data type                   */
  int     Clen;                /* Internal data size in chars (bytes)  */
  int     Poff;                /* Calculated offset for Packed tables  */
  FORMAT  F;                   /* Output format (should be in COLCRT)  */
  ushort  Flags;               /* Used by MariaDB CONNECT handler      */
  }; // end of COLDEF

#endif // __RELDEF_H

