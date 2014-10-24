/************* TabMul C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABMUL                                                */
/* -------------                                                       */
/*  Version 1.7                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to PlugDB Software Development          2003 - 2012  */
/*  Author: Olivier BERTRAND                                           */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the TDBMUL class DB routines.                     */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*    TABMUL.CPP     - Source code                                     */
/*    PLGDBSEM.H     - DB application declaration file                 */
/*    TABDOS.H       - TABDOS classes declaration file                 */
/*    TABMUL.H       - TABFIX classes declaration file                 */
/*    GLOBAL.H       - Global declaration file                         */
/*                                                                     */
/*  REQUIRED LIBRARIES:                                                */
/*  -------------------                                                */
/*    Large model C library                                            */
/*                                                                     */
/*  REQUIRED PROGRAMS:                                                 */
/*  ------------------                                                 */
/*    IBM, Borland, GNU or Microsoft C++ Compiler and Linker           */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant section of system dependant header files.         */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
#include <stdlib.h>
#include <stdio.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#else
#if defined(UNIX)
#include <fnmatch.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "osutil.h"
#else
//#include <io.h>
#endif
//#include <fcntl.h>
#endif

/***********************************************************************/
/*  Include application header files:                                  */
/***********************************************************************/
#include "global.h"      // global declarations
#include "plgdbsem.h"    // DB application declarations
#include "reldef.h"      // DB definition declares
#include "filamtxt.h"
#include "tabdos.h"      // TDBDOS and DOSCOL class dcls
#include "tabmul.h"      // TDBMUL and MULCOL classes dcls

extern "C" int   trace;

/* ------------------------- Class TDBMUL ---------------------------- */

/***********************************************************************/
/*  TABMUL constructors.                                               */
/***********************************************************************/
TDBMUL::TDBMUL(PTDBASE tdbp) : TDBASE(tdbp->GetDef())
  {
  Tdbp = tdbp;
  Filenames = NULL;
  Rows = 0;
  Mul = tdbp->GetDef()->GetMultiple();
  NumFiles = 0;
  iFile = 0;
  } // end of TDBMUL standard constructor

TDBMUL::TDBMUL(PTDBMUL tdbp) : TDBASE(tdbp)
  {
  Tdbp = tdbp->Tdbp;
  Filenames = tdbp->Filenames;
  Rows = tdbp->Rows;
  Mul = tdbp->Mul;
  NumFiles = tdbp->NumFiles;
  iFile = tdbp->iFile;
  } // end of TDBMUL copy constructor

// Method
PTDB TDBMUL::CopyOne(PTABS t)
  {
  PTDBMUL tp;
  PGLOBAL g = t->G;        // Is this really useful ???

  tp = new(g) TDBMUL(this);
  tp->Tdbp = (PTDBASE)Tdbp->CopyOne(t);
  tp->Columns = tp->Tdbp->GetColumns();
  return tp;
  } // end of CopyOne

PTDB TDBMUL::Duplicate(PGLOBAL g)
  {
  PTDBMUL tmup = new(g) TDBMUL(this);

  tmup->Tdbp = (PTDBASE)Tdbp->Duplicate(g);
  return tmup;
  } // end of Duplicate

