/*************** TabCol H Declares Source Code File (.H) ***************/
/*  Name: TABCOL.H    Version 2.7                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2012    */
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
 class DllExport XTAB: public BLOCK { // Table Name-Owner-Correl block.
 public:
  // Constructors
  XTAB(LPCSTR name, LPCSTR correl = NULL);
  XTAB(PTABLE tp);

  // Implementation
  PTABLE GetNext(void) {return Next;}
  PTDB   GetTo_Tdb(void) {return To_Tdb;}
  LPCSTR GetName(void) {return Name;}
  LPCSTR GetCorrel(void) {return Correl;}
  LPCSTR GetCreator(void) {return Creator;}
  LPCSTR GetQualifier(void) {return Qualifier;}
  void   SetTo_Tdb(PTDB tdbp) {To_Tdb = tdbp;}
  void   SetName(LPCSTR name) {Name = name;}
  void   SetCorrel(LPCSTR correl) {Correl = correl;}
  void   SetCreator(LPCSTR crname) {Creator = crname;}
  void   SetQualifier(LPCSTR qname) {Qualifier = qname;}

  // Methods
  PTABLE Link(PTABLE);
  void   Print(PGLOBAL g, FILE *f, uint n);
  void   Print(PGLOBAL g, char *ps, uint z);

 protected:
  // Members
  PTABLE    Next;              // Points to next table in chain
  PTDB      To_Tdb;            // Points to Table description Block
  LPCSTR    Name;              // Table name (can be changed by LNA and PLG)
  LPCSTR    Correl;            // Correlation name
  LPCSTR    Creator;           // Creator name
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
  virtual int    GetType(void) {return TYPE_COLUMN;}
  virtual int    GetResultType(void) {assert(false); return TYPE_VOID;}
  virtual int    GetLength(void) {assert(false); return 0;}
  virtual int    GetLengthEx(void) {assert(false); return 0;}
  virtual int    GetPrecision() {assert(false); return 0;};
          LPCSTR GetName(void) {return Name;}
          LPCSTR GetQualifier(void) {return Qualifier;}
          PTABLE GetTo_Table(void) {return To_Table;}
          PCOL   GetTo_Col(void) {return To_Col;}
          void   SetQualifier(LPCSTR qualif) {Qualifier = qualif;}
          void   SetTo_Table(PTABLE tablep) {To_Table = tablep;}
          void   SetTo_Col(PCOL colp) {To_Col = colp;}

  // Methods
  virtual void   Print(PGLOBAL g, FILE *f, uint n);
  virtual void   Print(PGLOBAL g, char *ps, uint z);
  // All methods below should never be used for COLUMN's
  virtual void   Reset(void) {assert(false);}
  virtual bool   Compare(PXOB) {assert(false); return false;}
  virtual bool   SetFormat(PGLOBAL, FORMAT&);
  virtual bool   Eval(PGLOBAL) {assert(false); return true;}
  virtual int    CheckSpcCol(PTDB, int) {assert(false); return 2;}
  virtual bool   CheckSort(PTDB) {assert(false); return false;}
  virtual void   MarkCol(ushort) {assert(false);}

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
/*  SERVID, TABID, and CONID.                                          */
/***********************************************************************/
class SPCCOL: public COLUMN {  // Special Column Name/Qualifier block.
 public:
  // Constructor
  SPCCOL(LPCSTR name) : COLUMN(name) {}

 private:
  // Members
  }; // end of class SPCCOL
