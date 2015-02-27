/************* jsonudf C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: jsonudf     Version 1.0                               */
/*  (C) Copyright to the author Olivier BERTRAND          2015         */
/*  This program are the JSON User Defined Functions     .             */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>
#include <mysqld.h>
#include <mysql.h>
#include <sql_error.h>

#include "global.h"
#include "plgdbsem.h"
#include "json.h"

#define MEMFIX  512

uint GetJsonGrpSize(void);

extern "C" {
DllExport my_bool Json_Value_init(UDF_INIT*, UDF_ARGS*, char*);
DllExport char *Json_Value(UDF_INIT*, UDF_ARGS*, char*,
                         unsigned long*, char *, char *);
DllExport void Json_Value_deinit(UDF_INIT*);
DllExport my_bool Json_Array_init(UDF_INIT*, UDF_ARGS*, char*);
DllExport char *Json_Array(UDF_INIT*, UDF_ARGS*, char*,
                         unsigned long*, char *, char *);
DllExport void Json_Array_deinit(UDF_INIT*);
DllExport my_bool Json_Array_Add_init(UDF_INIT*, UDF_ARGS*, char*);
DllExport char *Json_Array_Add(UDF_INIT*, UDF_ARGS*, char*,
                         unsigned long*, char *, char *);
DllExport void Json_Array_Add_deinit(UDF_INIT*);
DllExport my_bool Json_Object_init(UDF_INIT*, UDF_ARGS*, char*);
DllExport char *Json_Object(UDF_INIT*, UDF_ARGS*, char*,
                         unsigned long*, char *, char *);
DllExport void Json_Object_deinit(UDF_INIT*);
DllExport my_bool Json_Array_Grp_init(UDF_INIT*, UDF_ARGS*, char*);
DllExport void Json_Array_Grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
DllExport char *Json_Array_Grp(UDF_INIT*, UDF_ARGS*, char*,
                         unsigned long*, char *, char *);
DllExport void Json_Array_Grp_clear(UDF_INIT *, char *, char *);
DllExport void Json_Array_Grp_deinit(UDF_INIT*);
DllExport my_bool Json_Object_Grp_init(UDF_INIT*, UDF_ARGS*, char*);
DllExport void Json_Object_Grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
DllExport char *Json_Object_Grp(UDF_INIT*, UDF_ARGS*, char*,
                         unsigned long*, char *, char *);
DllExport void Json_Object_Grp_clear(UDF_INIT *, char *, char *);
DllExport void Json_Object_Grp_deinit(UDF_INIT*);
} // extern "C"

/***********************************************************************/
/*  Allocate and initialise the memory area.                           */
/***********************************************************************/
static my_bool JsonInit(UDF_INIT *initid, char *message, 
                        unsigned long reslen, unsigned long memlen)
{
  PGLOBAL g = PlugInit(NULL, memlen);

  if (!g) {
    strcpy(message, "Allocation error");
    return true;
  } else if (g->Sarea_Size == 0) {
    strcpy(message, g->Message);
    PlugExit(g);
    return true;
  } else
    initid->ptr = (char*)g;

  initid->maybe_null = false;
  initid->max_length = reslen;
  return false;
} // end of Json_Object_init

/***********************************************************************/
/*  Returns true if the argument is a JSON string.                     */
/***********************************************************************/
static my_bool IsJson(UDF_ARGS *args, int i)
{
  return (args->arg_type[i] == STRING_RESULT &&
          !strnicmp(args->attributes[i], "Json_", 5));
} // end of IsJson

