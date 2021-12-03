/*************** Tabxml H Declares Source Code File (.H) ***************/
/*  Name: TABXML.H    Version 1.7                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2007-2016    */
/*                                                                     */
/*  This file contains the XML table classes declares.                 */
/***********************************************************************/
typedef class XMLDEF *PXMLDEF;
typedef class TDBXML *PTDBXML;
typedef class XMLCOL *PXMLCOL;

DllExport PQRYRES XMLColumns(PGLOBAL, char *, char *, PTOS, bool);

/* --------------------------- XML classes --------------------------- */

/***********************************************************************/
/*  XML table.                                                         */
/***********************************************************************/
class DllExport XMLDEF : public TABDEF {  /* Logical table description */
  friend class TDBXML;
  friend class TDBXCT;
  friend PQRYRES XMLColumns(PGLOBAL, char*, char*, PTOS, bool);
 public:
  // Constructor
   XMLDEF(void);

  // Implementation
  virtual const char *GetType(void) {return "XML";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
	PCSZ    Fn;                     /* Path/Name of corresponding file   */
  char   *Encoding;               /* New XML table file encoding       */
  char   *Tabname;                /* Name of Table node                */
  char   *Rowname;                /* Name of first level nodes         */
  char   *Colname;                /* Name of second level nodes        */
  char   *Mulnode;                /* Name of multiple node             */
  char   *XmlDB;                  /* Name of XML DB node               */
  char   *Nslist;                 /* List of namespaces to register    */
  char   *DefNs;                  /* Dummy name of default namespace   */
  char   *Attrib;                 /* Table node attributes             */
  char   *Hdattr;                 /* Header node attributes            */
	PCSZ    Entry;						      /* Zip entry name or pattern				 */
	int     Coltype;                /* Default column type               */
  int     Limit;                  /* Limit of multiple values          */
  int     Header;                 /* n first rows are header rows      */
  bool    Xpand;                  /* Put multiple tags in several rows */
  bool    Usedom;                 /* True: DOM, False: libxml2         */
	bool    Zipped;                 /* True: Zipped XML file(s)          */
	bool    Mulentries;             /* True: multiple entries in zip file*/
	bool    Skip;                   /* Skip null columns                 */
}; // end of XMLDEF

#if defined(INCLUDE_TDBXML)
#include "m_ctype.h"

/***********************************************************************/
/*  This is the class declaration for the simple XML tables.           */
/***********************************************************************/
class DllExport TDBXML : public TDBASE {
  friend class XMLCOL;
  friend class XMULCOL;
  friend class XPOSCOL;
  friend PQRYRES XMLColumns(PGLOBAL, char*, char*, PTOS, bool);
 public:
  // Constructor
  TDBXML(PXMLDEF tdp);
  TDBXML(PTDBXML tdbp);

  // Implementation
  virtual AMT   GetAmType(void) {return TYPE_AM_XML;}
  virtual PTDB  Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBXML(this);}

  // Methods
  virtual PTDB  Clone(PTABS t);
  virtual int   GetRecpos(void);
  virtual int   GetProgCur(void) {return N;}
  virtual PCSZ  GetFile(PGLOBAL g) {return Xfile;}
  virtual void  SetFile(PGLOBAL g, PCSZ fn) {Xfile = fn;}
  virtual void  ResetDB(void) {N = 0;}
  virtual void  ResetSize(void) {MaxSize = -1;}
  virtual int   RowNumber(PGLOBAL g, bool b = false);
          int   LoadTableFile(PGLOBAL g, char *filename);
          bool  Initialize(PGLOBAL g);
          bool  SetTabNode(PGLOBAL g);
          void  SetNodeAttr(PGLOBAL g, char *attr, PXNODE node);
          bool  CheckRow(PGLOBAL g, bool b);

  // Database routines
  virtual PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual PCOL  InsertSpecialColumn(PCOL colp);
//virtual int   GetMaxSame(PGLOBAL g) {return (Xpand) ? Limit : 1;}
  virtual int   Cardinality(PGLOBAL g);
  virtual int   GetMaxSize(PGLOBAL g);
//virtual bool  NeedIndexing(PGLOBAL g);
  virtual bool  OpenDB(PGLOBAL g);
  virtual int   ReadDB(PGLOBAL g);
  virtual int   WriteDB(PGLOBAL g);
  virtual int   DeleteDB(PGLOBAL g, int irc);
  virtual void  CloseDB(PGLOBAL g);
  virtual int   CheckWrite(PGLOBAL g) {Checked = true; return 0;}
	virtual const CHARSET_INFO *data_charset();

