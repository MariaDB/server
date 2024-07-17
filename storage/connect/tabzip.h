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
	ZIPDEF(void) = default;

	// Implementation
	const char *GetType(void) override {return "ZIP";}

	// Methods
	bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
	PTDB GetTable(PGLOBAL g, MODE m) override;

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
	AMT  GetAmType(void) override {return TYPE_AM_ZIP;}
	PCSZ GetFile(PGLOBAL) override {return zfn;}
	void SetFile(PGLOBAL, PCSZ fn) override {zfn = fn;}

	// Methods
	PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
	int  Cardinality(PGLOBAL g) override;
	int  GetMaxSize(PGLOBAL g) override;
	int  GetRecpos(void) override {return 0;}

	// Database routines
	bool OpenDB(PGLOBAL g) override;
	int  ReadDB(PGLOBAL g) override;
	int  WriteDB(PGLOBAL g) override;
	int  DeleteDB(PGLOBAL g, int irc) override;
	void CloseDB(PGLOBAL g) override;

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
	int  GetAmType(void) override { return TYPE_AM_ZIP; }

	// Methods
	void ReadColumn(PGLOBAL g) override;

protected:
	// Default constructor not to be used
	ZIPCOL(void) = default;

	// Members
	TDBZIP *Tdbz;
	int     flag;
}; // end of class ZIPCOL
