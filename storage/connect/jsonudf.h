/******************** tabjson H Declares Source Code File (.H) *******************/
/*  Name: jsonudf.h   Version 1.1                                                */
/*                                                                               */
/*  (C) Copyright to the author Olivier BERTRAND          2015                   */
/*                                                                               */
/*  This file contains the JSON UDF function and class declares.                 */
/*********************************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "block.h"
#include "osutil.h"
#include "maputil.h"
#include "json.h"

#define UDF_EXEC_ARGS \
  UDF_INIT*, UDF_ARGS*, char*, unsigned long*, char*, char*

/*********************************************************************************/
/*  The JSON tree node. Can be an Object or an Array.                     	  	 */
/*********************************************************************************/
typedef struct _jnode {
	PSZ   Key;                    // The key used for object
	OPVAL Op;                     // Operator used for this node
	PVAL  CncVal;                 // To cont value used for OP_CNC
	PVAL  Valp;                   // The internal array VALUE
	int   Rank;                   // The rank in array
	int   Rx;                     // Read row number
	int   Nx;                     // Next to read row number
} JNODE, *PJNODE;

typedef class JSNX     *PJSNX;
typedef class JOUTPATH *PJTP;
typedef class JOUTALL  *PJTA;

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

	DllExport my_bool Json_Object_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object(UDF_EXEC_ARGS);
	DllExport void Json_Object_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_Nonull_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object_Nonull(UDF_EXEC_ARGS);
	DllExport void Json_Object_Nonull_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_Add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object_Add(UDF_EXEC_ARGS);
	DllExport void Json_Object_Add_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_Delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object_Delete(UDF_EXEC_ARGS);
	DllExport void Json_Object_Delete_deinit(UDF_INIT*);

	DllExport my_bool Json_Object_List_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Object_List(UDF_EXEC_ARGS);
	DllExport void Json_Object_List_deinit(UDF_INIT*);

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

	DllExport my_bool Json_Get_Item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Get_Item(UDF_EXEC_ARGS);
	DllExport void Json_Get_Item_deinit(UDF_INIT*);

	DllExport my_bool JsonGetString_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *JsonGetString(UDF_EXEC_ARGS);
	DllExport void JsonGetString_deinit(UDF_INIT*);

	DllExport my_bool JsonGetInt_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long JsonGetInt(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void JsonGetInt_deinit(UDF_INIT*);

	DllExport my_bool JsonGetReal_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport double JsonGetReal(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void JsonGetReal_deinit(UDF_INIT*);

	DllExport my_bool JsonLocate_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *JsonLocate(UDF_EXEC_ARGS);
	DllExport void JsonLocate_deinit(UDF_INIT*);

	DllExport my_bool Json_Locate_All_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_Locate_All(UDF_EXEC_ARGS);
	DllExport void Json_Locate_All_deinit(UDF_INIT*);

	DllExport my_bool Json_File_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Json_File(UDF_EXEC_ARGS);
	DllExport void Json_File_deinit(UDF_INIT*);

	DllExport my_bool Jfile_Make_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Jfile_Make(UDF_EXEC_ARGS);
	DllExport void Jfile_Make_deinit(UDF_INIT*);

	DllExport my_bool Bson_Array_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Bson_Array(UDF_EXEC_ARGS);
	DllExport void Bson_Array_deinit(UDF_INIT*);

	DllExport my_bool Bson_Object_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Bson_Object(UDF_EXEC_ARGS);
	DllExport void Bson_Object_deinit(UDF_INIT*);

	DllExport my_bool Bson_File_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *Bson_File(UDF_EXEC_ARGS);
	DllExport void Bson_File_deinit(UDF_INIT*);
} // extern "C"

/*********************************************************************************/
/*  Structure JPN. Used to make the locate path.                                 */
/*********************************************************************************/
typedef struct _jpn {
	enum JTYP Type;
	PSZ       Key;
	int       N;
} JPN, *PJPN;

/*********************************************************************************/
/*  Class JSNX: JSON access method.                                              */
/*********************************************************************************/
class JSNX : public BLOCK {
public:
	// Constructors
	JSNX(PGLOBAL g, PJSON row, int type, int len = 64, int prec = 0);

	// Implementation
	int     GetPrecision(void) {return Prec;}
	PVAL    GetValue(void) {return Value;}

	// Methods
	my_bool SetJpath(PGLOBAL g, char *path, my_bool jb = false);
	my_bool ParseJpath(PGLOBAL g);
	void    ReadValue(PGLOBAL g);
	PJVAL   GetJson(PGLOBAL g);
	char   *Locate(PGLOBAL g, PJSON jsp, PJVAL jvp, int k = 1);
	char   *LocateAll(PGLOBAL g, PJSON jsp, PJVAL jvp, int mx = 10);

protected:
	my_bool SetArrayOptions(PGLOBAL g, char *p, int i, PSZ nm);
	PVAL    GetColumnValue(PGLOBAL g, PJSON row, int i);
	PJVAL   GetValue(PGLOBAL g, PJSON row, int i);
	PVAL    ExpandArray(PGLOBAL g, PJAR arp, int n);
	PVAL    CalculateArray(PGLOBAL g, PJAR arp, int n);
	PVAL    MakeJson(PGLOBAL g, PJSON jsp);
	void    SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val, int n);
	my_bool LocateArray(PJAR jarp);
	my_bool LocateObject(PJOB jobp);
	my_bool LocateValue(PJVAL jvp);
	my_bool LocateArrayAll(PJAR jarp);
	my_bool LocateObjectAll(PJOB jobp);
	my_bool LocateValueAll(PJVAL jvp);
	my_bool CompareTree(PJSON jp1, PJSON jp2);
	my_bool AddPath(void);

	// Default constructor not to be used
	JSNX(void) {}

	// Members
	PJSON    Row;
	PJVAL    Jvalp;
	PJPN     Jpnp;
	JOUTSTR *Jp;
	JNODE   *Nodes;               // The intermediate objects
	PVAL     Value;
	PVAL     MulVal;              // To value used by multiple column
	char    *Jpath;               // The json path
	int      Buf_Type;
	int      Long;
	int      Prec;
	int      Nod;                 // The number of intermediate objects
	int      Xnod;                // Index of multiple values
	int      K;										// Kth item to locate
	int      I;										// Index of JPN
	int      Imax;								// Max number of JPN's
	int      B;										// Index base
	my_bool  Xpd;                 // True for expandable column
	my_bool  Parsed;              // True when parsed
	my_bool  Found;								// Item found by locate
}; // end of class JSNX
