/*************** tabjson H Declares Source Code File (.H) **************/
/*  Name: tabjson.h   Version 1.3                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2021  */
/*                                                                     */
/*  This file contains the JSON classes declares.                      */
/***********************************************************************/
#pragma once
//#include "osutil.h"				// Unuseful and bad for OEM
#include "block.h"
#include "colblk.h"
#include "json.h"

enum JMODE {MODE_OBJECT, MODE_ARRAY, MODE_VALUE};

typedef class JSONDEF *PJDEF;
typedef class TDBJSON *PJTDB;
typedef class JSONCOL *PJCOL;
class TDBJSN;
DllExport PQRYRES JSONColumns(PGLOBAL, PCSZ, PCSZ, PTOS, bool);

/***********************************************************************/
/*  The JSON tree node. Can be an Object or an Array.           	  	 */
/***********************************************************************/
typedef struct _jnode {
  PSZ   Key;                    // The key used for object
  OPVAL Op;                     // Operator used for this node
  PVAL  CncVal;                 // To cont value used for OP_CNC
  PVAL  Valp;                   // The internal array VALUE
  int   Rank;                   // The rank in array
  int   Rx;                     // Read row number
  int   Nx;                     // Next to read row number
} JNODE, *PJNODE;

typedef struct _jncol {
	struct _jncol *Next;
	char *Name;
	char *Fmt;
	JTYP  Type;
	int   Len;
	int   Scale;
	bool  Cbn;
	bool  Found;
} JCOL, *PJCL;

/***********************************************************************/
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
class JSONDISC : public BLOCK {
public:
	// Constructor
	JSONDISC(PGLOBAL g, uint *lg);

	// Functions
	int  GetColumns(PGLOBAL g, PCSZ db, PCSZ dsn, PTOS topt);
	bool Find(PGLOBAL g, PJVAL jvp, PCSZ key, int j);
	void AddColumn(PGLOBAL g);

	// Members
	JCOL    jcol;
	PJCL    jcp, fjcp, pjcp;
//PVL     vlp;
	PJDEF   tdp;
	TDBJSN *tjnp;
	PJTDB   tjsp;
	PJPR    jpp;
	PJSON   jsp;
	PJOB    row;
	PCSZ    sep;
  PCSZ    strfy;
	char    colname[65], fmt[129], buf[16];
	uint   *length;
	int     i, n, bf, ncol, lvl, sz, limit;
	bool    all;
}; // end of JSONDISC

/***********************************************************************/
/*  JSON table.                                                        */
/***********************************************************************/
class DllExport JSONDEF : public DOSDEF {         /* Table description */
  friend class TDBJSON;
  friend class TDBJSN;
  friend class TDBJCL;
	friend class JSONDISC;
#if defined(CMGO_SUPPORT)
	friend class CMGFAM;
#endif   // CMGO_SUPPORT
#if defined(JAVA_SUPPORT)
	friend class JMGFAM;
#endif   // JAVA_SUPPORT
public:
  // Constructor
  JSONDEF(void);

  // Implementation
  const char *GetType(void) override {return "JSON";}

  // Methods
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE m) override;

 protected:
  // Members
  JMODE Jmode;                  /* MODE_OBJECT by default              */
	PCSZ  Objname;                /* Name of first level object          */
	PCSZ  Xcol;                   /* Name of expandable column           */
  int   Limit;                  /* Limit of multiple values            */
  int   Pretty;                 /* Depends on file structure           */
  int   Base;                   /* The array index base                */
  bool  Strict;                 /* Strict syntax checking              */
	char  Sep;                    /* The Jpath separator                 */
	const char *Uri;							/* MongoDB connection URI              */
	PCSZ  Collname;               /* External collection name            */
	PSZ   Options;                /* Colist ; Pipe                       */
	PSZ   Filter;                 /* Filter                              */
	PSZ   Driver;									/* MongoDB Driver (C or JAVA)          */
	bool  Pipe;							      /* True if Colist is a pipeline        */
	int   Version;							  /* Driver version                      */
	PSZ   Wrapname;								/* MongoDB java wrapper name           */
  }; // end of JSONDEF

/* -------------------------- TDBJSN class --------------------------- */

/***********************************************************************/
/*  This is the JSN Access Method class declaration.                   */
/*  The table is a DOS file, each record being a JSON object.          */
/***********************************************************************/
class DllExport TDBJSN : public TDBDOS {
  friend class JSONCOL;
	friend class JSONDEF;
	friend class JSONDISC;
#if defined(CMGO_SUPPORT)
	friend class CMGFAM;
#endif   // CMGO_SUPPORT
#if defined(JAVA_SUPPORT)
	friend class JMGFAM;
#endif   // JAVA_SUPPORT
public:
  // Constructor
   TDBJSN(PJDEF tdp, PTXF txfp);
   TDBJSN(TDBJSN *tdbp);

  // Implementation
  AMT   GetAmType(void) override {return TYPE_AM_JSN;}
  bool  SkipHeader(PGLOBAL g) override;
  PTDB  Duplicate(PGLOBAL g) override {return (PTDB)new(g) TDBJSN(this);}
          PJSON GetRow(void) {return Row;}
					void  SetG(PGLOBAL g) {G = g;}

