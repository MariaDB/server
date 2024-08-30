// TABUTIL.H     Olivier Bertrand    2013
// Defines the TAB catalog tables

#ifndef TABUTIL
#define TABUTIL 1

//#include "tabtbl.h"

typedef class PRXDEF *PPRXDEF;
typedef class TDBPRX *PTDBPRX;
typedef class XXLCOL *PXXLCOL;
typedef class PRXCOL *PPRXCOL;
typedef class TBCDEF *PTBCDEF;
typedef class TDBTBC *PTDBTBC;

TABLE_SHARE *GetTableShare(PGLOBAL g, THD *thd, const char *db, 
                                      const char *name, bool& mysql);
PQRYRES TabColumns(PGLOBAL g, THD *thd, const char *db, 
                                        const char *name, bool& info);

TABLE_SHARE *Remove_tshp(PCATLG cat);
void Restore_tshp(PCATLG cat, TABLE_SHARE *s);

/* -------------------------- PROXY classes -------------------------- */

/***********************************************************************/
/*  PROXY: table based on another table. Can be used to have a         */
/*  different view on an existing table.                               */
/*  However, its real use is to be the base of TBL and PRX tables.     */
/***********************************************************************/

/***********************************************************************/
/*  PRX table.                                                         */
/***********************************************************************/
class DllExport PRXDEF : public TABDEF {  /* Logical table description */
	friend class TDBPRX;
	friend class TDBTBC;
 public:
  // Constructor
	 PRXDEF(void);

  // Implementation
  const char *GetType(void) override {return "PRX";}

  // Methods
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE mode) override;

 protected:
  // Members
  PTABLE  Tablep;                                /* The object table   */
  }; // end of PRXDEF

/***********************************************************************/
/*  This is the class declaration for the XCSV table.                  */
/***********************************************************************/
class DllExport TDBPRX : public TDBASE {
  friend class PRXDEF;
  friend class PRXCOL;
 public:
  // Constructors
  TDBPRX(PPRXDEF tdp);
  TDBPRX(PTDBPRX tdbp);

  // Implementation
  AMT   GetAmType(void) override {return TYPE_AM_PRX;}
  PTDB  Duplicate(PGLOBAL g) override
                {return (PTDB)new(g) TDBPRX(this);}

  // Methods
  PTDB  Clone(PTABS t) override;
  int   GetRecpos(void) override {return Tdbp->GetRecpos();}
	void  ResetDB(void) override {Tdbp->ResetDB();}
	int   RowNumber(PGLOBAL g, bool b = FALSE) override;
  PCSZ  GetServer(void) override {return (Tdbp) ? Tdbp->GetServer() : (PSZ)"?";}

  // Database routines
	PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  virtual bool  InitTable(PGLOBAL g);
  int   Cardinality(PGLOBAL g) override;
  int   GetMaxSize(PGLOBAL g) override;
  bool  OpenDB(PGLOBAL g) override;
  int   ReadDB(PGLOBAL g) override;
  int   WriteDB(PGLOBAL g) override;
  int   DeleteDB(PGLOBAL g, int irc) override;
  void  CloseDB(PGLOBAL g) override {if (Tdbp) Tdbp->CloseDB(g);}
          PTDB  GetSubTable(PGLOBAL g, PTABLE tabp, bool b = false);
          void  RemoveNext(PTABLE tp);

 protected:
  // Members
  PTDB Tdbp;                      // The object table
  }; // end of class TDBPRX

/***********************************************************************/
/*  Class PRXCOL: PRX access method column descriptor.                 */
/*  This A.M. is used for PRX tables.                                  */
/***********************************************************************/
class DllExport PRXCOL : public COLBLK {
  friend class TDBPRX;
  friend class TDBTBL;
  friend class TDBOCCUR;
 public:
  // Constructors
  PRXCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "PRX");
  PRXCOL(PRXCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  int  GetAmType(void) override {return TYPE_AM_PRX;}

  // Methods
  using COLBLK::Init;
  void Reset(void) override;
  bool IsSpecial(void) override {return Pseudo;}
  bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check) override
                {return false;}
  void ReadColumn(PGLOBAL g) override;
  void WriteColumn(PGLOBAL g) override;
  virtual bool Init(PGLOBAL g, PTDB tp);

 protected:
          char *Decode(PGLOBAL g, const char *cnm);

  // Default constructor not to be used
  PRXCOL(void) = default;

  // Members
  PCOL     Colp;               // Points to matching table column
  PVAL     To_Val;             // To the matching column value
  bool     Pseudo;             // TRUE for special columns
  int      Colnum;             // Used when retrieving columns by number
  }; // end of class PRXCOL

/***********************************************************************/
/*  This is the class declaration for the TBC column catalog table.    */
/***********************************************************************/
class TDBTBC : public TDBCAT {
 public:
  // Constructors
  TDBTBC(PPRXDEF tdp);

 protected:
	// Specific routines
	PQRYRES GetResult(PGLOBAL g) override;

  // Members
  PSZ     Db;                    // Database of the table  
  PSZ     Tab;                   // Table name            
  }; // end of class TDBMCL

class XCOLBLK : public COLBLK {
  friend class PRXCOL;
}; // end of class XCOLBLK

#endif // TABUTIL
