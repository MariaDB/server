/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates
   Copyright (c) 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* The hash functions used for saveing keys */

#include "heapdef.h"
#include <m_ctype.h>


static inline size_t
hp_charpos(CHARSET_INFO *cs, const uchar *b, const uchar *e, size_t num)
{
  return my_ci_charpos(cs, (const char*) b, (const char *) e, num);
}


static ulong hp_hashnr(HP_KEYDEF *keydef, const uchar *key);
/*
  Find out how many rows there is in the given range

  SYNOPSIS
    hp_rb_records_in_range()
    info		HEAP handler
    inx			Index to use
    min_key		Min key. Is = 0 if no min range
    max_key		Max key. Is = 0 if no max range

  NOTES
    min_key.flag can have one of the following values:
      HA_READ_KEY_EXACT		Include the key in the range
      HA_READ_AFTER_KEY		Don't include key in range

    max_key.flag can have one of the following values:
      HA_READ_BEFORE_KEY	Don't include key in range
      HA_READ_AFTER_KEY		Include all 'end_key' values in the range

  RETURN
   HA_POS_ERROR		Something is wrong with the index tree.
   0			There is no matching keys in the given range
   number > 0		There is approximately 'number' matching rows in
			the range.
*/

ha_rows hp_rb_records_in_range(HP_INFO *info, int inx,
                               const key_range *min_key,
                               const key_range *max_key)
{
  ha_rows start_pos, end_pos;
  HP_KEYDEF *keyinfo= info->s->keydef + inx;
  TREE *rb_tree = &keyinfo->rb_tree;
  heap_rb_param custom_arg;
  DBUG_ENTER("hp_rb_records_in_range");

  info->lastinx= inx;
  custom_arg.keyseg= keyinfo->seg;
  custom_arg.search_flag= SEARCH_FIND | SEARCH_SAME;
  if (min_key)
  {
    custom_arg.key_length= hp_rb_pack_key(keyinfo, (uchar*) info->recbuf,
					  (uchar*) min_key->key,
					  min_key->keypart_map);
    start_pos= tree_record_pos(rb_tree, info->recbuf, min_key->flag,
			       &custom_arg);
  }
  else
  {
    start_pos= 0;
  }

  if (max_key)
  {
    custom_arg.key_length= hp_rb_pack_key(keyinfo, (uchar*) info->recbuf,
					  (uchar*) max_key->key,
                                          max_key->keypart_map);
    end_pos= tree_record_pos(rb_tree, info->recbuf, max_key->flag,
			     &custom_arg);
  }
  else
  {
    end_pos= rb_tree->elements_in_tree + (ha_rows)1;
  }

  DBUG_PRINT("info",("start_pos: %lu  end_pos: %lu", (ulong) start_pos,
		     (ulong) end_pos));
  if (start_pos == HA_POS_ERROR || end_pos == HA_POS_ERROR)
    DBUG_RETURN(HA_POS_ERROR);
  DBUG_RETURN(end_pos < start_pos ? (ha_rows) 0 :
	      (end_pos == start_pos ? (ha_rows) 1 : end_pos - start_pos));
}


	/* Search after a record based on a key */
	/* Sets info->current_ptr to found record */
	/* next_flag:  Search=0, next=1, prev =2, same =3 */

