/************** MyConn C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: MYCONN                                                 */
/* -------------                                                        */
/*  Version 1.5                                                         */
/*                                                                      */
/* COPYRIGHT:                                                           */
/* ----------                                                           */
/*  (C) Copyright to the author Olivier BERTRAND          2007-2012     */
/*                                                                      */
/* WHAT THIS PROGRAM DOES:                                              */
/* -----------------------                                              */
/*  Implements a connection to MySQL.                                   */
/*  It can optionally use the embedded MySQL library.                   */
/*                                                                      */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                               */
/* --------------------------------------                               */
/*                                                                      */
/*  REQUIRED FILES:                                                     */
/*  ---------------                                                     */
/*    MYCONN.CPP     - Source code                                      */
/*    MYCONN.H       - MYCONN class declaration file                    */
/*    GLOBAL.H       - Global declaration file                          */
/*                                                                      */
/*  REQUIRED LIBRARIES:                                                 */
/*  -------------------                                                 */
/*    Large model C library                                             */
/*                                                                      */
/*  REQUIRED PROGRAMS:                                                  */
/*  ------------------                                                  */
/*    IBM, Borland, GNU or Microsoft C++ Compiler and Linker            */
/*                                                                      */
/************************************************************************/
#include "my_global.h"
#if defined(WIN32)
//#include <windows.h>
#else   // !WIN32
#include "osutil.h"
#endif  // !WIN32

#include "global.h"
#include "plgdbsem.h"
#include "value.h"
#include "valblk.h"
#define  DLL_EXPORT            // Items are exported from this DLL
#include "myconn.h"

#if defined(EMBEDDED)
static char *server_args[] = {
  "this_program",       /* this string is not used */
  "--skip-bdb",
  "--skip-innodb"
  };

static char *server_groups[] = {
  "PlugDB_SERVER",
  "embedded",
  "server",
  (char *)NULL
  };
#endif   // EMBEDDED

/* -------------------------- Class MYSQLC --------------------------- */

/***********************************************************************/
/*  Implementation of the MYSQLC class.                                */
/***********************************************************************/
MYSQLC::MYSQLC(void)
  {
  m_DB = NULL;
  m_Stmt = NULL;
  m_Res = NULL;
  m_Rows = -1;
  m_Row = NULL;
  m_Fields = -1;
  N = 0;
  } // end of MYSQLC constructor

/***********************************************************************/
/*  Get the number of lines of the result set.                         */
/*  Currently we send the Select command and return m_Rows             */
/*  Perhaps should we use Select count(*) ... (?????)                  */
/*  No because here we execute only one query instead of two           */
/*  (the select count(*) plus the normal query)                        */
/***********************************************************************/
int MYSQLC::GetResultSize(PGLOBAL g, PSZ sql)
  {
  if (m_Rows < 0)
    if (ExecSQL(g, sql) != RC_OK)
      return -1;

  return m_Rows;
  } // end of GetResultSize

/***********************************************************************/
/*  Open a MySQL (remote) connection.                                  */
/***********************************************************************/
int MYSQLC::Open(PGLOBAL g, PSZ host, PSZ db, PSZ user, PSZ pwd, int pt)
  {
  m_DB = mysql_init(NULL);

  if (!m_DB) {
    strcpy(g->Message, "mysql_init failed: no memory");
    return RC_FX;
    } // endif m_DB

  // Notice that the client and server use separate group names.
  // This is critical, because the server will not accept the
  // client's options, and vice versa.
  mysql_options(m_DB, MYSQL_READ_DEFAULT_GROUP, "PlugDB_CLIENT");

#if 0
  if (pwd && !strcmp(pwd, "*")) {
    if (GetPromptAnswer(g, "*Enter password:")) {
      m_DB = NULL;
      return RC_FX;
    } else
      pwd = g->Message;

    } // endif pwd
#endif // 0

  if (!mysql_real_connect(m_DB, host, user, pwd, db, pt, NULL, CLIENT_MULTI_RESULTS)) {
#if defined(_DEBUG)
    sprintf(g->Message, "mysql_real_connect failed: (%d) %s",
                        mysql_errno(m_DB), mysql_error(m_DB));
#else   // !_DEBUG
    sprintf(g->Message, "(%d) %s", mysql_errno(m_DB), mysql_error(m_DB));
#endif  // !_DEBUG
    m_DB = NULL;
    return RC_FX;
    } // endif mysql_real_connect

  return RC_OK;
  } // end of Open

