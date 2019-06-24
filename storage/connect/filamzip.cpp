/*********** File AM Zip C++ Program Source Code File (.CPP) ***********/
/* PROGRAM NAME: FILAMZIP                                              */
/* -------------                                                       */
/*  Version 1.3                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2016-2017    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the ZIP file access method classes.               */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the System header files.              */
/***********************************************************************/
#include "my_global.h"
#if !defined(__WIN__)
#if defined(UNIX)
#include <fnmatch.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#else    // !UNIX
#include <io.h>
#endif  // !UNIX
#include <fcntl.h>
#endif  // !__WIN__
#include <time.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "osutil.h"
#include "filamtxt.h"
#include "tabfmt.h"
//#include "tabzip.h"
#include "filamzip.h"

#define WRITEBUFFERSIZE (16384)

bool ZipLoadFile(PGLOBAL g, PCSZ zfn, PCSZ fn, PCSZ entry, bool append, bool mul);

/***********************************************************************/
/*  Compress a file in zip when creating a table.                      */
/***********************************************************************/
static bool ZipFile(PGLOBAL g, ZIPUTIL *zutp, PCSZ fn, PCSZ entry, char *buf)
{
	int   rc = RC_OK, size_read, size_buf = WRITEBUFFERSIZE;
	FILE *fin;

	if (zutp->addEntry(g, entry))
		return true;
	else if (!(fin = fopen(fn, "rb"))) {
		sprintf(g->Message, "error in opening %s for reading", fn);
		return true;
	} // endif fin

	do {
		size_read = (int)fread(buf, 1, size_buf, fin);

		if (size_read < size_buf && feof(fin) == 0) {
			sprintf(g->Message, "error in reading %s", fn);
			rc = RC_FX;
		}	// endif size_read

		if (size_read > 0) {
			rc = zutp->writeEntry(g, buf, size_read);

			if (rc == RC_FX)
				sprintf(g->Message, "error in writing %s in the zipfile", fn);

		}	// endif size_read

	} while (rc == RC_OK && size_read > 0);

	fclose(fin);
	zutp->closeEntry();
	return rc != RC_OK;
}	// end of ZipFile

