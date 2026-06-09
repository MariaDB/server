/* Copyright (c) 2026 Dearsh Oberoi

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include "myisamdef.h"
#include <my_rnd.h>

int mi_random_dive(MI_INFO *info, int inx, uchar *buf,
                    struct my_rnd_struct *rand_state)
{
  MI_KEYDEF *keyinfo;
  my_off_t pos;
  uchar *buff, *page, *end, t_buff[HA_MAX_KEY_BUFF];
  uint nod_flag, max_keynr, total_slots, chosen_slot, cur_slot;
  DBUG_ENTER("mi_random_dive");

  if ((inx= _mi_check_index(info,inx)) < 0)
    DBUG_RETURN(my_errno);

  if (fast_mi_readinfo(info))
    DBUG_RETURN(my_errno);

  keyinfo=info->s->keyinfo+inx;
  if (keyinfo->key_alg != HA_KEY_ALG_BTREE)
  {
    my_errno= HA_ERR_WRONG_COMMAND;
    DBUG_RETURN(my_errno);
  }

  if (info->s->concurrent_insert)
    mysql_rwlock_rdlock(&info->s->key_root_lock[inx]);

  pos= info->s->state.key_root[inx];

  for(;;)
  {
    if (!(buff=_mi_fetch_keypage(info,keyinfo,pos,DFLT_INIT_HITS,info->buff,1)))
      goto err;

    nod_flag=mi_test_if_nod(buff);
    page= buff+2+nod_flag;
    end= buff+mi_getint(buff);
    t_buff[0]= 0;
    max_keynr= 0;

    while (page < end)
    {
      if (!(*keyinfo->get_key)(keyinfo,nod_flag,&page,t_buff))
        goto err;
      max_keynr++;
    }

    total_slots= max_keynr+(nod_flag > 0 ? 1 : 0);
    if (!total_slots)
    {
      my_errno= HA_ERR_END_OF_FILE;
      goto err;
    }
    chosen_slot= (uint) (total_slots*my_rnd(rand_state));
    if (chosen_slot >= total_slots)
      chosen_slot--;

    page= buff+2+nod_flag;
    t_buff[0]= 0;
    cur_slot= 0;
    while (page < end && cur_slot < chosen_slot + (nod_flag ? 0 : 1))
    {
      if (!(*keyinfo->get_key)(keyinfo,nod_flag,&page,t_buff))
        goto err;
      cur_slot++;
    }

    if (nod_flag)
    {
      pos= _mi_kpos(nod_flag, page);
    } else {
      pos= _mi_dpos(info, nod_flag, page);
      break;
    }
  }

  info->lastpos= pos;

  if (info->s->concurrent_insert)
    mysql_rwlock_unlock(&info->s->key_root_lock[inx]);

  if (!(*info->read_record)(info,info->lastpos,buf))
  {
    info->update|= HA_STATE_AKTIV;
    DBUG_RETURN(0);
  }

  DBUG_RETURN(my_errno);

err:
  if (info->s->concurrent_insert)
    mysql_rwlock_unlock(&info->s->key_root_lock[inx]);
  DBUG_RETURN(my_errno);
}