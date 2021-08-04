package wrappers;

//import java.math.BigDecimal;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Set;

import org.bson.BsonArray;
import org.bson.BsonBoolean;
import org.bson.BsonDateTime;
//import org.bson.BsonDecimal128;
import org.bson.BsonDocument;
import org.bson.BsonDouble;
import org.bson.BsonInt32;
import org.bson.BsonInt64;
import org.bson.BsonNull;
import org.bson.BsonString;
import org.bson.BsonValue;
import org.bson.Document;
import org.bson.conversions.Bson;
//import org.bson.types.Decimal128;

import com.mongodb.MongoClient;
import com.mongodb.MongoClientURI;
import com.mongodb.MongoException;
import com.mongodb.client.AggregateIterable;
import com.mongodb.client.FindIterable;
import com.mongodb.client.MongoCollection;
import com.mongodb.client.MongoCursor;
import com.mongodb.client.MongoDatabase;
import com.mongodb.client.model.Filters;
import com.mongodb.client.result.DeleteResult;
import com.mongodb.client.result.UpdateResult;

public class Mongo3Interface {
	boolean DEBUG = false;
	String Errmsg = "No error";
	String bvalName = null;
	Set<String> Colnames = null;
	MongoClient client = null;
	MongoDatabase db = null;
	MongoCollection<BsonDocument> coll = null;
	FindIterable<BsonDocument> finditer = null;
	AggregateIterable<BsonDocument> aggiter = null;
	MongoCursor<BsonDocument> cursor = null;
	BsonDocument doc = null;
	BsonDocument util = null;
	BsonNull bsonull = new BsonNull();

	// === Constructors/finalize =========================================
	public Mongo3Interface() {
		this(false);
	} // end of default constructor

	public Mongo3Interface(boolean b) {
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
			System.out.println("Mongo3: URI=" + parms[0] + " DB=" + parms[1]);

		try {
			MongoClientURI uri = new MongoClientURI(parms[0]);

			client = new MongoClient(uri);

			if (DEBUG)
				System.out.println("Connection " + client.toString() + " established");

			// Now connect to your databases
			db = client.getDatabase(parms[1]);

			// if (parms[2] != null && !parms[2].isEmpty()) {
			// if (DEBUG)
			// System.out.println("user=" + parms[2] + " pwd=" + parms[3]);

			// @SuppressWarnings("deprecation")
			// boolean auth = db.authenticate(parms[2], parms[3].toCharArray());

			// if (DEBUG)
			// System.out.println("Authentication: " + auth);

			// } // endif user

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
			coll = db.getCollection(name).withDocumentClass(BsonDocument.class);
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
			if (query != null) {
				Bson dbq = Document.parse(query);
				finditer = coll.find(dbq);
			} else
				finditer = coll.find();

			if (fields != null) {
				Bson dbf = BsonDocument.parse(fields);
				finditer = finditer.projection(dbf);
			} // endif fields

			cursor = finditer.iterator();
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
				Document pipe = Document.parse(pipeline);
				ArrayList<?> pip = (ArrayList<?>) pipe.get("pipeline");

			aggiter = coll.aggregate((List<? extends Bson>) pip);
			cursor = aggiter.iterator();
			} catch (MongoException me) {
				SetErrmsg(me);
			return true;
			} // end try/catch

		return false;
	} // end of AggregateColl

	public boolean Rewind() {
		if (cursor != null)
			cursor.close();

		if (finditer != null)
			cursor = finditer.iterator();
		else if (aggiter != null)
			cursor = aggiter.iterator();

		return (cursor == null);
	} // end of Rewind

	public int ReadNext() {
		try {
			if (cursor.hasNext()) {
				doc = cursor.next();

				if (DEBUG)
					System.out.println("Class doc = " + doc.getClass());

				Colnames = doc.keySet();
				return Colnames.size();
			} else
				return 0;

		} catch (MongoException mx) {
			SetErrmsg(mx);
		} // end try/catch

		return -1;
	} // end of ReadNext

	public boolean Fetch(int row) {
		if (cursor.hasNext()) {
			doc = cursor.next();
			Colnames = doc.keySet();
			return true;
		} else
			return false;

	} // end of Fetch

	public String GetDoc() {
		return (doc != null) ? doc.toJson() : null;
	} // end of GetDoc

	public Set<String> GetColumns() {
		if (doc != null)
			return doc.keySet();
		else
			return null;

	} // end of GetColumns

	public String ColumnName(int n) {
		if (n < Colnames.size())
			return (String) Colnames.toArray()[n];
		else
			return null;

	} // end of ColumnName

