/* Copyright (C) Olivier Bertrand 2004 - 2014

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/**
  @file user_connect.cc

  @brief
  Implements the user_connect class.

  @details
  To support multi_threading, each query creates and use a PlugDB "user"
  that is a connection with its personnal memory allocation.

  @note

*/

/****************************************************************************/
/*  Author: Olivier Bertrand  --  bertrandop@gmail.com  --  2004-2014       */
/****************************************************************************/
#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#define DONT_DEFINE_VOID
#define MYSQL_SERVER
#include "sql_class.h"
#undef  OFFSET

#define NOPARSE
#include "osutil.h"
#include "global.h"
#include "plgdbsem.h"
#include "connect.h"
#include "user_connect.h"
#include "mycat.h"

/****************************************************************************/
/*  Initialize the user_connect static member.                              */
/****************************************************************************/
PCONNECT user_connect::to_users= NULL;

/****************************************************************************/
/*  Get the work_size SESSION variable value .                              */
/****************************************************************************/
uint GetWorkSize(void);
void SetWorkSize(uint);

/* -------------------------- class user_connect -------------------------- */

/****************************************************************************/
/*  Constructor.                                                            */
/****************************************************************************/
user_connect::user_connect(THD *thd, const char *dbn)
{
  thdp= thd;
  next= NULL;
  previous= NULL;
  g= NULL;
  last_query_id= 0;
  count= 0;

  // Statistics
  nrd= fnd= nfd= 0;
  tb1= 0;
} // end of user_connect constructor


/****************************************************************************/
/*  Destructor.                                                             */
/****************************************************************************/
user_connect::~user_connect()
{
  // Terminate CONNECT and Plug-like environment, should return NULL
  g= CntExit(g);
} // end of user_connect destructor


/****************************************************************************/
/*  Initialization.                                                         */
/****************************************************************************/
bool user_connect::user_init()
{
  // Initialize Plug-like environment
  uint      worksize= GetWorkSize();
  PACTIVITY ap= NULL;
  PDBUSER   dup= NULL;

  // Areasize= 64M because of VEC tables. Should be parameterisable
//g= PlugInit(NULL, 67108864);
//g= PlugInit(NULL, 134217728);  // 128M was because of old embedded tests
  g= PlugInit(NULL, worksize);

  // Check whether the initialization is complete
  if (!g || !g->Sarea || PlugSubSet(g, g->Sarea, g->Sarea_Size)
         || !(dup= PlgMakeUser(g))) {
    if (g)
      printf("%s\n", g->Message);

    int rc= PlugExit(g);
    g= NULL;
    free(dup);
    return true;
    } // endif g->

  dup->Catalog= new MYCAT(NULL);

  ap= new ACTIVITY;
  memset(ap, 0, sizeof(ACTIVITY));
  strcpy(ap->Ap_Name, "CONNECT");
  g->Activityp= ap;
  g->Activityp->Aptr= dup;
  next= to_users;
  to_users= this;

  if (next)
    next->previous= this;

  last_query_id= thdp->query_id;
  count= 1;
  return false;
} // end of user_init


void user_connect::SetHandler(ha_connect *hc)
{
  PDBUSER dup= (PDBUSER)g->Activityp->Aptr;
  MYCAT  *mc= (MYCAT*)dup->Catalog;
  mc->SetHandler(hc);
}

/****************************************************************************/
/*  Check whether we begin a new query and if so cleanup the previous one.  */
/****************************************************************************/
bool user_connect::CheckCleanup(void)
{
  if (thdp->query_id > last_query_id) {
    uint worksize= GetWorkSize();

    PlugCleanup(g, true);

    if (g->Sarea_Size != worksize) {
      if (g->Sarea)
        free(g->Sarea);

      // Check whether the work area size was changed
      if (!(g->Sarea = PlugAllocMem(g, worksize))) {
        g->Sarea = PlugAllocMem(g, g->Sarea_Size);
        SetWorkSize(g->Sarea_Size);       // Was too big
      } else
        g->Sarea_Size = worksize;         // Ok

      } // endif worksize

    PlugSubSet(g, g->Sarea, g->Sarea_Size);
    g->Xchk = NULL;
    g->Createas = 0;
    g->Alchecked = 0;
    g->Mrr = 0;
    last_query_id= thdp->query_id;

    if (trace)
      printf("=====> Begin new query %llu\n", last_query_id);

    return true;
    } // endif query_id

  return false;
} // end of CheckCleanup

