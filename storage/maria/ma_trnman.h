/* Copyright (C) 2006-2008 MySQL AB, 2008-2009 Sun Microsystems, Inc.

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

#ifndef _ma_trnman_h
#define _ma_trnman_h

/**
   Sets table's trn and prints debug information
   Links table into new_trn->used_instances

   @param tbl              MARIA_HA of table
   @param newtrn           what to put into tbl->trn
*/

static inline void _ma_set_trn_for_table(MARIA_HA *tbl, TRN *newtrn)
{
  DBUG_PRINT("info",("table: %p  trn: %p -> %p",
                     tbl, tbl->trn, newtrn));

  /* check that we are not calling this twice in a row */
  DBUG_ASSERT(newtrn->used_instances != (void*) tbl);

  tbl->trn= newtrn;
  /* Link into used list */
  if (newtrn->used_instances)
    ((MARIA_HA*) newtrn->used_instances)->trn_prev= &tbl->trn_next;
  tbl->trn_next= (MARIA_HA*) newtrn->used_instances;
  tbl->trn_prev= (MARIA_HA**) &newtrn->used_instances;
  newtrn->used_instances= tbl;
}


/*
  Same as _ma_set_trn_for_table(), but don't link table into used_instance list
  Used when we want to temporary set trn for a table in extra()
*/ 

static inline void _ma_set_tmp_trn_for_table(MARIA_HA *tbl, TRN *newtrn)
{
  DBUG_PRINT("info",("table: %p  trn: %p -> %p",
                     tbl, tbl->trn, newtrn));
  tbl->trn= newtrn;
  tbl->trn_prev= 0;
  tbl->trn_next= 0;               /* To avoid assert in ha_maria::close() */
}


/*
  Reset TRN in table
*/

static inline void _ma_reset_trn_for_table(MARIA_HA *tbl)
{
  DBUG_PRINT("info",("table: %p  trn: %p -> NULL", tbl, tbl->trn));

  /* The following is only false if tbl->trn == &dummy_transaction_object */
  if (tbl->trn_prev)
  {
    if (tbl->trn_next)
      tbl->trn_next->trn_prev= tbl->trn_prev;
    *tbl->trn_prev= tbl->trn_next;
    tbl->trn_prev= 0;
    tbl->trn_next= 0;
  }
  tbl->trn= 0;
}


/*
  Take over the used_instances link from a trn object
  Reset the link in the trn object
*/

static inline void relink_trn_used_instances(MARIA_HA **used_tables, TRN *trn)
{
  if (likely(*used_tables= (MARIA_HA*) trn->used_instances))
  {
    /* Check that first back link is correct */
    DBUG_ASSERT((*used_tables)->trn_prev == (MARIA_HA **)&trn->used_instances);

    /* Fix back link to point to new base for the list */
    (*used_tables)->trn_prev= used_tables;
    trn->used_instances= 0;
  }
}

#endif /* _ma_trnman_h */
