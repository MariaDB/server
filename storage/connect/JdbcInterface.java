import java.math.*;
import java.sql.*;
//import java.util.Arrays;
import java.util.Collections;
import java.util.List;
//import java.io.File;
//import java.lang.reflect.Field;

public class JdbcInterface {
	boolean           DEBUG = false;
	String            Errmsg = "No error";
	Connection        conn = null;
	DatabaseMetaData  dbmd = null;
	Statement         stmt = null;
	PreparedStatement pstmt = null;
    ResultSet         rs = null;
    ResultSetMetaData rsmd = null;
    
    // === Constructors/finalize  =========================================
    public JdbcInterface() {
    	this(true);
    } // end of default constructor

    public JdbcInterface(boolean b) {
    	DEBUG = b;
    } // end of constructor
    
    private void SetErrmsg(Exception e) {
        if (DEBUG)
    		System.out.println(e.getMessage());
      	
        Errmsg = e.toString();
    } // end of SetErrmsg
      
    public String GetErrmsg() {
      String err = Errmsg;
      
      Errmsg = "No error";
      return err;
    } // end of GetErrmsg

    public int JdbcConnect(String[] parms, int fsize, boolean scrollable) {
      int rc = 0;
      
      if (DEBUG)
      	System.out.println("In JdbcInterface: driver=" + parms[0]);
      
      try {
		if (DEBUG)
		  System.out.println("In try block");
			      
		if (parms[0] != null && !parms[0].isEmpty()) {
		  if (DEBUG)
			System.out.println("Loading class" + parms[0]);
		  
  		  Class.forName(parms[0]); //loads the driver
		} // endif driver
			
	    if (DEBUG)
		  System.out.println("URL=" + parms[1]);
	      
    	if (parms[2] != null && !parms[2].isEmpty()) {
  	      if (DEBUG)
  	      	System.out.println("user=" + parms[2] + " pwd=" + parms[3]);
  	      
    	  conn = DriverManager.getConnection(parms[1], parms[2], parms[3]);
    	} else
    	  conn = DriverManager.getConnection(parms[1]);

	    if (DEBUG)
		  System.out.println("Connection " + conn.toString() + " established");
	    
	    // Get the data base meta data object
	    dbmd = conn.getMetaData();
	    
	    // Get a statement from the connection
	    if (scrollable)
		  stmt = conn.createStatement(java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE, java.sql.ResultSet.CONCUR_READ_ONLY);
	    else
		  stmt = conn.createStatement(java.sql.ResultSet.TYPE_FORWARD_ONLY, java.sql.ResultSet.CONCUR_READ_ONLY);

		if (DEBUG)
		  System.out.println("Statement type = " + stmt.getResultSetType()
			                 + " concurrency = " + stmt.getResultSetConcurrency());
		  
        if (DEBUG)   // Get the fetch size of a statement
		  System.out.println("Default fetch size = " + stmt.getFetchSize());

        if (fsize != 0) {
	      // Set the fetch size
	      stmt.setFetchSize(fsize);
	      
		  if (DEBUG)
			System.out.println("New fetch size = " + stmt.getFetchSize());
			      
        } // endif fsize
	      
	  } catch(ClassNotFoundException e) {
		SetErrmsg(e);
	    rc = -1; 
	  } catch (SQLException se) {
		SetErrmsg(se);
	    rc = -2; 
	  } catch( Exception e ) {
		SetErrmsg(e);
	    rc = -3; 
	  } // end try/catch
      
      return rc;
    } // end of JdbcConnect
    
    public int CreatePrepStmt(String sql) {
    	int rc = 0;
    	
    	try {
    		pstmt = conn.prepareStatement(sql);
    	} catch (SQLException se) {
    		SetErrmsg(se);
    	    rc = -1; 
    	} catch (Exception e) {
    		SetErrmsg(e);
    	    rc = -2; 
    	} // end try/catch
    	
    	return rc;
    } // end of CreatePrepStmt
    
    public void SetStringParm(int i, String s) {
    	try {
    		pstmt.setString(i, s);
    	} catch (Exception e) {
    		SetErrmsg(e);
    	} // end try/catch
    	
    } // end of SetStringParm
    
    public void SetIntParm(int i, int n) {
    	try {
    		pstmt.setInt(i, n);
    	} catch (Exception e) {
    		SetErrmsg(e);
    	} // end try/catch
    	
    } // end of SetIntParm
    
