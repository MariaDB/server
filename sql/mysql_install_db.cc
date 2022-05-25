/* Copyright (C) 2010-2011 Monty Program Ab & Vladislav Vaintroub

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
  mysql_install_db creates a new database instance (optionally as service)
  on Windows.
*/
#define DONT_DEFINE_VOID
#include "mariadb.h"
#include <my_getopt.h>
#include <m_string.h>
#include <password.h>

#include <windows.h>
#include <shellapi.h>
#include <accctrl.h>
#include <aclapi.h>
#include <ntsecapi.h>
#include <sddl.h>
struct IUnknown;
#include <shlwapi.h>
#include <winservice.h>

#include <string>

#define USAGETEXT \
"mysql_install_db.exe  Ver 1.00 for Windows\n" \
"Copyright (C) 2010-2011 Monty Program Ab & Vladislav Vaintroub\n" \
"This software comes with ABSOLUTELY NO WARRANTY. This is free software,\n" \
"and you are welcome to modify and redistribute it under the GPL v2 license\n" \
"Usage: mysql_install_db.exe [OPTIONS]\n" \
"OPTIONS:"

extern "C" const char* mysql_bootstrap_sql[];

static char default_datadir[MAX_PATH];
static int create_db_instance(const char *datadir);
static uint opt_silent;
static char datadir_buffer[FN_REFLEN];
static char mysqld_path[FN_REFLEN];
static char *opt_datadir;
static char *opt_service;
static char *opt_password;
static int  opt_port;
static int  opt_innodb_page_size;
static char *opt_socket;
static my_bool opt_default_user;
static my_bool opt_allow_remote_root_access;
static my_bool opt_skip_networking;
static my_bool opt_verbose_bootstrap;
static my_bool verbose_errors;
static my_bool opt_large_pages;
static char *opt_config;

#define DEFAULT_INNODB_PAGE_SIZE 16*1024

