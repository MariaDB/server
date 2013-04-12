// TDBMYSQL.H     Olivier Bertrand    2007-2013
#include "myconn.h"               // MySQL connection declares

typedef class MYSQLDEF *PMYDEF;
typedef class TDBMYSQL *PTDBMY;
typedef class MYSQLC   *PMYC;
typedef class MYSQLCOL *PMYCOL;

/* ------------------------- MYSQL classes --------------------------- */

/***********************************************************************/
/*  MYSQL: table type that are MySQL tables.                           */
/*  Using embedded MySQL library (or optionally calling a MySQL server)*/
/***********************************************************************/

/***********************************************************************/
/*  MYSQL table.                                                       */
/***********************************************************************/
class MYSQLDEF : public TABDEF           {/* Logical table description */
  friend class TDBMYSQL;
  friend class TDBMCL;
  friend class ha_connect;
 public:
  // Constructor
  MYSQLDEF(void);


  // Implementation
  virtual const char *GetType(void) {return "MYSQL";}
  inline  PSZ  GetHostname(void) {return Hostname;};
  inline  PSZ  GetDatabase(void) {return Database;};
  inline  PSZ  GetTabname(void) {return Tabname;}
  inline  PSZ  GetUsername(void) {return Username;};
  inline  PSZ  GetPassword(void) {return Password;};
  inline  int  GetPortnumber(void) {return Portnumber;}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);
          bool ParseURL(PGLOBAL g, char *url);

 protected:
  // Members
  PSZ     Hostname;           /* Host machine to use                   */
  PSZ     Database;           /* Database to be used by server         */
  PSZ     Tabname;            /* External table name                   */
  PSZ     Username;           /* User logon name                       */
  PSZ     Password;           /* Password logon info                   */
  int     Portnumber;         /* MySQL port number (0 = default)       */
  bool    Bind;               /* Use prepared statement on insert      */
  bool    Delayed;            /* Delayed insert                        */
  }; // end of MYSQLDEF

/***********************************************************************/
/*  This is the class declaration for the MYSQL table.                 */
/***********************************************************************/
class TDBMYSQL : public TDBASE {
  friend class MYSQLCOL;
 public:
  // Constructor
  TDBMYSQL(PMYDEF tdp);
  TDBMYSQL(PGLOBAL g, PTDBMY tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_MYSQL;}
  virtual PTDB Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBMYSQL(g, this);}

  // Methods
  virtual PTDB CopyOne(PTABS t);
  virtual int  GetAffectedRows(void) {return AftRows;}
  virtual int  GetRecpos(void) {return N;}
  virtual int  GetProgMax(PGLOBAL g);
  virtual void ResetDB(void) {N = 0;}
  virtual int  RowNumber(PGLOBAL g, bool b = FALSE);
          void SetDatabase(LPCSTR db) {Database = (char*)db;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:
  // Internal functions
  bool MakeSelect(PGLOBAL g);
  bool MakeInsert(PGLOBAL g);
//bool MakeUpdate(PGLOBAL g);  
//bool MakeDelete(PGLOBAL g);
  int  BindColumns(PGLOBAL g);

  // Members
  MYSQLC      Myc;            // MySQL connection class
  MYSQL_BIND *Bind;           // To the MySQL bind structure array
  char       *Host;           // Host machine to use
  char       *User;           // User logon info
  char       *Pwd;            // Password logon info
  char       *Database;       // Database to be used by server
  char       *Tabname;        // External table name
  char       *Query;          // Points to SQL query
  char       *Qbuf;            // Used for not prepared insert
  bool        Fetched;        // True when fetch was done
  bool        Prep;           // Use prepared statement on insert
  bool        Delayed;        // Use delayed insert
  int         m_Rc;           // Return code from command
  int         AftRows;        // The number of affected rows
  int         N;              // The current table index
  int         Port;           // MySQL port number (0 = default) 
  int         Nparm;          // The number of statement parameters
  }; // end of class TDBMYSQL

/***********************************************************************/
/*  Class MYSQLCOL: MySQL table column.                                */
/***********************************************************************/
class MYSQLCOL : public COLBLK {
  friend class TDBMYSQL;
 public:
  // Constructors
  MYSQLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i,  PSZ am = "MYSQL");
  MYSQLCOL(MYSQLCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_MYSQL;}
          void InitBind(PGLOBAL g);

  // Methods
  virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);

 protected:
  // Default constructor not to be used
  MYSQLCOL(void) {}

  // Members
  MYSQL_BIND   *Bind;            // This column bind structure pointer
  PVAL          To_Val;          // To value used for Update/Insert
  unsigned long Slen;            // Bind string lengh
  int           Rank;            // Rank (position) number in the query
  }; // end of class MYSQLCOL

/***********************************************************************/
/*  This is the class declaration for the MYSQL column catalog table.  */
/***********************************************************************/
class TDBMCL : public TDBCAT {
 public:
  // Constructor
  TDBMCL(PMYDEF tdp);

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  // Members
  PSZ     Host;                  // Host machine to use            
  PSZ     Db;                    // Database to be used by server  
  PSZ     Tab;                   // External table name            
  PSZ     User;                  // User logon name                
  PSZ     Pwd;                   // Password logon info            
  int     Port;                  // MySQL port number (0 = default)
  }; // end of class TDBMCL
