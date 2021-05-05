/************ CMgoConn C++ Functions Source Code File (.CPP) ***********/
/*  Name: CMgoConn.CPP  Version 1.0                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017         */
/*                                                                     */
/*  This file contains the MongoDB C connection classes functions.     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Required objects includes.                                         */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "colblk.h"
#include "xobject.h"
#include "xtable.h"
#include "filter.h"
#include "cmgoconn.h"

bool CMgoConn::IsInit = false;

bool IsNum(PSZ s);
bool MakeSelector(PGLOBAL g, PFIL fp, PSTRG s);

/* --------------------------- Class INCOL --------------------------- */

/***********************************************************************/
/*  Add a column in the column list.                                   */
/***********************************************************************/
void INCOL::AddCol(PGLOBAL g, PCOL colp, char *jp)
{
	char *p;
	PKC   kp, kcp;

	if ((p = strchr(jp, '.'))) {
		PINCOL icp;

		*p++ = 0;

		for (kp = Klist; kp; kp = kp->Next)
			if (kp->Incolp && !strcmp(jp, kp->Key))
				break;

		if (!kp) {
			icp = new(g) INCOL(IsNum(p));
			kcp = (PKC)PlugSubAlloc(g, NULL, sizeof(KEYCOL));
			kcp->Next = NULL;
			kcp->Incolp = icp;
			kcp->Colp = NULL;
			kcp->Key = PlugDup(g, jp);

			if (Klist) {
				for (kp = Klist; kp->Next; kp = kp->Next);

				kp->Next = kcp;
			} else
				Klist = kcp;

		} else
			icp = kp->Incolp;

		*(p - 1) = '.';
		icp->AddCol(g, colp, p);
	} else {
		kcp = (PKC)PlugSubAlloc(g, NULL, sizeof(KEYCOL));

		kcp->Next = NULL;
		kcp->Incolp = NULL;
		kcp->Colp = colp;
		kcp->Key = jp;

		if (Klist) {
			for (kp = Klist; kp->Next; kp = kp->Next);

			kp->Next = kcp;
		} else
			Klist = kcp;

	} // endif jp

}	// end of AddCol

/***********************************************************************/
/*  Clear.                                                             */
/***********************************************************************/
void INCOL::Init(void)
{
	bson_init(Child);

	for (PKC kp = Klist; kp; kp = kp->Next)
		if (kp->Incolp)
			kp->Incolp->Init();

} // end of init

/***********************************************************************/
/*  Destroy.                                                           */
/***********************************************************************/
void INCOL::Destroy(void)
{
	bson_destroy(Child);

	for (PKC kp = Klist; kp; kp = kp->Next)
		if (kp->Incolp)
			kp->Incolp->Destroy();

} // end of Destroy

/* -------------------------- Class CMgoConn ------------------------- */

/***********************************************************************/
/*  Implementation of the CMgoConn class.                              */
/***********************************************************************/
CMgoConn::CMgoConn(PGLOBAL g, PCPARM pcg)
{
	Pcg = pcg;
	Uri = NULL;
	Pool = NULL;
	Client = NULL;
	Database = NULL;
	Collection = NULL;
	Cursor = NULL;
	Query = NULL;
	Opts = NULL;
	Fpc = NULL;
	fp = NULL;
	m_Connected = false;
} // end of CMgoConn standard constructor

/***********************************************************************/
/*  Required to initialize libmongoc's internals.											 */
/***********************************************************************/
void CMgoConn::mongo_init(bool init)
{
	if (init)
		mongoc_init();
	else if (IsInit)
		mongoc_cleanup();

	IsInit = init;
}	// end of mongo_init

