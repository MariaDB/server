/* Copyright 2015 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
 */

//! @file some utility functions and classes not directly related to replication

#include "mariadb.h"
#include "wsrep_xid.h"
#include "sql_class.h"
#include "wsrep_mysqld.h" // for logging macros

#include <mysql/service_wsrep.h>

#include <algorithm> /* std::sort() */
/*
 * WSREPXid
 */

#define WSREP_XID_PREFIX "WSREPXi"
#define WSREP_XID_PREFIX_LEN 7
#define WSREP_XID_VERSION_OFFSET WSREP_XID_PREFIX_LEN
#define WSREP_XID_VERSION_1 'd'
#define WSREP_XID_VERSION_2 'e'
#define WSREP_XID_VERSION_3 'f'
#define WSREP_XID_UUID_OFFSET 8
#define WSREP_XID_SEQNO_OFFSET (WSREP_XID_UUID_OFFSET + sizeof(wsrep_uuid_t))
#define WSREP_XID_GTRID_LEN_V_1_2 (WSREP_XID_SEQNO_OFFSET + sizeof(wsrep_seqno_t))
#define WSREP_XID_RPL_GTID_OFFSET (WSREP_XID_SEQNO_OFFSET + sizeof(wsrep_seqno_t))
#define WSREP_XID_GTRID_LEN_V_3 (WSREP_XID_RPL_GTID_OFFSET + sizeof(wsrep_server_gtid_t))

void wsrep_xid_init(XID* xid, const wsrep::gtid& wsgtid, const wsrep_server_gtid_t& gtid)
{
  xid->formatID= 1;
  xid->gtrid_length= WSREP_XID_GTRID_LEN_V_3;
  xid->bqual_length= 0;
  memset(xid->data, 0, sizeof(xid->data));
  memcpy(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN);
  xid->data[WSREP_XID_VERSION_OFFSET]= WSREP_XID_VERSION_3;
  memcpy(xid->data + WSREP_XID_UUID_OFFSET,  wsgtid.id().data(),sizeof(wsrep::id));
  int8store(xid->data + WSREP_XID_SEQNO_OFFSET, wsgtid.seqno().get());
  memcpy(xid->data + WSREP_XID_RPL_GTID_OFFSET, &gtid, sizeof(wsrep_server_gtid_t));
}

extern "C"
int wsrep_is_wsrep_xid(const void* xid_ptr)
{
  const XID* xid= static_cast<const XID*>(xid_ptr);
  return (xid->formatID      == 1                   &&
          xid->bqual_length  == 0                   &&
          xid->gtrid_length  >= static_cast<long>(WSREP_XID_GTRID_LEN_V_1_2) &&
          !memcmp(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN)      &&
          (((xid->data[WSREP_XID_VERSION_OFFSET] == WSREP_XID_VERSION_1   ||
             xid->data[WSREP_XID_VERSION_OFFSET] == WSREP_XID_VERSION_2)  &&
            xid->gtrid_length  == WSREP_XID_GTRID_LEN_V_1_2)              ||
           (xid->data[WSREP_XID_VERSION_OFFSET] == WSREP_XID_VERSION_3    &&
            xid->gtrid_length  == WSREP_XID_GTRID_LEN_V_3)));
}

const unsigned char* wsrep_xid_uuid(const xid_t* xid)
{
  DBUG_ASSERT(xid);
  static wsrep::id const undefined;
  if (wsrep_is_wsrep_xid(xid))
    return reinterpret_cast<const unsigned char*>
        (xid->data + WSREP_XID_UUID_OFFSET);
  else
    return static_cast<const unsigned char*>(wsrep::id::undefined().data());
}

const wsrep::id& wsrep_xid_uuid(const XID& xid)
{
  compile_time_assert(sizeof(wsrep::id) == sizeof(wsrep_uuid_t));
  return *reinterpret_cast<const wsrep::id*>(wsrep_xid_uuid(&xid));
}

long long wsrep_xid_seqno(const xid_t* xid)
{
  DBUG_ASSERT(xid);
  long long ret= wsrep::seqno::undefined().get();
  if (wsrep_is_wsrep_xid(xid))
  {
    switch (xid->data[WSREP_XID_VERSION_OFFSET])
    {
    case WSREP_XID_VERSION_1:
      memcpy(&ret, xid->data + WSREP_XID_SEQNO_OFFSET, sizeof ret);
      break;
    case WSREP_XID_VERSION_2:
    case WSREP_XID_VERSION_3:
      ret= sint8korr(xid->data + WSREP_XID_SEQNO_OFFSET);
      break;
    default:
      break;
    }
  }
  return ret;
}