uchar *hp_search(HP_INFO *info, HP_KEYDEF *keyinfo, const uchar *key,
                uint nextflag)
{
  reg1 HASH_INFO *pos,*prev_ptr;
  uint old_nextflag;
  HP_SHARE *share=info->s;
  DBUG_ENTER("hp_search");
  old_nextflag=nextflag;
  prev_ptr=0;

  if (share->records)
  {
    ulong search_pos=
      hp_mask(hp_hashnr(keyinfo, key), share->blength, share->records);
    pos=hp_find_hash(&keyinfo->block, search_pos);
    if (search_pos !=
        hp_mask(pos->hash_of_key, share->blength, share->records))
      goto not_found;                           /* Wrong link */
    do
    {
      if (!hp_key_cmp(keyinfo, pos->ptr_to_rec, key))
      {
	switch (nextflag) {
	case 0:					/* Search after key */
	  DBUG_PRINT("exit", ("found key at %p", pos->ptr_to_rec));
	  info->current_hash_ptr=pos;
	  DBUG_RETURN(info->current_ptr= pos->ptr_to_rec);
	case 1:					/* Search next */
	  if (pos->ptr_to_rec == info->current_ptr)
	    nextflag=0;
	  break;
	case 2:					/* Search previous */
	  if (pos->ptr_to_rec == info->current_ptr)
	  {
	    my_errno=HA_ERR_KEY_NOT_FOUND;	/* If gpos == 0 */
	    info->current_hash_ptr=prev_ptr;
	    DBUG_RETURN(info->current_ptr=prev_ptr ? prev_ptr->ptr_to_rec : 0);
	  }
	  prev_ptr=pos;				/* Prev. record found */
	  break;
	case 3:					/* Search same */
	  if (pos->ptr_to_rec == info->current_ptr)
	  {
	    info->current_hash_ptr=pos;
	    DBUG_RETURN(info->current_ptr);
	  }
	}
      }
    }
    while ((pos=pos->next_key));
  }

not_found:
  my_errno=HA_ERR_KEY_NOT_FOUND;
  if (nextflag == 2 && ! info->current_ptr)
  {
    /* Do a previous from end */
    info->current_hash_ptr=prev_ptr;
    DBUG_RETURN(info->current_ptr=prev_ptr ? prev_ptr->ptr_to_rec : 0);
  }

  if (old_nextflag && nextflag)
    my_errno=HA_ERR_RECORD_CHANGED;		/* Didn't find old record */
  DBUG_PRINT("exit",("Error: %d",my_errno));
  info->current_hash_ptr=0;  
  DBUG_RETURN((info->current_ptr= 0));
}


/*
  Search next after last read;  Assumes that the table hasn't changed
  since last read !
*/

uchar *hp_search_next(HP_INFO *info, HP_KEYDEF *keyinfo, const uchar *key,
		      HASH_INFO *pos)
{
  DBUG_ENTER("hp_search_next");

  while ((pos= pos->next_key))
  {
    if (! hp_key_cmp(keyinfo, pos->ptr_to_rec, key))
    {
      info->current_hash_ptr=pos;
      DBUG_RETURN (info->current_ptr= pos->ptr_to_rec);
    }
  }
  my_errno=HA_ERR_KEY_NOT_FOUND;
  DBUG_PRINT("exit",("Error: %d",my_errno));
  info->current_hash_ptr=0;
  DBUG_RETURN ((info->current_ptr= 0));
}


/*
  Change
    next_link -> ... -> X -> pos
  to
    next_link -> ... -> X -> newlink
*/

void hp_movelink(HASH_INFO *pos, HASH_INFO *next_link, HASH_INFO *newlink)
{
  HASH_INFO *old_link;
  do
  {
    old_link=next_link;
  }
  while ((next_link=next_link->next_key) != pos);
  old_link->next_key=newlink;
  return;
}

	/* Calc hashvalue for a key */

static ulong hp_hashnr(HP_KEYDEF *keydef, const uchar *key)
{
  /*register*/ 
  ulong nr=1, nr2=4;
  HA_KEYSEG *seg,*endseg;

  for (seg=keydef->seg,endseg=seg+keydef->keysegs ; seg < endseg ; seg++)
  {
    uchar *pos=(uchar*) key;
    key+=seg->length;
    if (seg->null_bit)
    {
      key++;					/* Skip null byte */
      if (*pos)					/* Found null */
      {
	nr^= (nr << 1) | 1;
	/* Add key pack length (2) to key for VARCHAR segments */
        if (seg->type == HA_KEYTYPE_VARTEXT1)
          key+= 2;
	continue;
      }
      pos++;
    }
    if (seg->type == HA_KEYTYPE_TEXT)
    {
       CHARSET_INFO *cs= seg->charset;
       size_t length= seg->length;
       if (cs->mbmaxlen > 1)
       {
         size_t char_length;
         char_length= hp_charpos(cs, pos, pos + length, length/cs->mbmaxlen);
         set_if_smaller(length, char_length);
       }
       my_ci_hash_sort(cs, pos, length, &nr, &nr2);
    }
    else if (seg->type == HA_KEYTYPE_VARTEXT1)  /* Any VARCHAR segments */
    {
       CHARSET_INFO *cs= seg->charset;
       size_t pack_length= 2;                     /* Key packing is constant */
       size_t length= uint2korr(pos);
       if (cs->mbmaxlen > 1)
       {
         size_t char_length;
         char_length= hp_charpos(cs, pos +pack_length,
                                 pos +pack_length + length,
                                 seg->length/cs->mbmaxlen);
         set_if_smaller(length, char_length);
       }
       my_ci_hash_sort(cs, pos+pack_length, length, &nr, &nr2);
       key+= pack_length;
    }
    else
    {
      for (; pos < (uchar*) key ; pos++)
      {
	nr^=(ulong) ((((uint) nr & 63)+nr2)*((uint) *pos)) + (nr << 8);
	nr2+=3;
      }
    }
  }
#ifdef ONLY_FOR_HASH_DEBUGGING
  DBUG_PRINT("exit", ("hash: 0x%lx", nr));
#endif
  return((ulong) nr);
}

	/* Calc hashvalue for a key in a record */

