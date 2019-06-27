/*************** TabRest H Declares Source Code File (.H) **************/
/*  Name: tabrest.h   Version 1.0                                      */
/*  (C) Copyright to the author Olivier BERTRAND          2019         */
/*  This file contains the common tabrest classes declares.            */
/***********************************************************************/
#pragma once

/***********************************************************************/
/*  Restest table.                                                     */
/***********************************************************************/
class RESTDEF : public TABDEF {         /* Table description */
public:
	// Constructor
	RESTDEF(void) { Tdp = NULL; Http = Uri = Fn = NULL; }

	// Implementation
	virtual const char *GetType(void) { return "REST"; }

	// Methods
	virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
	virtual PTDB GetTable(PGLOBAL g, MODE m);

protected:
	// Members
	PRELDEF Tdp;
	PCSZ    Http;										/* Web connection HTTP               */
	PCSZ    Uri;							      /* Web connection URI                */
	PCSZ    Fn;                     /* The intermediate file name        */
}; // end of class RESTDEF
