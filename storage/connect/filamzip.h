/************** filamzip H Declares Source Code File (.H) **************/
/*  Name: filamzip.h   Version 1.0                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2016         */
/*                                                                     */
/*  This file contains the ZIP file access method classes declares.    */
/***********************************************************************/
#ifndef __FILAMZIP_H
#define __FILAMZIP_H

#include "block.h"
#include "filamap.h"
#include "unzip.h"

#define DLLEXPORT extern "C"

typedef class ZIPFAM *PZIPFAM;
typedef class ZPXFAM *PZPXFAM;

/***********************************************************************/
/*  This is the ZIP utility fonctions class.                           */
/***********************************************************************/
class DllExport ZIPUTIL : public BLOCK {
public:
	// Constructor
	ZIPUTIL(PSZ tgt, bool mul);
//ZIPUTIL(ZIPUTIL *zutp);

	// Implementation
//PTXF Duplicate(PGLOBAL g) { return (PTXF) new(g)ZIPFAM(this); }

	// Methods
	virtual bool OpenTable(PGLOBAL g, MODE mode, char *fn);
	bool open(PGLOBAL g, char *fn);
	bool openEntry(PGLOBAL g);
	void close(void);
	void closeEntry(void);
	bool WildMatch(PSZ pat, PSZ str);
	int  findEntry(PGLOBAL g, bool next);
	int  nextEntry(PGLOBAL g);

	// Members
	unzFile         zipfile;							// The ZIP container file
	PSZ             target;								// The target file name
	unz_file_info   finfo;								// The current file info
	PFBLOCK         fp;
	char           *memory;
	uint            size;
	int             multiple;             // Multiple targets
	bool            entryopen;						// True when open current entry
	char            fn[FILENAME_MAX];     // The current entry file name
	char            mapCaseTable[256];
}; // end of ZIPFAM

/***********************************************************************/
/*  This is the ZIP file access method.                                */
/***********************************************************************/
class DllExport ZIPFAM : public MAPFAM {
	friend class ZPXFAM;
public:
	// Constructors
	ZIPFAM(PDOSDEF tdp);
	ZIPFAM(PZIPFAM txfp);
	ZIPFAM(PDOSDEF tdp, PZPXFAM txfp);

	// Implementation
	virtual AMT  GetAmType(void) { return TYPE_AM_ZIP; }
	virtual PTXF Duplicate(PGLOBAL g) { return (PTXF) new(g)ZIPFAM(this); }

	// Methods
	virtual int  Cardinality(PGLOBAL g);
	virtual int  GetFileLength(PGLOBAL g);
//virtual int  MaxBlkSize(PGLOBAL g, int s) {return s;}
	virtual bool OpenTableFile(PGLOBAL g);
	virtual bool DeferReading(void) { return false; }
	virtual int  GetNext(PGLOBAL g);
//virtual int  ReadBuffer(PGLOBAL g);
//virtual int  WriteBuffer(PGLOBAL g);
//virtual int  DeleteRecords(PGLOBAL g, int irc);
//virtual void CloseTableFile(PGLOBAL g, bool abort);

protected:
	// Members
	ZIPUTIL *zutp;
	PSZ      target;
	bool     mul;
}; // end of ZIPFAM

/***********************************************************************/
/*  This is the fixed ZIP file access method.                          */
/***********************************************************************/
class DllExport ZPXFAM : public MPXFAM {
	friend class ZIPFAM;
public:
	// Constructors
	ZPXFAM(PDOSDEF tdp);
	ZPXFAM(PZPXFAM txfp);

	// Implementation
	virtual AMT  GetAmType(void) { return TYPE_AM_ZIP; }
	virtual PTXF Duplicate(PGLOBAL g) { return (PTXF) new(g)ZPXFAM(this); }

	// Methods
	virtual int  GetFileLength(PGLOBAL g);
	virtual int  Cardinality(PGLOBAL g);
	virtual bool OpenTableFile(PGLOBAL g);
	virtual int  GetNext(PGLOBAL g);
//virtual int  ReadBuffer(PGLOBAL g);

protected:
	// Members
	ZIPUTIL *zutp;
	PSZ      target;
	bool     mul;
}; // end of ZPXFAM

#endif // __FILAMZIP_H
