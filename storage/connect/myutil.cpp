/************** MyUtil C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: MYUTIL                                                 */
/* -------------                                                        */
/*  Version 1.2                                                         */
/*                                                                      */
/*  Author Olivier BERTRAND                               2014          */
/*                                                                      */
/* WHAT THIS PROGRAM DOES:                                              */
/* -----------------------                                              */
/*  It contains utility functions to convert data types.                */
/*  It can optionally use the embedded MySQL library.                   */
/*                                                                      */
/************************************************************************/
#include "my_global.h"
#include <mysql.h>
#if defined(_WIN32)
//#include <windows.h>
#else   // !_WIN32
#include "osutil.h"
#endif  // !_WIN32

#include "global.h"
#include "plgdbsem.h"
//#include "value.h"
//#include "valblk.h"
#include "myutil.h"
#define  DLL_EXPORT            // Items are exported from this DLL

//extern "C" int xconv;
TYPCONV GetTypeConv(void);

/************************************************************************/
/*  Convert from MySQL type name to PlugDB type number                  */
/************************************************************************/
int MYSQLtoPLG(char *typname, char *var)
  {
  int     type;
  TYPCONV xconv = GetTypeConv();

  if (!stricmp(typname, "int") || !stricmp(typname, "mediumint") ||
      !stricmp(typname, "integer"))
    type = TYPE_INT;
  else if (!stricmp(typname, "smallint"))
    type = TYPE_SHORT;
  else if (!stricmp(typname, "char") || !stricmp(typname, "varchar") ||
		       !stricmp(typname, "enum") || !stricmp(typname, "set"))
    type = TYPE_STRING;
  else if (!stricmp(typname, "double")  || !stricmp(typname, "float") ||
           !stricmp(typname, "real"))
    type = TYPE_DOUBLE;
  else if (!stricmp(typname, "decimal") || !stricmp(typname, "numeric"))
    type = TYPE_DECIM;
  else if (!stricmp(typname, "date") || !stricmp(typname, "datetime") ||
           !stricmp(typname, "time") || !stricmp(typname, "timestamp") ||
           !stricmp(typname, "year"))
    type = TYPE_DATE;
  else if (!stricmp(typname, "bigint") || !stricmp(typname, "longlong"))
    type = TYPE_BIGINT;
  else if (!stricmp(typname, "tinyint"))
    type = TYPE_TINY;
  else if (!stricmp(typname, "text") && var) {
    switch (xconv) {
      case TPC_YES:
        type = TYPE_STRING;
        *var = 'X';
        break;
      case TPC_SKIP:
        *var = 'K';
        /* falls through */
      default: // TPC_NO
        type = TYPE_ERROR;
      } // endswitch xconv

    return type;
  } else
    type = TYPE_ERROR;

  if (var) {
    if (type == TYPE_DATE) {
      // This is to make the difference between temporal values 
      if (!stricmp(typname, "date"))
        *var = 'D';
      else if (!stricmp(typname, "datetime"))
        *var = 'A';
      else if (!stricmp(typname, "timestamp"))
        *var = 'S';
      else if (!stricmp(typname, "time"))
        *var = 'T';
      else if (!stricmp(typname, "year"))
        *var = 'Y';

		} else if (type == TYPE_STRING) {
			if (!stricmp(typname, "varchar"))
			  // This is to make the difference between CHAR and VARCHAR
				*var = 'V';

		} else if (type == TYPE_ERROR && xconv == TPC_SKIP)
      *var = 'K';
    else
      *var = 0;

    } // endif var

  return type;
  } // end of MYSQLtoPLG

/************************************************************************/
/*  Convert from PlugDB type to MySQL type number                       */
/************************************************************************/
enum enum_field_types PLGtoMYSQL(int type, bool dbf, char v)
  {
  enum enum_field_types mytype;

  switch (type) {
    case TYPE_INT:
      mytype = MYSQL_TYPE_LONG;
      break;
    case TYPE_SHORT:
      mytype = MYSQL_TYPE_SHORT;
      break;
    case TYPE_DOUBLE:
      mytype = MYSQL_TYPE_DOUBLE;
      break;
    case TYPE_DATE:
      mytype = (dbf) ? MYSQL_TYPE_DATE :
          (v == 'S') ? MYSQL_TYPE_TIMESTAMP :
          (v == 'D') ? MYSQL_TYPE_NEWDATE :
          (v == 'T') ? MYSQL_TYPE_TIME :
          (v == 'Y') ? MYSQL_TYPE_YEAR : MYSQL_TYPE_DATETIME;
      break;
    case TYPE_STRING:
      mytype = (v) ? MYSQL_TYPE_VARCHAR : MYSQL_TYPE_STRING;
      break;
    case TYPE_BIGINT:
      mytype = MYSQL_TYPE_LONGLONG;
      break;
    case TYPE_TINY:
      mytype = MYSQL_TYPE_TINY;
      break;
    case TYPE_DECIM:
#if !defined(ALPHA)
      mytype = MYSQL_TYPE_NEWDECIMAL;
#else     // ALPHA
      mytype = MYSQL_TYPE_DECIMAL;
#endif    // ALPHA
      break;
    default:
      mytype = MYSQL_TYPE_NULL;
    } // endswitch mytype

  return mytype;
  } // end of PLGtoMYSQL

