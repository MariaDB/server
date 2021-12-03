/**************** json H Declares Source Code File (.H) ****************/
/*  Name: json.h   Version 1.2                                         */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2020  */
/*                                                                     */
/*  This file contains the JSON classes declares.                      */
/***********************************************************************/
#pragma once
#include <mysql_com.h>
#include "value.h"
#include "xobject.h"

#if defined(_DEBUG)
#define X  assert(false);
#else
#define X
#endif

enum JTYP {
	TYPE_NULL = TYPE_VOID,
	TYPE_STRG = TYPE_STRING,
	TYPE_DBL = TYPE_DOUBLE,
	TYPE_BOOL = TYPE_TINY,
	TYPE_BINT = TYPE_BIGINT,
	TYPE_INTG = TYPE_INT,
	TYPE_DTM = TYPE_DATE,
	TYPE_FLOAT,
	TYPE_JAR,
	TYPE_JOB,
	TYPE_JVAL,
	TYPE_JSON,
	TYPE_DEL,
	TYPE_UNKNOWN
};

class JDOC;
class JOUT;
class JSON;
class JVALUE;
class JOBJECT;
class JARRAY;

typedef class JDOC    *PJDOC;
typedef class JSON    *PJSON;
typedef class JVALUE  *PJVAL;
typedef class JOBJECT *PJOB;
typedef class JARRAY  *PJAR;

typedef struct JPAIR *PJPR;
//typedef struct VAL   *PVL;

/***********************************************************************/
/* Structure JPAIR. The pairs of a json Object.                        */
/***********************************************************************/
struct JPAIR {
	PCSZ  Key;      // This pair key name
	PJVAL Val;      // To the value of the pair
	PJPR  Next;     // To the next pair
}; // end of struct JPAIR

//PVL   AllocVal(PGLOBAL g, JTYP type);
char *NextChr(PSZ s, char sep);
char *GetJsonNull(void);
const char* GetFmt(int type, bool un);

PJSON ParseJson(PGLOBAL g, char* s, size_t n, int* prty = NULL, bool* b = NULL);
PSZ   Serialize(PGLOBAL g, PJSON jsp, char *fn, int pretty);
DllExport bool IsNum(PSZ s);
bool  IsArray(PSZ s);
bool  Stringified(PCSZ strfy, char *colname);

/***********************************************************************/
/* Class JDOC. The class for parsing and serializing json documents.   */
/***********************************************************************/
class JDOC: public BLOCK {
	friend PJSON ParseJson(PGLOBAL, char*, size_t, int*, bool*);
	friend PSZ Serialize(PGLOBAL, PJSON, char*, int);
public:
	JDOC(void) : js(NULL), s(NULL), len(0), dfp(0), pty(NULL) {}

	void  SetJp(JOUT* jp) { js = jp; }

 protected:
	PJAR  ParseArray(PGLOBAL g, int& i);
	PJOB  ParseObject(PGLOBAL g, int& i);
	PJVAL ParseValue(PGLOBAL g, int& i);
	char *ParseString(PGLOBAL g, int& i);
	void  ParseNumeric(PGLOBAL g, int& i, PJVAL jvp);
	PJAR  ParseAsArray(PGLOBAL g, int& i, int pretty, int *ptyp);
	bool  SerializeArray(PJAR jarp, bool b);
	bool  SerializeObject(PJOB jobp);
	bool  SerializeValue(PJVAL jvp);

	// Members used when parsing and serializing
 private:
	JOUT* js;
	char *s;
	int   len, dfp;
	bool *pty;
}; // end of class JDOC

/***********************************************************************/
/* Class JSON. The base class for all other json classes.              */
/***********************************************************************/
class JSON : public BLOCK {
public:
	// Constructor
	JSON(void) { Type = TYPE_JSON; }
	JSON(int) {}

	// Implementation
	inline  JTYP   GetType(void) { return Type; }

	// Methods
	virtual int    size(void) { return 1; }
	virtual void   Clear(void) { X }
	virtual PJOB   GetObject(void) { return NULL; }
	virtual PJAR   GetArray(void) { return NULL; }
	virtual PJVAL  GetArrayValue(int i) { X return NULL; }
	virtual int    GetSize(bool b) { X return 0; }
	virtual PJSON  GetJsp(void) { X return NULL; }
	virtual PJPR   GetFirst(void) { X return NULL; }
	virtual PSZ    GetText(PGLOBAL g, PSTRG text) { X return NULL; }
	virtual bool   Merge(PGLOBAL g, PJSON jsp) { X return true; }
	virtual void   SetValue(PJSON jsp) { X }
	virtual bool   DeleteValue(int i) { X return true; }
	virtual bool   IsNull(void) { X return true; }

	// Members
	JTYP Type;
}; // end of class JSON

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
	virtual void  Clear(void) {First = Last = NULL;}
