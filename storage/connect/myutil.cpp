/************** MyUtil C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: MYUTIL                                                 */
/* -------------                                                        */
/*  Version 1.0                                                         */
/*                                                                      */
/*  Author Olivier BERTRAND                               2013          */
/*                                                                      */
/* WHAT THIS PROGRAM DOES:                                              */
/* -----------------------                                              */
/*  It contains utility functions to convert data types.                */
/*  It can optionally use the embedded MySQL library.                   */
/*                                                                      */
/************************************************************************/
#include "my_global.h"
#include <mysql.h>
#if defined(WIN32)
//#include <windows.h>
#else   // !WIN32
#include "osutil.h"
#endif  // !WIN32

#include "global.h"
#include "plgdbsem.h"
//#include "value.h"
//#include "valblk.h"
#define  DLL_EXPORT            // Items are exported from this DLL

/************************************************************************/
/*  Convert from MySQL type name to PlugDB type number                  */
/************************************************************************/
int MYSQLtoPLG(char *typname)
  {
  int type;

  if (!stricmp(typname, "int") || !stricmp(typname, "mediumint") ||
      !stricmp(typname, "integer"))
    type = TYPE_INT;
  else if (!stricmp(typname, "tinyint") || !stricmp(typname, "smallint"))
    type = TYPE_SHORT;
  else if (!stricmp(typname, "char") || !stricmp(typname, "varchar") ||
           !stricmp(typname, "text") || !stricmp(typname, "blob"))
    type = TYPE_STRING;
  else if (!stricmp(typname, "double") || !stricmp(typname, "float") ||
           !stricmp(typname, "real") || !stricmp(typname, "bigint") ||
           !stricmp(typname, "decimal") || !stricmp(typname, "numeric"))
    type = TYPE_FLOAT;
  else if (!stricmp(typname, "date") || !stricmp(typname, "datetime") ||
           !stricmp(typname, "time") || !stricmp(typname, "timestamp") ||
           !stricmp(typname, "year"))
    type = TYPE_DATE;
  else if (!stricmp(typname, "bigint") || !stricmp(typname, "longlong"))
    type = TYPE_BIGINT;
  else
    type = TYPE_ERROR;

  return type;
  } // end of MYSQLtoPLG

/************************************************************************/
/*  Convert from PlugDB type to MySQL type number                       */
/************************************************************************/
enum enum_field_types PLGtoMYSQL(int type, bool gdf)
  {
  enum enum_field_types mytype;

  switch (type) {
    case TYPE_INT:
      mytype = MYSQL_TYPE_LONG;
      break;
    case TYPE_SHORT:
      mytype = MYSQL_TYPE_SHORT;
      break;
    case TYPE_FLOAT:
      mytype = MYSQL_TYPE_DOUBLE;
      break;
    case TYPE_DATE:
      mytype = (gdf) ? MYSQL_TYPE_DATE : MYSQL_TYPE_DATETIME;
      break;
    case TYPE_STRING:
      mytype = MYSQL_TYPE_VARCHAR;
      break;
    case TYPE_BIGINT:
      mytype = MYSQL_TYPE_LONGLONG;
      break;
    default:
      mytype = MYSQL_TYPE_NULL;
    } // endswitch mytype

  return mytype;
  } // end of PLGtoMYSQL

/************************************************************************/
/*  Convert from MySQL type to PlugDB type number                       */
/************************************************************************/
int MYSQLtoPLG(int mytype)
  {
  int type;

  switch (mytype) {
    case MYSQL_TYPE_TINY:
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
    case MYSQL_TYPE_DECIMAL:
#if !defined(ALPHA)
    case MYSQL_TYPE_NEWDECIMAL:
#endif   // !ALPHA)
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      type = TYPE_FLOAT;
      break;
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIME:
      type = TYPE_DATE;
      break;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
#if !defined(ALPHA)
    case MYSQL_TYPE_VARCHAR:
#endif   // !ALPHA)
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
      type = TYPE_STRING;
      break;
    default:
      type = TYPE_ERROR;
    } // endswitch mytype

  return type;
  } // end of MYSQLtoPLG

/************************************************************************/
/*  Returns the format corresponding to a MySQL date type.              */
/************************************************************************/
char *MyDateFmt(int mytype)
  {
  char *fmt;

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
/*  Returns the format corresponding to a MySQL date type.              */
/************************************************************************/
char *MyDateFmt(char *typname)
  {
  char *fmt;

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