/***********************************************************************/
/*  Initializes the table filename list.                               */
/*  Note: tables created by concatenating the file components without  */
/*  specifying the LRECL value (that should be restricted to _MAX_PATH)*/
/*  have a LRECL that is the sum of the lengths of all components.     */
/*  This is why we use a big filename array to take care of that.      */
/***********************************************************************/
bool TDBMUL::InitFileNames(PGLOBAL g)
  {
#define PFNZ  4096
#define FNSZ  _MAX_DRIVE+_MAX_DIR+_MAX_FNAME+_MAX_EXT
  char *pfn[PFNZ];
  char *filename;
  int   rc, n = 0;

  if (trace)
    htrc("in InitFileName: fn[]=%d\n", FNSZ);

  filename = (char*)PlugSubAlloc(g, NULL, FNSZ);

  // The sub table may need to refer to the Table original block
  Tdbp->SetTable(To_Table);         // Was not set at construction

  PlugSetPath(filename, Tdbp->GetFile(g), Tdbp->GetPath());

  if (trace)
    htrc("InitFileName: fn='%s'\n", filename);

  if (Mul == 1) {
    /*******************************************************************/
    /*  To_File is a multiple name with special characters             */
    /*******************************************************************/
#if defined(WIN32)
    char   drive[_MAX_DRIVE], direc[_MAX_DIR];
    WIN32_FIND_DATA FileData;
    HANDLE hSearch;

    _splitpath(filename, drive, direc, NULL, NULL);

    // Start searching files in the target directory.
    hSearch = FindFirstFile(filename, &FileData);

    if (hSearch == INVALID_HANDLE_VALUE) {
      rc = GetLastError();

      if (rc != ERROR_FILE_NOT_FOUND) {
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL, GetLastError(), 0,
                      (LPTSTR)&filename, sizeof(filename), NULL);
        sprintf(g->Message, MSG(BAD_FILE_HANDLE), filename);
        return true;
        } // endif rc

      goto suite;
      } // endif hSearch

    while (n < PFNZ) {
      strcat(strcat(strcpy(filename, drive), direc), FileData.cFileName);
      pfn[n] = (char*)PlugSubAlloc(g, NULL, strlen(filename) + 1);
      strcpy(pfn[n++], filename);

      if (!FindNextFile(hSearch, &FileData)) {
        rc = GetLastError();

        if (rc != ERROR_NO_MORE_FILES) {
          sprintf(g->Message, MSG(NEXT_FILE_ERROR), rc);
          FindClose(hSearch);
          return true;
          } // endif rc

        break;
        } // endif FindNextFile

      } // endwhile n

    // Close the search handle.
    if (!FindClose(hSearch)) {
      strcpy(g->Message, MSG(SRCH_CLOSE_ERR));
      return true;
      } // endif FindClose

#else   // !WIN32
    struct stat fileinfo;
    char   fn[PATH_MAX], direc[PATH_MAX], pattern[256], ftype[8];
    DIR   *dir;
    struct dirent *entry;

    _splitpath(filename, NULL, direc, pattern, ftype);
    strcat(pattern, ftype);

    if (trace)
      htrc("direc=%s pattern=%s ftype=%s\n", direc, pattern, ftype);

    // Start searching files in the target directory.
    if (!(dir = opendir(direc))) {
      sprintf(g->Message, MSG(BAD_DIRECTORY), direc, strerror(errno));

      if (trace)
        htrc("%s\n", g->Message);

      return true;
      } // endif dir

    if (trace)
      htrc("dir opened: reading files\n");

    while ((entry = readdir(dir)) && n < PFNZ) {
      strcat(strcpy(fn, direc), entry->d_name);

      if (trace)
        htrc("%s read\n", fn);

      if (lstat(fn, &fileinfo) < 0) {
        sprintf(g->Message, "%s: %s", fn, strerror(errno));
        return true;
      } else if (!S_ISREG(fileinfo.st_mode))
        continue;      // Not a regular file (should test for links)

      /*******************************************************************/
      /*  Test whether the file name matches the table name filter.      */
      /*******************************************************************/
      if (fnmatch(pattern, entry->d_name, 0))
        continue;      // Not a match

      strcat(strcpy(filename, direc), entry->d_name);
      pfn[n] = (char*)PlugSubAlloc(g, NULL, strlen(filename) + 1);
      strcpy(pfn[n++], filename);

      if (trace)
        htrc("Adding pfn[%d] %s\n", n, filename);

      } // endwhile readdir

    // Close the dir handle.
    closedir(dir);
#endif  // !WIN32

  } else {
    /*******************************************************************/
    /*  To_File is the name of a file containing the file name list    */
    /*******************************************************************/
    char *p;
    FILE *stream;

    if (!(stream= global_fopen(g, MSGID_OPEN_MODE_STRERROR, filename, "r")))
      return true;

    while (n < PFNZ) {
      if (!fgets(filename, FNSZ, stream)) {
        fclose(stream);
        break;
        } // endif fgets

      p = filename + strlen(filename) - 1;

#if defined(UNIX)
      // Data files can be imported from Windows (having CRLF)
      if (*p == '\n' || *p == '\r') {
        // is this enough for Unix ???
        *p--;          // Eliminate ending CR or LF character

        if (p >= filename)
          // is this enough for Unix ???
          if (*p == '\n' || *p == '\r')
            *p--;    // Eliminate ending CR or LF character

        } // endif p

#else
      if (*p == '\n')
        p--;        // Eliminate ending new-line character
#endif
      // Trim rightmost blanks
      for (; p >= filename && *p == ' '; p--) ;

      *(++p) = '\0';

      // Suballocate the file name
      pfn[n] = (char*)PlugSubAlloc(g, NULL, strlen(filename) + 1);
      strcpy(pfn[n++], filename);
    } // endfor n

  } // endif Mul

#if defined(WIN32)
 suite:
#endif

  if (n) {
    Filenames = (char**)PlugSubAlloc(g, NULL, n * sizeof(char*));

    for (int i = 0; i < n; i++)
      Filenames[i] = pfn[i];

  } else {
    Filenames = (char**)PlugSubAlloc(g, NULL, sizeof(char*));
    Filenames[0] = NULL;
  } // endif n

  NumFiles = n;
  return false;
  } // end of InitFileNames

/***********************************************************************/
/*  The table column list is the sub-table column list.                */
/***********************************************************************/
PCOL TDBMUL::ColDB(PGLOBAL g, PSZ name, int num)
  {
  PCOL cp;

  /*********************************************************************/
  /*  Because special columns are directly added to the MUL block,     */
  /*  make sure that the sub-table has the same column list, before    */
  /*  and after the call to the ColDB function.                        */
  /*********************************************************************/
  Tdbp->SetColumns(Columns);
  cp = Tdbp->ColDB(g, name, num);
  Columns = Tdbp->GetColumns();
  return cp;
} // end of ColDB

/***********************************************************************/
/*  MUL GetProgMax: get the max value for progress information.        */
/***********************************************************************/
int TDBMUL::GetProgMax(PGLOBAL g)
  {
  if (!Filenames && InitFileNames(g))
    return -1;

  return NumFiles;                // This is a temporary setting
  } // end of GetProgMax

/***********************************************************************/
/*  MUL GetProgCur: get the current value for progress information.    */
/***********************************************************************/
int TDBMUL::GetProgCur(void)
  {
  return iFile;                   // This is a temporary setting
  } // end of GetProgMax

/***********************************************************************/
/*  MUL Cardinality: returns table cardinality in number of rows.      */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/*  Can be used on Multiple FIX table only.                            */
/***********************************************************************/
int TDBMUL::Cardinality(PGLOBAL g)
  {
  if (!g)
    return Tdbp->Cardinality(g);

  if (!Filenames && InitFileNames(g))
    return -1;

  int n, card = 0;

  for (int i = 0; i < NumFiles; i++) {
    Tdbp->SetFile(g, Filenames[i]);
    Tdbp->ResetSize();

    if ((n = Tdbp->Cardinality(g)) < 0) {
//    strcpy(g->Message, MSG(BAD_CARDINALITY));
      return -1;
      } // endif n

    card += n;
    } // endfor i

  return card;
  } // end of Cardinality

