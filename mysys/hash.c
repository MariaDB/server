/* Copyright (c) 2000, 2010, Oracle and/or its affiliates.
   Copyright (c) 2011, 2020, MariaDB Corporation.

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
/* One of key_length or key_length_offset must be given */
/* Key length of 0 isn't allowed */

#include "mysys_priv.h"
#include <m_string.h>
#include <m_ctype.h>
#include "hash.h"

#define NO_RECORD	~((my_hash_value_type) 0)
#define LOWFIND 1
#define LOWUSED 2
#define HIGHFIND 4
#define HIGHUSED 8

typedef struct st_hash_info {
  uint32 next;					/* index to next key */
  my_hash_value_type hash_nr;
  uchar *data;					/* data for current entry */
} HASH_LINK;

static uint my_hash_mask(my_hash_value_type hashnr,
                         size_t buffmax, size_t maxlength);
static void movelink(HASH_LINK *array,uint pos,uint next_link,uint newlink);
static int hashcmp(const HASH *hash, HASH_LINK *pos, const uchar *key,
                   size_t length);

my_hash_value_type my_hash_sort(CHARSET_INFO *cs, const uchar *key,
                                size_t length)
{
  ulong nr1= 1, nr2= 4;
  my_ci_hash_sort(cs, (uchar*) key, length, &nr1, &nr2);
  return (my_hash_value_type) nr1;
}

/**
  @brief Initialize the hash
  
  @details

  Initialize the hash, by defining and giving valid values for
  its elements. The failure to allocate memory for the
  hash->array element will not result in a fatal failure. The
  dynamic array that is part of the hash will allocate memory
  as required during insertion.

  @param[in,out] hash         The hash that is initialized
  @param[in[     growth_size  size incrememnt for the underlying dynarray
  @param[in]     charset      The character set information
  @param[in]     size         The hash size
  @param[in]     key_offest   The key offset for the hash
  @param[in]     key_length   The length of the key used in
                              the hash
  @param[in]     get_key      get the key for the hash
  @param[in]     free_element pointer to the function that
                              does cleanup
  @param[in]     flags        flags set in the hash
  @return        indicates success or failure of initialization
    @retval 0 success
    @retval 1 failure
*/
my_bool
my_hash_init2(PSI_memory_key psi_key, HASH *hash, uint growth_size,
              CHARSET_INFO *charset, ulong size, size_t key_offset,
              size_t key_length, my_hash_get_key get_key,
              my_hash_function hash_function,
              void (*free_element)(void*), uint flags)
{
  my_bool res;
  DBUG_ENTER("my_hash_init2");
  DBUG_PRINT("enter",("hash:%p  size: %u", hash, (uint) size));

  hash->records=0;
  hash->key_offset=key_offset;
  hash->key_length=key_length;
  hash->blength=1;
  hash->get_key=get_key;
  hash->hash_function= hash_function ? hash_function : my_hash_sort;
  hash->free=free_element;
  hash->flags=flags;
  hash->charset=charset;
  res= init_dynamic_array2(psi_key, &hash->array, sizeof(HASH_LINK), NULL, size,
                           growth_size, MYF((flags & HASH_THREAD_SPECIFIC ?
                                             MY_THREAD_SPECIFIC : 0)));
  DBUG_RETURN(res);
}


/*
  Call hash->free on all elements in hash.

  SYNOPSIS
    my_hash_free_elements()
    hash   hash table

  NOTES:
    Sets records to 0
*/

static inline void my_hash_free_elements(HASH *hash)
{
  uint records= hash->records;
  if (records == 0)
    return;

  /*
    Set records to 0 early to guard against anyone looking at the structure
    during the free process
  */
  hash->records= 0;

  if (hash->free)
  {
    HASH_LINK *data=dynamic_element(&hash->array,0,HASH_LINK*);
    HASH_LINK *end= data + records;
    do
    {
      (*hash->free)((data++)->data);
    } while (data < end);
  }
}


