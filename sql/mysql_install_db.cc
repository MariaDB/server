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
#include <my_global.h>
#include <my_getopt.h>
#include <my_sys.h>
#include <m_string.h>

#include <windows.h>
#include <shellapi.h>
#include <accctrl.h>
#include <aclapi.h>
struct IUnknown;
#include <shlwapi.h>

#define USAGETEXT \
"mysql_install_db.exe  Ver 1.00 for Windows\n" \
"Copyright (C) 2010-2011 Monty Program Ab & Vladislav Vaintroub\n" \
"This software comes with ABSOLUTELY NO WARRANTY. This is free software,\n" \
"and you are welcome to modify and redistribute it under the GPL v2 license\n" \
"Usage: mysql_install_db.exe [OPTIONS]\n" \
"OPTIONS:"

extern "C" const char* mysql_bootstrap_sql[];

char default_os_user[]= "NT AUTHORITY\\NetworkService";
static int create_db_instance();
static uint opt_silent;
static char datadir_buffer[FN_REFLEN];
static char mysqld_path[FN_REFLEN];
static char *opt_datadir;
static char *opt_service;
static char *opt_password;
static int  opt_port;
static int  opt_innodb_page_size;
static char *opt_socket;
static char *opt_os_user;
static char *opt_os_password;
static my_bool opt_default_user;
static my_bool opt_allow_remote_root_access;
static my_bool opt_skip_networking;
static my_bool opt_verbose_bootstrap;
static my_bool verbose_errors;

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
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static my_bool
get_one_option(int optid, 
   const struct my_option *opt __attribute__ ((unused)),
   char *argument __attribute__ ((unused)))
{
  DBUG_ENTER("get_one_option");
  switch (optid) {
  case '?':
    printf("%s\n", USAGETEXT);
    my_print_help(my_long_options);
    exit(0);
    break;
  }
  DBUG_RETURN(0);
}


static void die(const char *fmt, ...)
{
  va_list args;
  DBUG_ENTER("die");

  /* Print the error message */
  va_start(args, fmt);
  fprintf(stderr, "FATAL ERROR: ");
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  if (verbose_errors)
  {
   fprintf(stderr,
   "https://mariadb.com/kb/en/installation-issues-on-windows contains some help\n"
   "for solving the most common problems.  If this doesn't help you, please\n"
   "leave a comment in the Knowledge Base or file a bug report at\n"
   "https://jira.mariadb.org");
  }
  fflush(stderr);
  va_end(args);
  my_end(0);
  exit(1);
}


static void verbose(const char *fmt, ...)
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


int main(int argc, char **argv)
{
  int error;
  char self_name[FN_REFLEN];
  char *p;

  MY_INIT(argv[0]);
  GetModuleFileName(NULL, self_name, FN_REFLEN);
  strcpy(mysqld_path,self_name);
  p= strrchr(mysqld_path, FN_LIBCHAR);
  if (p)
  {
    strcpy(p, "\\mysqld.exe");
  }

  if ((error= handle_options(&argc, &argv, my_long_options, get_one_option)))
    exit(error);
  if (!opt_datadir)
  {
    my_print_help(my_long_options);
    die("parameter --datadir=# is mandatory");
  }

  /* Print some help on errors */
  verbose_errors= TRUE;

  if (!opt_os_user)
  {
    opt_os_user= default_os_user;
    opt_os_password= NULL;
  }
  /* Workaround WiX bug (strip possible quote character at the end of path) */
  size_t len= strlen(opt_datadir);
  if (len > 0)
  {
    if (opt_datadir[len-1] == '"')
    {
      opt_datadir[len-1]= 0;
    }
  }
  GetFullPathName(opt_datadir, FN_REFLEN, datadir_buffer, NULL);
  opt_datadir= datadir_buffer;

  if (create_db_instance())
  {
    die("database creation failed");
  }

  printf("Creation of the database was successful");
  return 0;
}



/**
  Convert slashes in paths into MySQL-compatible form
*/

static void convert_slashes(char *s)
{
  for (; *s ; s++)
   if (*s == '\\')
     *s= '/';
}


/**
  Calculate basedir from mysqld.exe path.
  Basedir assumed to be is one level up from the mysqld.exe directory location.
  E.g basedir for C:\my\bin\mysqld.exe would be C:\my
*/

static void get_basedir(char *basedir, int size, const char *mysqld_path)
{
  strcpy_s(basedir, size,  mysqld_path);
  convert_slashes(basedir);
  char *p= strrchr(basedir,'/');
  if (p)
  {
    *p = 0;
    p= strrchr(basedir, '/');
    if (p)
      *p= 0;
  }
}

#define STR(s) _STR(s)
#define _STR(s) #s

