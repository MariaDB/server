/******************** tabjson H Declares Source Code File (.H) *******************/
/*  Name: bsonudf.h   Version 1.0                                                */
/*                                                                               */
/*  (C) Copyright to the author Olivier BERTRAND          2020 - 2021            */
/*                                                                               */
/*  This file contains the BSON UDF function and class declares.                 */
/*********************************************************************************/
#pragma once
#include "jsonudf.h"
#include "bson.h"

#if 0
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
int     IsArgJson(UDF_ARGS* args, uint i);
char   *GetJsonFile(PGLOBAL g, char* fn);

/*********************************************************************************/
/*  Structure JPN. Used to make the locate path.                                 */
/*********************************************************************************/
typedef struct _jpn {
	int  Type;
	PCSZ Key;
	int  N;
} JPN, *PJPN;

#endif // 0

/* --------------------------- New Testing BJSON Stuff --------------------------*/
extern uint JsonGrpSize;
uint GetJsonGroupSize(void);


typedef class BJNX* PBJNX;

/*********************************************************************************/
/*  Class BJNX: BJSON access methods.                                            */
/*********************************************************************************/
class BJNX : public BDOC {
public:
	// Constructors
	BJNX(PGLOBAL g);
	BJNX(PGLOBAL g, PBVAL row, int type, int len = 64, int prec = 0, my_bool wr = false);

	// Implementation
	int     GetPrecision(void) { return Prec; }
	PVAL    GetValue(void) { return Value; }
	void    SetRow(PBVAL vp) { Row = vp; }
	void    SetChanged(my_bool b) { Changed = b; }

	// Methods
	my_bool SetJpath(PGLOBAL g, char* path, my_bool jb = false);
	my_bool ParseJpath(PGLOBAL g);
	void    ReadValue(PGLOBAL g);
	PBVAL   GetRowValue(PGLOBAL g, PBVAL row, int i);
	PBVAL   GetJson(PGLOBAL g);
	my_bool CheckPath(PGLOBAL g);
	my_bool CheckPath(PGLOBAL g, UDF_ARGS* args, PBVAL jsp, PBVAL& jvp, int n);
	my_bool WriteValue(PGLOBAL g, PBVAL jvalp);
	my_bool DeleteItem(PGLOBAL g, PBVAL vlp);
	char   *Locate(PGLOBAL g, PBVAL jsp, PBVAL jvp, int k = 1);
	char   *LocateAll(PGLOBAL g, PBVAL jsp, PBVAL jvp, int mx = 10);
	PSZ     MakeKey(UDF_ARGS* args, int i);
	PBVAL   MakeValue(UDF_ARGS* args, uint i, bool b = false, PBVAL* top = NULL);
	PBVAL   MakeTypedValue(PGLOBAL g, UDF_ARGS* args, uint i,
		                     JTYP type, PBVAL* top = NULL);
	PBVAL   ParseJsonFile(PGLOBAL g, char* fn, int& pty, size_t& len);
	char   *MakeResult(UDF_ARGS* args, PBVAL top, uint n = 2);
	PBSON   MakeBinResult(UDF_ARGS* args, PBVAL top, ulong len, int n = 2);

protected:
	my_bool SetArrayOptions(PGLOBAL g, char* p, int i, PSZ nm);
	PVAL    GetColumnValue(PGLOBAL g, PBVAL row, int i);
	PVAL    ExpandArray(PGLOBAL g, PBVAL arp, int n);
	PVAL    CalculateArray(PGLOBAL g, PBVAL arp, int n);
	PVAL    GetCalcValue(PGLOBAL g, PBVAL bap, int n);
	PBVAL   MakeJson(PGLOBAL g, PBVAL bvp, int n);
	void    SetJsonValue(PGLOBAL g, PVAL vp, PBVAL vlp);
	PBVAL   GetRow(PGLOBAL g);
	PBVAL   MoveVal(PBVAL vlp);
	PBVAL   MoveJson(PBJNX bxp, PBVAL jvp);
	PBVAL   MoveArray(PBJNX bxp, PBVAL jvp);
	PBVAL   MoveObject(PBJNX bxp, PBVAL jvp);
	PBVAL   MoveValue(PBJNX bxp, PBVAL jvp);
	my_bool CompareValues(PGLOBAL g, PBVAL v1, PBVAL v2);
	my_bool LocateArray(PGLOBAL g, PBVAL jarp);
	my_bool LocateObject(PGLOBAL g, PBVAL jobp);
	my_bool LocateValue(PGLOBAL g, PBVAL jvp);
	my_bool LocateArrayAll(PGLOBAL g, PBVAL jarp);
	my_bool LocateObjectAll(PGLOBAL g, PBVAL jobp);
	my_bool LocateValueAll(PGLOBAL g, PBVAL jvp);
	my_bool CompareTree(PGLOBAL g, PBVAL jp1, PBVAL jp2);
	my_bool AddPath(void);

	// Default constructor not to be used
	BJNX(void) {}

	// Members
	PBVAL    Row;
	PBVAL    Bvalp;
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
	my_bool  Changed;			  			// True when contains was modified
}; // end of class BJNX

