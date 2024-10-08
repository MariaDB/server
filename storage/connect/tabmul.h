/*************** Tabmul H Declares Source Code File (.H) ***************/
/*  Name: TABMUL.H   Version 1.5                                       */
/*                                                                     */
/*  (C) Copyright to PlugDB Software Development          2003-2017    */
/*  Author: Olivier BERTRAND                                           */
/*                                                                     */
/*  This file contains the TDBMUL and TDBDIR classes declares.         */
/***********************************************************************/
#if defined(_WIN32)
#include <io.h>
#else   // !_WIN32
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif  // !_WIN32
//#include "osutil.h"
#include "block.h"

typedef class TDBMUL *PTDBMUL;
typedef class TDBSDR *PTDBSDR;

/***********************************************************************/
/*  This is the MUL Access Method class declaration for files that are */
/*  physically split in multiple files having the same format.         */
/***********************************************************************/
class DllExport TDBMUL : public TDBASE {
//friend class MULCOL;
 public:
  // Constructor
  TDBMUL(PTDB tdbp);
  TDBMUL(PTDBMUL tdbp);

  // Implementation
  AMT  GetAmType(void) override {return Tdbp->GetAmType();}
  PTDB Duplicate(PGLOBAL g) override;

  // Methods
  void ResetDB(void) override;
  PTDB Clone(PTABS t) override;
  bool IsSame(PTDB tp) override {return tp == (PTDB)Tdbp;}
  PCSZ GetFile(PGLOBAL g) override {return Tdbp->GetFile(g);}
  int  GetRecpos(void) override {return 0;}
  PCOL ColDB(PGLOBAL g, PSZ name, int num) override;
          bool InitFileNames(PGLOBAL g);

  // Database routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override
                {strcpy(g->Message, MSG(MUL_MAKECOL_ERR)); return NULL;}
  int  Cardinality(PGLOBAL g) override;
  int  GetMaxSize(PGLOBAL g) override;
  int  GetProgMax(PGLOBAL g) override;
  int  GetProgCur(void) override;
  int  RowNumber(PGLOBAL g, bool b = false) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
  int  DeleteDB(PGLOBAL g, int irc) override;
  void CloseDB(PGLOBAL g) override;

 protected:

  // Members
  PTDB    Tdbp;               // Points to a (file) table class
  char*  *Filenames;          // Points to file names
  int     Rows;               // Total rows of already read files
  int     Mul;                // Type of multiple file list
  int     NumFiles;           // Number of physical files
  int     iFile;              // Index of currently processed file
  }; // end of class TDBMUL

#if 0
/***********************************************************************/
/*  This is the MSD Access Method class declaration for files that are */
/*  physically split in multiple files having the same format.         */
/*  This sub-class also include files of the sub-directories.          */
/***********************************************************************/
class DllExport TDBMSD : public TDBMUL {
	//friend class MULCOL;
public:
	// Constructor
	TDBMSD(PTDB tdbp) : TDBMUL(tdbp) {}
	TDBMSD(PTDBMSD tdbp) : TDBMUL(tdbp) {}

	// Implementation
	PTDB Duplicate(PGLOBAL g);

	// Methods
	PTDB Clone(PTABS t) override;
	bool InitFileNames(PGLOBAL g);

	// Database routines

protected:

	// Members
}; // end of class TDBMSD
#endif

/***********************************************************************/
/*  Directory listing table.                                           */
/***********************************************************************/
class DllExport DIRDEF : public TABDEF {    /* Directory listing table */
  friend class CATALOG;
  friend class TDBDIR;
 public:
  // Constructor
  DIRDEF(void) {Fn = NULL; Incl = false; Huge = false;}

  // Implementation
  const char *GetType(void) override {return "DIR";}

  // Methods
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE m) override;

 protected:
  // Members
  PSZ     Fn;                 /* Path/Name of file search              */
  bool    Incl;               /* true to include sub-directories       */
	bool    Huge;               /* true if files can be larger than 2GB  */
	bool    Nodir;							/* true to exclude directories           */
  }; // end of DIRDEF