static char *get_plugindir()
{
  static char plugin_dir[2*MAX_PATH];
  get_basedir(plugin_dir, sizeof(plugin_dir), mysqld_path);
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
  char basedir[MAX_PATH];
  get_basedir(basedir, sizeof(basedir), mysqld_path);

  my_snprintf(cmdline, size - 1,
    "\"\"%s\" --no-defaults %s --innodb-page-size=%d --bootstrap"
    " \"--lc-messages-dir=%s/share\""
    " --basedir=. --datadir=. --default-storage-engine=myisam"
    " --max_allowed_packet=9M "
    " --net-buffer-length=16k\"", mysqld_path,
    opt_verbose_bootstrap ? "--console" : "", opt_innodb_page_size, basedir);
  return cmdline;
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

  /* Create ini file. */
  FILE *myini= fopen("my.ini","wt");
  if (!myini)
  {
    die("Cannot create my.ini in data directory");
  }

  /* Write out server settings. */
  fprintf(myini, "[mysqld]\n");
  convert_slashes(path_buf);
  fprintf(myini, "datadir=%s\n", path_buf);
  if (opt_skip_networking)
  {
    fprintf(myini,"skip-networking\n");
    if (!opt_socket)
      opt_socket= opt_service;
  }
  enable_named_pipe= (my_bool) 
    ((opt_socket && opt_socket[0]) || opt_skip_networking);

  if (enable_named_pipe)
  {
    fprintf(myini,"enable-named-pipe\n");
  }

  if (opt_socket && opt_socket[0])
  {
    fprintf(myini, "socket=%s\n", opt_socket);
  }
  if (opt_port)
  {
    fprintf(myini,"port=%d\n", opt_port);
  }
  if (opt_innodb_page_size != DEFAULT_INNODB_PAGE_SIZE)
  {
    fprintf(myini, "innodb-page-size=%d\n", opt_innodb_page_size);
  }
  /* Write out client settings. */
  fprintf(myini, "[client]\n");

  /* Used for named pipes */
  if (opt_socket && opt_socket[0])
    fprintf(myini,"socket=%s\n",opt_socket);
  if (opt_skip_networking)
    fprintf(myini,"protocol=pipe\n");
  else if (opt_port)
    fprintf(myini,"port=%d\n",opt_port);

  char *plugin_dir = get_plugindir();
  if (plugin_dir)
    fprintf(myini, "plugin-dir=%s\n", plugin_dir);
  fclose(myini);
  return 0;
}


static const char update_root_passwd_part1[]=
  "UPDATE mysql.user SET Password = PASSWORD(";
static const char update_root_passwd_part2[]=
  ") where User='root';\n";
static const char remove_default_user_cmd[]= 
  "DELETE FROM mysql.user where User='';\n";
static const char allow_remote_root_access_cmd[]=
  "CREATE TEMPORARY TABLE tmp_user LIKE user;\n"
  "INSERT INTO tmp_user SELECT * from user where user='root' "
    " AND host='localhost';\n"
  "UPDATE tmp_user SET host='%';\n"
  "INSERT INTO user SELECT * FROM tmp_user;\n"
  "DROP TABLE tmp_user;\n";
static const char end_of_script[]="-- end.";

/* Register service. Assume my.ini is in datadir */