    public void SetShortParm(int i, short n) {
    	try {
    		pstmt.setShort(i, n);
    	} catch (Exception e) {
    		SetErrmsg(e);
    	} // end try/catch
    	
    } // end of SetShortParm
    
    public void SetBigintParm(int i, long n) {
    	try {
    		pstmt.setLong(i, n);
    	} catch (Exception e) {
    		SetErrmsg(e);
    	} // end try/catch
    	
    } // end of SetBigintParm
    
    public void SetFloatParm(int i, float f) {
    	try {
    		pstmt.setFloat(i, f);
    	} catch (Exception e) {
    		SetErrmsg(e);
   	} // end try/catch
    	
    } // end of SetFloatParm
    
    public void SetDoubleParm(int i, double d) {
    	try {
    		pstmt.setDouble(i, d);
    	} catch (Exception e) {
    		SetErrmsg(e);
   	} // end try/catch
    	
    } // end of SetDoubleParm
    
    public void SetTimestampParm(int i, Timestamp t) {
    	try {
    		pstmt.setTimestamp(i, t);
    	} catch (Exception e) {
    		SetErrmsg(e);
    	} // end try/catch
    	
    } // end of SetTimestampParm
    
    public int ExecutePrep() {
        int n = -3;
        
        if (pstmt != null) try {
      	  n = pstmt.executeUpdate();
        } catch (SQLException se) {
    	  SetErrmsg(se);
  		  n = -1;
        } catch (Exception e) {
    	  SetErrmsg(e);
    	  n = -2;
        } //end try/catch

        return n;
    } // end of ExecutePrep
    
    public boolean ClosePrepStmt() {
    	boolean b = false;
    	
		if (pstmt != null) try {
    		pstmt.close();
    		pstmt = null;
    	} catch (SQLException se) {
    		SetErrmsg(se);
    	    b = true; 
    	} catch (Exception e) {
    		SetErrmsg(e);
    	    b = true; 
    	} // end try/catch
    	
    	return b;
    } // end of ClosePrepStmt 

    public int JdbcDisconnect() {
      int rc = 0;
      
      // Cancel pending statement
	  if (stmt != null)
		try {
		  System.out.println("Cancelling statement");
		  stmt.cancel();
		} catch(SQLException se) {
		  SetErrmsg(se);
		  rc += 1;
	    } // nothing more we can do
		      
	  // Close the statement and the connection
	  if (rs != null)
		try {
		  if (DEBUG)
			System.out.println("Closing result set");
		  
		  rs.close();
		} catch(SQLException se) {
		  SetErrmsg(se);
		  rc = 2;
	    } // nothing more we can do
		      
	  if (stmt != null)
		try {
		  if (DEBUG)
		    System.out.println("Closing statement");
		  
		  stmt.close();
		} catch(SQLException se) {
		  SetErrmsg(se);
		  rc += 4;
	    } // nothing more we can do
	  
	  ClosePrepStmt();
		      
      if (conn != null)
		try {
		  if (DEBUG)
		    System.out.println("Closing connection");
		  
		  conn.close();
	    } catch (SQLException se) {
		  SetErrmsg(se);
		  rc += 8;
	    } //end try/catch
	
      if (DEBUG)
    	System.out.println("All closed");
      
      return rc;
    } // end of JdbcDisconnect
    
    public int GetMaxValue(int n) {
      int m = 0;
      
      try {
        switch (n) {
        case 1:        // Max columns in table
    	  m = dbmd.getMaxColumnsInTable();
    	  break;
        case 2:        // Max catalog name length
    	  m = dbmd.getMaxCatalogNameLength();
    	  break;
        case 3:        // Max schema name length
    	  m = dbmd.getMaxSchemaNameLength();
    	  break;
        case 4:        // Max table name length
    	  m = dbmd.getMaxTableNameLength();
    	  break;
        case 5:        // Max column name length
    	  m = dbmd.getMaxColumnNameLength();
    	  break;
        } // endswitch n
      
      } catch(Exception e) {
  		SetErrmsg(e);
  		m = -1;
      } // end try/catch
      
      return m;
    } // end of GetMaxValue
    
