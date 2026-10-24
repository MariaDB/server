/************ Javaconn C++ Functions Source Code File (.CPP) ***********/
/*  Name: JAVAConn.CPP  Version 1.1                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2017 - 2021  */
/*                                                                     */
/*  This file contains the JAVA connection classes functions.          */
/***********************************************************************/

#if defined(_WIN32)
// This is needed for RegGetValue
#define _WINVER 0x0601
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif   // _WIN32

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include <my_global.h>
//#include <m_string.h>
#if defined(_WIN32)
#include <direct.h>                      // for getcwd
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif   // __BORLANDC__
#else   // !_WIN32
#if defined(UNIX)
#include <errno.h>
#else   // !UNIX
#endif  // !UNIX
#include <stdio.h>
#include <stdlib.h>                      // for getenv
#define NODW
#endif  // !_WIN32

#include <m_string.h>

/***********************************************************************/
/*  Required objects includes.                                         */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "colblk.h"
#include "xobject.h"
#include "xtable.h"
#include "tabext.h"
#include "javaconn.h"
#include "resource.h"
#include "valblk.h"
#include "osutil.h"

#if defined(_WIN32)
extern "C" HINSTANCE s_hModule;           // Saved module handle
#endif   // _WIN32
#define nullptr 0

//TYPCONV GetTypeConv();
//int GetConvSize();
extern char *JvmPath;   // The connect_jvm_path global variable value
extern char *ClassPath; // The connect_class_path global variable value

char *GetPluginDir(void);
char *GetMessageDir(void);
char *GetJavaWrapper(void);		// The connect_java_wrapper variable value
extern MYSQL_PLUGIN_IMPORT char lc_messages_dir[FN_REFLEN];

/***********************************************************************/
/*  Static JAVAConn objects.                                           */
/***********************************************************************/
void  *JAVAConn::LibJvm = NULL;
CRTJVM JAVAConn::CreateJavaVM = NULL;
GETJVM JAVAConn::GetCreatedJavaVMs = NULL;
#if defined(_DEBUG)
GETDEF JAVAConn::GetDefaultJavaVMInitArgs = NULL;
#endif   // _DEBUG

/***********************************************************************/
/*  Some macro's (should be defined elsewhere to be more accessible)   */
/***********************************************************************/
#if defined(_DEBUG)
#define ASSERT(f)          assert(f)
#define DEBUG_ONLY(f)      (f)
#else   // !_DEBUG
#define ASSERT(f)          ((void)0)
#define DEBUG_ONLY(f)      ((void)0)
#endif  // !_DEBUG

/***********************************************************************/
/*  JAVAConn construction/destruction.                                 */
/***********************************************************************/
JAVAConn::JAVAConn(PGLOBAL g, PCSZ wrapper)
{
	m_G = g;
	jvm = nullptr;            // Pointer to the JVM (Java Virtual Machine)
	env = nullptr;            // Pointer to native interface
	jdi = nullptr;						// Pointer to the java wrapper class
	job = nullptr;						// The java wrapper class object
	errid = nullptr;
	DiscFunc = "Disconnect";
	Msg = NULL;
	m_Wrap = (wrapper) ? wrapper : GetJavaWrapper();

	if (!strchr(m_Wrap, '/')) {
		// Add the wrapper package name
		char *wn = (char*)PlugSubAlloc(g, NULL, strlen(m_Wrap) + 10);
		m_Wrap = strcat(strcpy(wn, "wrappers/"), m_Wrap);
	} // endif m_Wrap

	fp = NULL;
	m_Opened = false;
	m_Connected = false;
	m_Rows = 0;
//*m_ErrMsg = '\0';
} // end of JAVAConn

//JAVAConn::~JAVAConn()
//  {
//if (Connected())
//  EndCom();

//  } // end of ~JAVAConn
char *JAVAConn::GetUTFString(jstring s)
{
	char *str;
	const char *utf = env->GetStringUTFChars(s, nullptr);

	str = PlugDup(m_G, utf);
	env->ReleaseStringUTFChars(s, utf);
	env->DeleteLocalRef(s);
	return str;
}	// end of GetUTFString