	public int ColumnType(int n, String name) {
		// if (rsmd == null) {
		// System.out.println("No result metadata");
		// } else try {
		// if (n == 0)
		// n = rs.findColumn(name);

		// return rsmd.getColumnType(n);
		// } catch (SQLException se) {
		// SetErrmsg(se);
		// } //end try/catch

		return 666; // Not a type
	} // end of ColumnType

	public Object ColumnDesc(Object obj, int n, int[] val, int lvl) {
		Object ret = null;
		BsonValue bval = (BsonValue) ((obj != null) ? obj : doc);
		BsonDocument dob = (bval instanceof BsonDocument) ? (BsonDocument) bval : null;
		BsonArray ary = (bval instanceof BsonArray) ? (BsonArray) bval : null;

		try {
			if (ary != null) {
				bval = ary.get(n);
				bvalName = Integer.toString(n);
			} else if (dob != null) {
				// String[] k = dob.keySet().toArray(new String[0]);
				Object[] k = dob.keySet().toArray();
				bval = dob.get(k[n]);
				bvalName = (String) k[n];
			} else
				bvalName = "x" + Integer.toString(n);

			val[0] = 0; // ColumnType
			val[1] = 0; // Precision
			val[2] = 0; // Scale
			val[3] = 0; // Nullable
			val[4] = 0; // ncol

			if (bval.isString()) {
				val[0] = 1;
				val[1] = bval.asString().getValue().length();
			} else if (bval.isInt32()) {
				val[0] = 7;
				val[1] = Integer.toString(bval.asInt32().getValue()).length();
			} else if (bval.isInt64()) {
				val[0] = 5;
				val[1] = Long.toString(bval.asInt64().getValue()).length();
			} else if (bval.isObjectId()) {
				val[0] = 1;
				val[1] = bval.asObjectId().getValue().toString().length();
			} else if (bval.isDateTime()) {
				Long TS = (bval.asDateTime().getValue() / 1000);
				val[0] = 8;
				val[1] = TS.toString().length();
			} else if (bval.isDouble()) {
				String d = Double.toString(bval.asDouble().getValue());
				int i = d.indexOf('.') + 1;

				val[0] = 2;
				val[1] = d.length();
				val[2] = (i > 0) ? val[1] - i : 0;
			} else if (bval.isBoolean()) {
				val[0] = 4;
				val[1] = 1;
			} else if (bval.isDocument()) {
				if (lvl > 0) {
					ret = bval;
					val[0] = 1;
					val[4] = bval.asDocument().keySet().size();
				} else if (lvl == 0) {
					val[0] = 1;
					val[1] = bval.asDocument().toJson().length();
				} // endif lvl

			} else if (bval.isArray()) {
				if (lvl > 0) {
					ret = bval;
					val[0] = 2;
					val[4] = bval.asArray().size();
				} else if (lvl == 0) {
					val[0] = 1;
					util = new BsonDocument("arr", bval.asArray());
					String s = util.toJson();
					int i1 = s.indexOf('[');
					int i2 = s.lastIndexOf(']');
					val[1] = i2 - i1 + 1;
				} // endif lvl

			} else if (bval.isDecimal128()) {
				val[0] = 9;
				val[1] = bval.asDecimal128().toString().length();
			} else if (bval.isNull()) {
				val[0] = 0;
				val[3] = 1;
			} else {
				SetErrmsg("Type " + bval.getBsonType() + " of " + bvalName + " not supported");
				val[0] = -1;
			} // endif's

			return ret;
		} catch (Exception ex) {
			SetErrmsg(ex);
		} // end try/catch

		val[0] = -1;
		return null;
	} // end of ColumnDesc

	public String ColDescName() {
		return bvalName;
	} // end of ColDescName

	protected BsonValue GetFieldObject(String path) {
		BsonValue o = doc;
		BsonDocument dob = null;
		BsonArray ary = null;
		String[] names = null;

		if (path == null || path.equals("") || path.equals("*"))
			return doc;
		else if (o instanceof BsonDocument)
			dob = doc;
		else if (o instanceof BsonArray)
			ary = (BsonArray) o;
		else
			return doc;

		try {
			names = path.split("\\.");

			for (String name : names) {
				if (ary != null) {
					o = ary.get(Integer.parseInt(name));
				} else
					o = dob.get(name);

				if (o == null)
					break;

				if (DEBUG)
					System.out.println("Class o = " + o.getClass());

				if (o instanceof BsonDocument) {
					dob = (BsonDocument) o;
					ary = null;
				} else if (o instanceof BsonArray) {
					ary = (BsonArray) o;
				} else
					break;

			} // endfor name

		} catch (IndexOutOfBoundsException x) {
			o = null;
		} catch (MongoException me) {
			SetErrmsg(me);
			o = null;
		} // end try/catch

		return o;
	} // end of GetFieldObject

