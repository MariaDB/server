/************ JMgoConn C++ Functions Source Code File (.CPP) ***********/
/*  Name: JMgoConn.CPP  Version 1.1                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017         */
/*                                                                     */
/*  This file contains the MongoDB Java connection classes functions.  */
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
#include "jmgoconn.h"

#define nullptr 0

bool IsNum(PSZ s);
bool MakeSelector(PGLOBAL g, PFIL fp, PSTRG s);

/* --------------------------- Class JNCOL --------------------------- */

/***********************************************************************/
/*  Add a column in the column list.                                   */
/***********************************************************************/
void JNCOL::AddCol(PGLOBAL g, PCOL colp, PSZ jp)
{
	char *p;
	PJKC  kp, kcp;

	if ((p = strchr(jp, '.'))) {
		PJNCOL icp;

		*p++ = 0;

		for (kp = Klist; kp; kp = kp->Next)
			if (kp->Jncolp && !strcmp(jp, kp->Key))
				break;

		if (!kp) {
			icp = new(g) JNCOL(IsNum(p));
			kcp = (PJKC)PlugSubAlloc(g, NULL, sizeof(JKCOL));
			kcp->Next = NULL;
			kcp->Jncolp = icp;
			kcp->Colp = NULL;

			if (Array) {
				kcp->Key = NULL;
				kcp->N = atoi(p);
			} else {
				kcp->Key = PlugDup(g, jp);
				kcp->N = 0;
			} // endif Array

			if (Klist) {
				for (kp = Klist; kp->Next; kp = kp->Next);

				kp->Next = kcp;
			} else
				Klist = kcp;

		} else
			icp = kp->Jncolp;

		*(p - 1) = '.';
		icp->AddCol(g, colp, p);
	} else {
		kcp = (PJKC)PlugSubAlloc(g, NULL, sizeof(JKCOL));

		kcp->Next = NULL;
		kcp->Jncolp = NULL;
		kcp->Colp = colp;

		if (Array) {
			kcp->Key = NULL;
			kcp->N = atoi(jp);
		} else {
			kcp->Key = jp;
			kcp->N = 0;
		} // endif Array

		if (Klist) {
			for (kp = Klist; kp->Next; kp = kp->Next);

			kp->Next = kcp;
		} else
			Klist = kcp;

	} // endif jp

}	// end of AddCol

/***********************************************************************/
/*  JMgoConn construction/destruction.                                 */
/***********************************************************************/
JMgoConn::JMgoConn(PGLOBAL g, PCSZ collname, PCSZ wrapper)
	       : JAVAConn(g, wrapper)
{
	CollName = collname;
	readid = fetchid = getdocid = objfldid = fcollid = acollid =
	mkdocid = docaddid = mkarid = araddid = insertid = updateid =
	deleteid = gcollid =	countid =	rewindid = nullptr;
	DiscFunc = "MongoDisconnect";
	Fpc = NULL;
	m_Fetch = 0;
	m_Ncol = 0;
	m_Version = 0;
} // end of JMgoConn

/***********************************************************************/
/*  AddJars: add some jar file to the Class path.                      */
/***********************************************************************/
void JMgoConn::AddJars(PSTRG jpop, char sep)
{
#if defined(BSON_SUPPORT)
	if (m_Version == 2) {
		jpop->Append(sep);
//	jpop->Append("C:/Eclipse/workspace/MongoWrap2/bin");
		jpop->Append(sep);
		jpop->Append("C:/mongo-java-driver/mongo-java-driver-2.13.3.jar");
	} else {
		jpop->Append(sep);
//	jpop->Append("C:/Eclipse/workspace/MongoWrap3/bin");
//	jpop->Append("C:/Program Files/MariaDB 10.1/lib/plugin/JavaWrappers.jar");
		jpop->Append(sep);
		jpop->Append("C:/mongo-java-driver/mongo-java-driver-3.4.2.jar");
	} // endif m_Version
#endif   // BSON_SUPPORT
} // end of AddJars

