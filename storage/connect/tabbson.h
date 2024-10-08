/*************** tabbson H Declares Source Code File (.H) **************/
/*  Name: tabbson.h   Version 1.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2020 - 2021  */
/*                                                                     */
/*  This file contains the BSON classes declares.                      */
/***********************************************************************/
#pragma once
#include "block.h"
#include "colblk.h"
#include "bson.h"
#include "tabjson.h"

typedef class BTUTIL*  PBTUT;
typedef class BCUTIL*  PBCUT;
typedef class BSONDEF* PBDEF;
typedef class TDBBSON* PBTDB;
typedef class BSONCOL* PBSCOL;
class TDBBSN;
DllExport PQRYRES BSONColumns(PGLOBAL, PCSZ, PCSZ, PTOS, bool);

/***********************************************************************/
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
class BSONDISC : public BLOCK {
public:
  // Constructor
  BSONDISC(PGLOBAL g, uint* lg);

  // Functions
  int  GetColumns(PGLOBAL g, PCSZ db, PCSZ dsn, PTOS topt);
  bool Find(PGLOBAL g, PBVAL jvp, PCSZ key, int j);
  void AddColumn(PGLOBAL g);

  // Members
  JCOL    jcol;
  PJCL    jcp, fjcp, pjcp;
  //PVL     vlp;
  PBDEF   tdp;
  TDBBSN *tjnp;
  PBTDB   tjsp;
  PBPR    jpp;
  PBVAL   jsp;
  PBPR    row;
  PBTUT   bp;
  PCSZ    sep;
  PCSZ    strfy;
  char    colname[65], fmt[129], buf[16];
  uint   *length;
  int     i, n, bf, ncol, lvl, sz, limit;
  bool    all;
}; // end of BSONDISC

/***********************************************************************/
/*  JSON table.                                                        */
/***********************************************************************/
class DllExport BSONDEF : public DOSDEF {         /* Table description */
  friend class TDBBSON;
  friend class TDBBSN;
  friend class TDBBCL;
  friend class BSONDISC;
  friend class BSONCOL;
#if defined(CMGO_SUPPORT)
  friend class CMGFAM;
#endif   // CMGO_SUPPORT
#if defined(JAVA_SUPPORT)
  friend class JMGFAM;
#endif   // JAVA_SUPPORT
public:
  // Constructor
  BSONDEF(void);

  // Implementation
  const char* GetType(void) override { return "BSON"; }

  // Methods
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE m) override;

protected:
  // Members
  PGLOBAL G;										/* Bson utility memory                 */
  JMODE   Jmode;                /* MODE_OBJECT by default              */
  PCSZ    Objname;              /* Name of first level object          */
  PCSZ    Xcol;                 /* Name of expandable column           */
  int     Limit;                /* Limit of multiple values            */
  int     Pretty;               /* Depends on file structure           */
  int     Base;                 /* The array index base                */
  bool    Strict;               /* Strict syntax checking              */
  char    Sep;                  /* The Jpath separator                 */
  const char* Uri;							/* MongoDB connection URI              */
  PCSZ    Collname;             /* External collection name            */
  PSZ     Options;              /* Colist ; Pipe                       */
  PSZ     Filter;               /* Filter                              */
  PSZ     Driver;								/* MongoDB Driver (C or JAVA)          */
  bool    Pipe;							    /* True if Colist is a pipeline        */
  int     Version;							/* Driver version                      */
  PSZ     Wrapname;							/* MongoDB java wrapper name           */
}; // end of BSONDEF


/* -------------------------- BTUTIL class --------------------------- */

/***********************************************************************/
/*  Handles all BJSON actions for a BSON table.                        */
/***********************************************************************/
class BTUTIL : public BDOC {
public:
  // Constructor
  BTUTIL(PGLOBAL G, TDBBSN* tp) : BDOC(G) { Tp = tp; }

  // Utility functions
  PBVAL FindRow(PGLOBAL g);
  PBVAL ParseLine(PGLOBAL g, int prty, bool cma);
  PBVAL MakeTopTree(PGLOBAL g, int type);
  PSZ   SerialVal(PGLOBAL g, PBVAL top, int pretty);

protected:
  // Members
  TDBBSN* Tp;
}; // end of class BTUTIL

