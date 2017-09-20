package wrappers;

import java.util.Date;
import java.util.List;
import java.util.Set;

import com.mongodb.AggregationOptions;
import com.mongodb.BasicDBList;
import com.mongodb.BasicDBObject;
import com.mongodb.Cursor;
import com.mongodb.DB;
import com.mongodb.DBCollection;
import com.mongodb.DBObject;
import com.mongodb.MongoClient;
import com.mongodb.MongoClientURI;
import com.mongodb.MongoException;
import com.mongodb.WriteConcernException;
import com.mongodb.WriteResult;
import com.mongodb.util.JSON;

public class Mongo2Interface {
	boolean DEBUG = false;
	String Errmsg = "No error";
	Set<String> Colnames = null;
	Cursor cursor = null;
	MongoClient client = null;
	DB db = null;
	DBCollection coll = null;
	BasicDBObject doc = null;
	BasicDBObject dbq = null;
	BasicDBObject dbf = null;
	List<DBObject> pip = null;
	AggregationOptions aop = null;

	// === Constructors/finalize =========================================
	public Mongo2Interface() {
		this(false);
	} // end of default constructor

	public Mongo2Interface(boolean b) {
		DEBUG = b;
	} // end of constructor

	protected void SetErrmsg(String str) {
		if (DEBUG)
			System.out.println(str);

		Errmsg = str;
	} // end of SetErrmsg

	protected void SetErrmsg(Exception e) {
		if (DEBUG)
			System.out.println(e.getMessage());

		Errmsg = e.toString();
	} // end of SetErrmsg

	public String GetErrmsg() {
		String err = Errmsg;

		Errmsg = "No error";
		return err;
	} // end of GetErrmsg

	public int MongoConnect(String[] parms) {
		int rc = 0;

		if (DEBUG)
			System.out.println("Mongo2: URI=" + parms[0] + " DB=" + parms[1]);

		try {
			MongoClientURI uri = new MongoClientURI(parms[0]);

			client = new MongoClient(uri);

			if (DEBUG)
				System.out.println("Connection " + client.toString() + " established");

			// Now connect to your databases
			db = client.getDB(parms[1]);

			if (parms[2] != null && !parms[2].isEmpty()) {
				if (DEBUG)
					System.out.println("user=" + parms[2] + " pwd=" + parms[3]);

				@SuppressWarnings("deprecation")
				boolean auth = db.authenticate(parms[2], parms[3].toCharArray());

				if (DEBUG)
					System.out.println("Authentication: " + auth);

			} // endif user

		} catch (MongoException me) {
			SetErrmsg(me);
			rc = -1;
		} catch (Exception e) {
			SetErrmsg(e);
			rc = -3;
		} // end try/catch

		return rc;
	} // end of MongoConnect

	public int MongoDisconnect() {
		int rc = 0;

		try {
			if (cursor != null) {
				if (DEBUG)
					System.out.println("Closing cursor");

				cursor.close();
				cursor = null;
			} // endif client

			if (client != null) {
				if (DEBUG)
					System.out.println("Closing connection");

				client.close();
				client = null;
			} // endif client

		} catch (MongoException se) {
			SetErrmsg(se);
			rc += 8;
		} // end try/catch

		return rc;
	} // end of MongoDisconnect

	public boolean GetCollection(String name) {
		if (DEBUG)
			System.out.println("GetCollection: name=" + name);

		try {
			coll = db.getCollection(name);
		} catch (Exception e) {
			SetErrmsg(e);
			return true;
		} // end try/catch

		return false;
	} // end of GetCollection

	public long GetCollSize() {
		return (coll != null) ? coll.count() : 0;
	} // end of GetCollSize

	public boolean FindColl(String query, String fields) {
		if (DEBUG)
			System.out.println("FindColl: query=" + query + " fields=" + fields);

		try {
			if (query != null || fields != null) {
				dbq = (BasicDBObject) JSON.parse((query != null) ? query : "{}");

				if (fields != null) {
					dbf = (BasicDBObject) JSON.parse(fields);
					cursor = coll.find(dbq, dbf);
				} else
					cursor = coll.find(dbq);

			} else
				cursor = coll.find();

		} catch (Exception e) {
			SetErrmsg(e);
			return true;
		} // end try/catch

		return false;
	} // end of FindColl

	@SuppressWarnings("unchecked")
	public boolean AggregateColl(String pipeline) {
		if (DEBUG)
			System.out.println("AggregateColl: pipeline=" + pipeline);

		try {
			DBObject pipe = (DBObject) JSON.parse(pipeline);

			pip = (List<DBObject>) pipe.get("pipeline");
			aop = AggregationOptions.builder().batchSize(0).allowDiskUse(true)
					.outputMode(AggregationOptions.OutputMode.CURSOR).build();
			cursor = coll.aggregate(pip, aop);
		} catch (MongoException me) {
			SetErrmsg(me);
			return true;
		} // end try/catch

		return false;
	} // end of AggregateColl

	public boolean Rewind() {
		if (cursor != null)
			cursor.close();

		if (pip == null) {
			if (dbf != null)
				cursor = coll.find(dbq, dbf);
			else if (dbq != null)
				cursor = coll.find(dbq);
			else
				cursor = coll.find();

		} else
			cursor = coll.aggregate(pip, aop);

		return (cursor == null);
	} // end of Rewind

