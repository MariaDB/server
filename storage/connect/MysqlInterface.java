package wrappers;

import java.sql.*;
import java.util.Hashtable;

import javax.sql.DataSource;
import com.mysql.cj.jdbc.MysqlDataSource;

public class MysqlInterface extends JdbcInterface {
    public MysqlInterface() {
    	this(true);
    } // end of default constructor

    public MysqlInterface(boolean b) {
    	super(b);
    	
    	if (dst == null)
    		dst = new Hashtable<String, DataSource>();
    	
    } // end of default constructor

    @Override
	public int JdbcConnect(String[] parms, int fsize, boolean scrollable) {
	      int             rc = 0;
	      String          url = parms[1];
	      DataSource      ds = null;
	      MysqlDataSource mds = null;
	      
	      if (DEBUG)
	    		System.out.println("Connecting to MySQL data source");
	      
	      try {
			CheckURL(url, "mysql");
			         
	    	if ((ds = dst.get(url)) == null) {
	    		mds = new MysqlDataSource();
	            mds.setUrl(url);
	            
	            if (parms[2] != null)
	            	mds.setUser(parms[2]);
	            
	            if (parms[3] != null)
	            	mds.setPassword(parms[3]);
	            
	            ds = mds;
	    	  
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
	    
} // end of class MysqlInterface
