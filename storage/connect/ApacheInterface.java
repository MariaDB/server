package wrappers;

import java.sql.*;
import java.util.Hashtable;
import org.apache.commons.dbcp2.BasicDataSource;

public class ApacheInterface extends JdbcInterface {
    static Hashtable<String,BasicDataSource> pool = new Hashtable<String, BasicDataSource>(); 
    
    public ApacheInterface() {
    	this(true);
    } // end of default constructor

    public ApacheInterface(boolean b) {
    	super(b);
    } // end of constructor

    @Override
    public int JdbcConnect(String[] parms, int fsize, boolean scrollable) {
        int rc = 0;
        String url = parms[1];
        BasicDataSource ds = null;
        
	    if (DEBUG)
	    	System.out.println("Connecting to Apache data source");
	      
        try {
    	    CheckURL(url, null);
           
          	if ((ds = pool.get(url)) == null) {
            	ds = new BasicDataSource();
                ds.setDriverClassName(parms[0]);
                ds.setUrl(url);
                ds.setUsername(parms[2]);
                ds.setPassword(parms[3]);
                pool.put(url, ds);
            } // endif ds
            
          	// if (parms.length > 4 && parms[4] != null)
          	//  ds.setConnectionProperties(parms[4]);
          		
            // Get a connection from the data source
            conn = ds.getConnection();
        	    
        	// Get the data base meta data object
        	dbmd = conn.getMetaData();
        	    
    	    // Get a statement from the connection
    	    stmt = GetStmt(fsize, scrollable);
    	} catch (SQLException se) {
    		SetErrmsg(se);
    	    rc = -2; 
    	} catch (Exception e) {
    		SetErrmsg(e);
    	    rc = -3; 
    	} // end try/catch

        return rc;
    } // end of JdbcConnect    

} // end of class ApacheInterface 