/************************************************************************/
/*  Convert from PlugDB type to MySQL type name                         */
/************************************************************************/
const char *PLGtoMYSQLtype(int type, bool dbf, char v)
  {
  switch (type) {
    case TYPE_INT:      return "INT";
    case TYPE_SHORT:    return "SMALLINT";
    case TYPE_DOUBLE:   return "DOUBLE";
    case TYPE_DATE:     return   dbf ? "DATE" : 
                          (v == 'S') ? "TIMESTAMP" :
                          (v == 'D') ? "DATE" :
                          (v == 'T') ? "TIME" :
                          (v == 'Y') ? "YEAR" : "DATETIME";
    case TYPE_STRING:   return v ? "VARCHAR" : "CHAR";
    case TYPE_BIGINT:   return "BIGINT";
    case TYPE_TINY:     return "TINYINT";
    case TYPE_DECIM:    return "DECIMAL";
    default:            return (v) ? "VARCHAR" : "CHAR";
    } // endswitch mytype

  } // end of PLGtoMYSQLtype

/************************************************************************/
/*  Convert from MySQL type to PlugDB type number                       */
/************************************************************************/
int MYSQLtoPLG(int mytype, char *var)
  {
  int type, xconv = GetTypeConv();

  switch (mytype) {
    case MYSQL_TYPE_SHORT:
      type = TYPE_SHORT;
      break;
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_ENUM:        // ???
      type = TYPE_INT;
      break;
    case MYSQL_TYPE_LONGLONG:
      type = TYPE_BIGINT;
      break;
    case MYSQL_TYPE_TINY:
      type = TYPE_TINY;
      break;
    case MYSQL_TYPE_DECIMAL:
#if !defined(ALPHA)
    case MYSQL_TYPE_NEWDECIMAL:
#endif   // !ALPHA)
      type = TYPE_DECIM;
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      type = TYPE_DOUBLE;
      break;
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIME:
      type = TYPE_DATE;
      break;
    case MYSQL_TYPE_VAR_STRING:
#if !defined(ALPHA)
    case MYSQL_TYPE_VARCHAR:
#endif   // !ALPHA)
    case MYSQL_TYPE_STRING:
      type = (*var == 'B') ? TYPE_BIN : TYPE_STRING;
      break;
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      if (var) {
        switch (xconv) {
          case TPC_YES:
            if (*var != 'B') {
              // This is a TEXT column
              type = TYPE_STRING;
              *var = 'X';
            } else
              type = TYPE_BIN;
 
            break;
          case TPC_SKIP:
            *var = 'K';       // Skip
            /* falls through */
          default:            // TPC_NO
            type = TYPE_ERROR;
          } // endswitch xconv

        return type;
        } // endif var
      /* falls through */
    default:
      type = TYPE_ERROR;
    } // endswitch mytype

  if (var) switch (mytype) {
    // This is to make the difference between CHAR and VARCHAR 
#if !defined(ALPHA)
    case MYSQL_TYPE_VARCHAR:
#endif   // !ALPHA)
    case MYSQL_TYPE_VAR_STRING: *var = 'V'; break;
    // This is to make the difference between temporal values 
    case MYSQL_TYPE_TIMESTAMP:  *var = 'S'; break;
    case MYSQL_TYPE_DATE:       *var = 'D'; break;
    case MYSQL_TYPE_DATETIME:   *var = 'A'; break;
    case MYSQL_TYPE_YEAR:       *var = 'Y'; break;
    case MYSQL_TYPE_TIME:       *var = 'T'; break;
    default:                    *var = 0;
    } // endswitch mytype

  return type;
  } // end of MYSQLtoPLG

/************************************************************************/
/*  Returns the format corresponding to a MySQL date type number.       */
/************************************************************************/
PCSZ MyDateFmt(int mytype)
  {
  PCSZ fmt;

  switch (mytype) {
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATETIME:
      fmt = "YYYY-MM-DD hh:mm:ss";
      break;
    case MYSQL_TYPE_DATE:
      fmt = "YYYY-MM-DD";
      break;
    case MYSQL_TYPE_YEAR:
      fmt = "YYYY";
      break;
    case MYSQL_TYPE_TIME:
      fmt = "hh:mm:ss";
      break;
    default:
      fmt = NULL;
    } // endswitch mytype

  return fmt;
  } // end of MyDateFmt

/************************************************************************/
/*  Returns the format corresponding to a MySQL date type name.         */
/************************************************************************/
PCSZ MyDateFmt(char *typname)
  {
  PCSZ fmt;

  if (!stricmp(typname, "datetime") || !stricmp(typname, "timestamp"))
    fmt = "YYYY-MM-DD hh:mm:ss";
  else if (!stricmp(typname, "date"))
    fmt = "YYYY-MM-DD";
  else if (!stricmp(typname, "year"))
    fmt = "YYYY";
  else if (!stricmp(typname, "time"))
    fmt = "hh:mm:ss";
  else
    fmt = NULL;

  return fmt;
  } // end of MyDateFmt

