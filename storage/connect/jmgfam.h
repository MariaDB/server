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
	AMT   GetAmType(void) override { return TYPE_AM_MGO; }
	bool  GetUseTemp(void) override { return false; }
	int   GetPos(void) override;
	int   GetNextPos(void) override;
	PTXF  Duplicate(PGLOBAL g) override { return (PTXF)new(g) JMGFAM(this); }
	void  SetLrecl(int lrecl) { Lrecl = lrecl; }

	// Methods
	void  Reset(void) override;
	int   GetFileLength(PGLOBAL g) override;
	int   Cardinality(PGLOBAL g) override;
	int   MaxBlkSize(PGLOBAL g, int s) override;
	bool  AllocateBuffer(PGLOBAL g) override { return false; }
	int   GetRowID(void) override;
	bool  RecordPos(PGLOBAL g) override;
	bool  SetPos(PGLOBAL g, int recpos) override;
	int   SkipRecord(PGLOBAL g, bool header) override;
	bool  OpenTableFile(PGLOBAL g) override;
	int   ReadBuffer(PGLOBAL g) override;
	int   WriteBuffer(PGLOBAL g) override;
	int   DeleteRecords(PGLOBAL g, int irc) override;
	void  CloseTableFile(PGLOBAL g, bool abort) override;
	void  Rewind(void) override;

protected:
	bool  OpenTempFile(PGLOBAL g) override { return false; }
	bool  MoveIntermediateLines(PGLOBAL g, bool *b) override { return false; }
	int   RenameTempFile(PGLOBAL g) override { return RC_OK; }
	int   InitDelete(PGLOBAL g, int fpos, int spos) override;
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

