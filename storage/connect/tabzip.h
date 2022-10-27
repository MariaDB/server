/*************** tabzip H Declares Source Code File (.H) ***************/
/*  Name: tabzip.h   Version 1.0                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2016         */
/*                                                                     */
/*  This file contains the ZIP classe declares.                        */
/***********************************************************************/
#include "osutil.h"
#include "block.h"
#include "colblk.h"
#include "xtable.h"
#include "unzip.h"

typedef class ZIPDEF *PZIPDEF;
typedef class TDBZIP *PTDBZIP;
typedef class ZIPCOL *PZIPCOL;

/***********************************************************************/
/*  ZIP table: display info about a ZIP file.                          */
/***********************************************************************/
class DllExport ZIPDEF : public DOSDEF {          /* Table description */
	friend class TDBZIP;
	friend class UNZFAM;
public:
	// Constructor
	ZIPDEF(void) {}

	// Implementation
	virtual const char *GetType(void) {return "ZIP";}

	// Methods
	virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
	virtual PTDB GetTable(PGLOBAL g, MODE m);

protected:
	// Members
	PCSZ        target;								 // The inside file to query
}; // end of ZIPDEF

/***********************************************************************/
/*  This is the ZIP Access Method class declaration.                   */
/***********************************************************************/
class DllExport TDBZIP : public TDBASE {
	friend class ZIPCOL;
public:
	// Constructor
	TDBZIP(PZIPDEF tdp);

	// Implementation
	virtual AMT  GetAmType(void) {return TYPE_AM_ZIP;}
	virtual PCSZ GetFile(PGLOBAL) {return zfn;}
	virtual void SetFile(PGLOBAL, PCSZ fn) {zfn = fn;}

	// Methods
	virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
	virtual int  Cardinality(PGLOBAL g);
	virtual int  GetMaxSize(PGLOBAL g);
	virtual int  GetRecpos(void) {return 0;}

	// Database routines
	virtual bool OpenDB(PGLOBAL g);
	virtual int  ReadDB(PGLOBAL g);
	virtual int  WriteDB(PGLOBAL g);
	virtual int  DeleteDB(PGLOBAL g, int irc);
	virtual void CloseDB(PGLOBAL g);

protected:
	bool open(PGLOBAL g, const char *filename);
	void close(void);

	// Members
	unzFile         zipfile;							   // The ZIP container file
	PCSZ            zfn;									   // The ZIP file name
//PSZ             target;
	unz_file_info64 finfo;									 // The current file info
	char            fn[FILENAME_MAX];     	 // The current file name
	int             nexterr;								 // Next file error
}; // end of class TDBZIP

/***********************************************************************/
/*  Class ZIPCOL: ZIP access method column descriptor.                 */
/***********************************************************************/
class DllExport ZIPCOL : public COLBLK {
	friend class TDBZIP;
public:
	// Constructors
	ZIPCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "ZIP");

	// Implementation
	virtual int  GetAmType(void) { return TYPE_AM_ZIP; }

	// Methods
	virtual void ReadColumn(PGLOBAL g);

protected:
	// Default constructor not to be used
	ZIPCOL(void) {}

	// Members
	TDBZIP *Tdbz;
	int     flag;
}; // end of class ZIPCOL