/***********************************************************************/
/*  Find and Compress several files in zip when creating a table.      */
/***********************************************************************/
static bool ZipFiles(PGLOBAL g, ZIPUTIL *zutp, PCSZ pat, char *buf)
{
	char filename[_MAX_PATH];
	/*********************************************************************/
	/*  pat is a multiple file name with wildcard characters             */
	/*********************************************************************/
	strcpy(filename, pat);

#if defined(__WIN__)
	char   drive[_MAX_DRIVE], direc[_MAX_DIR];
	int  rc;
	WIN32_FIND_DATA FileData;
	HANDLE hSearch;

	_splitpath(filename, drive, direc, NULL, NULL);

	// Start searching files in the target directory.
	hSearch = FindFirstFile(filename, &FileData);

	if (hSearch == INVALID_HANDLE_VALUE) {
		rc = GetLastError();

		if (rc != ERROR_FILE_NOT_FOUND) {
			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, GetLastError(), 0, (LPTSTR)&filename, sizeof(filename), NULL);
			sprintf(g->Message, MSG(BAD_FILE_HANDLE), filename);
			return true;
		} else {
			strcpy(g->Message, "Cannot find any file to load");
			return true;
		}	// endif rc

	} // endif hSearch

	while (true) {
		if (!(FileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			strcat(strcat(strcpy(filename, drive), direc), FileData.cFileName);

			if (ZipFile(g, zutp, filename, FileData.cFileName, buf)) {
				FindClose(hSearch);
				return true;
			} // endif ZipFile

		} // endif dwFileAttributes

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

#else   // !__WIN__
	struct stat fileinfo;
	char   fn[FN_REFLEN], direc[FN_REFLEN], pattern[FN_HEADLEN], ftype[FN_EXTLEN];
	DIR   *dir;
	struct dirent *entry;

	_splitpath(filename, NULL, direc, pattern, ftype);
	strcat(pattern, ftype);

	// Start searching files in the target directory.
	if (!(dir = opendir(direc))) {
		sprintf(g->Message, MSG(BAD_DIRECTORY), direc, strerror(errno));
		return true;
	} // endif dir

	while ((entry = readdir(dir))) {
		strcat(strcpy(fn, direc), entry->d_name);

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

		if (ZipFile(g, zutp, filename, entry->d_name, buf)) {
			closedir(dir);
			return true;
		} // endif ZipFile

	} // endwhile readdir

	// Close the dir handle.
	closedir(dir);
#endif  // !__WIN__

	return false;
}	// end of ZipFiles

/***********************************************************************/
/*  Load and Compress a file in zip when creating a table.             */
/***********************************************************************/
bool ZipLoadFile(PGLOBAL g, PCSZ zfn, PCSZ fn, PCSZ entry, bool append, bool mul)
{
	char    *buf;
	bool     err;
	ZIPUTIL *zutp = new(g) ZIPUTIL(NULL);

	if (zutp->open(g, zfn, append))
		return true;

	buf = (char*)PlugSubAlloc(g, NULL, WRITEBUFFERSIZE);

	if (mul)
		err = ZipFiles(g, zutp, fn, buf);
	else
	  err = ZipFile(g, zutp, fn, entry, buf);

	zutp->close();
	return err;
}	// end of ZipLoadFile

/* -------------------------- class ZIPUTIL -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
ZIPUTIL::ZIPUTIL(PCSZ tgt)
{
	zipfile = NULL;
	target = tgt;
	fp = NULL;
	entryopen = false;
} // end of ZIPUTIL standard constructor

#if 0
ZIPUTIL::ZIPUTIL(ZIPUTIL *zutp)
{
	zipfile = zutp->zipfile;
	target = zutp->target;
	fp = zutp->fp;
	entryopen = zutp->entryopen;
} // end of UNZIPUTL copy constructor
#endif // 0

/***********************************************************************/
/*  Fill the zip time structure																				 */
/*  param: tmZip	time structure to be filled													 */
/***********************************************************************/
void ZIPUTIL::getTime(tm_zip& tmZip)
{
	time_t rawtime;
	time(&rawtime);
	struct tm *timeinfo = localtime(&rawtime);
	tmZip.tm_sec = timeinfo->tm_sec;
	tmZip.tm_min = timeinfo->tm_min;
	tmZip.tm_hour = timeinfo->tm_hour;
	tmZip.tm_mday = timeinfo->tm_mday;
	tmZip.tm_mon = timeinfo->tm_mon;
	tmZip.tm_year = timeinfo->tm_year;
}	// end of getTime

/***********************************************************************/
/*  open a zip file for deflate.																			 */
/*  param: filename	path and the filename of the zip file to open.		 */
/*	append:		set true to append the zip file													 */
/*  return:	true if open, false otherwise.														 */
/***********************************************************************/
bool ZIPUTIL::open(PGLOBAL g, PCSZ filename, bool append)
{
	if (!zipfile && !(zipfile = zipOpen64(filename,
		                               append ? APPEND_STATUS_ADDINZIP
																	        : APPEND_STATUS_CREATE)))
		sprintf(g->Message, "Zipfile open error on %s", filename);

	return (zipfile == NULL);
}	// end of open

/***********************************************************************/
/*  Close the zip file.																								 */
/***********************************************************************/
void ZIPUTIL::close()
{
	if (zipfile) {
		closeEntry();
		zipClose(zipfile, 0);
		zipfile = NULL;
	}	// endif zipfile

	if (fp)
		fp->Count = 0;

}	// end of close

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file from a ZIP file.         */
/***********************************************************************/
bool ZIPUTIL::OpenTable(PGLOBAL g, MODE mode, PCSZ fn, bool append)
{
	/*********************************************************************/
	/*  The file will be compressed.                                     */
	/*********************************************************************/
	if (mode == MODE_INSERT) {
		bool b = open(g, fn, append);

		if (!b) {
			if (addEntry(g, target))
				return true;

			/*****************************************************************/
			/*  Link a Fblock. This make possible to automatically close it  */
			/*  in case of error g->jump.                                    */
			/*****************************************************************/
			PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

			fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
			fp->Type = TYPE_FB_ZIP;
			fp->Fname = PlugDup(g, fn);
			fp->Next = dbuserp->Openlist;
			dbuserp->Openlist = fp;
			fp->Count = 1;
			fp->Length = 0;
			fp->Memory = NULL;
			fp->Mode = mode;
			fp->File = this;
			fp->Handle = 0;
		} else
			return true;

	} else {
		strcpy(g->Message, "Only INSERT mode supported for ZIPPING files");
		return true;
	}	// endif mode

	return false;
} // end of OpenTableFile

/***********************************************************************/
/*  Add target in zip file.				   		      												 */
/***********************************************************************/
bool ZIPUTIL::addEntry(PGLOBAL g, PCSZ entry)
{
	//?? we dont need the stinking time
	zip_fileinfo zi = { {0, 0, 0, 0, 0, 0}, 0, 0, 0 };

	getTime(zi.tmz_date);
	target = entry;

	int err = zipOpenNewFileInZip(zipfile, target, &zi,
		        NULL, 0, NULL, 0, NULL, Z_DEFLATED, Z_DEFAULT_COMPRESSION);

	return !(entryopen = (err == ZIP_OK));
}	// end of addEntry

/***********************************************************************/
/*  writeEntry: Deflate the buffer to the zip file.                    */
/***********************************************************************/
int ZIPUTIL::writeEntry(PGLOBAL g, char *buf, int len)
{
	if (zipWriteInFileInZip(zipfile, buf, len) < 0) {
		sprintf(g->Message, "Error writing %s in the zipfile", target);
		return RC_FX;
	}	// endif zipWriteInFileInZip

	return RC_OK;
} // end of writeEntry

/***********************************************************************/
/*  Close the zip file.																								 */
/***********************************************************************/
void ZIPUTIL::closeEntry()
{
	if (entryopen) {
		zipCloseFileInZip(zipfile);
		entryopen = false;
	}	// endif entryopen

}	// end of closeEntry

/* ------------------------- class UNZIPUTL -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
UNZIPUTL::UNZIPUTL(PCSZ tgt, bool mul)
{
	zipfile = NULL;
	target = tgt;
	pwd = NULL;
	fp = NULL;
	memory = NULL;
	size = 0;
	entryopen = false;
	multiple = mul;
	memset(fn, 0, sizeof(fn));

	// Init the case mapping table.
#if defined(__WIN__)
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = toupper(i);
#else
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = i;
#endif
} // end of UNZIPUTL standard constructor

UNZIPUTL::UNZIPUTL(PDOSDEF tdp)
{
	zipfile = NULL;
	target = tdp->GetEntry();
	pwd = tdp->Pwd;
	fp = NULL;
	memory = NULL;
	size = 0;
	entryopen = false;
	multiple = tdp->GetMul();
	memset(fn, 0, sizeof(fn));

	// Init the case mapping table.
#if defined(__WIN__)
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = toupper(i);
#else
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = i;
#endif
} // end of UNZIPUTL standard constructor

#if 0
UNZIPUTL::UNZIPUTL(PZIPUTIL zutp)
{
	zipfile = zutp->zipfile;
	target = zutp->target;
	fp = zutp->fp;
	finfo = zutp->finfo;
	entryopen = zutp->entryopen;
	multiple = zutp->multiple;
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = zutp->mapCaseTable[i];
} // end of UNZIPUTL copy constructor
#endif // 0

/***********************************************************************/
/* This code is the copyright property of Alessandro Felice Cantatore. */
/* http://xoomer.virgilio.it/acantato/dev/wildcard/wildmatch.html			 */
/***********************************************************************/
bool UNZIPUTL::WildMatch(PCSZ pat, PCSZ str) {
	PCSZ s, p;
	bool star = FALSE;

loopStart:
	for (s = str, p = pat; *s; ++s, ++p) {
		switch (*p) {
		case '?':
			if (*s == '.') goto starCheck;
			break;
		case '*':
			star = TRUE;
			str = s, pat = p;
			if (!*++pat) return TRUE;
			goto loopStart;
		default:
			if (mapCaseTable[(uint)*s] != mapCaseTable[(uint)*p])
				goto starCheck;
			break;
		} /* endswitch */
	} /* endfor */
	if (*p == '*') ++p;
	return (!*p);

starCheck:
	if (!star) return FALSE;
	str++;
	goto loopStart;
}	// end of WildMatch

/***********************************************************************/
/*  open a zip file.																									 */
/*  param: filename	path and the filename of the zip file to open.		 */
/*  return:	true if open, false otherwise.														 */
/***********************************************************************/
bool UNZIPUTL::open(PGLOBAL g, PCSZ filename)
{
	if (!zipfile && !(zipfile = unzOpen64(filename)))
		sprintf(g->Message, "Zipfile open error on %s", filename);

	return (zipfile == NULL);
}	// end of open

/***********************************************************************/
/*  Close the zip file.																								 */
/***********************************************************************/
void UNZIPUTL::close()
{
	if (zipfile) {
		closeEntry();
		unzClose(zipfile);
		zipfile = NULL;
	}	// endif zipfile

	if (fp)
		fp->Count = 0;

}	// end of close

/***********************************************************************/
/*  Find next entry matching target pattern.                           */
/***********************************************************************/
int UNZIPUTL::findEntry(PGLOBAL g, bool next)
{
	int  rc;

	do {
		if (next) {
			rc = unzGoToNextFile(zipfile);

			if (rc == UNZ_END_OF_LIST_OF_FILE)
				return RC_EF;
			else if (rc != UNZ_OK) {
				sprintf(g->Message, "unzGoToNextFile rc = %d", rc);
				return RC_FX;
			} // endif rc

		} // endif next

		if (target && *target) {
			rc = unzGetCurrentFileInfo(zipfile, NULL, fn, sizeof(fn),
				NULL, 0, NULL, 0);
			if (rc == UNZ_OK) {
				if (WildMatch(target, fn))
					return RC_OK;

			} else {
				sprintf(g->Message, "GetCurrentFileInfo rc = %d", rc);
				return RC_FX;
			} // endif rc

		} else
			return RC_OK;

		next = true;
	} while (true);

	strcpy(g->Message, "FindNext logical error");
	return RC_FX;
}	// end of FindEntry


/***********************************************************************/
/*  Get the next used entry.                                           */
/***********************************************************************/
int UNZIPUTL::nextEntry(PGLOBAL g)
{
	if (multiple) {
		int rc;

		closeEntry();

		if ((rc = findEntry(g, true)) != RC_OK)
			return rc;

		if (openEntry(g))
			return RC_FX;

		return RC_OK;
	} else
		return RC_EF;

} // end of nextEntry


/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file from a ZIP file.         */
/***********************************************************************/
bool UNZIPUTL::OpenTable(PGLOBAL g, MODE mode, PCSZ fn)
{
	/*********************************************************************/
	/*  The file will be decompressed into virtual memory.               */
	/*********************************************************************/
	if (mode == MODE_READ || mode == MODE_ANY) {
		bool b = open(g, fn);

		if (!b) {
			int rc;

			if (target && *target) {
				if (!multiple) {
					rc = unzLocateFile(zipfile, target, 0);

					if (rc == UNZ_END_OF_LIST_OF_FILE) {
						sprintf(g->Message, "Target file %s not in %s", target, fn);
						return true;
					} else if (rc != UNZ_OK) {
						sprintf(g->Message, "unzLocateFile rc=%d", rc);
						return true;
					}	// endif's rc

				} else {
					if ((rc = findEntry(g, false)) == RC_FX)
						return true;
					else if (rc == RC_EF) {
						sprintf(g->Message, "No match of %s in %s", target, fn);
						return true;
					} // endif rc

				} // endif multiple

			} // endif target

			if (openEntry(g))
				return true;

			if (size > 0) {
				/*******************************************************************/
				/*  Link a Fblock. This make possible to automatically close it    */
				/*  in case of error g->jump.                                      */
				/*******************************************************************/
				PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

				fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
				fp->Type = TYPE_FB_ZIP;
				fp->Fname = PlugDup(g, fn);
				fp->Next = dbuserp->Openlist;
				dbuserp->Openlist = fp;
				fp->Count = 1;
				fp->Length = size;
				fp->Memory = memory;
				fp->Mode = mode;
				fp->File = this;
				fp->Handle = 0;
			} // endif fp

		} else
			return true;

	} else {
		strcpy(g->Message, "Only READ mode supported for ZIPPED tables");
		return true;
	}	// endif mode

	return false;
} // end of OpenTableFile

/***********************************************************************/
/*  Insert only if the entry does not exist.   												 */
/***********************************************************************/
bool UNZIPUTL::IsInsertOk(PGLOBAL g, PCSZ fn)
{
	bool ok = true, b = open(g, fn);

	if (!b) {
		if (!target || *target == 0) {
			unz_global_info64 ginfo;
			int err = unzGetGlobalInfo64(zipfile, &ginfo);

			ok = !(err == UNZ_OK && ginfo.number_entry > 0);
		} else						// Check if the target exist
			ok = (unzLocateFile(zipfile, target, 0) != UNZ_OK);

		unzClose(zipfile);
	} // endif b

	return ok;
} // end of IsInsertOk

/***********************************************************************/
/*  Open target in zip file.						      												 */
/***********************************************************************/
bool UNZIPUTL::openEntry(PGLOBAL g)
{
	int rc;

	rc = unzGetCurrentFileInfo(zipfile, &finfo, fn, sizeof(fn),
		NULL, 0, NULL, 0);

	if (rc != UNZ_OK) {
		sprintf(g->Message, "unzGetCurrentFileInfo64 rc=%d", rc);
		return true;
	} else if ((rc = unzOpenCurrentFilePassword(zipfile, pwd)) != UNZ_OK) {
		sprintf(g->Message, "unzOpen fn=%s rc=%d", fn, rc);
		return true;
	}	// endif rc

	size = finfo.uncompressed_size;

	try {
		memory = new char[size + 1];
	} catch (...) {
		strcpy(g->Message, "Out of memory");
		return true;
	} // end try/catch

	if ((rc = unzReadCurrentFile(zipfile, memory, size)) < 0) {
		sprintf(g->Message, "unzReadCurrentFile rc = %d", rc);
		unzCloseCurrentFile(zipfile);
		delete[] memory;
		memory = NULL;
		entryopen = false;
	} else {
		memory[size] = 0;    // Required by some table types (XML)
		entryopen = true;
	} // endif rc

	if (trace(1))
		htrc("Openning entry%s %s\n", fn, (entryopen) ? "oked" : "failed");

	return !entryopen;
}	// end of openEntry

/***********************************************************************/
/*  Close the zip file.																								 */
/***********************************************************************/
void UNZIPUTL::closeEntry()
{
	if (entryopen) {
		unzCloseCurrentFile(zipfile);
		entryopen = false;
	}	// endif entryopen

	if (memory) {
		delete[] memory;
		memory = NULL;
	}	// endif memory

}	// end of closeEntry

/* -------------------------- class UNZFAM --------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
UNZFAM::UNZFAM(PDOSDEF tdp) : MAPFAM(tdp)
{
	zutp = NULL;
	tdfp = tdp;
  //target = tdp->GetEntry();
	//mul = tdp->GetMul();
} // end of UNZFAM standard constructor

UNZFAM::UNZFAM(PUNZFAM txfp) : MAPFAM(txfp)
{
	zutp = txfp->zutp;
	tdfp = txfp->tdfp;
	//target = txfp->target;
	//mul = txfp->mul;
} // end of UNZFAM copy constructor

/***********************************************************************/
/*  ZIP GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int UNZFAM::GetFileLength(PGLOBAL g)
{
	int len = (zutp && zutp->entryopen) ? (int)(Top - Memory)
		                                  : TXTFAM::GetFileLength(g) * 3;

	if (trace(1))
		htrc("Zipped file length=%d\n", len);

	return len;
} // end of GetFileLength

/***********************************************************************/
/*  ZIP Cardinality: return the number of rows if possible.            */
/***********************************************************************/
int UNZFAM::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;

	int card = -1;
	int len = GetFileLength(g);

	if (len) {
		// Estimated ???
		card = (len / (int)Lrecl) * 2;
		card = card ? card : 10;				// Lrecl can be too big 
	} else
		card = 0;

	return card;
} // end of Cardinality

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file from a ZIP file.         */
/***********************************************************************/
bool UNZFAM::OpenTableFile(PGLOBAL g)
{
	char    filename[_MAX_PATH];
	MODE    mode = Tdbp->GetMode();

	/*********************************************************************/
	/*  Allocate the ZIP utility class.                                  */
	/*********************************************************************/
	zutp = new(g) UNZIPUTL(tdfp);

	//  We used the file name relative to recorded datapath
	PlugSetPath(filename, To_File, Tdbp->GetPath());

	if (!zutp->OpenTable(g, mode, filename)) {
		// The pseudo "buffer" is here the entire real buffer
		Fpos = Mempos = Memory = zutp->memory;
		Top = Memory + zutp->size;
		To_Fb = zutp->fp;                           // Useful when closing
	} else
		return true;

	return false;
	} // end of OpenTableFile