/***********************************************************************/
/*  Sum up the sizes of all sub-tables.                                */
/***********************************************************************/
int TDBMUL::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    int i;
    int mxsz;

    if (trace)
      htrc("TDBMUL::GetMaxSize: Filenames=%p\n", Filenames);
    
    if (!Filenames && InitFileNames(g))
      return -1;

    if (Use == USE_OPEN) {
      strcpy(g->Message, MSG(MAXSIZE_ERROR));
      return -1;
    } else
      MaxSize = 0;

    for (i = 0; i < NumFiles; i++) {
      Tdbp->SetFile(g, Filenames[i]);
      Tdbp->ResetSize();

      if ((mxsz = Tdbp->GetMaxSize(g)) < 0) {
        MaxSize = -1;
        return mxsz;
        } // endif mxsz

      MaxSize += mxsz;
      } // endfor i

    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  Reset read/write position values.                                  */
/***********************************************************************/
void TDBMUL::ResetDB(void)
  {
  for (PCOL colp = Columns; colp; colp = colp->GetNext())
    if (colp->GetAmType() == TYPE_AM_FILID)
      colp->COLBLK::Reset();

  Tdbp->ResetDB();
  } // end of ResetDB

/***********************************************************************/
/*  Returns RowId if b is false or Rownum if b is true.                */
/***********************************************************************/
int TDBMUL::RowNumber(PGLOBAL g, bool b)
  {
  return ((b) ? 0 : Rows)
       + ((iFile < NumFiles) ? Tdbp->RowNumber(g, b) : 1);
  } // end of RowNumber

