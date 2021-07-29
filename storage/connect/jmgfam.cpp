/************ JMONGO FAM C++ Program Source Code File (.CPP) ***********/
/* PROGRAM NAME: jmgfam.cpp                                            */
/* -------------                                                       */
/*  Version 1.2                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          20017 - 2021 */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the Java MongoDB access method classes.           */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the System header files.              */
/***********************************************************************/
#include "my_global.h"
#if defined(_WIN32)
//#include <io.h>
//#include <fcntl.h>
//#include <errno.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif   // __BORLANDC__
//#include <windows.h>
#else   // !_WIN32
#if defined(UNIX) || defined(UNIV_LINUX)
//#include <errno.h>
#include <unistd.h>
//#if !defined(sun)                      // Sun has the ftruncate fnc.
//#define USETEMP                        // Force copy mode for DELETE
//#endif   // !sun
#else   // !UNIX
//#include <io.h>
#endif  // !UNIX
//#include <fcntl.h>
#endif  // !_WIN32

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  filamtxt.h  is header containing the file AM classes declarations. */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "reldef.h"
#include "filamtxt.h"
#include "tabdos.h"
#if defined(BSON_SUPPORT)
#include "tabbson.h"
#else
#include "tabjson.h"
#endif   // BSON_SUPPORT
#include "jmgfam.h"

#if defined(UNIX) || defined(UNIV_LINUX)
#include "osutil.h"
//#define _fileno fileno
//#define _O_RDONLY O_RDONLY
#endif

/* --------------------------- Class JMGFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
JMGFAM::JMGFAM(PJDEF tdp) : DOSFAM((PDOSDEF)NULL)
{
	Jcp = NULL;
	//Client = NULL;
	//Database = NULL;
	//Collection = NULL;
	//Cursor = NULL;
	//Query = NULL;
	//Opts = NULL;
	Ops.Driver = tdp->Schema;
	Ops.Url = tdp->Uri;
	Ops.User = NULL;
	Ops.Pwd = NULL;
	Ops.Scrollable = false;
	Ops.Fsize = 0;
	Ops.Version = tdp->Version;
	To_Fbt = NULL;
	Mode = MODE_ANY;
	Uristr = tdp->Uri;
	Db_name = tdp->Schema;
	Coll_name = tdp->Collname;
	Options = tdp->Options;
	Filter = tdp->Filter;
	Wrapname = tdp->Wrapname;
	Done = false;
	Pipe = tdp->Pipe;
	Version = tdp->Version;
	Lrecl = tdp->Lrecl + tdp->Ending;
	Curpos = 0;
} // end of JMGFAM Json standard constructor

#if defined(BSON_SUPPORT)
JMGFAM::JMGFAM(PBDEF tdp) : DOSFAM((PDOSDEF)NULL)
{
	Jcp = NULL;
	Ops.Driver = tdp->Schema;
	Ops.Url = tdp->Uri;
	Ops.User = NULL;
	Ops.Pwd = NULL;
	Ops.Scrollable = false;
	Ops.Fsize = 0;
	Ops.Version = tdp->Version;
	To_Fbt = NULL;
	Mode = MODE_ANY;
	Uristr = tdp->Uri;
	Db_name = tdp->Schema;
	Coll_name = tdp->Collname;
	Options = tdp->Options;
	Filter = tdp->Filter;
	Wrapname = tdp->Wrapname;
	Done = false;
	Pipe = tdp->Pipe;
	Version = tdp->Version;
	Lrecl = tdp->Lrecl + tdp->Ending;
	Curpos = 0;
} // end of JMGFAM Bson standard constructor
#endif   // BSON_SUPPORT

JMGFAM::JMGFAM(PJMGFAM tdfp) : DOSFAM(tdfp)
{
	Jcp = tdfp->Jcp;
	//Client = tdfp->Client;
	//Database = NULL;
	//Collection = tdfp->Collection;
	//Cursor = tdfp->Cursor;
	//Query = tdfp->Query;
	//Opts = tdfp->Opts;
	Ops = tdfp->Ops;
	To_Fbt = tdfp->To_Fbt;
	Mode = tdfp->Mode;
	Uristr = tdfp->Uristr;
	Db_name = tdfp->Db_name;
	Coll_name = tdfp->Coll_name;
	Options = tdfp->Options;
	Filter = NULL;
	Wrapname = tdfp->Wrapname;
	Done = tdfp->Done;
	Pipe = tdfp->Pipe;
	Version = tdfp->Version;
	Curpos = tdfp->Curpos;
} // end of JMGFAM copy constructor

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void JMGFAM::Reset(void)
{
	TXTFAM::Reset();
	Fpos = Tpos = Spos = 0;
} // end of Reset

/***********************************************************************/
/*  MGO GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int JMGFAM::GetFileLength(PGLOBAL g)
{
	return 0;
} // end of GetFileLength

/***********************************************************************/
/*  Cardinality: returns table cardinality in number of rows.          */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int JMGFAM::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;

	return (!Init(g)) ? Jcp->CollSize(g) : 0;
} // end of Cardinality

/***********************************************************************/
/*  Note: This function is not really implemented yet.                 */
/***********************************************************************/
int JMGFAM::MaxBlkSize(PGLOBAL, int s)
{
	return s;
} // end of MaxBlkSize