/***********************************************************************/
/*  Connect to the MongoDB server and get the collection.              */
/***********************************************************************/
bool CMgoConn::Connect(PGLOBAL g)
{
	if (!Pcg->Db_name || !Pcg->Coll_name) {
		// This would crash in mongoc_client_get_collection
		strcpy(g->Message, "Missing DB or collection name");
		return true;
	}	// endif name

	if (!IsInit)
#if defined(__WIN__)
		__try {
		  mongo_init(true);
	  } __except (EXCEPTION_EXECUTE_HANDLER) {
		  strcpy(g->Message, "Cannot load MongoDB C driver");
		  return true;
	  }	// end try/except
#else   // !__WIN__
		mongo_init(true);
#endif  // !__WIN__

	Uri = mongoc_uri_new(Pcg->Uristr);

	if (!Uri) {
		sprintf(g->Message, "Failed to parse URI: \"%s\"", Pcg->Uristr);
		return true;
	}	// endif Uri

	// Create a new client pool instance
	Pool = mongoc_client_pool_new(Uri);
	mongoc_client_pool_set_error_api(Pool, 2);

	// Register the application name so we can track it in the profile logs
	// on the server. This can also be done from the URI.
	mongoc_client_pool_set_appname(Pool, "Connect");

	// Create a new client instance
	Client = mongoc_client_pool_pop(Pool);

	if (!Client) {
		sprintf(g->Message, "Failed to get Client");
		return true;
	}	// endif Client

	// Get a handle on the collection Coll_name
	Collection = mongoc_client_get_collection(Client, Pcg->Db_name, Pcg->Coll_name);

	if (!Collection) {
		sprintf(g->Message, "Failed to get Collection %s.%s",
			      Pcg->Db_name, Pcg->Coll_name);
		return true;
	}	// endif Collection

	/*********************************************************************/
	/*  Link a Fblock. This make possible to automatically close it      */
	/*  in case of error (throw).                                        */
	/*********************************************************************/
	PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

	fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
	fp->Type = TYPE_FB_MONGO;
	fp->Fname = NULL;
	fp->Next = dbuserp->Openlist;
	dbuserp->Openlist = fp;
	fp->Count = 1;
	fp->Length = 0;
	fp->Memory = NULL;
	fp->Mode = MODE_ANY;
	fp->File = this;
	fp->Handle = 0;

	m_Connected = true;
	return false;
} // end of Connect

/***********************************************************************/
/*  CollSize: returns the number of documents in the collection.       */
/***********************************************************************/
int CMgoConn::CollSize(PGLOBAL g)
{
	int         cnt;
	bson_t     *query;
	const char *jf = NULL;

	if (Pcg->Pipe)
		return 10;
	else if (Pcg->Filter)
		jf = Pcg->Filter;

	if (jf) {
		query = bson_new_from_json((const uint8_t *)jf, -1, &Error);

		if (!query) {
			htrc("Wrong filter: %s", Error.message);
			return 10;
		}	// endif Query

	} else
		query = bson_new();

	cnt = (int)mongoc_collection_count(Collection,
		         MONGOC_QUERY_NONE, query, 0, 0, NULL, &Error);

	if (cnt < 0) {
		htrc("Collection count: %s", Error.message);
		cnt = 2;
	} // endif Cardinal

	bson_destroy(query);
	return cnt;
} // end of CollSize

