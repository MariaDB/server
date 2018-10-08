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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
 */

//! @file some utility functions and classes not directly related to replication

#include "mariadb.h"
#include "wsrep_xid.h"
#include "sql_class.h"
#include "wsrep_mysqld.h" // for logging macros

#include <sstream>
/*
 * WSREPXid
 */

#define WSREP_XID_PREFIX "WSREPXi"
#define WSREP_XID_PREFIX_LEN 7
#define WSREP_XID_VERSION_OFFSET WSREP_XID_PREFIX_LEN
#define WSREP_XID_VERSION_1 'd'
#define WSREP_XID_VERSION_2 'e'
#define WSREP_XID_UUID_OFFSET 8
#define WSREP_XID_SEQNO_OFFSET (WSREP_XID_UUID_OFFSET + sizeof(wsrep_uuid_t))
#define WSREP_XID_GTRID_LEN (WSREP_XID_SEQNO_OFFSET + sizeof(wsrep_seqno_t))

void wsrep_xid_init(XID* xid, const wsrep::gtid& gtid)
{
  xid->formatID= 1;
  xid->gtrid_length= WSREP_XID_GTRID_LEN;
  xid->bqual_length= 0;
  memset(xid->data, 0, sizeof(xid->data));
  memcpy(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN);
  memcpy(xid->data + WSREP_XID_UUID_OFFSET,
         gtid.id().data(), gtid.id().size());
  long long seqno= gtid.seqno().get();
  memcpy(xid->data + WSREP_XID_SEQNO_OFFSET, &seqno, sizeof(seqno));
}

//extern "C"
int wsrep_is_wsrep_xid(const void* xid_ptr)
{
  const XID* xid= reinterpret_cast<const XID*>(xid_ptr);
  return (xid->formatID      == 1                   &&
          xid->gtrid_length  == WSREP_XID_GTRID_LEN &&
          xid->bqual_length  == 0                   &&
          !memcmp(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN) &&
          (xid->data[WSREP_XID_VERSION_OFFSET] == WSREP_XID_VERSION_1 ||
           xid->data[WSREP_XID_VERSION_OFFSET] == WSREP_XID_VERSION_2));
}

wsrep::id wsrep_xid_uuid(const XID& xid)
{
  wsrep::id ret(xid.data + WSREP_XID_UUID_OFFSET, 16);
  if (wsrep_is_wsrep_xid(&xid))
    return wsrep::id(xid.data + WSREP_XID_UUID_OFFSET, 16);
  else
    return wsrep::id::undefined();
}

wsrep::seqno wsrep_xid_seqno(const XID& xid)
{
  if (wsrep_is_wsrep_xid(&xid))
  {
    wsrep_seqno_t seqno;
    memcpy(&seqno, xid.data + WSREP_XID_SEQNO_OFFSET, sizeof(wsrep_seqno_t));
    return wsrep::seqno(seqno);
  }
  else
  {
    return wsrep::seqno::undefined();
  }
}

static my_bool set_SE_checkpoint(THD* unused, plugin_ref plugin, void* arg)
{
  XID* xid= static_cast<XID*>(arg);
  handlerton* hton= plugin_data(plugin, handlerton *);

  if (hton->db_type == DB_TYPE_INNODB)
  {
    const wsrep::id uuid(wsrep_xid_uuid(*xid));
    std::ostringstream oss;
    oss << uuid;
    WSREP_DEBUG("Set WSREPXid for InnoDB:  %s:%lld",
                oss.str().c_str(), get_wsrep_xid_seqno(xid));
    hton->set_checkpoint(hton, xid);
  }

  return FALSE;
}

void wsrep_set_SE_checkpoint(XID& xid)
{
  plugin_foreach(NULL, set_SE_checkpoint, MYSQL_STORAGE_ENGINE_PLUGIN, &xid);
}

void wsrep_set_SE_checkpoint(const wsrep::gtid& gtid)
{
  XID xid;
  wsrep_xid_init(&xid, gtid);
  wsrep_set_SE_checkpoint(xid);
}

static my_bool get_SE_checkpoint(THD* unused, plugin_ref plugin, void* arg)
{
  XID* xid= reinterpret_cast<XID*>(arg);
  handlerton* hton= plugin_data(plugin, handlerton *);

  if (hton->get_checkpoint)
  {
    hton->get_checkpoint(hton, xid);
    std::ostringstream oss;
    oss << wsrep_xid_uuid(*xid);
    oss << ":";
    oss << wsrep_xid_seqno(*xid);
    WSREP_DEBUG("Read WSREPXid from InnoDB:  %s", oss.str().c_str());
  }
  return FALSE;
}

void wsrep_get_SE_checkpoint(XID& xid)
{
  plugin_foreach(NULL, get_SE_checkpoint, MYSQL_STORAGE_ENGINE_PLUGIN,
                        &xid);
}

wsrep::gtid wsrep_get_SE_checkpoint()
{
  XID xid;
  xid.null();

  wsrep_get_SE_checkpoint(xid);

  if (xid.is_null())
  {
    return wsrep::gtid();
  }

  if (!wsrep_is_wsrep_xid(&xid))
  {
    WSREP_WARN("Read non-wsrep XID from storage engines.");
    return wsrep::gtid::undefined();
  }
  return wsrep::gtid(wsrep_xid_uuid(xid), wsrep_xid_seqno(xid));
}


