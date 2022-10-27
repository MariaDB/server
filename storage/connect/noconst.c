/***********************************************************************/
/*  (C) Copyright to the author Olivier BERTRAND          2015         */
/***********************************************************************/
#include <my_global.h>
#include <mysqld.h>
#include <string.h>

#if defined(__WIN__)
#define DllExport  __declspec( dllexport )
#else   // !__WIN__
#define DllExport
#endif  // !__WIN__

extern "C" {
	DllExport my_bool noconst_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *noconst(UDF_INIT*, UDF_ARGS*, char*, unsigned long*, char*, char*);
} // extern "C"

/***********************************************************************/
/*  Returns its argument saying it is not a constant.                  */
/***********************************************************************/
my_bool noconst_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  if (args->arg_count != 1 || args->arg_type[0] != STRING_RESULT) {
    strcpy(message, "noconst unique argument must be a string");
    return true;
  } // endif arg

  initid->const_item = false;   // The trick!
  return false;
} // end of noconst_init

char *noconst(UDF_INIT *initid, UDF_ARGS *args, char *result,
  unsigned long *res_length, char *, char *)
{
  return args->args[0];
} // end of noconst

