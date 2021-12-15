/*************** CMGFam H Declares Source Code File (.H) ***************/
/*  Name: cmgfam.h    Version 1.6                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017 - 2020  */
/*                                                                     */
/*  This file contains the MongoDB access method classes declares.     */
/***********************************************************************/
#include "cmgoconn.h"

typedef class TXTFAM *PTXF;
typedef class CMGFAM *PCMGFAM;
typedef class MGODEF *PMGODEF;
typedef class TDBCMG *PTDBCMG;

/***********************************************************************/
/*  This is the MongoDB Access Method class declaration.               */
/***********************************************************************/
class DllExport CMGFAM : public DOSFAM {
	friend void mongo_init(bool);
public:
	// Constructor
	CMGFAM(PJDEF tdp);
#if defined(BSON_SUPPORT)
	CMGFAM(PBDEF tdp);
#endif   // BSON_SUPPORT
	CMGFAM(PCMGFAM txfp);

	// Implementation
	virtual AMT   GetAmType(void) { return TYPE_AM_MGO; }
	virtual bool  GetUseTemp(void) { return false; }
	virtual int   GetPos(void);
	virtual int   GetNextPos(void);
	void  SetTdbp(PTDBDOS tdbp) { Tdbp = tdbp; }
	virtual PTXF  Duplicate(PGLOBAL g) { return (PTXF)new(g) CMGFAM(this); }
	void  SetLrecl(int lrecl) { Lrecl = lrecl; }

	// Methods
	virtual void  Reset(void);
	virtual int   GetFileLength(PGLOBAL g);
	virtual int   Cardinality(PGLOBAL g);
	virtual int   MaxBlkSize(PGLOBAL g, int s);
	virtual bool  AllocateBuffer(PGLOBAL g) { return false; }
	virtual int   GetRowID(void);
	virtual bool  RecordPos(PGLOBAL g);
	virtual bool  SetPos(PGLOBAL g, int recpos);
	virtual int   SkipRecord(PGLOBAL g, bool header);
	virtual bool  OpenTableFile(PGLOBAL g);
	virtual int   ReadBuffer(PGLOBAL g);
	virtual int   WriteBuffer(PGLOBAL g);
	virtual int   DeleteRecords(PGLOBAL g, int irc);
	virtual void  CloseTableFile(PGLOBAL g, bool abort);
	virtual void  Rewind(void);

protected:
	virtual bool  OpenTempFile(PGLOBAL g) { return false; }
	virtual bool  MoveIntermediateLines(PGLOBAL g, bool *b) { return false; }
	virtual int   RenameTempFile(PGLOBAL g) { return RC_OK; }
	virtual int   InitDelete(PGLOBAL g, int fpos, int spos);
	bool  Init(PGLOBAL g);

	// Members
	CMgoConn *Cmgp;       // Points to a C Mongo connection class
	CMGOPARM	Pcg;				// Parms passed to Cmgp
	PFBLOCK   To_Fbt;     // Pointer to temp file block
	MODE      Mode;
	bool      Done;			  // Init done
}; // end of class CMGFAM

