/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of TokuDB


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    TokuDBis is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    TokuDB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TokuDB.  If not, see <http://www.gnu.org/licenses/>.

======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

/****************************************************************************
 * DS-MRR implementation, essentially copied from InnoDB/MyISAM/Maria
 ***************************************************************************/

/**
 * Multi Range Read interface, DS-MRR calls
 */
int ha_tokudb::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                       uint n_ranges, uint mode, 
                                       HANDLER_BUFFER *buf)
{
  return ds_mrr.dsmrr_init(this, seq, seq_init_param, n_ranges, mode, buf);
}

int ha_tokudb::multi_range_read_next(range_id_t *range_info)
{
  return ds_mrr.dsmrr_next(range_info);
}

ha_rows ha_tokudb::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                                 void *seq_init_param,  
                                                 uint n_ranges, uint *bufsz,
                                                 uint *flags, 
                                                 COST_VECT *cost)
{
  /* See comments in ha_myisam::multi_range_read_info_const */
  ds_mrr.init(this, table);
  ha_rows res= ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param, n_ranges,
                                       bufsz, flags, cost);
  return res;
}

ha_rows ha_tokudb::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                           uint key_parts, uint *bufsz, 
                                           uint *flags, COST_VECT *cost)
{
  ds_mrr.init(this, table);
  ha_rows res= ds_mrr.dsmrr_info(keyno, n_ranges, keys, key_parts, bufsz, 
                                 flags, cost);
  return res;
}

int ha_tokudb::multi_range_read_explain_info(uint mrr_mode, char *str, size_t size)
{
  return ds_mrr.dsmrr_explain_info(mrr_mode, str, size);
}