    public int GetColumns(String[] parms) {
      int ncol = -1;
      
      try {
  		if (rs != null) rs.close();
    	rs = dbmd.getColumns(parms[0], parms[1], parms[2], parms[3]);
    	
		if (rs != null) {
		  rsmd = rs.getMetaData();
    	  ncol = rsmd.getColumnCount();
		} // endif rs
		
      } catch(SQLException se) {
  		SetErrmsg(se);
      } // end try/catch
      
      return ncol;
    } // end of GetColumns
    
    public int GetTables(String[] parms) {
        int ncol = 0;
        String[] typ = null;
        
        if (parms[3] != null) {
          typ = new String[1];
          typ[0] = parms[3];
        } // endif parms
        
        try {
    	  if (rs != null) rs.close();
      	  rs = dbmd.getTables(parms[0], parms[1], parms[2], typ);
      	
  		  if (rs != null) {
  		    rsmd = rs.getMetaData();
      	    ncol = rsmd.getColumnCount();
  		  } // endif rs
  		
        } catch(SQLException se) {
    	  SetErrmsg(se);
        } // end try/catch
        
        return ncol;
      } // end of GetColumns
      
    public int Execute(String query) {
	      int n = 0;
	      
	      if (DEBUG)
			System.out.println("Executing '" + query + "'");
	    	
	      try {
	    	boolean b = stmt.execute(query);
	    	
	    	if (b == false) {
	    	  n = stmt.getUpdateCount();
	    	  if (rs != null) rs.close();
	    	} // endif b
	    	
	    	if (DEBUG)
			  System.out.println("Query '" + query + "' executed: n = " + n);
	    		
	      } catch (SQLException se) {
	  		SetErrmsg(se);
			n = -1;
	      } catch (Exception e) {
	  		SetErrmsg(e);
	      	n = -2;
	      } //end try/catch

	      return n;
	    } // end of Execute
    
    public int GetResult() {
    	int ncol = 0;
    	
    	try {
    		rs = stmt.getResultSet();
    		
    		if (rs != null) {
			  rsmd = rs.getMetaData();
	    	  ncol = rsmd.getColumnCount();
	    	
	    	  if (DEBUG)
			    System.out.println("Result set has " + rsmd.getColumnCount() + " column(s)");
	    	
    		} // endif rs
	    		
	    } catch (SQLException se) {
			SetErrmsg(se);
			ncol = -1;
	      } catch (Exception e) {
	  		SetErrmsg(e);
	    	ncol = -2;
	    } //end try/catch

	    return ncol;
    } // end of GetResult
	    
    public int ExecuteQuery(String query) {
      int ncol = 0;
      
      if (DEBUG)
		System.out.println("Executing query '" + query + "'");
    	
      try {
    	rs = stmt.executeQuery(query);
		rsmd = rs.getMetaData();
    	ncol = rsmd.getColumnCount();
    	
    	if (DEBUG) {
		  System.out.println("Query '" + query + "' executed successfully");
		  System.out.println("Result set has " + rsmd.getColumnCount() + " column(s)");
    	} // endif DEBUG
    		
      } catch (SQLException se) {
  		SetErrmsg(se);
		ncol = -1;
      } catch (Exception e) {
  		SetErrmsg(e);
  		ncol = -2;
      } //end try/catch

      return ncol;
    } // end of ExecuteQuery
    
    public int ExecuteUpdate(String query) {
        int n = 0;
        
        if (DEBUG)
  		  System.out.println("Executing update query '" + query + "'");
      	
        try {
      	  n = stmt.executeUpdate(query);
      	
      	  if (DEBUG)
  		    System.out.println("Update Query '" + query + "' executed: n = " + n);
      		
        } catch (SQLException se) {
    	  SetErrmsg(se);
  		  n = -1;
        } catch (Exception e) {
    	  SetErrmsg(e);
    	  n = -2;
        } //end try/catch

        return n;
    } // end of ExecuteUpdate
    
    public int ReadNext() {
	  if (rs != null) {
	    try {
	  	  return rs.next() ? 1 : 0;
		} catch (SQLException se) {
		  SetErrmsg(se);
		  return -1;
		} //end try/catch
	    	  
	  } else
	    return 0;
	      
	} // end of ReadNext
	      
    public boolean Fetch(int row) {
	  if (rs != null) {
	    try {
	  	  return rs.absolute(row);
		} catch (SQLException se) {
		  SetErrmsg(se);
		  return false;
		} //end try/catch
	    	  
	  } else
	    return false;
	      
	} // end of Fetch
	      