/*
  Free memory used by hash.

  SYNOPSIS
    my_hash_free()
    hash   the hash to delete elements of

  NOTES: Hash can't be reused without calling my_hash_init again.
*/

void my_hash_free(HASH *hash)
{
  DBUG_ENTER("my_hash_free");
  DBUG_PRINT("enter",("hash:%p  elements: %ld",
                      hash, hash->records));

  my_hash_free_elements(hash);
  hash->free= 0;
  delete_dynamic(&hash->array);
  hash->blength= 0;
  DBUG_VOID_RETURN;
}


/*
  Delete all elements from the hash (the hash itself is to be reused).

  SYNOPSIS
    my_hash_reset()
    hash   the hash to delete elements of
*/

void my_hash_reset(HASH *hash)
{
  DBUG_ENTER("my_hash_reset");
  DBUG_PRINT("enter",("hash:%p", hash));

  my_hash_free_elements(hash);
  reset_dynamic(&hash->array);
  /* Set row pointers so that the hash can be reused at once */
  hash->blength= 1;
  DBUG_VOID_RETURN;
}

/* some helper functions */

/*
  This function is char* instead of uchar* as HPUX11 compiler can't
  handle inline functions that are not defined as native types
*/

static inline char*
my_hash_key(const HASH *hash, const uchar *record, size_t *length,
            my_bool first)
{
  if (hash->get_key)
    return (char*) (*hash->get_key)(record,length,first);
  *length=hash->key_length;
  return (char*) record+hash->key_offset;
}

	/* Calculate pos according to keys */

static uint my_hash_mask(my_hash_value_type hashnr, size_t buffmax,
                         size_t maxlength)
{
  if ((hashnr & (buffmax-1)) < maxlength)
    return (uint) (hashnr & (buffmax-1));
  return (uint) (hashnr & ((buffmax >> 1) -1));
}

static inline uint my_hash_rec_mask(HASH_LINK *pos,
                                    size_t buffmax, size_t maxlength)
{
  return my_hash_mask(pos->hash_nr, buffmax, maxlength);
}



/* for compilers which can not handle inline */
static
#if !defined(__USLC__) && !defined(__sgi)
inline
#endif
my_hash_value_type rec_hashnr(HASH *hash,const uchar *record)
{
  size_t length;
  uchar *key= (uchar*) my_hash_key(hash, record, &length, 0);
  return hash->hash_function(hash->charset, key, length);
}


uchar* my_hash_search(const HASH *hash, const uchar *key, size_t length)
{
  HASH_SEARCH_STATE state;
  return my_hash_first(hash, key, length, &state);
}

uchar* my_hash_search_using_hash_value(const HASH *hash, 
                                       my_hash_value_type hash_value,
                                       const uchar *key,
                                       size_t length)
{
  HASH_SEARCH_STATE state;
  return my_hash_first_from_hash_value(hash, hash_value,
                                       key, length, &state);
}


/*
  Search after a record based on a key

  NOTE
   Assigns the number of the found record to HASH_SEARCH_STATE state
*/

uchar* my_hash_first(const HASH *hash, const uchar *key, size_t length,
                     HASH_SEARCH_STATE *current_record)
{
  uchar *res;
  DBUG_ASSERT(my_hash_inited(hash));

  res= my_hash_first_from_hash_value(hash,
                                     hash->hash_function(hash->charset, key,
                                                         length ? length :
                                                         hash->key_length),
                                     key, length, current_record);
  return res;
}