/***********************************************************************/
/*  GetNext: go to next entry.                                         */
/***********************************************************************/
int UNZFAM::GetNext(PGLOBAL g)
{
	int rc = zutp->nextEntry(g);

	if (rc != RC_OK)
		return rc;

	Mempos = Memory = zutp->memory;
	Top = Memory + zutp->size;
	return RC_OK;
} // end of GetNext

#if 0
/***********************************************************************/
/*  ReadBuffer: Read one line for a ZIP file.                          */
/***********************************************************************/
int UNZFAM::ReadBuffer(PGLOBAL g)
{
	int rc, len;

	// Are we at the end of the memory
	if (Mempos >= Top) {
		if ((rc = zutp->nextEntry(g)) != RC_OK)
			return rc;

		Mempos = Memory = zutp->memory;
		Top = Memory + zutp->size;
	}	// endif Mempos

#if 0
	if (!Placed) {
		/*******************************************************************/
		/*  Record file position in case of UPDATE or DELETE.              */
		/*******************************************************************/
		int rc;

	next:
		Fpos = Mempos;
		CurBlk = (int)Rows++;

		/*******************************************************************/
		/*  Check whether optimization on ROWID                            */
		/*  can be done, as well as for join as for local filtering.       */
		/*******************************************************************/
		switch (Tdbp->TestBlock(g)) {
		case RC_EF:
			return RC_EF;
		case RC_NF:
			// Skip this record
			if ((rc = SkipRecord(g, false)) != RC_OK)
				return rc;

			goto next;
		} // endswitch rc

	} else
		Placed = false;
#else
	// Perhaps unuseful
	Fpos = Mempos;
	CurBlk = (int)Rows++;
	Placed = false;
#endif

	// Immediately calculate next position (Used by DeleteDB)
	while (*Mempos++ != '\n');        // What about Unix ???

	// Set caller line buffer
	len = (Mempos - Fpos) - 1;

	// Don't rely on ENDING setting
	if (len > 0 && *(Mempos - 2) == '\r')
		len--;             // Line ends by CRLF

	memcpy(Tdbp->GetLine(), Fpos, len);
	Tdbp->GetLine()[len] = '\0';
	return RC_OK;
} // end of ReadBuffer

