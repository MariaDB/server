/******************** tabjson H Declares Source Code File (.H) *******************/
/*  Name: jsonudf.h   Version 1.4                                                */
/*                                                                               */
/*  (C) Copyright to the author Olivier BERTRAND          2015-2020              */
/*                                                                               */
/*  This file contains the JSON UDF function and class declares.                 */
/*********************************************************************************/
#pragma once
#include "global.h"
#include "plgdbsem.h"
#include "block.h"
#include "osutil.h"
#include "maputil.h"
#include "json.h"

#define UDF_EXEC_ARGS \
  UDF_INIT*, UDF_ARGS*, char*, unsigned long*, char*, char*

// BSON size should be equal on Linux and Windows
#define BMX 255
typedef struct BSON* PBSON;

/***********************************************************************/
/*  Structure used to return binary json to Json UDF functions.        */
/***********************************************************************/
struct BSON {
	char    Msg[BMX + 1];
	char   *Filename;
	PGLOBAL G;
	int     Pretty;
	ulong   Reslen;
	my_bool Changed;
	PJSON   Top;
	PJSON   Jsp;
	PBSON   Bsp;
}; // end of struct BSON

PBSON JbinAlloc(PGLOBAL g, UDF_ARGS* args, ulong len, PJSON jsp);

/*********************************************************************************/
/*  The JSON tree node. Can be an Object or an Array.                     	  	 */
/*********************************************************************************/
typedef struct _jnode {
	PSZ   Key;                    // The key used for object
	OPVAL Op;                     // Operator used for this node
	PVAL  CncVal;                 // To cont value used for OP_CNC
	int   Rank;                   // The rank in array
	int   Rx;                     // Read row number
	int   Nx;                     // Next to read row number
} JNODE, *PJNODE;

typedef class JSNX     *PJSNX;

/*********************************************************************************/
/*  The JSON utility functions.                     	  	                       */
/*********************************************************************************/
bool    IsNum(PSZ s);
char   *NextChr(PSZ s, char sep);
char   *GetJsonNull(void);
uint    GetJsonGrpSize(void);
my_bool JsonSubSet(PGLOBAL g, my_bool b = false);
my_bool CalcLen(UDF_ARGS* args, my_bool obj, unsigned long& reslen,
	              unsigned long& memlen, my_bool mod = false);
my_bool JsonInit(UDF_INIT* initid, UDF_ARGS* args, char* message, my_bool mbn,
								 unsigned long reslen, unsigned long memlen,
								 unsigned long more = 0);
my_bool CheckMemory(PGLOBAL g, UDF_INIT* initid, UDF_ARGS* args, uint n,
	                  my_bool m, my_bool obj = false, my_bool mod = false);
PSZ     MakePSZ(PGLOBAL g, UDF_ARGS* args, int i);
int     IsJson(UDF_ARGS* args, uint i, bool b = false);
char   *GetJsonFile(PGLOBAL g, char* fn);

