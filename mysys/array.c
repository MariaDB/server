/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

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

/* Handling of arrays that can grow dynamicly. */

#include "mysys_priv.h"
#include "m_string.h"

/*
  Initiate dynamic array

  SYNOPSIS
    init_dynamic_array2()
      array		Pointer to an array
      element_size	Size of element
      init_buffer       Initial buffer pointer
      init_alloc	Number of initial elements
      alloc_increment	Increment for adding new elements
      my_flags		Flags to my_malloc

  DESCRIPTION
    init_dynamic_array() initiates array and allocate space for
    init_alloc eilements.
    Array is usable even if space allocation failed, hence, the
    function never returns TRUE.

  RETURN VALUE
    FALSE	Ok
*/

my_bool init_dynamic_array2(PSI_memory_key psi_key, DYNAMIC_ARRAY *array,
                            size_t element_size, void *init_buffer,
                            size_t init_alloc, size_t alloc_increment,
                            myf my_flags)
{
  DBUG_ENTER("init_dynamic_array2");
  if (!alloc_increment)
  {
    alloc_increment=MY_MAX((8192-MALLOC_OVERHEAD)/element_size,16);
    if (init_alloc > 8 && alloc_increment > init_alloc * 2)
      alloc_increment=init_alloc*2;
  }
  array->elements=0;
  array->max_element=init_alloc;
  array->alloc_increment=alloc_increment;
  array->size_of_element=element_size;
  array->m_psi_key= psi_key;
  array->malloc_flags= my_flags;
  DBUG_ASSERT((my_flags & MY_INIT_BUFFER_USED) == 0);
  if ((array->buffer= init_buffer))
  {
    array->malloc_flags|= MY_INIT_BUFFER_USED; 
    DBUG_RETURN(FALSE);
  }
  /* 
    Since the dynamic array is usable even if allocation fails here malloc
    should not throw an error
  */
  if (init_alloc &&
      !(array->buffer= (uchar*) my_malloc(psi_key, element_size*init_alloc,
                                          MYF(my_flags))))
    array->max_element=0;
  DBUG_RETURN(FALSE);
}

/*
  Insert element at the end of array. Allocate memory if needed.

  SYNOPSIS
    insert_dynamic()
      array
      element

  RETURN VALUE
    TRUE	Insert failed
    FALSE	Ok
*/

my_bool insert_dynamic(DYNAMIC_ARRAY *array, const void * element)
{
  void *buffer;
  if (array->elements == array->max_element)
  {						/* Call only when necessary */
    if (!(buffer=alloc_dynamic(array)))
      return TRUE;
  }
  else
  {
    buffer=array->buffer+(array->elements * array->size_of_element);
    array->elements++;
  }
  memcpy(buffer,element,(size_t) array->size_of_element);
  return FALSE;
}


/*
  Alloc space for next element(s)

  SYNOPSIS
    alloc_dynamic()
      array

  DESCRIPTION
    alloc_dynamic() checks if there is empty space for at least
    one element if not tries to allocate space for alloc_increment
    elements at the end of array.

  RETURN VALUE
    pointer	Pointer to empty space for element
    0		Error
*/

void *alloc_dynamic(DYNAMIC_ARRAY *array)
{
  DBUG_ENTER("alloc_dynamic");

  DBUG_ASSERT(array->size_of_element);          /* Ensure init() is called */
  if (array->elements == array->max_element)
  {
    char *new_ptr;
    if (array->malloc_flags & MY_INIT_BUFFER_USED)
    {
      /*
        In this scenario, the buffer is statically preallocated,
        so we have to create an all-new malloc since we overflowed
      */
      if (!(new_ptr= (char *) my_malloc(array->m_psi_key,
                                        (array->max_element+
                                         array->alloc_increment) *
                                        array->size_of_element,
                                        MYF(array->malloc_flags | MY_WME))))
        DBUG_RETURN(0);
      if (array->elements)
        memcpy(new_ptr, array->buffer,
               array->elements * array->size_of_element);
      array->malloc_flags&= ~MY_INIT_BUFFER_USED;
    }
    else if (!(new_ptr=(char*)
               my_realloc(array->m_psi_key, array->buffer,
                          (array->max_element+ array->alloc_increment) *
                          array->size_of_element,
                          MYF(MY_WME | MY_ALLOW_ZERO_PTR |
                              array->malloc_flags))))
      DBUG_RETURN(0);
    array->buffer= (uchar*) new_ptr;
    array->max_element+=array->alloc_increment;
  }
  DBUG_RETURN(array->buffer+(array->elements++ * array->size_of_element));
}


/*
  Pop last element from array.

  SYNOPSIS
    pop_dynamic()
      array

  RETURN VALUE
    pointer	Ok
    0		Array is empty
*/

void *pop_dynamic(DYNAMIC_ARRAY *array)
{
  if (array->elements)
    return array->buffer+(--array->elements * array->size_of_element);
  return 0;
}