/***********************************************************************/
/*  OpenDB: Data Base open routine for MONGO access method.            */
/***********************************************************************/
bool CMgoConn::MakeCursor(PGLOBAL g)
{
	const char *p;
	bool  id, b = false, all = false;
	PCSZ  options = Pcg->Options;
	PTDB  tp = Pcg->Tdbp;
	PCOL  cp;
	PSTRG s = NULL;
	PFIL  filp = tp->GetFilter();

	id = (tp->GetMode() != MODE_READ);

	if (options && !stricmp(options, "all")) {
		options = NULL;
		all = true;
	} // endif Options

	for (cp = tp->GetColumns(); cp; cp = cp->GetNext())
		if (!strcmp(cp->GetName(), "_id"))
			id = true;
		else if (cp->GetFmt() && !strcmp(cp->GetFmt(), "*") && !options)
			all = true;

	if (Pcg->Pipe) {
		if (trace(1))
			htrc("Pipeline: %s\n", options);

		p = strrchr(options, ']');

		if (!p) {
			strcpy(g->Message, "Missing ] in pipeline");
			return true;
		} else
			*(char*)p = 0;

		s = new(g) STRING(g, 1023, (PSZ)options);

		if (filp) {
			s->Append(",{\"$match\":");

			if (MakeSelector(g, filp, s)) {
				strcpy(g->Message, "Failed making selector");
				return true;
			} else
				s->Append('}');

			tp->SetFilter(NULL);   // Not needed anymore
		} // endif To_Filter

		if (!all && tp->GetColumns()) {
			// Project list
			s->Append(",{\"$project\":{\"");

			if (!id)
				s->Append("_id\":0,\"");

			for (cp = tp->GetColumns(); cp; cp = cp->GetNext()) {
				if (b)
					s->Append(",\"");
				else
					b = true;

				s->Append(cp->GetJpath(g, true));
				s->Append("\":1");
			} // endfor cp

			s->Append("}}");
		} // endif all

		s->Append("]}");
		s->Resize(s->GetLength() + 1);
		*(char*)p = ']';		 // Restore Colist for discovery
		p = s->GetStr();

		if (trace(33))
			htrc("New Pipeline: %s\n", p);

		Query = bson_new_from_json((const uint8_t *)p, -1, &Error);

		if (!Query) {
			sprintf(g->Message, "Wrong pipeline: %s", Error.message);
			return true;
		}	// endif Query

		Cursor = mongoc_collection_aggregate(Collection, MONGOC_QUERY_NONE,
			                                   Query, NULL, NULL);

		if (mongoc_cursor_error(Cursor, &Error)) {
			sprintf(g->Message, "Mongo aggregate Failure: %s", Error.message);
			return true;
		} // endif error

	} else {
		if (Pcg->Filter || filp) {
			if (trace(1)) {
				if (Pcg->Filter)
					htrc("Filter: %s\n", Pcg->Filter);

				if (filp) {
					char buf[512];

					filp->Prints(g, buf, 511);
					htrc("To_Filter: %s\n", buf);
				} // endif To_Filter

			}	// endif trace

			s = new(g) STRING(g, 1023, (PSZ)Pcg->Filter);

			if (filp) {
				if (Pcg->Filter)
					s->Append(',');

				if (MakeSelector(g, filp, s)) {
					strcpy(g->Message, "Failed making selector");
					return NULL;
				}	// endif Selector

				tp->SetFilter(NULL);   // Not needed anymore
			} // endif To_Filter

			if (trace(33))
				htrc("selector: %s\n", s->GetStr());

			s->Resize(s->GetLength() + 1);
			Query = bson_new_from_json((const uint8_t *)s->GetStr(), -1, &Error);

			if (!Query) {
				sprintf(g->Message, "Wrong filter: %s", Error.message);
				return NULL;
			}	// endif Query

		} else
			Query = bson_new();

		if (!all) {
			if (options && *options) {
				if (trace(1))
					htrc("options=%s\n", options);

				p = options;
			} else if (tp->GetColumns()) {
				// Projection list
				if (s)
					s->Set("{\"projection\":{\"");
				else
					s = new(g) STRING(g, 511, "{\"projection\":{\"");

				if (!id)
					s->Append("_id\":0,\"");

				for (cp = tp->GetColumns(); cp; cp = cp->GetNext()) {
					if (b)
						s->Append(",\"");
					else
						b = true;

					s->Append(cp->GetJpath(g, true));
					s->Append("\":1");
				} // endfor cp

				s->Append("}}");
				s->Resize(s->GetLength() + 1);
				p = s->GetStr();
			} else {
				// count(*)	?
				p = "{\"projection\":{\"_id\":1}}";
			} // endif Options

			Opts = bson_new_from_json((const uint8_t *)p, -1, &Error);

			if (!Opts) {
				sprintf(g->Message, "Wrong options: %s", Error.message);
				return NULL;
			} // endif Opts

		} // endif all

		Cursor = mongoc_collection_find_with_opts(Collection, Query, Opts, NULL);
	} // endif Pipe

	return false;
} // end of MakeCursor

/***********************************************************************/
/*  Fetch next document.                                               */
/***********************************************************************/
int CMgoConn::ReadNext(PGLOBAL g)
{
	int rc = RC_OK;

	if (!Cursor && MakeCursor(g)) {
		rc = RC_FX;
	} else if (mongoc_cursor_next(Cursor, &Document)) {
		if (trace(512)) {
			bson_iter_t iter;
			ShowDocument(&iter, Document, "");
		} else if (trace(1))
			htrc("%s\n", GetDocument(g));

	} else if (mongoc_cursor_error(Cursor, &Error)) {
		sprintf(g->Message, "Mongo Cursor Failure: %s", Error.message);
		rc = RC_FX;
	} else
		rc = RC_EF;

	return rc;
} // end of Fetch

