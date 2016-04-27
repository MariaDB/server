import java.math.*;
import java.sql.*;
import java.util.Collections;
import java.util.List;

public class JdbcInterface {
	boolean           DEBUG = false;
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

    public int JdbcConnect(String[] parms, int fsize, boolean scrollable) {
      int rc = 0;
      
      if (DEBUG)
      	System.out.println("In JdbcInterface: driver=" + parms[0]);
      
      try {
		if (DEBUG)
		  System.out.println("In try block");
			      
		if (parms[0] != null && !parms[0].isEmpty()) {
		  System.out.println("b is true!");
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
	    System.err.println("ClassNotFoundException: " + e.getMessage());
	    rc = 1; 
	  } catch (SQLException se) {
		System.out.println("SQL Exception:");

		// Loop through the SQL Exceptions
		while (se != null) {
			System.out.println("State  : " + se.getSQLState());
		    System.out.println("Message: " + se.getMessage());
		    System.out.println("Error  : " + se.getErrorCode());

		    se = se.getNextException();
		} // end while se
			
	    rc = 2; 
	  } catch( Exception e ) {
		System.out.println(e);
	    rc = 3; 
	  } // end try/catch
      
      return rc;
    } // end of JdbcConnect
    
    public boolean CreatePrepStmt(String sql) {
    	boolean b = false;
    	
    	try {
    		pstmt = conn.prepareStatement(sql);
    	} catch (SQLException se) {
    		System.out.println(se);
    	    b = true; 
    	} catch (Exception e) {
    		System.out.println(e);
    	    b = true; 
    	} // end try/catch
    	
    	return b;
    } // end of CreatePrepStmt
    
    public void SetStringParm(int i, String s) {
    	try {
    		pstmt.setString(i, s);
    	} catch (Exception e) {
    		System.out.println(e);
    	} // end try/catch
    	
    } // end of SetStringParm
    
    public void SetIntParm(int i, int n) {
    	try {
    		pstmt.setInt(i, n);
    	} catch (Exception e) {
    		System.out.println(e);
    	} // end try/catch
    	
    } // end of SetIntParm
    
    public void SetShortParm(int i, short n) {
    	try {
    		pstmt.setShort(i, n);
    	} catch (Exception e) {
    		System.out.println(e);
    	} // end try/catch
    	
    } // end of SetShortParm
    
    public void SetBigintParm(int i, long n) {
    	try {
    		pstmt.setLong(i, n);
    	} catch (Exception e) {
    		System.out.println(e);
    	} // end try/catch
    	
    } // end of SetBigintParm
    
    public void SetFloatParm(int i, float f) {
    	try {
    		pstmt.setFloat(i, f);
    	} catch (Exception e) {
    		System.out.println(e);
    	} // end try/catch
    	
    } // end of SetFloatParm
    
    public void SetDoubleParm(int i, double d) {
    	try {
    		pstmt.setDouble(i, d);
    	} catch (Exception e) {
    		System.out.println(e);
    	} // end try/catch
    	
    } // end of SetDoubleParm
    
    public void SetTimestampParm(int i, Timestamp t) {
    	try {
    		pstmt.setTimestamp(i, t);
    	} catch (Exception e) {
    		System.out.println(e);
    	} // end try/catch
    	
    } // end of SetTimestampParm
    
    public int ExecutePrep() {
        int n = -3;
        
        if (pstmt != null) try {
      	  n = pstmt.executeUpdate();
        } catch (SQLException se) {
  		  System.out.println(se);
  		  n = -1;
        } catch (Exception e) {
    	  System.out.println(e);
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
    		System.out.println(se);
    	    b = true; 
    	} catch (Exception e) {
    		System.out.println(e);
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
		  System.out.println(se);
		  rc += 1;
	    } // nothing more we can do
		      
	  // Close the statement and the connection
	  if (rs != null)
		try {
		  System.out.println("Closing result set");
		  rs.close();
		} catch(SQLException se) {
		  System.out.println(se);
		  rc = 2;
	    } // nothing more we can do
		      
	  if (stmt != null)
		try {
		  System.out.println("Closing statement");
		  stmt.close();
		} catch(SQLException se) {
		  System.out.println(se);
		  rc += 4;
	    } // nothing more we can do
	  
	  ClosePrepStmt();
		      
      if (conn != null)
		try {
		  System.out.println("Closing connection");
		  conn.close();
	    } catch (SQLException se) {
		  System.out.println(se);
		  rc += 8;
	    } //end try/catch
	
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
		System.out.println(e);
      } // end try/catch
      
      return m;
    } // end of GetMaxValue
    
    public int GetColumns(String[] parms) {
      int ncol = 0;
      
      try {
  		if (rs != null) rs.close();
    	rs = dbmd.getColumns(parms[0], parms[1], parms[2], parms[3]);
    	
		if (rs != null) {
		  rsmd = rs.getMetaData();
    	  ncol = rsmd.getColumnCount();
		} // endif rs
		
      } catch(SQLException se) {
		System.out.println(se);
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
  		  System.out.println(se);
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
			System.out.println(se);
			n = -1;
	      } catch (Exception e) {
	      	System.out.println(e);
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
			System.out.println(se);
			ncol = -1;
	      } catch (Exception e) {
	    	System.out.println(e);
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
		System.out.println(se);
		ncol = -1;
      } catch (Exception e) {
  		System.out.println(e);
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
  		  System.out.println(se);
  		  n = -1;
        } catch (Exception e) {
    	  System.out.println(e);
    	  n = -2;
        } //end try/catch

        return n;
    } // end of ExecuteQuery
    
    public int ReadNext() {
	  if (rs != null) {
	    try {
	  	  return rs.next() ? 1 : 0;
		} catch (SQLException se) {
		  System.out.println(se);
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
		  System.out.println(se);
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
		System.out.println(se);
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
		System.out.println("ColumnType: " + se);
	  } //end try/catch
	    	  
	  return 0;  
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
		System.out.println(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of ColumnType
	    
    public String StringField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getString(n) : rs.getString(name);
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of StringField
	    
    public int IntField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getInt(n) : rs.getInt(name);
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return 0;  
	} // end of IntField
	    
    public long BigintField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
		BigDecimal bigDecimal = (n > 0) ? rs.getBigDecimal(n) : rs.getBigDecimal(name);
        return bigDecimal != null ? bigDecimal.longValue() : null;
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return 0;  
	} // end of BiginttField
	    
    public double DoubleField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getDouble(n) : rs.getDouble(name);
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return 0.;  
	} // end of DoubleField
	    
    public float FloatField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getFloat(n) : rs.getFloat(name);
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return 0;  
	} // end of FloatField
	    
    public boolean BooleanField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getBoolean(n) : rs.getBoolean(name);
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return false;  
	} // end of BooleanField
	    
    public Date DateField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getDate(n) : rs.getDate(name);
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of DateField
	    
    public Time TimeField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getTime(n) : rs.getTime(name);
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of TimeField
	    
    public Timestamp TimestampField(int n, String name) {
	  if (rs == null) {
		System.out.println("No result set");
	  } else try {
	    return (n > 0) ? rs.getTimestamp(n) : rs.getTimestamp(name);
	  } catch (SQLException se) {
		System.out.println(se);
	  } //end try/catch
	    	  
	  return null;  
	} // end of TimestampField
    
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
	    
} // end of class JdbcInterface