 protected:
  // Members
  PXDOC   Docp;
  PXNODE  Root;
  PXNODE  Curp;
  PXNODE  DBnode;
  PXNODE  TabNode;
  PXNODE  RowNode;
  PXNODE  ColNode;
  PXLIST  Nlist;
  PXLIST  Clist;
  PFBLOCK To_Xb;                    // Pointer to XML file block
  PCOL    Colp;                     // The multiple column
  bool    Changed;                  // After Update, Insert or Delete
  bool    Checked;                  // After Update check pass
  bool    NextSame;                 // Same next row
  bool    Xpand;                    // Put multiple tags in several rows
  bool    NewRow;                   // True when inserting a new row
  bool    Hasnod;                   // True if rows have subnodes
  bool    Write;                    // True for Insert and Update
  bool    Usedom;                   // True for DOM, False for libxml2
  bool    Bufdone;                  // True when column buffers allocated
  bool    Nodedone;                 // True when column nodes allocated
  bool    Void;                     // True if the file does not exist
	bool    Zipped;                   // True if Zipped XML file(s)
	bool    Mulentries;               // True if multiple entries in zip file
	PCSZ    Xfile;                    // The XML file
  char   *Enc;                      // New XML table file encoding
  char   *Tabname;                  // Name of Table node
  char   *Rowname;                  // Name of first level nodes
  char   *Colname;                  // Name of second level nodes
  char   *Mulnode;                  // Name of multiple node
  char   *XmlDB;                    // Name of XML DB node
  char   *Nslist;                   // List of namespaces to register
  char   *DefNs;                    // Dummy name of default namespace
  char   *Attrib;                   // Table node attribut(s)
  char   *Hdattr;                   // Header node attribut(s)
	PCSZ    Entry;						        // Zip entry name or pattern
	int     Coltype;                  // Default column type
  int     Limit;                    // Limit of multiple values
  int     Header;                   // n first rows are header rows
  int     Multiple;                 // If multiple files
  int     Nrow;                     // The table cardinality
  int     Irow;                     // The current row index
  int     Nsub;                     // The current subrow index
  int     N;                        // The current Rowid
  }; // end of class TDBXML

/***********************************************************************/
/*  Class XMLCOL: XDB table access method column descriptor.           */
/***********************************************************************/
class XMLCOL : public COLBLK {
	friend class TDBXML;
 public:
  // Constructors
  XMLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "XML");
  XMLCOL(XMLCOL *colp, PTDB tdbp);   // Constructor used in copy process

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_XML;}
  virtual void SetTo_Val(PVAL valp) {To_Val = valp;}
          bool ParseXpath(PGLOBAL g, bool mode);

  // Methods
  virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);
          bool AllocBuf(PGLOBAL g, bool mode);
          void AllocNodes(PGLOBAL g, PXDOC dp);

 protected:
//xmlNodePtr SelectSingleNode(xmlNodePtr node, char *name);

  // Default constructor not to be used
  XMLCOL(void) : COLBLK(1) {}

  // Members
  PXLIST  Nl;              
  PXLIST  Nlx;            
  PXNODE  ColNode;        
  PXNODE  ValNode;        
  PXNODE  Cxnp;            
  PXNODE  Vxnp;            
  PXATTR  Vxap;            
  PXATTR  AttNode;            
  PTDBXML Tdbp;                          
  char   *Valbuf;                 // To the node value buffer
  char   *Xname;                  // The node or attribute name
  char*  *Nodes;                  // The intermediate nodes
  int     Type;                   // 0: Attribute, 1: Tag, 2: position
  int     Nod;                    // The number of intermediate nodes
  int     Inod;                   // Index of multiple node
  int     Rank;                   // Position
  bool    Mul;                    // true for multiple column
  bool    Checked;                // Was checked while Updating
  int     Long;                   // Buffer length
  int     Nx;                     // The last read row
  int     Sx;                     // The last read sub-row
  int     N;                      // The number of (multiple) values
  PVAL    To_Val;                 // To value used for Update/Insert
  }; // end of class XMLCOL

/***********************************************************************/
/*  Derived class XMLCOLX: used to replace a multiple XMLCOL by the    */
/*  derived class XMULCOL that has specialize read and write functions.*/
/*  Note: this works only if the members of the derived class are the  */
/*  same than the ones of the original class (NO added members).       */
/***********************************************************************/
class XMLCOLX : public XMLCOL {
 public:
  // Fake operator new used to change a filter into a derived filter
  void * operator new(size_t size, PXMLCOL colp) {return colp;}
#if !defined(__BORLANDC__)
  // Avoid warning C4291 by defining a matching dummy delete operator
  void operator delete(void *, size_t size) {}
  void operator delete(void *, PXMLCOL) {}
#endif
  }; // end of class XMLCOLX

/***********************************************************************/
/*  Class XMULCOL: XML table access method multiple column descriptor. */
/***********************************************************************/
class XMULCOL : public XMLCOLX {
 public:
  // The constructor must restore Value because XOBJECT has a void
  // constructor called by default that set Value to NULL
  XMULCOL(PVAL valp) {Value = valp; Mul = true;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);
  }; // end of class XMULCOL

/***********************************************************************/
/*  Class XPOSCOL: XML table column accessed by position.              */
/***********************************************************************/
class XPOSCOL : public XMLCOLX {
 public:
  // The constructor must restore Value because XOBJECT has a void
  // constructor called by default that set Value to NULL
  XPOSCOL(PVAL valp) {Value = valp;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);
  }; // end of class XPOSCOL

/***********************************************************************/
/*  This is the class declaration for the XML catalog table.           */
/***********************************************************************/
class TDBXCT : public TDBCAT {
 public:
  // Constructor
  TDBXCT(PXMLDEF tdp);

 protected:
  // Specific routines
  virtual PQRYRES GetResult(PGLOBAL g);

  // Members
  PTOS  Topt;
  char *Db;
  char *Tabn;
  }; // end of class TDBXCT

#endif // INCLUDE_TDBXML