/***********************************************************************/
/*  Connect: connect to a data source.                                 */
/***********************************************************************/
bool JMgoConn::Connect(PJPARM sop)
{
	bool		 err = false;
	jint     rc;
	jboolean brc;
	jstring  cln;
	PGLOBAL& g = m_G;

	m_Version = sop->Version;

	/*******************************************************************/
	/*  Create or attach a JVM. 																			 */
	/*******************************************************************/
	if (Open(g))
		return true;

	/*******************************************************************/
	/*  Connect to MongoDB.      																			 */
	/*******************************************************************/
	jmethodID cid = nullptr;

	if (gmID(g, cid, "MongoConnect", "([Ljava/lang/String;)I"))
		return true;

	// Build the java string array
	jobjectArray parms = env->NewObjectArray(4,    // constructs java array of 4
		env->FindClass("java/lang/String"), NULL);   // Strings

	//m_Scrollable = sop->Scrollable;
	//m_RowsetSize = sop->Fsize;

	// change some elements
	if (sop->Driver)
		env->SetObjectArrayElement(parms, 0, env->NewStringUTF(sop->Url));

	if (sop->Url)
		env->SetObjectArrayElement(parms, 1, env->NewStringUTF(sop->Driver));

	if (sop->User)
		env->SetObjectArrayElement(parms, 2, env->NewStringUTF(sop->User));

	if (sop->Pwd)
		env->SetObjectArrayElement(parms, 3, env->NewStringUTF(sop->Pwd));

	// call method
	rc = env->CallIntMethod(job, cid, parms);
	err = Check(rc);
	env->DeleteLocalRef(parms);				 	// Not used anymore

	if (err) {
		sprintf(g->Message, "Connecting: %s rc=%d", Msg, (int)rc);
		return true;
	}	// endif Msg

	/*********************************************************************/
	/*  Get the collection.                                              */
	/*********************************************************************/
	if (gmID(g, gcollid, "GetCollection", "(Ljava/lang/String;)Z"))
		return true;

	cln = env->NewStringUTF(CollName);
	brc = env->CallBooleanMethod(job, gcollid, cln);
	env->DeleteLocalRef(cln);

	if (Check(brc ? -1 : 0)) {
		sprintf(g->Message, "GetCollection: %s", Msg);
		return true;
	}	// endif Msg

	m_Connected = true;
	return false;
} // end of Connect

/***********************************************************************/
/*  CollSize: returns the number of documents in the collection.       */
/***********************************************************************/
int JMgoConn::CollSize(PGLOBAL g)
{
	if (!gmID(g, countid, "GetCollSize", "()J")) {
		jlong card = env->CallLongMethod(job, countid);

		return (int)card;
	} else
		return 2;				 // Make MariaDB happy

} // end of CollSize

