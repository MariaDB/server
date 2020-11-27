/******************** tabjson H Declares Source Code File (.H) *******************/
/*  Name: bsonudf.h   Version 1.0                                                */
/*                                                                               */
/*  (C) Copyright to the author Olivier BERTRAND          2020                   */
/*                                                                               */
/*  This file contains the BSON UDF function and class declares.                 */
/*********************************************************************************/
#pragma once
#include "jsonudf.h"
#include "bson.h"

/* --------------------------- New Testing BJSON Stuff --------------------------*/

typedef class BJNX* PBJNX;

/*********************************************************************************/
/*  Class BJNX: BJSON access methods.                                            */
/*********************************************************************************/
class BJNX : public BDOC {
public:
	// Constructors
	BJNX(PGLOBAL g, PBVAL row, int type, int len = 64, int prec = 0, my_bool wr = false);

	// Implementation
	int     GetPrecision(void) { return Prec; }
	PVAL    GetValue(void) { return Value; }

	// Methods
	my_bool SetJpath(PGLOBAL g, char* path, my_bool jb = false);
	my_bool ParseJpath(PGLOBAL g);
	void    ReadValue(PGLOBAL g);
	PBVAL   GetRowValue(PGLOBAL g, PBVAL row, int i, my_bool b = true);
	PBVAL   GetJson(PGLOBAL g);
	my_bool CheckPath(PGLOBAL g);
	my_bool WriteValue(PGLOBAL g, PBVAL jvalp);
	char* Locate(PGLOBAL g, PBVAL jsp, PBVAL jvp, int k = 1);
	char* LocateAll(PGLOBAL g, PBVAL jsp, PBVAL jvp, int mx = 10);

protected:
	my_bool SetArrayOptions(PGLOBAL g, char* p, int i, PSZ nm);
	PVAL    GetColumnValue(PGLOBAL g, PBVAL row, int i);
	PVAL    ExpandArray(PGLOBAL g, PBVAL arp, int n);
	PVAL    CalculateArray(PGLOBAL g, PBVAL arp, int n);
	PVAL    MakeJson(PGLOBAL g, PBVAL bvp);
	void    SetJsonValue(PGLOBAL g, PVAL vp, PBVAL vlp);
	PBVAL   GetRow(PGLOBAL g);
	my_bool CompareValues(PGLOBAL g, PBVAL v1, PBVAL v2);
	my_bool LocateArray(PGLOBAL g, PBVAL jarp);
	my_bool LocateObject(PGLOBAL g, PBPR jobp);
	my_bool LocateValue(PGLOBAL g, PBVAL jvp);
	my_bool LocateArrayAll(PGLOBAL g, PBVAL jarp);
	my_bool LocateObjectAll(PGLOBAL g, PBPR jobp);
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

	DllExport my_bool bsonlocate_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bsonlocate(UDF_EXEC_ARGS);
	DllExport void bsonlocate_deinit(UDF_INIT*);

	DllExport my_bool bson_locate_all_init(UDF_INIT*, UDF_ARGS*, char*);
	DllExport char* bson_locate_all(UDF_EXEC_ARGS);
	DllExport void bson_locate_all_deinit(UDF_INIT*);
} // extern "C"