ulong hp_rec_hashnr(register HP_KEYDEF *keydef, register const uchar *rec)
{
  ulong nr=1, nr2=4;
  HA_KEYSEG *seg,*endseg;

  for (seg=keydef->seg,endseg=seg+keydef->keysegs ; seg < endseg ; seg++)
  {
    uchar *pos=(uchar*) rec+seg->start,*end=pos+seg->length;
    if (seg->null_bit)
    {
      if (rec[seg->null_pos] & seg->null_bit)
      {
	nr^= (nr << 1) | 1;
	continue;
      }
    }
    if (seg->type == HA_KEYTYPE_TEXT)
    {
      CHARSET_INFO *cs= seg->charset;
      size_t char_length= seg->length;
      if (cs->mbmaxlen > 1)
      {
        char_length= hp_charpos(cs, pos, pos + char_length,
                                char_length / cs->mbmaxlen);
        set_if_smaller(char_length, seg->length); /* QQ: ok to remove? */
      }
      my_ci_hash_sort(cs, pos, char_length, &nr, &nr2);
    }
    else if (seg->type == HA_KEYTYPE_VARTEXT1)  /* Any VARCHAR segments */
    {
      CHARSET_INFO *cs= seg->charset;
      size_t pack_length= seg->bit_start;
      size_t length= (pack_length == 1 ? (size_t) *(uchar*) pos : uint2korr(pos));
      if (cs->mbmaxlen > 1)
      {
        size_t char_length;
        char_length= hp_charpos(cs, pos + pack_length,
                                pos + pack_length + length,
                                seg->length/cs->mbmaxlen);
        set_if_smaller(length, char_length);
      }
      else
        set_if_smaller(length, seg->length);
      my_ci_hash_sort(cs, pos+pack_length, length, &nr, &nr2);
    }
    else
    {
      if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
      {
        uchar bits= get_rec_bits(rec + seg->bit_pos,
                                 seg->bit_start, seg->bit_length);
	nr^=(ulong) ((((uint) nr & 63)+nr2)*((uint) bits))+ (nr << 8);
	nr2+=3;
        end--;
      }

      for (; pos < end ; pos++)
      {
	nr^=(ulong) ((((uint) nr & 63)+nr2)*((uint) *pos))+ (nr << 8);
	nr2+=3;
      }
    }
  }
#ifdef ONLY_FOR_HASH_DEBUGGING
  DBUG_PRINT("exit", ("hash: 0x%lx", nr));
#endif
  return(nr);
}


/*
  Compare keys for two records. Returns 0 if they are identical

  SYNOPSIS
    hp_rec_key_cmp()
    keydef		Key definition
    rec1		Record to compare
    rec2		Other record to compare

  NOTES
    diff_if_only_endspace_difference is used to allow us to insert
    'a' and 'a ' when there is an an unique key.

  RETURN
    0		Key is identical
    <> 0 	Key differes
*/

