/***********************************************************************/
/*  JavaConn.h : header file for the Java connection classes.          */
/***********************************************************************/

/***********************************************************************/
/*  Included C-definition files required by the interface.             */
/***********************************************************************/
#include "block.h"
#include "jdbccat.h"

/***********************************************************************/
/*  Java native interface.                                             */
/***********************************************************************/
#include <jni.h>

/***********************************************************************/
/*  Constants and defines.                                             */
/***********************************************************************/
//  Miscellaneous sizing info
#define MAX_NUM_OF_MSG   10     // Max number of error messages
//efine MAX_CURRENCY     30     // Max size of Currency($) string
#define MAX_TNAME_LEN    32     // Max size of table names
//efine MAX_FNAME_LEN    256    // Max size of field names
//efine MAX_STRING_INFO  256    // Max size of string from SQLGetInfo
//efine MAX_DNAME_LEN    256    // Max size of Recordset names
//efine MAX_CONNECT_LEN  512    // Max size of Connect string
//efine MAX_CURSOR_NAME  18     // Max size of a cursor name
//efine DEFAULT_FIELD_TYPE 0    // TYPE_NULL

#if !defined(_WIN32)
typedef unsigned char *PUCHAR;
#endif   // !_WIN32

enum JCATINFO {
	JCAT_TAB = 1,      // JDBC Tables
	JCAT_COL = 2,      // JDBC Columns
	JCAT_KEY = 3,      // JDBC PrimaryKeys
};

/***********************************************************************/
/*  This structure is used to control the catalog functions.           */
/***********************************************************************/
typedef struct tagJCATPARM {
	JCATINFO Id;                 // Id to indicate function 
	PQRYRES  Qrp;                // Result set pointer
	PCSZ     DB;                 // Database (Schema)
	PCSZ     Tab;                // Table name or pattern
	PCSZ     Pat;                // Table type or column pattern
} JCATPARM;

typedef jint(JNICALL *CRTJVM) (JavaVM **, void **, void *);
typedef jint(JNICALL *GETJVM) (JavaVM **, jsize, jsize *);
#if defined(_DEBUG)
typedef jint(JNICALL *GETDEF) (void *);
#endif   // _DEBUG

//class JAVAConn;

/***********************************************************************/
/*  JAVAConn class.                                                    */
/***********************************************************************/
class DllExport JAVAConn : public BLOCK {
	friend class TDBJMG;
	friend class JMGDISC;
private:
	JAVAConn();                      // Standard (unused) constructor

public:
	// Constructor
	JAVAConn(PGLOBAL g, PCSZ wrapper);

	// Set static variables
	static void SetJVM(void) {
		LibJvm = NULL;
		CreateJavaVM = NULL;
		GetCreatedJavaVMs = NULL;
#if defined(_DEBUG)
		GetDefaultJavaVMInitArgs = NULL;
#endif   // _DEBUG
	}	// end of SetJVM

	static void ResetJVM(void);
	static bool GetJVM(PGLOBAL g);

	// Implementation
public:
	//virtual ~JAVAConn();
	bool IsOpen(void) { return m_Opened; }
	bool IsConnected(void) { return m_Connected; }

	// Java operations
protected:
	char *GetUTFString(jstring s);
	bool gmID(PGLOBAL g, jmethodID& mid, const char *name, const char *sig);
	bool Check(jint rc = 0);

public:
	virtual void AddJars(PSTRG jpop, char sep) = 0;
	virtual bool Connect(PJPARM sop) = 0;
	virtual bool Open(PGLOBAL g);
	virtual bool MakeCursor(PGLOBAL g, PTDB tdbp, PCSZ options, 
		                                            PCSZ filter, bool pipe) = 0;
	virtual void Close(void);

protected:
	// Members
#if defined(_WIN32)
	static HANDLE LibJvm;              // Handle to the jvm DLL
#else   // !_WIN32
	static void  *LibJvm;              // Handle for the jvm shared library
#endif  // !_WIN32
	static CRTJVM CreateJavaVM;
	static GETJVM GetCreatedJavaVMs;
#if defined(_DEBUG)
	static GETDEF GetDefaultJavaVMInitArgs;
#endif   // _DEBUG
	PGLOBAL   m_G;
	JavaVM   *jvm;                      // Pointer to the JVM (Java Virtual Machine)
	JNIEnv   *env;                      // Pointer to native interface
	jclass    jdi;											// Pointer to the java wrapper class
	jobject   job;											// The java wrapper class object
	jmethodID errid;										// The GetErrmsg method ID
	PFBLOCK   fp;
	bool      m_Opened;
	bool      m_Connected;
	PCSZ      DiscFunc;
	PCSZ      Msg;
	PCSZ      m_Wrap;
	int       m_Rows;
}; // end of JAVAConn class definition
