/************** filamzip H Declares Source Code File (.H) **************/
/*  Name: filamzip.h   Version 1.3                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2016-2020    */
/*                                                                     */
/*  This file contains the ZIP file access method classes declares.    */
/***********************************************************************/
#ifndef __FILAMZIP_H
#define __FILAMZIP_H

#include "block.h"
#include "filamap.h"
#include "filamfix.h"
#include "filamdbf.h"
#include "zip.h"
#include "unzip.h"

#define DLLEXPORT extern "C"

typedef class UNZFAM *PUNZFAM;
typedef class UZXFAM *PUZXFAM;
typedef class UZDFAM* PUZDFAM;
typedef class ZIPFAM *PZIPFAM;
typedef class ZPXFAM *PZPXFAM;

/***********************************************************************/
/*  This is the ZIP utility fonctions class.                           */
/***********************************************************************/
class DllExport ZIPUTIL : public BLOCK {
 public:
	// Constructor
	ZIPUTIL(PCSZ tgt);
	//ZIPUTIL(ZIPUTIL *zutp);

	// Methods
	bool OpenTable(PGLOBAL g, MODE mode, PCSZ fn, bool append);
	bool open(PGLOBAL g, PCSZ fn, bool append);
	bool addEntry(PGLOBAL g, PCSZ entry);
	void close(void);
	void closeEntry(void);
  int  writeEntry(PGLOBAL g, char *buf, int len);
	void getTime(tm_zip& tmZip);

	// Members
	zipFile         zipfile;							// The ZIP container file
	PCSZ            target;								// The target file name
	PCSZ            pwd;								  // The ZIP file password
	PFBLOCK         fp;
	bool            entryopen;						// True when open current entry
}; // end of ZIPUTIL

/***********************************************************************/
/*  This is the unZIP utility fonctions class.                         */
/***********************************************************************/
class DllExport UNZIPUTL : public BLOCK {
 public:
	// Constructor
  UNZIPUTL(PCSZ tgt, PCSZ pw, bool mul);
  UNZIPUTL(PDOSDEF tdp);

	// Implementation
//PTXF Duplicate(PGLOBAL g) { return (PTXF) new(g)UNZFAM(this); }

	// Methods
	bool OpenTable(PGLOBAL g, MODE mode, PCSZ fn);
	bool open(PGLOBAL g, PCSZ fn);
	bool openEntry(PGLOBAL g);
	void close(void);
	void closeEntry(void);
	bool WildMatch(PCSZ pat, PCSZ str);
	int  findEntry(PGLOBAL g, bool next);
	int  nextEntry(PGLOBAL g);
	bool IsInsertOk(PGLOBAL g, PCSZ fn);

	// Members
	unzFile         zipfile;							// The ZIP container file
	PCSZ            target;								// The target file name
	PCSZ            pwd;								  // The ZIP file password
	unz_file_info   finfo;								// The current file info
	PFBLOCK         fp;
	char           *memory;
	uint            size;
	int             multiple;             // Multiple targets
	bool            entryopen;						// True when open current entry
	char            fn[FILENAME_MAX];     // The current entry file name
	char            mapCaseTable[256];
}; // end of UNZIPUTL

/***********************************************************************/
/*  This is the unzip file access method.                              */
/***********************************************************************/
class DllExport UNZFAM : public MAPFAM {
//friend class UZXFAM;
 public:
	// Constructors
	UNZFAM(PDOSDEF tdp);
	UNZFAM(PUNZFAM txfp);

	// Implementation
	AMT  GetAmType(void) override {return TYPE_AM_ZIP;}
	PTXF Duplicate(PGLOBAL g) override {return (PTXF) new(g) UNZFAM(this);}

	// Methods
	int  Cardinality(PGLOBAL g) override;
	int  GetFileLength(PGLOBAL g) override;
	//virtual int  MaxBlkSize(PGLOBAL g, int s) {return s;}
	bool OpenTableFile(PGLOBAL g) override;
	bool DeferReading(void) override { return false; }
	int  GetNext(PGLOBAL g) override;
	//virtual int  ReadBuffer(PGLOBAL g);
	//virtual int  WriteBuffer(PGLOBAL g);
	//virtual int  DeleteRecords(PGLOBAL g, int irc);
	//virtual void CloseTableFile(PGLOBAL g, bool abort);

