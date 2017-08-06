/************ MONGO FAM C++ Program Source Code File (.CPP) ************/
/* PROGRAM NAME: mongofam.cpp                                          */
/* -------------                                                       */
/*  Version 1.3                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          20017        */
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
#include "tabjson.h"
#include "mongofam.h"

#if defined(UNIX) || defined(UNIV_LINUX)
#include "osutil.h"
#endif

/* --------------------------- Class MGOFAM -------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
MGOFAM::MGOFAM(PJDEF tdp) : DOSFAM((PDOSDEF)NULL)
{
	Cmgp = NULL;
	Pcg.Tdbp = NULL;

	if (tdp) {
		Pcg.Uristr = tdp->Uri;
		Pcg.Db_name = tdp->Schema;
		Pcg.Coll_name = tdp->Collname;
		Pcg.Options = tdp->Options;
		Pcg.Filter = tdp->Filter;
		Pcg.Pipe = tdp->Pipe && tdp->Options != NULL;
	} else {
		Pcg.Uristr = NULL;
		Pcg.Db_name = NULL;
		Pcg.Coll_name = NULL;
		Pcg.Options = NULL;
		Pcg.Filter = NULL;
		Pcg.Pipe = false;
	} // endif tdp

	To_Fbt = NULL;
	Mode = MODE_ANY;
	Done = false;
	Lrecl = tdp->Lrecl + tdp->Ending;
} // end of MGOFAM standard constructor
 
 MGOFAM::MGOFAM(PMGOFAM tdfp) : DOSFAM(tdfp)
{
	Pcg = tdfp->Pcg;
	To_Fbt = tdfp->To_Fbt;
	Mode = tdfp->Mode;
	Done = tdfp->Done;
 } // end of MGOFAM copy constructor

/***********************************************************************/
/*  Reset: reset position values at the beginning of file.             */
/***********************************************************************/
void MGOFAM::Reset(void)
{
	TXTFAM::Reset();
	Fpos = Tpos = Spos = 0;
} // end of Reset

/***********************************************************************/
/*  MGO GetFileLength: returns file size in number of bytes.           */
/***********************************************************************/
int MGOFAM::GetFileLength(PGLOBAL g)
{
	return 0;
} // end of GetFileLength

/***********************************************************************/
/*  Cardinality: returns the number of documents in the collection.    */
/*  This function can be called with a null argument to test the       */
/*  availability of Cardinality implementation (1 yes, 0 no).          */
/***********************************************************************/
int MGOFAM::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;

	return (!Init(g)) ? Cmgp->CollSize(g) : 0;
} // end of Cardinality

/***********************************************************************/
/*  Note: This function is not really implemented yet.                 */
/***********************************************************************/
int MGOFAM::MaxBlkSize(PGLOBAL, int s)
{
	return s;
} // end of MaxBlkSize

/***********************************************************************/
/*  Init: initialize MongoDB processing.                               */
/***********************************************************************/
bool MGOFAM::Init(PGLOBAL g)
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
bool MGOFAM::OpenTableFile(PGLOBAL g)
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
int MGOFAM::GetRowID(void)
{
	return Rows;
} // end of GetRowID

/***********************************************************************/
/*  GetPos: return the position of last read record.                   */
/***********************************************************************/
int MGOFAM::GetPos(void)
{
	return Fpos;
} // end of GetPos

/***********************************************************************/
/*  GetNextPos: return the position of next record.                    */
/***********************************************************************/
int MGOFAM::GetNextPos(void)
{
	return Fpos;						// TODO
} // end of GetNextPos

/***********************************************************************/
/*  SetPos: Replace the table at the specified position.               */
/***********************************************************************/
bool MGOFAM::SetPos(PGLOBAL g, int pos)
{
	Fpos = pos;
	Placed = true;
	return false;
} // end of SetPos

/***********************************************************************/
/*  Record file position in case of UPDATE or DELETE.                  */
/***********************************************************************/
bool MGOFAM::RecordPos(PGLOBAL g)
{
	strcpy(g->Message, "MGOFAM::RecordPos NIY");
	return true;
} // end of RecordPos

/***********************************************************************/
/*  Initialize Fpos and the current position for indexed DELETE.       */
/***********************************************************************/
int MGOFAM::InitDelete(PGLOBAL g, int fpos, int spos)
{
	strcpy(g->Message, "MGOFAM::InitDelete NIY");
	return RC_FX;
} // end of InitDelete

/***********************************************************************/
/*  Skip one record in file.                                           */
/***********************************************************************/
int MGOFAM::SkipRecord(PGLOBAL g, bool header)
{
	return RC_OK;                  // Dummy
} // end of SkipRecord

/***********************************************************************/
/*  ReadBuffer: Get next document from a collection.                   */
/***********************************************************************/
int MGOFAM::ReadBuffer(PGLOBAL g)
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
int MGOFAM::WriteBuffer(PGLOBAL g)
{
	return Cmgp->Write(g);
} // end of WriteBuffer

/***********************************************************************/
/*  Data Base delete line routine for MGO and BLK access methods.      */
/***********************************************************************/
int MGOFAM::DeleteRecords(PGLOBAL g, int irc)
{
	return (irc == RC_OK) ? WriteBuffer(g) : RC_OK;
} // end of DeleteRecords

/***********************************************************************/
/*  Table file close routine for MGO access method.                    */
/***********************************************************************/
void MGOFAM::CloseTableFile(PGLOBAL g, bool)
{
	Cmgp->Close();
	Done = false;
} // end of CloseTableFile

/***********************************************************************/
/*  Rewind routine for MGO access method.                              */
/***********************************************************************/
void MGOFAM::Rewind(void)
{
	Cmgp->Rewind();
} // end of Rewind

