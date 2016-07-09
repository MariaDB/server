package wrappers;

import java.sql.*;
import java.util.Hashtable;

import javax.sql.DataSource;
import org.mariadb.jdbc.MariaDbDataSource;

public class MariadbInterface extends JdbcInterface {
    public MariadbInterface() {
    	this(true);
    } // end of default constructor

    public MariadbInterface(boolean b) {
    	super(b);
    	
    	if (dst == null)
    		dst = new Hashtable<String, DataSource>();
    	
    } // end of default constructor

	@Override
	public int JdbcConnect(String[] parms, int fsize, boolean scrollable) {
	      int               rc = 0;
	      String            url = parms[1];
	      DataSource        ds = null;
	      MariaDbDataSource ads = null;
	      
	      if (DEBUG)
	    	  System.out.println("Connecting to MariaDB data source");
	      
	      try {
		    CheckURL(url, "mariadb");
			         
	    	if ((ds = dst.get(url)) == null) {
	    		ads = new MariaDbDataSource();
	    		ads.setUrl(url);
	            
	            if (parms[2] != null)
	            	ads.setUser(parms[2]);
	            
	            if (parms[3] != null)
	            	ads.setPassword(parms[3]);
	            
	            ds = ads;
	    	  
	    	  dst.put(url, ds);
	    	} // endif ds
	        
	        // Get a connection from the data source
	        conn = ds.getConnection();
		    
		    // Get the data base meta data object
		    dbmd = conn.getMetaData();
		    
    	    // Get a statement from the connection
    	    stmt = GetStmt(fsize, scrollable);
	  	  } catch (SQLException se) {
	  		SetErrmsg(se);
	  	    rc = -2; 
	  	  } catch( Exception e ) {
	  		SetErrmsg(e);
	  	    rc = -3; 
	  	  } // end try/catch

	      return rc;
	    } // end of JdbcConnect
	    
}