/***********************************************************************/
/*  Screen for errors.                                                 */
/***********************************************************************/
bool JAVAConn::Check(jint rc)
{
	jstring s;

	if (env->ExceptionCheck()) {
		jthrowable exc = env->ExceptionOccurred();
		jmethodID tid = env->GetMethodID(env->FindClass("java/lang/Object"),
			"toString", "()Ljava/lang/String;");

		if (exc != nullptr && tid != nullptr) {
			s = (jstring)env->CallObjectMethod(exc, tid);
			Msg = GetUTFString(s);
		} else
			Msg = "Exception occurred";

		env->ExceptionClear();
	} else if (rc < 0) {
		s = (jstring)env->CallObjectMethod(job, errid);
		Msg = GetUTFString(s);
	} else
		Msg = NULL;

	return (Msg != NULL);
} // end of Check

/***********************************************************************/
/*  Get MethodID if not exists yet.                                    */
/***********************************************************************/
bool JAVAConn::gmID(PGLOBAL g, jmethodID& mid, const char *name, const char *sig)
{
	if (mid == nullptr) {
		mid = env->GetMethodID(jdi, name, sig);

		if (Check()) {
			snprintf(g->Message, sizeof(g->Message), "%s", Msg);
			return true;
		} else
			return false;

	} else
		return false;

} // end of gmID

#if 0
/***********************************************************************/
/*  Utility routine.                                                   */
/***********************************************************************/
int JAVAConn::GetMaxValue(int n)
{
	jint      m;
	jmethodID maxid = nullptr;

	if (gmID(m_G, maxid, "GetMaxValue", "(I)I"))
		return -1;

	// call method
	if (Check(m = env->CallIntMethod(job, maxid, n)))
		htrc("GetMaxValue: %s", Msg);

	return (int)m;
} // end of GetMaxValue
#endif // 0

/***********************************************************************/
/*  Reset the JVM library.                                             */
/***********************************************************************/
void JAVAConn::ResetJVM(void)
{
	if (LibJvm) {
#if defined(_WIN32)
		FreeLibrary((HMODULE)LibJvm);
#else   // !_WIN32
		dlclose(LibJvm);
#endif  // !_WIN32
		LibJvm = NULL;
		CreateJavaVM = NULL;
		GetCreatedJavaVMs = NULL;
#if defined(_DEBUG)
		GetDefaultJavaVMInitArgs = NULL;
#endif   // _DEBUG
	} // endif LibJvm

} // end of ResetJVM

#if defined(_WIN32)
/*
  Remember the error of a JVM probe. Only the first error that is not
  "not found" is kept, so that e.g. a wrong bitness is not hidden by the
  next probe. err is initially ERROR_MOD_NOT_FOUND.
*/
static void set_jvm_error(DWORD *err, DWORD error)
{
  if (*err == ERROR_MOD_NOT_FOUND || *err == ERROR_PATH_NOT_FOUND)
    *err= error;
}


/* Check the snprintf() result for a truncated path; record the error. */
static bool path_fits(int len, size_t size, DWORD *err)
{
  if (len < int(size))
    return true;
  set_jvm_error(err, ERROR_FILENAME_EXCED_RANGE);
  return false;
}


/* Load jvm.dll from the path. */
static HMODULE load_jvm(const char *path, DWORD *err)
{
  HMODULE h;

  if (!(h= LoadLibrary(path)))
    set_jvm_error(err, GetLastError());
  return h;
}


/* Read a string value from HKLM, a missing value is not an error. */
static bool get_registry_string(const char *key, const char *name, char *buf,
                                DWORD size, DWORD *err)
{
  LSTATUS rc= RegGetValue(HKEY_LOCAL_MACHINE, key, name,
                          RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, buf,
                          &size);
  if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND)
    set_jvm_error(err, rc == ERROR_MORE_DATA ? ERROR_FILENAME_EXCED_RANGE : rc);
  return rc == ERROR_SUCCESS;
}


