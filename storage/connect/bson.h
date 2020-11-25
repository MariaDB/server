#pragma once
/**************** bson H Declares Source Code File (.H) ****************/
/*  Name: bson.h   Version 1.0                                         */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2020         */
/*                                                                     */
/*  This file contains the BSON classe declares.                       */
/***********************************************************************/
#include <mysql_com.h>
#include "json.h"
#include "xobject.h"

#if defined(_DEBUG)
#define X  assert(false);
#else
#define X												  
#endif

#define ARGS    MY_MIN(24,(int)len-i),s+MY_MAX(i-3,0)
#define MOF(X)  MakeOff(Base, X)
#define MP(X)   MakePtr(Base, X)
#define MPP(X)  (PBPR)MakePtr(Base, X)
#define MVP(X)  (PBVAL)MakePtr(Base, X)
#define MZP(X)  (PSZ)MakePtr(Base, X)
#define LLN(X)	*(longlong*)MakePtr(Base, X)
#define DBL(X)	*(double*)MakePtr(Base, X)

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
	OFFSET Vlp;    // To the value of the pair
	OFFSET Next;   // Offset to the next pair in object
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
	BJSON(void* base, PBVAL vp = NULL) { Base = base; Bvp = vp; }

	void* GetBase(void) { return Base; }

	// SubAlloc functions
	void* BsonSubAlloc(PGLOBAL g, size_t size);
	PBPR  SubAllocPair(PGLOBAL g, OFFSET key, OFFSET val = 0);
	PBVAL SubAllocVal(PGLOBAL g);
	PBVAL SubAllocVal(PGLOBAL g, OFFSET toval, JTYP type = TYPE_UNKNOWN, short nd = 0);
	PBVAL SubAllocVal(PGLOBAL g, PVAL valp);
	PBVAL DupVal(PGLOBAL g, PBVAL bvp);

	// Array functions
	int   GetArraySize(PBVAL bap, bool b = false);
	PBVAL GetArrayValue(PBVAL bap, int i);
  PSZ   GetArrayText(PGLOBAL g, PBVAL bap, PSTRG text);
	PBVAL MergeArray(PGLOBAL g, PBVAL bap1,PBVAL bap2);
	PBVAL DeleteValue(PBVAL bap, int n);
	PBVAL AddArrayValue(PGLOBAL g, PBVAL bap, PBVAL nvp = NULL, int* x = NULL);
	PBVAL SetArrayValue(PGLOBAL g, PBVAL bap, PBVAL nvp, int n);
	bool  IsArrayNull(PBVAL bap);

	// Object functions
	int   GetObjectSize(PBPR bop, bool b = false);
  PSZ   GetObjectText(PGLOBAL g, PBPR bop, PSTRG text);
	PBPR  MergeObject(PGLOBAL g, PBPR bop1, PBPR bop2);
	PBPR  AddPair(PGLOBAL g, PBPR bop, PSZ key, OFFSET val = 0);
	PBVAL GetKeyValue(PBPR bop, PSZ key);
	PBVAL GetKeyList(PGLOBAL g, PBPR bop);
	PBVAL GetObjectValList(PGLOBAL g, PBPR bop);
	PBPR  SetKeyValue(PGLOBAL g, PBPR bop, OFFSET bvp, PSZ key);
	PBPR  DeleteKey(PBPR bop, PCSZ k);
	bool  IsObjectNull(PBPR bop);

	// Value functions
	int   GetSize(PBVAL vlp, bool b = false);
	PBPR  GetObject(PBVAL vlp);
	PBVAL GetArray(PBVAL vlp);
	//PJSON GetJsp(void) { return (DataType == TYPE_JSON ? Jsp : NULL); }
	PSZ   GetValueText(PGLOBAL g, PBVAL vlp, PSTRG text);
	//inline PJSON  GetJson(void) { return (DataType == TYPE_JSON ? Jsp : this); }
	PSZ   GetString(PGLOBAL g, PBVAL vp, char* buff = NULL);
	int   GetInteger(PBVAL vp);
	long long GetBigint(PBVAL vp);
	double GetDouble(PBVAL vp);
	PVAL  GetValue(PGLOBAL g, PBVAL vp);
	void  SetValueObj(PBVAL vlp, PBPR bop);
	void  SetValueArr(PBVAL vlp, PBVAL bap);
	void  SetValueVal(PBVAL vlp, PBVAL vp);
	void  SetValue(PGLOBAL g, PBVAL vlp, PVAL valp);
	void  SetString(PBVAL vlp, PSZ s, int ci = 0);
	void  SetInteger(PBVAL vlp, int n);
	void  SetBigint(PGLOBAL g, PBVAL vlp, longlong ll);
	void  SetFloat(PBVAL vlp, double f);
	void  SetBool(PBVAL vlp, bool b);
	bool  IsValueNull(PBVAL vlp);

	// Members
	PBVAL Bvp;
	void* Base;

protected:
	// Default constructor not to be used
	BJSON(void) {}
}; // end of class BJSON

/***********************************************************************/
/* Class JDOC. The class for parsing and serializing json documents.   */
/***********************************************************************/
class BDOC : public BJSON {
public:
	BDOC(void *);

	PBVAL ParseJson(PGLOBAL g, char* s, size_t n, int* prty = NULL, bool* b = NULL);
	PSZ   Serialize(PGLOBAL g, PBVAL bvp, char* fn, int pretty);

protected:
	OFFSET ParseArray(PGLOBAL g, int& i);
	OFFSET ParseObject(PGLOBAL g, int& i);
	PBVAL  ParseValue(PGLOBAL g, int& i);
	OFFSET ParseString(PGLOBAL g, int& i);
	void   ParseNumeric(PGLOBAL g, int& i, PBVAL bvp);
	OFFSET ParseAsArray(PGLOBAL g, int& i, int pretty, int* ptyp);
	bool   SerializeArray(OFFSET arp, bool b);
	bool   SerializeObject(OFFSET obp);
	bool   SerializeValue(PBVAL vp);

