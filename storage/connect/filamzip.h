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
/*  This is the ZIP file access method.                                */
/***********************************************************************/
class DllExport ZIPFAM : public MAPFAM {
public:
	// Constructor
	ZIPFAM(PDOSDEF tdp);
	ZIPFAM(PZIPFAM txfp);

	// Implementation
	virtual AMT  GetAmType(void) {return TYPE_AM_ZIP;}
  virtual PTXF Duplicate(PGLOBAL g) {return (PTXF) new(g) ZIPFAM(this);}

	// Methods
	virtual int  GetFileLength(PGLOBAL g);
	virtual int  Cardinality(PGLOBAL g) {return (g) ? 10 : 1;}
//virtual int  MaxBlkSize(PGLOBAL g, int s) {return s;}
	virtual bool OpenTableFile(PGLOBAL g);
	virtual bool DeferReading(void) {return false;}
  virtual int  ReadBuffer(PGLOBAL g);
//virtual int  WriteBuffer(PGLOBAL g);
//virtual int  DeleteRecords(PGLOBAL g, int irc);
//virtual void CloseTableFile(PGLOBAL g, bool abort);
	        void close(void);

protected:
	bool open(PGLOBAL g, const char *filename);
	bool openEntry(PGLOBAL g);
	void closeEntry(void);
	bool WildMatch(PSZ pat, PSZ str);
	int  findEntry(PGLOBAL g, bool next);

	// Members
	unzFile         zipfile;							   // The ZIP container file
	PSZ             zfn;									   // The ZIP file name
	PSZ             target;									 // The target file name
	unz_file_info   finfo;									 // The current file info
//char            fn[FILENAME_MAX];     	 // The current file name
	bool            entryopen;							 // True when open current entry
	int             multiple;                // Multiple targets
	char            mapCaseTable[256];
}; // end of ZIPFAM

/***********************************************************************/
/*  This is the fixed ZIP file access method.                          */
/***********************************************************************/
class DllExport ZPXFAM : public ZIPFAM {
public:
	// Constructor
	ZPXFAM(PDOSDEF tdp);
	ZPXFAM(PZPXFAM txfp);

	// Implementation
	virtual PTXF Duplicate(PGLOBAL g) {return (PTXF) new(g) ZPXFAM(this);}

	// Methods
	virtual int  ReadBuffer(PGLOBAL g);

protected:
	// Members
	int Lrecl;
}; // end of ZPXFAM

#endif // __FILAMZIP_H