/*
  Replace element in array with given element and index

  SYNOPSIS
    set_dynamic()
      array
      element	Element to be inserted
      idx	Index where element is to be inserted

  DESCRIPTION
    set_dynamic() replaces element in array.
    If idx > max_element insert new element. Allocate memory if needed.

  RETURN VALUE
    TRUE	Idx was out of range and allocation of new memory failed
    FALSE	Ok
*/

my_bool set_dynamic(DYNAMIC_ARRAY *array, const void *element, size_t idx)
{
  if (idx >= array->elements)
  {
    if (idx >= array->max_element && allocate_dynamic(array, idx))
      return TRUE;
    bzero((uchar*) (array->buffer+array->elements*array->size_of_element),
	  (idx - array->elements)*array->size_of_element);
    array->elements=idx+1;
  }
  memcpy(array->buffer+(idx * array->size_of_element),element,
         array->size_of_element);
  return FALSE;
}


/*
  Ensure that dynamic array has enough elements

  SYNOPSIS
    allocate_dynamic()
    array
    max_elements        Numbers of elements that is needed

  NOTES
   Any new allocated element are NOT initialized

  RETURN VALUE
    FALSE	Ok
    TRUE	Allocation of new memory failed
*/

my_bool allocate_dynamic(DYNAMIC_ARRAY *array, size_t max_elements)
{
  DBUG_ENTER("allocate_dynamic");

  if (max_elements >= array->max_element)
  {
    size_t size;
    uchar *new_ptr;
    size= (max_elements + array->alloc_increment)/array->alloc_increment;
    size*= array->alloc_increment;
    if (array->malloc_flags & MY_INIT_BUFFER_USED)
    {
       /*
         In this senerio, the buffer is statically preallocated,
         so we have to create an all-new malloc since we overflowed
       */
       if (!(new_ptr= (uchar *) my_malloc(array->m_psi_key, size *
                                          array->size_of_element,
                                          MYF(array->malloc_flags | MY_WME))))
         DBUG_RETURN(0);
       memcpy(new_ptr, array->buffer,
              array->elements * array->size_of_element);
       array->malloc_flags&= ~MY_INIT_BUFFER_USED;
    }
    else if (!(new_ptr= (uchar*) my_realloc(array->m_psi_key,
                                            array->buffer,size *
                                            array->size_of_element,
                                            MYF(MY_WME | MY_ALLOW_ZERO_PTR |
                                                array->malloc_flags))))
      DBUG_RETURN(TRUE);
    array->buffer= new_ptr;
    array->max_element= size;
  }
  DBUG_RETURN(FALSE);
}


/*
  Get an element from array by given index

  SYNOPSIS
    get_dynamic()
      array
      uchar*	Element to be returned. If idx > elements contain zeroes.
      idx	Index of element wanted.
*/

void get_dynamic(DYNAMIC_ARRAY *array, void *element, size_t idx)
{
  if (idx >= array->elements)
  {
    DBUG_PRINT("warning",("To big array idx: %d, array size is %d",
                          idx,array->elements));
    bzero(element,array->size_of_element);
    return;
  }
  memcpy(element,array->buffer+idx*array->size_of_element,
         (size_t) array->size_of_element);
}


/*
  Empty array by freeing all memory

  SYNOPSIS
    delete_dynamic()
      array	Array to be deleted
*/

void delete_dynamic(DYNAMIC_ARRAY *array)
{
  /*
    Just mark as empty if we are using a static buffer
  */
  if (!(array->malloc_flags & MY_INIT_BUFFER_USED) && array->buffer)
    my_free(array->buffer);

  array->buffer= 0;
  array->elements= array->max_element= 0;
}

/*
  Delete element by given index

  SYNOPSIS
    delete_dynamic_element()
      array
      idx        Index of element to be deleted
*/

void delete_dynamic_element(DYNAMIC_ARRAY *array, size_t idx)
{
  char *ptr= (char*) array->buffer+array->size_of_element*idx;
  array->elements--;
  memmove(ptr,ptr+array->size_of_element,
          (array->elements-idx)*array->size_of_element);
}

/*
  Wrapper around delete_dynamic, calling a FREE function on every
  element, before releasing the memory

  SYNOPSIS
    delete_dynamic_with_callback()
      array
      f          The function to be called on every element before
                 deleting the array;
*/
void delete_dynamic_with_callback(DYNAMIC_ARRAY *array, FREE_FUNC f) {
  size_t i;
  char *ptr= (char*) array->buffer;
  for (i= 0; i < array->elements; i++, ptr+= array->size_of_element) {
    f(ptr);
  }
  delete_dynamic(array);
}
/*
  Free unused memory

  SYNOPSIS
    freeze_size()
      array	Array to be freed

*/

void freeze_size(DYNAMIC_ARRAY *array)
{
  size_t elements;

  /*
    Do nothing if we are using a static buffer
  */
  if (array->malloc_flags & MY_INIT_BUFFER_USED)
    return;

  elements= MY_MAX(array->elements, 1);
  if (array->buffer && array->max_element > elements)
  {
    array->buffer=(uchar*) my_realloc(array->m_psi_key, array->buffer,
                                      elements * array->size_of_element,
                                      MYF(MY_WME | array->malloc_flags));
    array->max_element= elements;
  }
}