/* -------------------------- BCUTIL class --------------------------- */

/***********************************************************************/
/*  Handles all BJSON actions for a BSON columns.                      */
/***********************************************************************/
class BCUTIL : public BTUTIL {
public:
  // Constructor
  BCUTIL(PGLOBAL G, PBSCOL cp, TDBBSN* tp) : BTUTIL(G, tp)
    { Cp = cp; Jb = false; }

  // Utility functions
  void  SetJsonValue(PGLOBAL g, PVAL vp, PBVAL jvp);
  PBVAL MakeBson(PGLOBAL g, PBVAL jsp, int n);
  PBVAL GetRowValue(PGLOBAL g, PBVAL row, int i);
  PVAL  GetColumnValue(PGLOBAL g, PBVAL row, int i);
  PVAL  ExpandArray(PGLOBAL g, PBVAL arp, int n);
  PVAL  CalculateArray(PGLOBAL g, PBVAL arp, int n);
  PBVAL GetRow(PGLOBAL g);

protected:
  // Member
  PBSCOL  Cp;
  bool    Jb;
}; // end of class BCUTIL

   /* -------------------------- TDBBSN class --------------------------- */

/***********************************************************************/
/*  This is the BSN Access Method class declaration.                   */
/*  The table is a DOS file, each record being a JSON object.          */
/***********************************************************************/
class DllExport TDBBSN : public TDBDOS {
  friend class BSONCOL;
  friend class BSONDEF;
  friend class BTUTIL;
  friend class BCUTIL;
  friend class BSONDISC;
#if defined(CMGO_SUPPORT)
  friend class CMGFAM;
#endif   // CMGO_SUPPORT
#if defined(JAVA_SUPPORT)
  friend class JMGFAM;
#endif   // JAVA_SUPPORT
public:
  // Constructor
  TDBBSN(PGLOBAL g, PBDEF tdp, PTXF txfp);
  TDBBSN(TDBBSN* tdbp);

  // Implementation
  AMT  GetAmType(void) override { return TYPE_AM_JSN; }
  bool SkipHeader(PGLOBAL g) override;
  PTDB Duplicate(PGLOBAL g) override { return (PTDB)new(g) TDBBSN(this); }
  PBVAL GetRow(void) { return Row; }

  // Methods
  PTDB Clone(PTABS t) override;
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  PCOL InsertSpecialColumn(PCOL colp) override;
  int  RowNumber(PGLOBAL g, bool b = FALSE) override {return (b) ? M : N;}
  bool CanBeFiltered(void) override 
               {return Txfp->GetAmType() == TYPE_AM_MGO || !Xcol;}

  // Database routines
  int  Cardinality(PGLOBAL g) override;
  int  GetMaxSize(PGLOBAL g) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  bool PrepareWriting(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
  void CloseDB(PGLOBAL g) override;

  // Specific routine
  int  EstimatedLength(void) override;

protected:
  PBVAL FindRow(PGLOBAL g);
//int   MakeTopTree(PGLOBAL g, PBVAL jsp);

  // Members
  PBTUT   Bp;                      // The BSUTIL handling class
  PBVAL   Top;                     // The top JSON tree
  PBVAL   Row;                     // The current row
  PBSCOL  Colp;                    // The multiple column
  JMODE   Jmode;                   // MODE_OBJECT by default
  PCSZ    Objname;                 // The table object name
  PCSZ    Xcol;                    // Name of expandable column
  int     Fpos;                    // The current row index
  int     N;                       // The current Rownum
  int     M;                       // Index of multiple value
  int     Limit;		    				   // Limit of multiple values
  int     Pretty;                  // Depends on file structure
  int     NextSame;                // Same next row
  int     SameRow;                 // Same row nb
  int     Xval;                    // Index of expandable array
  int     B;                       // Array index base
  char    Sep;                     // The Jpath separator
  bool    Strict;                  // Strict syntax checking
  bool    Comma;                   // Row has final comma
}; // end of class TDBBSN

/* -------------------------- BSONCOL class -------------------------- */

/***********************************************************************/
/*  Class BSONCOL: JSON access method column descriptor.               */
/***********************************************************************/
class DllExport BSONCOL : public DOSCOL {
  friend class TDBBSN;
  friend class TDBBSON;
  friend class BCUTIL;
#if defined(CMGO_SUPPORT)
  friend class CMGFAM;
#endif   // CMGO_SUPPORT
#if defined(JAVA_SUPPORT)
  friend class JMGFAM;
#endif   // JAVA_SUPPORT
public:
  // Constructors
  BSONCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);
  BSONCOL(BSONCOL* colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  int   GetAmType(void) override { return Tbp->GetAmType(); }
  bool  Stringify(void) override { return Sgfy; }

  // Methods
  bool  SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check) override;
          bool  ParseJpath(PGLOBAL g);
  PSZ   GetJpath(PGLOBAL g, bool proj) override;
  void  ReadColumn(PGLOBAL g) override;
  void  WriteColumn(PGLOBAL g) override;

protected:
  bool  CheckExpand(PGLOBAL g, int i, PSZ nm, bool b);
  bool  SetArrayOptions(PGLOBAL g, char* p, int i, PSZ nm);