int hp_rec_key_cmp(HP_KEYDEF *keydef, const uchar *rec1, const uchar *rec2)
{
  HA_KEYSEG *seg,*endseg;

  for (seg=keydef->seg,endseg=seg+keydef->keysegs ; seg < endseg ; seg++)
  {
    if (seg->null_bit)
    {
      if ((rec1[seg->null_pos] & seg->null_bit) !=
	  (rec2[seg->null_pos] & seg->null_bit))
	return 1;
      if (rec1[seg->null_pos] & seg->null_bit)
	continue;
    }
    if (seg->type == HA_KEYTYPE_TEXT)
    {
      CHARSET_INFO *cs= seg->charset;
      size_t char_length1;
      size_t char_length2;
      uchar *pos1= (uchar*)rec1 + seg->start;
      uchar *pos2= (uchar*)rec2 + seg->start;
      if (cs->mbmaxlen > 1)
      {
        size_t char_length= seg->length / cs->mbmaxlen;
        char_length1= hp_charpos(cs, pos1, pos1 + seg->length, char_length);
        set_if_smaller(char_length1, seg->length);
        char_length2= hp_charpos(cs, pos2, pos2 + seg->length, char_length);
        set_if_smaller(char_length2, seg->length);
      }
      else
      {
        char_length1= char_length2= seg->length;
      }
      if (my_ci_strnncollsp(seg->charset,
                            pos1, char_length1,
                            pos2, char_length2))
	return 1;
    }
    else if (seg->type == HA_KEYTYPE_VARTEXT1)  /* Any VARCHAR segments */
    {
      uchar *pos1= (uchar*) rec1 + seg->start;
      uchar *pos2= (uchar*) rec2 + seg->start;
      size_t char_length1, char_length2;
      size_t pack_length= seg->bit_start;
      CHARSET_INFO *cs= seg->charset;
      if (pack_length == 1)
      {
        char_length1= (size_t) *(uchar*) pos1++;
        char_length2= (size_t) *(uchar*) pos2++;
      }
      else
      {
        char_length1= uint2korr(pos1);
        char_length2= uint2korr(pos2);
        pos1+= 2;
        pos2+= 2;
      }
      if (cs->mbmaxlen > 1)
      {
        size_t safe_length1= char_length1;
        size_t safe_length2= char_length2;
        size_t char_length= seg->length / cs->mbmaxlen;
        char_length1= hp_charpos(cs, pos1, pos1 + char_length1, char_length);
        set_if_smaller(char_length1, safe_length1);
        char_length2= hp_charpos(cs, pos2, pos2 + char_length2, char_length);
        set_if_smaller(char_length2, safe_length2);
      }
      else
      {
        set_if_smaller(char_length1, seg->length);
        set_if_smaller(char_length2, seg->length);
      }

      if (my_ci_strnncollsp(seg->charset,
                            pos1, char_length1,
                            pos2, char_length2))
	return 1;
    }
    else
    {
      uint dec= 0;
      if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
      {
        uchar bits1= get_rec_bits(rec1 + seg->bit_pos,
                                  seg->bit_start, seg->bit_length);
        uchar bits2= get_rec_bits(rec2 + seg->bit_pos,
                                  seg->bit_start, seg->bit_length);
        if (bits1 != bits2)
          return 1;
        dec= 1;
      }
      if (bcmp(rec1 + seg->start, rec2 + seg->start, seg->length - dec))
	return 1;
    }
  }
  return 0;
}

	/* Compare a key in a record to a whole key */

