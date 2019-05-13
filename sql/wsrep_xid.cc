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

#include "wsrep_xid.h"
#include "sql_class.h"
#include "wsrep_mysqld.h" // for logging macros

#include <algorithm> /* std::sort() */

/*
 * WSREPXid
 */

#define WSREP_XID_PREFIX "WSREPXid"
#define WSREP_XID_PREFIX_LEN MYSQL_XID_PREFIX_LEN
#define WSREP_XID_UUID_OFFSET 8
#define WSREP_XID_SEQNO_OFFSET (WSREP_XID_UUID_OFFSET + sizeof(wsrep_uuid_t))
#define WSREP_XID_GTRID_LEN (WSREP_XID_SEQNO_OFFSET + sizeof(wsrep_seqno_t))

void wsrep_xid_init(XID* xid, const wsrep_uuid_t& uuid, wsrep_seqno_t seqno)
{
  xid->formatID= 1;
  xid->gtrid_length= WSREP_XID_GTRID_LEN;
  xid->bqual_length= 0;
  memset(xid->data, 0, sizeof(xid->data));
  memcpy(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN);
  memcpy(xid->data + WSREP_XID_UUID_OFFSET,  &uuid,  sizeof(wsrep_uuid_t));
  memcpy(xid->data + WSREP_XID_SEQNO_OFFSET, &seqno, sizeof(wsrep_seqno_t));
}

int wsrep_is_wsrep_xid(const XID* xid)
{
  return (xid->formatID      == 1                   &&
          xid->gtrid_length  == WSREP_XID_GTRID_LEN &&
          xid->bqual_length  == 0                   &&
          !memcmp(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN));
}

const wsrep_uuid_t* wsrep_xid_uuid(const XID& xid)
{
  if (wsrep_is_wsrep_xid(&xid))
    return reinterpret_cast<const wsrep_uuid_t*>(xid.data
                                                 + WSREP_XID_UUID_OFFSET);
  else
    return &WSREP_UUID_UNDEFINED;
}

wsrep_seqno_t wsrep_xid_seqno(const XID& xid)
{
  if (wsrep_is_wsrep_xid(&xid))
  {
    wsrep_seqno_t seqno;
    memcpy(&seqno, xid.data + WSREP_XID_SEQNO_OFFSET, sizeof(wsrep_seqno_t));
    return seqno;
  }
  else
  {
    return WSREP_SEQNO_UNDEFINED;
  }
}

static my_bool set_SE_checkpoint(THD* unused, plugin_ref plugin, void* arg)
{
  XID* xid= static_cast<XID*>(arg);
  handlerton* hton= plugin_data(plugin, handlerton *);

  if (hton->set_checkpoint)
  {
    const wsrep_uuid_t* uuid(wsrep_xid_uuid(*xid));
    char uuid_str[40] = {0, };
    wsrep_uuid_print(uuid, uuid_str, sizeof(uuid_str));
    WSREP_DEBUG("Set WSREPXid for InnoDB:  %s:%lld",
                uuid_str, (long long)wsrep_xid_seqno(*xid));
    hton->set_checkpoint(hton, xid);
  }
  return FALSE;
}

bool wsrep_set_SE_checkpoint(XID& xid)
{
  return plugin_foreach(NULL, set_SE_checkpoint, MYSQL_STORAGE_ENGINE_PLUGIN,
                        &xid);
}

bool wsrep_set_SE_checkpoint(const wsrep_uuid_t& uuid, wsrep_seqno_t seqno)
{
  XID xid;
  wsrep_xid_init(&xid, uuid, seqno);
  return wsrep_set_SE_checkpoint(xid);
}

static my_bool get_SE_checkpoint(THD* unused, plugin_ref plugin, void* arg)
{
  XID* xid= reinterpret_cast<XID*>(arg);
  handlerton* hton= plugin_data(plugin, handlerton *);

  if (hton->get_checkpoint)
  {
    hton->get_checkpoint(hton, xid);
    const wsrep_uuid_t* uuid(wsrep_xid_uuid(*xid));
    char uuid_str[40] = {0, };
    wsrep_uuid_print(uuid, uuid_str, sizeof(uuid_str));
    WSREP_DEBUG("Read WSREPXid from InnoDB:  %s:%lld",
                uuid_str, (long long)wsrep_xid_seqno(*xid));
  }
  return FALSE;
}

bool wsrep_get_SE_checkpoint(XID& xid)
{
  return plugin_foreach(NULL, get_SE_checkpoint, MYSQL_STORAGE_ENGINE_PLUGIN,
                        &xid);
}

bool wsrep_get_SE_checkpoint(wsrep_uuid_t& uuid, wsrep_seqno_t& seqno)
{
  uuid= WSREP_UUID_UNDEFINED;
  seqno= WSREP_SEQNO_UNDEFINED;

  XID xid;
  xid.null();

  if (wsrep_get_SE_checkpoint(xid))
  {
    return true;
  }

  if (xid.is_null())
  {
    return false;
  }

  if (!wsrep_is_wsrep_xid(&xid))
  {
    WSREP_WARN("Read non-wsrep XID from storage engines.");
    return false;
  }

  uuid= *wsrep_xid_uuid(xid);
  seqno= wsrep_xid_seqno(xid);

  return false;
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
      return (wsrep_xid_seqno(left) < wsrep_xid_seqno(right));
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