/***********************************************************************/
/*  Calculate the reslen and memlen needed by a function.              */
/***********************************************************************/
static my_bool CalcLen(UDF_ARGS *args, my_bool obj,
                       unsigned long& reslen, unsigned long& memlen)
{
  unsigned long i, k;
  reslen = args->arg_count + 2;

  // Calculate the result max length
  for (i = 0; i < args->arg_count; i++) {
    if (obj) {
      if (!(k = args->attribute_lengths[i]))
        k = strlen(args->attributes[i]);

      reslen += (k + 3);     // For quotes and :
      } // endif obj

    switch (args->arg_type[i]) {
      case STRING_RESULT:
        if (IsJson(args, i))
          reslen += args->lengths[i];
        else
          reslen += (args->lengths[i] + 1) * 2;   // Pessimistic !
  
        break;
      case INT_RESULT:
        reslen += 20;
        break;
      case REAL_RESULT:
        reslen += 31;
        break;
      case DECIMAL_RESULT:
        reslen += (args->lengths[i] + 7);   // 6 decimals
        break;
      case TIME_RESULT:
      case ROW_RESULT:
      case IMPOSSIBLE_RESULT:
      default:
        // What should we do here ?
        break;
      } // endswitch arg_type

    } // endfor i

  // Calculate the amount of memory needed
  memlen = MEMFIX + sizeof(JOUTSTR) + reslen;

  for (i = 0; i < args->arg_count; i++) {
    memlen += (args->lengths[i] + sizeof(JVALUE));

    if (obj) {
      if (!(k = args->attribute_lengths[i]))
        k = strlen(args->attributes[i]);

      memlen += (k + sizeof(JOBJECT) + sizeof(JPAIR));
    } else
      memlen += sizeof(JARRAY);

    switch (args->arg_type[i]) {
      case STRING_RESULT:
        if (IsJson(args, i))
          memlen += args->lengths[i] * 5;  // Estimate parse memory
  
        memlen += sizeof(TYPVAL<PSZ>);
        break;
      case INT_RESULT:
        memlen += sizeof(TYPVAL<int>);
        break;
      case REAL_RESULT:
      case DECIMAL_RESULT:
        memlen += sizeof(TYPVAL<double>);
        break;
      case TIME_RESULT:
      case ROW_RESULT:
      case IMPOSSIBLE_RESULT:
      default:
        // What should we do here ?
        break;
      } // endswitch arg_type

    } // endfor i

  return false;
} // end of CalcLen

/***********************************************************************/
/*  Make a zero terminated string from the passed argument.            */
/***********************************************************************/
static PSZ MakePSZ(PGLOBAL g, UDF_ARGS *args, int i)
{
  if (args->args[i]) {
    int n = args->lengths[i];
    PSZ s = (PSZ)PlugSubAlloc(g, NULL, n + 1);

    memcpy(s, args->args[i], n);
    s[n] = 0;
    return s;
  } else
    return NULL;

} // end of MakePSZ

/***********************************************************************/
/*  Make a valid key from the passed argument.                         */
/***********************************************************************/
static PSZ MakeKey(PGLOBAL g, UDF_ARGS *args, int i)
{
  int  n = args->attribute_lengths[i];
  bool b;  // true if attribute is zero terminated
  PSZ  p, s = args->attributes[i];

  if (s && *s && (n || *s == '\'')) {
    if ((b = (!n || !s[n])))
      n = strlen(s);

    if (n > 5 && IsJson(args, i)) {
      s += 5;
      n -= 5;
    } else if (*s == '\'' && s[n-1] == '\'') {
      s++;
      n -= 2;
      b = false;
      } // endif *s

    if (n < 1)
      return "Key";

    if (!b) {
      p = (PSZ)PlugSubAlloc(g, NULL, n + 1);
      memcpy(p, s, n);
      p[n] = 0;
      s = p;
      } // endif b

    } // endif s

  return s;
} // end of MakeKey

