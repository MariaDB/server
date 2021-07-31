/**************** RCMsg C Program Source Code File (.C) ****************/
/* PROGRAM NAME: RCMSG                                                 */
/* -------------                                                       */
/*  Version 1.3                                                        */
/*                                                                     */
/* COPYRIGHT                                                           */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND:  2005 - 2014         */
/*                                                                     */
/* WHAT THIS PROGRAM DOES                                              */
/* -----------------------                                             */
/*  This program simulates LoadString.                                 */
/*                                                                     */
/***********************************************************************/
#if !defined(XMSG)
#include <stdio.h>
#include <string.h>
#include "resource.h"
#include "rcmsg.h"
#if defined(NEWMSG)
#include "msgid.h"
#endif   // NEWMSG

#if !defined(_WIN32)
#define stricmp  strcasecmp
#endif   // !_WIN32

char *msglang(void);

const char *GetMsgid(int id)
  {
  const char *p = NULL;

  // This conditional until a real fix is found for MDEV-7304
#if defined(FRENCH)
  if (!stricmp(msglang(), "french"))
    switch (id) {
#include "frids.h"
#if defined(NEWMSG)
#include "frcas.h"
#endif   // NEWMSG
    } // endswitch(id)

  else    // English
#endif   // FRENCH
    switch (id) {
#include "enids.h"
#if defined(NEWMSG)
#include "encas.h"
#endif   // NEWMSG
    } // endswitch(id)

  return p;
  } // end of GetMsgid

int GetRcString(int id, char *buf, int bufsize)
  {
  const char *p = NULL;
  char msg[32];

  if (!(p = GetMsgid(id))) {
    sprintf(msg, "ID=%d unknown", id);
    p = msg;
    } // endif p

  return sprintf(buf, "%.*s", bufsize-1, p);
  } // end of GetRcString

#endif // !XMSG