/***********************************************************************/
/*  This is the DIR Access Method class declaration for tables that    */
/*  represent a directory listing. The pathname is given at the create */
/*  time and can contain wildcard characters in the file name, and the */
/*  (virtual) table is populated when it is in use.                    */
/***********************************************************************/
class TDBDIR : public TDBASE {
  friend class DIRCOL;
	friend class TDBMUL;
public:
  // Constructor
  TDBDIR(PDIRDEF tdp);
	TDBDIR(PSZ fpat);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_DIR;}

  // Methods
  int GetRecpos(void) override {return iFile;}

  // Database routines
  PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int  GetMaxSize(PGLOBAL g) override;
  int  GetProgMax(PGLOBAL g) override {return GetMaxSize(g);}
  int  GetProgCur(void) override {return iFile;}
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
  int  DeleteDB(PGLOBAL g, int irc) override;
  void CloseDB(PGLOBAL g) override;

 protected:
	void Init(void);
  char *Path(PGLOBAL g);

  // Members
  PSZ  To_File;                 // Points to file search pathname
  int  iFile;                   // Index of currently retrieved file
#if defined(_WIN32)
	PVAL Dvalp;							      // Used to retrieve file date values
	WIN32_FIND_DATA FileData;			// Find data structure
	HANDLE hSearch;               // Search handle
  char Drive[_MAX_DRIVE];       // Drive name
#else   // !_WIN32
  struct stat    Fileinfo;      // File info structure
  struct dirent *Entry;         // Point to directory entry structure
  DIR *Dir;                     // To searched directory structure
  bool Done;                    // true when _splipath is done
  char Pattern[_MAX_FNAME+_MAX_EXT];
#endif  // !_WIN32
  char Fpath[_MAX_PATH];        // Absolute file search pattern
  char Direc[_MAX_DIR];         // Search path
  char Fname[_MAX_FNAME];       // File name
  char Ftype[_MAX_EXT];         // File extention
	bool Nodir;                   // Exclude directories from file list
  }; // end of class TDBDIR

/***********************************************************************/
/*  This is the DIR Access Method class declaration for tables that    */
/*  represent a directory listing. The pathname is given at the create */
/*  time and can contain wildcard characters in the file name, and the */
/*  (virtual) table is populated when it is in use. In addition, this  */
/*  class also includes files of included sub-directories.             */
/***********************************************************************/
class TDBSDR : public TDBDIR {
  friend class DIRCOL;
	friend class TDBMUL;
 public:
  // Constructors
  TDBSDR(PDIRDEF tdp) : TDBDIR(tdp) {Sub = NULL;}
	TDBSDR(PSZ fpat) : TDBDIR(fpat) {Sub = NULL;}

  // Database routines
  int  GetMaxSize(PGLOBAL g) override;
  int  GetProgMax(PGLOBAL g) override {return GetMaxSize(g);}
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
//void CloseDB(PGLOBAL g) override;

 protected:
          int  FindInDir(PGLOBAL g);

  typedef struct _Sub_Dir {
    struct _Sub_Dir *Next;
    struct _Sub_Dir *Prev;
#if defined(_WIN32)
    HANDLE H;               // Search handle
#else   // !_WIN32
    DIR *D;
#endif  // !_WIN32
    size_t Len;           // Initial directory name length
    } SUBDIR, *PSUBDIR;

  // Members
  PSUBDIR Sub;                  // To current Subdir block
  }; // end of class TDBSDR

/***********************************************************************/
/*  Class DIRCOL: DIR access method column descriptor.                 */
/*  This A.M. is used for tables populated by DIR file name list.      */
/***********************************************************************/
class DIRCOL : public COLBLK {
 public:
  // Constructors
  DIRCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "DIR");
  DIRCOL(DIRCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  int    GetAmType(void) override {return TYPE_AM_DIR;}

  // Methods
  void   ReadColumn(PGLOBAL g) override;

 protected:
  // Default constructor not to be used
  DIRCOL(void) = default;
#if defined(_WIN32)
	void SetTimeValue(PGLOBAL g, FILETIME& ftime);
#endif   // _WIN32

  // Members
	PTDBDIR	Tdbp;								// To DIR table
  int     N;                  // Column number
  }; // end of class DIRCOL