 protected:
	// Members
	UNZIPUTL *zutp;
	PDOSDEF   tdfp;
}; // end of UNZFAM

/***********************************************************************/
/*  This is the fixed unzip file access method.                        */
/***********************************************************************/
class DllExport UZXFAM : public MPXFAM {
//friend class UNZFAM;
 public:
	// Constructors
	UZXFAM(PDOSDEF tdp);
	UZXFAM(PUZXFAM txfp);

	// Implementation
	AMT  GetAmType(void) override { return TYPE_AM_ZIP; }
	PTXF Duplicate(PGLOBAL g) override { return (PTXF) new(g)UZXFAM(this); }

	// Methods
	int  GetFileLength(PGLOBAL g) override;
	int  Cardinality(PGLOBAL g) override;
	bool OpenTableFile(PGLOBAL g) override;
	int  GetNext(PGLOBAL g) override;
	//virtual int  ReadBuffer(PGLOBAL g);

 protected:
	// Members
	UNZIPUTL *zutp;
	PDOSDEF   tdfp;
}; // end of UZXFAM

/***********************************************************************/
/*  This is the fixed unzip file access method.                        */
/***********************************************************************/
class DllExport UZDFAM : public DBMFAM {
	//friend class UNZFAM;
public:
	// Constructors
	UZDFAM(PDOSDEF tdp);
	UZDFAM(PUZDFAM txfp);

	// Implementation
	AMT  GetAmType(void) override { return TYPE_AM_ZIP; }
	PTXF Duplicate(PGLOBAL g) override { return (PTXF) new(g)UZDFAM(this); }

	// Methods
	int  GetFileLength(PGLOBAL g) override;
	int  Cardinality(PGLOBAL g) override;
	bool OpenTableFile(PGLOBAL g) override;
	int  GetNext(PGLOBAL g) override;
	//virtual int  ReadBuffer(PGLOBAL g);

protected:
	int dbfhead(PGLOBAL g, void* buf);
	int ScanHeader(PGLOBAL g, int* rln);

	// Members
	UNZIPUTL* zutp;
	PDOSDEF   tdfp;
}; // end of UZDFAM

/***********************************************************************/
/*  This is the zip file access method.                                */
/***********************************************************************/
class DllExport ZIPFAM : public DOSFAM {
 public:
	// Constructors
	ZIPFAM(PDOSDEF tdp);

	// Implementation
	AMT  GetAmType(void) override {return TYPE_AM_ZIP;}

	// Methods
	int  Cardinality(PGLOBAL g) override {return 0;}
	int  GetFileLength(PGLOBAL g) override {return g ? 0 : 1;}
	//virtual int  MaxBlkSize(PGLOBAL g, int s) {return s;}
	bool OpenTableFile(PGLOBAL g) override;
	int  ReadBuffer(PGLOBAL g) override;
	int  WriteBuffer(PGLOBAL g) override;
	//virtual int  DeleteRecords(PGLOBAL g, int irc);
	void CloseTableFile(PGLOBAL g, bool abort) override;

 protected:
	// Members
	ZIPUTIL *zutp;
	PCSZ     target;
	bool     append;
//bool     replace;
}; // end of ZIPFAM

/***********************************************************************/
/*  This is the fixed zip file access method.                          */
/***********************************************************************/
class DllExport ZPXFAM : public FIXFAM {
 public:
	// Constructors
	ZPXFAM(PDOSDEF tdp);

	// Implementation
	AMT  GetAmType(void) override {return TYPE_AM_ZIP;}

	// Methods
	int  Cardinality(PGLOBAL g) override {return 0;}
	int  GetFileLength(PGLOBAL g) override {return g ? 0 : 1;}
	bool OpenTableFile(PGLOBAL g) override;
	int  WriteBuffer(PGLOBAL g) override;
	void CloseTableFile(PGLOBAL g, bool abort) override;

 protected:
	// Members
	ZIPUTIL *zutp;
	PCSZ      target;
	bool     append;
}; // end of ZPXFAM

#endif // __FILAMZIP_H