//virtual JTYP  GetValType(void) {return TYPE_JOB;}
  virtual PJPR  GetFirst(void) {return First;}
	virtual int   GetSize(bool b);
  virtual PJOB  GetObject(void) {return this;}
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
  virtual void  Clear(void) {First = Last = NULL; Size = 0;}
	virtual int   size(void) { return Size; }
  virtual PJAR  GetArray(void) {return this;}
	virtual int   GetSize(bool b);
  virtual PJVAL GetArrayValue(int i);
	virtual PSZ   GetText(PGLOBAL g, PSTRG text);
	virtual bool  Merge(PGLOBAL g, PJSON jsp);
  virtual bool  DeleteValue(int n);
  virtual bool  IsNull(void);

	// Specific
	PJVAL AddArrayValue(PGLOBAL g, PJVAL jvp = NULL, int* x = NULL);
	void  SetArrayValue(PGLOBAL g, PJVAL jvp, int i);
	void  InitArray(PGLOBAL g);

 protected:
  // Members
	int    Size;		 // The number of items in the array
  int    Alloc;    // The Mvals allocated size
  PJVAL  First;    // Used when constructing
  PJVAL  Last;     // Last constructed value
  PJVAL *Mvals;    // Allocated when finished
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
//JVALUE(PGLOBAL g, PVL vlp);
	JVALUE(PGLOBAL g, PVAL valp);
	JVALUE(PGLOBAL g, PCSZ strp);
	JVALUE(int i) : JSON(i) {}

  //using JSON::GetVal;
  //using JSON::SetVal;

	// Methods
	virtual void   Clear(void);
//virtual JTYP   GetType(void) {return TYPE_JVAL;}
  virtual JTYP   GetValType(void);
  virtual PJOB   GetObject(void);
  virtual PJAR   GetArray(void);
  virtual PJSON  GetJsp(void) {return (DataType == TYPE_JSON ? Jsp : NULL);}
  virtual PSZ    GetText(PGLOBAL g, PSTRG text);
	virtual bool   IsNull(void);

	// Specific
	//inline PVL  GetVal(void) { return Val; }
	//inline void SetVal(PVL vlp) { Val = vlp; }
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
		 char  *Strp;      // Ptr to a string
		 int    N;         // An integer value
		 long long LLn;		 // A big integer value
		 double F;				 // A (double) float value
		 bool   B;				 // True or false
	 };
//PVL   Val;      // To the string or numeric value
	PJVAL Next;     // Next value in array
	JTYP  DataType; // The data value type
	int   Nd;				// Decimal number
	bool  Del;      // True when deleted
}; // end of class JVALUE


/***********************************************************************/
/* Class JOUT. Used by Serialize.                                      */
/***********************************************************************/
class JOUT : public BLOCK {
public:
	JOUT(PGLOBAL gp) : BLOCK() { g = gp; Pretty = 3; }

	virtual bool WriteStr(const char* s) = 0;
	virtual bool WriteChr(const char c) = 0;
	virtual bool Escape(const char* s) = 0;
	int  Prty(void) { return Pretty; }

	// Member
	PGLOBAL g;
	int     Pretty;
}; // end of class JOUT

/***********************************************************************/
/* Class JOUTSTR. Used to Serialize to a string.                       */
/***********************************************************************/
class JOUTSTR : public JOUT {
public:
	JOUTSTR(PGLOBAL g);

	virtual bool WriteStr(const char* s);
	virtual bool WriteChr(const char c);
	virtual bool Escape(const char* s);

	// Member
	char* Strp;                         // The serialized string
	size_t N;                            // Position of next char
	size_t Max;                          // String max size
}; // end of class JOUTSTR

/***********************************************************************/
/* Class JOUTFILE. Used to Serialize to a file.                        */
/***********************************************************************/
class JOUTFILE : public JOUT {
public:
	JOUTFILE(PGLOBAL g, FILE* str, int pty) : JOUT(g) { Stream = str; Pretty = pty; }

	virtual bool WriteStr(const char* s);
	virtual bool WriteChr(const char c);
	virtual bool Escape(const char* s);

	// Member
	FILE* Stream;
}; // end of class JOUTFILE

/***********************************************************************/
/* Class JOUTPRT. Used to Serialize to a pretty file.                  */
/***********************************************************************/
class JOUTPRT : public JOUTFILE {
public:
	JOUTPRT(PGLOBAL g, FILE* str) : JOUTFILE(g, str, 2) { M = 0; B = false; }

	virtual bool WriteStr(const char* s);
	virtual bool WriteChr(const char c);

	// Member
	int  M;
	bool B;
}; // end of class JOUTPRT


/***********************************************************************/
/* Class SWAP. Used to make or unmake a JSON tree movable.             */
/* This is done by making all pointers to offsets.                     */
/***********************************************************************/
class SWAP : public BLOCK {
public:
	// Constructor
	SWAP(PGLOBAL g, PJSON jsp) 
	{
		G = g, Base = (char*)jsp - 8;
	}

	// Methods
	void   SwapJson(PJSON jsp, bool move);

protected:
	size_t MoffJson(PJSON jnp);
	size_t MoffArray(PJAR jarp);
	size_t MoffObject(PJOB jobp);
	size_t MoffJValue(PJVAL jvp);
	size_t MoffPair(PJPR jpp);
//size_t MoffVal(PVL vlp);
	PJSON  MptrJson(PJSON jnp);
	PJAR   MptrArray(PJAR jarp);
	PJOB   MptrObject(PJOB jobp);
	PJVAL  MptrJValue(PJVAL jvp);
	PJPR   MptrPair(PJPR jpp);
//PVL    MptrVal(PVL vlp);

	// Member
	PGLOBAL G;
	void   *Base;
}; // end of class SWAP