uchar* my_hash_first_from_hash_value(const HASH *hash,
                                     my_hash_value_type hash_value,
                                     const uchar *key,
                                     size_t length,
                                     HASH_SEARCH_STATE *current_record)
{
  HASH_LINK *pos;
  DBUG_ENTER("my_hash_first_from_hash_value");

  if (hash->records)
  {
    uint flag= 1;
    uint idx= my_hash_mask(hash_value,
                           hash->blength, hash->records);
    do
    {
      pos= dynamic_element(&hash->array,idx,HASH_LINK*);
      if (!hashcmp(hash,pos,key,length))
      {
	DBUG_PRINT("exit",("found key at %d",idx));
	*current_record= idx;
	DBUG_RETURN (pos->data);
      }
      if (flag)
      {
	flag=0;					/* Reset flag */
	if (my_hash_rec_mask(pos, hash->blength, hash->records) != idx)
	  break;				/* Wrong link */
      }
    }
    while ((idx=pos->next) != NO_RECORD);
  }
  *current_record= NO_RECORD;
  DBUG_RETURN(0);
}

	/* Get next record with identical key */
	/* Can only be called if previous calls was my_hash_search */

uchar* my_hash_next(const HASH *hash, const uchar *key, size_t length,
                    HASH_SEARCH_STATE *current_record)
{
  HASH_LINK *pos;
  uint idx;

  if (*current_record != NO_RECORD)
  {
    HASH_LINK *data=dynamic_element(&hash->array,0,HASH_LINK*);
    for (idx=data[*current_record].next; idx != NO_RECORD ; idx=pos->next)
    {
      pos=data+idx;
      if (!hashcmp(hash,pos,key,length))
      {
	*current_record= idx;
	return pos->data;
      }
    }
    *current_record= NO_RECORD;
  }
  return 0;
}


	/* Change link from pos to new_link */

static void movelink(HASH_LINK *array,uint find,uint next_link,uint newlink)
{
  HASH_LINK *old_link;
  do
  {
    old_link=array+next_link;
  }
  while ((next_link=old_link->next) != find);
  old_link->next= newlink;
  return;
}

/*
  Compare a key in a record to a whole key. Return 0 if identical

  SYNOPSIS
    hashcmp()
    hash   hash table
    pos    position of hash record to use in comparison
    key    key for comparison
    length length of key

  NOTES:
    If length is 0, comparison is done using the length of the
    record being compared against.

  RETURN
    = 0  key of record == key
    != 0 key of record != key
 */

static int hashcmp(const HASH *hash, HASH_LINK *pos, const uchar *key,
                   size_t length)
{
  size_t rec_keylength;
  uchar *rec_key= (uchar*) my_hash_key(hash, pos->data, &rec_keylength, 1);
  return ((length && length != rec_keylength) ||
	  my_strnncoll(hash->charset, (uchar*) rec_key, rec_keylength,
		       (uchar*) key, rec_keylength));
}


/**
   Write a hash-key to the hash-index

   @return
   @retval  0  ok
   @retval  1  Duplicate key or out of memory
*/