    public String ColumnName(int n) {
      if (rsmd == null) {
		System.out.println("No result metadata");
      } else try {
    	return rsmd.getColumnLabel(n);
      } catch (SQLException se) {
  		SetErrmsg(se);
      } //end try/catch
    	  
      return null;  
    } // end of ColumnName
    
    public int ColumnType(int n, String name) {
	  if (rsmd == null) {
		System.out.println("No result metadata");
	  } else try {
		if (n == 0)
		  n = rs.findColumn(name);
		
	    return rsmd.getColumnType(n);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return 666;   // Not a type
	} // end of ColumnType
	    
    public String ColumnDesc(int n, int[] val) {
	  if (rsmd == null) {
		System.out.println("No result metadata");
		return null;
	  } else try {
		val[0] = rsmd.getColumnType(n);
		val[1] = rsmd.getPrecision(n);
		val[2] = rsmd.getScale(n);
		val[3] = rsmd.isNullable(n);
	    return rsmd.getColumnLabel(n);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of ColumnDesc
	    
    public String StringField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getString(n) : rs.getString(name);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of StringField
	    
    public int IntField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getInt(n) : rs.getInt(name);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return 0;  
	} // end of IntField
	    
    public long BigintField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
		BigDecimal bigDecimal = (n > 0) ? rs.getBigDecimal(n) : rs.getBigDecimal(name);
        return bigDecimal != null ? bigDecimal.longValue() : 0;
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return 0;  
	} // end of BiginttField
	    
    public double DoubleField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getDouble(n) : rs.getDouble(name);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return 0.;  
	} // end of DoubleField
	    
    public float FloatField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getFloat(n) : rs.getFloat(name);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return 0;  
	} // end of FloatField
	    
    public boolean BooleanField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getBoolean(n) : rs.getBoolean(name);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return false;  
	} // end of BooleanField
	    
    public Date DateField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getDate(n) : rs.getDate(name);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of DateField
	    
    public Time TimeField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getTime(n) : rs.getTime(name);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of TimeField
	    
    public Timestamp TimestampField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getTimestamp(n) : rs.getTimestamp(name);
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of TimestampField
    
    public String ObjectField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getObject(n).toString() : rs.getObject(name).toString();
	  } catch (SQLException se) {
		SetErrmsg(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of ObjectField
	    
    public int GetDrivers(String[] s, int mxs) {
    	int n = 0;
    	List<Driver> drivers = Collections.list(DriverManager.getDrivers());
    	int size = Math.min(mxs, drivers.size());
		
    	for (int i = 0; i < size; i++) {
    		Driver driver = (Driver)drivers.get(i);

    		// Get name of driver
    		s[n++] = driver.getClass().getName();
	    
    		// Get version info
    		s[n++] = driver.getMajorVersion() + "." + driver.getMinorVersion();
    		s[n++] = driver.jdbcCompliant() ? "Yes" : "No";
    		s[n++] = driver.toString();
    	} // endfor i
    	
    	return size;
    } // end of GetDrivers
    
    /**
    * Adds the specified path to the java library path
    * from Fahd Shariff blog
    *
    * @param pathToAdd the path to add
        static public int addLibraryPath(String pathToAdd) {
		System.out.println("jpath = " + pathToAdd);

    	try {
    		Field usrPathsField = ClassLoader.class.getDeclaredField("usr_paths");
    		usrPathsField.setAccessible(true);

    		//get array of paths
    		String[] paths = (String[])usrPathsField.get(null);

    		//check if the path to add is already present
    		for (String path : paths) {
    			System.out.println("path = " + path);
    			
    			if (path.equals(pathToAdd))
    				return -5;
    			
    		} // endfor path

    		//add the new path
    		String[] newPaths = Arrays.copyOf(paths, paths.length + 1);
    		newPaths[paths.length] = pathToAdd;
    		usrPathsField.set(null, newPaths);
            System.setProperty("java.library.path",
            		System.getProperty("java.library.path") + File.pathSeparator + pathToAdd);
            Field fieldSysPath = ClassLoader.class.getDeclaredField("sys_paths");
            fieldSysPath.setAccessible(true);
            fieldSysPath.set(null, null);
    	} catch (Exception e) {
			SetErrmsg(e);
    		return -1;
    	} // end try/catch
    	
    	return 0;
    } // end of addLibraryPath
    */   
	    
} // end of class JdbcInterface