/***********************************************************************/
/*  Table file close routine for MAP access method.                    */
/***********************************************************************/
void UNZFAM::CloseTableFile(PGLOBAL g, bool)
{
	close();
} // end of CloseTableFile
#endif // 0

/* -------------------------- class UZXFAM --------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
UZXFAM::UZXFAM(PDOSDEF tdp) : MPXFAM(tdp)
{
	zutp = NULL;
	tdfp = tdp;
	//target = tdp->GetEntry();
	//mul = tdp->GetMul();
	//Lrecl = tdp->GetLrecl();
} // end of UZXFAM standard constructor

UZXFAM::UZXFAM(PUZXFAM txfp) : MPXFAM(txfp)
{
	zutp = txfp->zutp;
	tdfp = txfp->tdfp;
	//target = txfp->target;
	//mul = txfp->mul;
	//Lrecl = txfp->Lrecl;
} // end of UZXFAM copy constructor

/***********************************************************************/
/*  ZIP GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int UZXFAM::GetFileLength(PGLOBAL g)
{
	int len;

	if (!zutp && OpenTableFile(g))
		return 0;

	if (zutp->entryopen)
		len = zutp->size;
	else
		len = 0;

	return len;
} // end of GetFileLength

/***********************************************************************/
/*  ZIP Cardinality: return the number of rows if possible.            */
/***********************************************************************/
int UZXFAM::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;

	int card = -1;
	int len = GetFileLength(g);

	if (!(len % Lrecl))
		card = len / (int)Lrecl;           // Fixed length file
	else
		sprintf(g->Message, MSG(NOT_FIXED_LEN), zutp->fn, len, Lrecl);

	// Set number of blocks for later use
	Block = (card > 0) ? (card + Nrec - 1) / Nrec : 0;
	return card;
} // end of Cardinality

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file from a ZIP file.         */
/***********************************************************************/
bool UZXFAM::OpenTableFile(PGLOBAL g)
{
	// May have been already opened in GetFileLength
	if (!zutp || !zutp->zipfile) {
		char    filename[_MAX_PATH];
		MODE    mode = Tdbp->GetMode();

		/*********************************************************************/
		/*  Allocate the ZIP utility class.                                  */
		/*********************************************************************/
		if (!zutp)
			zutp = new(g)UNZIPUTL(tdfp);

		//  We used the file name relative to recorded datapath
		PlugSetPath(filename, To_File, Tdbp->GetPath());

		if (!zutp->OpenTable(g, mode, filename)) {
			// The pseudo "buffer" is here the entire real buffer
			Memory = zutp->memory;
			Fpos = Mempos = Memory + Headlen;
			Top = Memory + zutp->size;
			To_Fb = zutp->fp;                           // Useful when closing
		} else
			return true;

	} else
		Reset();

	return false;
} // end of OpenTableFile

