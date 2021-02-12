/**************** bson H Declares Source Code File (.H) ****************/
/*  Name: bson.h   Version 1.0                                         */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2020         */
/*                                                                     */
/*  This file contains the BSON classe declares.                       */
/***********************************************************************/
#pragma once
#include <mysql_com.h>
#include "json.h"
#include "xobject.h"

#if defined(_DEBUG)
#define X  assert(false);
#else
#define X												  
#endif

#define ARGS    MY_MIN(24,(int)len-i),s+MY_MAX(i-3,0)

class BDOC;
class BOUT;
class BJSON;

typedef class BDOC* PBDOC;
typedef class BJSON* PBJSON;
typedef uint  OFFSET;

/***********************************************************************/
/* Structure BVAL. Binary representation of a JVALUE.                  */
/***********************************************************************/
typedef struct _jvalue {
	union {
		OFFSET To_Val;    // Offset to a value
		int    N;         // An integer value
		float  F;				  // A float value
		bool   B;				  // A boolean value True or false (0)
	};
	short    Nd;				// Number of decimals
	short    Type;      // The value type
	OFFSET   Next;      // Offset to the next value in array
} BVAL, *PBVAL;  // end of struct BVALUE

/***********************************************************************/
/* Structure BPAIR. The pairs of a json Object.                        */
/***********************************************************************/
typedef struct _jpair {
	OFFSET Key;    // Offset to this pair key name
	BVAL   Vlp;    // The value of the pair
} BPAIR, *PBPR;  // end of struct BPAIR

char* NextChr(PSZ s, char sep);
char* GetJsonNull(void);
const char* GetFmt(int type, bool un);														 

DllExport bool IsNum(PSZ s);

/***********************************************************************/
/* Class BJSON. The class handling all BJSON operations.               */
/***********************************************************************/
class BJSON : public BLOCK {
public:
	// Constructor
	BJSON(PGLOBAL g, PBVAL vp = NULL)
	  { G = g, Base = G->Sarea; Bvp = vp; Throw = true; }

	// Utility functions
	inline OFFSET   MOF(void  *p) {return MakeOff(Base, p);}
	inline void     *MP(OFFSET o) {return MakePtr(Base, o);} 
	inline PBPR     MPP(OFFSET o) {return (PBPR)MakePtr(Base, o);}
	inline PBVAL    MVP(OFFSET o) {return (PBVAL)MakePtr(Base, o);}
	inline PSZ      MZP(OFFSET o) {return (PSZ)MakePtr(Base, o);}
	inline longlong LLN(OFFSET o) {return *(longlong*)MakePtr(Base, o);}
	inline double   DBL(OFFSET o) {return *(double*)MakePtr(Base, o);}

	void  Reset(void) {Base = G->Sarea;}
	void* GetBase(void) { return Base; }
	void  SubSet(bool b = false);
	void  MemSave(void) {G->Saved_Size = ((PPOOLHEADER)G->Sarea)->To_Free;}
	void  MemSet(size_t size);
	void  GetMsg(PGLOBAL g) { if (g != G) strcpy(g->Message, G->Message); }

	// SubAlloc functions
	void* BsonSubAlloc(size_t size);
	PBPR  NewPair(OFFSET key, int type = TYPE_NULL);
	OFFSET NewPair(PSZ key, int type = TYPE_NULL)
				{return MOF(NewPair(DupStr(key), type));}
	PBVAL NewVal(int type = TYPE_NULL);
	PBVAL NewVal(PVAL valp);
	PBVAL SubAllocVal(OFFSET toval, int type = TYPE_NULL, short nd = 0);
	PBVAL SubAllocVal(PBVAL toval, int type = TYPE_NULL, short nd = 0)
				{return SubAllocVal(MOF(toval), type, nd);}
	PBVAL SubAllocStr(OFFSET str, short nd = 0);
	PBVAL SubAllocStr(PSZ str, short nd = 0)
				{return SubAllocStr(DupStr(str), nd);}
	PBVAL DupVal(PBVAL bvp);
	OFFSET DupStr(PSZ str) { return MOF(NewStr(str)); }
	PSZ   NewStr(PSZ str);

	// Array functions
	inline PBVAL GetArray(PBVAL vlp) {return MVP(vlp->To_Val);}
	int   GetArraySize(PBVAL bap, bool b = false);
	PBVAL GetArrayValue(PBVAL bap, int i);
  PSZ   GetArrayText(PGLOBAL g, PBVAL bap, PSTRG text);
	void  MergeArray(PBVAL bap1,PBVAL bap2);
	bool  DeleteValue(PBVAL bap, int n);
	void  AddArrayValue(PBVAL bap, OFFSET nvp = 0, int* x = NULL);
	inline void AddArrayValue(PBVAL bap, PBVAL nvp = NULL, int* x = NULL)
				{AddArrayValue(bap, MOF(nvp), x);}
	void  SetArrayValue(PBVAL bap, PBVAL nvp, int n);
	bool  IsArrayNull(PBVAL bap);