/***********************************************************************/
/*  Get the Json string of the current document.                       */
/***********************************************************************/
PSZ CMgoConn::GetDocument(PGLOBAL g)
{
	char *str = bson_as_json(Document, NULL);
	PSZ   doc = PlugDup(g, str);

	bson_free(str);
	return doc;
} // end of GetDocument

/***********************************************************************/
/*  Use to trace restaurants document contains.                        */
/***********************************************************************/
void CMgoConn::ShowDocument(bson_iter_t *iter, const bson_t *doc, const char *k)
{
	if (!doc || bson_iter_init(iter, doc)) {
		const char *key;

		while (bson_iter_next(iter)) {
			key = bson_iter_key(iter);
			htrc("Found element key: \"%s\"\n", key);

			if (BSON_ITER_HOLDS_UTF8(iter))
				htrc("%s.%s=\"%s\"\n", k, key, bson_iter_utf8(iter, NULL));
			else if (BSON_ITER_HOLDS_INT32(iter))
				htrc("%s.%s=%d\n", k, key, bson_iter_int32(iter));
			else if (BSON_ITER_HOLDS_INT64(iter))
				htrc("%s.%s=%lld\n", k, key, bson_iter_int64(iter));
			else if (BSON_ITER_HOLDS_DOUBLE(iter))
				htrc("%s.%s=%g\n", k, key, bson_iter_double(iter));
			else if (BSON_ITER_HOLDS_DATE_TIME(iter))
				htrc("%s.%s=date(%lld)\n", k, key, bson_iter_date_time(iter));
			else if (BSON_ITER_HOLDS_OID(iter)) {
				char str[25];

				bson_oid_to_string(bson_iter_oid(iter), str);
				htrc("%s.%s=%s\n", k, key, str);
			} else if (BSON_ITER_HOLDS_DECIMAL128(iter)) {
				char             *str = NULL;
				bson_decimal128_t dec;

				bson_iter_decimal128(iter, &dec);
				bson_decimal128_to_string(&dec, str);
				htrc("%s.%s=%s\n", k, key, str);
			} else if (BSON_ITER_HOLDS_DOCUMENT(iter)) {
				bson_iter_t child;

				if (bson_iter_recurse(iter, &child))
					ShowDocument(&child, NULL, key);

			} else if (BSON_ITER_HOLDS_ARRAY(iter)) {
				bson_t				*arr;
				bson_iter_t    itar;
				const uint8_t *data = NULL;
				uint32_t       len = 0;

				bson_iter_array(iter, &len, &data);
				arr = bson_new_from_data(data, len);
				ShowDocument(&itar, arr, key);
			}	// endif's

		}	// endwhile bson_iter_next

	} // endif bson_iter_init

} // end of ShowDocument

/***********************************************************************/
/*  Group columns for inserting or updating.                           */
/***********************************************************************/
void CMgoConn::MakeColumnGroups(PGLOBAL g)
{
	Fpc = new(g) INCOL(false);

	for (PCOL colp = Pcg->Tdbp->GetColumns(); colp; colp = colp->GetNext())
		if (!colp->IsSpecial())
			Fpc->AddCol(g, colp, colp->GetJpath(g, false));

} // end of MakeColumnGroups

/***********************************************************************/
/*  DocWrite.                                                          */
/***********************************************************************/
bool CMgoConn::DocWrite(PGLOBAL g, PINCOL icp)
{
	for (PKC kp = icp->Klist; kp; kp = kp->Next)
		if (kp->Incolp) {
			bool isdoc = !kp->Incolp->Array;

			if (isdoc)
				BSON_APPEND_DOCUMENT_BEGIN(icp->Child, kp->Key, kp->Incolp->Child);
			else
				BSON_APPEND_ARRAY_BEGIN(icp->Child, kp->Key, kp->Incolp->Child);

			if (DocWrite(g, kp->Incolp))
				return true;

			if (isdoc)
				bson_append_document_end(icp->Child, kp->Incolp->Child);
			else
				bson_append_array_end(icp->Child, kp->Incolp->Child);

		} else if (AddValue(g, kp->Colp, icp->Child, kp->Key, false))
			return true;

		return false;
} // end of DocWrite