/*
  Load jvm.dll from a JDK or JRE home. Like java.exe, prefer "server" (the
  only JVM since JDK 9) over "client", also in the jre subdirectory of JDK 8.
*/
static HMODULE load_jvm_from_home(const char *home, DWORD *err)
{
  static const char *dirs[]=
  {
    "\\bin\\server", "\\bin\\client", "\\jre\\bin\\server", "\\jre\\bin\\client"
  };
  char path[MAX_PATH];
  HMODULE h= NULL;

  for (uint i= 0; !h && i < array_elements(dirs); i++)
  {
    if (path_fits(snprintf(path, sizeof(path), "%s%s\\jvm.dll", home, dirs[i]),
                  sizeof(path), err))
      h= load_jvm(path, err);
  }
  return h;
}


/*
  Load jvm.dll using the registry keys of Java installers: JavaSoft\{JDK|JRE}
  has CurrentVersion, naming a subkey with RuntimeLib (JRE) or JavaHome.
  JDK 8 used the longer key names.
*/
static HMODULE load_jvm_from_registry(DWORD *err)
{
  static const char *keys[]=
  {
    "SOFTWARE\\JavaSoft\\JDK", "SOFTWARE\\JavaSoft\\JRE",
    "SOFTWARE\\JavaSoft\\Java Development Kit",
    "SOFTWARE\\JavaSoft\\Java Runtime Environment"
  };
  HMODULE h= NULL;

  for (uint i= 0; !h && i < array_elements(keys); i++)
  {
    char version[MAX_PATH], subkey[MAX_PATH], path[MAX_PATH];

    if (!get_registry_string(keys[i], "CurrentVersion", version,
                             sizeof(version), err))
      continue;

    if (!path_fits(snprintf(subkey, sizeof(subkey), "%s\\%s", keys[i], version),
                   sizeof(subkey), err))
      continue;
    if (get_registry_string(subkey, "RuntimeLib", path, sizeof(path), err))
      h= load_jvm(path, err);
    if (!h && get_registry_string(subkey, "JavaHome", path, sizeof(path), err))
      h= load_jvm_from_home(path, err);
  }
  return h;
}
#endif /* _WIN32 */

