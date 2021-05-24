/***********************************************************************/
/*  CMgoConn.h : header file for the MongoDB connection classes.       */
/***********************************************************************/

/***********************************************************************/
/*  Include MongoDB library header files.                       	  	 */
/***********************************************************************/
#include <bson.h>
#include <bcon.h>
#include <mongoc.h>

// C connection to a MongoDB data source
class TDBCMG;
class MGOCOL;

/***********************************************************************/
/*  Include MongoDB library header files.                       	  	 */
/***********************************************************************/
typedef class INCOL  *PINCOL;
typedef class MGODEF *PMGODEF;
typedef class TDBCMG *PTDBCMG;
typedef class MGOCOL *PMGOCOL;

typedef struct mongo_parms {
	PTDB Tdbp;
	PCSZ Uristr;               // Driver URI
	PCSZ Db_name;
	PCSZ Coll_name;
	PCSZ Options;
	PCSZ Filter;
	PCSZ Line;
	bool Pipe;
} CMGOPARM, *PCPARM;

typedef struct KEYCOL {
	KEYCOL *Next;
	PINCOL  Incolp;
	PCOL    Colp;
	char   *Key;
	bool    Array;
} *PKC;

typedef struct _path_list *PTHP;

typedef struct _path_list {
	PSZ  Path;
	PSZ  Name;
	PTHP Next;
} PTH;

/***********************************************************************/
/*  Used when inserting values in a MongoDB collection.                */
/***********************************************************************/
class INCOL : public BLOCK {
public:
	// Constructor
	INCOL(void) { Child = bson_new(); Klist = NULL; }

	// Methods
	void AddCol(PGLOBAL g, PCOL colp, char *jp);
	void Init(void);
	void Destroy(void);

	//Members
	bson_t *Child;
	PKC     Klist;
}; // end of INCOL;

/***********************************************************************/
/*  CMgoConn class.                                                    */
/***********************************************************************/
class CMgoConn : public BLOCK {
	friend class TDBCMG;
	friend class CMGDISC;
public:
	// Constructor
	CMgoConn(PGLOBAL g, PCPARM pcg);

	//static void *mgo_alloc(size_t n);
	//static void *mgo_calloc(size_t n, size_t sz);
	//static void *mgo_realloc(void *m, size_t n);
	//static void  mgo_free(void *) {}

	// Implementation
	bool IsConnected(void) { return m_Connected; }
	bool Connect(PGLOBAL g);
	int  CollSize(PGLOBAL g);
	void CMgoConn::Project(PGLOBAL g, PSTRG s);
	bool MakeCursor(PGLOBAL g);
	int  ReadNext(PGLOBAL g);
	PSZ  GetDocument(PGLOBAL g);
	void ShowDocument(bson_iter_t *iter, const bson_t *doc, const char *k);
	void MakeColumnGroups(PGLOBAL g);
	bool DocWrite(PGLOBAL g, PINCOL icp);
	int  Write(PGLOBAL g);
	bool DocDelete(PGLOBAL g);
	void Rewind(void);
	void Close(void);
	PSZ  Mini(PGLOBAL g, PCOL colp, const bson_t *bson, bool b);
	void GetColumnValue(PGLOBAL g, PCOL colp);
	bool AddValue(PGLOBAL g, PCOL colp, bson_t *doc, char *key, bool upd);
	static void mongo_init(bool init);

protected:
	// Members
	PCPARM								Pcg;
	mongoc_uri_t         *Uri;
//mongoc_client_pool_t *Pool;				// Thread safe client pool
	mongoc_client_t      *Client;		  // The MongoDB client
	mongoc_database_t    *Database;	  // The MongoDB database
	mongoc_collection_t  *Collection; // The MongoDB collection
	mongoc_cursor_t      *Cursor;
	const bson_t         *Document;
	bson_t               *Query;			// MongoDB cursor filter
	bson_t               *Opts;			  // MongoDB cursor options
	bson_error_t          Error;
	PINCOL                Fpc;				// To insert INCOL classes
	PFBLOCK               fp;
	bool                  m_Connected;
	static bool           IsInit;
}; // end of class CMgoConn