/*********************************************************************************/
/*  The JSON UDF functions.                                               	  	 */
/*********************************************************************************/
extern "C" {
	DllExport my_bool jsonvalue_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jsonvalue(UDF_EXEC_ARGS);
	DllExport void jsonvalue_deinit(UDF_INIT*);

	DllExport my_bool json_make_array_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_make_array(UDF_EXEC_ARGS);
	DllExport void json_make_array_deinit(UDF_INIT*);

	DllExport my_bool json_array_add_values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_array_add_values(UDF_EXEC_ARGS);
	DllExport void json_array_add_values_deinit(UDF_INIT*);

	DllExport my_bool json_array_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_array_add(UDF_EXEC_ARGS);
	DllExport void json_array_add_deinit(UDF_INIT*);

	DllExport my_bool json_array_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_array_delete(UDF_EXEC_ARGS);
	DllExport void json_array_delete_deinit(UDF_INIT*);

	DllExport my_bool jsonsum_int_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long jsonsum_int(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void jsonsum_int_deinit(UDF_INIT*);

	DllExport my_bool jsonsum_real_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport double jsonsum_real(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void jsonsum_real_deinit(UDF_INIT*);

	DllExport my_bool jsonavg_real_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport double jsonavg_real(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void jsonavg_real_deinit(UDF_INIT*);

	DllExport my_bool json_make_object_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_make_object(UDF_EXEC_ARGS);
	DllExport void json_make_object_deinit(UDF_INIT*);

	DllExport my_bool json_object_nonull_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_object_nonull(UDF_EXEC_ARGS);
	DllExport void json_object_nonull_deinit(UDF_INIT*);

	DllExport my_bool json_object_key_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_object_key(UDF_EXEC_ARGS);
	DllExport void json_object_key_deinit(UDF_INIT*);

	DllExport my_bool json_object_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_object_add(UDF_EXEC_ARGS);
	DllExport void json_object_add_deinit(UDF_INIT*);

	DllExport my_bool json_object_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_object_delete(UDF_EXEC_ARGS);
	DllExport void json_object_delete_deinit(UDF_INIT*);

	DllExport my_bool json_object_list_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_object_list(UDF_EXEC_ARGS);
	DllExport void json_object_list_deinit(UDF_INIT*);

	DllExport my_bool json_object_values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_object_values(UDF_EXEC_ARGS);
	DllExport void json_object_values_deinit(UDF_INIT*);

	DllExport my_bool jsonset_grp_size_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long jsonset_grp_size(UDF_INIT*, UDF_ARGS*, char*, char*);

	DllExport my_bool jsonget_grp_size_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long jsonget_grp_size(UDF_INIT*, UDF_ARGS*, char*, char*);

	DllExport my_bool json_array_grp_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport void json_array_grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
	DllExport char *json_array_grp(UDF_EXEC_ARGS);
	DllExport void json_array_grp_clear(UDF_INIT *, char *, char *);
	DllExport void json_array_grp_deinit(UDF_INIT*);

	DllExport my_bool json_object_grp_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport void json_object_grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
	DllExport char *json_object_grp(UDF_EXEC_ARGS);
	DllExport void json_object_grp_clear(UDF_INIT *, char *, char *);
	DllExport void json_object_grp_deinit(UDF_INIT*);

	DllExport my_bool json_item_merge_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_item_merge(UDF_EXEC_ARGS);
	DllExport void json_item_merge_deinit(UDF_INIT*);

	DllExport my_bool json_get_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_get_item(UDF_EXEC_ARGS);
	DllExport void json_get_item_deinit(UDF_INIT*);

	DllExport my_bool jsonget_string_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jsonget_string(UDF_EXEC_ARGS);
	DllExport void jsonget_string_deinit(UDF_INIT*);

	DllExport my_bool jsonget_int_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long jsonget_int(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void jsonget_int_deinit(UDF_INIT*);

	DllExport my_bool jsonget_real_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport double jsonget_real(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void jsonget_real_deinit(UDF_INIT*);

	DllExport my_bool jsoncontains_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long jsoncontains(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void jsoncontains_deinit(UDF_INIT*);

	DllExport my_bool jsonlocate_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jsonlocate(UDF_EXEC_ARGS);
	DllExport void jsonlocate_deinit(UDF_INIT*);

	DllExport my_bool json_locate_all_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_locate_all(UDF_EXEC_ARGS);
	DllExport void json_locate_all_deinit(UDF_INIT*);

	DllExport my_bool jsoncontains_path_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long jsoncontains_path(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void jsoncontains_path_deinit(UDF_INIT*);

	DllExport my_bool json_set_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_set_item(UDF_EXEC_ARGS);
	DllExport void json_set_item_deinit(UDF_INIT*);

	DllExport my_bool json_insert_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_insert_item(UDF_EXEC_ARGS);
	DllExport void json_insert_item_deinit(UDF_INIT*);

	DllExport my_bool json_update_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_update_item(UDF_EXEC_ARGS);
	DllExport void json_update_item_deinit(UDF_INIT*);

	DllExport my_bool json_file_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_file(UDF_EXEC_ARGS);
	DllExport void json_file_deinit(UDF_INIT*);

	DllExport my_bool jfile_make_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jfile_make(UDF_EXEC_ARGS);
	DllExport void jfile_make_deinit(UDF_INIT*);

	DllExport my_bool jbin_array_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_array(UDF_EXEC_ARGS);
	DllExport void jbin_array_deinit(UDF_INIT*);

	DllExport my_bool jbin_array_add_values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_array_add_values(UDF_EXEC_ARGS);
	DllExport void jbin_array_add_values_deinit(UDF_INIT*);

	DllExport my_bool jbin_array_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_array_add(UDF_EXEC_ARGS);
	DllExport void jbin_array_add_deinit(UDF_INIT*);

	DllExport my_bool jbin_array_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_array_delete(UDF_EXEC_ARGS);
	DllExport void jbin_array_delete_deinit(UDF_INIT*);

	DllExport my_bool jbin_object_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_object(UDF_EXEC_ARGS);
	DllExport void jbin_object_deinit(UDF_INIT*);

	DllExport my_bool jbin_object_nonull_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_object_nonull(UDF_EXEC_ARGS);
	DllExport void jbin_object_nonull_deinit(UDF_INIT*);

	DllExport my_bool jbin_object_key_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_object_key(UDF_EXEC_ARGS);
	DllExport void jbin_object_key_deinit(UDF_INIT*);

	DllExport my_bool jbin_object_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_object_add(UDF_EXEC_ARGS);
	DllExport void jbin_object_add_deinit(UDF_INIT*);

	DllExport my_bool jbin_object_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_object_delete(UDF_EXEC_ARGS);
	DllExport void jbin_object_delete_deinit(UDF_INIT*);

	DllExport my_bool jbin_object_list_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_object_list(UDF_EXEC_ARGS);
	DllExport void jbin_object_list_deinit(UDF_INIT*);

	DllExport my_bool jbin_get_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_get_item(UDF_EXEC_ARGS);
	DllExport void jbin_get_item_deinit(UDF_INIT*);

	DllExport my_bool jbin_item_merge_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_item_merge(UDF_EXEC_ARGS);
	DllExport void jbin_item_merge_deinit(UDF_INIT*);

	DllExport my_bool jbin_set_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_set_item(UDF_EXEC_ARGS);
	DllExport void jbin_set_item_deinit(UDF_INIT*);

	DllExport my_bool jbin_insert_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_insert_item(UDF_EXEC_ARGS);
	DllExport void jbin_insert_item_deinit(UDF_INIT*);

	DllExport my_bool jbin_update_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_update_item(UDF_EXEC_ARGS);
	DllExport void jbin_update_item_deinit(UDF_INIT*);

	DllExport my_bool jbin_file_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jbin_file(UDF_EXEC_ARGS);
	DllExport void jbin_file_deinit(UDF_INIT*);

	DllExport my_bool json_serialize_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_serialize(UDF_EXEC_ARGS);
	DllExport void json_serialize_deinit(UDF_INIT*);

	DllExport my_bool jfile_convert_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* jfile_convert(UDF_EXEC_ARGS);
	DllExport void jfile_convert_deinit(UDF_INIT*);

	DllExport my_bool jfile_bjson_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* jfile_bjson(UDF_EXEC_ARGS);
	DllExport void jfile_bjson_deinit(UDF_INIT*);

	DllExport my_bool envar_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *envar(UDF_EXEC_ARGS);

#if defined(DEVELOPMENT)
	DllExport my_bool uvar_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *uvar(UDF_EXEC_ARGS);
#endif   // DEVELOPMENT

	DllExport my_bool countin_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long countin(UDF_INIT*, UDF_ARGS*, char*, char*);
} // extern "C"																											  


/*********************************************************************************/
/*  Structure JPN. Used to make the locate path.                                 */
/*********************************************************************************/
typedef struct _jpn {
	int  Type;
	PCSZ Key;
	int  N;
} JPN, *PJPN;

/*********************************************************************************/
/*  Class JSNX: JSON access method.                                              */
/*********************************************************************************/
class JSNX : public BLOCK {
public:
	// Constructors
	JSNX(PGLOBAL g, PJSON row, int type, int len = 64, int prec = 0, my_bool wr = false);

	// Implementation
	int     GetPrecision(void) {return Prec;}
	PVAL    GetValue(void) {return Value;}

	// Methods
	my_bool SetJpath(PGLOBAL g, char *path, my_bool jb = false);
	my_bool ParseJpath(PGLOBAL g);
	void    ReadValue(PGLOBAL g);
	PJVAL   GetRowValue(PGLOBAL g, PJSON row, int i, my_bool b = true);
	PJVAL   GetJson(PGLOBAL g);
	my_bool CheckPath(PGLOBAL g);
	my_bool WriteValue(PGLOBAL g, PJVAL jvalp);
	char   *Locate(PGLOBAL g, PJSON jsp, PJVAL jvp, int k = 1);
	char   *LocateAll(PGLOBAL g, PJSON jsp, PJVAL jvp, int mx = 10);

protected:
	my_bool SetArrayOptions(PGLOBAL g, char *p, int i, PSZ nm);
	PVAL    GetColumnValue(PGLOBAL g, PJSON row, int i);
	PVAL    ExpandArray(PGLOBAL g, PJAR arp, int n);
	PVAL    GetCalcValue(PGLOBAL g, PJAR bap, int n);
	PVAL    CalculateArray(PGLOBAL g, PJAR arp, int n);
	PJVAL   MakeJson(PGLOBAL g, PJSON jsp, int i);
	void    SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val);
	PJSON   GetRow(PGLOBAL g);
	my_bool CompareValues(PJVAL v1, PJVAL v2);
	my_bool LocateArray(PGLOBAL g, PJAR jarp);
	my_bool LocateObject(PGLOBAL g, PJOB jobp);
	my_bool LocateValue(PGLOBAL g, PJVAL jvp);
	my_bool LocateArrayAll(PGLOBAL g, PJAR jarp);
	my_bool LocateObjectAll(PGLOBAL g, PJOB jobp);
	my_bool LocateValueAll(PGLOBAL g, PJVAL jvp);
	my_bool CompareTree(PGLOBAL g, PJSON jp1, PJSON jp2);
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
	//PVAL     MulVal;              // To value used by multiple column
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
	my_bool  Wr;			  					// Write mode
	my_bool  Jb;			  					// Must return json item
}; // end of class JSNX

/*********************************************************************************/
/*  Class JUP: used by jfile_convert to make a json file pretty = 0.             */
/*********************************************************************************/
class JUP : public BLOCK {
public:
	// Constructor
	JUP(PGLOBAL g);

	// Implementation
	void  AddBuff(char c) {
		if (k < recl)
			buff[k++] = c;
		else
			throw "Record size is too small";
	}	// end of AddBuff

	// Methods
	char *UnprettyJsonFile(PGLOBAL g, char* fn, char* outfn, int lrecl);
	bool  unPretty(PGLOBAL g, int lrecl);
	void  CopyObject(PGLOBAL g);
	void  CopyArray(PGLOBAL g);
	void  CopyValue(PGLOBAL g);
	void  CopyString(PGLOBAL g);
	void  CopyNumeric(PGLOBAL g);

	// Members
	FILE  *fs;
	char  *s;
	char  *buff;
	size_t len;
	uint   i;
	int    k, recl;
}; // end of class JUP
