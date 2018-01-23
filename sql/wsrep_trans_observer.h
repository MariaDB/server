/* Copyright 2016 Codership Oy <http://www.codership.com>

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

#ifndef WSREP_TRANS_OBSERVER_H
#define WSREP_TRANS_OBSERVER_H

#include "my_global.h"
#include "wsrep_applier.h" /* wsrep_apply_error */

class THD;

/*
  Called after each row operation.

  Return zero on succes, non-zero on failure.
 */
int wsrep_after_row(THD*, bool);

/*
  Called before the transaction is prepared.

  Return zero on succes, non-zero on failure.
 */
int wsrep_before_prepare(THD*, bool);

/*
  Called after the transaction has been prepared.

  Return zero on succes, non-zero on failure.
 */
int wsrep_after_prepare(THD*, bool);


/*
  Called before the transaction is committed.

  This function must be called from both client and
  applier contexts before commit.

  Return zero on succes, non-zero on failure.
 */
int wsrep_before_commit(THD*, bool);

/*
  Called after the transaction has been ordered for commit.

  This function must be called from both client and
  applier contexts after the commit has been ordered.

  @param thd Pointer to THD
  @param all 
  @param err Error buffer in case of applying error

  Return zero on succes, non-zero on failure.
 */
int wsrep_ordered_commit(THD* thd, bool all, const wsrep_apply_error& err);

/*
  Called after the transaction has been committed.

  Return zero on succes, non-zero on failure.
 */
int wsrep_after_commit(THD*, bool);

/*
  Called before the transaction is rolled back.

  Return zero on succes, non-zero on failure.
 */
int wsrep_before_rollback(THD*, bool);

/*
  Called after the transaction has been rolled back.

  Return zero on succes, non-zero on failure.
 */
int wsrep_after_rollback(THD*, bool);

/*
  Called after each command.

  Return zero on success, non-zero on failure.
 */
int wsrep_after_command(THD*, bool);

/*
  Called before a GTID is logged into binlog without committing a
  transaction.

  Return zero on success, non-zero on failure.
 */
int wsrep_before_GTID_binlog(THD* thd, bool);


/*
  Register wsrep transaction observer hooks
*/
int wsrep_register_trans_observer(void *p);

/*
  Unregister wsrep transaction observer hooks
 */
int wsrep_unregister_trans_observer(void *p);

#endif /* WSREP_TRANS_OBSERVER */