int hp_key_cmp(HP_KEYDEF *keydef, const uchar *rec, const uchar *key)
{
  HA_KEYSEG *seg,*endseg;

  for (seg=keydef->seg,endseg=seg+keydef->keysegs ;
       seg < endseg ;
       key+= (seg++)->length)
  {
    if (seg->null_bit)
    {
      int found_null= MY_TEST(rec[seg->null_pos] & seg->null_bit);
      if (found_null != (int) *key++)
	return 1;
      if (found_null)
      {
        /* Add key pack length (2) to key for VARCHAR segments */
        if (seg->type == HA_KEYTYPE_VARTEXT1)
          key+= 2;
	continue;
      }
    }
    if (seg->type == HA_KEYTYPE_TEXT)
    {
      CHARSET_INFO *cs= seg->charset;
      size_t char_length_key;
      size_t char_length_rec;
      uchar *pos= (uchar*) rec + seg->start;
      if (cs->mbmaxlen > 1)
      {
        size_t char_length= seg->length / cs->mbmaxlen;
        char_length_key= hp_charpos(cs, key, key + seg->length, char_length);
        set_if_smaller(char_length_key, seg->length);
        char_length_rec= hp_charpos(cs, pos, pos + seg->length, char_length);
        set_if_smaller(char_length_rec, seg->length);
      }
      else
      {
        char_length_key= seg->length;
        char_length_rec= seg->length;
      }
      
      if (my_ci_strnncollsp(seg->charset,
                            pos, char_length_rec,
                            key, char_length_key))
	return 1;
    }
    else if (seg->type == HA_KEYTYPE_VARTEXT1)  /* Any VARCHAR segments */
    {
      uchar *pos= (uchar*) rec + seg->start;
      CHARSET_INFO *cs= seg->charset;
      size_t pack_length= seg->bit_start;
      size_t char_length_rec= (pack_length == 1 ? (size_t) *(uchar*) pos :
                             uint2korr(pos));
      /* Key segments are always packed with 2 bytes */
      size_t char_length_key= uint2korr(key);
      pos+= pack_length;
      key+= 2;                                  /* skip key pack length */
      if (cs->mbmaxlen > 1)
      {
        size_t char_length1, char_length2;
        char_length1= char_length2= seg->length / cs->mbmaxlen; 
        char_length1= hp_charpos(cs, key, key + char_length_key, char_length1);
        set_if_smaller(char_length_key, char_length1);
        char_length2= hp_charpos(cs, pos, pos + char_length_rec, char_length2);
        set_if_smaller(char_length_rec, char_length2);
      }
      else
        set_if_smaller(char_length_rec, seg->length);

      if (my_ci_strnncollsp(seg->charset,
                            pos, char_length_rec,
                            key, char_length_key))
	return 1;
    }
    else
    {
      uint dec= 0;
      if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
      {
        uchar bits= get_rec_bits(rec + seg->bit_pos,
                                 seg->bit_start, seg->bit_length);
        if (bits != (*key))
          return 1;
        dec= 1;
        key++;
      }

      if (bcmp(rec + seg->start, key, seg->length - dec))
	return 1;
    }
  }
  return 0;
}


	/* Copy a key from a record to a keybuffer */

void hp_make_key(HP_KEYDEF *keydef, uchar *key, const uchar *rec)
{
  HA_KEYSEG *seg,*endseg;

  for (seg=keydef->seg,endseg=seg+keydef->keysegs ; seg < endseg ; seg++)
  {
    CHARSET_INFO *cs= seg->charset;
    size_t char_length= seg->length;
    uchar *pos= (uchar*) rec + seg->start;
    if (seg->null_bit)
      *key++= MY_TEST(rec[seg->null_pos] & seg->null_bit);
    if (cs->mbmaxlen > 1)
    {
      char_length= hp_charpos(cs, pos, pos + seg->length,
                              char_length / cs->mbmaxlen);
      set_if_smaller(char_length, seg->length); /* QQ: ok to remove? */
    }
    if (seg->type == HA_KEYTYPE_VARTEXT1)
      char_length+= seg->bit_start;             /* Copy also length */
    else if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
    {
      *key++= get_rec_bits(rec + seg->bit_pos,
                           seg->bit_start, seg->bit_length);
      char_length--;
    }
    memcpy(key,rec+seg->start,(size_t) char_length);
    key+= char_length;
  }
}

#define FIX_LENGTH(cs, pos, length, char_length)                        \
  do {                                                                  \
    if (length > char_length)                                           \
      char_length= hp_charpos(cs, pos, pos+length, char_length);        \
    set_if_smaller(char_length,length);                                 \
  } while(0)


