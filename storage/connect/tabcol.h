/*************** TabCol H Declares Source Code File (.H) ***************/
/*  Name: TABCOL.H    Version 2.8                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2013    */
/*                                                                     */
/*  This file contains the XTAB, COLUMN and XORDER class definitions.  */
/***********************************************************************/

/***********************************************************************/
/*  Include required application header files                          */
/*  block.h      is header containing Block    global declarations.    */
/***********************************************************************/
#include "xobject.h"

/***********************************************************************/
/*  Definition of class XTAB with all its method functions.            */
/***********************************************************************/
class DllExport XTAB: public BLOCK {  // Table Name-Schema-Srcdef block.
  friend class TDBPRX;
  friend class TDBTBM;
 public:
  // Constructors
  XTAB(LPCSTR name, LPCSTR srcdef = NULL);
  XTAB(PTABLE tp);

  // Implementation
  PTABLE GetNext(void) {return Next;}
  PTDB   GetTo_Tdb(void) {return To_Tdb;}
  LPCSTR GetName(void) {return Name;}
  LPCSTR GetSrc(void) {return Srcdef;}
  LPCSTR GetSchema(void) {return Schema;}
  LPCSTR GetQualifier(void) {return Qualifier;}
  void   SetTo_Tdb(PTDB tdbp) {To_Tdb = tdbp;}
  void   SetName(LPCSTR name) {Name = name;}
  void   SetSrc(LPCSTR srcdef) {Srcdef = srcdef;}
  void   SetSchema(LPCSTR schname) {Schema = schname;}
  void   SetQualifier(LPCSTR qname) {Qualifier = qname;}

  // Methods
  PTABLE Link(PTABLE);
  void   Printf(PGLOBAL g, FILE *f, uint n) override;
  void   Prints(PGLOBAL g, char *ps, uint z) override;

 protected:
  // Members
  PTABLE    Next;              // Points to next table in chain
  PTDB      To_Tdb;            // Points to Table description Block
  LPCSTR    Name;              // Table name
  LPCSTR    Srcdef;            // Table Source definition
  LPCSTR    Schema;            // Schema name
  LPCSTR    Qualifier;         // Qualifier name
  }; // end of class XTAB


/***********************************************************************/
/*  Definition of class COLUMN with all its method functions.          */
/*  Note: because of LNA routines, the constantness of Name was        */
/*  removed and constructing a COLUMN with null name was allowed.      */
/*  Perhaps this should be replaced by the use of a specific class.    */
/***********************************************************************/
class DllExport COLUMN: public XOBJECT {  // Column Name/Qualifier block.
 public:
  // Constructor
  COLUMN(LPCSTR name);

  // Implementation
  int    GetType(void) override {return TYPE_COLUMN;}
  int    GetResultType(void) override {assert(false); return TYPE_VOID;}
  int    GetLength(void) override {assert(false); return 0;}
  int    GetLengthEx(void) override {assert(false); return 0;}
  int    GetScale() override {assert(false); return 0;};
          LPCSTR GetName(void) {return Name;}
          LPCSTR GetQualifier(void) {return Qualifier;}
          PTABLE GetTo_Table(void) {return To_Table;}
          PCOL   GetTo_Col(void) {return To_Col;}
          void   SetQualifier(LPCSTR qualif) {Qualifier = qualif;}
          void   SetTo_Table(PTABLE tablep) {To_Table = tablep;}
          void   SetTo_Col(PCOL colp) {To_Col = colp;}

  // Methods
  void   Printf(PGLOBAL g, FILE *f, uint n) override;
  void   Prints(PGLOBAL g, char *ps, uint z) override;
  // All methods below should never be used for COLUMN's
  void   Reset(void) override {assert(false);}
  bool   Compare(PXOB) override {assert(false); return false;}
  bool   SetFormat(PGLOBAL, FORMAT&) override;
  bool   Eval(PGLOBAL) override {assert(false); return true;}

 private:
  // Members
  PTABLE  To_Table;             // Point to Table Name Block
  PCOL    To_Col;               // Points to Column Description Block
  LPCSTR  const Name;           // Column name
  LPCSTR  Qualifier;            // Qualifier name
  }; // end of class COLUMN

/***********************************************************************/
/*  Definition of class SPCCOL with all its method functions.          */
/*  Note: Currently the special columns are ROWID, ROWNUM, FILEID,     */
/*  SERVID, TABID, PARTID, and CONID.                                  */
/***********************************************************************/
class SPCCOL: public COLUMN {  // Special Column Name/Qualifier block.
 public:
  // Constructor
  SPCCOL(LPCSTR name) : COLUMN(name) {}

 private:
  // Members
  }; // end of class SPCCOL
