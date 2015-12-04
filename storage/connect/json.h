/**************** json H Declares Source Code File (.H) ****************/
/*  Name: json.h   Version 1.2                                         */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2015  */
/*                                                                     */
/*  This file contains the JSON classes declares.                      */
/***********************************************************************/
#include "value.h"

#if defined(_DEBUG)
#define X  assert(false);
#else
#define X
#endif

enum JTYP {TYPE_STRG = 1, 
           TYPE_DBL = 2,
           TYPE_BOOL = 4,
           TYPE_BINT = 5, 
					 TYPE_INTG = 7,
					 TYPE_JSON = 12,
           TYPE_JAR, 
           TYPE_JOB, 
           TYPE_JVAL};

class JOUT;
class JSON;
class JMAP;
class JVALUE;
class JOBJECT;
class JARRAY;

typedef class JPAIR   *PJPR;
typedef class JSON    *PJSON;
typedef class JVALUE  *PJVAL;
typedef class JOBJECT *PJOB;
typedef class JARRAY  *PJAR;

typedef struct {
  char *str;
  int   len;
  } STRG, *PSG;

PJSON ParseJson(PGLOBAL g, char *s, int n, int *prty = NULL, bool *b = NULL);
PJAR  ParseArray(PGLOBAL g, int& i, STRG& src, bool *pty);
PJOB  ParseObject(PGLOBAL g, int& i, STRG& src, bool *pty);
PJVAL ParseValue(PGLOBAL g, int& i, STRG& src, bool *pty);
char *ParseString(PGLOBAL g, int& i, STRG& src);
PVAL  ParseNumeric(PGLOBAL g, int& i, STRG& src);
PSZ   Serialize(PGLOBAL g, PJSON jsp, char *fn, int pretty);
bool  SerializeArray(JOUT *js, PJAR jarp, bool b);
bool  SerializeObject(JOUT *js, PJOB jobp);
bool  SerializeValue(JOUT *js, PJVAL jvp);

/***********************************************************************/
/* Class JOUT. Used by Serialize.                                      */
/***********************************************************************/
class JOUT : public BLOCK {
 public:
	JOUT(PGLOBAL gp) : BLOCK() {g = gp; Pretty = 3;}

  virtual bool WriteStr(const char *s) = 0;
  virtual bool WriteChr(const char c) = 0;
  virtual bool Escape(const char *s) = 0;
					int  Prty(void) {return Pretty;}

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

  virtual bool WriteStr(const char *s);
  virtual bool WriteChr(const char c);
  virtual bool Escape(const char *s);

  // Member
  char  *Strp;                         // The serialized string
  size_t N;                            // Position of next char
  size_t Max;                          // String max size
}; // end of class JOUTSTR

/***********************************************************************/
/* Class JOUTFILE. Used to Serialize to a file.                        */
/***********************************************************************/
class JOUTFILE : public JOUT {
 public:
	JOUTFILE(PGLOBAL g, FILE *str, int pty) : JOUT(g) {Stream = str; Pretty = pty;}

  virtual bool WriteStr(const char *s);
  virtual bool WriteChr(const char c);
  virtual bool Escape(const char *s);

  // Member
  FILE *Stream;
}; // end of class JOUTFILE

/***********************************************************************/
/* Class JOUTPRT. Used to Serialize to a pretty file.                  */
/***********************************************************************/
class JOUTPRT : public JOUTFILE {
 public:
  JOUTPRT(PGLOBAL g, FILE *str) : JOUTFILE(g, str, 2) {M = 0; B = false;}

  virtual bool WriteStr(const char *s);
  virtual bool WriteChr(const char c);

  // Member
  int  M;
  bool B;
}; // end of class JOUTPRT

/***********************************************************************/
/* Class PAIR. The pairs of a json Object.                             */
/***********************************************************************/
class JPAIR : public BLOCK {
  friend class JOBJECT;
	friend class JSNX;
	friend PJOB ParseObject(PGLOBAL, int&, STRG&, bool*);
  friend bool SerializeObject(JOUT *, PJOB);
 public:
  JPAIR(PSZ key) : BLOCK() {Key = key; Val = NULL; Next = NULL;}

  inline PSZ   GetKey(void) {return Key;}
  inline PJVAL GetVal(void) {return Val;}
  inline PJPR  GetNext(void) {return Next;}

 protected:
  PSZ   Key;      // This pair key name
  PJVAL Val;      // To the value of the pair
  PJPR  Next;     // To the next pair
}; // end of class JPAIR

/***********************************************************************/
/* Class JSON. The base class for all other json classes.              */
/***********************************************************************/
class JSON : public BLOCK {
 public:
  JSON(void) {Size = 0;}