static int register_service()
{
  char buf[3*MAX_PATH +32]; /* path to mysqld.exe, to my.ini, service name */
  SC_HANDLE sc_manager, sc_service;

  size_t datadir_len= strlen(opt_datadir);
  const char *backslash_after_datadir= "\\";

  if (datadir_len && opt_datadir[datadir_len-1] == '\\')
    backslash_after_datadir= "";

  verbose("Registering service '%s'", opt_service);
  my_snprintf(buf, sizeof(buf)-1,
    "\"%s\" \"--defaults-file=%s%smy.ini\" \"%s\"" ,  mysqld_path, opt_datadir, 
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
    SERVICE_ERROR_NORMAL, buf, NULL, NULL, NULL, opt_os_user, opt_os_password);

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

static int set_directory_permissions(const char *dir, const char *os_user)
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
  BOOL isWellKnownSID= FALSE;
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
  ea.grfAccessMode= GRANT_ACCESS;
  ea.grfAccessPermissions= GENERIC_ALL; 
  ea.grfInheritance= CONTAINER_INHERIT_ACE|OBJECT_INHERIT_ACE; 
  ea.Trustee.TrusteeType= TRUSTEE_IS_UNKNOWN; 
  ACL* pNewDACL= 0; 
  DWORD err= SetEntriesInAcl(1,&ea,pOldDACL,&pNewDACL); 
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


/* 
  Give directory permissions for special service user NT SERVICE\servicename
  this user is available only on Win7 and later.
*/

void grant_directory_permissions_to_service()
{
  char service_user[MAX_PATH+ 12];
  OSVERSIONINFO info;
  info.dwOSVersionInfoSize= sizeof(info);
  GetVersionEx(&info);
  if (info.dwMajorVersion >6 || 
    (info.dwMajorVersion== 6 && info.dwMinorVersion > 0)
    && opt_service)
  {
    my_snprintf(service_user,sizeof(service_user), "NT SERVICE\\%s", 
      opt_service);
    set_directory_permissions(opt_datadir, service_user);
  }
}


/* Create database instance (including registering as service etc) .*/

static int create_db_instance()
{
  int ret= 0;
  char cwd[MAX_PATH];
  DWORD cwd_len= MAX_PATH;
  char cmdline[3*MAX_PATH];
  FILE *in;
  bool created_datadir= false;
  DWORD last_error;

  verbose("Running bootstrap");

  GetCurrentDirectory(cwd_len, cwd);

  /* Create datadir and datadir/mysql, if they do not already exist. */

  if (CreateDirectory(opt_datadir, NULL))
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
            opt_datadir);
        break;
      case ERROR_PATH_NOT_FOUND:
        die("Can't create data directory '%s' "
            "(one or more intermediate directories do not exist)\n",
            opt_datadir);
        break;
      default:
        die("Can't create data directory '%s', last error %u\n",
         opt_datadir, last_error);
        break;
    }
  }

  if (!SetCurrentDirectory(opt_datadir))
  {
    last_error = GetLastError();
    switch (last_error)
    {
      case ERROR_DIRECTORY:
        die("Can't set current directory to '%s', the path is not a valid directory \n",
            opt_datadir);
        break;
      default:
        die("Can' set current directory to '%s', last error %u\n",
            opt_datadir, last_error);
        break;
    }
  }

  if (!PathIsDirectoryEmpty(opt_datadir))
  {
    fprintf(stderr,"ERROR : Data directory %s is not empty."
        " Only new or empty existing directories are accepted for --datadir\n",opt_datadir);
    exit(1);
  }

  if (!CreateDirectory("mysql",NULL))
  {
    last_error = GetLastError();
    DWORD attributes;
    switch(last_error)
    {
      case ERROR_ACCESS_DENIED:
        die("Can't create subdirectory 'mysql' in '%s' (access denied)\n",opt_datadir);
        break;
      case ERROR_ALREADY_EXISTS:
       attributes = GetFileAttributes("mysql");

       if (attributes == INVALID_FILE_ATTRIBUTES)
         die("GetFileAttributes() failed for existing file '%s\\mysql', last error %u",
            opt_datadir, GetLastError());
       else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
         die("File '%s\\mysql' exists, but it is not a directory", opt_datadir);

       break;
    }
  }

  /*
    Set data directory permissions for both current user and 
    default_os_user (the one who runs services).
  */
  set_directory_permissions(opt_datadir, NULL);
  set_directory_permissions(opt_datadir, default_os_user);

  /* Do mysqld --bootstrap. */
  init_bootstrap_command_line(cmdline, sizeof(cmdline));

  if(opt_verbose_bootstrap)
    printf("Executing %s\n", cmdline);

  in= popen(cmdline, "wt");
  if (!in)
    goto end;

  if (setvbuf(in, NULL, _IONBF, 0))
  {
    verbose("WARNING: Cannot disable buffering on mysqld's stdin");
  }
  if (fwrite("use mysql;\n",11,1, in) != 1)
  {
    verbose("ERROR: Cannot write to mysqld's stdin");
    ret= 1;
    goto end;
  }

  int i;
  for (i=0; mysql_bootstrap_sql[i]; i++)
  {
    /* Write the bootstrap script to stdin. */
    if (fwrite(mysql_bootstrap_sql[i], strlen(mysql_bootstrap_sql[i]), 1, in) != 1)
    {
      verbose("ERROR: Cannot write to mysqld's stdin");
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
    verbose("Setting root password",remove_default_user_cmd);
    fputs(update_root_passwd_part1, in);

    /* Use hex encoding for password, to avoid escaping problems.*/
    fputc('0', in);
    fputc('x', in);
    for(int i= 0; opt_password[i]; i++)
    {
      fprintf(in,"%02x",opt_password[i]);
    }

    fputs(update_root_passwd_part2, in);
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


  /* Create my.ini file in data directory.*/
  ret= create_myini();
  if (ret)
    goto end;

  /* Register service if requested. */
  if (opt_service && opt_service[0])
  {
    ret= register_service();
    grant_directory_permissions_to_service();
    if (ret)
      goto end;
  }

end:
  if (ret)
  {
    SetCurrentDirectory(cwd);
    clean_directory(opt_datadir);
    if (created_datadir)
      RemoveDirectory(opt_datadir);
  }
  return ret;
}