/***********************************************************************/
/*  GetNext: go to next entry.                                         */
/***********************************************************************/
int UZXFAM::GetNext(PGLOBAL g)
{
	int rc = zutp->nextEntry(g);

	if (rc != RC_OK)
			return rc;

	int len = zutp->size;

	if (len % Lrecl) {
		sprintf(g->Message, MSG(NOT_FIXED_LEN), zutp->fn, len, Lrecl);
		return RC_FX;
	}	// endif size

	Memory = zutp->memory;
	Top = Memory + len;
	Rewind();
	return RC_OK;
} // end of GetNext

/* -------------------------- class ZIPFAM --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
ZIPFAM::ZIPFAM(PDOSDEF tdp) : DOSFAM(tdp)
{
	zutp = NULL;
	target = tdp->GetEntry();
	append = tdp->GetAppend();
} // end of ZIPFAM standard constructor

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file from a ZIP file.         */
/***********************************************************************/
bool ZIPFAM::OpenTableFile(PGLOBAL g)
{
	char filename[_MAX_PATH];
	MODE mode = Tdbp->GetMode();
	int  len = TXTFAM::GetFileLength(g);

	//  We used the file name relative to recorded datapath
	PlugSetPath(filename, To_File, Tdbp->GetPath());

	if (len < 0)
		return true;
	else if (!append && len > 0) {
		strcpy(g->Message, "No insert into existing zip file");
		return true;
	} else if (append && len > 0) {
		UNZIPUTL *zutp = new(g) UNZIPUTL(target, false);

		if (!zutp->IsInsertOk(g, filename)) {
			strcpy(g->Message, "No insert into existing entry");
			return true;
		}	// endif Ok

	} // endif's

	/*********************************************************************/
	/*  Allocate the ZIP utility class.                                  */
	/*********************************************************************/
	zutp = new(g) ZIPUTIL(target);

	//  We used the file name relative to recorded datapath
	PlugSetPath(filename, To_File, Tdbp->GetPath());

	if (!zutp->OpenTable(g, mode, filename, append)) {
		To_Fb = zutp->fp;                           // Useful when closing
	} else
		return true;

	return AllocateBuffer(g);
} // end of OpenTableFile

