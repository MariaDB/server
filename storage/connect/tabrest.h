/*************** TabRest H Declares Source Code File (.H) **************/
/*  Name: tabrest.h   Version 1.0                                      */
/*  (C) Copyright to the author Olivier BERTRAND          2019         */
/*  This file contains the common tabrest classes declares.            */
/***********************************************************************/
#pragma once

#if defined(_WIN32)
#else // !_WIN32
#define stricmp strcasecmp
#endif // !_WIN32

/***********************************************************************/
/*  Functions used by REST.                                            */
/***********************************************************************/
#if defined(MARIADB)
PQRYRES RESTColumns(PGLOBAL g, PTOS tp, char* tab, char* db, bool info);
#endif  // !MARIADB


/***********************************************************************/
/*  Data structure for curl callback function                          */
/***********************************************************************/
struct MemoryStruct {
    char *memory;
    size_t size;
};


/***********************************************************************/
/*  Restest table.                                                     */
/***********************************************************************/
class RESTDEF : public TABDEF { /* Table description */
private:
  bool curl_inited;
public:
// Constructor
  RESTDEF()
    :curl_inited(false),
     Tdp(NULL),
     Http(NULL),
     Uri(NULL)
  {}
  int curl_init (PGLOBAL g);
  void curl_deinit ();
  // Methods
  const char *GetType(void) override { return "REST"; }
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  virtual PTDB GetTable(PGLOBAL g, MODE m) override;
  int curl_run(PGLOBAL g);
  // Members
  PRELDEF Tdp;
  PCSZ    Http;                   /* Web connection HTTP               */
  PCSZ    Uri;                    /* Web connection URI                */
  char     filename[_MAX_PATH + 1];/* The intermediate file name        */
  ~RESTDEF()
  {
    curl_deinit();
  }
}; // end of class RESTDEF