/***********************************************************************/
/*  Dynamically link the JVM library.                                  */
/*  The purpose of this function is to allow using the CONNECT plugin  */
/*  for other table types when the Java JDK is not installed.          */
/***********************************************************************/
bool JAVAConn::GetJVM(PGLOBAL g)
{
	if (!LibJvm) {
#if defined(_WIN32)
		DWORD err = ERROR_MOD_NOT_FOUND;
		char  path[MAX_PATH];

		const char *what = "jvm.dll (set JAVA_HOME or connect_jvm_path)";

		if (JvmPath) {
			// An explicitly configured path is exclusive, no fallback
			what = JvmPath;
			if (path_fits(snprintf(path, sizeof(path), "%s\\jvm.dll", JvmPath),
			              sizeof(path), &err))
				LibJvm = load_jvm(path, &err);
		} else {
			const char *home = getenv("JAVA_HOME");

			if (home && *home)
				LibJvm = load_jvm_from_home(home, &err);

			if (!LibJvm)
				LibJvm = load_jvm_from_registry(&err);
		}

		// Get the needed entries
		if (!LibJvm) {
			char  buf[256];
			DWORD rc = err;

			FormatMessage(FORMAT_MESSAGE_FLAGS, NULL, rc, 0,
				            (LPTSTR)buf, sizeof(buf), NULL);
			snprintf(g->Message, sizeof(g->Message), MSG(DLL_LOAD_ERROR)": %s", rc,
							 what, buf);
		} else if (!(CreateJavaVM = (CRTJVM)GetProcAddress((HINSTANCE)LibJvm,
			                                       "JNI_CreateJavaVM"))) {
			snprintf(g->Message, sizeof(g->Message), MSG(PROCADD_ERROR), GetLastError(), "JNI_CreateJavaVM");
			FreeLibrary((HMODULE)LibJvm);
			LibJvm = NULL;
		} else if (!(GetCreatedJavaVMs = (GETJVM)GetProcAddress((HINSTANCE)LibJvm,
			                                       "JNI_GetCreatedJavaVMs"))) {
			snprintf(g->Message, sizeof(g->Message), MSG(PROCADD_ERROR), GetLastError(), "JNI_GetCreatedJavaVMs");
			FreeLibrary((HMODULE)LibJvm);
			LibJvm = NULL;
#if defined(_DEBUG)
		} else if (!(GetDefaultJavaVMInitArgs = (GETDEF)GetProcAddress((HINSTANCE)LibJvm,
			                                       "JNI_GetDefaultJavaVMInitArgs"))) {
			snprintf(g->Message, sizeof(g->Message), MSG(PROCADD_ERROR), GetLastError(),
				"JNI_GetDefaultJavaVMInitArgs");
			FreeLibrary((HMODULE)LibJvm);
			LibJvm = NULL;
#endif   // _DEBUG
		} // endif LibJvm
#else   // !_WIN32
		char soname[FN_REFLEN];
		int ntry;
		const char *error = NULL;

		for (ntry = 0; !LibJvm && ntry < 2; ntry++) {
			if (!ntry && JvmPath) {
				snprintf(soname, sizeof(soname), "%s/libjvm.so", JvmPath);
				ntry = 2;
			} else if (!ntry && getenv("JAVA_HOME")) {
				// TODO: Replace i386 by a better guess
				snprintf(soname, sizeof(soname), "%s/jre/lib/i386/client/libjvm.so",
								 getenv("JAVA_HOME"));
			} else {	 // Will need LD_LIBRARY_PATH to be set
				safe_strcpy(soname, sizeof(soname), "libjvm.so");
				ntry = 2;
			} // endelse

			LibJvm = dlopen(soname, RTLD_LAZY);
		} // endfor ntry

			// Load the desired shared library
		if (!LibJvm) {
			error = dlerror();
			snprintf(g->Message, sizeof(g->Message), MSG(SHARED_LIB_ERR), soname, SVP(error));
		} else if (!(CreateJavaVM = (CRTJVM)dlsym(LibJvm, "JNI_CreateJavaVM"))) {
			error = dlerror();
			snprintf(g->Message, sizeof(g->Message), MSG(GET_FUNC_ERR), "JNI_CreateJavaVM", SVP(error));
			dlclose(LibJvm);
			LibJvm = NULL;
		} else if (!(GetCreatedJavaVMs = (GETJVM)dlsym(LibJvm, "JNI_GetCreatedJavaVMs"))) {
			error = dlerror();
			snprintf(g->Message, sizeof(g->Message), MSG(GET_FUNC_ERR), "JNI_GetCreatedJavaVMs", SVP(error));
			dlclose(LibJvm);
			LibJvm = NULL;
#if defined(_DEBUG)
		} else if (!(GetDefaultJavaVMInitArgs = (GETDEF)dlsym(LibJvm,
			"JNI_GetDefaultJavaVMInitArgs"))) {
			error = dlerror();
			snprintf(g->Message, sizeof(g->Message), MSG(GET_FUNC_ERR), "JNI_GetDefaultJavaVMInitArgs", SVP(error));
			dlclose(LibJvm);
			LibJvm = NULL;
#endif   // _DEBUG
		} // endif LibJvm
#endif  // !_WIN32

	} // endif LibJvm

	return LibJvm == NULL;
} // end of GetJVM