uint hp_rb_make_key(HP_KEYDEF *keydef, uchar *key, 
		    const uchar *rec, uchar *recpos)
{
  uchar *start_key= key;
  HA_KEYSEG *seg, *endseg;

  for (seg= keydef->seg, endseg= seg + keydef->keysegs; seg < endseg; seg++)
  {
    size_t char_length;
    if (seg->null_bit)
    {
      if (!(*key++= 1 - MY_TEST(rec[seg->null_pos] & seg->null_bit)))
        continue;
    }
    if (seg->flag & HA_SWAP_KEY)
    {
      uint length= seg->length;
      uchar *pos= (uchar*) rec + seg->start;
      DBUG_ASSERT(seg->type != HA_KEYTYPE_BIT);

      if (seg->type == HA_KEYTYPE_FLOAT)
      {
	float nr;
	float4get(nr, pos);
	if (isnan(nr))
	{
	  /* Replace NAN with zero */
 	  bzero(key, length);
	  key+= length;
	  continue;
	}
      }
      else if (seg->type == HA_KEYTYPE_DOUBLE)
      {
	double nr;
	float8get(nr, pos);
	if (isnan(nr))
	{
 	  bzero(key, length);
	  key+= length;
	  continue;
	}
      }
      pos+= length;
      while (length--)
      {
	*key++= *--pos;
      }
      continue;
    }

    if (seg->flag & HA_VAR_LENGTH_PART)
    {
      uchar *pos=      (uchar*) rec + seg->start;
      size_t length=     seg->length;
      size_t pack_length= seg->bit_start;
      size_t tmp_length= (pack_length == 1 ? (uint) *(uchar*) pos :
                        uint2korr(pos));
      CHARSET_INFO *cs= seg->charset;
      char_length= length/cs->mbmaxlen;

      pos+= pack_length;			/* Skip VARCHAR length */
      set_if_smaller(length,tmp_length);
      FIX_LENGTH(cs, pos, length, char_length);
      store_key_length_inc(key,char_length);
      memcpy((uchar*) key,(uchar*) pos,(size_t) char_length);
      key+= char_length;
      continue;
    }

    char_length= seg->length;
    if (seg->charset->mbmaxlen > 1)
    {
      char_length= hp_charpos(seg->charset, 
                              rec + seg->start, rec + seg->start + char_length,
                              char_length / seg->charset->mbmaxlen);
      set_if_smaller(char_length, seg->length); /* QQ: ok to remove? */
      if (char_length < seg->length)
        my_ci_fill(seg->charset, (char*) key + char_length,
                                 seg->length - char_length, ' ');
    }
    if (seg->type == HA_KEYTYPE_BIT && seg->bit_length)
    {
      *key++= get_rec_bits(rec + seg->bit_pos,
                           seg->bit_start, seg->bit_length);
      char_length--;
    }
    memcpy(key, rec + seg->start, (size_t) char_length);
    key+= seg->length;
  }
  memcpy(key, &recpos, sizeof(uchar*));
  return (uint) (key - start_key);
}


uint hp_rb_pack_key(HP_KEYDEF *keydef, uchar *key, const uchar *old,
                    key_part_map keypart_map)
{
  HA_KEYSEG *seg, *endseg;
  uchar *start_key= key;

  for (seg= keydef->seg, endseg= seg + keydef->keysegs;
       seg < endseg && keypart_map; old+= seg->length, seg++)
  {
    size_t char_length;
    keypart_map>>= 1;
    if (seg->null_bit)
    {
      /* Convert NULL from MySQL representation into HEAP's. */
      if (!(*key++= (char) 1 - *old++))
      {
	/* Add key pack length (2) to key for VARCHAR segments */
        if (seg->type == HA_KEYTYPE_VARTEXT1)
          old+= 2;
        continue;
      }
    }
    if (seg->flag & HA_SWAP_KEY)
    {
      uint length= seg->length;
      uchar *pos= (uchar*) old + length;
      
      while (length--)
      {
	*key++= *--pos;
      }
      continue;
    }
    if (seg->flag & (HA_VAR_LENGTH_PART | HA_BLOB_PART))
    {
      /* Length of key-part used with heap_rkey() always 2 */
      size_t tmp_length=uint2korr(old);
      size_t length= seg->length;
      CHARSET_INFO *cs= seg->charset;
      char_length= length/cs->mbmaxlen;

      old+= 2;
      set_if_smaller(length,tmp_length);	/* Safety */
      FIX_LENGTH(cs, old, length, char_length);
      store_key_length_inc(key,char_length);
      memcpy((uchar*) key, old,(size_t) char_length);
      key+= char_length;
      continue;
    }
    char_length= seg->length;
    if (seg->charset->mbmaxlen > 1)
    {
      char_length= hp_charpos(seg->charset, old, old+char_length,
                              char_length / seg->charset->mbmaxlen);
      set_if_smaller(char_length, seg->length); /* QQ: ok to remove? */
      if (char_length < seg->length)
        my_ci_fill(seg->charset, (char*) key + char_length,
                                 seg->length - char_length, ' ');
    }
    memcpy(key, old, (size_t) char_length);
    key+= seg->length;
  }
  return (uint) (key - start_key);
}


