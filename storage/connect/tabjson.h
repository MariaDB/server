/*************** tabjson H Declares Source Code File (.H) **************/
/*  Name: tabjson.h   Version 1.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2015  */
/*                                                                     */
/*  This file contains the JSON classes declares.                      */
/***********************************************************************/
#include "osutil.h"
#include "block.h"
#include "colblk.h"
#include "json.h"

enum JMODE {MODE_OBJECT, MODE_ARRAY, MODE_VALUE};

typedef class JSONDEF *PJDEF;
typedef class TDBJSON *PJTDB;
typedef class JSONCOL *PJCOL;

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

/***********************************************************************/
/*  JSON table.                                                        */
/***********************************************************************/
class JSONDEF : public DOSDEF {                   /* Table description */
  friend class TDBJSON;
  friend class TDBJSN;
  friend class TDBJCL;
  friend PQRYRES JSONColumns(PGLOBAL, char*, PTOS, bool);
 public:
  // Constructor
  JSONDEF(void);

  // Implementation
  virtual const char *GetType(void) {return "JSON";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
  JMODE Jmode;                  /* MODE_OBJECT by default              */
  char *Objname;                /* Name of first level object          */
  char *Xcol;                   /* Name of expandable column           */
  int   Limit;                  /* Limit of multiple values            */
  int   Pretty;                 /* Depends on file structure           */
  int   Level;                  /* Used for catalog table              */
  int   Base;                   /* Tne array index base                */
  bool  Strict;                 /* Strict syntax checking              */
  }; // end of JSONDEF

/* -------------------------- TDBJSN class --------------------------- */

/***********************************************************************/
/*  This is the JSN Access Method class declaration.                   */
/*  The table is a DOS file, each record being a JSON object.          */
/***********************************************************************/
class TDBJSN : public TDBDOS {
  friend class JSONCOL;
	friend class JSONDEF;
public:
  // Constructor
   TDBJSN(PJDEF tdp, PTXF txfp);
   TDBJSN(TDBJSN *tdbp);

  // Implementation
  virtual AMT   GetAmType(void) {return TYPE_AM_JSN;}
  virtual bool  SkipHeader(PGLOBAL g);
  virtual PTDB  Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBJSN(this);}
          PJSON GetRow(void) {return Row;} 

  // Methods
  virtual PTDB  CopyOne(PTABS t);
  virtual PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual PCOL  InsertSpecialColumn(PCOL colp);
  virtual int   RowNumber(PGLOBAL g, bool b = FALSE)
                 {return (b) ? M : N;}

  // Database routines
  virtual int   Cardinality(PGLOBAL g);
  virtual int   GetMaxSize(PGLOBAL g);
  virtual bool  OpenDB(PGLOBAL g);
  virtual int   ReadDB(PGLOBAL g);
	virtual bool  PrepareWriting(PGLOBAL g);
	virtual int   WriteDB(PGLOBAL g);

 protected:
          PJSON FindRow(PGLOBAL g);
          int   MakeTopTree(PGLOBAL g, PJSON jsp);

  // Members
	PGLOBAL G;											 // Support of parse memory
	PJSON   Top;                     // The top JSON tree
	PJSON   Row;                     // The current row
	PJSON   Val;                     // The value of the current row
	PJCOL   Colp;                    // The multiple column
	JMODE   Jmode;                   // MODE_OBJECT by default
  char   *Objname;                 // The table object name
  char   *Xcol;                    // Name of expandable column
	int     Fpos;                    // The current row index
	int     N;                       // The current Rownum
	int     M;                       // Index of multiple value
	int     Limit;		    				   // Limit of multiple values
	int     Pretty;                  // Depends on file structure
	int     NextSame;                // Same next row
	int     SameRow;                 // Same row nb
	int     Xval;                    // Index of expandable array
	int     B;                       // Array index base
	bool    Strict;                  // Strict syntax checking
	bool    Comma;                   // Row has final comma
  }; // end of class TDBJSN

/* -------------------------- JSONCOL class -------------------------- */

/***********************************************************************/
/*  Class JSONCOL: JSON access method column descriptor.               */
/***********************************************************************/
class JSONCOL : public DOSCOL {
  friend class TDBJSN;
  friend class TDBJSON;
 public:
  // Constructors
  JSONCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);
  JSONCOL(JSONCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  virtual int  GetAmType(void) {return Tjp->GetAmType();}

  // Methods
  virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
          bool ParseJpath(PGLOBAL g);
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);

 protected:
  bool    CheckExpand(PGLOBAL g, int i, PSZ nm, bool b);
  bool    SetArrayOptions(PGLOBAL g, char *p, int i, PSZ nm);
  PVAL    GetColumnValue(PGLOBAL g, PJSON row, int i);
  PVAL    ExpandArray(PGLOBAL g, PJAR arp, int n);
  PVAL    CalculateArray(PGLOBAL g, PJAR arp, int n);
  PVAL    MakeJson(PGLOBAL g, PJSON jsp);
  void    SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val, int n);
  PJSON   GetRow(PGLOBAL g);

  // Default constructor not to be used
  JSONCOL(void) {}

  // Members
	PGLOBAL G;										// Support of parse memory
	TDBJSN *Tjp;                  // To the JSN table block
  PVAL    MulVal;               // To value used by multiple column
  char   *Jpath;                // The json path
  JNODE  *Nodes;                // The intermediate objects
  int     Nod;                  // The number of intermediate objects
  int     Xnod;                 // Index of multiple values
  bool    Xpd;                  // True for expandable column
  bool    Parsed;               // True when parsed
  }; // end of class JSONCOL

/* -------------------------- TDBJSON class -------------------------- */

/***********************************************************************/
/*  This is the JSON Access Method class declaration.                  */
/***********************************************************************/
class TDBJSON : public TDBJSN {
	friend class JSONDEF;
	friend class JSONCOL;
 public:
  // Constructor
   TDBJSON(PJDEF tdp, PTXF txfp);
   TDBJSON(PJTDB tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_JSON;}
  virtual PTDB Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBJSON(this);}
          PJAR GetDoc(void) {return Doc;} 

  // Methods
  virtual PTDB CopyOne(PTABS t);

  // Database routines
  virtual int  Cardinality(PGLOBAL g);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual void ResetSize(void);
  virtual int  GetProgCur(void) {return N;}
	virtual int  GetRecpos(void);
  virtual bool SetRecpos(PGLOBAL g, int recpos);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual bool PrepareWriting(PGLOBAL g) {return false;}
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);
          int  MakeDocument(PGLOBAL g);

  // Optimization routines
  virtual int  MakeIndex(PGLOBAL g, PIXDEF pxdf, bool add);

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
class TDBJCL : public TDBCAT {
 public:
  // Constructor
  TDBJCL(PJDEF tdp);

 protected:
  // Specific routines
  virtual PQRYRES GetResult(PGLOBAL g);

  // Members
  PTOS  Topt;
  char *Db;
  }; // end of class TDBJCL
