package wrappers;

import java.io.BufferedReader;
import java.io.Console;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.Set;

public class Client2 {
	static boolean DEBUG = true;
	static final Console c = System.console();
	static Mongo2Interface jdi = null;

	public static void main(String[] args) {
		int rc, m, i = 0;
		boolean brc;
		Set<String> columns;
		String[] parms = new String[4];

		jdi = new Mongo2Interface(DEBUG);

		parms[0] = getLine("URI: ", false);
		parms[1] = getLine("DB: ", false);
		parms[2] = null;
		parms[3] = null;

		if (parms[0] == null)
			parms[0] = "mongodb://localhost:27017";

		if (parms[1] == null)
			parms[1] = "test";

		rc = jdi.MongoConnect(parms);

		if (rc == 0) {
			String name, pipeline, query, fields;
			System.out.println("Successfully connected to " + parms[1]);

			while ((name = getLine("Collection: ", false)) != null) {
				if (jdi.GetCollection(name))
					System.out.println("GetCollection failed");
				else
					System.out.println("Collection size: " + jdi.GetCollSize());

				pipeline = getLine("Pipeline: ", false);

				if (pipeline == null) {
					query = getLine("Filter: ", false);
					fields = getLine("Proj: ", false);
					brc = jdi.FindColl(query, fields);
				} else
					brc = jdi.AggregateColl(pipeline);

				System.out.println("Returned brc = " + brc);

				if (!brc) {
					for (i = 0; i < 10; i++) {
						m = jdi.ReadNext();

						if (m > 0) {
							columns = jdi.GetColumns();

							for (String col : columns)
								System.out.println(col + "=" + jdi.GetField(col));

							if (pipeline == null) {
								if (name.equalsIgnoreCase("gtst"))
									System.out.println("gtst=" + jdi.GetField("*"));

								if (name.equalsIgnoreCase("inventory")) {
									System.out.println("warehouse=" + jdi.GetField("instock.0.warehouse"));
									System.out.println("quantity=" + jdi.GetField("instock.1.qty"));
								} // endif inventory

								if (name.equalsIgnoreCase("restaurants")) {
									System.out.println("score=" + jdi.GetField("grades.0.score"));
									System.out.println("date=" + jdi.GetField("grades.0.date"));
								} // endif restaurants
						
							} // endif pipeline

						} else if (m < 0) {
							System.out.println("ReadNext: " + jdi.GetErrmsg());
							break;
						} else
							break;

					} // endfor i

				} // endif brc

			} // endwhile name

			rc = jdi.MongoDisconnect();
			System.out.println("Disconnect returned " + rc);
		} else
			System.out.println(jdi.GetErrmsg() + " rc=" + rc);

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

} // end of class Client