extern "C" {
	DllExport my_bool bson_test_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_test(UDF_EXEC_ARGS);
	DllExport void bson_test_deinit(UDF_INIT*);

	DllExport my_bool bsonvalue_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bsonvalue(UDF_EXEC_ARGS);
	DllExport void bsonvalue_deinit(UDF_INIT*);

	DllExport my_bool bson_make_array_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_make_array(UDF_EXEC_ARGS);
	DllExport void bson_make_array_deinit(UDF_INIT*);

	DllExport my_bool bson_array_add_values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_array_add_values(UDF_EXEC_ARGS);
	DllExport void bson_array_add_values_deinit(UDF_INIT*);

	DllExport my_bool bson_array_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_array_add(UDF_EXEC_ARGS);
	DllExport void bson_array_add_deinit(UDF_INIT*);

	DllExport my_bool bson_array_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_array_delete(UDF_EXEC_ARGS);
	DllExport void bson_array_delete_deinit(UDF_INIT*);

	DllExport my_bool bsonlocate_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bsonlocate(UDF_EXEC_ARGS);
	DllExport void bsonlocate_deinit(UDF_INIT*);

	DllExport my_bool bson_locate_all_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_locate_all(UDF_EXEC_ARGS);
	DllExport void bson_locate_all_deinit(UDF_INIT*);

	DllExport my_bool bson_contains_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long bson_contains(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void bson_contains_deinit(UDF_INIT*);

	DllExport my_bool bsoncontains_path_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long bsoncontains_path(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void bsoncontains_path_deinit(UDF_INIT*);

	DllExport my_bool bson_make_object_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_make_object(UDF_EXEC_ARGS);
	DllExport void bson_make_object_deinit(UDF_INIT*);

	DllExport my_bool bson_object_nonull_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_object_nonull(UDF_EXEC_ARGS);
	DllExport void bson_object_nonull_deinit(UDF_INIT*);

	DllExport my_bool bson_object_key_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_object_key(UDF_EXEC_ARGS);
	DllExport void bson_object_key_deinit(UDF_INIT*);

	DllExport my_bool bson_object_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_object_add(UDF_EXEC_ARGS);
	DllExport void bson_object_add_deinit(UDF_INIT*);

	DllExport my_bool bson_object_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_object_delete(UDF_EXEC_ARGS);
	DllExport void bson_object_delete_deinit(UDF_INIT*);

	DllExport my_bool bson_object_list_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_object_list(UDF_EXEC_ARGS);
	DllExport void bson_object_list_deinit(UDF_INIT*);

	DllExport my_bool bson_object_values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_object_values(UDF_EXEC_ARGS);
	DllExport void bson_object_values_deinit(UDF_INIT*);

	DllExport my_bool bson_item_merge_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_item_merge(UDF_EXEC_ARGS);
	DllExport void bson_item_merge_deinit(UDF_INIT*);

	DllExport my_bool bson_get_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_get_item(UDF_EXEC_ARGS);
	DllExport void bson_get_item_deinit(UDF_INIT*);

	DllExport my_bool bsonget_string_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bsonget_string(UDF_EXEC_ARGS);
	DllExport void bsonget_string_deinit(UDF_INIT*);

	DllExport my_bool bsonget_int_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long bsonget_int(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void bsonget_int_deinit(UDF_INIT*);

	DllExport my_bool bsonget_real_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport double bsonget_real(UDF_INIT*, UDF_ARGS*, char*, char*);
	DllExport void bsonget_real_deinit(UDF_INIT*);

	DllExport my_bool bsonset_def_prec_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long bsonset_def_prec(UDF_INIT*, UDF_ARGS*, char*, char*);

	DllExport my_bool bsonget_def_prec_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long bsonget_def_prec(UDF_INIT*, UDF_ARGS*, char*, char*);

	DllExport my_bool bsonset_grp_size_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long bsonset_grp_size(UDF_INIT*, UDF_ARGS*, char*, char*);

	DllExport my_bool bsonget_grp_size_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long bsonget_grp_size(UDF_INIT*, UDF_ARGS*, char*, char*);

	DllExport my_bool bson_array_grp_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport void bson_array_grp_clear(UDF_INIT *, char *, char *);
	DllExport void bson_array_grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
	DllExport char *bson_array_grp(UDF_EXEC_ARGS);
	DllExport void bson_array_grp_deinit(UDF_INIT*);

	DllExport my_bool bson_object_grp_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport void bson_object_grp_clear(UDF_INIT *, char *, char *);
	DllExport void bson_object_grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
	DllExport char *bson_object_grp(UDF_EXEC_ARGS);
	DllExport void bson_object_grp_deinit(UDF_INIT*);

	DllExport my_bool bson_delete_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bson_delete_item(UDF_EXEC_ARGS);
	DllExport void bson_delete_item_deinit(UDF_INIT*);

	DllExport my_bool bson_set_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bson_set_item(UDF_EXEC_ARGS);
	DllExport void bson_set_item_deinit(UDF_INIT*);

	DllExport my_bool bson_insert_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bson_insert_item(UDF_EXEC_ARGS);
	DllExport void bson_insert_item_deinit(UDF_INIT*);

	DllExport my_bool bson_update_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bson_update_item(UDF_EXEC_ARGS);
	DllExport void bson_update_item_deinit(UDF_INIT*);

	DllExport my_bool bson_file_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bson_file(UDF_EXEC_ARGS);
	DllExport void bson_file_deinit(UDF_INIT*);

	DllExport my_bool bfile_make_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bfile_make(UDF_EXEC_ARGS);
	DllExport void bfile_make_deinit(UDF_INIT*);

	DllExport my_bool bfile_convert_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bfile_convert(UDF_EXEC_ARGS);
	DllExport void bfile_convert_deinit(UDF_INIT*);

	DllExport my_bool bfile_bjson_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bfile_bjson(UDF_EXEC_ARGS);
	DllExport void bfile_bjson_deinit(UDF_INIT*);

	DllExport my_bool bson_serialize_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bson_serialize(UDF_EXEC_ARGS);
	DllExport void bson_serialize_deinit(UDF_INIT*);

	DllExport my_bool bbin_make_array_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_make_array(UDF_EXEC_ARGS);
	DllExport void bbin_make_array_deinit(UDF_INIT*);

	DllExport my_bool bbin_array_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_array_add(UDF_EXEC_ARGS);
	DllExport void bbin_array_add_deinit(UDF_INIT*);

	DllExport my_bool bbin_array_add_values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_array_add_values(UDF_EXEC_ARGS);
	DllExport void bbin_array_add_values_deinit(UDF_INIT*);

	DllExport my_bool bbin_array_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_array_delete(UDF_EXEC_ARGS);
	DllExport void bbin_array_delete_deinit(UDF_INIT*);

	DllExport my_bool bbin_array_grp_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport void bbin_array_grp_clear(UDF_INIT *, char *, char *);
	DllExport void bbin_array_grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
	DllExport char *bbin_array_grp(UDF_EXEC_ARGS);
	DllExport void bbin_array_grp_deinit(UDF_INIT*);

	DllExport my_bool bbin_object_grp_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport void bbin_object_grp_clear(UDF_INIT *, char *, char *);
	DllExport void bbin_object_grp_add(UDF_INIT *, UDF_ARGS *, char *, char *);
	DllExport char *bbin_object_grp(UDF_EXEC_ARGS);
	DllExport void bbin_object_grp_deinit(UDF_INIT*);

	DllExport my_bool bbin_make_object_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_make_object(UDF_EXEC_ARGS);
	DllExport void bbin_make_object_deinit(UDF_INIT*);

	DllExport my_bool bbin_object_nonull_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_object_nonull(UDF_EXEC_ARGS);
	DllExport void bbin_object_nonull_deinit(UDF_INIT*);

	DllExport my_bool bbin_object_key_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_object_key(UDF_EXEC_ARGS);
	DllExport void bbin_object_key_deinit(UDF_INIT*);

	DllExport my_bool bbin_object_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_object_add(UDF_EXEC_ARGS);
	DllExport void bbin_object_add_deinit(UDF_INIT*);

	DllExport my_bool bbin_object_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_object_delete(UDF_EXEC_ARGS);
	DllExport void bbin_object_delete_deinit(UDF_INIT*);

	DllExport my_bool bbin_object_list_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_object_list(UDF_EXEC_ARGS);
	DllExport void bbin_object_list_deinit(UDF_INIT*);

	DllExport my_bool bbin_object_values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_object_values(UDF_EXEC_ARGS);
	DllExport void bbin_object_values_deinit(UDF_INIT*);

	DllExport my_bool bbin_get_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_get_item(UDF_EXEC_ARGS);
	DllExport void bbin_get_item_deinit(UDF_INIT*);

	DllExport my_bool bbin_item_merge_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_item_merge(UDF_EXEC_ARGS);
	DllExport void bbin_item_merge_deinit(UDF_INIT*);

	DllExport my_bool bbin_set_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_set_item(UDF_EXEC_ARGS);
	DllExport void bbin_set_item_deinit(UDF_INIT*);

	DllExport my_bool bbin_insert_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_insert_item(UDF_EXEC_ARGS);
	DllExport void bbin_insert_item_deinit(UDF_INIT*);

	DllExport my_bool bbin_update_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_update_item(UDF_EXEC_ARGS);
	DllExport void bbin_update_item_deinit(UDF_INIT*);

	DllExport my_bool bbin_delete_item_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_delete_item(UDF_EXEC_ARGS);
	DllExport void bbin_delete_item_deinit(UDF_INIT*);

	DllExport my_bool bbin_locate_all_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bbin_locate_all(UDF_EXEC_ARGS);
	DllExport void bbin_locate_all_deinit(UDF_INIT*);

	DllExport my_bool bbin_file_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *bbin_file(UDF_EXEC_ARGS);
	DllExport void bbin_file_deinit(UDF_INIT*);
} // extern "C"
