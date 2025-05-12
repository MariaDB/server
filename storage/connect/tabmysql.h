// TDBMYSQL.H     Olivier Bertrand    2007-2017
#include "myconn.h"               // MySQL connection declares

typedef class MYSQLDEF *PMYDEF;
typedef class TDBMYSQL *PTDBMY;
typedef class MYSQLCOL *PMYCOL;
typedef class TDBMYEXC *PTDBMYX;
typedef class MYXCOL   *PMYXCOL;
typedef class MYSQLC   *PMYC;

/* ------------------------- MYSQL classes --------------------------- */

/***********************************************************************/
/*  MYSQL: table type that are MySQL tables.                           */
/*  Using embedded MySQL library (or optionally calling a MySQL server)*/
/***********************************************************************/

/***********************************************************************/
/*  MYSQL table.                                                       */
/***********************************************************************/
class MYSQLDEF : public EXTDEF           {/* Logical table description */
  friend class TDBMYSQL;
  friend class TDBMYEXC;
  friend class TDBMCL;
  friend class ha_connect;
 public:
  // Constructor
  MYSQLDEF(void);

  // Implementation
  const char *GetType(void) override {return "MYSQL";}
  inline  PSZ  GetHostname(void) {return Hostname;};
//inline  PSZ  GetDatabase(void) {return Tabschema;};
//inline  PSZ  GetTabname(void) {return Tabname;}
//inline  PSZ  GetSrcdef(void) {return Srcdef;}
//inline  PSZ  GetUsername(void) {return Username;};
//inline  PSZ  GetPassword(void) {return Password;};
  inline  int  GetPortnumber(void) {return Portnumber;}

  // Methods
//virtual int  Indexable(void) {return 2;}
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE m) override;
          bool ParseURL(PGLOBAL g, char *url, bool b = true);
          bool GetServerInfo(PGLOBAL g, const char *server_name);

 protected:
  // Members
  PSZ     Hostname;           /* Host machine to use                   */
//PSZ     Tabschema;          /* Database to be used by server         */
//PSZ     Tabname;            /* External table name                   */
//PSZ     Srcdef;             /* The source table SQL definition       */
//PSZ     Username;           /* User logon name                       */
//PSZ     Password;           /* Password logon info                   */
  PSZ     Server;             /* PServerID                             */
//PSZ     Qrystr;             /* The original query                    */
  int     Portnumber;         /* MySQL port number (0 = default)       */
//int     Maxerr;             /* Maxerr for an Exec table              */
//int     Quoted;             /* Identifier quoting level              */
  bool    Isview;             /* true if this table is a MySQL view    */
  bool    Bind;               /* Use prepared statement on insert      */
  bool    Delayed;            /* Delayed insert                        */
  bool    Ignored;            /* Use insert IGNORE                     */
//bool    Xsrc;               /* Execution type                        */
  bool    Huge;               /* True for big table                    */
  }; // end of MYSQLDEF

/***********************************************************************/
/*  This is the class declaration for the MYSQL table.                 */
/***********************************************************************/
class TDBMYSQL : public TDBEXT {
  friend class MYSQLCOL;
	friend class TDBTBM;
 public:
  // Constructor
  TDBMYSQL(PMYDEF tdp);
  TDBMYSQL(PTDBMY tdbp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_MYSQL;}
  PTDB Duplicate(PGLOBAL g) override {return (PTDB)new(g) TDBMYSQL(this);}

  // Methods
  PTDB Clone(PTABS t) override;
//virtual int  GetAffectedRows(void) {return AftRows;}
  int  GetRecpos(void) override {return N;}
  int  GetProgMax(PGLOBAL g) override;
  void ResetDB(void) override {N = 0;}
  int  RowNumber(PGLOBAL g, bool b = false) override;
  bool IsView(void) override {return Isview;}
  PCSZ GetServer(void) override {return Server;}
          void SetDatabase(LPCSTR db) {Schema = (char*)db;}

  // Schema routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int  Cardinality(PGLOBAL g) override;
//int  GetMaxSize(PGLOBAL g) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
  int  DeleteDB(PGLOBAL g, int irc) override;
  void CloseDB(PGLOBAL g) override;
  bool ReadKey(PGLOBAL g, OPVAL op, const key_range *kr) override;

  // Specific routines
          bool SetColumnRanks(PGLOBAL g);
          PCOL MakeFieldColumn(PGLOBAL g, char *name);
          PSZ  FindFieldColumn(char *name);

 protected:
  // Internal functions
  bool MakeSelect(PGLOBAL g, bool mx);
  bool MakeInsert(PGLOBAL g);
  int  BindColumns(PGLOBAL g __attribute__((unused)));
  bool MakeCommand(PGLOBAL g) override;
//int  MakeUpdate(PGLOBAL g);  
//int  MakeDelete(PGLOBAL g);
  int  SendCommand(PGLOBAL g);

