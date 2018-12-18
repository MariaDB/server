/* Copyright (c) 2000, 2004-2006 MySQL AB
   Use is subject to license terms

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

/* Extra functions we want to do with a database */
/* - Set flags for quicker databasehandler */
/* - Set databasehandler to normal */
/* - Reset recordpointers as after open database */

#include "heapdef.h"

static void heap_extra_keyflag(register HP_INFO *info,
                               enum ha_extra_function function);

#define REMEMBER_OLD_POS 64U

	/* set extra flags for database */

int heap_extra(register HP_INFO *info, enum ha_extra_function function)
{
  DBUG_ENTER("heap_extra");

  const HP_SHARE *share= info->s;

  switch (function) {
  case HA_EXTRA_RESET_STATE:
    heap_reset(info);
    /* fall through */
  case HA_EXTRA_NO_READCHECK:
    info->opt_flag&= ~READ_CHECK_USED;	/* No readcheck */
    break;
  case HA_EXTRA_READCHECK:
    info->opt_flag|= READ_CHECK_USED;
    break;
  case HA_EXTRA_CHANGE_KEY_TO_UNIQUE:
  case HA_EXTRA_CHANGE_KEY_TO_DUP:
    heap_extra_keyflag(info, function);
    break;
  case HA_EXTRA_REMEMBER_POS:
    info->opt_flag|= REMEMBER_OLD_POS;
    bmove((uchar*) info->lastkey + share->max_key_length * 2,
	  (uchar*) info->lastkey, info->lastkey_len);
    info->save_update=	info->update;
    info->save_lastinx= info->lastinx;
    info->save_current_ptr= info->current_ptr;
    info->save_current_hash_ptr= info->current_hash_ptr;
    info->save_lastkey_len= info->lastkey_len;
    info->save_current_record= info->current_record;
    break;
  case HA_EXTRA_RESTORE_POS:
    if (info->opt_flag & REMEMBER_OLD_POS)
    {
      bmove((uchar*) info->lastkey,
	    (uchar*) info->lastkey + share->max_key_length * 2,
	    info->save_lastkey_len);
      info->update= info->save_update | HA_STATE_WRITTEN;
      info->lastinx= info->save_lastinx;
      info->current_ptr= info->save_current_ptr;
      info->current_hash_ptr= info->save_current_hash_ptr;
      info->lastkey_len= info->save_lastkey_len;
      info->current_record= info->save_current_record;
      info->next_block= 0;
    }
    info->opt_flag&= ~REMEMBER_OLD_POS;
    break;
  default:
    break;
  }
  DBUG_RETURN(0);
} /* heap_extra */


int heap_reset(HP_INFO *info)
{
  info->lastinx= -1;
  info->current_record= (ulong) ~0L;
  info->current_hash_ptr=0;
  info->update=0;
  info->next_block=0;
  return 0;
}


/*
    Start/Stop Inserting Duplicates Into a Table, WL#1648.
 */
static void heap_extra_keyflag(register HP_INFO *info,
                               enum ha_extra_function function)
{
  uint  idx;

  for (idx= 0; idx< info->s->keys; idx++)
  {
    switch (function) {
    case HA_EXTRA_CHANGE_KEY_TO_UNIQUE:
      info->s->keydef[idx].flag|= HA_NOSAME;
      break;
    case HA_EXTRA_CHANGE_KEY_TO_DUP:
      info->s->keydef[idx].flag&= ~(HA_NOSAME);
      break;
    default:
      break;
    }
  }
}