  // Methods
  PTDB  Clone(PTABS t) override;
  PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  PCOL  InsertSpecialColumn(PCOL colp) override;
  int   RowNumber(PGLOBAL g, bool b = FALSE) override
                 {return (b) ? M : N;}
	bool  CanBeFiltered(void) override 
	              {return Txfp->GetAmType() == TYPE_AM_MGO || !Xcol;}

  // Database routines
  //int   Cardinality(PGLOBAL g) override;
  //int   GetMaxSize(PGLOBAL g) override;
  bool  OpenDB(PGLOBAL g) override;
  int   ReadDB(PGLOBAL g) override;
	bool  PrepareWriting(PGLOBAL g) override;
	int   WriteDB(PGLOBAL g) override;
  void  CloseDB(PGLOBAL g) override;

	// Specific routine
	int   EstimatedLength(void) override;

protected:
          PJSON FindRow(PGLOBAL g);
          bool  MakeTopTree(PGLOBAL g, PJSON jsp);

  // Members
	PGLOBAL G;											 // Support of parse memory
	PJSON   Top;                     // The top JSON tree
	PJSON   Row;                     // The current row
	PJVAL   Val;                     // The value of the current row
	PJCOL   Colp;                    // The multiple column
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
  bool    Xpdable;                 // False: expandable columns are NULL
}; // end of class TDBJSN

/* -------------------------- JSONCOL class -------------------------- */

/***********************************************************************/
/*  Class JSONCOL: JSON access method column descriptor.               */
/***********************************************************************/
class DllExport JSONCOL : public DOSCOL {
  friend class TDBJSN;
  friend class TDBJSON;
#if defined(CMGO_SUPPORT)
	friend class CMGFAM;
#endif   // CMGO_SUPPORT
#if defined(JAVA_SUPPORT)
	friend class JMGFAM;
#endif   // JAVA_SUPPORT
public:
  // Constructors
  JSONCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);
  JSONCOL(JSONCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  int   GetAmType(void) override {return Tjp->GetAmType();}
  bool  Stringify(void) override { return Sgfy; }

  // Methods
  bool  SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check) override;
          bool  ParseJpath(PGLOBAL g);
	PSZ   GetJpath(PGLOBAL g, bool proj) override;
	void  ReadColumn(PGLOBAL g) override;
  void  WriteColumn(PGLOBAL g) override;

 protected:
  bool  CheckExpand(PGLOBAL g, int i, PSZ nm, bool b);
  bool  SetArrayOptions(PGLOBAL g, char *p, int i, PSZ nm);
  PVAL  GetColumnValue(PGLOBAL g, PJSON row, int i);
  PVAL  ExpandArray(PGLOBAL g, PJAR arp, int n);
  PVAL  CalculateArray(PGLOBAL g, PJAR arp, int n);
  PVAL  MakeJson(PGLOBAL g, PJSON jsp, int n);
  PJVAL GetRowValue(PGLOBAL g, PJSON row, int i);
  void  SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val);
	PJSON GetRow(PGLOBAL g);

  // Default constructor not to be used
  JSONCOL(void) = default;

  // Members
	PGLOBAL G;										// Support of parse memory
	TDBJSN *Tjp;                  // To the JSN table block
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
}; // end of class JSONCOL

/* -------------------------- TDBJSON class -------------------------- */

/***********************************************************************/
/*  This is the JSON Access Method class declaration.                  */
/***********************************************************************/
class DllExport TDBJSON : public TDBJSN {
	friend class JSONDEF;
	friend class JSONCOL;
 public:
  // Constructor
   TDBJSON(PJDEF tdp, PTXF txfp);
   TDBJSON(PJTDB tdbp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_JSON;}
  PTDB Duplicate(PGLOBAL g) override {return (PTDB)new(g) TDBJSON(this);}
          PJAR GetDoc(void) {return Doc;} 

  // Methods
  PTDB Clone(PTABS t) override;

  // Database routines
  int  Cardinality(PGLOBAL g) override;
  int  GetMaxSize(PGLOBAL g) override;
  void ResetSize(void) override;
  int  GetProgCur(void) override {return N;}
	int  GetRecpos(void) override;
  bool SetRecpos(PGLOBAL g, int recpos) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  bool PrepareWriting(PGLOBAL g) override {return false;}
  int  WriteDB(PGLOBAL g) override;
  int  DeleteDB(PGLOBAL g, int irc) override;
  void CloseDB(PGLOBAL g) override;
          int  MakeDocument(PGLOBAL g);

  // Optimization routines
  int  MakeIndex(PGLOBAL g, PIXDEF pxdf, bool add) override;

 protected:
          int  MakeNewDoc(PGLOBAL g);

  // Members
  PJAR  Doc;                       // The document array
  int   Multiple;                  // 0: No 1: DIR 2: Section 3: filelist
  bool  Done;                      // True when document parsing is done
  bool  Changed;                   // After Update, Insert or Delete
  }; // end of class TDBJSON

/***********************************************************************/
/*  This is the class declaration for the JSON catalog table.          */
/***********************************************************************/
class DllExport TDBJCL : public TDBCAT {
 public:
  // Constructor
  TDBJCL(PJDEF tdp);

 protected:
  // Specific routines
  PQRYRES GetResult(PGLOBAL g) override;

  // Members
  PTOS Topt;
  PCSZ Db;
	PCSZ Dsn;
  }; // end of class TDBJCL