/***********************************************************************/
/*  OpenDB: Data Base open routine for MONGO access method.            */
/***********************************************************************/
bool JMgoConn::MakeCursor(PGLOBAL g, PTDB tdbp, PCSZ options,
	                                               PCSZ filter, bool pipe)
{
	const char *p;
	bool  b = false, id = (tdbp->GetMode() != MODE_READ), all = false;
	uint  len;
	PCOL  cp;
	PSZ   jp;
	PCSZ  op = NULL, sf = NULL, Options = options;
	PSTRG s = NULL;
	PFIL  filp = tdbp->GetFilter();

	if (Options && !stricmp(Options, "all")) {
		Options = NULL;
		all = true;
	} // endif Options

	for (cp = tdbp->GetColumns(); cp; cp = cp->GetNext())
		if (!strcmp(cp->GetName(), "_id"))
			id = true;
		else if (cp->GetFmt() && !strcmp(cp->GetFmt(), "*") && (!Options || pipe))
			all = true;

	if (pipe && Options) {
		if (trace(1))
			htrc("Pipeline: %s\n", Options);

		p = strrchr(Options, ']');

		if (!p) {
			strcpy(g->Message, "Missing ] in pipeline");
			return true;
		} else
			*(char*)p = 0;

		s = new(g) STRING(g, 1023, (PSZ)Options);

		if (filp) {
			s->Append(",{\"$match\":");

			if (MakeSelector(g, filp, s)) {
				strcpy(g->Message, "Failed making selector");
				return true;
			} else
				s->Append('}');

			tdbp->SetFilter(NULL);     // Not needed anymore
		} // endif To_Filter

		if (!all && tdbp->GetColumns()) {
			// Project list
			len = s->GetLength();
			s->Append(",{\"$project\":{\"");

			if (!id)
				s->Append("_id\":0,\"");

			for (PCOL cp = tdbp->GetColumns(); cp; cp = cp->GetNext()) {
				if (b)
					s->Append(",\"");
				else
					b = true;

				if ((jp = cp->GetJpath(g, true)))
					s->Append(jp);
				else {
					s->Truncate(len);
					goto nop;
				}	// endif Jpath

				s->Append("\":1");
			} // endfor cp

			s->Append("}}");
		} // endif all

	nop:
		s->Append("]}");
		s->Resize(s->GetLength() + 1);
		*(char*)p = ']';		 // Restore Colist for discovery
		p = s->GetStr();

		if (trace(33))
			htrc("New Pipeline: %s\n", p);

		return AggregateCollection(p);
	} else {
		if (filter || filp) {
			if (trace(1)) {
				if (filter)
					htrc("Filter: %s\n", filter);

				if (filp) {
					char buf[512];

					filp->Prints(g, buf, 511);
					htrc("To_Filter: %s\n", buf);
				} // endif To_Filter

			}	// endif trace

			s = new(g) STRING(g, 1023, (PSZ)filter);
			len = s->GetLength();

			if (filp) {
				if (filter)
					s->Append(',');

				if (MakeSelector(g, filp, s)) {
					strcpy(g->Message, "Failed making selector");
					return true;
				}	// endif Selector

				tdbp->SetFilter(NULL);     // Not needed anymore
			} // endif To_Filter

			if (trace(33))
				htrc("selector: %s\n", s->GetStr());

			s->Resize(s->GetLength() + 1);
			sf = PlugDup(g, s->GetStr());
		} // endif Filter

		if (!all) {
			if (Options && *Options) {
				if (trace(1))
					htrc("options=%s\n", Options);

				op = Options;
			} else if (tdbp->GetColumns()) {
				// Projection list
				if (s)
					s->Set("{\"");
				else
					s = new(g) STRING(g, 511, "{\"");

				if (!id)
					s->Append("_id\":0,\"");

				for (PCOL cp = tdbp->GetColumns(); cp; cp = cp->GetNext()) {
					if (b)
						s->Append(",\"");
					else
						b = true;

					if ((jp = cp->GetJpath(g, true)))
						s->Append(jp);
					else {
						// Can this happen?
						htrc("Fail getting projection path of %s\n", cp->GetName());
						goto nope;
					}	// endif Jpath

					s->Append("\":1");
				} // endfor cp

				s->Append("}");
				s->Resize(s->GetLength() + 1);
				op = s->GetStr();
			} else {
				// count(*)	?
				op = "{\"_id\":1}";
			} // endif Options

		} // endif all

	nope:
		return FindCollection(sf, op);
	} // endif Pipe

} // end of MakeCursor

/***********************************************************************/
/*  Find a collection and make cursor.                                 */
/***********************************************************************/
bool JMgoConn::FindCollection(PCSZ query, PCSZ proj)
{
	bool		 rc = true;
	jboolean brc;
	jstring  qry = nullptr, prj = nullptr;
	PGLOBAL& g = m_G;

	// Get the methods used to execute a query and get the result
	if (!gmID(g, fcollid, "FindColl",	"(Ljava/lang/String;Ljava/lang/String;)Z")) {
		if (query)
			qry = env->NewStringUTF(query);

		if (proj)
			prj = env->NewStringUTF(proj);

		brc = env->CallBooleanMethod(job, fcollid, qry, prj);

		if (!Check(brc ? -1 : 0)) {
			rc = false;
		} else
			sprintf(g->Message, "FindColl: %s", Msg);

		if (query)
			env->DeleteLocalRef(qry);

		if (proj)
			env->DeleteLocalRef(prj);

	} // endif xqid

	return rc;
} // end of FindCollection