	public String GetField(String path) {
		BsonValue o = GetFieldObject(path);

		if (o != null) {
			if (o.isString()) {
				return o.asString().getValue();
			} else if (o.isInt32()) {
				return Integer.toString(o.asInt32().getValue());
			} else if (o.isInt64()) {
				return Long.toString(o.asInt64().getValue());
			} else if (o.isObjectId()) {
				return o.asObjectId().getValue().toString();
			} else if (o.isDateTime()) {
				Integer TS = (int) (o.asDateTime().getValue() / 1000);
				return TS.toString();
			} else if (o.isDouble()) {
				return Double.toString(o.asDouble().getValue());
			} else if (o.isBoolean()) {
				return o.asBoolean().getValue() ? "1" : "0";
			} else if (o.isDocument()) {
				return o.asDocument().toJson();
			} else if (o.isArray()) {
				util = new BsonDocument("arr", o.asArray());
				String s = util.toJson();
				int i1 = s.indexOf('[');
				int i2 = s.lastIndexOf(']');
				return s.substring(i1, i2 + 1);
			} else if (o.isDecimal128()) {
				return o.asDecimal128().toString();
			} else if (o.isNull()) {
				return null;
			} else
				return o.toString();

		} else
			return null;

	} // end of GetField

	public Object MakeBson(String s, int json) {
		BsonValue bval;

		if (json == 1)
			bval = BsonDocument.parse(s);
		else if (json == 2)
			bval = BsonArray.parse(s);
		else
			bval = null;

		return bval;
	} // end of MakeBson

	protected BsonValue ObjToBson(Object val, int json) {
		BsonValue bval = null;

		if (val == null)
			bval = bsonull;
		else if (val.getClass() == String.class) {
			if (json == 1)
				bval = BsonDocument.parse((String) val);
			else if (json == 2)
				bval = BsonArray.parse((String) val);
			else
				bval = new BsonString((String) val);

		} else if (val.getClass() == Integer.class)
			bval = new BsonInt32((int) val);
		else if (val.getClass() == Double.class)
			bval = new BsonDouble((double) val);
		else if (val.getClass() == BigInteger.class)
			bval = new BsonInt64((long) val);
		else if (val.getClass() == Boolean.class)
			bval = new BsonBoolean((Boolean) val);
		else if (val.getClass() == Date.class)
			bval = new BsonDateTime(((Date) val).getTime() * 1000);
		else if (val.getClass() == BsonDocument.class)
			bval = (BsonDocument) val;
		else if (val.getClass() == BsonArray.class)
			bval = (BsonArray) val;
		// else if (val.getClass() == BigDecimal.class)
		// bval = new BsonDecimal128((BigDecimal) val);

		return bval;
	} // end of ObjToBson

	public Object MakeDocument() {
		return new BsonDocument();
	} // end of MakeDocument

	public boolean DocAdd(Object bdc, String key, Object val, int json) {
		try {
			((BsonDocument) bdc).append(key, ObjToBson(val, json));
		} catch (MongoException me) {
			SetErrmsg(me);
			return true;
		} // end try/catch

		return false;
	} // end of DocAdd

	public Object MakeArray() {
		return new BsonArray();
	} // end of MakeArray

	public boolean ArrayAdd(Object bar, int n, Object val, int json) {
		try {
			for (int i = ((BsonArray) bar).size(); i < n; i++)
				((BsonArray) bar).add(bsonull);

			((BsonArray) bar).add(ObjToBson(val, json));
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
			coll.insertOne((BsonDocument) dob);
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
			UpdateResult res = coll.updateOne(Filters.eq("_id", doc.get("_id")), (Bson) upd);

			if (DEBUG)
				System.out.println("CollUpdate: " + res.toString());

			n = res.getModifiedCount();
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
			DeleteResult res;

			if (all)
				res = coll.deleteMany(new Document());
			else
				res = coll.deleteOne(Filters.eq("_id", doc.get("_id")));

			if (DEBUG)
				System.out.println("CollDelete: " + res.toString());

			n = res.getDeletedCount();
		} catch (MongoException me) {
			SetErrmsg(me);
		} catch (Exception ex) {
			SetErrmsg(ex);
		} // end try/catch

		return n;
	} // end of CollDelete

} // end of class Mongo3Interface
