/************* jsonudf C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: jsonudf     Version 1.0                               */
/*  (C) Copyright to the author Olivier BERTRAND          2015         */
/*  This program are the JSON User Defined Functions     .             */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>
#include <mysql.h>

#include "global.h"
#include "plgdbsem.h"
#include "json.h"

extern "C" {
DllExport my_bool Json_Value_init(UDF_INIT*, UDF_ARGS*, char*);
DllExport char *Json_Value(UDF_INIT*, UDF_ARGS*, char*,
                         unsigned long*, char *, char *);
DllExport void Json_Value_deinit(UDF_INIT*);
DllExport my_bool Json_Array_init(UDF_INIT*, UDF_ARGS*, char*);
DllExport char *Json_Array(UDF_INIT*, UDF_ARGS*, char*,
                         unsigned long*, char *, char *);
DllExport void Json_Array_deinit(UDF_INIT*);
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
my_bool JsonInit(UDF_INIT *initid, char *message, unsigned long len)
{
  PGLOBAL g = PlugInit(NULL, len);

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
  initid->max_length = len - 512;
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
  char *str;
  PJVAL jvp = new(g) JVALUE;

  switch (args->arg_type[i]) {
    case STRING_RESULT:
      if ((str = MakePSZ(g, args, i))) {
        if (IsJson(args, i))
          jvp->SetValue(ParseJson(g, str, strlen(str), 0));
        else
          jvp->SetString(g, str);

        } // endif str

      break;
    case INT_RESULT:
      jvp->SetInteger(g, *(int*)args->args[i]);
      break;
    case REAL_RESULT:
      jvp->SetFloat(g, *(double*)args->args[i]);
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
  if (args->arg_count > 1) {
    strcpy(message, "Json_Value cannot accept more than 1 argument");
    return true;
    } // endif arg_count

  return JsonInit(initid, message, 1024);
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
  return JsonInit(initid, message, 8192);
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
/*  Make a Json Oject containing all the parameters.                   */
/***********************************************************************/
my_bool Json_Object_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  return JsonInit(initid, message, 8192);
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
  if (args->arg_count != 1) {
    strcpy(message, "Json_Array_Grp can only accept 1 argument");
    return true;
  } else if (JsonInit(initid, message, 16384))
    return true;

  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JARRAY;
  return false;
} // end of Json_Array_Grp_init

void Json_Array_Grp_add(UDF_INIT *initid, UDF_ARGS *args, 
                      char *is_null, char *error)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJAR    arp = (PJAR)g->Activityp;

  arp->AddValue(g, MakeValue(g, args, 0));
} // end of Json_Array_Grp_add

char *Json_Array_Grp(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                unsigned long *res_length, char *is_null, char *error)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJAR    arp = (PJAR)g->Activityp;

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
  if (args->arg_count != 2) {
    strcpy(message, "Json_Array_Grp can only accept 2 argument");
    return true;
  } else if (JsonInit(initid, message, 16384))
    return true;

  PGLOBAL g = (PGLOBAL)initid->ptr;

  PlugSubSet(g, g->Sarea, g->Sarea_Size);
  g->Activityp = (PACTIVITY)new(g) JOBJECT;
  return false;
} // end of Json_Object_Grp_init

void Json_Object_Grp_add(UDF_INIT *initid, UDF_ARGS *args, 
                      char *is_null, char *error)
{
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJOB    objp = (PJOB)g->Activityp;

  objp->SetValue(g, MakeValue(g, args, 0), MakePSZ(g, args, 1)); 
} // end of Json_Object_Grp_add

char *Json_Object_Grp(UDF_INIT *initid, UDF_ARGS *args, char *result, 
                unsigned long *res_length, char *is_null, char *error)
{
  char   *str;
  PGLOBAL g = (PGLOBAL)initid->ptr;
  PJOB    objp = (PJOB)g->Activityp;

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
} // end of Json_Object_Grp_clear

void Json_Object_Grp_deinit(UDF_INIT* initid)
{
  PlugExit((PGLOBAL)initid->ptr);
} // end of Json_Object_Grp_deinit