/***********************************************************************/
/*  Returns true if the connection is still alive.                     */
/***********************************************************************/
bool MYSQLC::Connected(void)
  {
  int rc;

  if (!m_DB)
    return FALSE;
  else if ((rc = mysql_ping(m_DB)) == CR_SERVER_GONE_ERROR)
    return FALSE;
  else
    return TRUE;

  } // end of Connected

/***********************************************************************/
/*  Returns the thread ID of the current MySQL connection.             */
/***********************************************************************/
ulong MYSQLC::GetThreadID(void)
  {
  return (m_DB) ? mysql_thread_id(m_DB) : 0;
  } // end of GetThreadID

/***********************************************************************/
/*  Returns a string that represents the server version number.        */
/***********************************************************************/
const char *MYSQLC::ServerInfo(void)
  {
  return (m_DB) ? mysql_get_server_info(m_DB) : NULL;
  } // end of ServerInfo

/***********************************************************************/
/*  Returns the version number of the server as a number that           */
/*  represents the MySQL server version in this format:                 */
/*  major_version*10000 + minor_version *100 + sub_version             */
/***********************************************************************/
ulong MYSQLC::ServerVersion(void)
  {
  return (m_DB) ? mysql_get_server_version(m_DB) : 0;
  } // end of ServerVersion

/**************************************************************************/
/*  KillQuery: Send MySQL a Kill Query command.                           */
/**************************************************************************/
int MYSQLC::KillQuery(ulong id)
  {
  char kill[20];

  sprintf(kill, "KILL QUERY %u", id);
  return (m_DB) ? mysql_query(m_DB, kill) : 1;
  } // end of KillQuery

/***********************************************************************/
/*  Prepare the SQL statement used to insert into a MySQL table.       */
/***********************************************************************/
int MYSQLC::PrepareSQL(PGLOBAL g, const char *stmt)
  {
  if (!m_DB) {
    strcpy(g->Message, "MySQL not connected");
    return -4;
  } else if (m_Stmt)
    return -1;              // should not append

#if defined(ALPHA)
  if (!(m_Stmt = mysql_prepare(m_DB, stmt, strlen(stmt)))) {

    sprintf(g->Message, "mysql_prepare failed: %s [%s]",
                         mysql_error(m_DB), stmt);
    return -1;
    } // endif m_Stmt

  // Return the parameter count from the statement
  return mysql_param_count(m_Stmt);
#else   // !ALPHA
  if (!(m_Stmt = mysql_stmt_init(m_DB))) {
    strcpy(g->Message, "mysql_stmt_init(), out of memory");
    return -2;
    } // endif m_Stmt

  if (mysql_stmt_prepare(m_Stmt, stmt, strlen(stmt))) {
    sprintf(g->Message, "mysql_stmt_prepare() failed: (%d) %s",
            mysql_stmt_errno(m_Stmt), mysql_stmt_error(m_Stmt));
    return -3;
    } // endif prepare

  // Return the parameter count from the statement
  return mysql_stmt_param_count(m_Stmt);
#endif   // !ALPHA
  } // end of PrepareSQL

/***********************************************************************/
/*  Exec the Select SQL command and get back the result size in rows.  */
/***********************************************************************/
int MYSQLC::ExecSQL(PGLOBAL g, const char *query, int *w)
  {
  int rc = RC_OK;

  if (!m_DB) {
    strcpy(g->Message, "MySQL not connected");
    return RC_FX;
    } // endif m_DB

  if (w)
    *w = 0;

  if (m_Rows >= 0)
    return RC_OK;                  // Already done

  if (mysql_query(m_DB, query) != 0) {
    char *msg = (char*)PlugSubAlloc(g, NULL, 512 + strlen(query));

    sprintf(msg, "(%d) %s [%s]", mysql_errno(m_DB),
                                 mysql_error(m_DB), query);
    strncpy(g->Message, msg, sizeof(g->Message) - 1);
    g->Message[sizeof(g->Message) - 1] = 0;
    rc = RC_FX;
  } else if (mysql_field_count(m_DB) > 0) {
    if (!(m_Res = mysql_store_result(m_DB))) {
      char *msg = (char*)PlugSubAlloc(g, NULL, 512 + strlen(query));

      sprintf(msg, "mysql_store_result failed: %s", mysql_error(m_DB));
      strncpy(g->Message, msg, sizeof(g->Message) - 1);
      g->Message[sizeof(g->Message) - 1] = 0;
      rc = RC_FX;
    } else {
      m_Fields = mysql_num_fields(m_Res);
      m_Rows = (int)mysql_num_rows(m_Res);
    } // endif m_Res

  } else {
    m_Rows = (int)mysql_affected_rows(m_DB);
    sprintf(g->Message, "Affected rows: %d\n", m_Rows);
    rc = RC_NF;
  } // endif field count

  if (w)
    *w = mysql_warning_count(m_DB);

  return rc;
  } // end of ExecSQL