	// Members used when parsing and serializing
	JOUT* jp;						 // Used with serialize
	char* s;						 // The Json string to parse
	int   len;					 // The Json string length
	bool  pty[3];					 // Used to guess what pretty is

	// Default constructor not to be used
	BDOC(void) {}
}; // end of class BDOC

#if 0
/***********************************************************************/
/* Class JOBJECT: contains a list of value pairs.                      */
/***********************************************************************/
class JOBJECT : public JSON {
	friend class JDOC;
	friend class JSNX;
	friend class SWAP;
public:
	JOBJECT(void) : JSON() { Type = TYPE_JOB; First = Last = NULL; }
	JOBJECT(int i) : JSON(i) {}

	// Methods
	virtual void  Clear(void) { First = Last = NULL; }
	virtual PJPR  GetFirst(void) { return First; }
	virtual int   GetSize(PBPR prp, bool b);
	virtual PJOB  GetObject(void) { return this; }
	virtual PSZ   GetText(PGLOBAL g, PSTRG text);
	virtual bool  Merge(PGLOBAL g, PJSON jsp);
	virtual bool  IsNull(void);

	// Specific
	PJPR  AddPair(PGLOBAL g, PCSZ key);
	PJVAL GetKeyValue(const char* key);
	PJAR  GetKeyList(PGLOBAL g);
	PJAR  GetValList(PGLOBAL g);
	void  SetKeyValue(PGLOBAL g, PJVAL jvp, PCSZ key);
	void  DeleteKey(PCSZ k);

protected:
	PJPR First;
	PJPR Last;
}; // end of class JOBJECT

/***********************************************************************/
/* Class JARRAY.                                                       */
/***********************************************************************/
class JARRAY : public JSON {
	friend class SWAP;
public:
	JARRAY(void);
	JARRAY(int i) : JSON(i) {}

	// Methods
	virtual void  Clear(void) { First = Last = NULL; Size = 0; }
	virtual int   size(void) { return Size; }
	virtual PJAR  GetArray(void) { return this; }
	virtual int   GetSize(bool b);
	virtual PJVAL GetArrayValue(int i);
	virtual PSZ   GetText(PGLOBAL g, PSTRG text);
	virtual bool  Merge(PGLOBAL g, PJSON jsp);
	virtual bool  DeleteValue(int n);
	virtual bool  IsNull(void);

	// Specific
	PJVAL AddArrayValue(PGLOBAL g, PJVAL jvp = NULL, int* x = NULL);
	bool  SetArrayValue(PGLOBAL g, PJVAL jvp, int i);
	void  InitArray(PGLOBAL g);

protected:
	// Members
	int    Size;		 // The number of items in the array
	int    Alloc;    // The Mvals allocated size
	PJVAL  First;    // Used when constructing
	PJVAL  Last;     // Last constructed value
	PJVAL* Mvals;    // Allocated when finished
}; // end of class JARRAY

/***********************************************************************/
/* Class JVALUE.                                                       */
/***********************************************************************/
class JVALUE : public JSON {
	friend class JARRAY;
	friend class JSNX;
	friend class JSONDISC;
	friend class JSONCOL;
	friend class JSON;
	friend class JDOC;
	friend class SWAP;
public:
	JVALUE(void) : JSON() { Type = TYPE_JVAL; Clear(); }
	JVALUE(PJSON jsp);
	JVALUE(PGLOBAL g, PVAL valp);
	JVALUE(PGLOBAL g, PCSZ strp);
	JVALUE(int i) : JSON(i) {}

	// Methods
	virtual void   Clear(void);
	//virtual JTYP   GetType(void) {return TYPE_JVAL;}
	virtual JTYP   GetValType(void);
	virtual PJOB   GetObject(void);
	virtual PJAR   GetArray(void);
	virtual PJSON  GetJsp(void) { return (DataType == TYPE_JSON ? Jsp : NULL); }
	virtual PSZ    GetText(PGLOBAL g, PSTRG text);
	virtual bool   IsNull(void);

	// Specific
	inline PJSON  GetJson(void) { return (DataType == TYPE_JSON ? Jsp : this); }
	PSZ    GetString(PGLOBAL g, char* buff = NULL);
	int    GetInteger(void);
	long long GetBigint(void);
	double GetFloat(void);
	PVAL   GetValue(PGLOBAL g);
	void   SetValue(PJSON jsp);
	void   SetValue(PGLOBAL g, PVAL valp);
	void   SetString(PGLOBAL g, PSZ s, int ci = 0);
	void   SetInteger(PGLOBAL g, int n);
	void   SetBigint(PGLOBAL g, longlong ll);
	void   SetFloat(PGLOBAL g, double f);
	void   SetBool(PGLOBAL g, bool b);

protected:
	union {
		PJSON  Jsp;       // To the json value
		char* Strp;      // Ptr to a string
		int    N;         // An integer value
		long long LLn;		 // A big integer value
		double F;				 // A (double) float value
		bool   B;				 // True or false
	};
	PJVAL Next;     // Next value in array
	JTYP  DataType; // The data value type
	int   Nd;				// Decimal number
	bool  Del;      // True when deleted
}; // end of class JVALUE
#endif // 0