/***********************************************************************/
/*  WriteDB: Data Base write routine for DOS access method.            */
/***********************************************************************/
int CMgoConn::Write(PGLOBAL g)
{
	int  rc = RC_OK;
	PTDB tp = Pcg->Tdbp;

	if (tp->GetMode() == MODE_INSERT) {
  	Fpc->Init();

		if (DocWrite(g, Fpc))
			return RC_FX;

		if (trace(2)) {
			char *str = bson_as_json(Fpc->Child, NULL);
			htrc("Inserting: %s\n", str);
			bson_free(str);
		} // endif trace

		if (!mongoc_collection_insert(Collection, MONGOC_INSERT_NONE,
			Fpc->Child, NULL, &Error)) {
			sprintf(g->Message, "Mongo insert: %s", Error.message);
			rc = RC_FX;
		} // endif insert

	} else {
		bool        b = false;
		bson_iter_t iter;
		bson_t     *query = bson_new();

		bson_iter_init(&iter, Document);

		if (bson_iter_find(&iter, "_id")) {
			if (BSON_ITER_HOLDS_OID(&iter))
				b = BSON_APPEND_OID(query, "_id", bson_iter_oid(&iter));
			else if (BSON_ITER_HOLDS_INT32(&iter))
				b = BSON_APPEND_INT32(query, "_id", bson_iter_int32(&iter));
			else if (BSON_ITER_HOLDS_INT64(&iter))
				b = BSON_APPEND_INT64(query, "_id", bson_iter_int64(&iter));
			else if (BSON_ITER_HOLDS_DOUBLE(&iter))
				b = BSON_APPEND_DOUBLE(query, "_id", bson_iter_double(&iter));
			else if (BSON_ITER_HOLDS_UTF8(&iter))
				b = BSON_APPEND_UTF8(query, "_id", bson_iter_utf8(&iter, NULL));

		} // endif iter

		if (b) {
			if (trace(2)) {
				char *str = bson_as_json(query, NULL);
				htrc("update query: %s\n", str);
				bson_free(str);
			}	// endif trace

			if (tp->GetMode() == MODE_UPDATE) {
				bson_t  child;
				bson_t *update = bson_new();

				BSON_APPEND_DOCUMENT_BEGIN(update, "$set", &child);

				for (PCOL colp = tp->GetSetCols(); colp; colp = colp->GetNext())
					if (AddValue(g, colp, &child, colp->GetJpath(g, false), true))
						rc = RC_FX;

				bson_append_document_end(update, &child);

				if (rc == RC_OK)
					if (!mongoc_collection_update(Collection, MONGOC_UPDATE_NONE,
						query, update, NULL, &Error)) {
						sprintf(g->Message, "Mongo update: %s", Error.message);
						rc = RC_FX;
					} // endif update

				bson_destroy(update);
			} else if (!mongoc_collection_remove(Collection,
				MONGOC_REMOVE_SINGLE_REMOVE, query, NULL, &Error)) {
				sprintf(g->Message, "Mongo delete: %s", Error.message);
				rc = RC_FX;
			} // endif remove

		} else {
			strcpy(g->Message, "Mongo update: cannot find _id");
			rc = RC_FX;
		}	// endif b

		bson_destroy(query);
	} // endif Mode

	return rc;
} // end of Write

/***********************************************************************/
/*  Remove all documents from the collection.                          */
/***********************************************************************/
bool CMgoConn::DocDelete(PGLOBAL g)
{
	Query = bson_new();

	if (!mongoc_collection_remove(Collection, MONGOC_REMOVE_NONE,
		                            Query, NULL, &Error)) {
		sprintf(g->Message, "Mongo remove all: %s", Error.message);
		return true;
	}	// endif remove

	return false;
} // end of DocDelete

/***********************************************************************/
/*  Rewind the collection.                                             */
/***********************************************************************/
void CMgoConn::Rewind(void)
{
	mongoc_cursor_t *cursor = mongoc_cursor_clone(Cursor);

	mongoc_cursor_destroy(Cursor);
	Cursor = cursor;
} // end of Rewind

