package wrappers;

import java.io.BufferedReader;
import java.io.Console;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.Set;

public class Client3 {
	static boolean DEBUG = true;
	static final Console c = System.console();
	static Mongo3Interface jdi = null;

	public static void main(String[] args) {
		int rc, level = 0;
		boolean brc, desc = false;
		Set<String> columns;
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
			String name, pipeline, query, fields;
			System.out.println("Successfully connected to " + parms[0]);

			while ((name = getLine("Collection: ", false)) != null) {
				if (jdi.GetCollection(name))
					System.out.println("GetCollection failed");
				else
					System.out.println("Collection size: " + jdi.GetCollSize());

				pipeline = getLine("Pipeline: ", false);

				if (pipeline == null || (desc = pipeline.equals("*"))) {
					query = getLine("Filter: ", false);
					fields = getLine("Proj: ", false);

					if (desc)
						level = Integer.parseInt(getLine("Level: ", false));

					brc = jdi.FindColl(query, fields);
				} else
					brc = jdi.AggregateColl(pipeline);

				System.out.println("Returned brc = " + brc);

				if (!brc && !desc) {
					for (int i = 0; jdi.ReadNext() > 0 && i < 10; i++) {
						columns = jdi.GetColumns();

						for (String col : columns)
							System.out.println(col + "=" + jdi.GetField(col));

						if (name.equalsIgnoreCase("gtst"))
							System.out.println("gtst=" + jdi.GetField("*"));

						if (name.equalsIgnoreCase("inventory")) {
							System.out.println("warehouse=" + jdi.GetField("instock.0.warehouse"));
							System.out.println("quantity=" + jdi.GetField("instock.1.qty"));
						} // endif inventory

						if (name.equalsIgnoreCase("restaurants")) {
							System.out.println("score=" + jdi.GetField("grades.0.score"));
							System.out.println("date=" + jdi.GetField("grades.0.date"));
						} // endif inventory

					} // endfor i

				} else if (desc) {
					int ncol;

					for (int i = 0; (ncol = jdi.ReadNext()) > 0 && i < 2; i++) {
						if (discovery(null, "", ncol, level))
							break;

						System.out.println("--------------");
					} // endfor i

				} // endif desc

			} // endwhile query

			rc = jdi.MongoDisconnect();
			System.out.println("Disconnect returned " + rc);
		} else
			System.out.println(jdi.GetErrmsg() + " rc=" + rc);

	} // end of main

	private static boolean discovery(Object obj, String name, int ncol, int level) {
		int[] val = new int[5];
		Object ret = null;
		String bvn = null;

		for (int k = 0; k < ncol; k++) {
			ret = jdi.ColumnDesc(obj, k, val, level);
			bvn = jdi.ColDescName();

			if (ret != null)
				discovery(ret, name.concat(bvn).concat("."), val[4], level - 1);
			else if (val[0] > 0)
				System.out.println(
						name + bvn + ": type=" + val[0] + " length=" + val[1] + " prec=" + val[2] + " nullable=" + val[3]);
			else if (val[0] < 0)
				System.out.println(jdi.GetErrmsg());

		} // endfor k

		return false;
	} // end of discovery

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
