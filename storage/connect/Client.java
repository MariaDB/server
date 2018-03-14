
package wrappers;

import java.io.BufferedReader;
import java.io.Console;
import java.io.IOException;
import java.io.InputStreamReader;
import java.sql.Date;
import java.sql.Time;
import java.sql.Timestamp;

public class Client {
    static boolean    	 DEBUG = true;
    static final Console c = System.console();
    static JdbcInterface jdi = null;
   
	public static void main(String[] args) {
		int rc, n, ncol, i = 0, fsize = 0;
		boolean scrollable = false;
		String  s;
		String[] parms = new String[4];
		
	    if (args.length > 0)
	         try {
	             i = Integer.parseInt(args[i]);
	         } catch (NumberFormatException e) {
	             i = 0;
	         } // end try/catch
	    
	    switch (i) {
	    case 1:
	    	jdi = new ApacheInterface(DEBUG);
	    	break;
	    case 2:
	    	jdi = new MysqlInterface(DEBUG);
	    	break;
	    case 3:
	    	jdi = new MariadbInterface(DEBUG);
	    	break;
	    case 4:
	    	jdi = new OracleInterface(DEBUG);
	    	break;
	    case 5:
	    	jdi = new PostgresqlInterface(DEBUG);
	    	break;
	    default:
			jdi = new JdbcInterface(DEBUG);
	    } // endswitch i
		
		parms[0] = getLine("Driver: ", false);
		parms[1] = getLine("URL: ", false);
		parms[2] = getLine("User: ", false);
		parms[3] = getLine("Password: ", true);
		s = getLine("Fsize: ", false);
		fsize = (s != null) ? Integer.parseInt(s) : 0;
		s = getLine("Scrollable: ", false);
		scrollable = (s != null) ? s.toLowerCase().charAt(0) != 'n' : false;
		
		rc = jdi.JdbcConnect(parms, fsize, scrollable);
		
		if (rc == 0) {
		  String query;
		  System.out.println("Successfully connected to " + parms[1]);
		  
		  s = jdi.GetQuoteString();
		  System.out.println("Qstr = '" + s + "'");
		  
		  while ((query = getLine("Query: ", false)) != null) {
		    n = jdi.Execute(query);
		    System.out.println("Returned n = " + n);
		    
		    if ((ncol = jdi.GetResult()) > 0)
  		      PrintResult(ncol);
		    else
		      System.out.println("Affected rows = " + n);
		    
		  } // endwhile
		  
		  rc = jdi.JdbcDisconnect();
		  System.out.println("Disconnect returned " + rc);
		} else
		  System.out.println(jdi.GetErrmsg() + " rc=" + rc);
		
	} // end of main

	private static void PrintResult(int ncol) {
    	// Get result set meta data
    	int i;
    	Date date = new Date(0);
    	Time time = new Time(0);
    	Timestamp tsp = new Timestamp(0);
    	String columnName;
		Object job;

    	// Get the column names; column indices start from 1
    	for (i = 1; i <= ncol; i++) {
    		columnName = jdi.ColumnName(i);
    		
    		if (columnName == null)
    			return;

    		// Get the name of the column's table name
    		//String tableName = rsmd.getTableName(i);
    		
    		if (i > 1)
		    	System.out.print("\t");
    		
    		System.out.print(columnName);
    	} // endfor i
    	
    	System.out.println();
	    
	    // Loop through the result set
	    while (jdi.ReadNext() > 0) {
	    	for (i = 1; i <= ncol; i++) {
	    		if (i > 1)
			    	System.out.print("\t");
	    		
	    		if (DEBUG)
	    			System.out.print("(" + jdi.ColumnType(i, null) + ")");
	    		
	    		switch (jdi.ColumnType(i, null)) {
	    		case java.sql.Types.VARCHAR:
	    		case java.sql.Types.LONGVARCHAR:
	    		case java.sql.Types.CHAR:
				case 1111:
			    	System.out.print(jdi.StringField(i, null));
	    			break;
	    		case java.sql.Types.INTEGER:
			    	System.out.print(jdi.IntField(i, null));
	    			break;
	    		case java.sql.Types.BIGINT:
			    	System.out.print(jdi.BigintField(i, null));
	    			break;
	    		case java.sql.Types.TIME:
	    			time.setTime((long)jdi.TimeField(i, null) * 1000);
			    	System.out.print(time);
	    			break;
	    		case java.sql.Types.DATE:
	    			date.setTime((long)jdi.DateField(i, null) * 1000);
			    	System.out.print(date);
	    			break;
	    		case java.sql.Types.TIMESTAMP:
	    			tsp.setTime((long)jdi.TimestampField(i, null) * 1000);
			    	System.out.print(tsp);
	    			break;
	    		case java.sql.Types.SMALLINT:
			    	System.out.print(jdi.IntField(i, null));
	    			break;
	    		case java.sql.Types.DOUBLE:
	    		case java.sql.Types.REAL:
	    		case java.sql.Types.FLOAT:
	    		case java.sql.Types.DECIMAL:
	    			System.out.print(jdi.DoubleField(i, null));
	    			break;
	    		case java.sql.Types.BOOLEAN:
	    			System.out.print(jdi.BooleanField(i, null));
	    		default:
					job = jdi.ObjectField(i, null);
					System.out.print(job.toString());
	    			break;
	    		} // endswitch Type
	    		
	    	} // endfor i
	    	
	    	System.out.println();
	    } // end while rs

	} // end of PrintResult

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
