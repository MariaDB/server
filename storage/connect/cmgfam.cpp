/************** CMGFAM C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: cmgfam.cpp                                            */
/* -------------                                                       */
/*  Version 1.5                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          20017 - 2020 */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program are the MongoDB access method classes.                */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the System header files.              */
/***********************************************************************/
#include "my_global.h"

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
#include "cmgfam.h"

#if defined(UNIX) || defined(UNIV_LINUX)
#include "osutil.h"
#endif

/* --------------------------- Class CMGFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
CMGFAM::CMGFAM(PJDEF tdp) : DOSFAM((PDOSDEF)NULL)
{
	Cmgp = NULL;
	Pcg.Tdbp = NULL;

	if (tdp) {
		Pcg.Uristr = tdp->Uri;
		Pcg.Db_name = tdp->Schema;
		Pcg.Coll_name = tdp->Collname;
		Pcg.Options = tdp->Options;
		Pcg.Filter = tdp->Filter;
		Pcg.Line = NULL;
		Pcg.Pipe = tdp->Pipe && tdp->Options != NULL;
		Lrecl = tdp->Lrecl + tdp->Ending;
	} else {
		Pcg.Uristr = NULL;
		Pcg.Db_name = NULL;
		Pcg.Coll_name = NULL;
		Pcg.Options = NULL;
		Pcg.Filter = NULL;
		Pcg.Line = NULL;
		Pcg.Pipe = false;
		Lrecl = 0;
	} // endif tdp

	To_Fbt = NULL;
	Mode = MODE_ANY;
	Done = false;
} // end of CMGFAM standard constructor
 
#if defined(BSON_SUPPORT)
	/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
CMGFAM::CMGFAM(PBDEF tdp) : DOSFAM((PDOSDEF)NULL)
{
	Cmgp = NULL;
	Pcg.Tdbp = NULL;

	if (tdp) {
		Pcg.Uristr = tdp->Uri;
		Pcg.Db_name = tdp->Schema;
		Pcg.Coll_name = tdp->Collname;
		Pcg.Options = tdp->Options;
		Pcg.Filter = tdp->Filter;
		Pcg.Line = NULL;
		Pcg.Pipe = tdp->Pipe && tdp->Options != NULL;
		Lrecl = tdp->Lrecl + tdp->Ending;
	} else {
		Pcg.Uristr = NULL;
		Pcg.Db_name = NULL;
		Pcg.Coll_name = NULL;
		Pcg.Options = NULL;
		Pcg.Filter = NULL;
		Pcg.Line = NULL;
		Pcg.Pipe = false;
		Lrecl = 0;
	} // endif tdp

	To_Fbt = NULL;
	Mode = MODE_ANY;
	Done = false;
} // end of CMGFAM standard constructor
#endif    // BSON_SUPPORT

CMGFAM::CMGFAM(PCMGFAM tdfp) : DOSFAM(tdfp)
{
	Cmgp = tdfp->Cmgp;
	Pcg = tdfp->Pcg;
	To_Fbt = tdfp->To_Fbt;
	Mode = tdfp->Mode;
	Done = tdfp->Done;
} // end of CMGFAM copy constructor

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void CMGFAM::Reset(void)
{
	TXTFAM::Reset();
	Fpos = Tpos = Spos = 0;
} // end of Reset

/***********************************************************************/
/*  MGO GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int CMGFAM::GetFileLength(PGLOBAL g)
{
	return 0;
} // end of GetFileLength

/***********************************************************************/
/*  Cardinality: returns the number of documents in the collection.    */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int CMGFAM::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;

	return (!Init(g)) ? Cmgp->CollSize(g) : 0;
} // end of Cardinality

/***********************************************************************/
/*  Note: This function is not really implemented yet.                 */
/***********************************************************************/
int CMGFAM::MaxBlkSize(PGLOBAL, int s)
{
	return s;
} // end of MaxBlkSize

