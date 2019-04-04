/* Copyright (C) 2009-2014 Kentoku Shiba

  This program is free software); you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation); version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY); without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program); if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

my_bool vp_param_support_xa();
int vp_param_choose_table_mode(
  THD *thd,
  int choose_table_mode
);
int vp_param_choose_table_mode_for_lock(
  THD *thd,
  int choose_table_mode_for_lock
);
int vp_param_multi_range_mode(
  THD *thd,
  int multi_range_mode
);
int vp_param_child_binlog(
  THD *thd,
  int child_binlog
);
#ifndef WITHOUT_VP_BG_ACCESS
int vp_param_bgs_mode(
  THD *thd,
  int bgs_mode
);
int vp_param_bgi_mode(
  THD *thd,
  int bgi_mode
);
int vp_param_bgu_mode(
  THD *thd,
  int bgu_mode
);
#endif
int vp_param_allow_bulk_autoinc(
  THD *thd,
  int allow_bulk_autoinc
);
int vp_param_udf_ct_bulk_insert_interval(
  int udf_ct_bulk_insert_interval
);
longlong vp_param_udf_ct_bulk_insert_rows(
  longlong udf_ct_bulk_insert_rows
);
