/*************** tabjson H Declares Source Code File (.H) **************/
/*  Name: jsonudf.h   Version 1.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2015         */
/*                                                                     */
/*  This file contains the JSON UDF function and classe declares.      */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "block.h"
#include "osutil.h"
#include "maputil.h"
#include "json.h"

#define UDF_EXEC_ARGS \
  UDF_INIT*, UDF_ARGS*, char*, unsigned long*, char*, char*

/***********************************************************************/
/*  The JSON tree node. Can be an Object or an Array.           	  	 */
/***********************************************************************/
typedef struct _jnode {
	PSZ   Key;                    // The key used for object
	OPVAL Op;                     // Operator used for this node
	PVAL  CncVal;                 // To cont value used for OP_CNC
	PVAL  Valp;                   // The internal array VALUE
	int   Rank;                   // The rank in array
	int   Rx;                     // Read row number
	int   Nx;                     // Next to read row number
} JNODE, *PJNODE;

typedef class JSNX *PJSNX;
typedef class JOUTPATH *PJTP;

extern "C" {
	DllExport my_bool Json_Value_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Value(UDF_EXEC_ARGS);
	DllExport void Json_Value_deinit(UDF_INIT*);

	DllExport my_bool Json_Array_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Array(UDF_EXEC_ARGS);
	DllExport void Json_Array_deinit(UDF_INIT*);

	DllExport my_bool Json_Array_Add_Values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Array_Add_Values(UDF_EXEC_ARGS);
	DllExport void Json_Array_Add_Values_deinit(UDF_INIT*);

	DllExport my_bool Json_Array_Add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Array_Add(UDF_EXEC_ARGS);
	DllExport void Json_Array_Add_deinit(UDF_INIT*);

	DllExport my_bool Json_Array_Delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Array_Delete(UDF_EXEC_ARGS);
	DllExport void Json_Array_Delete_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_Nonull_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object_Nonull(UDF_EXEC_ARGS);
	DllExport void Json_Object_Nonull_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_Add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object_Add(UDF_EXEC_ARGS);
	DllExport void Json_Object_Add_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_Delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object_Delete(UDF_EXEC_ARGS);
	DllExport void Json_Object_Delete_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object(UDF_EXEC_ARGS);
	DllExport void Json_Object_deinit(UDF_INIT*);

	DllExport my_bool Json_Array_Grp_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport void Json_Array_Grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
	DllExport char *Json_Array_Grp(UDF_EXEC_ARGS);
	DllExport void Json_Array_Grp_clear(UDF_INIT *, char *, char *);
	DllExport void Json_Array_Grp_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_Grp_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport void Json_Object_Grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
	DllExport char *Json_Object_Grp(UDF_EXEC_ARGS);
	DllExport void Json_Object_Grp_clear(UDF_INIT *, char *, char *);
	DllExport void Json_Object_Grp_deinit(UDF_INIT*);

	DllExport my_bool Json_Get_String_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Get_String(UDF_EXEC_ARGS);
	DllExport void Json_Get_String_deinit(UDF_INIT*);

	DllExport my_bool Json_Get_Int_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long Json_Get_Int(UDF_EXEC_ARGS);
	DllExport void Json_Get_Int_deinit(UDF_INIT*);

	DllExport my_bool Json_Get_Real_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport double Json_Get_Real(UDF_EXEC_ARGS);
	DllExport void Json_Get_Real_deinit(UDF_INIT*);

	DllExport my_bool Json_Locate_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Locate(UDF_EXEC_ARGS);
	DllExport void Json_Locate_deinit(UDF_INIT*);

	DllExport my_bool Json_File_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_File(UDF_EXEC_ARGS);
	DllExport void Json_File_deinit(UDF_INIT*);

	DllExport my_bool Json_Make_File_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Make_File(UDF_EXEC_ARGS);
	DllExport void Json_Make_File_deinit(UDF_INIT*);
} // extern "C"

/***********************************************************************/
/*  Class JSNX: JSON access method.                                    */
/***********************************************************************/
class JSNX : public BLOCK {
public:
	// Constructors
	JSNX(PGLOBAL g, PJSON row, int type, int len = 64, int prec = 0);

	// Implementation
	int     GetPrecision(void) {return Prec;}
	PVAL    GetValue(void) {return Value;}

	// Methods
	my_bool SetJpath(PGLOBAL g, char *path);
	my_bool ParseJpath(PGLOBAL g);
	void    ReadValue(PGLOBAL g);
	PJVAL   GetJson(PGLOBAL g);
	char   *Locate(PGLOBAL g, PJSON jsp, char *what, 
	               enum Item_result type, unsigned long len);

protected:
	my_bool CheckExpand(PGLOBAL g, int i, PSZ nm, my_bool b);
	my_bool SetArrayOptions(PGLOBAL g, char *p, int i, PSZ nm);
	PVAL    GetColumnValue(PGLOBAL g, PJSON row, int i);
	PJVAL   GetValue(PGLOBAL g, PJSON row, int i);
	PVAL    ExpandArray(PGLOBAL g, PJAR arp, int n);
	PVAL    CalculateArray(PGLOBAL g, PJAR arp, int n);
	PVAL    MakeJson(PGLOBAL g, PJSON jsp);
	void    SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val, int n);
//PJSON   GetRow(PGLOBAL g);
	my_bool LocateArray(PJAR jarp);
	my_bool LocateObject(PJOB jobp);
	my_bool LocateValue(PJVAL jvp);

	// Default constructor not to be used
	JSNX(void) {}

	// Members
	PJSON   Row;
	PVAL    Value;
	PVAL    MulVal;               // To value used by multiple column
	PJTP    Jp;
	JNODE  *Nodes;                // The intermediate objects
	char   *Jpath;                // The json path
	int     Buf_Type;
	int     Long;
	int     Prec;
	int     Nod;                  // The number of intermediate objects
	int     Xnod;                 // Index of multiple values
	int     B;										// Index base
	my_bool Xpd;                  // True for expandable column
	my_bool Parsed;               // True when parsed
}; // end of class JSNX

/***********************************************************************/
/* Class JOUTPATH. Used to make the locate path.                       */
/***********************************************************************/
class JOUTPATH : public JOUTSTR {
public:
	JOUTPATH(PGLOBAL g, char *w, enum Item_result type, unsigned long len)
		: JOUTSTR(g) {What = w; Type = type; Len = len; Found = false;}

	// Members
	enum Item_result Type;
	unsigned long    Len;
	char            *What;
	my_bool          Found;
}; // end of class JOUTPATH