	public int ReadNext() {
		try {
			if (cursor.hasNext()) {
				doc = (BasicDBObject) cursor.next();

				if (DEBUG)
					System.out.println("Class doc = " + doc.getClass());

				Colnames = doc.keySet();
				return 1;
			} else
				return 0;

		} catch (MongoException me) {
			SetErrmsg(me);
			return -1;
		} // end try/catch

	} // end of ReadNext

	public boolean Fetch(int row) {
		if (cursor.hasNext()) {
			doc = (BasicDBObject) cursor.next();
			Colnames = doc.keySet();
			return true;
		} else
			return false;

	} // end of Fetch

	public String GetDoc() {
		return (doc != null) ? doc.toString() : null;
	} // end of GetDoc

	public Set<String> GetColumns() {
		if (doc != null)
			return doc.keySet();
		else
			return null;

	} // end of GetColumns

	public String ColumnDesc(int n, int[] val) {
		// if (rsmd == null) {
		// System.out.println("No result metadata");
		// return null;
		// } else try {
		// val[0] = rsmd.getColumnType(n);
		// val[1] = rsmd.getPrecision(n);
		// val[2] = rsmd.getScale(n);
		// val[3] = rsmd.isNullable(n);
		// return rsmd.getColumnLabel(n);
		// } catch (SQLException se) {
		// SetErrmsg(se);
		// } //end try/catch

		return null;
	} // end of ColumnDesc

	protected Object GetFieldObject(String path) {
		Object o = null;
		BasicDBObject dob = null;
		BasicDBList lst = null;
		String[] names = null;

		if (path == null || path.equals("*"))
			return doc;
		else if (doc instanceof BasicDBObject)
			dob = doc;
		// else if (o instanceof BasicDBList)
		// lst = (BasicDBList) doc;
		else
			return doc;

		try {
			names = path.split("\\.");

			for (String name : names) {
				if (lst != null) {
					o = lst.get(Integer.parseInt(name));
				} else
					o = dob.get(name);

				if (o == null)
					break;

				if (DEBUG)
					System.out.println("Class o = " + o.getClass());

				if (o instanceof BasicDBObject) {
					dob = (BasicDBObject) o;
					lst = null;
				} else if (o instanceof BasicDBList) {
					lst = (BasicDBList) o;
				} else
					break;

			} // endfor name

		} catch (IndexOutOfBoundsException x) {
			o = null;
		} catch (MongoException se) {
			SetErrmsg(se);
			o = null;
		} // end try/catch

		return o;
	} // end of GetFieldObject

	public String GetField(String path) {
		Object o = GetFieldObject(path);

		if (o != null) {
			if (o instanceof Date) {
				Integer TS = (int) (((Date) o).getTime() / 1000);
				return TS.toString();
			} // endif Date

			return o.toString();
		} else
			return null;

	} // end of GetField

	public Object MakeDocument() {
		return new BasicDBObject();
	} // end of MakeDocument

	public boolean DocAdd(Object bdc, String key, Object val) {
		try {
			((BasicDBObject) bdc).append(key, val);
		} catch (MongoException me) {
			SetErrmsg(me);
			return true;
		} // end try/catch

		return false;
	} // end of DocAdd

	public Object MakeArray() {
		return new BasicDBList();
	} // end of MakeArray

	public boolean ArrayAdd(Object bar, int n, Object val) {
		try {
			((BasicDBList) bar).put(n, val);
		} catch (MongoException me) {
			SetErrmsg(me);
			return true;
		} catch (Exception ex) {
			SetErrmsg(ex);
			return true;
		} // end try/catch

		return false;
	} // end of ArrayAdd

	public boolean CollInsert(Object dob) {
		try {
			coll.insert((BasicDBObject) dob);
		} catch (MongoException me) {
			SetErrmsg(me);
			return true;
		} catch (Exception ex) {
			SetErrmsg(ex);
			return true;
		} // end try/catch

		return false;
	} // end of CollInsert

	public long CollUpdate(Object upd) {
		long n = -1;

		if (DEBUG)
			System.out.println("upd: " + upd.toString());

		try {
			DBObject qry = new BasicDBObject("_id", doc.get("_id"));

			WriteResult res = coll.update(qry, (DBObject) upd);

			if (DEBUG)
				System.out.println("CollUpdate: " + res.toString());

			n = res.getN();
		} catch (MongoException me) {
			SetErrmsg(me);
		} catch (Exception ex) {
			SetErrmsg(ex);
		} // end try/catch

		return n;
	} // end of CollUpdate

	public long CollDelete(boolean all) {
		long n = -1;

		try {
			WriteResult res;
			BasicDBObject qry = new BasicDBObject();

			if (!all)
				qry.append("_id", doc.get("_id"));

			res = coll.remove(qry);

			if (DEBUG)
				System.out.println("CollDelete: " + res.toString());

			n = res.getN();
		} catch (WriteConcernException wx) {
			SetErrmsg(wx);
		} catch (MongoException me) {
			SetErrmsg(me);
		} catch (UnsupportedOperationException ux) {
			SetErrmsg(ux);
			n = 0;
		} // end try/catch

		return n;
	} // end of CollDelete

} // end of class MongoInterface
