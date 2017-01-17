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

/* -------------------------- class ZIPUTIL -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
ZIPUTIL::ZIPUTIL(PSZ tgt, bool mul)
{
	zipfile = NULL;
	target = tgt;
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
} // end of ZIPUTIL standard constructor

#if 0
ZIPUTIL::ZIPUTIL(PZIPUTIL zutp)
{
	zipfile = zutp->zipfile;
	target = zutp->target;
	fp = zutp->fp;
	finfo = zutp->finfo;
	entryopen = zutp->entryopen;
	multiple = zutp->multiple;
	for (int i = 0; i < 256; ++i) mapCaseTable[i] = zutp->mapCaseTable[i];
} // end of ZIPUTIL copy constructor
#endif // 0

/***********************************************************************/
/* This code is the copyright property of Alessandro Felice Cantatore. */
/* http://xoomer.virgilio.it/acantato/dev/wildcard/wildmatch.html			 */
/***********************************************************************/
bool ZIPUTIL::WildMatch(PSZ pat, PSZ str) {
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
			if (mapCaseTable[(unsigned)*s] != mapCaseTable[(unsigned)*p])
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
bool ZIPUTIL::open(PGLOBAL g, char *filename)
{
	if (!zipfile && !(zipfile = unzOpen64(filename)))
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
		unzClose(zipfile);
		zipfile = NULL;
	}	// endif zipfile

}	// end of close

/***********************************************************************/
/*  Find next entry matching target pattern.                           */
/***********************************************************************/
int ZIPUTIL::findEntry(PGLOBAL g, bool next)
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
int ZIPUTIL::nextEntry(PGLOBAL g)
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
bool ZIPUTIL::OpenTable(PGLOBAL g, MODE mode, char *fn)
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

			if (size > 0)	{
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
		strcpy(g->Message, "Only READ mode supported for ZIP files");
		return true;
	}	// endif mode

	return false;
} // end of OpenTableFile

/***********************************************************************/
/*  Open target in zip file.						      												 */
/***********************************************************************/
bool ZIPUTIL::openEntry(PGLOBAL g)
{
	int rc;

	rc = unzGetCurrentFileInfo(zipfile, &finfo, fn, sizeof(fn),
		NULL, 0, NULL, 0);

	if (rc != UNZ_OK) {
		sprintf(g->Message, "unzGetCurrentFileInfo64 rc=%d", rc);
		return true;
	} else if ((rc = unzOpenCurrentFile(zipfile)) != UNZ_OK) {
		sprintf(g->Message, "unzOpen fn=%s rc=%d", fn, rc);
		return true;
	}	// endif rc

	size = finfo.uncompressed_size;
	memory = new char[size + 1];

	if ((rc = unzReadCurrentFile(zipfile, memory, size)) < 0) {
		sprintf(g->Message, "unzReadCurrentFile rc = %d", rc);
		unzCloseCurrentFile(zipfile);
		free(memory);
		memory = NULL;
		entryopen = false;
	} else {
		memory[size] = 0;    // Required by some table types (XML)
		entryopen = true;
	} // endif rc

	if (trace)
		htrc("Openning entry%s %s\n", fn, (entryopen) ? "oked" : "failed");

	return !entryopen;
}	// end of openEntry

/***********************************************************************/
/*  Close the zip file.																								 */
/***********************************************************************/
void ZIPUTIL::closeEntry()
{
	if (entryopen) {
		unzCloseCurrentFile(zipfile);
		entryopen = false;
	}	// endif entryopen

	if (memory) {
		free(memory);
		memory = NULL;
	}	// endif memory

}	// end of closeEntry

/* -------------------------- class ZIPFAM --------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
ZIPFAM::ZIPFAM(PDOSDEF tdp) : MAPFAM(tdp)
{
	zutp = NULL;
  target = tdp->GetEntry();
	mul = tdp->GetMul();
} // end of ZIPFAM standard constructor

ZIPFAM::ZIPFAM(PZIPFAM txfp) : MAPFAM(txfp)
{
	zutp = txfp->zutp;
	target = txfp->target;
	mul = txfp->mul;
} // end of ZIPFAM copy constructor

ZIPFAM::ZIPFAM(PDOSDEF tdp, PZPXFAM txfp) : MAPFAM(tdp)
{
	zutp = txfp->zutp;
	target = txfp->target;
	mul = txfp->mul;
} // end of ZIPFAM constructor used in ResetTableOpt

/***********************************************************************/
/*  ZIP GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int ZIPFAM::GetFileLength(PGLOBAL g)
{
	int len = (zutp && zutp->entryopen) ? Top - Memory
		                                  : TXTFAM::GetFileLength(g) * 3;

	if (trace)
		htrc("Zipped file length=%d\n", len);

	return len;
} // end of GetFileLength

/***********************************************************************/
/*  ZIP Cardinality: return the number of rows if possible.            */
/***********************************************************************/
int ZIPFAM::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;

	int card = -1;
	int len = GetFileLength(g);

	card = (len / (int)Lrecl) * 2;           // Estimated ???
	return card;
} // end of Cardinality

/***********************************************************************/
/*  OpenTableFile: Open a DOS/UNIX table file from a ZIP file.         */
/***********************************************************************/
bool ZIPFAM::OpenTableFile(PGLOBAL g)
{
	char    filename[_MAX_PATH];
	MODE    mode = Tdbp->GetMode();

	/*********************************************************************/
	/*  Allocate the ZIP utility class.                                  */
	/*********************************************************************/
	zutp = new(g) ZIPUTIL(target, mul);

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
int ZIPFAM::GetNext(PGLOBAL g)
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
int ZIPFAM::ReadBuffer(PGLOBAL g)
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
void ZIPFAM::CloseTableFile(PGLOBAL g, bool)
{
	close();
} // end of CloseTableFile
#endif // 0

/* -------------------------- class ZPXFAM --------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
ZPXFAM::ZPXFAM(PDOSDEF tdp) : MPXFAM(tdp)
{
	zutp = NULL;
	target = tdp->GetEntry();
	mul = tdp->GetMul();
	//Lrecl = tdp->GetLrecl();
} // end of ZPXFAM standard constructor

ZPXFAM::ZPXFAM(PZPXFAM txfp) : MPXFAM(txfp)
{
	zutp = txfp->zutp;
	target = txfp->target;
	mul = txfp->mul;
//Lrecl = txfp->Lrecl;
} // end of ZPXFAM copy constructor

/***********************************************************************/
/*  ZIP GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int ZPXFAM::GetFileLength(PGLOBAL g)
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
int ZPXFAM::Cardinality(PGLOBAL g)
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
bool ZPXFAM::OpenTableFile(PGLOBAL g)
{
	// May have been already opened in GetFileLength
	if (!zutp || !zutp->zipfile) {
		char    filename[_MAX_PATH];
		MODE    mode = Tdbp->GetMode();

		/*********************************************************************/
		/*  Allocate the ZIP utility class.                                  */
		/*********************************************************************/
		if (!zutp)
			zutp = new(g)ZIPUTIL(target, mul);

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
int ZPXFAM::GetNext(PGLOBAL g)
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

