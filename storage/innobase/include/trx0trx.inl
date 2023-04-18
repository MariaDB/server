/*****************************************************************************

Copyright (c) 1996, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2021, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/trx0trx.ic
The transaction

Created 3/26/1996 Heikki Tuuri
*******************************************************/

/**********************************************************************//**
Determines if a transaction is in the given state.
The caller must hold trx->mutex, or it must be the thread
that is serving a running transaction.
A running RW transaction must be in trx_sys.rw_trx_hash.
@return TRUE if trx->state == state */
UNIV_INLINE
bool
trx_state_eq(
/*=========*/
	const trx_t*	trx,	/*!< in: transaction */
	trx_state_t	state,	/*!< in: state;
				if state != TRX_STATE_NOT_STARTED
				asserts that
				trx->state != TRX_STATE_NOT_STARTED */
	bool		relaxed)
				/*!< in: whether to allow
				trx->state == TRX_STATE_NOT_STARTED
				after an error has been reported */
{
#ifdef UNIV_DEBUG
	switch (trx->state) {
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		ut_ad(!trx->is_autocommit_non_locking());
		return(trx->state == state);

	case TRX_STATE_ACTIVE:
		if (trx->is_autocommit_non_locking()) {
			ut_ad(!trx->is_recovered);
			ut_ad(trx->read_only);
			ut_ad(trx->mysql_thd);
		}
		return(state == trx->state);

	case TRX_STATE_NOT_STARTED:
		/* These states are not allowed for running transactions. */
		ut_a(state == TRX_STATE_NOT_STARTED
		     || (relaxed
			 && thd_get_error_number(trx->mysql_thd)));

		return(true);
	}
	ut_error;
#endif /* UNIV_DEBUG */
	return(trx->state == state);
}

/****************************************************************//**
Retrieves the error_info field from a trx.
@return the error info */
UNIV_INLINE
const dict_index_t*
trx_get_error_info(
/*===============*/
	const trx_t*	trx)	/*!< in: trx object */
{
	return(trx->error_info);
}