my_bool my_hash_insert(HASH *info, const uchar *record)
{
  int flag;
  size_t idx, halfbuff, first_index;
  size_t length;
  my_hash_value_type current_hash_nr, UNINIT_VAR(rec_hash_nr),
    UNINIT_VAR(rec2_hash_nr);
  uchar *UNINIT_VAR(rec_data),*UNINIT_VAR(rec2_data), *key;
  HASH_LINK *data,*empty,*UNINIT_VAR(gpos),*UNINIT_VAR(gpos2),*pos;

  key= (uchar*) my_hash_key(info, record, &length, 1);
  current_hash_nr= info->hash_function(info->charset, key, length);

  if (info->flags & HASH_UNIQUE)
  {
    if (my_hash_search_using_hash_value(info, current_hash_nr, key, length))
      return(TRUE);				/* Duplicate entry */
  }

  flag=0;
  if (!(empty=(HASH_LINK*) alloc_dynamic(&info->array)))
    return(TRUE);				/* No more memory */

  data=dynamic_element(&info->array,0,HASH_LINK*);
  halfbuff= info->blength >> 1;

  idx=first_index=info->records-halfbuff;
  if (idx != info->records)				/* If some records */
  {
    do
    {
      my_hash_value_type hash_nr;
      pos=data+idx;
      hash_nr= pos->hash_nr;
      if (flag == 0)				/* First loop; Check if ok */
	if (my_hash_mask(hash_nr, info->blength, info->records) != first_index)
	  break;
      if (!(hash_nr & halfbuff))
      {						/* Key will not move */
	if (!(flag & LOWFIND))
	{
	  if (flag & HIGHFIND)
	  {
	    flag= LOWFIND | HIGHFIND;
	    /* key shall be moved to the current empty position */
	    gpos= empty;
            rec_data=    pos->data;
            rec_hash_nr= pos->hash_nr;
	    empty=pos;				/* This place is now free */
	  }
	  else
	  {
	    flag= LOWFIND | LOWUSED;		/* key isn't changed */
	    gpos= pos;
            rec_data=    pos->data;
            rec_hash_nr= pos->hash_nr;
	  }
	}
	else
	{
	  if (!(flag & LOWUSED))
	  {
	    /* Change link of previous LOW-key */
	    gpos->data=    rec_data;
	    gpos->hash_nr= rec_hash_nr;
	    gpos->next=    (uint) (pos-data);
	    flag= (flag & HIGHFIND) | (LOWFIND | LOWUSED);
	  }
	  gpos= pos;
	  rec_data=    pos->data;
          rec_hash_nr= pos->hash_nr;
	}
      }
      else
      {						/* key will be moved */
	if (!(flag & HIGHFIND))
	{
	  flag= (flag & LOWFIND) | HIGHFIND;
	  /* key shall be moved to the last (empty) position */
	  gpos2= empty;
          empty= pos;
	  rec2_data=    pos->data;
          rec2_hash_nr= pos->hash_nr;
	}
	else
	{
	  if (!(flag & HIGHUSED))
	  {
	    /* Change link of previous hash-key and save */
	    gpos2->data=    rec2_data;
	    gpos2->hash_nr= rec2_hash_nr;
	    gpos2->next=    (uint) (pos-data);
	    flag= (flag & LOWFIND) | (HIGHFIND | HIGHUSED);
	  }
	  gpos2= pos;
	  rec2_data=    pos->data;
          rec2_hash_nr= pos->hash_nr;
	}
      }
    }
    while ((idx=pos->next) != NO_RECORD);

    if ((flag & (LOWFIND | LOWUSED)) == LOWFIND)
    {
      gpos->data=    rec_data;
      gpos->hash_nr= rec_hash_nr;
      gpos->next=    NO_RECORD;
    }
    if ((flag & (HIGHFIND | HIGHUSED)) == HIGHFIND)
    {
      gpos2->data=    rec2_data;
      gpos2->hash_nr= rec2_hash_nr;
      gpos2->next=    NO_RECORD;
    }
  }

  idx= my_hash_mask(current_hash_nr, info->blength, info->records + 1);
  pos= data+idx;
  /* Check if we are at the empty position */
  if (pos == empty)
  {
    pos->next=NO_RECORD;
  }
  else
  {
    /* Move conflicting record to empty position (last) */
    empty[0]= pos[0];
    /* Check if the moved record was in same hash-nr family */
    gpos= data + my_hash_rec_mask(pos, info->blength, info->records + 1);
    if (pos == gpos)
    {
      /* Point to moved record */
      pos->next= (uint32) (empty - data);
    }
    else
    {
      pos->next= NO_RECORD;
      movelink(data,(uint) (pos-data),(uint) (gpos-data),(uint) (empty-data));
    }
  }
  pos->data=    (uchar*) record;
  pos->hash_nr= current_hash_nr;
  if (++info->records == info->blength)
    info->blength+= info->blength;
  return(0);
}


/**
   Remove one record from hash-table.

   @fn    hash_delete()
   @param hash		Hash tree
   @param record	Row to be deleted

   @notes
   The record with the same record ptr is removed.
   If there is a free-function it's called if record was found.

   hash->free() is guarantee to be called only after the row has been
   deleted from the hash and the hash can be reused by other threads.

   @return
   @retval  0  ok
   @retval  1 Record not found
*/

