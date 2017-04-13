/************** MongoFam H Declares Source Code File (.H) **************/
/*  Name: mongofam.h    Version 1.3                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017         */
/*                                                                     */
/*  This file contains the MongoDB access method classes declares.     */
/***********************************************************************/
#pragma once

/***********************************************************************/
/*  Include MongoDB library header files.                       	  	 */
/***********************************************************************/
#include <bson.h>
#include <bcon.h>
#include <mongoc.h>

#include "block.h"
//#include "array.h"

typedef class TXTFAM *PTXF;
typedef class MGOFAM *PMGOFAM;
typedef class MGODEF *PMGODEF;
typedef class TDBMGO *PTDBMGO;

/***********************************************************************/
/*  This is the MongoDB Access Method class declaration.               */
/***********************************************************************/
class DllExport MGOFAM : public DOSFAM {
	friend void mongo_init(bool);
public:
	// Constructor
	MGOFAM(PJDEF tdp);
	MGOFAM(PMGOFAM txfp);

	// Implementation
	virtual AMT   GetAmType(void) { return TYPE_AM_MGO; }
	virtual bool  GetUseTemp(void) { return false; }
	virtual int   GetPos(void);
	virtual int   GetNextPos(void);
	virtual PTXF  Duplicate(PGLOBAL g) { return (PTXF)new(g) MGOFAM(this); }
	void  SetLrecl(int lrecl) { Lrecl = lrecl; }

	// Methods
	virtual void  Reset(void);
	virtual int   GetFileLength(PGLOBAL g);
	virtual int   Cardinality(PGLOBAL g);
	virtual int   MaxBlkSize(PGLOBAL g, int s);
	virtual bool  AllocateBuffer(PGLOBAL g) { return false; }
	virtual int   GetRowID(void);
	virtual bool  RecordPos(PGLOBAL g);
	virtual bool  SetPos(PGLOBAL g, int recpos);
	virtual int   SkipRecord(PGLOBAL g, bool header);
	virtual bool  OpenTableFile(PGLOBAL g);
	virtual int   ReadBuffer(PGLOBAL g);
	virtual int   WriteBuffer(PGLOBAL g);
	virtual int   DeleteRecords(PGLOBAL g, int irc);
	virtual void  CloseTableFile(PGLOBAL g, bool abort);
	virtual void  Rewind(void);

protected:
	virtual bool  OpenTempFile(PGLOBAL g) { return false; }
	virtual bool  MoveIntermediateLines(PGLOBAL g, bool *b) { return false; }
	virtual int   RenameTempFile(PGLOBAL g) { return RC_OK; }
	virtual int   InitDelete(PGLOBAL g, int fpos, int spos);
	bool  Init(PGLOBAL g);
	void  ShowDocument(bson_iter_t *i, const bson_t *b, const char *k);
	//static void *mgo_alloc(size_t n);
	//static void *mgo_calloc(size_t n, size_t sz);
	//static void *mgo_realloc(void *m, size_t n);
	//static void  mgo_free(void *) {}


	// Members
//static PGLOBAL        G;
	mongoc_uri_t         *Uri;
	mongoc_client_pool_t *Pool;				// Thread safe client pool
	mongoc_client_t      *Client;		  // The MongoDB client
  mongoc_database_t    *Database;	  // The MongoDB database
	mongoc_collection_t  *Collection; // The MongoDB collection
	mongoc_cursor_t      *Cursor;
	const bson_t         *Document;
//bson_mem_vtable_t     Vtable;
	bson_t               *Query;			// MongoDB cursor filter
	bson_t               *Opts;			  // MongoDB cursor options
	bson_error_t          Error;
	PFBLOCK               To_Fbt;     // Pointer to temp file block
	MODE                  Mode;
	const char           *uristr;
	const char           *db_name;
	const char           *coll_name;
	const char           *options;
	const char           *filter;
	bool                  Done;			  // Init done
}; // end of class MGOFAM