	// Object functions
	inline PBPR GetObject(PBVAL bop) {return MPP(bop->To_Val);}
	inline PBPR	GetNext(PBPR brp) { return MPP(brp->Vlp.Next); }
	void  SetPairValue(PBPR brp, PBVAL bvp);
	int   GetObjectSize(PBVAL bop, bool b = false);
  PSZ   GetObjectText(PGLOBAL g, PBVAL bop, PSTRG text);
	PBVAL MergeObject(PBVAL bop1, PBVAL bop2);
	PBVAL AddPair(PBVAL bop, PSZ key, int type = TYPE_NULL);
	PSZ   GetKey(PBPR prp) {return prp ? MZP(prp->Key) : NULL;}
	PBVAL GetTo_Val(PBPR prp) {return prp ? MVP(prp->Vlp.To_Val) : NULL;}
	PBVAL GetVlp(PBPR prp) {return prp ? (PBVAL)&prp->Vlp : NULL;}
	PBVAL GetKeyValue(PBVAL bop, PSZ key);
	PBVAL GetKeyList(PBVAL bop);
	PBVAL GetObjectValList(PBVAL bop);
	void  SetKeyValue(PBVAL bop, OFFSET bvp, PSZ key);
	inline void SetKeyValue(PBVAL bop, PBVAL vlp, PSZ key)
				{SetKeyValue(bop, MOF(vlp), key);}
	bool  DeleteKey(PBVAL bop, PCSZ k);
	bool  IsObjectNull(PBVAL bop);

	// Value functions
	int   GetSize(PBVAL vlp, bool b = false);
	PBVAL GetNext(PBVAL vlp) {return MVP(vlp->Next);}
	//PJSON GetJsp(void) { return (DataType == TYPE_JSON ? Jsp : NULL); }
	PSZ   GetValueText(PGLOBAL g, PBVAL vlp, PSTRG text);
	PBVAL GetBson(PBVAL bvp);
	PSZ   GetString(PBVAL vp, char* buff = NULL);
	int   GetInteger(PBVAL vp);
	long long GetBigint(PBVAL vp);
	double GetDouble(PBVAL vp);
	PVAL  GetValue(PGLOBAL g, PBVAL vp);
	void  SetValueObj(PBVAL vlp, PBVAL bop);
	void  SetValueArr(PBVAL vlp, PBVAL bap);
	void  SetValueVal(PBVAL vlp, PBVAL vp);
	PBVAL SetValue(PBVAL vlp, PVAL valp);
	void  SetString(PBVAL vlp, PSZ s, int ci = 0);
	void  SetInteger(PBVAL vlp, int n);
	void  SetBigint(PBVAL vlp, longlong ll);
	void  SetFloat(PBVAL vlp, double f, int nd = -1);
	void  SetFloat(PBVAL vlp, PSZ s);
	void  SetBool(PBVAL vlp, bool b);
	void  Clear(PBVAL vlp) { vlp->N = 0; vlp->Nd = 0; vlp->Next = 0; }
	bool  IsValueNull(PBVAL vlp);
	bool  IsJson(PBVAL vlp)	{return vlp ? vlp->Type == TYPE_JAR ||
		                                    vlp->Type == TYPE_JOB ||
		                                    vlp->Type == TYPE_JVAL : false;}

	// Members
	PGLOBAL G;
	PBVAL   Bvp;
	void   *Base;
	bool    Throw;

protected:
	// Default constructor not to be used
	BJSON(void) {}
}; // end of class BJSON

/***********************************************************************/
/* Class JDOC. The class for parsing and serializing json documents.   */
/***********************************************************************/
class BDOC : public BJSON {
public:
	BDOC(PGLOBAL G);

	bool  GetComma(void) { return comma; }
	int   GetPretty(void) { return pretty; }
	void  SetPretty(int pty) { pretty = pty; }

	// Methods
	PBVAL ParseJson(PGLOBAL g, char* s, size_t n);
	PSZ   Serialize(PGLOBAL g, PBVAL bvp, char* fn, int pretty);

protected:
	OFFSET ParseArray(size_t& i);
	OFFSET ParseObject(size_t& i);
	PBVAL  ParseValue(size_t& i, PBVAL bvp);
	OFFSET ParseString(size_t& i);
	void   ParseNumeric(size_t& i, PBVAL bvp);
	OFFSET ParseAsArray(size_t& i);
	bool   SerializeArray(OFFSET arp, bool b);
	bool   SerializeObject(OFFSET obp);
	bool   SerializeValue(PBVAL vp, bool b = false);

	// Members used when parsing and serializing
	JOUT* jp;						 // Used with serialize
	char* s;						 // The Json string to parse
	size_t len;					 // The Json string length
	int   pretty;				 // The pretty style of the file to parse
	bool  pty[3];				 // Used to guess what pretty is
	bool  comma;				 // True if Pretty = 1

	// Default constructor not to be used
	BDOC(void) {}
}; // end of class BDOC