/***********************************************************************/
/*  Init: initialize MongoDB processing.                               */
/***********************************************************************/
bool JMGFAM::Init(PGLOBAL g)
{
	if (Done)
		return false;

	/*********************************************************************/
	/*  Open an JDBC connection for this table.                          */
	/*  Note: this may not be the proper way to do. Perhaps it is better */
	/*  to test whether a connection is already open for this datasource */
	/*  and if so to allocate just a new result set. But this only for   */
	/*  drivers allowing concurency in getting results ???               */
	/*********************************************************************/
	if (!Jcp)
		Jcp = new(g) JMgoConn(g, Coll_name, Wrapname);
	else if (Jcp->IsOpen())
		Jcp->Close();

	if (Jcp->Connect(&Ops))
		return true;

	Done = true;
	return false;
} // end of Init

/***********************************************************************/
/*  OpenTableFile: Open a MongoDB table.                               */
/***********************************************************************/
bool JMGFAM::OpenTableFile(PGLOBAL g)
{
	Mode = Tdbp->GetMode();

	if (Pipe && Mode != MODE_READ) {
		strcpy(g->Message, "Pipeline tables are read only");
		return true;
	}	// endif Pipe

	if (Init(g))
		return true;

	if (Jcp->GetMethodId(g, Mode))
		return true;

	if (Mode == MODE_DELETE && !Tdbp->GetNext()) {
		// Delete all documents
		if (!Jcp->MakeCursor(g, Tdbp, "all", Filter, false))
			if (Jcp->DocDelete(g, true) == RC_OK)
				return false;

		return true;
	}	// endif Mode

//if (Mode == MODE_INSERT)
//	Jcp->MakeColumnGroups(g, Tdbp);

	if (Mode != MODE_UPDATE)
		return Jcp->MakeCursor(g, Tdbp, Options, Filter, Pipe);

	return false;
	} // end of OpenTableFile

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int JMGFAM::GetRowID(void)
{
	return Rows;
} // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int JMGFAM::GetPos(void)
{
	return Fpos;
} // end of GetPos

/***********************************************************************/
/*  GetNextPos: return the position of next record.                    */
/***********************************************************************/
int JMGFAM::GetNextPos(void)
{
	return Fpos;						// TODO
} // end of GetNextPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool JMGFAM::SetPos(PGLOBAL g, int pos)
{
	Fpos = pos;
	Placed = true;
	return false;
} // end of SetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/***********************************************************************/
bool JMGFAM::RecordPos(PGLOBAL g)
{
	strcpy(g->Message, "JMGFAM::RecordPos NIY");
	return true;
} // end of RecordPos

/***********************************************************************/
/*  Initialize Fpos and the current position for indexed DELETE.       */
/***********************************************************************/
int JMGFAM::InitDelete(PGLOBAL g, int fpos, int spos)
{
	strcpy(g->Message, "JMGFAM::InitDelete NIY");
	return RC_FX;
} // end of InitDelete

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int JMGFAM::SkipRecord(PGLOBAL g, bool header)
{
	return RC_OK;                  // Dummy
} // end of SkipRecord

/***********************************************************************/
/*  ReadBuffer: Get next document from a collection.                   */
/***********************************************************************/
int JMGFAM::ReadBuffer(PGLOBAL g)
{
	int rc = RC_FX;

	if (!Curpos && Mode == MODE_UPDATE)
		if (Jcp->MakeCursor(g, Tdbp, Options, Filter, Pipe))
			return RC_FX;

	if (++CurNum >= Rbuf) {
		Rbuf = Jcp->Fetch();
		Curpos++;
		CurNum = 0;
	} // endif CurNum

	if (Rbuf > 0) {
		PSZ str = Jcp->GetDocument();

		if (str) {
			if (trace(1))
				htrc("%s\n", str);

			strncpy(Tdbp->GetLine(), str, Lrecl);
			rc = RC_OK;
		} else
			strcpy(g->Message, "Null document");

	} else if (!Rbuf)
		rc = RC_EF;

	return rc;
} // end of ReadBuffer

/***********************************************************************/
/*  WriteBuffer: File write routine for JMG access method.             */
/***********************************************************************/
int JMGFAM::WriteBuffer(PGLOBAL g)
{
	int rc = RC_OK;

	if (Mode == MODE_INSERT) {
		rc = Jcp->DocWrite(g, Tdbp->GetLine());
	} else if (Mode == MODE_DELETE) {
		rc = Jcp->DocDelete(g, false);
	} else if (Mode == MODE_UPDATE) {
		rc = Jcp->DocUpdate(g, Tdbp);
	}	// endif Mode

	return rc;
} // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for MGO and BLK access methods.      */
/***********************************************************************/
int JMGFAM::DeleteRecords(PGLOBAL g, int irc)
{
	return (irc == RC_OK) ? WriteBuffer(g) : RC_OK;
} // end of DeleteRecords

/***********************************************************************/
/*  Table file close routine for MGO access method.                    */
/***********************************************************************/
void JMGFAM::CloseTableFile(PGLOBAL g, bool)
{
	Jcp->Close();
	Done = false;
} // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for MGO access method.                              */
/***********************************************************************/
void JMGFAM::Rewind(void)
{
	Jcp->Rewind();
} // end of Rewind