/***********************************************************************/
/*  Table close routine for MONGO tables.                              */
/***********************************************************************/
void CMgoConn::Close(void)
{
	if (Query) bson_destroy(Query);
	if (Opts) bson_destroy(Opts);
	if (Cursor)	mongoc_cursor_destroy(Cursor);
	if (Collection) mongoc_collection_destroy(Collection);
	if (Client) mongoc_client_pool_push(Pool, Client);
	if (Pool) mongoc_client_pool_destroy(Pool);
	if (Uri) mongoc_uri_destroy(Uri);
	if (Fpc) Fpc->Destroy();
	if (fp) fp->Count = 0;
} // end of Close

/***********************************************************************/
/*  Mini: used to suppress blanks to json strings.                     */
/***********************************************************************/
char *CMgoConn::Mini(PGLOBAL g, PCOL colp, const bson_t *bson, bool b)
{
	char *s, *str = NULL;
	char *Mbuf = (char*)PlugSubAlloc(g, NULL, colp->GetLength() + 1);
	int   i, k = 0;
	bool  ok = true;

	if (b)
		s = str = bson_array_as_json(bson, NULL);
	else
		s = str = bson_as_json(bson, NULL);

	for (i = 0; i < colp->GetLength() && s[i]; i++) {
		switch (s[i]) {
			case ' ':
				if (ok) continue;
			case '"':
				ok = !ok;
			default:
				break;
		} // endswitch s[i]

		Mbuf[k++] = s[i];
	} // endfor i

	bson_free(str);

	if (i >= colp->GetLength()) {
		sprintf(g->Message, "Value too long for column %s", colp->GetName());
		throw (int)TYPE_AM_MGO;
	}	// endif i

	Mbuf[k] = 0;
	return Mbuf;
} // end of Mini

/***********************************************************************/
/*  Retrieve the column value from the document.                       */
/***********************************************************************/
void CMgoConn::GetColumnValue(PGLOBAL g, PCOL colp)
{
	char *jpath = colp->GetJpath(g, false);
	PVAL  value = colp->GetValue();

	if (!strcmp(jpath, "*")) {
		value->SetValue_psz(Mini(g, colp, Document, false));
	} else if (bson_iter_init(&Iter, Document) &&
		         bson_iter_find_descendant(&Iter, jpath, &Desc)) {
		if (BSON_ITER_HOLDS_UTF8(&Desc))
			value->SetValue_psz((PSZ)bson_iter_utf8(&Desc, NULL));
		else if (BSON_ITER_HOLDS_INT32(&Desc))
			value->SetValue(bson_iter_int32(&Desc));
		else if (BSON_ITER_HOLDS_INT64(&Desc))
			value->SetValue(bson_iter_int64(&Desc));
		else if (BSON_ITER_HOLDS_DOUBLE(&Desc))
			value->SetValue(bson_iter_double(&Desc));
		else if (BSON_ITER_HOLDS_DATE_TIME(&Desc))
			value->SetValue(bson_iter_date_time(&Desc) / 1000);
		else if (BSON_ITER_HOLDS_BOOL(&Desc)) {
			bool b = bson_iter_bool(&Desc);

			if (value->IsTypeNum())
				value->SetValue(b ? 1 : 0);
			else
				value->SetValue_psz(b ? "true" : "false");

		} else if (BSON_ITER_HOLDS_OID(&Desc)) {
			char str[25];

			bson_oid_to_string(bson_iter_oid(&Desc), str);
			value->SetValue_psz(str);
		} else if (BSON_ITER_HOLDS_NULL(&Iter)) {
			// Apparently this does not work...
			value->Reset();
			value->SetNull(true);
		} else if (BSON_ITER_HOLDS_DECIMAL128(&Desc)) {
			char *str = NULL;
			bson_decimal128_t dec;

			bson_iter_decimal128(&Desc, &dec);
			bson_decimal128_to_string(&dec, str);
			value->SetValue_psz(str);
			bson_free(str);
		} else if (BSON_ITER_HOLDS_DOCUMENT(&Iter)) {
			bson_t				*doc;
			const uint8_t *data = NULL;
			uint32_t       len = 0;

			bson_iter_document(&Desc, &len, &data);

			if (data) {
				doc = bson_new_from_data(data, len);
				value->SetValue_psz(Mini(g, colp, doc, false));
				bson_destroy(doc);
			} else {
				// ... but we can come here in case of NULL!
				value->Reset();
				value->SetNull(true);
			} // endif data

		} else if (BSON_ITER_HOLDS_ARRAY(&Iter)) {
			bson_t				*arr;
			const uint8_t *data = NULL;
			uint32_t       len = 0;

			bson_iter_array(&Desc, &len, &data);

			if (data) {
				arr = bson_new_from_data(data, len);
				value->SetValue_psz(Mini(g, colp, arr, true));
				bson_destroy(arr);
			} else {
				// This is a bug in returning the wrong type
				// This fix is only for document items
				bson_t *doc;

				bson_iter_document(&Desc, &len, &data);

				if (data) {
					doc = bson_new_from_data(data, len);
					value->SetValue_psz(Mini(g, colp, doc, false));
					bson_destroy(doc);
				} else {
					// ... or we can also come here in case of NULL!
					value->Reset();
					value->SetNull(true);
				}	// endif data

			} // endif data

		} else
			value->Reset();

	} else {
		// Field does not exist
		value->Reset();
		value->SetNull(true);
	} // endif Iter

} // end of GetColumnValue