/***********************************************************************/
/*  Init: initialize MongoDB processing.                               */
/***********************************************************************/
bool CMGFAM::Init(PGLOBAL g)
{
	if (Done)
		return false;

	/*********************************************************************/
	/*  Open an C connection for this table.                             */
	/*********************************************************************/
	if (!Cmgp) {
		Pcg.Tdbp = Tdbp;
		Cmgp = new(g) CMgoConn(g, &Pcg);
	} else if (Cmgp->IsConnected())
		Cmgp->Close();

	if (Cmgp->Connect(g))
		return true;

	Done = true;
	return false;
} // end of Init

/***********************************************************************/
/*  OpenTableFile: Open a MongoDB table.                               */
/***********************************************************************/
bool CMGFAM::OpenTableFile(PGLOBAL g)
{
	Mode = Tdbp->GetMode();

	if (Pcg.Pipe && Mode != MODE_READ) {
		strcpy(g->Message, "Pipeline tables are read only");
		return true;
	}	// endif Pipe

	if (Init(g))
		return true;

	if (Mode == MODE_DELETE && !Tdbp->GetNext())
		// Delete all documents
		return Cmgp->DocDelete(g);
	else if (Mode == MODE_INSERT)
		Cmgp->MakeColumnGroups(g);

	return false;
} // end of OpenTableFile

/***********************************************************************/
/*  GetRowID: return the RowID of last read record.                    */
/***********************************************************************/
int CMGFAM::GetRowID(void)
{
	return Rows;
} // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int CMGFAM::GetPos(void)
{
	return Fpos;
} // end of GetPos

/***********************************************************************/
/*  GetNextPos: return the position of next record.                    */
/***********************************************************************/
int CMGFAM::GetNextPos(void)
{
	return Fpos;						// TODO
} // end of GetNextPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool CMGFAM::SetPos(PGLOBAL g, int pos)
{
	Fpos = pos;
	Placed = true;
	return false;
} // end of SetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/***********************************************************************/
bool CMGFAM::RecordPos(PGLOBAL g)
{
	strcpy(g->Message, "CMGFAM::RecordPos NIY");
	return true;
} // end of RecordPos

/***********************************************************************/
/*  Initialize Fpos and the current position for indexed DELETE.       */
/***********************************************************************/
int CMGFAM::InitDelete(PGLOBAL g, int fpos, int spos)
{
	strcpy(g->Message, "CMGFAM::InitDelete NIY");
	return RC_FX;
} // end of InitDelete

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int CMGFAM::SkipRecord(PGLOBAL g, bool header)
{
	return RC_OK;                  // Dummy
} // end of SkipRecord

/***********************************************************************/
/*  ReadBuffer: Get next document from a collection.                   */
/***********************************************************************/
int CMGFAM::ReadBuffer(PGLOBAL g)
{
	int rc = Cmgp->ReadNext(g);

	if (rc != RC_OK)
		return rc;

	strncpy(Tdbp->GetLine(), Cmgp->GetDocument(g), Lrecl);
	return RC_OK;
} // end of ReadBuffer

/***********************************************************************/
/*  WriteBuffer: File write routine for MGO access method.             */
/***********************************************************************/
int CMGFAM::WriteBuffer(PGLOBAL g)
{
	Pcg.Line = Tdbp->GetLine();
	return Cmgp->Write(g);
} // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for MGO and BLK access methods.      */
/***********************************************************************/
int CMGFAM::DeleteRecords(PGLOBAL g, int irc)
{
	return (irc == RC_OK) ? WriteBuffer(g) : RC_OK;
} // end of DeleteRecords

/***********************************************************************/
/*  Table file close routine for MGO access method.                    */
/***********************************************************************/
void CMGFAM::CloseTableFile(PGLOBAL g, bool)
{
	Cmgp->Close();
	Done = false;
} // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for MGO access method.                              */
/***********************************************************************/
void CMGFAM::Rewind(void)
{
	Cmgp->Rewind();
} // end of Rewind

