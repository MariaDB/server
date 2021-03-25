/************** MongoFam H Declares Source Code File (.H) **************/
/*  Name: jmgfam.h    Version 1.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017 - 2020  */
/*                                                                     */
/*  This file contains the JAVA MongoDB access method classes declares */
/***********************************************************************/
#pragma once

/***********************************************************************/
/*  Include MongoDB library header files.                       	  	 */
/***********************************************************************/
#include "block.h"
//#include "mongo.h"
#include "jmgoconn.h"

typedef class JMGFAM *PJMGFAM;
typedef class MGODEF *PMGODEF;

/***********************************************************************/
/*  This is the Java MongoDB Access Method class declaration.          */
/***********************************************************************/
class DllExport JMGFAM : public DOSFAM {
	friend void mongo_init(bool);
public:
	// Constructor
	JMGFAM(PJDEF tdp);
#if defined(BSON_SUPPORT)
	JMGFAM(PBDEF tdp);
#endif   // BSON_SUPPORT
	JMGFAM(PJMGFAM txfp);

	// Implementation
	virtual AMT   GetAmType(void) { return TYPE_AM_MGO; }
	virtual bool  GetUseTemp(void) { return false; }
	virtual int   GetPos(void);
	virtual int   GetNextPos(void);
	virtual PTXF  Duplicate(PGLOBAL g) { return (PTXF)new(g) JMGFAM(this); }
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
//bool  MakeCursor(PGLOBAL g);

	// Members
	JMgoConn  *Jcp;              // Points to a Mongo connection class
	JDBCPARM   Ops;              // Additional parameters
	PFBLOCK    To_Fbt;           // Pointer to temp file block
	MODE       Mode;
	PCSZ       Uristr;
	PCSZ       Db_name;
	PCSZ       Coll_name;
	PCSZ       Options;
	PCSZ       Filter;
	PSZ        Wrapname;
	bool       Done;			       // Init done
	bool       Pipe;
	int        Version;
	int        Curpos;           // Cursor position of last fetch
}; // end of class JMGFAM