/***********************************************************************/
/*  Find a collection and make cursor.                                 */
/***********************************************************************/
bool JMgoConn::AggregateCollection(PCSZ pipeline)
{
	bool		 rc = true;
	jboolean brc;
	jstring  pip = nullptr;
	PGLOBAL& g = m_G;

	// Get the methods used to execute a query and get the result
	if (!gmID(g, acollid, "AggregateColl", "(Ljava/lang/String;)Z")) {
		pip = env->NewStringUTF(pipeline);

		brc = env->CallBooleanMethod(job, acollid, pip);

		if (!Check(brc ? -1 : 0)) {
			rc = false;
		} else
			sprintf(g->Message, "AggregateColl: %s", Msg);

		env->DeleteLocalRef(pip);
	} // endif acollid

	return rc;
} // end of AggregateCollection

/***********************************************************************/
/*  Fetch next row.                                                    */
/***********************************************************************/
int JMgoConn::Fetch(int pos)
{
	jint     rc = JNI_ERR;
	PGLOBAL& g = m_G;

	//if (m_Full)						// Result set has one row
	//	return 1;

	//if (pos) {
	//	if (!m_Scrollable) {
	//		strcpy(g->Message, "Cannot fetch(pos) if FORWARD ONLY");
	//		return rc;
	//	} else if (gmID(m_G, fetchid, "Fetch", "(I)Z"))
	//		return rc;

	//	if (env->CallBooleanMethod(job, fetchid, pos))
	//		rc = m_Rows;

	//} else {
		if (gmID(g, readid, "ReadNext", "()I"))
			return (int)rc;

		rc = env->CallIntMethod(job, readid);

		if (!Check(rc)) {
			//if (rc == 0)
			//	m_Full = (m_Fetch == 1);
			//else
			//	m_Fetch++;

			m_Ncol = (int)rc;
			rc = MY_MIN(rc, 1);
			m_Rows += rc;
		} else
			sprintf(g->Message, "Fetch: %s", Msg);

	//} // endif pos

	return rc;
} // end of Fetch

/***********************************************************************/
/*  Get the Json string of the current document.                       */
/***********************************************************************/
PSZ JMgoConn::GetDocument(void)
{
	PGLOBAL& g = m_G;
	PSZ      doc = NULL;
	jstring  jdc;

	if (!gmID(g, getdocid, "GetDoc", "()Ljava/lang/String;")) {
		jdc = (jstring)env->CallObjectMethod(job, getdocid);

		if (jdc)
			doc = (PSZ)GetUTFString(jdc);

	} // endif getdocid

	return doc;
	} // end of GetDocument

/***********************************************************************/
/*  Group columns for inserting or updating.                           */
/***********************************************************************/
void JMgoConn::MakeColumnGroups(PGLOBAL g, PTDB tdbp)
{
	Fpc = new(g) JNCOL(false);

	for (PCOL colp = tdbp->GetColumns(); colp; colp = colp->GetNext())
		if (!colp->IsSpecial())
			Fpc->AddCol(g, colp, colp->GetJpath(g, false));

} // end of MakeColumnGroups

