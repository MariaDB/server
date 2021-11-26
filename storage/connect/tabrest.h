/*************** TabRest H Declares Source Code File (.H) **************/
/*  Name: tabrest.h   Version 1.0                                      */
/*  (C) Copyright to the author Olivier BERTRAND          2019         */
/*  This file contains the common tabrest classes declares.            */
/***********************************************************************/
#pragma once

#if defined(_WIN32)
#else // !_WIN32
#define stricmp strcasecmp
#endif // !_WIN32

typedef int(__stdcall* XGETREST) (char*, bool, PCSZ, PCSZ, PCSZ);

/***********************************************************************/
/*  Functions used by REST.                                            */
/***********************************************************************/
XGETREST GetRestFunction(PGLOBAL g);
#if defined(REST_SOURCE)
extern "C" int restGetFile(char* m, bool xt, PCSZ http, PCSZ uri, PCSZ fn);
#endif   // REST_SOURCE
#if defined(MARIADB)
PQRYRES RESTColumns(PGLOBAL g, PTOS tp, char* tab, char* db, bool info);
#endif  // !MARIADB


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