/***********************************************************************/
/*  Bind the parameter buffers.                                        */
/***********************************************************************/
int MYSQLC::BindParams(PGLOBAL g, MYSQL_BIND *bind)
  {
  if (!m_DB) {
    strcpy(g->Message, "MySQL not connected");
    return RC_FX;
  } else
    assert(m_Stmt);

#if defined(ALPHA)
  if (mysql_bind_param(m_Stmt, bind)) {
    sprintf(g->Message, "mysql_bind_param() failed: %s",
                        mysql_stmt_error(m_Stmt));
#else   // !ALPHA
  if (mysql_stmt_bind_param(m_Stmt, bind)) {
    sprintf(g->Message, "mysql_stmt_bind_param() failed: %s",
                        mysql_stmt_error(m_Stmt));
#endif  // !ALPHA
    return RC_FX;
    } // endif bind

  return RC_OK;
  } // end of BindParams

/***********************************************************************/
/*  Execute a prepared statement.                                      */
/***********************************************************************/
int MYSQLC::ExecStmt(PGLOBAL g)
  {
  if (!m_DB) {
    strcpy(g->Message, "MySQL not connected");
    return RC_FX;
    } // endif m_DB

#if defined(ALPHA)
  if (mysql_execute(m_Stmt)) {
    sprintf(g->Message, "mysql_execute() failed: %s",
                        mysql_stmt_error(m_Stmt));
    return RC_FX;
    } // endif execute
#else   // !ALPHA
  if (mysql_stmt_execute(m_Stmt)) {
    sprintf(g->Message, "mysql_stmt_execute() failed: %s",
                        mysql_stmt_error(m_Stmt));
    return RC_FX;
    } // endif execute
#endif  // !ALPHA

  // Check the total number of affected rows
  if (mysql_stmt_affected_rows(m_Stmt) != 1) {
    sprintf(g->Message, "Invalid affected rows by MySQL");
    return RC_FX;
    } // endif affected_rows

  return RC_OK;
  } // end of ExecStmt

/***********************************************************************/
/*  Fetch one result line from the query result set.                   */
/***********************************************************************/
int MYSQLC::Fetch(PGLOBAL g, int pos)
  {
  if (!m_DB) {
    strcpy(g->Message, "MySQL not connected");
    return RC_FX;
    } // endif m_DB

  if (!m_Res) {
    // Result set was not initialized
    strcpy(g->Message, MSG(FETCH_NO_RES));
    return RC_FX;
  } else
    N++;

  if (pos >= 0)
    mysql_data_seek(m_Res, (my_ulonglong)pos);

  m_Row = mysql_fetch_row(m_Res);
  return (m_Row) ? RC_OK : RC_EF;
  } // end of Fetch

/***********************************************************************/
/*  Get one field of the current row.                                  */
/***********************************************************************/
char *MYSQLC::GetCharField(int i)
  {
  if (m_Res && m_Row) {
#if defined(_DEBUG)
    MYSQL_FIELD *fld = mysql_fetch_field_direct(m_Res, i);
#endif   // _DEBUG
    MYSQL_ROW row = m_Row + i;

    return (row) ? (char*)*row : (char*)"<null>";
  } else
    return NULL;

  } // end of GetCharField

/***********************************************************************/
/*  Get the max length of the field.                                   */
/***********************************************************************/
int MYSQLC::GetFieldLength(int i)
  {
  if (m_Res) {
    MYSQL_FIELD *fld = mysql_fetch_field_direct(m_Res, i);

    return fld->max_length;
  } else
    return 0;

  } // end of GetFieldLength

/***********************************************************************/
/*  Make a PlugDB result structure from the MySQL result.              */
/***********************************************************************/
PQRYRES MYSQLC::GetResult(PGLOBAL g, bool pdb)
  {
  char        *fmt;
  int          n;
  PCOLRES     *pcrp, crp;
  PQRYRES      qrp;
  MYSQL_FIELD *fld;
  MYSQL_ROW    row;

  if (!m_Res || !m_Fields) {
    sprintf(g->Message, "%s result", (m_Res) ? "Void" : "No");
    return NULL;
    } // endif m_Res

  /*********************************************************************/
  /*  Put the result in storage for future retrieval.                  */
  /*********************************************************************/
  qrp = (PQRYRES)PlugSubAlloc(g, NULL, sizeof(QRYRES));
  pcrp = &qrp->Colresp;
  qrp->Continued = FALSE;
  qrp->Truncated = FALSE;
  qrp->Info = FALSE;
  qrp->Suball = TRUE;
  qrp->BadLines = 0;
  qrp->Maxsize = m_Rows;
  qrp->Maxres = m_Rows;
  qrp->Nbcol = 0;
  qrp->Nblin = 0;
  qrp->Cursor = 0;

  for (fld = mysql_fetch_field(m_Res); fld;
       fld = mysql_fetch_field(m_Res)) {
    *pcrp = (PCOLRES)PlugSubAlloc(g, NULL, sizeof(COLRES));
    crp = *pcrp;
    pcrp = &crp->Next;
    crp->Ncol = ++qrp->Nbcol;

    crp->Name = (char*)PlugSubAlloc(g, NULL, fld->name_length + 1);
    strcpy(crp->Name, fld->name);

    if ((crp->Type = MYSQLtoPLG(fld->type)) == TYPE_ERROR) {
      sprintf(g->Message, "Type %d not supported for column %s",
                          fld->type, crp->Name);
      return NULL;
    } else if (crp->Type == TYPE_DATE && !pdb)
      // For direct MySQL connection, display the MySQL date string
      crp->Type = TYPE_STRING;

    crp->Prec = fld->decimals;
    crp->Length = fld->max_length;
    crp->Clen = GetTypeSize(crp->Type, crp->Length);
    crp->DBtype = GetDBType((int)crp->Type);

    if (!(crp->Kdata = AllocValBlock(g, NULL, crp->Type, m_Rows,
                                     crp->Clen, 0, FALSE, TRUE))) {
      sprintf(g->Message, MSG(INV_RESULT_TYPE),
                          GetFormatType(crp->Type));
      return NULL;
    } else if (crp->Type == TYPE_DATE) {
      fmt = MyDateFmt(fld->type);
      crp->Kdata->SetFormat(g, fmt, strlen(fmt));
    } // endif's

    if (fld->flags & NOT_NULL_FLAG)
      crp->Nulls = NULL;
    else {
      crp->Nulls = (char*)PlugSubAlloc(g, NULL, m_Rows);
      memset(crp->Nulls, ' ', m_Rows);
    } // endelse fld->flags

    } // endfor fld

  *pcrp = NULL;
  assert(qrp->Nbcol == m_Fields);

  /*********************************************************************/
  /*  Now fill the allocated result structure.                         */
  /*********************************************************************/
  for (n = 0; n < m_Rows; n++) {
    if (!(m_Row = mysql_fetch_row(m_Res))) {
      sprintf(g->Message, "Missing row %d from result", n + 1);
      return NULL;
      } // endif m_Row

    for (crp = qrp->Colresp; crp; crp = crp->Next) {
      if (row = m_Row + (crp->Ncol - 1))
        if (*row)
          crp->Kdata->SetValue((PSZ)*row, n);
        else {
          if (!*row && crp->Nulls)
            crp->Nulls[n] = '*';           // Null value
    
          crp->Kdata->Reset(n);
        } // endelse *row

      } // endfor crp

    } // endfor n

  qrp->Nblin = n;
  return qrp;
  } // end of GetResult

/***********************************************************************/
/*  Free the current result.                                           */
/***********************************************************************/
void MYSQLC::FreeResult(void)
  {
  if (m_Res) {
    mysql_free_result(m_Res);
    m_Res = NULL;
    } // endif m_Res

  // Reset the connection
  m_Row = NULL;
  m_Rows = -1;
  m_Fields = -1;
  N = 0;
  } // end of FreeResult

/***********************************************************************/
/*  Place the cursor at the beginning of the result set.               */
/***********************************************************************/
void MYSQLC::Rewind(void)
  {
  if (m_Res)
    mysql_data_seek(m_Res, 0);

  } // end of Rewind

/***********************************************************************/
/*  Close the connection.                                              */
/***********************************************************************/
void MYSQLC::Close(void)
  {
  FreeResult();
  mysql_close(m_DB);
  m_DB = NULL;
  } // end of Close

/***********************************************************************/
/*  Discard additional results from a stored procedure.                */
/***********************************************************************/
void MYSQLC::DiscardResults(void)
  {
  MYSQL_RES *res;

  while(!mysql_next_result(m_DB)) {
    res = mysql_store_result(m_DB);
    mysql_free_result(res);
    } // endwhile next result 

  } // end of DiscardResults 