/***********************************************************************/
/*  Get additional method ID.                                          */
/***********************************************************************/
bool JMgoConn::GetMethodId(PGLOBAL g, MODE mode)
{
	if (mode == MODE_UPDATE) {
		if (gmID(g, mkdocid, "MakeDocument", "()Ljava/lang/Object;"))
			return true;

		if (gmID(g, docaddid, "DocAdd",
			"(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)Z"))
			return true;

		if (gmID(g, updateid, "CollUpdate", "(Ljava/lang/Object;)J"))
			return true;

	} else if (mode == MODE_INSERT) {
		if (gmID(g, mkdocid, "MakeDocument", "()Ljava/lang/Object;"))
			return true;

		if (gmID(g, docaddid, "DocAdd",
			"(Ljava/lang/Object;Ljava/lang/String;Ljava/lang/Object;)Z"))
			return true;

		if (gmID(g, mkarid, "MakeArray", "()Ljava/lang/Object;"))
			return true;

		if (gmID(g, araddid, "ArrayAdd", "(Ljava/lang/Object;ILjava/lang/Object;)Z"))
			return true;

		if (gmID(g, insertid, "CollInsert", "(Ljava/lang/Object;)Z"))
			return true;

	} else if (mode == MODE_DELETE)
		if (gmID(g, deleteid, "CollDelete", "(Z)J"))
			return true;

	return gmID(g, rewindid, "Rewind", "()Z");
} // end of GetMethodId

/***********************************************************************/
/*  MakeObject.                                                        */
/***********************************************************************/
jobject JMgoConn::MakeObject(PGLOBAL g, PCOL colp, bool&error )
{
	jclass    cls;
	jmethodID cns = nullptr;								// Constructor
	jobject   val = nullptr;
	PVAL      valp = colp->GetValue();

	error = false;

	if (valp->IsNull())
		return NULL;

	try {
		switch (valp->GetType()) {
			case TYPE_STRING:
				val = env->NewStringUTF(valp->GetCharValue());
				break;
			case TYPE_INT:
			case TYPE_SHORT:
				cls = env->FindClass("java/lang/Integer");
				cns = env->GetMethodID(cls, "<init>", "(I)V");
				val = env->NewObject(cls, cns, valp->GetIntValue());
				break;
			case TYPE_TINY:
				cls = env->FindClass("java/lang/Boolean");
				cns = env->GetMethodID(cls, "<init>", "(Z)V");
				val = env->NewObject(cls, cns, (valp->GetIntValue() != 0));
				break;
			case TYPE_BIGINT:
				cls = env->FindClass("java/lang/Long");
				cns = env->GetMethodID(cls, "<init>", "(J)V");
				val = env->NewObject(cls, cns, valp->GetBigintValue());
				break;
			case TYPE_DOUBLE:
				cls = env->FindClass("java/lang/Double");
				cns = env->GetMethodID(cls, "<init>", "(D)V");
				val = env->NewObject(cls, cns, valp->GetFloatValue());
				break;
			default:
				sprintf(g->Message, "Cannot make object from %d type", valp->GetType());
				error = true;
				break;
		}	// endswitch Type

	} catch (...) {
		sprintf(g->Message, "Cannot make object from %s value", colp->GetName());
		error = true;
	}	// end try/catch

	return val;
}	// end of MakeObject

/***********************************************************************/
/*  MakeDoc.                                                           */
/***********************************************************************/
jobject JMgoConn::MakeDoc(PGLOBAL g, PJNCOL jcp)
{
	bool    error = false;
	jobject parent, child, val;
	jstring jkey;
	
	if (jcp->Array)
		parent = env->CallObjectMethod(job, mkarid);
	else
		parent = env->CallObjectMethod(job, mkdocid);

	for (PJKC kp = jcp->Klist; kp; kp = kp->Next)
		if (kp->Jncolp) {
			if (!(child = MakeDoc(g, kp->Jncolp)))
				return NULL;

			if (!jcp->Array) {
				jkey = env->NewStringUTF(kp->Key);

				if (env->CallBooleanMethod(job, docaddid, parent, jkey, child))
					return NULL;

				env->DeleteLocalRef(jkey);
			} else
				if (env->CallBooleanMethod(job, araddid, parent, kp->N, child))
					return NULL;

		} else {
			if (!(val = MakeObject(g, kp->Colp, error))) {
				if (error)
					return NULL;

			} else if (!jcp->Array) {
				jkey = env->NewStringUTF(kp->Key);

				if (env->CallBooleanMethod(job, docaddid, parent, jkey, val))
					return NULL;

				env->DeleteLocalRef(jkey);
			} else if (env->CallBooleanMethod(job, araddid, parent, kp->N, val)) {
				if (Check(-1))
					sprintf(g->Message, "ArrayAdd: %s", Msg);
				else
					sprintf(g->Message, "ArrayAdd: unknown error");

				return NULL;
			}	// endif ArrayAdd

		} // endif Jncolp

	return parent;
} // end of MakeDoc