/***********************************************************************/
/*  Make a JSON value from the passed argument.                        */
/***********************************************************************/
static PJVAL MakeValue(PGLOBAL g, UDF_ARGS *args, int i)
{
  char *sap = args->args[i];
  PJSON jsp;
  PJVAL jvp = new(g) JVALUE;

  if (sap) switch (args->arg_type[i]) {
    case STRING_RESULT:
      if (args->lengths[i]) {
        if (IsJson(args, i)) {
          if (!(jsp = ParseJson(g, sap, args->lengths[i], 0)))
            push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, 0, 
                         g->Message);

          if (jsp && jsp->GetType() == TYPE_JVAL)
            jvp = (PJVAL)jsp;
          else
            jvp->SetValue(jsp);

        } else
          jvp->SetString(g, MakePSZ(g, args, i));

        } // endif str

      break;
    case INT_RESULT:
      jvp->SetInteger(g, *(int*)sap);
      break;
    case REAL_RESULT:
      jvp->SetFloat(g, *(double*)sap);
      break;
    case DECIMAL_RESULT:
      jvp->SetFloat(g, atof(MakePSZ(g, args, i)));
      break;
    case TIME_RESULT:
    case ROW_RESULT:
    case IMPOSSIBLE_RESULT:
    default:
      break;
    } // endswitch arg_type

  return jvp;
} // end of MakeValue

/***********************************************************************/
/*  Make a Json value containing the parameter.                        */
/***********************************************************************/
my_bool Json_Value_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  if (args->arg_count > 1) {
    strcpy(message, "Json_Value cannot accept more than 1 argument");
    return true;
  } else
    CalcLen(args, false, reslen, memlen);

  return JsonInit(initid, message, reslen, memlen);
} // end of Json_Value_init

char *Json_Value(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                unsigned long *res_length, char *is_null, char *error)
{
  char   *str;
  PJVAL   jvp;
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  jvp = MakeValue(g, args, 0);

  if (!(str = Serialize(g, jvp, NULL, 0)))
    str = strcpy(result, g->Message);

  *res_length = strlen(str);
  return str;
} // end of Json_Value

void Json_Value_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Value_deinit

/***********************************************************************/
/*  Make a Json array containing all the parameters.                   */
/***********************************************************************/
my_bool Json_Array_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  CalcLen(args, false, reslen, memlen);
  return JsonInit(initid, message, reslen, memlen);
} // end of Json_Array_init

char *Json_Array(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                unsigned long *res_length, char *is_null, char *error)
{
  char   *str;
  uint    i;
  PJAR    arp;
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  arp = new(g) JARRAY;

  for (i = 0; i < args->arg_count; i++)
    arp->AddValue(g, MakeValue(g, args, i));

  arp->InitArray(g);

  if (!(str = Serialize(g, arp, NULL, 0)))
    str = strcpy(result, g->Message);

  *res_length = strlen(str);
  return str;
} // end of Json_Array

void Json_Array_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Array_deinit

/***********************************************************************/
/*  Add values to a Json array.                                        */
/***********************************************************************/
my_bool Json_Array_Add_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  if (args->arg_count < 2) {
    strcpy(message, "Json_Value_Add must have at least 2 arguments");
    return true;
  } else if (!IsJson(args, 0)) {
    strcpy(message, "Json_Value_Add first argument must be a json array");
    return true;
  } else
    CalcLen(args, false, reslen, memlen);

  return JsonInit(initid, message, reslen, memlen);
} // end of Json_Array_Add_init

char *Json_Array_Add(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                unsigned long *res_length, char *is_null, char *error)
{
  char   *str;
  PJVAL   jvp;
  PJAR    arp;
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  jvp = MakeValue(g, args, 0);

  if (jvp->GetValType() != TYPE_JAR) {
    arp = new(g) JARRAY;
    arp->AddValue(g, jvp);
  } else
    arp = jvp->GetArray();

  for (uint i = 1; i < args->arg_count; i++)
    arp->AddValue(g, MakeValue(g, args, i));

  arp->InitArray(g);

  if (!(str = Serialize(g, arp, NULL, 0)))
    str = strcpy(result, g->Message);

  *res_length = strlen(str);
  return str;
} // end of Json_Array_Add

void Json_Array_Add_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Array_Add_deinit

/***********************************************************************/
/*  Make a Json Oject containing all the parameters.                   */
/***********************************************************************/
my_bool Json_Object_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen;

  CalcLen(args, true, reslen, memlen);
  return JsonInit(initid, message, reslen, memlen);
} // end of Json_Object_init

