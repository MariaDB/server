package wrappers;

import java.io.BufferedReader;
import java.io.Console;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.Date;

public class TestInsert3 {
	static boolean DEBUG = true;
	static final Console c = System.console();
	static Mongo3Interface jdi = null;

	public static void main(String[] args) {
		int rc;
		String[] parms = new String[4];

		jdi = new Mongo3Interface(DEBUG);

		parms[0] = getLine("URI: ", false);
		parms[1] = getLine("Database: ", false);
		parms[2] = null;
		parms[3] = null;

		if (parms[0] == null)
			parms[0] = "mongodb://localhost:27017";

		if (parms[1] == null)
			parms[1] = "test";

		rc = jdi.MongoConnect(parms);

		if (rc == 0) {
			Object bdoc = jdi.MakeDocument();

			if (jdi.DocAdd(bdoc, "_id", (Object) 1, 0))
				System.out.println(jdi.GetErrmsg());

			if (jdi.DocAdd(bdoc, "Name", (Object) "Smith", 0))
				System.out.println(jdi.GetErrmsg());

			if (jdi.DocAdd(bdoc, "Age", (Object) 39, 0))
				System.out.println(jdi.GetErrmsg());

			if (jdi.DocAdd(bdoc, "Pi", (Object) 3.14, 0))
				System.out.println(jdi.GetErrmsg());

			if (jdi.DocAdd(bdoc, "Phone", (Object) "{\"ext\":[4,5,7]}", 1))
				System.out.println(jdi.GetErrmsg());

			if (jdi.DocAdd(bdoc, "Scores", (Object) "[24,2,13]", 2))
				System.out.println(jdi.GetErrmsg());

			Object bar = jdi.MakeArray();

			for (int i = 0; i < 2; i++)
				if (jdi.ArrayAdd(bar, i, (Object) (Math.random() * 10.0), 0))
					System.out.println(jdi.GetErrmsg());

			if (jdi.DocAdd(bdoc, "Prices", bar, 0))
				System.out.println(jdi.GetErrmsg());

			Object dat = new Date();

			if (jdi.DocAdd(bdoc, "Date", dat, 0))
				System.out.println(jdi.GetErrmsg());

			System.out.println(bdoc);

			// Try to update
			if (!jdi.GetCollection("updtest") && !jdi.FindColl(null, null)) {
				if (jdi.CollDelete(true) < 0)
					System.out.println(jdi.GetErrmsg());

				if (jdi.CollInsert(bdoc))
					System.out.println(jdi.GetErrmsg());

				Object updlist = jdi.MakeDocument();

				if (jdi.DocAdd(updlist, "Age", (Object) 40, 0))
					System.out.println(jdi.GetErrmsg());

				Object upd = jdi.MakeDocument();

				if (jdi.DocAdd(upd, "$set", updlist, 0))
					System.out.println(jdi.GetErrmsg());

				if (jdi.ReadNext() > 0 && jdi.CollUpdate(upd) < 0)
					System.out.println(jdi.GetErrmsg());

				if (!jdi.Rewind() && jdi.ReadNext() > 0)
					System.out.println(jdi.GetDoc());
				else
					System.out.println("Failed Rewind");

			} // endif n

		} // endif rc

	} // end of main

	// ==================================================================
	private static String getLine(String p, boolean b) {
		String response;

		if (c != null) {
			// Standard console mode
			if (b) {
				response = new String(c.readPassword(p));
			} else
				response = c.readLine(p);

		} else {
			// For instance when testing from Eclipse
			BufferedReader in = new BufferedReader(new InputStreamReader(System.in));

			System.out.print(p);

			try {
				// Cannot suppress echo for password entry
				response = in.readLine();
			} catch (IOException e) {
				response = "";
			} // end of try/catch

		} // endif c

		return (response.isEmpty()) ? null : response;
	} // end of getLine

}