/***********************************************************************/
/*  Insert a new document in the collation.                            */
/***********************************************************************/
int JMgoConn::DocWrite(PGLOBAL g)
{
	jobject doc;

	if (!Fpc || !(doc = MakeDoc(g, Fpc)))
		return RC_FX;

	if (env->CallBooleanMethod(job, insertid, doc)) {
		if (Check(-1))
			sprintf(g->Message, "CollInsert: %s", Msg);
		else
			sprintf(g->Message, "CollInsert: unknown error");

		return RC_FX;
	} // endif Insert

	return RC_OK;
} // end of DocWrite

/***********************************************************************/
/*  Update the current document from the collection.                   */
/***********************************************************************/
int JMgoConn::DocUpdate(PGLOBAL g, PTDB tdbp)
{
	int     rc = RC_OK;
	bool    error;
	PCOL    colp;
	jstring jkey;
	jobject val, upd, updlist = env->CallObjectMethod(job, mkdocid);

	// Make the list of changes to do
	for (colp = tdbp->GetSetCols(); colp; colp = colp->GetNext()) {
		jkey = env->NewStringUTF(colp->GetJpath(g, false));
		val = MakeObject(g, colp, error);

		if (error)
			return RC_FX;

		if (env->CallBooleanMethod(job, docaddid, updlist, jkey, val))
			return RC_OK;

		env->DeleteLocalRef(jkey);
	}	// endfor colp

	// Make the update parameter
	upd = env->CallObjectMethod(job, mkdocid);
	jkey = env->NewStringUTF("$set");

	if (env->CallBooleanMethod(job, docaddid, upd, jkey, updlist))
		return RC_OK;

	env->DeleteLocalRef(jkey);

	jlong ar = env->CallLongMethod(job, updateid, upd);

	if (trace(1))
		htrc("DocUpdate: ar = %ld\n", ar);

	if (Check((int)ar)) {
		sprintf(g->Message, "CollUpdate: %s", Msg);
		rc = RC_FX;
	} // endif ar

	return rc;
} // end of DocUpdate

/***********************************************************************/
/*  Remove all or only the current document from the collection.       */
/***********************************************************************/
int JMgoConn::DocDelete(PGLOBAL g, bool all)
{
	int   rc = RC_OK;
	jlong ar = env->CallLongMethod(job, deleteid, all);

	if (trace(1))
		htrc("DocDelete: ar = %ld\n", ar);

	if (Check((int)ar)) {
		sprintf(g->Message, "CollDelete: %s", Msg);
		rc = RC_FX;
	} // endif ar

	return rc;
} // end of DocDelete

/***********************************************************************/
/*  Rewind the collection.                                             */
/***********************************************************************/
bool JMgoConn::Rewind(void)
{
	return env->CallBooleanMethod(job, rewindid);
} // end of Rewind

/***********************************************************************/
/*  Retrieve the column string value from the document.                */
/***********************************************************************/
PSZ JMgoConn::GetColumnValue(PSZ path)
{
	PGLOBAL& g = m_G;
	PSZ      fld = NULL;
	jstring  fn, jn = nullptr;

	if (!path || (jn = env->NewStringUTF(path)) == nullptr) {
		sprintf(g->Message, "Fail to allocate jstring %s", SVP(path));
		throw (int)TYPE_AM_MGO;
	}	// endif name

	if (!gmID(g, objfldid, "GetField", "(Ljava/lang/String;)Ljava/lang/String;")) {
		fn = (jstring)env->CallObjectMethod(job, objfldid, jn);

		if (fn)
			fld = (PSZ)GetUTFString(fn);

	}	// endif objfldid

	return fld;
} // end of GetColumnValue