my_bool my_hash_delete(HASH *hash, uchar *record)
{
  uint pos2,idx,empty_index;
  my_hash_value_type pos_hashnr, lastpos_hashnr;
  size_t blength;
  HASH_LINK *data,*lastpos,*gpos,*pos,*pos3,*empty;
  DBUG_ENTER("my_hash_delete");
  if (!hash->records)
    DBUG_RETURN(1);

  blength=hash->blength;
  data=dynamic_element(&hash->array,0,HASH_LINK*);
  /* Search after record with key */
  pos= data + my_hash_mask(rec_hashnr(hash, record), blength, hash->records);
  gpos = 0;

  while (pos->data != record)
  {
    gpos=pos;
    if (pos->next == NO_RECORD)
      DBUG_RETURN(1);			/* Key not found */
    pos=data+pos->next;
  }

  if ( --(hash->records) < hash->blength >> 1) hash->blength>>=1;
  lastpos=data+hash->records;

  /* Remove link to record */
  empty=pos; empty_index=(uint) (empty-data);
  if (gpos)
    gpos->next=pos->next;		/* unlink current ptr */
  else if (pos->next != NO_RECORD)
  {
    empty=data+(empty_index=pos->next);
    pos[0]= empty[0];
  }

  if (empty == lastpos)		/* last key at wrong pos or no next link */
    goto exit;

  /* Move the last key (lastpos) */
  lastpos_hashnr= lastpos->hash_nr;
  /* pos is where lastpos should be */
  pos= data + my_hash_mask(lastpos_hashnr, hash->blength, hash->records);
  if (pos == empty)			/* Move to empty position. */
  {
    empty[0]=lastpos[0];
    goto exit;
  }
  pos_hashnr= pos->hash_nr;
  /* pos3 is where the pos should be */
  pos3= data + my_hash_mask(pos_hashnr, hash->blength, hash->records);
  if (pos != pos3)
  {					/* pos is on wrong posit */
    empty[0]=pos[0];			/* Save it here */
    pos[0]=lastpos[0];			/* This should be here */
    movelink(data,(uint) (pos-data),(uint) (pos3-data),empty_index);
    goto exit;
  }
  pos2= my_hash_mask(lastpos_hashnr, blength, hash->records + 1);
  if (pos2 == my_hash_mask(pos_hashnr, blength, hash->records + 1))
  {					/* Identical key-positions */
    if (pos2 != hash->records)
    {
      empty[0]=lastpos[0];
      movelink(data,(uint) (lastpos-data),(uint) (pos-data),empty_index);
      goto exit;
    }
    idx= (uint) (pos-data);		/* Link pos->next after lastpos */
  }
  else idx= NO_RECORD;		/* Different positions merge */

  empty[0]=lastpos[0];
  movelink(data,idx,empty_index,pos->next);
  pos->next=empty_index;

exit:
  (void) pop_dynamic(&hash->array);
  if (hash->free)
    (*hash->free)((uchar*) record);
  DBUG_RETURN(0);
}


/**
   Update keys when record has changed.
   This is much more efficient than using a delete & insert.
*/