/***********************************************************************/
/*  MUL Access Method opening routine.                                 */
/*  Open first file, other will be opened sequencially when reading.   */
/***********************************************************************/
bool TDBMUL::OpenDB(PGLOBAL g)
  {
  if (trace)
    htrc("MUL OpenDB: tdbp=%p tdb=R%d use=%d key=%p mode=%d\n",
      this, Tdb_No, Use, To_Key_Col, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, replace it at its beginning.               */
    /*******************************************************************/
    if (Filenames[iFile = 0]) {
      Tdbp->CloseDB(g);
      Tdbp->SetUse(USE_READY);
      Tdbp->SetFile(g, Filenames[iFile = 0]);
      Tdbp->ResetSize();
      Rows = 0;
      ResetDB();
      return Tdbp->OpenDB(g);  // Re-open with new file name
    } else
      return false;

    } // endif use

  /*********************************************************************/
  /*  We need to calculate MaxSize before opening the query.           */
  /*********************************************************************/
  if (GetMaxSize(g) < 0)
    return true;

  /*********************************************************************/
  /*  Open the first table file of the list.                           */
  /*********************************************************************/
//if (!Filenames && InitFileNames(g))     // was done in GetMaxSize
//  return true;

  if (Filenames[iFile = 0]) {
    Tdbp->SetFile(g, Filenames[0]);
    Tdbp->SetMode(Mode);
    Tdbp->ResetDB();
    Tdbp->ResetSize();

    if (Tdbp->OpenDB(g))
      return true;

    } // endif *Filenames

  Use = USE_OPEN;
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for MUL access method.              */
/***********************************************************************/
int TDBMUL::ReadDB(PGLOBAL g)
  {
  int rc;

  if (NumFiles == 0)
    return RC_EF;
  else if (To_Kindex) {
    /*******************************************************************/
    /*  Reading is by an index table.                                  */
    /*******************************************************************/
    strcpy(g->Message, MSG(NO_INDEX_READ));
    rc = RC_FX;
  } else {
    /*******************************************************************/
    /*  Now start the reading process.                                 */
    /*******************************************************************/
   retry:
    rc = Tdbp->ReadDB(g);

    if (rc == RC_EF) {
      if (Tdbp->GetDef()->GetPseudo() & 1)
        // Total number of rows met so far
        Rows += Tdbp->RowNumber(g) - 1;

      if (++iFile < NumFiles) {
        /***************************************************************/
        /*  Continue reading from next table file.                     */
        /***************************************************************/
        Tdbp->CloseDB(g);
        Tdbp->SetUse(USE_READY);
        Tdbp->SetFile(g, Filenames[iFile]);
        Tdbp->ResetSize();
        ResetDB();

        if (Tdbp->OpenDB(g))     // Re-open with new file name
          return RC_FX;

        goto retry;
        } // endif iFile

    } else if (rc == RC_FX)
      strcat(strcat(strcat(g->Message, " ("), Tdbp->GetFile(g)), ")");

  } // endif To_Kindex

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  Data Base write routine for MUL access method.                     */
/***********************************************************************/
int TDBMUL::WriteDB(PGLOBAL g)
  {
  return Tdbp->WriteDB(g);
//  strcpy(g->Message, MSG(TABMUL_READONLY));
//  return RC_FX;                    // NIY
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for MUL access method.               */
/***********************************************************************/
int TDBMUL::DeleteDB(PGLOBAL g, int irc)
  {
  // When implementing DELETE_MODE InitFileNames must be updated to
  // eliminate CRLF under Windows if the file is read in binary.
  strcpy(g->Message, MSG(TABMUL_READONLY));
  return RC_FX;                                      // NIY
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for MUL access method.                     */
/***********************************************************************/
void TDBMUL::CloseDB(PGLOBAL g)
  {
  if (NumFiles > 0) {
    Tdbp->CloseDB(g);
    iFile = NumFiles;
    } // endif NumFiles

  } // end of CloseDB

/* --------------------------- Class DIRDEF -------------------------- */

/***********************************************************************/
/*  DefineAM: define specific AM block values from XDB file.           */
/***********************************************************************/
bool DIRDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  Desc = Fn = GetStringCatInfo(g, "Filename", NULL);
  Incl = (GetIntCatInfo("Subdir", 0) != 0);
  Huge = (GetIntCatInfo("Huge", 0) != 0);
  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB DIRDEF::GetTable(PGLOBAL g, MODE m)
  {
#if 0
  if (Huge)
    return new(g) TDBDHR(this);        // Not implemented yet
  else
#endif
  if (Incl)
    return new(g) TDBSDR(this);        // Including sub-directory files
  else
    return new(g) TDBDIR(this);    // Not Including sub-directory files

  } // end of GetTable

/* ------------------------- Class TDBDIR ---------------------------- */

/***********************************************************************/
/*  TABDIR constructors.                                               */
/***********************************************************************/
TDBDIR::TDBDIR(PDIRDEF tdp) : TDBASE(tdp)
  {
  To_File = tdp->Fn;
  iFile = 0;
#if defined(WIN32)
  memset(&FileData, 0, sizeof(_finddata_t));
  Hsearch = -1;
  *Drive = '\0';
#else   // !WIN32
  memset(&Fileinfo, 0, sizeof(struct stat));
  Entry = NULL;
  Dir = NULL;
  Done = false;
  *Pattern = '\0';
#endif  // !WIN32
  *Fpath = '\0';
  *Direc = '\0';
  *Fname = '\0';
  *Ftype = '\0';
  } // end of TDBDIR standard constructor

TDBDIR::TDBDIR(PTDBDIR tdbp) : TDBASE(tdbp)
  {
  To_File = tdbp->To_File;
  iFile = tdbp->iFile;
#if defined(WIN32)
  FileData = tdbp->FileData;
  Hsearch = tdbp->Hsearch;
  strcpy(Drive, tdbp->Drive);
#else   // !WIN32
  Fileinfo = tdbp->Fileinfo;
  Entry = tdbp->Entry;
  Dir = tdbp->Dir;
  Done = tdbp->Done;
  strcpy(Pattern, tdbp->Pattern);
#endif  // !WIN32
  strcpy(Direc, tdbp->Direc);
  strcpy(Fname, tdbp->Fname);
  strcpy(Ftype, tdbp->Ftype);
  } // end of TDBDIR copy constructor

// Method
PTDB TDBDIR::CopyOne(PTABS t)
  {
  PTDB    tp;
  PGLOBAL g = t->G;        // Is this really useful ???

  tp = new(g) TDBDIR(this);
  tp->SetColumns(Columns);
  return tp;
  } // end of CopyOne

/***********************************************************************/
/*  Initialize/get the components of the search file pattern.          */
/***********************************************************************/
char* TDBDIR::Path(PGLOBAL g)
  {
  PCATLG cat = PlgGetCatalog(g);

#if defined(WIN32)
  if (!*Drive) {
    PlugSetPath(Fpath, To_File, ((PTABDEF)To_Def)->GetPath());
    _splitpath(Fpath, Drive, Direc, Fname, Ftype);
  } else
    _makepath(Fpath, Drive, Direc, Fname, Ftype);   // Usefull ???

  return Fpath;
#else   // !WIN32
  if (!Done) {
    PlugSetPath(Fpath, To_File, ((PTABDEF)To_Def)->GetPath());
    _splitpath(Fpath, NULL, Direc, Fname, Ftype);
    strcat(strcpy(Pattern, Fname), Ftype);
    Done = true;
    } // endif Done

  return Pattern;
#endif  // !WIN32
  } // end of Path

/***********************************************************************/
/*  Allocate DIR column description block.                             */
/***********************************************************************/
PCOL TDBDIR::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) DIRCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  DIR GetMaxSize: returns the number of retrieved files.             */
/***********************************************************************/
int TDBDIR::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    int    n = -1;
#if defined(WIN32)
    int    h;

    // Start searching files in the target directory.
    h = _findfirst(Path(g), &FileData);

    if (h != -1) {
      for (n = 1;; n++)
        if (_findnext(h, &FileData))
          break;

      // Close the search handle.
      _findclose(h);
    } else
      n = 0;

#else   // !WIN32
    Path(g);

    // Start searching files in the target directory.
    if (!(Dir = opendir(Direc))) {
      sprintf(g->Message, MSG(BAD_DIRECTORY), Direc, strerror(errno));
      return -1;
      } // endif dir

    while ((Entry = readdir(Dir))) {
      strcat(strcpy(Fpath, Direc), Entry->d_name);

      if (lstat(Fpath, &Fileinfo) < 0) {
        sprintf(g->Message, "%s: %s", Fpath, strerror(errno));
        return -1;
      } else if (S_ISREG(Fileinfo.st_mode))
        // Test whether the file name matches the table name filter
        if (!fnmatch(Pattern, Entry->d_name, 0))
          n++;      // We have a match

      } // endwhile Entry

    // Close the DIR handle.
    closedir(Dir);
#endif  // !WIN32
    MaxSize = n;
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  DIR Access Method opening routine.                                 */
/*  Open first file, other will be opened sequencially when reading.   */
/***********************************************************************/
bool TDBDIR::OpenDB(PGLOBAL g)
  {
  if (trace)
    htrc("DIR OpenDB: tdbp=%p tdb=R%d use=%d mode=%d\n",
      this, Tdb_No, Use, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, reopen it.                                 */
    /*******************************************************************/
    CloseDB(g);
    SetUse(USE_READY);
    } // endif use

  Use = USE_OPEN;
#if !defined(WIN32)
  Path(g);                          // Be sure it is done
  Dir = NULL;                       // For ReadDB
#endif   // !WIN32
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for DIR access method.                      */
/***********************************************************************/
int TDBDIR::ReadDB(PGLOBAL g)
  {
  int rc = RC_OK;

#if defined(WIN32)
  if (Hsearch == -1) {
    /*******************************************************************/
    /*  Start searching files in the target directory. The use of the  */
    /*  Path function is required when called from TDBSDR.             */
    /*******************************************************************/
    Hsearch = _findfirst(Path(g), &FileData);

    if (Hsearch == -1)
      rc = RC_EF;
    else
      iFile++;

  } else {
    if (_findnext(Hsearch, &FileData)) {
      // Restore file name and type pattern
      _splitpath(To_File, NULL, NULL, Fname, Ftype);
      rc = RC_EF;
    } else
      iFile++;

  } // endif Hsearch

  if (rc == RC_OK)
    _splitpath(FileData.name, NULL, NULL, Fname, Ftype);

#else   // !Win32
  rc = RC_NF;

  if (!Dir)
    // Start searching files in the target directory.
    if (!(Dir = opendir(Direc))) {
      sprintf(g->Message, MSG(BAD_DIRECTORY), Direc, strerror(errno));
      rc = RC_FX;
      } // endif dir

  while (rc == RC_NF)
    if ((Entry = readdir(Dir))) {
      // We need the Fileinfo structure to get info about the file
      strcat(strcpy(Fpath, Direc), Entry->d_name);

      if (lstat(Fpath, &Fileinfo) < 0) {
        sprintf(g->Message, "%s: %s", Fpath, strerror(errno));
        rc = RC_FX;
      } else if (S_ISREG(Fileinfo.st_mode))
        // Test whether the file name matches the table name filter
        if (!fnmatch(Pattern, Entry->d_name, 0)) {
          iFile++;      // We have a match
          _splitpath(Entry->d_name, NULL, NULL, Fname, Ftype);
          rc = RC_OK;
          } // endif fnmatch

    } else {
      // Restore file name and type pattern
      _splitpath(To_File, NULL, NULL, Fname, Ftype);
      rc = RC_EF;
    } // endif Entry

#endif  // !WIN32

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  Data Base write routine for DIR access method.                     */
/***********************************************************************/
int TDBDIR::WriteDB(PGLOBAL g)
  {
  strcpy(g->Message, MSG(TABDIR_READONLY));
  return RC_FX;                    // NIY
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for DIR access method.               */
/***********************************************************************/
int TDBDIR::DeleteDB(PGLOBAL g, int irc)
  {
  strcpy(g->Message, MSG(TABDIR_READONLY));
  return RC_FX;                                      // NIY
  } // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for MUL access method.                     */
/***********************************************************************/
void TDBDIR::CloseDB(PGLOBAL g)
  {
#if defined(WIN32)
  // Close the search handle.
  _findclose(Hsearch);
  Hsearch = -1;
#else   // !WIN32
  // Close the DIR handle
  if (Dir) {
    closedir(Dir);
    Dir = NULL;
    } // endif dir
#endif  // !WIN32
  iFile = 0;
  } // end of CloseDB

// ------------------------ DIRCOL functions ----------------------------

/***********************************************************************/
/*  DIRCOL public constructor.                                         */
/***********************************************************************/
DIRCOL::DIRCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
  : COLBLK(cdp, tdbp, i)
  {
  if (cprec) {
    Next = cprec->GetNext();
    cprec->SetNext(this);
  } else {
    Next = tdbp->GetColumns();
    tdbp->SetColumns(this);
  } // endif cprec

  // Set additional DIR access method information for column.
  N = cdp->GetOffset();
  } // end of DIRCOL constructor

/***********************************************************************/
/*  DIRCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
DIRCOL::DIRCOL(DIRCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  N = col1->N;
  } // end of DIRCOL copy constructor

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the information    */
/*  corresponding to this column and convert it to buffer type.        */
/***********************************************************************/
void DIRCOL::ReadColumn(PGLOBAL g)
  {
  PTDBDIR tdbp = (PTDBDIR)To_Tdb;

  if (trace)
    htrc("DIR ReadColumn: col %s R%d use=%.4X status=%.4X type=%d N=%d\n",
      Name, tdbp->GetTdb_No(), ColUse, Status, Buf_Type, N);

  /*********************************************************************/
  /*  Retrieve the information corresponding to the column number.     */
  /*********************************************************************/
  switch (N) {
#if defined(WIN32)
    case  0: Value->SetValue_psz(tdbp->Drive);                  break;
#endif   // WIN32
    case  1: Value->SetValue_psz(tdbp->Direc);                  break;
    case  2: Value->SetValue_psz(tdbp->Fname);                  break;
    case  3: Value->SetValue_psz(tdbp->Ftype);                  break;
#if defined(WIN32)
    case  4: Value->SetValue((int)tdbp->FileData.attrib);      break;
    case  5: Value->SetValue((int)tdbp->FileData.size);        break;
    case  6: Value->SetValue((int)tdbp->FileData.time_write);  break;
    case  7: Value->SetValue((int)tdbp->FileData.time_create); break;
    case  8: Value->SetValue((int)tdbp->FileData.time_access); break;
#else   // !WIN32
    case  4: Value->SetValue((int)tdbp->Fileinfo.st_mode);     break;
    case  5: Value->SetValue((int)tdbp->Fileinfo.st_size);     break;
    case  6: Value->SetValue((int)tdbp->Fileinfo.st_mtime);    break;
    case  7: Value->SetValue((int)tdbp->Fileinfo.st_ctime);    break;
    case  8: Value->SetValue((int)tdbp->Fileinfo.st_atime);    break;
    case  9: Value->SetValue((int)tdbp->Fileinfo.st_uid);      break;
    case 10: Value->SetValue((int)tdbp->Fileinfo.st_gid);      break;
#endif  // !WIN32
    default:
      sprintf(g->Message, MSG(INV_DIRCOL_OFST), N);
      longjmp(g->jumper[g->jump_level], GetAmType());
    } // endswitch N

  } // end of ReadColumn

/* ------------------------- Class TDBSDR ---------------------------- */

/***********************************************************************/
/*  TABSDR copy constructors.                                          */
/***********************************************************************/
TDBSDR::TDBSDR(PTDBSDR tdbp) : TDBDIR(tdbp)
  {
  Sub = tdbp->Sub;
  } // end of TDBSDR copy constructor

// Method
PTDB TDBSDR::CopyOne(PTABS t)
  {
  PTDB    tp;
  PGLOBAL g = t->G;        // Is this really useful ???

  tp = new(g) TDBSDR(this);
  tp->SetColumns(Columns);
  return tp;
  } // end of CopyOne

/***********************************************************************/
/*  SDR GetMaxSize: returns the number of retrieved files.             */
/***********************************************************************/
int TDBSDR::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    Path(g);
    MaxSize = FindInDir(g);
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  SDR GetMaxSize: returns the number of retrieved files.             */
/***********************************************************************/
int TDBSDR::FindInDir(PGLOBAL g)
  {
  int   n = 0;
  size_t m = strlen(Direc);

  // Start searching files in the target directory.
#if defined(WIN32)
  int h = _findfirst(Path(g), &FileData);

  if (h != -1) {
    for (n = 1;; n++)
      if (_findnext(h, &FileData))
        break;

    // Close the search handle.
    _findclose(h);
    } // endif h

  // Now search files in sub-directories.
  _makepath(Fpath, Drive, Direc, "*", "");
  h = _findfirst(Fpath, &FileData);

  if (h != -1) {
    while (true) {
      if (FileData.attrib & _A_SUBDIR && *FileData.name != '.') {
        // Look in the name sub-directory
        strcat(strcat(Direc, FileData.name), "\\");
        n += FindInDir(g);
        Direc[m] = '\0';         // Restore path
        } // endif SUBDIR

      if (_findnext(h, &FileData))
        break;

      } // endwhile

    // Close the search handle.
    _findclose(h);
    } // endif h
#else   // !WIN32
  int k;
  DIR *dir = opendir(Direc);

  if (!dir) {
    sprintf(g->Message, MSG(BAD_DIRECTORY), Direc, strerror(errno));
    return -1;
    } // endif dir

  while ((Entry = readdir(dir))) {
    strcat(strcpy(Fpath, Direc), Entry->d_name);

    if (lstat(Fpath, &Fileinfo) < 0) {
      sprintf(g->Message, "%s: %s", Fpath, strerror(errno));
      return -1;
    } else if (S_ISDIR(Fileinfo.st_mode) && *Entry->d_name != '.') {
      // Look in the name sub-directory
      strcat(strcat(Direc, Entry->d_name), "/");

      if ((k = FindInDir(g)) < 0)
        return k;
      else
        n += k;

      Direc[m] = '\0';         // Restore path
    } else if (S_ISREG(Fileinfo.st_mode))
      // Test whether the file name matches the table name filter
      if (!fnmatch(Pattern, Entry->d_name, 0))
        n++;      // We have a match

    } // endwhile readdir

  // Close the DIR handle.
  closedir(dir);
#endif  // !WIN32

  return n;
  } // end of FindInDir

/***********************************************************************/
/*  DIR Access Method opening routine.                                 */
/*  Open first file, other will be opened sequencially when reading.   */
/***********************************************************************/
bool TDBSDR::OpenDB(PGLOBAL g)
  {
  if (!Sub) {
    Path(g);
    Sub = (PSUBDIR)PlugSubAlloc(g, NULL, sizeof(SUBDIR));
    Sub->Next = NULL;
    Sub->Prev = NULL;
#if defined(WIN32)
    Sub->H = -1;
    Sub->Len = strlen(Direc);
#else   // !WIN32
    Sub->D = NULL;
    Sub->Len = 0;
#endif  // !WIN32
    } // endif To_Sub

  return TDBDIR::OpenDB(g);
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for SDR access method.                      */
/***********************************************************************/
int TDBSDR::ReadDB(PGLOBAL g)
  {
  int rc;

#if defined(WIN32)
 again:
  rc = TDBDIR::ReadDB(g);

  if (rc == RC_EF) {
    // Are there more files in sub-directories
   retry:
    do {
      if (Sub->H == -1) {
        _makepath(Fpath, Drive, Direc, "*", "");
        Sub->H = _findfirst(Fpath, &FileData);
      } else if (_findnext(Sub->H, &FileData)) {
        _findclose(Sub->H);
        Sub->H = -1;
        *FileData.name = '\0';
        } // endif findnext

      } while(*FileData.name == '.');

    if (Sub->H == -1) {
      // No more sub-directories. Are we in a sub-directory?
      if (!Sub->Prev)
        return rc;               // No, all is finished

      // here we must continue in the parent directory
      Sub = Sub->Prev;
      goto retry;
    } else {
      // Search next sub-directory
      Direc[Sub->Len] = '\0';

      if (!Sub->Next) {
        PSUBDIR sup;

        sup = (PSUBDIR)PlugSubAlloc(g, NULL, sizeof(SUBDIR));
        sup->Next = NULL;
        sup->Prev = Sub;
        sup->H = -1;
        Sub->Next = sup;
        } // endif Next

      Sub = Sub->Next;
      strcat(strcat(Direc, FileData.name), "\\");
      Sub->Len = strlen(Direc);

      // Reset Hsearch used by TDBDIR::ReadDB
      _findclose(Hsearch);
      Hsearch = -1;
      goto again;
    } // endif H

    } // endif rc
#else   // !WIN32
  rc = RC_NF;

 again:
  if (!Sub->D)
    // Start searching files in the target directory.
    if (!(Sub->D = opendir(Direc))) {
      sprintf(g->Message, MSG(BAD_DIRECTORY), Direc, strerror(errno));
      rc = RC_FX;
      } // endif dir

  while (rc == RC_NF)
    if ((Entry = readdir(Sub->D))) {
      // We need the Fileinfo structure to get info about the file
      strcat(strcpy(Fpath, Direc), Entry->d_name);

      if (lstat(Fpath, &Fileinfo) < 0) {
        sprintf(g->Message, "%s: %s", Fpath, strerror(errno));
        rc = RC_FX;
      } else if (S_ISDIR(Fileinfo.st_mode) && *Entry->d_name != '.') {
        // Look in the name sub-directory
        if (!Sub->Next) {
          PSUBDIR sup;

          sup = (PSUBDIR)PlugSubAlloc(g, NULL, sizeof(SUBDIR));
          sup->Next = NULL;
          sup->Prev = Sub;
          Sub->Next = sup;
          } // endif Next

        Sub = Sub->Next;
        Sub->D = NULL;
        Sub->Len = strlen(Direc);
        strcat(strcat(Direc, Entry->d_name), "/");
        goto again;
      } else if (S_ISREG(Fileinfo.st_mode))
        // Test whether the file name matches the table name filter
        if (!fnmatch(Pattern, Entry->d_name, 0)) {
          iFile++;      // We have a match
          _splitpath(Entry->d_name, NULL, NULL, Fname, Ftype);
          rc = RC_OK;
          } // endif fnmatch

    } else {
      // No more files. Close the DIR handle.
      closedir(Sub->D);

      // Are we in a sub-directory?
      if (Sub->Prev) {
        // Yes, we must continue in the parent directory
        Direc[Sub->Len] = '\0';
        Sub = Sub->Prev;
      } else
        rc = RC_EF;              // No, all is finished

    } // endif Entry

#endif  // !WIN32

  return rc;
  } // end of ReadDB

#if 0
/* ------------------------- Class TDBDHR ---------------------------- */

/***********************************************************************/
/*  TABDHR constructors.                                               */
/***********************************************************************/
TDBDHR::TDBDHR(PDHRDEF tdp) : TDBASE(tdp)
  {
  memset(&FileData, 0, sizeof(WIN32_FIND_DATA));
  Hsearch = INVALID_HANDLE_VALUE;
  iFile = 0;
  *Drive = '\0';
  *Direc = '\0';
  *Fname = '\0';
  *Ftype = '\0';
  } // end of TDBDHR standard constructor

TDBDHR::TDBDHR(PTDBDHR tdbp) : TDBASE(tdbp)
  {
  FileData = tdbp->FileData;
  Hsearch = tdbp->Hsearch;
  iFile = tdbp->iFile;
  strcpy(Drive, tdbp->Drive);
  strcpy(Direc, tdbp->Direc);
  strcpy(Fname, tdbp->Fname);
  strcpy(Ftype, tdbp->Ftype);
  } // end of TDBDHR copy constructor

// Method
PTDB TDBDHR::CopyOne(PTABS t)
  {
  PTDB    tp;
  PGLOBAL g = t->G;        // Is this really useful ???

  tp = new(g) TDBDHR(this);
  tp->Columns = Columns;
  return tp;
  } // end of CopyOne

/***********************************************************************/
/*  Allocate DHR column description block.                             */
/***********************************************************************/
PCOL TDBDHR::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  return new(g) DHRCOL(cdp, this, cprec, n);
  } // end of MakeCol

/***********************************************************************/
/*  DHR GetMaxSize: returns the number of retrieved files.             */
/***********************************************************************/
int TDBDHR::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    char    filename[_MAX_PATH];
    int     i, rc;
    int    n = -1;
    HANDLE  h;
    PDBUSER dup = PlgGetUser(g);

    PlugSetPath(filename, To_File, dup->Path);

    // Start searching files in the target directory.
    h = FindFirstFile(filename, &FileData);

    if (h == INVALID_HANDLE_VALUE) {
      switch (rc = GetLastError()) {
        case ERROR_NO_MORE_FILES:
        case ERROR_FILE_NOT_FOUND:
          n = 0;
          break;
        default:
          FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL, rc, 0,
                        (LPTSTR)&filename, sizeof(filename), NULL);
          sprintf(g->Message, MSG(BAD_FILE_HANDLE), filename);
        } // endswitch rc

    } else {
      for (n = 1;; n++)
        if (!FindNextFile(h, &FileData)) {
          rc = GetLastError();

          if (rc != ERROR_NO_MORE_FILES) {
            sprintf(g->Message, MSG(NEXT_FILE_ERROR), rc);
            n = -1;
            } // endif rc

          break;
          } // endif FindNextFile

      // Close the search handle.
      if (!FindClose(h) && n != -1)
        strcpy(g->Message, MSG(SRCH_CLOSE_ERR));

    } // endif Hsearch

    MaxSize = n;
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  DHR Access Method opening routine.                                 */
/*  Open first file, other will be opened sequencially when reading.   */
/***********************************************************************/
bool TDBDHR::OpenDB(PGLOBAL g)
  {
  if (trace)
    htrc("DHR OpenDB: tdbp=%p tdb=R%d use=%d mode=%d\n",
      this, Tdb_No, Use, Mode);

  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, reopen it.                                 */
    /*******************************************************************/
    CloseDB(g);
    SetUse(USE_READY);
    } // endif use

  /*********************************************************************/
  /*  Direct access needed for join or sorting.                        */
  /*********************************************************************/
  if (NeedIndexing(g)) {
    // Direct access of DHR tables is not implemented yet
    sprintf(g->Message, MSG(NO_DIR_INDX_RD), "DHR");
    return true;
    } // endif NeedIndexing

  Use = USE_OPEN;
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for DHR access method.                      */
/***********************************************************************/
int TDBDHR::ReadDB(PGLOBAL g)
  {
  int   rc = RC_OK;
  DWORD erc;

  if (Hsearch == INVALID_HANDLE_VALUE) {
    char   *filename[_MAX_PATH];
    PDBUSER dup = PlgGetUser(g);

    PlugSetPath(filename, To_File, dup->Path);
    _splitpath(filename, Drive, Direc, NULL, NULL);

    /*******************************************************************/
    /*  Start searching files in the target directory.                 */
    /*******************************************************************/
    Hsearch = FindFirstFile(filename, &FileData);

    if (Hsearch != INVALID_HANDLE_VALUE)
      iFile = 1;
    else switch (erc = GetLastError()) {
      case ERROR_NO_MORE_FILES:
      case ERROR_FILE_NOT_FOUND:
//    case ERROR_PATH_NOT_FOUND:               ???????
        rc = RC_EF;
        break;
      default:
        FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL, erc,  0,
                      (LPTSTR)&filename, sizeof(filename), NULL);
        sprintf(g->Message, MSG(BAD_FILE_HANDLE), filename);
        rc = RC_FX;
      } // endswitch erc

  } else {
    if (!FindNextFile(Hsearch, &FileData)) {
      DWORD erc = GetLastError();

      if (erc != ERROR_NO_MORE_FILES) {
        sprintf(g->Message, MSG(NEXT_FILE_ERROR), erc);
        FindClose(Hsearch);
        rc = RC_FX;
      } else
        rc = RC_EF;

    } else
      iFile++;

  } // endif Hsearch

  if (rc == RC_OK)
    _splitpath(FileData.cFileName, NULL, NULL, Fname, Ftype);

  return rc;
  } // end of ReadDB

/***********************************************************************/
/*  Data Base close routine for MUL access method.                     */
/***********************************************************************/
void TDBDHR::CloseDB(PGLOBAL g)
  {
  // Close the search handle.
  if (!FindClose(Hsearch)) {
    strcpy(g->Message, MSG(SRCH_CLOSE_ERR));
    longjmp(g->jumper[g->jump_level], GetAmType());
    } // endif FindClose

  iFile = 0;
  Hsearch = INVALID_HANDLE_VALUE;
  } // end of CloseDB

// ------------------------ DHRCOL functions ----------------------------

/***********************************************************************/
/*  DHRCOL public constructor.                                         */
/***********************************************************************/
DHRCOL::DHRCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PSZ am)
  : COLBLK(cdp, tdbp, i)
  {
  if (cprec) {
    Next = cprec->GetNext();
    cprec->SetNext(this);
  } else {
    Next = tdbp->GetColumns();
    tdbp->SetColumns(this);
  } // endif cprec

  // Set additional DHR access method information for column.
  N = cdp->GetOffset();
  } // end of DOSCOL constructor

/***********************************************************************/
/*  DHRCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
DHRCOL::DHRCOL(DHRCOL *col1, PTDB tdbp) : COLBLK(col1, tdbp)
  {
  N = col1->N;
  } // end of DHRCOL copy constructor

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the information    */
/*  corresponding to this column and convert it to buffer type.        */
/***********************************************************************/
void DHRCOL::ReadColumn(PGLOBAL g)
  {
  int     rc;
  PTDBDHR tdbp = (PTDBDHR)To_Tdb;

  if (trace)
    htrc("DHR ReadColumn: col %s R%d use=%.4X status=%.4X type=%d N=%d\n",
      Name, tdbp->GetTdb_No(), ColUse, Status, Buf_Type, N);

  /*********************************************************************/
  /*  Retrieve the information corresponding to the column number.     */
  /*********************************************************************/
  switch (N) {
    case 0:                                // Drive
      Value->SetValue(Drive, _MAX_DRIVE);
      break;
    case 1:                                // Path
      Value->SetValue(Direc, _MAX_DHR);
      break;
    case 2:                                // Name
      Value->SetValue(Fname, _MAX_FNAME);
      break;
    case 3:                                // Extention
      Value->SetValue(Ftype, _MAX_EXT);
      break;
    case 4:                                // Extention
      Value->SetValue(tdbp->FileData.cAlternateFileName, 14);
      break;
    case 5:
      Value->SetValue(tdbp->FileData.dwFileAttributes);
      break;
    case 6:
      Value->SetValue(..................
  } // end of ReadColumn
#endif // 0