char *Json_Object(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                  unsigned long *res_length, char *is_null, char *error)
{
  char   *str;
  uint    i;
  PJOB    objp;
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  objp = new(g) JOBJECT;

  for (i = 0; i < args->arg_count; i++)
    objp->SetValue(g, MakeValue(g, args, i), MakeKey(g, args, i));

  if (!(str = Serialize(g, objp, NULL, 0)))
    str = strcpy(result, g->Message);

  *res_length = strlen(str);
  return str;
} // end of Json_Object

void Json_Object_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_deinit

/***********************************************************************/
/*  Make a Json array from values comming from rows.                   */
/***********************************************************************/
my_bool Json_Array_Grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen, n = GetJsonGrpSize();

  if (args->arg_count != 1) {
    strcpy(message, "Json_Array_Grp can only accept 1 argument");
    return true;
  } else 
    CalcLen(args, false, reslen, memlen);
  
  reslen *= n;
  memlen += ((memlen - MEMFIX) * (n - 1));

  if (JsonInit(initid, message, reslen, memlen))
    return true;

  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JARRAY;
  g->N = (int)n;
  return false;
} // end of Json_Array_Grp_init

void Json_Array_Grp_add(UDF_INIT *initid, UDF_ARGS *args, 
                      char *is_null, char *error)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJAR    arp = (PJAR)g->Activityp;

  if (g->N-- > 0)
    arp->AddValue(g, MakeValue(g, args, 0));

} // end of Json_Array_Grp_add

char *Json_Array_Grp(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                unsigned long *res_length, char *is_null, char *error)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJAR    arp = (PJAR)g->Activityp;

  if (g->N < 0)
    push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, 0, 
                 "Result truncated to json_grp_size values");

  arp->InitArray(g);

  if (!(str = Serialize(g, arp, NULL, 0)))
    str = strcpy(result, g->Message);

  *res_length = strlen(str);
  return str;
} // end of Json_Array_Grp

void Json_Array_Grp_clear(UDF_INIT *initid, char *is_null, char *error)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JARRAY;
  g->N = GetJsonGrpSize();
} // end of Json_Array_Grp_clear

void Json_Array_Grp_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Array_Grp_deinit

/***********************************************************************/
/*  Make a Json object from values comming from rows.                  */
/***********************************************************************/
my_bool Json_Object_Grp_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  unsigned long reslen, memlen, n = GetJsonGrpSize();

  if (args->arg_count != 2) {
    strcpy(message, "Json_Array_Grp can only accept 2 argument");
    return true;
  } else 
    CalcLen(args, true, reslen, memlen);
  
  reslen *= n;
  memlen += ((memlen - MEMFIX) * (n - 1));

  if (JsonInit(initid, message, reslen, memlen))
    return true;

  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JOBJECT;
  g->N = (int)n;
  return false;
} // end of Json_Object_Grp_init

void Json_Object_Grp_add(UDF_INIT *initid, UDF_ARGS *args, 
                      char *is_null, char *error)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJOB    objp = (PJOB)g->Activityp;

  if (g->N-- > 0)
    objp->SetValue(g, MakeValue(g, args, 0), MakePSZ(g, args, 1));

} // end of Json_Object_Grp_add

char *Json_Object_Grp(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                unsigned long *res_length, char *is_null, char *error)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJOB    objp = (PJOB)g->Activityp;

  if (g->N < 0)
    push_warning(current_thd, Sql_condition::WARN_LEVEL_WARN, 0, 
                 "Result truncated to json_grp_size values");

  if (!(str = Serialize(g, objp, NULL, 0)))
    str = strcpy(result, g->Message);

  *res_length = strlen(str);
  return str;
} // end of Json_Object_Grp

void Json_Object_Grp_clear(UDF_INIT *initid, char *is_null, char *error)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JOBJECT;
  g->N = GetJsonGrpSize();
} // end of Json_Object_Grp_clear

void Json_Object_Grp_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_Grp_deinit