static struct my_option my_long_options[]=
{
  {"help", '?', "Display this help message and exit.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"datadir", 'd', "Data directory of the new database",
  &opt_datadir, &opt_datadir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"service", 'S', "Name of the Windows service",
  &opt_service, &opt_service, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"password", 'p', "Root password",
  &opt_password, &opt_password, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"port", 'P', "mysql port",
  &opt_port, &opt_port, 0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"socket", 'W',
  "named pipe name (if missing, it will be set the same as service)",
  &opt_socket, &opt_socket, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"default-user", 'D', "Create default user",
  &opt_default_user, &opt_default_user, 0 , GET_BOOL, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"allow-remote-root-access", 'R',
  "Allows remote access from network for user root",
  &opt_allow_remote_root_access, &opt_allow_remote_root_access, 0 , GET_BOOL,
  OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"skip-networking", 'N', "Do not use TCP connections, use pipe instead",
  &opt_skip_networking, &opt_skip_networking, 0 , GET_BOOL, OPT_ARG, 0, 0, 0, 0,
  0, 0},
  { "innodb-page-size", 'i', "Page size for innodb",
  &opt_innodb_page_size, &opt_innodb_page_size, 0, GET_INT, REQUIRED_ARG, DEFAULT_INNODB_PAGE_SIZE, 1*1024, 64*1024, 0, 0, 0 },
  {"silent", 's', "Print less information", &opt_silent,
   &opt_silent, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"verbose-bootstrap", 'o', "Include mysqld bootstrap output",&opt_verbose_bootstrap,
   &opt_verbose_bootstrap, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  { "large-pages",'l', "Use large pages", &opt_large_pages,
   &opt_large_pages, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"config",'c', "my.ini config template file", &opt_config,
   &opt_config,  0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static my_bool
get_one_option(const struct my_option *opt, const char *, const char *)
{
  DBUG_ENTER("get_one_option");
  switch (opt->id) {
  case '?':
    printf("%s\n", USAGETEXT);
    my_print_help(my_long_options);
    exit(0);
    break;
  }
  DBUG_RETURN(0);
}


ATTRIBUTE_NORETURN  static void die(const char *fmt, ...)
{
  va_list args;
  DBUG_ENTER("die");

  /* Print the error message */
  va_start(args, fmt);
  fprintf(stderr, "FATAL ERROR: ");
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  va_end(args);
  my_end(0);
  exit(1);
}


static void verbose( const char *fmt, ...)
{
  va_list args;

  if (opt_silent)
    return;

  /* Print the verbose message */
  va_start(args, fmt);
  vfprintf(stdout, fmt, args);
  fputc('\n', stdout);
  fflush(stdout);
  va_end(args);
}

static char full_config_path[MAX_PATH];

int main(int argc, char **argv)
{
  int error;
  char self_name[MAX_PATH];
  char *p;
  char *datadir = NULL;
  MY_INIT(argv[0]);
  GetModuleFileName(NULL, self_name, MAX_PATH);
  strcpy(mysqld_path,self_name);
  p= strrchr(mysqld_path, FN_LIBCHAR);
  if (p)
  {
    strcpy(p, "\\mysqld.exe");
  }

  if ((error= handle_options(&argc, &argv, my_long_options, get_one_option)))
    exit(error);

  if (opt_config != 0 && _access(opt_config, 04) != 0)
  {
    int err= errno;
    switch(err)
    {
      case EACCES:
        die("File %s can't be read", opt_config);
        break;
      case ENOENT:
        die("File %s does not exist", opt_config);
        break;
      default:
        die("Can't access file %s, errno %d",opt_config, err);
        break;
    }
  }
  if (opt_config)
  {
    DWORD dwret = GetFullPathName(opt_config, sizeof(full_config_path), full_config_path, NULL);
    if (dwret == 0)
    {
      die("GetFullPathName failed, last error %u", GetLastError());
    }
    else if (dwret > sizeof(full_config_path))
    {
      die("Can't resolve the config file name, path too large");
    }
    opt_config= full_config_path;
  }

  if(opt_datadir)
    datadir = opt_datadir;

  if (!datadir && opt_config)
  {
    for(auto section : {"server","mysqld"})
    {
      auto ret  = GetPrivateProfileStringA(section,"datadir", NULL, default_datadir,
        sizeof(default_datadir)-1, opt_config);
      if (ret)
      {
        datadir= default_datadir;
        printf("Data directory (from config file) is %s\n",datadir);
        break;
      }
    }
  }

  if (!datadir)
  {
    /*
      Figure out default data directory. It "data" directory, next to "bin" directory, where
      mysql_install_db.exe resides.
    */
    strcpy(default_datadir, self_name);
    p = strrchr(default_datadir, FN_LIBCHAR);
    if (p)
    {
      *p= 0;
      p= strrchr(default_datadir, FN_LIBCHAR);
      if (p)
        *p= 0;
    }
    if (!p)
    {
      die("--datadir option not provided, and default datadir not found");
      my_print_help(my_long_options);
    }
    strcat_s(default_datadir, "\\data");
    datadir= default_datadir;
    printf("Default data directory is %s\n",datadir);
  }

  DBUG_ASSERT(datadir);


  /* Workaround WiX bug (strip possible quote character at the end of path) */
  size_t len= strlen(datadir);
  if (len > 0)
  {
    if (datadir[len-1] == '"')
    {
      datadir[len-1]= 0;
    }
    if (datadir[0] == '"')
    {
      datadir++;
    }
  }
  GetFullPathName(datadir, FN_REFLEN, datadir_buffer, NULL);
  datadir= datadir_buffer;

  if (create_db_instance(datadir))
  {
    die("database creation failed");
  }

  printf("Creation of the database was successful\n");
  return 0;
}



/**
  Convert slashes in paths into MySQL-compatible form
*/

static void convert_slashes(char *s, char replacement)
{
  for (; *s; s++)
    if (*s == '\\' || *s == '/')
      *s= replacement;
}


/**
  Calculate basedir from mysqld.exe path.
  Basedir assumed to be is one level up from the mysqld.exe directory location.
  E.g basedir for C:\my\bin\mysqld.exe would be C:\my
*/

static void get_basedir(char *basedir, int size, const char *mysqld_path,
                        char slash)
{
  strcpy_s(basedir, size,  mysqld_path);
  convert_slashes(basedir, '\\');
  char *p= strrchr(basedir, '\\');
  if (p)
  {
    *p = 0;
    p= strrchr(basedir, '\\');
    if (p)
      *p= 0;
  }
}

#define STR(s) _STR(s)
#define _STR(s) #s

static char *get_plugindir()
{
  static char plugin_dir[2*MAX_PATH];
  get_basedir(plugin_dir, sizeof(plugin_dir), mysqld_path, '/');
  strcat(plugin_dir, "/" STR(INSTALL_PLUGINDIR));

  if (access(plugin_dir, 0) == 0)
    return plugin_dir;

  return NULL;
}

/**
  Allocate and initialize command line for mysqld --bootstrap.
 The resulting string is passed to popen, so it has a lot of quoting
 quoting around the full string plus quoting around parameters with spaces.
*/

static char *init_bootstrap_command_line(char *cmdline, size_t size)
{
  snprintf(cmdline, size - 1,
    "\"\"%s\""
    " --defaults-file=my.ini"
    " %s"
    " --bootstrap"
    " --datadir=."
    " --loose-innodb-buffer-pool-size=20M"
    "\""
    , mysqld_path, opt_verbose_bootstrap ? "--console" : "");
  return cmdline;
}

static char my_ini_path[MAX_PATH];

static void write_myini_str(const char *key, const char* val, const char *section="mysqld")
{
  DBUG_ASSERT(my_ini_path[0]);
  if (!WritePrivateProfileString(section, key, val, my_ini_path))
  {
    die("Can't write to ini file key=%s, val=%s, section=%s, Windows error %u",key,val,section,
      GetLastError());
  }
}


static void write_myini_int(const char* key, int val, const char* section = "mysqld")
{
  char buf[10];
  itoa(val, buf, 10);
  write_myini_str(key, buf, section);
}

/**
  Create my.ini in  current directory (this is assumed to be
  data directory as well).
*/

static int create_myini()
{
  my_bool enable_named_pipe= FALSE;
  printf("Creating my.ini file\n");

  char path_buf[MAX_PATH];
  GetCurrentDirectory(MAX_PATH, path_buf);
  snprintf(my_ini_path,sizeof(my_ini_path), "%s\\my.ini", path_buf);
  if (opt_config)
  {
    if (!CopyFile(opt_config,  my_ini_path,TRUE))
    {
      die("Can't copy %s to my.ini , last error %lu", opt_config, GetLastError());
    }
  }

  /* Write out server settings. */
  convert_slashes(path_buf,'/');
  write_myini_str("datadir",path_buf);

  if (opt_skip_networking)
  {
    write_myini_str("skip-networking","ON");
    if (!opt_socket)
      opt_socket= opt_service;
  }
  enable_named_pipe= (my_bool)
    ((opt_socket && opt_socket[0]) || opt_skip_networking);

  if (enable_named_pipe)
  {
    write_myini_str("named-pipe","ON");
  }

  if (opt_socket && opt_socket[0])
  {
    write_myini_str("socket", opt_socket);
  }
  if (opt_port)
  {
    write_myini_int("port", opt_port);
  }
  if (opt_innodb_page_size != DEFAULT_INNODB_PAGE_SIZE)
  {
    write_myini_int("innodb-page-size", opt_innodb_page_size);
  }
  if (opt_large_pages)
  {
    write_myini_str("large-pages","ON");
  }

  /* Write out client settings. */

  /* Used for named pipes */
  if (opt_socket && opt_socket[0])
    write_myini_str("socket",opt_socket,"client");
  if (opt_skip_networking)
    write_myini_str("protocol", "pipe", "client");
  else if (opt_port)
    write_myini_int("port",opt_port,"client");

  char *plugin_dir = get_plugindir();
  if (plugin_dir)
    write_myini_str("plugin-dir", plugin_dir, "client");
  return 0;
}


static constexpr const char* update_root_passwd=
  "UPDATE mysql.global_priv SET priv=json_set(priv,"
  "'$.password_last_changed', UNIX_TIMESTAMP(),"
  "'$.plugin','mysql_native_password',"
  "'$.authentication_string','%s') where User='root';\n";
static constexpr char remove_default_user_cmd[]=
  "DELETE FROM mysql.user where User='';\n";
static constexpr char allow_remote_root_access_cmd[]=
  "CREATE TEMPORARY TABLE tmp_user LIKE global_priv;\n"
  "INSERT INTO tmp_user SELECT * from global_priv where user='root' "
    " AND host='localhost';\n"
  "UPDATE tmp_user SET host='%';\n"
  "INSERT INTO global_priv SELECT * FROM tmp_user;\n"
  "DROP TABLE tmp_user;\n";
static const char end_of_script[]="-- end.";

/*
Add or remove privilege for a user
@param[in] account_name - user name, Windows style, e.g "NT SERVICE\mariadb", or ".\joe"
@param[in] privilege name - standard Windows privilege name, e.g "SeLockMemoryPrivilege"
@param[in] add - when true, add privilege, otherwise remove it

In special case where privilege name is NULL, and add is false
all privileges for the user are removed.
*/
static int handle_user_privileges(const char *account_name, const wchar_t *privilege_name, bool add)
{
  LSA_OBJECT_ATTRIBUTES attr{};
  LSA_HANDLE lsa_handle;
  auto status= LsaOpenPolicy(
      0, &attr, POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT, &lsa_handle);
  if (status)
  {
    verbose("LsaOpenPolicy returned %lu", LsaNtStatusToWinError(status));
    return 1;
  }
  BYTE sidbuf[SECURITY_MAX_SID_SIZE];
  PSID sid= (PSID) sidbuf;
  SID_NAME_USE name_use;
  char domain_name[256];
  DWORD cbSid= sizeof(sidbuf);
  DWORD cbDomain= sizeof(domain_name);
  BOOL ok= LookupAccountNameA(0, account_name, sid, &cbSid, domain_name,
                              &cbDomain, &name_use);
  if (!ok)
  {
    verbose("LsaOpenPolicy returned %lu", LsaNtStatusToWinError(status));
    return 1;
  }

  if (privilege_name)
  {
    LSA_UNICODE_STRING priv{};
    priv.Buffer= (PWSTR) privilege_name;
    priv.Length= (USHORT) wcslen(privilege_name) * sizeof(wchar_t);
    priv.MaximumLength= priv.Length;
    if (add)
    {
      status= LsaAddAccountRights(lsa_handle, sid, &priv, 1);
      if (status)
      {
        verbose("LsaAddAccountRights returned %lu/%lu", status,
                LsaNtStatusToWinError(status));
        return 1;
      }
    }
    else
    {
      status= LsaRemoveAccountRights(lsa_handle, sid, FALSE, &priv, 1);
      if (status)
      {
        verbose("LsaRemoveRights returned %lu/%lu",
                LsaNtStatusToWinError(status));
        return 1;
      }
    }
  }
  else
  {
    DBUG_ASSERT(!add);
    status= LsaRemoveAccountRights(lsa_handle, sid, TRUE, 0, 0);
  }
  LsaClose(lsa_handle);
  return 0;
}

/* Register service. Assume my.ini is in datadir */

static int register_service(const char *datadir, const char *user, const char *passwd)
{
  char buf[3*MAX_PATH +32]; /* path to mysqld.exe, to my.ini, service name */
  SC_HANDLE sc_manager, sc_service;

  size_t datadir_len= strlen(datadir);
  const char *backslash_after_datadir= "\\";

  if (datadir_len && datadir[datadir_len-1] == '\\')
    backslash_after_datadir= "";

  verbose("Registering service '%s'", opt_service);
  my_snprintf(buf, sizeof(buf)-1,
    "\"%s\" \"--defaults-file=%s%smy.ini\" \"%s\"" ,  mysqld_path, datadir,
    backslash_after_datadir, opt_service);

  /* Get a handle to the SCM database. */ 
  sc_manager= OpenSCManager( NULL, NULL, SC_MANAGER_ALL_ACCESS);
  if (!sc_manager) 
  {
    die("OpenSCManager failed (%u)\n", GetLastError());
  }

  /* Create the service. */
  sc_service= CreateService(sc_manager, opt_service,  opt_service,
    SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, 
    SERVICE_ERROR_NORMAL, buf, NULL, NULL, NULL, user, passwd);

  if (!sc_service) 
  {
    CloseServiceHandle(sc_manager);
    die("CreateService failed (%u)", GetLastError());
  }
  char description[] = "MariaDB database server";
  SERVICE_DESCRIPTION sd= { description };
  ChangeServiceConfig2(sc_service, SERVICE_CONFIG_DESCRIPTION, &sd);
  CloseServiceHandle(sc_service); 
  CloseServiceHandle(sc_manager);
  return 0;
}


static void clean_directory(const char *dir)
{
  char dir2[MAX_PATH + 4]= {};
  snprintf(dir2, MAX_PATH+2, "%s\\*", dir);

  SHFILEOPSTRUCT fileop;
  fileop.hwnd= NULL;    /* no status display */
  fileop.wFunc= FO_DELETE;  /* delete operation */
  fileop.pFrom= dir2;  /* source file name as double null terminated string */
  fileop.pTo= NULL;    /* no destination needed */
  fileop.fFlags= FOF_NOCONFIRMATION|FOF_SILENT;  /* do not prompt the user */


  fileop.fAnyOperationsAborted= FALSE;
  fileop.lpszProgressTitle= NULL;
  fileop.hNameMappings= NULL;

  SHFileOperation(&fileop);
}


/*
  Define directory permission to have inheritable all access for a user
  (defined as username or group string or as SID)
*/

static int set_directory_permissions(const char *dir, const char *os_user,
                                     DWORD permission)
{

   struct{
        TOKEN_USER tokenUser;
        BYTE buffer[SECURITY_MAX_SID_SIZE];
   } tokenInfoBuffer;

  HANDLE hDir= CreateFile(dir,READ_CONTROL|WRITE_DAC,0,NULL,OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS,NULL);
  if (hDir == INVALID_HANDLE_VALUE) 
    return -1;  
  ACL* pOldDACL;
  SECURITY_DESCRIPTOR* pSD= NULL; 
  EXPLICIT_ACCESS ea={0};
  WELL_KNOWN_SID_TYPE wellKnownSidType = WinNullSid;
  PSID pSid= NULL;

  GetSecurityInfo(hDir, SE_FILE_OBJECT , DACL_SECURITY_INFORMATION,NULL, NULL,
    &pOldDACL, NULL, (void**)&pSD); 

  if (os_user)
  {
    /* Check for 3 predefined service users 
       They might have localized names in non-English Windows, thus they need
       to be handled using well-known SIDs.
    */
    if (stricmp(os_user, "NT AUTHORITY\\NetworkService") == 0)
    {
      wellKnownSidType= WinNetworkServiceSid;
    }
    else if (stricmp(os_user, "NT AUTHORITY\\LocalService") == 0)
    {
      wellKnownSidType= WinLocalServiceSid;
    }
    else if (stricmp(os_user, "NT AUTHORITY\\LocalSystem") == 0)
    {
      wellKnownSidType= WinLocalSystemSid;
    }

    if (wellKnownSidType != WinNullSid)
    {
      DWORD size= SECURITY_MAX_SID_SIZE;
      pSid= (PSID)tokenInfoBuffer.buffer;
      if (!CreateWellKnownSid(wellKnownSidType, NULL, pSid,
        &size))
      {
        return 1;
      }
      ea.Trustee.TrusteeForm= TRUSTEE_IS_SID;
      ea.Trustee.ptstrName= (LPTSTR)pSid;
    }
    else
    {
      ea.Trustee.TrusteeForm= TRUSTEE_IS_NAME;
      ea.Trustee.ptstrName= (LPSTR)os_user;
    }
  }
  else
  {
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY, &token))
    {

      DWORD length= (DWORD) sizeof(tokenInfoBuffer);
      if (GetTokenInformation(token, TokenUser, &tokenInfoBuffer, 
        length, &length))
      {
        pSid= tokenInfoBuffer.tokenUser.User.Sid;
      }
    }
    if (!pSid)
      return 0;
    ea.Trustee.TrusteeForm= TRUSTEE_IS_SID;
    ea.Trustee.ptstrName= (LPTSTR)pSid;
  }
  ea.Trustee.TrusteeType= TRUSTEE_IS_UNKNOWN;
  ea.grfAccessMode= GRANT_ACCESS;
  ea.grfAccessPermissions= permission;
  ea.grfInheritance= CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
  ACL *pNewDACL= 0;

  ACCESS_MASK access_mask;
  if (GetEffectiveRightsFromAcl(pOldDACL, &ea.Trustee, &access_mask) != ERROR_SUCCESS
    || (access_mask & permission) != permission)
  {
    SetEntriesInAcl(1, &ea, pOldDACL, &pNewDACL);
  }

  if (pNewDACL)
  {
    SetSecurityInfo(hDir,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,NULL, NULL,
      pNewDACL, NULL);
  }
  if (pSD != NULL) 
    LocalFree((HLOCAL) pSD); 
  if (pNewDACL != NULL) 
    LocalFree((HLOCAL) pNewDACL);
  CloseHandle(hDir); 
  return 0;
}

static void set_permissions(const char *datadir, const char *service_user)
{
  /*
    Set data directory permissions for both current user and
    the one who who runs services.
  */
  set_directory_permissions(datadir, NULL,
                            FILE_GENERIC_READ | FILE_GENERIC_WRITE);
  if (!service_user)
    return;

  /* Datadir permission for the service. */
  set_directory_permissions(datadir, service_user, FILE_ALL_ACCESS);
  char basedir[MAX_PATH];
  char path[MAX_PATH];

  struct
  {
    const char *subdir;
    DWORD perm;
  } all_subdirs[]= {
      {STR(INSTALL_PLUGINDIR), FILE_GENERIC_READ | FILE_GENERIC_EXECUTE},
      {STR(INSTALL_SHAREDIR), FILE_GENERIC_READ},
  };


  if (strncmp(service_user,"NT SERVICE\\",sizeof("NT SERVICE\\")-1) == 0)
  {
  	/*
     Read and execute permission for executables can/should be given
     to any service account, rather than specific one.
    */
    service_user="NT SERVICE\\ALL SERVICES";
  }
 
  get_basedir(basedir, sizeof(basedir), mysqld_path, '\\');
  for (int i= 0; i < array_elements(all_subdirs); i++)
  {
    auto subdir=
        snprintf(path, sizeof(path), "%s\\%s", basedir, all_subdirs[i].subdir);
    if (access(path, 0) == 0)
    {
      set_directory_permissions(path, service_user, all_subdirs[i].perm);
    }
  }

  /* Bindir, the directory where mysqld_path is located. */
  strcpy_s(path, mysqld_path);
  char *end= strrchr(path, '/');
  if (!end)
    end= strrchr(path, '\\');
  if (end)
    *end= 0;
  if (access(path, 0) == 0)
  {
    set_directory_permissions(path, service_user,
                              FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
  }
}

/* Create database instance (including registering as service etc) .*/

static int create_db_instance(const char *datadir)
{
  int ret= 0;
  char cwd[MAX_PATH];
  DWORD cwd_len= MAX_PATH;
  char cmdline[3*MAX_PATH];
  FILE *in;
  bool created_datadir= false;
  DWORD last_error;
  bool service_created= false;
  std::string mysql_db_dir;

  verbose("Running bootstrap");

  GetCurrentDirectory(cwd_len, cwd);

  /* Create datadir and datadir/mysql, if they do not already exist. */

  if (CreateDirectory(datadir, NULL))
  {
    created_datadir= true;
  }
  else if (GetLastError() != ERROR_ALREADY_EXISTS)
  {
    last_error = GetLastError();
    switch(last_error)
    {
      case ERROR_ACCESS_DENIED:
        die("Can't create data directory '%s' (access denied)\n",
            datadir);
        break;
      case ERROR_PATH_NOT_FOUND:
        die("Can't create data directory '%s' "
            "(one or more intermediate directories do not exist)\n",
            datadir);
        break;
      default:
        die("Can't create data directory '%s', last error %u\n",
         datadir, last_error);
        break;
    }
  }

  if (!SetCurrentDirectory(datadir))
  {
    last_error = GetLastError();
    switch (last_error)
    {
      case ERROR_DIRECTORY:
        die("Can't set current directory to '%s', the path is not a valid directory \n",
            datadir);
        break;
      default:
        die("Can' set current directory to '%s', last error %u\n",
            datadir, last_error);
        break;
    }
  }

  if (!PathIsDirectoryEmpty(datadir))
  {
    fprintf(stderr, "ERROR : Data directory %s is not empty."
        " Only new or empty existing directories are accepted for --datadir\n", datadir);
    exit(1);
  }

  std::string service_user;
  /* Register service if requested. */
  if (opt_service && opt_service[0])
  {
    /* Run service under virtual account NT SERVICE\service_name.*/
    service_user.append("NT SERVICE\\").append(opt_service);
    ret = register_service(datadir, service_user.c_str(), NULL);
    if (ret)
      goto end;
    service_created = true;
  }

  set_permissions(datadir, service_user.c_str());

  if (opt_large_pages)
  {
    handle_user_privileges(service_user.c_str(), L"SeLockMemoryPrivilege", true);
  }

  /*
  Get security descriptor for the data directory.
  It will be passed, as SDDL text, to the mysqld bootstrap subprocess,
  to allow for correct subdirectory permissions.
  */
  PSECURITY_DESCRIPTOR pSD;
  if (GetNamedSecurityInfoA(datadir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
    0, 0, 0, 0, &pSD) == ERROR_SUCCESS)
  {
    char* string_sd = NULL;
    if (ConvertSecurityDescriptorToStringSecurityDescriptor(pSD, SDDL_REVISION_1,
      DACL_SECURITY_INFORMATION, &string_sd, 0))
    {
      _putenv_s("MARIADB_NEW_DIRECTORY_SDDL", string_sd);
      LocalFree(string_sd);
    }
    LocalFree(pSD);
  }

  /* Create my.ini file in data directory.*/
  ret = create_myini();
  if (ret)
    goto end;

  /* Do mysqld --bootstrap. */
  init_bootstrap_command_line(cmdline, sizeof(cmdline));

  if(opt_verbose_bootstrap)
    printf("Executing %s\n", cmdline);

  in= popen(cmdline, "wt");
  if (!in)
    goto end;

  if (setvbuf(in, NULL, _IONBF, 0))
  {
    verbose("WARNING: Can't disable buffering on mysqld's stdin");
  }
  static const char *pre_bootstrap_sql[] = { "create database mysql;\n","use mysql;\n"};
  for (auto cmd  : pre_bootstrap_sql)
  {
    /* Write the bootstrap script to stdin. */
    if (fwrite(cmd, strlen(cmd), 1, in) != 1)
    {
      verbose("ERROR: Can't write to mysqld's stdin");
      ret= 1;
      goto end;
    }
  }

  for (int i= 0; mysql_bootstrap_sql[i]; i++)
  {
    auto cmd = mysql_bootstrap_sql[i];
    /* Write the bootstrap script to stdin. */
    if (fwrite(cmd, strlen(cmd), 1, in) != 1)
    {
      verbose("ERROR: Can't write to mysqld's stdin");
      ret= 1;
      goto end;
    }
  }

  /* Remove default user, if requested. */
  if (!opt_default_user)
  {
    verbose("Removing default user",remove_default_user_cmd);
    fputs(remove_default_user_cmd, in);
    fflush(in);
  }

  if (opt_allow_remote_root_access)
  {
     verbose("Allowing remote access for user root",remove_default_user_cmd);
     fputs(allow_remote_root_access_cmd,in);
     fflush(in);
  }

  /* Change root password if requested. */
  if (opt_password && opt_password[0])
  {
    verbose("Setting root password");
    char buf[2 * MY_SHA1_HASH_SIZE + 2];
    my_make_scrambled_password(buf, opt_password, strlen(opt_password));
    fprintf(in, update_root_passwd, buf);
    fflush(in);
  }

  /*
    On some reason, bootstrap chokes if last command sent via stdin ends with
    newline, so we supply a dummy comment, that does not end with newline.
  */
  fputs(end_of_script, in);
  fflush(in);

  /* Check if bootstrap has completed successfully. */
  ret= pclose(in);
  if (ret)
  {
    verbose("mysqld returned error %d in pclose",ret);
    goto end;
  }

end:
  if (!ret)
    return ret;

  /* Cleanup after error.*/
  if (created_datadir)
  {
    SetCurrentDirectory(cwd);
    clean_directory(datadir);
  }

  if (service_created)
  {
    auto sc_manager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (sc_manager)
    {
      auto sc_handle= OpenService(sc_manager,opt_service, DELETE);
      if (sc_handle)
      {
        DeleteService(sc_handle);
        CloseServiceHandle(sc_handle);
      }
      CloseServiceHandle(sc_manager);
    }

    /*Remove all service user privileges for the user.*/
    if(strncmp(service_user.c_str(), "NT SERVICE\\",
         sizeof("NT SERVICE\\")-1))
    {
      handle_user_privileges(service_user.c_str(), 0, false);
    }
    if (created_datadir)
      RemoveDirectory(opt_datadir);
  }
  return ret;
}