          int    size(void) {return Size;}
  virtual void   Clear(void) {Size = 0;}
  virtual JTYP   GetType(void) {return TYPE_JSON;}
  virtual JTYP   GetValType(void) {X return TYPE_JSON;}
  virtual void   InitArray(PGLOBAL g) {X}
//virtual PJVAL  AddValue(PGLOBAL g, PJVAL jvp = NULL, int *x = NULL) {X return NULL;}
  virtual PJPR   AddPair(PGLOBAL g, PSZ key) {X return NULL;}
	virtual PJAR   GetKeyList(PGLOBAL g) {X return NULL;}
	virtual PJVAL  GetValue(const char *key) {X return NULL;}
  virtual PJOB   GetObject(void) {return NULL;}
  virtual PJAR   GetArray(void) {return NULL;}
	virtual PJVAL  GetValue(int i) {X return NULL;}
  virtual PVAL   GetValue(void) {X return NULL;}
	virtual PJSON  GetJsp(void) { X return NULL; }
	virtual PJSON  GetJson(void) { X return NULL; }
  virtual PJPR   GetFirst(void) {X return NULL;}
  virtual int    GetInteger(void) {X return 0;}
  virtual double GetFloat() {X return 0.0;}
  virtual PSZ    GetString() {X return NULL;}
  virtual PSZ    GetText(PGLOBAL g, PSZ text) {X return NULL;}
	virtual bool   Merge(PGLOBAL g, PJSON jsp) { X return true; }
	virtual bool   SetValue(PGLOBAL g, PJVAL jvp, int i) { X return true; }
  virtual void   SetValue(PGLOBAL g, PJVAL jvp, PSZ key) {X}
  virtual void   SetValue(PVAL valp) {X}
  virtual void   SetValue(PJSON jsp) {X}
  virtual void   SetString(PGLOBAL g, PSZ s, short c) {X}
  virtual void   SetInteger(PGLOBAL g, int n) {X}
  virtual void   SetFloat(PGLOBAL g, double f) {X}
	virtual void   DeleteKey(char *k) {X}
	virtual bool   DeleteValue(int i) {X return true;}
	virtual bool   IsNull(void) {X return true;}

 protected:
  int Size;
}; // end of class JSON

/***********************************************************************/
/* Class JOBJECT: contains a list of value pairs.                      */
/***********************************************************************/
class JOBJECT : public JSON {
  friend PJOB ParseObject(PGLOBAL, int&, STRG&, bool*);
  friend bool SerializeObject(JOUT *, PJOB);
	friend class JSNX;
 public:
  JOBJECT(void) : JSON() {First = Last = NULL;}

  using JSON::GetValue;
  using JSON::SetValue;
  virtual void  Clear(void) {First = Last = NULL; Size = 0;}
  virtual JTYP  GetType(void) {return TYPE_JOB;}
  virtual PJPR  GetFirst(void) {return First;}
  virtual PJPR  AddPair(PGLOBAL g, PSZ key);
  virtual PJOB  GetObject(void) {return this;}
  virtual PJVAL GetValue(const char* key);
	virtual PJAR  GetKeyList(PGLOBAL g);
	virtual PSZ   GetText(PGLOBAL g, PSZ text);
	virtual bool  Merge(PGLOBAL g, PJSON jsp);
	virtual void  SetValue(PGLOBAL g, PJVAL jvp, PSZ key);
	virtual void  DeleteKey(char *k);
	virtual bool  IsNull(void);

 protected:
  PJPR First;
  PJPR Last;
}; // end of class JOBJECT

/***********************************************************************/
/* Class JARRAY.                                                       */
/***********************************************************************/
class JARRAY : public JSON {
  friend PJAR ParseArray(PGLOBAL, int&, STRG&, bool*);
 public:
  JARRAY(void) : JSON() {Alloc = 0; First = Last = NULL; Mvals = NULL;}

  using JSON::GetValue;
  using JSON::SetValue;
  virtual void  Clear(void) {First = Last = NULL; Size = 0;}
  virtual JTYP  GetType(void) {return TYPE_JAR;}
  virtual PJAR  GetArray(void) {return this;}
          PJVAL AddValue(PGLOBAL g, PJVAL jvp = NULL, int *x = NULL);
  virtual void  InitArray(PGLOBAL g);
  virtual PJVAL GetValue(int i);
	virtual bool  Merge(PGLOBAL g, PJSON jsp);
	virtual bool  SetValue(PGLOBAL g, PJVAL jvp, int i);
  virtual bool  DeleteValue(int n);
  virtual bool  IsNull(void);

 protected:
  // Members
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
	friend PJVAL ParseValue(PGLOBAL, int&, STRG&, bool*);
  friend bool  SerializeValue(JOUT *, PJVAL);
 public:
  JVALUE(void) : JSON() 
                {Jsp = NULL; Value = NULL; Next = NULL; Del = false;}
  JVALUE(PJSON jsp) : JSON()
                {Jsp = jsp; Value = NULL; Next = NULL; Del = false;}
  JVALUE(PGLOBAL g, PVAL valp);
	JVALUE(PGLOBAL g, PSZ strp);

  using JSON::GetValue;
  using JSON::SetValue;
  virtual void   Clear(void)
          {Jsp = NULL; Value = NULL; Next = NULL; Del = false; Size = 0;}
  virtual JTYP   GetType(void) {return TYPE_JVAL;}
  virtual JTYP   GetValType(void);
  virtual PJOB   GetObject(void);
  virtual PJAR   GetArray(void);
  virtual PVAL   GetValue(void) {return Value;}
  virtual PJSON  GetJsp(void) {return Jsp;}
	virtual PJSON  GetJson(void) { return (Jsp ? Jsp : this); }
	virtual int    GetInteger(void);
	virtual long long GetBigint(void);
	virtual double GetFloat(void);
  virtual PSZ    GetString(void);
  virtual PSZ    GetText(PGLOBAL g, PSZ text);
	virtual void   SetValue(PJSON jsp);
	virtual void   SetValue(PVAL valp) { Value = valp; Jsp = NULL; }
	virtual void   SetString(PGLOBAL g, PSZ s, short c = 0);
  virtual void   SetInteger(PGLOBAL g, int n);
	virtual void   SetBigint(PGLOBAL g, longlong ll);
	virtual void   SetFloat(PGLOBAL g, double f);
	virtual void   SetTiny(PGLOBAL g, char f);
	virtual bool   IsNull(void);

 protected:
  PJSON Jsp;      // To the json value
  PVAL  Value;    // The numeric value
  PJVAL Next;     // Next value in array
  bool  Del;      // True when deleted
}; // end of class JVALUE

