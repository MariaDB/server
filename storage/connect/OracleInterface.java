package wrappers;

import java.sql.*;
import java.util.Hashtable;

import javax.sql.DataSource;
import oracle.jdbc.pool.OracleDataSource;

public class OracleInterface extends JdbcInterface {
	public OracleInterface() {
		this(true);
	} // end of OracleInterface constructor

	public OracleInterface(boolean b) {
		super(b);
    	
    	if (dst == null)
    		dst = new Hashtable<String, DataSource>();
    	
	} // end of OracleInterface constructor

    @Override
	public int JdbcConnect(String[] parms, int fsize, boolean scrollable) {
	      int              rc = 0;
	      String           url = parms[1];
	      DataSource       ds = null;
	      OracleDataSource ods = null;
	      
	      if (DEBUG)
	    	  System.out.println("Connecting to Oracle data source");
	      
	      try {
			CheckURL(url, "oracle");
			         
	    	if ((ds = dst.get(url)) == null) {
	    		ods = new OracleDataSource();
	            ods.setURL(url);
	            
	            if (parms[2] != null)
	            	ods.setUser(parms[2]);
	            
	            if (parms[3] != null)
	            	ods.setPassword(parms[3]);
	            
	            ds = ods;
	    	  
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
	    
} // end of class OracleInterface
