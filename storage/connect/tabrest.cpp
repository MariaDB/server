/************** tabrest C++ Program Source Code File (.CPP) ************/
/* PROGRAM NAME: tabrest   Version 2.1                                 */
/*  (C) Copyright to the author Olivier BERTRAND          2018 - 2021  */
/*  This program is the REST Web API support for MariaDB.              */
/*  The way Connect handles NOSQL data returned by REST queries is     */
/*  just by retrieving it as a file and then leave the existing data   */
/*  type tables (JSON, XML or CSV) process it as usual.                */
/***********************************************************************/

/***********************************************************************/
/*  Definitions needed by the included files.                          */
/***********************************************************************/
#include <my_global.h>    // All MariaDB stuff
#include <mysqld.h>
#include <mysqld_error.h>
#include <sql_error.h>
#if !defined(_WIN32) && !defined(_WINDOWS)
#include <sys/types.h>
#include <sys/wait.h>
#endif	 // !_WIN32 && !_WINDOWS

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "plgxml.h"
#if defined(XML_SUPPORT)
#include "tabxml.h"
#endif   // XML_SUPPORT
#include "tabjson.h"
#include "tabfmt.h"
#include "tabrest.h"
#include <curl/curl.h>

#if defined(connect_EXPORTS)
#define PUSH_WARNING(M) push_warning(current_thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR, M)
#else
#define PUSH_WARNING(M) htrc(M)
#endif


/***********************************************************************/
/*  Return the columns definition to MariaDB.                          */
/***********************************************************************/
PQRYRES RESTColumns(PGLOBAL g, PTOS tp, char *tab, char *db, bool info)
{
  PQRYRES  qrp= NULL;
  RESTDEF  restObject;
  char     filename[_MAX_PATH + 1];  // MAX PATH ???
  int      rc;
  PCSZ     http, uri, fn, ftype;


  http = GetStringTableOption(g, tp, "Http", NULL);
  uri = GetStringTableOption(g, tp, "Uri", NULL);
  ftype = GetStringTableOption(g, tp, "Type", "JSON");
  fn = GetStringTableOption(g, tp, "Filename", NULL);

  if (!fn)
  {
    int n, m = strlen(ftype) + 1;
    strcat(strcpy(filename, tab), ".");
    n = strlen(filename);
    // Fold ftype to lower case
    for (int i = 0; i < m; i++)
      filename[n + i] = tolower(ftype[i]);
    fn = filename;
    tp->subtype = PlugDup(g, fn);
    snprintf(g->Message, sizeof(g->Message), "No file name. Table will use %s", fn);
    PUSH_WARNING(g->Message);
  }

  //  We used the file name relative to recorded datapath
  PlugSetPath(filename, fn, db);
  restObject.Http= http;
  restObject.Uri= uri;
  restObject.Fn= filename;
  remove(filename);
  // Retrieve the file from the web using curl and copy it locally
  if (restObject.curl_init(g))
  {
    snprintf(g->Message, sizeof(g->Message), "Initialization of curl failed.");
    return NULL;
  }
  rc = restObject.curl_run(g);
  if (rc)
  {
    snprintf(g->Message, sizeof(g->Message), "Cannot access to curl.");
    return NULL;
  }
  else if (!stricmp(ftype, "JSON"))
    qrp = JSONColumns(g, db, NULL, tp, info);
  else if (!stricmp(ftype, "CSV"))
    qrp = CSVColumns(g, NULL, tp, info);
#if defined(XML_SUPPORT)
  else if (!stricmp(ftype, "XML"))
    qrp = XMLColumns(g, db, tab, tp, info);
#endif   // XML_SUPPORT
  else
    snprintf(g->Message, sizeof(g->Message), "Usupported file type %s", ftype);
  return qrp;
} // end of RESTColumns


/* -------------------------- Class RESTDEF -------------------------- */

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool RESTDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
  char     filename[_MAX_PATH + 1];
  int      rc = 0, n;
  bool     xt = trace(515);
  LPCSTR   ftype;

  ftype = GetStringCatInfo(g, "Type", "JSON");
  if (xt)
    htrc("ftype = %s am = %s\n", ftype, SVP(am));

  n = (!stricmp(ftype, "JSON")) ? 1
#if defined(XML_SUPPORT)
    : (!stricmp(ftype, "XML"))  ? 2
