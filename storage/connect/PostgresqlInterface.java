package wrappers;

import java.sql.SQLException;
import java.util.Hashtable;

import javax.sql.DataSource;

import org.postgresql.jdbc2.optional.PoolingDataSource;

public class PostgresqlInterface extends JdbcInterface {
	public PostgresqlInterface() {
		this(true);
	} // end of constructor

	public PostgresqlInterface(boolean b) {
		super(b);
    	
    	if (dst == null)
    		dst = new Hashtable<String, DataSource>();
    	
	} // end of constructor

	@Override
	public int JdbcConnect(String[] parms, int fsize, boolean scrollable) {
	      int               rc = 0;
	      String            url = parms[1];
	      DataSource        ds = null;
	      PoolingDataSource pds = null;
	      
	      if (DEBUG)
	    	  System.out.println("Connecting to Postgresql data source");
	      
	      try {
			CheckURL(url, "postgresql");
			         
	    	if ((ds = dst.get(url)) == null) {
	    		pds = new PoolingDataSource();
	    		pds.setUrl(url);
	            
	            if (parms[2] != null)
	            	pds.setUser(parms[2]);
	            
	            if (parms[3] != null)
	            	pds.setPassword(parms[3]);
	            
	            ds = pds;
	    	  
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
	    
} // end of class PostgresqlInterface