wsrep::seqno wsrep_xid_seqno(const XID& xid)
{
  return wsrep::seqno(wsrep_xid_seqno(&xid));
}

static my_bool set_SE_checkpoint(THD* unused, plugin_ref plugin, void* arg)
{
  XID* xid= static_cast<XID*>(arg);
  handlerton* hton= plugin_data(plugin, handlerton *);

  if (hton->set_checkpoint)
  {
    const unsigned char* uuid= wsrep_xid_uuid(xid);
    char uuid_str[40]= {0, };
    wsrep_uuid_print((const wsrep_uuid_t*)uuid, uuid_str, sizeof(uuid_str));
    WSREP_DEBUG("Set WSREPXid for InnoDB:  %s:%lld",
                uuid_str, (long long)wsrep_xid_seqno(xid));
    hton->set_checkpoint(hton, xid);
  }
  return FALSE;
}

bool wsrep_set_SE_checkpoint(XID& xid)
{
  return plugin_foreach(NULL, set_SE_checkpoint, MYSQL_STORAGE_ENGINE_PLUGIN,
                        &xid);
}

bool wsrep_set_SE_checkpoint(const wsrep::gtid& wsgtid, const wsrep_server_gtid_t& gtid)
{
  XID xid;
  wsrep_xid_init(&xid, wsgtid, gtid);
  return wsrep_set_SE_checkpoint(xid);
}

static my_bool get_SE_checkpoint(THD* unused, plugin_ref plugin, void* arg)
{
  XID* xid= reinterpret_cast<XID*>(arg);
  handlerton* hton= plugin_data(plugin, handlerton *);

  if (hton->get_checkpoint)
  {
    hton->get_checkpoint(hton, xid);
    wsrep_uuid_t uuid;
    memcpy(&uuid, wsrep_xid_uuid(xid), sizeof(uuid));
    char uuid_str[40]= {0, };
    wsrep_uuid_print(&uuid, uuid_str, sizeof(uuid_str));
    WSREP_DEBUG("Read WSREPXid from InnoDB:  %s:%lld",
                uuid_str, (long long)wsrep_xid_seqno(xid));
  }
  return FALSE;
}

bool wsrep_get_SE_checkpoint(XID& xid)
{
  return plugin_foreach(NULL, get_SE_checkpoint, MYSQL_STORAGE_ENGINE_PLUGIN,
                        &xid);
}

static bool wsrep_get_SE_checkpoint_common(XID& xid)
{
  xid.null();

  if (wsrep_get_SE_checkpoint(xid))
  {
    return FALSE;
  }

  if (xid.is_null())
  {
    return FALSE;
  }

  if (!wsrep_is_wsrep_xid(&xid))
  {
    WSREP_WARN("Read non-wsrep XID from storage engines.");
    return FALSE;
  }

  return TRUE;
}

template<>
wsrep::gtid wsrep_get_SE_checkpoint()
{
  XID xid;

  if (!wsrep_get_SE_checkpoint_common(xid))
  {
    return wsrep::gtid();
  }

  return wsrep::gtid(wsrep_xid_uuid(xid),wsrep_xid_seqno(xid));
}

template<>
wsrep_server_gtid_t wsrep_get_SE_checkpoint()
{
  XID xid;
  wsrep_server_gtid_t gtid= {0,0,0};

  if (!wsrep_get_SE_checkpoint_common(xid))
  {
    return gtid;
  }

  if (xid.data[WSREP_XID_VERSION_OFFSET] == WSREP_XID_VERSION_3)
  {
    memcpy(&gtid, &xid.data[WSREP_XID_RPL_GTID_OFFSET], sizeof(wsrep_server_gtid_t));
  }

  return gtid;
}

/*
  Sort order for XIDs. Wsrep XIDs are sorted according to
  seqno in ascending order. Non-wsrep XIDs are considered
  equal among themselves and greater than with respect
  to wsrep XIDs.
 */
struct Wsrep_xid_cmp
{
  bool operator()(const XID& left, const XID& right) const
  {
    const bool left_is_wsrep= wsrep_is_wsrep_xid(&left);
    const bool right_is_wsrep= wsrep_is_wsrep_xid(&right);
    if (left_is_wsrep && right_is_wsrep)
    {
      return (wsrep_xid_seqno(&left) < wsrep_xid_seqno(&right));
    }
    else if (left_is_wsrep)
    {
      return true;
    }
    else
    {
      return false;
    }
  }
};

void wsrep_sort_xid_array(XID *array, int len)
{
  std::sort(array, array + len, Wsrep_xid_cmp());
}
