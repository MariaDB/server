/**************** mongo H Declares Source Code File (.H) ***************/
/*  Name: mongo.h   Version 1.1                                        */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2021         */
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
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
class MGODISC : public BLOCK {
public:
	// Constructor
	MGODISC(PGLOBAL g, int *lg);

	// Methods
	virtual bool Init(PGLOBAL g) { return false; }
	virtual void GetDoc(void) {}
	virtual bool Find(PGLOBAL g) = 0;

	// Functions
	int  GetColumns(PGLOBAL g, PCSZ db, PCSZ uri, PTOS topt);
	void AddColumn(PGLOBAL g, PCSZ colname, PCSZ fmt, int k);

	// Members
	BCOL    bcol;
	PBCOL   bcp, fbcp, pbcp;
	PMGODEF tdp;
	PTDB    tmgp;
	PCSZ    drv;
	int    *length;
	int     i, ncol, lvl;
	bool    all;
}; // end of MGODISC

/***********************************************************************/
/*  MongoDB table.                                                     */
/***********************************************************************/
class DllExport MGODEF : public EXTDEF {          /* Table description */
	friend class TDBCMG;
	friend class TDBJMG;
	friend class TDBGOL;
	friend class TDBJGL;
	friend class CMGFAM;
	friend class MGODISC;
	friend DllExport PQRYRES MGOColumns(PGLOBAL, PCSZ, PCSZ, PTOS, bool);
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
	PCSZ  Strfy;									/* The stringify columns               */
	int   Base;                   /* The array index base                */
	int   Version;                /* The Java driver version             */
	bool  Pipe;                   /* True is Colist is a pipeline        */
}; // end of MGODEF

#endif // __MONGO_H