my_bool my_hash_update(HASH *hash, uchar *record, uchar *old_key,
                       size_t old_key_length)
{
  uint new_index, new_pos_index, org_index, records, idx;
  size_t length, empty, blength;
  my_hash_value_type hash_nr;
  HASH_LINK org_link,*data,*previous,*pos;
  uchar *new_key;
  DBUG_ENTER("my_hash_update");

  new_key= (uchar*) my_hash_key(hash, record, &length, 1);
  hash_nr= hash->hash_function(hash->charset, new_key, length);
  
  if (HASH_UNIQUE & hash->flags)
  {
    HASH_SEARCH_STATE state;
    uchar *found;

    if ((found= my_hash_first_from_hash_value(hash, hash_nr, new_key, length,
                                              &state)))
    {
      do 
      {
        if (found != record)
          DBUG_RETURN(1);		/* Duplicate entry */
      } 
      while ((found= my_hash_next(hash, new_key, length, &state)));
    }
  }

  data=dynamic_element(&hash->array,0,HASH_LINK*);
  blength=hash->blength; records=hash->records;

  /* Search after record with key */

  idx= my_hash_mask(hash->hash_function(hash->charset, old_key,
                                        (old_key_length ? old_key_length :
                                                          hash->key_length)),
                    blength, records);
  org_index= idx;
  new_index= my_hash_mask(hash_nr, blength, records);
  previous=0;
  for (;;)
  {
    if ((pos= data+idx)->data == record)
      break;
    previous=pos;
    if ((idx=pos->next) == NO_RECORD)
      DBUG_RETURN(1);			/* Not found in links */
  }

  if (org_index == new_index)
  {
    data[idx].hash_nr= hash_nr;         /* Hash number may have changed */
    DBUG_RETURN(0);			/* Record is in right position */
  }

  org_link= *pos;
  empty=idx;

  /* Relink record from current chain */

  if (!previous)
  {
    if (pos->next != NO_RECORD)
    {
      empty=pos->next;
      *pos= data[pos->next];
    }
  }
  else
    previous->next=pos->next;		/* unlink pos */

  /* Move data to correct position */
  if (new_index == empty)
  {
    /*
      At this point record is unlinked from the old chain, thus it holds
      random position. By the chance this position is equal to position
      for the first element in the new chain. That means updated record
      is the only record in the new chain.
    */
    if (empty != idx)
    {
      /*
        Record was moved while unlinking it from the old chain.
        Copy data to a new position.
      */
      data[empty]= org_link;
    }
    data[empty].next= NO_RECORD;
    data[empty].hash_nr= hash_nr;
    DBUG_RETURN(0);
  }
  pos=data+new_index;
  new_pos_index= my_hash_rec_mask(pos, blength, records);
  if (new_index != new_pos_index)
  {					/* Other record in wrong position */
    data[empty]= *pos;
    movelink(data,new_index,new_pos_index, (uint) empty);
    org_link.next=NO_RECORD;
    data[new_index]= org_link;
    data[new_index].hash_nr= hash_nr;
  }
  else
  {					/* Link in chain at right position */
    org_link.next=data[new_index].next;
    data[empty]=org_link;
    data[empty].hash_nr= hash_nr;
    data[new_index].next= (uint) empty;
  }
  DBUG_RETURN(0);
}


uchar *my_hash_element(HASH *hash, size_t idx)
{
  if (idx < hash->records)
    return dynamic_element(&hash->array,idx,HASH_LINK*)->data;
  return 0;
}


/*
  Replace old row with new row.  This should only be used when key
  isn't changed
*/

void my_hash_replace(HASH *hash, HASH_SEARCH_STATE *current_record,
                     uchar *new_row)
{
  if (*current_record != NO_RECORD)            /* Safety */
    dynamic_element(&hash->array, *current_record, HASH_LINK*)->data= new_row;
}


/**
   Iterate over all elements in hash and call function with the element

   @param hash     hash array
   @param action   function to call for each argument
   @param argument second argument for call to action

   @notes
   If one of functions calls returns 1 then the iteration aborts

   @retval 0  ok
   @retval 1  iteration aborted becasue action returned 1
*/

my_bool my_hash_iterate(HASH *hash, my_hash_walk_action action, void *argument)
{
  uint records, i;

  records= hash->records;

  for (i= 0 ; i < records ; i++)
  {
    if ((*action)(dynamic_element(&hash->array, i, HASH_LINK *)->data,
                  argument))
      return 1;
  }
  return 0;
}