  // Members
  MYSQLC      Myc;            // MySQL connection class
  MYSQL_BIND *Bind;           // To the MySQL bind structure array
//PSTRG       Query;          // Constructed SQL query
  char       *Host;           // Host machine to use
//char       *User;           // User logon info
//char       *Pwd;            // Password logon info
//char       *Schema;         // Database to be used by server
//char       *TableName;      // External table name
//char       *Srcdef;         // The source table SQL definition
  char       *Server;         // The server ID
//char       *Qrystr;         // The original query
  bool        Fetched;        // True when fetch was done
  bool        Isview;         // True if this table is a MySQL view
  bool        Prep;           // Use prepared statement on insert
  bool        Delayed;        // Use delayed insert
  bool        Ignored;        // Use insert IGNORE
  int         m_Rc;           // Return code from command
//int         AftRows;        // The number of affected rows
  int         N;              // The current table index
  unsigned    Port;          // MySQL port number (0 = default)
//int         Nparm;          // The number of statement parameters
//int         Quoted;         // The identifier quoting level
  }; // end of class TDBMYSQL

/***********************************************************************/
/*  Class MYSQLCOL: MySQL table column.                                */
/***********************************************************************/
class MYSQLCOL : public COLBLK {
  friend class TDBMYSQL;
 public:
  // Constructors
  MYSQLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i,  PCSZ am = "MYSQL");
  MYSQLCOL(MYSQL_FIELD *fld, PTDB tdbp, int i,  PCSZ am = "MYSQL");
  MYSQLCOL(MYSQLCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  int  GetAmType(void) override {return TYPE_AM_MYSQL;}
          void InitBind(PGLOBAL g);

  // Methods
  bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check) override;
  void ReadColumn(PGLOBAL g) override;
  void WriteColumn(PGLOBAL g) override;
          bool FindRank(PGLOBAL g);

 protected:
  // Members
  MYSQL_BIND   *Bind;            // This column bind structure pointer
  PVAL          To_Val;          // To value used for Update/Insert
  unsigned long Slen;            // Bind string lengh
  int           Rank;            // Rank (position) number in the query
  }; // end of class MYSQLCOL

/***********************************************************************/
/*  This is the class declaration for the exec command MYSQL table.    */
/***********************************************************************/
class TDBMYEXC : public TDBMYSQL {
  friend class MYXCOL;
 public:
  // Constructors
  TDBMYEXC(PMYDEF tdp); 
  TDBMYEXC(PTDBMYX tdbp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_MYX;}
  PTDB Duplicate(PGLOBAL g) override {return (PTDB)new(g) TDBMYEXC(this);}

  // Methods
  PTDB Clone(PTABS t) override;
  bool IsView(void) override {return Isview;}

  // Database routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int  GetMaxSize(PGLOBAL g) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;

 protected:
  // Internal functions
  PCMD MakeCMD(PGLOBAL g);

  // Members
  PCMD     Cmdlist;           // The commands to execute
  char    *Cmdcol;            // The name of the Xsrc command column
  bool     Shw;               // Show warnings
  bool     Havew;             // True when processing warnings
  bool     Isw;               // True for warning lines
  int      Warnings;          // Warnings number
  int      Mxr;               // Maximum errors before closing
  int      Nerr;              // Number of errors so far
  }; // end of class TDBMYEXC

/***********************************************************************/
/*  Class MYXCOL: MySQL exec command table column.                     */
/***********************************************************************/
class MYXCOL : public MYSQLCOL {
  friend class TDBMYEXC;
 public:
  // Constructors
  MYXCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i,  PCSZ am = "MYSQL");
  MYXCOL(MYSQL_FIELD *fld, PTDB tdbp, int i,  PCSZ am = "MYSQL");
  MYXCOL(MYXCOL *colp, PTDB tdbp);   // Constructor used in copy process

  // Methods
  void ReadColumn(PGLOBAL g) override;
  void WriteColumn(PGLOBAL g) override;

 protected:
  // Members
  char    *Buffer;              // To get returned message
  int      Flag;                // Column content desc
  }; // end of class MYXCOL

/***********************************************************************/
/*  This is the class declaration for the MYSQL column catalog table.  */
/***********************************************************************/
class TDBMCL : public TDBCAT {
 public:
  // Constructor
  TDBMCL(PMYDEF tdp);

 protected:
	// Specific routines
	PQRYRES GetResult(PGLOBAL g) override;

  // Members
  PCSZ Host;                      // Host machine to use            
	PCSZ Db;                        // Database to be used by server  
	PCSZ Tab;                       // External table name            
	PCSZ User;                      // User logon name                
	PCSZ Pwd;                       // Password logon info            
	int  Port;                      // MySQL port number (0 = default)
  }; // end of class TDBMCL
