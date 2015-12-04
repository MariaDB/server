/******************** tabjson H Declares Source Code File (.H) *******************/
/*  Name: jsonudf.h   Version 1.2                                                */
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
	DllExport my_bool jsonvalue_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jsonvalue(UDF_EXEC_ARGS);
	DllExport void jsonvalue_deinit(UDF_INIT*);

	DllExport my_bool json_array_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_array(UDF_EXEC_ARGS);
	DllExport void json_array_deinit(UDF_INIT*);

	DllExport my_bool json_array_add_values_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_array_add_values(UDF_EXEC_ARGS);
	DllExport void json_array_add_values_deinit(UDF_INIT*);

	DllExport my_bool json_array_add_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_array_add(UDF_EXEC_ARGS);
	DllExport void json_array_add_deinit(UDF_INIT*);

	DllExport my_bool json_array_delete_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_array_delete(UDF_EXEC_ARGS);
	DllExport void json_array_delete_deinit(UDF_INIT*);

	DllExport my_bool json_object_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_object(UDF_EXEC_ARGS);
	DllExport void json_object_deinit(UDF_INIT*);

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
	DllExport long long jsoncontains(UDF_EXEC_ARGS);
	DllExport void jsoncontains_deinit(UDF_INIT*);

	DllExport my_bool jsonlocate_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *jsonlocate(UDF_EXEC_ARGS);
	DllExport void jsonlocate_deinit(UDF_INIT*);

	DllExport my_bool json_locate_all_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char *json_locate_all(UDF_EXEC_ARGS);
	DllExport void json_locate_all_deinit(UDF_INIT*);

	DllExport my_bool jsoncontains_path_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport long long jsoncontains_path(UDF_EXEC_ARGS);
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
	PVAL    CalculateArray(PGLOBAL g, PJAR arp, int n);
	PVAL    MakeJson(PGLOBAL g, PJSON jsp);
	void    SetJsonValue(PGLOBAL g, PVAL vp, PJVAL val, int n);
	PJSON   GetRow(PGLOBAL g);
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
	my_bool  Wr;			  					// Write mode
	my_bool  Jb;			  					// Must return json item
}; // end of class JSNX
