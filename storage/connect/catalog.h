/*************** Catalog H Declares Source Code File (.H) **************/
/*  Name: CATALOG.H  Version 3.3                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2015    */
/*                                                                     */
/*  This file contains the CATALOG PlugDB classes definitions.         */
/***********************************************************************/
#ifndef __CATALOG__H
#define  __CATALOG__H

#include "block.h"

/***********************************************************************/
/*  Defines the length of a buffer to contain entire table section.    */
/***********************************************************************/
#define PLG_MAX_PATH    144   /* Must be the same across systems       */
#define PLG_BUFF_LEN    100   /* Number of lines in binary file buffer */


//typedef class INDEXDEF *PIXDEF;

/***********************************************************************/
/*  Defines the structure used to enumerate tables or views.           */
/***********************************************************************/
typedef struct _curtab {
  PRELDEF CurTdb;
  char   *Curp;
  char   *Tabpat;
  bool    Ispat;
  bool    NoView;
  int     Nt;
  char   *Type[16];
  } CURTAB, *PCURTAB;

/***********************************************************************/
/*  Defines the structure used to get column catalog info.             */
/***********************************************************************/
typedef struct _colinfo {
  char  *Name;
  int    Type;
  int    Offset;
  int    Length;
  int    Key;
  int    Precision;
  int    Scale;
  int    Opt;
  int    Freq;
  char  *Remark;
  char  *Datefmt;
  char  *Fieldfmt;
  ushort Flags;         // Used by MariaDB CONNECT handlers
  } COLINFO, *PCOLINFO;

/***********************************************************************/
/*  CATALOG: base class for catalog classes.                           */
/***********************************************************************/
class DllExport CATALOG {
  friend class RELDEF;
  friend class TABDEF;
  friend class DIRDEF;
  friend class OEMDEF;
 public:
  CATALOG(void);                       // Constructor
  virtual ~CATALOG() { }               // Make -Wdelete-non-virtual-dtor happy

  // Implementation
  int     GetCblen(void) {return Cblen;}
  bool    GetDefHuge(void) {return DefHuge;}
  void    SetDefHuge(bool b) {DefHuge = b;}
  char   *GetCbuf(void) {return Cbuf;}
//char   *GetDataPath(void) {return (char*)DataPath;}

  // Methods
  virtual void    Reset(void) {}
//virtual void    SetDataPath(PGLOBAL g, const char *path) {}
  virtual bool    CheckName(PGLOBAL, char*) {return true;}
  virtual bool    ClearName(PGLOBAL, PSZ) {return true;}
  virtual PRELDEF MakeOneTableDesc(PGLOBAL, LPCSTR, LPCSTR) {return NULL;}
  virtual PRELDEF GetTableDescEx(PGLOBAL, PTABLE) {return NULL;}
  //virtual PRELDEF GetTableDesc(PGLOBAL, LPCSTR, LPCSTR,
  //                                      PRELDEF* = NULL) {return NULL;}
  virtual PRELDEF GetFirstTable(PGLOBAL) {return NULL;}
  virtual PRELDEF GetNextTable(PGLOBAL) {return NULL;}
  virtual bool    TestCond(PGLOBAL, const char*, const char*) {return true;}
  virtual bool    DropTable(PGLOBAL, PSZ, bool) {return true;}
  virtual PTDB    GetTable(PGLOBAL, PTABLE,
                           MODE = MODE_READ, LPCSTR = NULL) {return NULL;}
  virtual void    TableNames(PGLOBAL, char*, int, int[]) {}
  virtual void    ColumnNames(PGLOBAL, char*, char*, int, int[]) {}
  virtual void    ColumnDefs(PGLOBAL, char*, char*, int, int[]) {}
  virtual void   *DecodeValues(PGLOBAL, char*, char*, char*,
                                        int, int[]) {return NULL;}
  virtual int     ColumnType(PGLOBAL, char*, char*) {return 0;}
  virtual void    ClearDB(PGLOBAL) {}

 protected:
  virtual bool    ClearSection(PGLOBAL, const char*, const char*) {return true;}
  //virtual PRELDEF MakeTableDesc(PGLOBAL, LPCSTR, LPCSTR) {return NULL;}

  // Members
  char   *Cbuf;                        /* Buffer used for col section  */
  int     Cblen;                       /* Length of suballoc. buffer   */
  CURTAB  Ctb;                         /* Used to enumerate tables     */
  bool    DefHuge;                     /* true: tables default to huge */
//LPCSTR  DataPath;                    /* Is the Path of DB data dir   */
  }; // end of class CATALOG

#endif // __CATALOG__H
