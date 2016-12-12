/*********** File AM Zip C++ Program Source Code File (.CPP) ***********/
/* PROGRAM NAME: FILAMZIP                                              */
/* -------------                                                       */
/*  Version 1.0                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2016         */
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
#include <errno.h>
#include <unistd.h>
#else    // !UNIX
#include <io.h>
#endif  // !UNIX
#include <fcntl.h>
#endif  // !__WIN__

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

/* -------------------------- class ZIPFAM --------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
ZIPFAM::ZIPFAM(PDOSDEF tdp) : MAPFAM(tdp)
{
	zipfile = NULL;
	zfn = tdp->Zipfn;
	target = tdp->Fn;
//*fn = 0;
	entryopen = false;
	multiple = tdp->Multiple;

	// Init the case mapping table.
#if defined(__WIN__)
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = toupper(i);
#else
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = i;
#endif
} // end of ZIPFAM standard constructor

ZIPFAM::ZIPFAM(PZIPFAM txfp) : MAPFAM(txfp)
{
	zipfile = txfp->zipfile;
	zfn = txfp->zfn;
	target = txfp->target;
//strcpy(fn, txfp->fn);
	finfo = txfp->finfo;
	entryopen = txfp->entryopen;
	multiple = txfp->multiple;
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = txfp->mapCaseTable[i];
} // end of ZIPFAM copy constructor

/***********************************************************************/
/* This code is the copyright property of Alessandro Felice Cantatore. */
/* http://xoomer.virgilio.it/acantato/dev/wildcard/wildmatch.html			 */
/***********************************************************************/
bool ZIPFAM::WildMatch(PSZ pat, PSZ str) {
	PSZ  s, p;
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
			if (mapCaseTable[*s] != mapCaseTable[*p])
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
/*  ZIP GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int ZIPFAM::GetFileLength(PGLOBAL g)
{
	int len = (entryopen) ? Top - Memory : 100;	 // not 0 to avoid ASSERT

	if (trace)
		htrc("Zipped file length=%d\n", len);

	return len;
} // end of GetFileLength

/***********************************************************************/
/*  open a zip file.																									 */
/*  param: filename	path and the filename of the zip file to open.		 */
/*  return:	true if open, false otherwise.														 */
/***********************************************************************/
bool ZIPFAM::open(PGLOBAL g, const char *filename)
{
	if (!zipfile && !(zipfile = unzOpen64(filename)))
		sprintf(g->Message, "Zipfile open error");

	return (zipfile == NULL);
}	// end of open

/***********************************************************************/
/*  Close the zip file.																								 */
/***********************************************************************/
void ZIPFAM::close()
{
	if (zipfile) {
		closeEntry();
		unzClose(zipfile);
		zipfile = NULL;
	}	// endif zipfile

}	// end of close

/***********************************************************************/
/*  Find next entry matching target pattern.                           */
/***********************************************************************/
int ZIPFAM::findEntry(PGLOBAL g, bool next)
{
	int  rc;
	char fn[FILENAME_MAX];     	 // The current entry file name

	do {
		if (next) {
			rc = unzGoToNextFile(zipfile);

			if (rc == UNZ_END_OF_LIST_OF_FILE)
				return RC_EF;
			else if (rc != UNZ_OK) {
				sprintf(g->Message, "unzGoToNextFile rc = ", rc);
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
}	// end of FindNext

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file from a ZIP file.         */
/***********************************************************************/
bool ZIPFAM::OpenTableFile(PGLOBAL g)
{
	char    filename[_MAX_PATH];
	MODE    mode = Tdbp->GetMode();
  PFBLOCK fp;
  PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

	/*********************************************************************/
	/*  The file will be decompressed into virtual memory.               */
	/*********************************************************************/
	if (mode == MODE_READ) {
		//  We used the file name relative to recorded datapath
		PlugSetPath(filename, zfn, Tdbp->GetPath());

		bool b = open(g, filename);

		if (!b) {
			int rc;
			
			if (target && *target) {
				if (!multiple) {
					rc = unzLocateFile(zipfile, target, 0);

					if (rc == UNZ_END_OF_LIST_OF_FILE) {
						sprintf(g->Message, "Target file %s not in %s", target, filename);
						return true;
					} else if (rc != UNZ_OK) {
						sprintf(g->Message, "unzLocateFile rc=%d", rc);
						return true;
					}	// endif's rc

				} else {
					if ((rc = findEntry(g, false)) == RC_FX)
						return true;
					else if (rc == RC_NF) {
						sprintf(g->Message, "No match of %s in %s", target, filename);
						return true;
					} // endif rc

				} // endif multiple

			} // endif target

			if (openEntry(g))
				return true;

			if (Top > Memory)	{
				/*******************************************************************/
				/*  Link a Fblock. This make possible to automatically close it    */
				/*  in case of error g->jump.                                      */
				/*******************************************************************/
				fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
				fp->Type = TYPE_FB_ZIP;
				fp->Fname = PlugDup(g, filename);
				fp->Next = dbuserp->Openlist;
				dbuserp->Openlist = fp;
				fp->Count = 1;
				fp->Length = Top - Memory;
				fp->Memory = Memory;
				fp->Mode = mode;
				fp->File = this;
				fp->Handle = NULL;
			} // endif fp

			To_Fb = fp;                               // Useful when closing
		}	// endif b

	} else {
		strcpy(g->Message, "Only READ mode supported for ZIP files");
		return true;
	}	// endif mode

	return false;
	} // end of OpenTableFile

/***********************************************************************/
/*  Open target in zip file.						      												 */
/***********************************************************************/
bool ZIPFAM::openEntry(PGLOBAL g)
{
	int  rc;
	uint size;

	rc = unzGetCurrentFileInfo(zipfile, &finfo, 0, 0, 0, 0, 0, 0);

	if (rc != UNZ_OK) {
		sprintf(g->Message, "unzGetCurrentFileInfo64 rc=%d", rc);
		return true;
	} else if ((rc = unzOpenCurrentFile(zipfile)) != UNZ_OK) {
		sprintf(g->Message, "unzOpenCurrentFile rc=%d", rc);
		return true;
	}	// endif rc

	size = finfo.uncompressed_size;
	Memory = new char[size];

	if ((rc = unzReadCurrentFile(zipfile, Memory, size)) < 0) {
		sprintf(g->Message, "unzReadCurrentFile rc = ", rc);
		unzCloseCurrentFile(zipfile);
		free(Memory);
		entryopen = false;
	} else {
		// The pseudo "buffer" is here the entire real buffer
		Fpos = Mempos = Memory;
		Top = Memory + size;

		if (trace)
			htrc("Memory=%p size=%ud Top=%p\n", Memory, size, Top);

		entryopen = true;
	} // endif rc

	return !entryopen;
}	// end of openEntry

/***********************************************************************/
/*  Close the zip file.																								 */
/***********************************************************************/
void ZIPFAM::closeEntry()
{
	if (entryopen) {
		unzCloseCurrentFile(zipfile);
		entryopen = false;
	}	// endif entryopen

	if (Memory) {
		free(Memory);
		Memory = NULL;
	}	// endif Memory

}	// end of closeEntry

/***********************************************************************/
/*  ReadBuffer: Read one line for a ZIP file.                          */
/***********************************************************************/
int ZIPFAM::ReadBuffer(PGLOBAL g)
{
	int rc, len;

	// Are we at the end of the memory
	if (Mempos >= Top) {
		if (multiple) {
			closeEntry();

			if ((rc = findEntry(g, true)) != RC_OK)
				return rc;

			if (openEntry(g))
				return RC_FX;

		} else
			return RC_EF;

	} // endif Mempos

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

#if 0
/***********************************************************************/
/*  Table file close routine for MAP access method.                    */
/***********************************************************************/
void ZIPFAM::CloseTableFile(PGLOBAL g, bool)
{
	close();
} // end of CloseTableFile
#endif // 0

/* -------------------------- class ZPXFAM --------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
ZPXFAM::ZPXFAM(PDOSDEF tdp) : ZIPFAM(tdp)
{
	Lrecl = tdp->GetLrecl();
} // end of ZPXFAM standard constructor

ZPXFAM::ZPXFAM(PZPXFAM txfp) : ZIPFAM(txfp)
{
	Lrecl = txfp->Lrecl;
} // end of ZPXFAM copy constructor

/***********************************************************************/
/*  ReadBuffer: Read one line for a fixed ZIP file.                    */
/***********************************************************************/
int ZPXFAM::ReadBuffer(PGLOBAL g)
{
	int rc, len;

	// Are we at the end of the memory
	if (Mempos >= Top) {
		if (multiple) {
			closeEntry();

			if ((rc = findEntry(g, true)) != RC_OK)
				return rc;

			if (openEntry(g))
				return RC_FX;

		} else
			return RC_EF;

	} // endif Mempos

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
	Mempos += Lrecl;

	// Set caller line buffer
	len = Lrecl;

	// Don't rely on ENDING setting
	if (len > 0 && *(Mempos - 1) == '\n')
		len--;             // Line ends by LF

	if (len > 0 && *(Mempos - 2) == '\r')
		len--;             // Line ends by CRLF

	memcpy(Tdbp->GetLine(), Fpos, len);
	Tdbp->GetLine()[len] = '\0';
	return RC_OK;
} // end of ReadBuffer

