#ifndef XA_INCLUDED
#define XA_INCLUDED
/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

class XID_cache_element;
enum xa_states
{
  XA_ACTIVE= 0,
  XA_IDLE,
  XA_PREPARED,
  XA_ROLLBACK_ONLY,
  XA_NO_STATE
};

struct XID_STATE {
  XID_cache_element *xid_cache_element;

  bool check_has_uncommitted_xa() const;
  bool is_explicit_XA() const { return xid_cache_element != 0; }
  void set_error(uint error);
  void er_xaer_rmfail() const;
  XID *get_xid() const;
  enum xa_states get_state_code() const;
};

void xid_cache_init(void);
void xid_cache_free(void);
bool xid_cache_insert(XID *xid);
bool xid_cache_insert(THD *thd, XID_STATE *xid_state, XID *xid);
void xid_cache_delete(THD *thd, XID_STATE *xid_state);

bool trans_xa_start(THD *thd);
bool trans_xa_end(THD *thd);
bool trans_xa_prepare(THD *thd);
bool trans_xa_commit(THD *thd);
bool trans_xa_rollback(THD *thd);
bool trans_xa_detach(THD *thd);
bool mysql_xa_recover(THD *thd);

void xa_recover_get_fields(THD *thd, List<Item> *field_list,
                           my_hash_walk_action *action);

#endif /* XA_INCLUDED */