#if !defined(DBUG_OFF) || defined(MAIN)

my_bool my_hash_check(HASH *hash)
{
  int error;
  uint i,rec_link,found,max_links,seek,links,idx;
  uint records;
  size_t blength;
  HASH_LINK *data,*hash_info;

  records=hash->records; blength=hash->blength;
  data=dynamic_element(&hash->array,0,HASH_LINK*);
  error=0;

  for (i=found=max_links=seek=0 ; i < records ; i++)
  {
    size_t length;
    uchar *key= (uchar*) my_hash_key(hash, data[i].data, &length, 0);
    if (data[i].hash_nr != hash->hash_function(hash->charset, key, length))
    {
      DBUG_PRINT("error", ("record at %d has wrong hash", i));
      error= 1;
    }

    if (my_hash_rec_mask(data + i, blength, records) == i)
    {
      found++; seek++; links=1;
      for (idx=data[i].next ;
	   idx != NO_RECORD && found < records + 1;
	   idx=hash_info->next)
      {
	if (idx >= records)
	{
	  DBUG_PRINT("error",
		     ("Found pointer outside array to %d from link starting at %d",
		      idx,i));
	  error=1;
	}
	hash_info=data+idx;
	seek+= ++links;
	if ((rec_link= my_hash_rec_mask(hash_info,
                                        blength, records)) != i)
	{
          DBUG_PRINT("error", ("Record in wrong link at %d: Start %d  "
                               "Record:%p  Record-link %d",
                               idx, i, hash_info->data, rec_link));
	  error=1;
	}
	else
	  found++;
      }
      if (links > max_links) max_links=links;
    }
  }
  if (found != records)
  {
    DBUG_PRINT("error",("Found %u of %u records", found, records));
    error=1;
  }
  if (records)
    DBUG_PRINT("info",
	       ("records: %u   seeks: %d   max links: %d   hitrate: %.2f",
		records,seek,max_links,(float) seek / (float) records));
  DBUG_ASSERT(error == 0);
  return error;
}
#endif

#ifdef MAIN

#define RECORDS 1000

uchar *test_get_key(uchar *data, size_t *length,
                    my_bool not_used __attribute__((unused)))
{
  *length= 2;
  return data;
}


int main(int argc __attribute__((unused)),char **argv __attribute__((unused)))
{
  uchar records[RECORDS][2], copy[2];
  HASH hash_test;
  uint i;
  MY_INIT(argv[0]);
  DBUG_PUSH("d:t:O,/tmp/test_hash.trace");

  printf("my_hash_init\n");
  if (my_hash_init2(PSI_INSTRUMENT_ME, &hash_test, 100, &my_charset_bin, 20,
                    0, 0, (my_hash_get_key) test_get_key, 0, 0, HASH_UNIQUE))
  {
    fprintf(stderr, "hash init failed\n");
    exit(1);
  }

  printf("my_hash_insert\n");
  for (i= 0 ; i < RECORDS ; i++)
  {
    int2store(records[i],i);
    my_hash_insert(&hash_test, records[i]);
    my_hash_check(&hash_test);
  }
  printf("my_hash_update\n");
  for (i= 0 ; i < RECORDS ; i+=2)
  {
    memcpy(copy, records[i], 2);
    int2store(records[i],i + RECORDS);
    if (my_hash_update(&hash_test, records[i], copy, 2))
    {
      fprintf(stderr, "hash update failed\n");
      exit(1);
    }
    my_hash_check(&hash_test);
  }
  printf("my_hash_delete\n");
  for (i= 0 ; i < RECORDS ; i++)
  {
    if (my_hash_delete(&hash_test, records[i]))
    {
      fprintf(stderr, "hash delete failed\n");
      exit(1);
    }
    my_hash_check(&hash_test);
  }
  my_hash_free(&hash_test);
  printf("ok\n");
  my_end(MY_CHECK_ERROR);
  return(0);
}
#endif /* MAIN */
