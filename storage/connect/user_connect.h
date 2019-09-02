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

/** @file user_connect.h

    @brief
  Declaration of the user_connect class.

    @note
	Author Olivier Bertrand

    @see
  /sql/handler.h and /storage/connect/user_connect.cc
*/

#ifdef USE_PRAGMA_INTERFACE
#pragma interface     /* gcc class implementation */
#endif

/*****************************************************************************/
/*  This is the global structure having all CONNECT information.             */
/*****************************************************************************/
//typedef struct _global *PGLOBAL;
typedef class user_connect *PCONNECT;
typedef class ha_connect *PHC;
static int connect_done_func(void *);

/*****************************************************************************/
/*  The CONNECT users. There should be one by connected users.               */
/*****************************************************************************/
class user_connect
{
  friend class ha_connect;
  friend int connect_done_func(void *);
public:
  // Constructor
  user_connect(THD *thd);

  // Destructor
  virtual ~user_connect();

  // Implementation
  bool user_init();
  void SetHandler(ha_connect *hc);
  bool CheckCleanup(bool force = false);
  bool CheckQueryID(void) {return thdp->query_id > last_query_id;}
  bool CheckQuery(query_id_t vid) {return last_query_id > vid;}

  // Members
  THD         *thdp;                    // To the user thread
  static PCONNECT  to_users;            // To the chain of users
  PCONNECT     next;                    // Next user in chain
  PCONNECT     previous;                // Previous user in chain
  PGLOBAL      g;                       // The common handle to CONNECT
  query_id_t   last_query_id;           // the latest user query id
  int          count;                   // if used by several handlers
  // Statistics
  ulong        nrd, fnd, nfd;
  ulonglong    tb1;
}; // end of user_connect class definition