  // Default constructor not to be used
  BSONCOL(void) = default;

  // Members
  TDBBSN *Tbp;                  // To the JSN table block
  PBCUT   Cp;                   // To the BCUTIL handling class
  PVAL    MulVal;               // To value used by multiple column
  char   *Jpath;                // The json path
  JNODE  *Nodes;                // The intermediate objects
  int     Nod;                  // The number of intermediate objects
  int     Xnod;                 // Index of multiple values
  char    Sep;                  // The Jpath separator
  bool    Xpd;                  // True for expandable column
  bool    Parsed;               // True when parsed
  bool    Warned;               // True when warning issued
  bool    Sgfy;									// True if stringified
}; // end of class BSONCOL

/* -------------------------- TDBBSON class -------------------------- */

/***********************************************************************/
/*  This is the JSON Access Method class declaration.                  */
/***********************************************************************/
class DllExport TDBBSON : public TDBBSN {
  friend class BSONDEF;
  friend class BSONCOL;
public:
  // Constructor
  TDBBSON(PGLOBAL g, PBDEF tdp, PTXF txfp);
  TDBBSON(PBTDB tdbp);

  // Implementation
  AMT  GetAmType(void) override { return TYPE_AM_JSON; }
  PTDB Duplicate(PGLOBAL g) override { return (PTDB)new(g) TDBBSON(this); }
  PBVAL GetDoc(void) { return Docp; }

  // Methods
  PTDB Clone(PTABS t) override;

  // Database routines
  int  Cardinality(PGLOBAL g) override;
  int  GetMaxSize(PGLOBAL g) override;
  void ResetSize(void) override;
  int  GetProgCur(void) override { return N; }
  int  GetRecpos(void) override;
  bool SetRecpos(PGLOBAL g, int recpos) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  bool PrepareWriting(PGLOBAL g) override { return false; }
  int  WriteDB(PGLOBAL g) override;
  int  DeleteDB(PGLOBAL g, int irc) override;
  void CloseDB(PGLOBAL g) override;
  int  MakeDocument(PGLOBAL g);

  // Optimization routines
  int  MakeIndex(PGLOBAL g, PIXDEF pxdf, bool add) override;

protected:
  int  MakeNewDoc(PGLOBAL g);

  // Members
  PBVAL Docp;                      // The document array
  PBVAL Docrow;                    // Document row
  int   Multiple;                  // 0: No 1: DIR 2: Section 3: filelist
  int   Docsize;                   // The document size
  bool  Done;                      // True when document parsing is done
  bool  Changed;                   // After Update, Insert or Delete
}; // end of class TDBBSON

/***********************************************************************/
/*  This is the class declaration for the JSON catalog table.          */
/***********************************************************************/
class DllExport TDBBCL : public TDBCAT {
public:
  // Constructor
  TDBBCL(PBDEF tdp);

protected:
  // Specific routines
  PQRYRES GetResult(PGLOBAL g) override;

  // Members
  PTOS Topt;
  PCSZ Db;
  PCSZ Dsn;
}; // end of class TDBBCL