/***********************************************************************/
/*  Open: connect to a data source.                                    */
/***********************************************************************/
bool JAVAConn::Open(PGLOBAL g)
{
         bool		 brc = true;
	jboolean jt = (trace(1));

	// Link or check whether jvm library was linked
	if (GetJVM(g))
		return true;

	// Firstly check whether the jvm was already created
	JavaVM* jvms[1];
	jsize   jsz;
	jint    rc = GetCreatedJavaVMs(jvms, 1, &jsz);

	if (rc == JNI_OK && jsz == 1) {
		// jvm already existing
		jvm = jvms[0];
		rc = jvm->AttachCurrentThread((void**)&env, nullptr);

		if (rc != JNI_OK) {
			snprintf(g->Message, sizeof(g->Message), "Cannot attach jvm to the current thread");
			return true;
		} // endif rc

	} else {
		/*******************************************************************/
		/*  Create a new jvm																							 */
		/*******************************************************************/
		PSTRG    jpop = new(g)STRING(g, 512, "-Djava.class.path=.");
		char    *cp = NULL;
		char     sep;

#if defined(_WIN32)
		sep = ';';
#define N 1
		//#define N 2
		//#define N 3
#else
		sep = ':';
#define N 1
#endif

		// Add wrappers jar files 
		AddJars(jpop, sep);

		//================== prepare loading of Java VM ============================
		JavaVMInitArgs vm_args;                        // Initialization arguments
		JavaVMOption* options = new JavaVMOption[N];   // JVM invocation options

		// where to find java .class
		if (ClassPath && *ClassPath) {
			jpop->Append(sep);
			jpop->Append(ClassPath);
		}	// endif ClassPath

		jpop->Append(sep);
		jpop->Append(GetMessageDir());
		jpop->Append("JdbcInterface.jar");

#if defined(MONGO_SUPPORT)
		jpop->Append(sep);
		jpop->Append(GetMessageDir());
		jpop->Append("Mongo3.jar");
		jpop->Append(sep);
		jpop->Append(GetMessageDir());
		jpop->Append("Mongo2.jar");
#endif   // MONGO_SUPPORT

		if ((cp = getenv("CLASSPATH"))) {
			jpop->Append(sep);
			jpop->Append(cp);
		} // endif cp

		if (trace(1)) {
			htrc("ClassPath=%s\n", ClassPath ? ClassPath : "null");
			htrc("CLASSPATH=%s\n", cp ? cp : "null");
			htrc("%s\n", jpop->GetStr());
		} // endif trace

		options[0].optionString = jpop->GetStr();
#if N == 2
		options[1].optionString = "-Xcheck:jni";
#endif
#if N == 3
		options[1].optionString = "-Xms256M";
		options[2].optionString = "-Xmx512M";
#endif
#if defined(_DEBUG)
		vm_args.version = JNI_VERSION_1_2;             // minimum Java version
		rc = GetDefaultJavaVMInitArgs(&vm_args);
#else
		vm_args.version = JNI_VERSION_1_6;             // minimum Java version
#endif   // _DEBUG
		vm_args.nOptions = N;                          // number of options
		vm_args.options = options;
		vm_args.ignoreUnrecognized = false; // invalid options make the JVM init fail

		//=============== load and initialize Java VM and JNI interface =============
		rc = CreateJavaVM(&jvm, (void**)&env, &vm_args);  // YES !!
		delete[] options;    // we then no longer need the initialisation options.

		switch (rc) {
			case JNI_OK:
				snprintf(g->Message, sizeof(g->Message), "VM successfully created");
				brc = false;
				break;
			case JNI_ERR:
				snprintf(g->Message, sizeof(g->Message), "Initialising JVM failed: unknown error");
				break;
			case JNI_EDETACHED:
				snprintf(g->Message, sizeof(g->Message), "Thread detached from the VM");
				break;
			case JNI_EVERSION:
				snprintf(g->Message, sizeof(g->Message), "JNI version error");
				break;
			case JNI_ENOMEM:
				snprintf(g->Message, sizeof(g->Message), "Not enough memory");
				break;
			case JNI_EEXIST:
				snprintf(g->Message, sizeof(g->Message), "VM already created");
				break;
			case JNI_EINVAL:
				snprintf(g->Message, sizeof(g->Message), "Invalid arguments");
				break;
			default:
				snprintf(g->Message, sizeof(g->Message), "Unknown return code %d", (int)rc);
				break;
		} // endswitch rc

		if (trace(1))
			htrc("%s\n", g->Message);

		if (brc)
			return true;

		//=============== Display JVM version ===============
		jint ver = env->GetVersion();
		printf("JVM Version %d.%d\n", ((ver >> 16) & 0x0f), (ver & 0x0f));
	} // endif rc

	// try to find the java wrapper class
	jdi = env->FindClass(m_Wrap);

	if (jdi == nullptr) {
		snprintf(g->Message, sizeof(g->Message), "ERROR: class %s not found!", m_Wrap);
		return true;
	} // endif jdi

#if 0		// Suppressed because it does not make any usable change
	if (b && jpath && *jpath) {
		// Try to add that path the the jvm class path
		jmethodID alp = env->GetStaticMethodID(jdi, "addLibraryPath",
			"(Ljava/lang/String;)I");

		if (alp == nullptr) {
			env->ExceptionDescribe();
			env->ExceptionClear();
		} else {
			char *msg;
			jstring path = env->NewStringUTF(jpath);
			rc = env->CallStaticIntMethod(jdi, alp, path);

			if ((msg = Check(rc))) {
				snprintf(g->Message, sizeof(g->Message), msg);
				env->DeleteLocalRef(path);
				return RC_FX;
			} else switch (rc) {
				case JNI_OK:
					printf("jpath added\n");
					break;
				case JNI_EEXIST:
					printf("jpath already exist\n");
					break;
				case JNI_ERR:
				default:
					snprintf(g->Message, sizeof(g->Message), "Error adding jpath");
					env->DeleteLocalRef(path);
					return RC_FX;
			}	// endswitch rc

			env->DeleteLocalRef(path);
		}	// endif alp

	}	// endif jpath
#endif // 0

	// if class found, continue
	jmethodID ctor = env->GetMethodID(jdi, "<init>", "(Z)V");

	if (ctor == nullptr) {
		snprintf(g->Message, sizeof(g->Message), "ERROR: %s constructor not found!", m_Wrap);
		return true;
	} else
		job = env->NewObject(jdi, ctor, jt);

	if (job == nullptr) {
		snprintf(g->Message, sizeof(g->Message), "%s class object not constructed!", m_Wrap);
		return true;
	} // endif job

	// If the object is successfully constructed, 
	// we can then search for the method we want to call, 
	// and invoke it for the object:
	errid = env->GetMethodID(jdi, "GetErrmsg", "()Ljava/lang/String;");

	if (env->ExceptionCheck()) {
		snprintf(g->Message, sizeof(g->Message), "ERROR: method GetErrmsg() not found!");
		env->ExceptionDescribe();
		env->ExceptionClear();
		return true;
	} // endif Check

	/*********************************************************************/
	/*  Link a Fblock. This make possible to automatically close it      */
	/*  in case of error (throw).                                        */
	/*********************************************************************/
	PDBUSER dbuserp = (PDBUSER)g->Activityp->Aptr;

	fp = (PFBLOCK)PlugSubAlloc(g, NULL, sizeof(FBLOCK));
	fp->Type = TYPE_FB_JAVA;
	fp->Fname = NULL;
	fp->Next = dbuserp->Openlist;
	dbuserp->Openlist = fp;
	fp->Count = 1;
	fp->Length = 0;
	fp->Memory = NULL;
	fp->Mode = MODE_ANY;
	fp->File = this;
	fp->Handle = 0;

	m_Opened = true;
	return false;
} // end of Open

/***********************************************************************/
/*  Disconnect connection                                              */
/***********************************************************************/
void JAVAConn::Close()
{
	jint rc;

	if (m_Connected) {
		jmethodID did = nullptr;

		// Could have been detached in case of join
		rc = jvm->AttachCurrentThread((void**)&env, nullptr);

		if (gmID(m_G, did, DiscFunc, "()I"))
			printf("%s\n", Msg);
		else if (Check(env->CallIntMethod(job, did)))
			printf("%s: %s\n", DiscFunc, Msg);

		m_Connected = false;
	}	// endif m_Connected

	if ((rc = jvm->DetachCurrentThread()) != JNI_OK)
		printf("DetachCurrentThread: rc=%d\n", (int)rc);

	if (fp)
		fp->Count = 0;

	m_Opened = false;
} // end of Close