/***********************************************************************/
/*  ReadBuffer: Read one line for a ZIP file.                          */
/***********************************************************************/
int ZIPFAM::ReadBuffer(PGLOBAL g)
{
	strcpy(g->Message, "ReadBuffer should not been called when zipping");
	return RC_FX;
} // end of ReadBuffer

/***********************************************************************/
/*  WriteBuffer: Deflate the buffer to the zip file.                   */
/***********************************************************************/
int ZIPFAM::WriteBuffer(PGLOBAL g)
{
	int len;

	//  Prepare to write the new line
	strcat(strcpy(To_Buf, Tdbp->GetLine()), (Bin) ? CrLf : "\n");
	len = (int)(strchr(To_Buf, '\n') - To_Buf + 1);
	return zutp->writeEntry(g, To_Buf, len);
} // end of WriteBuffer

/***********************************************************************/
/*  Table file close routine for ZIP access method.                    */
/***********************************************************************/
void ZIPFAM::CloseTableFile(PGLOBAL g, bool)
{
	To_Fb->Count = 0;
	zutp->close();
} // end of CloseTableFile

/* -------------------------- class ZPXFAM --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
ZPXFAM::ZPXFAM(PDOSDEF tdp) : FIXFAM(tdp)
{
	zutp = NULL;
	target = tdp->GetEntry();
	append = tdp->GetAppend();
	//Lrecl = tdp->GetLrecl();
} // end of ZPXFAM standard constructor

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file from a ZIP file.         */
/***********************************************************************/
bool ZPXFAM::OpenTableFile(PGLOBAL g)
{
	char filename[_MAX_PATH];
	MODE mode = Tdbp->GetMode();
	int  len = TXTFAM::GetFileLength(g);

	if (len < 0)
		return true;
	else if (!append && len > 0) {
		strcpy(g->Message, "No insert into existing zip file");
		return true;
	} else if (append && len > 0) {
		UNZIPUTL *zutp = new(g) UNZIPUTL(target, false);

		if (!zutp->IsInsertOk(g, filename)) {
			strcpy(g->Message, "No insert into existing entry");
			return true;
		}	// endif Ok

	} // endif's

	/*********************************************************************/
	/*  Allocate the ZIP utility class.                                  */
	/*********************************************************************/
	zutp = new(g) ZIPUTIL(target);

	//  We used the file name relative to recorded datapath
	PlugSetPath(filename, To_File, Tdbp->GetPath());

	if (!zutp->OpenTable(g, mode, filename, append)) {
		To_Fb = zutp->fp;                           // Useful when closing
	} else
		return true;

	return AllocateBuffer(g);
} // end of OpenTableFile

/***********************************************************************/
/*  WriteBuffer: Deflate the buffer to the zip file.                   */
/***********************************************************************/
int ZPXFAM::WriteBuffer(PGLOBAL g)
{
	/*********************************************************************/
	/*  In Insert mode, we write only full blocks.                       */
	/*********************************************************************/
	if (++CurNum != Rbuf) {
		Tdbp->IncLine(Lrecl);            // Used by DOSCOL functions
		return RC_OK;
	} // endif CurNum

	//  Now start the compress process.
	if (zutp->writeEntry(g, To_Buf, Lrecl * Rbuf) != RC_OK) {
		Closing = true;
		return RC_FX;
	}	// endif writeEntry

	CurBlk++;
	CurNum = 0;
	Tdbp->SetLine(To_Buf);
	return RC_OK;
} // end of WriteBuffer

/***********************************************************************/
/*  Table file close routine for ZIP access method.                    */
/***********************************************************************/
void ZPXFAM::CloseTableFile(PGLOBAL g, bool)
{
	if (CurNum && !Closing) {
		// Some more inserted lines remain to be written
		Rbuf = CurNum--;
		WriteBuffer(g);
	} // endif Curnum

	To_Fb->Count = 0;
	zutp->close();
} // end of CloseTableFile
