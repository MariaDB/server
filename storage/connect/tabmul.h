/*************** Tabmul H Declares Source Code File (.H) ***************/
/*  Name: TABMUL.H   Version 1.4                                       */
/*                                                                     */
/*  (C) Copyright to PlugDB Software Development          2003-2012    */
/*  Author: Olivier BERTRAND                                           */
/*                                                                     */
/*  This file contains the TDBMUL and TDBDIR classes declares.         */
/***********************************************************************/
#if defined(WIN32)
#include <io.h>
#else   // !WIN32
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif  // !WIN32
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
  TDBMUL(PTDBASE tdbp);
  TDBMUL(PTDBMUL tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return Tdbp->GetAmType();}
  virtual PTDB Duplicate(PGLOBAL g);

  // Methods
  virtual void ResetDB(void);
  virtual PTDB CopyOne(PTABS t);
  virtual bool IsSame(PTDB tp) {return tp == (PTDB)Tdbp;}
  virtual PSZ  GetFile(PGLOBAL g) {return Tdbp->GetFile(g);}
  virtual int  GetRecpos(void) {return 0;}
  virtual PCOL ColDB(PGLOBAL g, PSZ name, int num);
          bool InitFileNames(PGLOBAL g);

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
                {strcpy(g->Message, MSG(MUL_MAKECOL_ERR)); return NULL;}
  virtual int  Cardinality(PGLOBAL g);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual int  GetProgMax(PGLOBAL g);
  virtual int  GetProgCur(void);
  virtual int  RowNumber(PGLOBAL g, bool b = false);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:

  // Members
  TDBASE *Tdbp;               // Points to a (file) table class
  char*  *Filenames;          // Points to file names
  int     Rows;               // Total rows of already read files
  int     Mul;                // Type of multiple file list
  int     NumFiles;           // Number of physical files
  int     iFile;              // Index of currently processed file
  }; // end of class TDBMUL

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
  virtual const char *GetType(void) {return "DIR";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
  PSZ     Fn;                 /* Path/Name of file search              */
  bool    Incl;               /* true to include sub-directories       */
  bool    Huge;               /* true if files can be larger than 2GB  */
  }; // end of DIRDEF

/***********************************************************************/
/*  This is the DIR Access Method class declaration for tables that    */
/*  represent a directory listing. The pathname is given at the create */
/*  time and can contain wildcard characters in the file name, and the */
/*  (virtual) table is populated when it is in use.                    */
/***********************************************************************/
class TDBDIR : public TDBASE {
  friend class DIRCOL;
 public:
  // Constructor
  TDBDIR(PDIRDEF tdp);
  TDBDIR(PTDBDIR tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_DIR;}
  virtual PTDB Duplicate(PGLOBAL g)
                {return (PTDB)new(g) TDBDIR(this);}

  // Methods
  virtual PTDB CopyOne(PTABS t);
  virtual int GetRecpos(void) {return iFile;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual int  GetProgMax(PGLOBAL g) {return GetMaxSize(g);}
  virtual int  GetProgCur(void) {return iFile;}
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:
  char *Path(PGLOBAL g);

  // Members
  PSZ  To_File;                 // Points to file search pathname
  int  iFile;                   // Index of currently retrieved file
#if defined(WIN32)
  _finddata_t    FileData;      // Find data structure
  int  Hsearch;                 // Search handle
  char Drive[_MAX_DRIVE];       // Drive name
#else   // !WIN32
  struct stat    Fileinfo;      // File info structure
  struct dirent *Entry;         // Point to directory entry structure
  DIR *Dir;                     // To searched directory structure
  bool Done;                    // true when _splipath is done
  char Pattern[_MAX_FNAME+_MAX_EXT];
#endif  // !WIN32
  char Fpath[_MAX_PATH];        // Absolute file search pattern
  char Direc[_MAX_DIR];         // Search path
  char Fname[_MAX_FNAME];       // File name
  char Ftype[_MAX_EXT];         // File extention
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
 public:
  // Constructors
  TDBSDR(PDIRDEF tdp) : TDBDIR(tdp) {Sub = NULL;}
  TDBSDR(PTDBSDR tdbp);

  // Implementation
  virtual PTDB Duplicate(PGLOBAL g)
                {return (PTDB)new(g) TDBSDR(this);}

  // Methods
  virtual PTDB CopyOne(PTABS t);

  // Database routines
  virtual int  GetMaxSize(PGLOBAL g);
  virtual int  GetProgMax(PGLOBAL g) {return GetMaxSize(g);}
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
//virtual void CloseDB(PGLOBAL g);

 protected:
          int  FindInDir(PGLOBAL g);

  typedef struct _Sub_Dir {
    struct _Sub_Dir *Next;
    struct _Sub_Dir *Prev;
#if defined(WIN32)
    int H;               // Search handle
#else   // !WIN32
    DIR *D;
#endif  // !WIN32
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
  DIRCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am = "DIR");
  DIRCOL(DIRCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  virtual int    GetAmType(void) {return TYPE_AM_DIR;}

  // Methods
  virtual void   ReadColumn(PGLOBAL g);

 protected:
  // Default constructor not to be used
  DIRCOL(void) {}

  // Members
  int     N;                  // Column number
  }; // end of class DIRCOL
