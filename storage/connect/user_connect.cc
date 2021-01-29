/* Copyright (C) MariaDB Corporation Ab

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

/**
  @file user_connect.cc

  @brief
  Implements the user_connect class.

  @details
  To support multi_threading, each query creates and use a PlugDB "user"
  that is a connection with its personnal memory allocation.

  @note
	Author Olivier Bertrand
*/

/****************************************************************************/
/*  Author: Olivier Bertrand  --  bertrandop@gmail.com  --  2004-2020       */
/****************************************************************************/
#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#define DONT_DEFINE_VOID
#define MYSQL_SERVER
#include <my_global.h>
#include "sql_class.h"
#undef  OFFSET

#define NOPARSE
#include "osutil.h"
#include "global.h"
#include "plgdbsem.h"
#include "connect.h"
#include "user_connect.h"
#include "mycat.h"

extern pthread_mutex_t usrmut;

/****************************************************************************/
/*  Initialize the user_connect static member.                              */
/****************************************************************************/
PCONNECT user_connect::to_users= NULL;

/****************************************************************************/
/*  Get the work_size SESSION variable value .                              */
/****************************************************************************/
size_t GetWorkSize(void);
void  SetWorkSize(size_t);

/* -------------------------- class user_connect -------------------------- */

/****************************************************************************/
/*  Constructor.                                                            */
/****************************************************************************/
user_connect::user_connect(THD *thd)
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
  size_t    worksize= GetWorkSize();
  PACTIVITY ap= NULL;
  PDBUSER   dup= NULL;

  // Areasize= 64M because of VEC tables. Should be parameterisable
//g= PlugInit(NULL, 67108864);
//g= PlugInit(NULL, 134217728);  // 128M was because of old embedded tests
  g= PlugInit(NULL, (size_t)worksize);

  // Check whether the initialization is complete
  if (!g || !g->Sarea || PlugSubSet(g->Sarea, g->Sarea_Size)
         || !(dup= PlgMakeUser(g))) {
    if (g)
      printf("%s\n", g->Message);

    g= PlugExit(g);

		if (dup)
	    free(dup);

    return true;
    } // endif g->

  dup->Catalog= new MYCAT(NULL);

  ap= new ACTIVITY;
  memset(ap, 0, sizeof(ACTIVITY));
  strcpy(ap->Ap_Name, "CONNECT");
  g->Activityp= ap;
  g->Activityp->Aptr= dup;

	pthread_mutex_lock(&usrmut);
  next= to_users;
  to_users= this;

  if (next)
    next->previous= this;

	count = 1;
	pthread_mutex_unlock(&usrmut);

	last_query_id= thdp->query_id;
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
bool user_connect::CheckCleanup(bool force)
{
  if (thdp->query_id > last_query_id || force) {
		size_t worksize = GetWorkSize();

    PlugCleanup(g, true);

    if (worksize != g->Sarea_Size) {
			FreeSarea(g);
			g->Saved_Size = g->Sarea_Size;

      // Check whether the work area could be allocated
      if (AllocSarea(g, worksize)) {
				AllocSarea(g, g->Saved_Size);
        SetWorkSize(g->Sarea_Size);       // Was too big
      } // endif sarea

    } // endif worksize

    PlugSubSet(g->Sarea, g->Sarea_Size);
    g->Xchk = NULL;
    g->Createas = false;
    g->Alchecked = 0;
    g->Mrr = 0;
		g->More = 0;
		g->Saved_Size = 0;
		last_query_id= thdp->query_id;

    if (trace(65) && !force)
      printf("=====> Begin new query %llu\n", last_query_id);

    return true;
    } // endif query_id

  return false;
} // end of CheckCleanup

