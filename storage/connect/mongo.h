/**************** mongo H Declares Source Code File (.H) ***************/
/*  Name: mongo.h   Version 1.0                                        */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017         */
/*                                                                     */
/*  This file contains the common MongoDB classes declares.            */
/***********************************************************************/
#ifndef __MONGO_H
#define __MONGO_H

#include "osutil.h"
#include "block.h"
#include "colblk.h"

typedef class MGODEF *PMGODEF;

typedef struct _bncol {
	struct _bncol *Next;
	char *Name;
	char *Fmt;
	int   Type;
	int   Len;
	int   Scale;
	bool  Cbn;
	bool  Found;
} BCOL, *PBCOL;

/***********************************************************************/
/*  MongoDB table.                                                     */
/***********************************************************************/
class DllExport MGODEF : public EXTDEF {          /* Table description */
	friend class TDBMGO;
	friend class TDBJMG;
	friend class TDBGOL;
	friend class MGOFAM;
	friend class MGODISC;
	friend PQRYRES MGOColumns(PGLOBAL, PCSZ, PCSZ, PTOS, bool);
public:
	// Constructor
	MGODEF(void);

	// Implementation
	virtual const char *GetType(void) { return "MONGO"; }

	// Methods
	virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
	virtual PTDB GetTable(PGLOBAL g, MODE m);

protected:
	// Members
	PCSZ  Driver;						      /* MongoDB Driver (C or JAVA)          */
	PCSZ  Uri;							      /* MongoDB connection URI              */
	PSZ   Wrapname;               /* Java wrapper name                   */
	PCSZ  Colist;                 /* Options list                        */
	PCSZ  Filter;									/* Filtering query                     */
	int   Level;                  /* Used for catalog table              */
	int   Base;                   /* The array index base                */
	int   Version;                /* The Java driver version             */
	bool  Pipe;                   /* True is Colist is a pipeline        */
}; // end of MGODEF

#endif // __MONGO_H