/***********************************************************************/
/*  AddValue: Add column value to the document to insert or update.    */
/***********************************************************************/
bool CMgoConn::AddValue(PGLOBAL g, PCOL colp, bson_t *doc, char *key, bool upd)
{
	bool rc = false;
	PVAL value = colp->GetValue();

	if (value->IsNull()) {
		if (upd)
			rc = BSON_APPEND_NULL(doc, key);
		else
			return false;

	} else switch (colp->GetResultType()) {
		case TYPE_STRING:
			rc = BSON_APPEND_UTF8(doc, key, value->GetCharValue());
			break;
		case TYPE_INT:
		case TYPE_SHORT:
			rc = BSON_APPEND_INT32(doc, key, value->GetIntValue());
			break;
		case TYPE_TINY:
			rc = BSON_APPEND_BOOL(doc, key, value->GetIntValue());
			break;
		case TYPE_BIGINT:
			rc = BSON_APPEND_INT64(doc, key, value->GetBigintValue());
			break;
		case TYPE_DOUBLE:
			rc = BSON_APPEND_DOUBLE(doc, key, value->GetFloatValue());
			break;
		case TYPE_DECIM:
		{bson_decimal128_t dec;

		if (bson_decimal128_from_string(value->GetCharValue(), &dec))
			rc = BSON_APPEND_DECIMAL128(doc, key, &dec);

		} break;
		case TYPE_DATE:
			rc = BSON_APPEND_DATE_TIME(doc, key, value->GetBigintValue() * 1000);
			break;
		default:
			sprintf(g->Message, "Type %d not supported yet", colp->GetResultType());
			return true;
	} // endswitch Buf_Type

	if (!rc) {
		strcpy(g->Message, "Adding value failed");
		return true;
	} else
		return false;

} // end of AddValue

#if 0
void *CMgoConn::mgo_alloc(size_t n)
{
	char *mst = (char*)PlgDBSubAlloc(G, NULL, n + sizeof(size_t));

	if (mst) {
		*(size_t*)mst = n;
		return mst + sizeof(size_t);
	} // endif mst

	return NULL;
} // end of mgo_alloc

void *CMgoConn::mgo_calloc(size_t n, size_t sz)
{
	void *m = mgo_alloc(n * sz);

	if (m)
		memset(m, 0, n * sz);

	return m;
} // end of mgo_calloc

void *CMgoConn::mgo_realloc(void *m, size_t n)
{
	if (!m)
		return n ? mgo_alloc(n) : NULL;

	size_t *osz = (size_t*)((char*)m - sizeof(size_t));

	if (n > *osz) {
		void *nwm = mgo_alloc(n);

		if (nwm)
			memcpy(nwm, m, *osz);

		return nwm;
	} else {
		*osz = n;
		return m;
	}	// endif n

} // end of mgo_realloc
#endif // 0