uint hp_rb_key_length(HP_KEYDEF *keydef, 
		      const uchar *key __attribute__((unused)))
{
  return keydef->length;
}


uint hp_rb_null_key_length(HP_KEYDEF *keydef, const uchar *key)
{
  const uchar *start_key= key;
  HA_KEYSEG *seg, *endseg;
  
  for (seg= keydef->seg, endseg= seg + keydef->keysegs; seg < endseg; seg++)
  {
    if (seg->null_bit && !*key++)
      continue;
    key+= seg->length;
  }
  return (uint) (key - start_key);
}
                  

uint hp_rb_var_key_length(HP_KEYDEF *keydef, const uchar *key)
{
  const uchar *start_key= key;
  HA_KEYSEG *seg, *endseg;
  
  for (seg= keydef->seg, endseg= seg + keydef->keysegs; seg < endseg; seg++)
  {
    uint length= seg->length;
    if (seg->null_bit && !*key++)
      continue;
    if (seg->flag & (HA_VAR_LENGTH_PART | HA_BLOB_PART))
    {
      get_key_length(length, key);
    }
    key+= length;
  }
  return (uint) (key - start_key);
}


/*
  Test if any of the key parts are NULL.
  Return:
    1 if any of the key parts was NULL
    0 otherwise
*/

my_bool hp_if_null_in_key(HP_KEYDEF *keydef, const uchar *record)
{
  HA_KEYSEG *seg,*endseg;
  for (seg=keydef->seg,endseg=seg+keydef->keysegs ; seg < endseg ; seg++)
  {
    if (seg->null_bit && (record[seg->null_pos] & seg->null_bit))
      return 1;
  }
  return 0;
}


/*
  Update auto_increment info

  SYNOPSIS
    update_auto_increment()
    info			MyISAM handler
    record			Row to update

  IMPLEMENTATION
    Only replace the auto_increment value if it is higher than the previous
    one. For signed columns we don't update the auto increment value if it's
    less than zero.
*/

void heap_update_auto_increment(HP_INFO *info, const uchar *record)
{
  ulonglong value= 0;			/* Store unsigned values here */
  longlong s_value= 0;			/* Store signed values here */

  HA_KEYSEG *keyseg= info->s->keydef[info->s->auto_key - 1].seg;
  const uchar *key=  (uchar*) record + keyseg->start;

  switch (info->s->auto_key_type) {
  case HA_KEYTYPE_INT8:
    s_value= (longlong) *(const signed char*) key;
    break;
  case HA_KEYTYPE_BINARY:
    value=(ulonglong)  *(uchar*) key;
    break;
  case HA_KEYTYPE_SHORT_INT:
    s_value= (longlong) sint2korr(key);
    break;
  case HA_KEYTYPE_USHORT_INT:
    value=(ulonglong) uint2korr(key);
    break;
  case HA_KEYTYPE_LONG_INT:
    s_value= (longlong) sint4korr(key);
    break;
  case HA_KEYTYPE_ULONG_INT:
    value=(ulonglong) uint4korr(key);
    break;
  case HA_KEYTYPE_INT24:
    s_value= (longlong) sint3korr(key);
    break;
  case HA_KEYTYPE_UINT24:
    value=(ulonglong) uint3korr(key);
    break;
  case HA_KEYTYPE_FLOAT:                        /* This shouldn't be used */
  {
    float f_1;
    float4get(f_1,key);
    /* Ignore negative values */
    value = (f_1 < (float) 0.0) ? 0 : (ulonglong) f_1;
    break;
  }
  case HA_KEYTYPE_DOUBLE:                       /* This shouldn't be used */
  {
    double f_1;
    float8get(f_1,key);
    /* Ignore negative values */
    value = (f_1 < 0.0) ? 0 : (ulonglong) f_1;
    break;
  }
  case HA_KEYTYPE_LONGLONG:
    s_value= sint8korr(key);
    break;
  case HA_KEYTYPE_ULONGLONG:
    value= uint8korr(key);
    break;
  default:
    DBUG_ASSERT(0);
    value=0;                                    /* Error */
    break;
  }

  /*
    The following code works becasue if s_value < 0 then value is 0
    and if s_value == 0 then value will contain either s_value or the
    correct value.
  */
  set_if_bigger(info->s->auto_increment,
                (s_value > 0) ? (ulonglong) s_value : value);
}
