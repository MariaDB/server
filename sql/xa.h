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


enum xa_states {XA_NOTR=0, XA_ACTIVE, XA_IDLE, XA_PREPARED, XA_ROLLBACK_ONLY};
extern const char *xa_state_names[];
class XID_cache_element;

struct XID_STATE {
  /* For now, this is only used to catch duplicated external xids */
  XID  xid;                           // transaction identifier
  enum xa_states xa_state;            // used by external XA only
  /* Error reported by the Resource Manager (RM) to the Transaction Manager. */
  uint rm_error;
  XID_cache_element *xid_cache_element;

  /**
    Check that XA transaction has an uncommitted work. Report an error
    to the user in case when there is an uncommitted work for XA transaction.

    @return  result of check
      @retval  false  XA transaction is NOT in state IDLE, PREPARED
                      or ROLLBACK_ONLY.
      @retval  true   XA transaction is in state IDLE or PREPARED
                      or ROLLBACK_ONLY.
  */

  bool check_has_uncommitted_xa() const
  {
    if (xa_state == XA_IDLE ||
        xa_state == XA_PREPARED ||
        xa_state == XA_ROLLBACK_ONLY)
    {
      my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[xa_state]);
      return true;
    }
    return false;
  }
};

void xid_cache_init(void);
void xid_cache_free(void);
bool xid_cache_insert(XID *xid, enum xa_states xa_state);
bool xid_cache_insert(THD *thd, XID_STATE *xid_state);
void xid_cache_delete(THD *thd, XID_STATE *xid_state);

bool trans_xa_start(THD *thd);
bool trans_xa_end(THD *thd);
bool trans_xa_prepare(THD *thd);
bool trans_xa_commit(THD *thd);
bool trans_xa_rollback(THD *thd);
bool mysql_xa_recover(THD *thd);