#endif   // XML_SUPPORT
    : (!stricmp(ftype, "CSV"))  ? 3 : 0;

  if (n == 0)
  {
    htrc("DefineAM: Unsupported REST table type %s\n", ftype);
    snprintf(g->Message, sizeof(g->Message),
             "Unsupported REST table type %s", ftype);
    return true;
  } // endif n

  Http = GetStringCatInfo(g, "Http", NULL);
  Uri = GetStringCatInfo(g, "Uri", NULL);
  Fn = GetStringCatInfo(g, "Filename", NULL);

  //  We used the file name relative to recorded datapath
  PlugSetPath(filename, Fn, GetPath());
  Fn= filename;
  remove(filename);
  if (curl_init(g))
  {
    snprintf(g->Message, sizeof(g->Message), "Initialization of curl failed.");
    return true;
  }
  if (curl_run(g))
    return true;
  else switch (n)
  {
    case 1:
      Tdp = new (g) JSONDEF; break;
#if defined(XML_SUPPORT)
    case 2:
      Tdp = new (g) XMLDEF;  break;
#endif   // XML_SUPPORT
    case 3:
      Tdp = new (g) CSVDEF;  break;
      default: Tdp = NULL;
  } // endswitch n

  // Do make the table/view definition
  if (Tdp && Tdp->Define(g, Cat, Name, Schema, "REST"))
    Tdp = NULL; // Error occurred

  if (xt)
    htrc("Tdp defined\n", rc);

  // Return true in case of error
  return (Tdp == NULL);
} // end of DefineAM


/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB RESTDEF::GetTable(PGLOBAL g, MODE m)
{
  if (trace(515))
    htrc("REST GetTable mode=%d\n", m);

  if (m != MODE_READ && m != MODE_READX && m != MODE_ANY)
  {
    snprintf(g->Message, sizeof(g->Message), "REST tables are currently read only");
    return NULL;
  }

  return Tdp->GetTable(g, m); // Leave file type do the job
} // end of GetTable


/***********************************************************************/
/*  curl_init: Initilize curl                                          */
/***********************************************************************/
int RESTDEF::curl_init(PGLOBAL g)
{
  CURLcode curl_res = curl_global_init(CURL_GLOBAL_ALL);
  if (curl_res != CURLE_OK)
  {
    snprintf(g->Message, sizeof(g->Message),
             "unable to initialize curl library, "
             "curl returned this error code: %u "
             "with the following error message: %s",
             curl_res, curl_easy_strerror(curl_res));
    return 1;
  }
  curl_inited = true;
  return 0;
}


/***********************************************************************/
/*  curl_deinit: Cleanup curl                                          */
/***********************************************************************/
void RESTDEF::curl_deinit()
{
  if (curl_inited)
  {
    curl_global_cleanup();
    curl_inited = false;
  }
}


/***********************************************************************/
/*  WriteMemoryCallback: Curl callback function                        */
/***********************************************************************/
static size_t WriteMemoryCallback(void *contents,
                                  size_t size __attribute__((unused)),
                                  size_t nmemb, void *userp)
{
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
  char *ptr = (char *)realloc(mem->memory, mem->size + nmemb + 1);
  if (ptr == NULL)
      return 0;
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, nmemb);
  mem->size += nmemb;
  mem->memory[mem->size] = 0;
  return nmemb;
}


/***********************************************************************/
/*  curl_run: Retrieve the REST answer by executing cURL.              */
/***********************************************************************/
int RESTDEF::curl_run(PGLOBAL g)
{
  CURL *curl = curl_easy_init();
  CURLcode curl_res = CURLE_OK;
  char buf[512];
  long http_code = 0;
  char curl_errbuf[CURL_ERROR_SIZE];
  if (!curl)
  {
    snprintf(g->Message, sizeof(g->Message), "Cannot initilize curl session.");
    return 1;
  }
  curl_errbuf[0] = '\0';
  if (Uri)
  {
    if (*Uri == '/' || Http[strlen(Http) - 1] == '/')
      my_snprintf(buf, sizeof(buf)-1, "%s%s", Http, Uri);
    else
      my_snprintf(buf, sizeof(buf)-1, "%s/%s", Http, Uri);
  }
  else
    my_snprintf(buf, sizeof(buf)-1, "%s", Http);

  struct MemoryStruct chunk;
  chunk.memory = (char *)malloc(1);
  chunk.size = 0;

  if ((curl_res= curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf)) !=
        CURLE_OK ||
      (curl_res= curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                                  WriteMemoryCallback)) !=
        CURLE_OK ||
      (curl_res= curl_easy_setopt(curl, CURLOPT_WRITEDATA,
                                 (void *)&chunk)) !=
        CURLE_OK ||
      (curl_res = curl_easy_setopt(curl, CURLOPT_URL, buf)) != CURLE_OK ||
      (curl_res = curl_easy_perform(curl)) != CURLE_OK ||
      (curl_res = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE,
                                     &http_code)) != CURLE_OK)
  {
    curl_easy_cleanup(curl);
    free(chunk.memory);
    if (curl_res)
    {
      snprintf(g->Message, sizeof(g->Message),
               "curl returned this error code: %u "
               "with the following error message: %s", curl_res,
               curl_errbuf[0] ? curl_errbuf : curl_easy_strerror(curl_res));
      return 1;
    }
  }
  curl_easy_cleanup(curl);
  FILE *f= fopen(Fn, "wb");
  fprintf(f, "%s", chunk.memory);
  fclose(f);
  free(chunk.memory);
  bool is_error = http_code < 200 || http_code >= 300;
  if (is_error)
  {
    snprintf(g->Message, sizeof(g->Message), "server error");
    return 1;
  }
  return 0;
}
/* ---------------------- End of Class RESTDEF ----------------------- */
