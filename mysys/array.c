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
      ps_key            Key to register instrumented memory
      array		Pointer to an array
      element_size	Size of element
      init_buffer       Initial buffer pointer
      init_alloc	Number of initial elements
      alloc_increment	Increment for adding new elements
      my_flags		Flags to my_malloc

  DESCRIPTION
    init_dynamic_array() initiates array and allocate space for
    init_alloc elements.
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
  if (unlikely(array->elements == array->max_element))
  {						/* Call only when necessary */
    if (!(buffer=alloc_dynamic(array)))
      return TRUE;
  }
  else
  {
    buffer=array->buffer+(array->elements * array->size_of_element);
    array->elements++;
  }
  memcpy(buffer, element, array->size_of_element);
  return FALSE;
}


/* Fast version of appending to dynamic array */

void init_append_dynamic(DYNAMIC_ARRAY_APPEND *append,
                         DYNAMIC_ARRAY *array)
{
  append->array= array;
  append->pos= array->buffer + array->elements * array->size_of_element;
  append->end= array->buffer + array->max_element * array->size_of_element;
}


my_bool append_dynamic(DYNAMIC_ARRAY_APPEND *append,
                       const void *element)
{
  DYNAMIC_ARRAY *array= append->array;
  size_t size_of_element= array->size_of_element;
  if (unlikely(append->pos == append->end))
  {
    void *buffer;
    if (!(buffer=alloc_dynamic(array)))
      return TRUE;
    append->pos= (uchar*)buffer + size_of_element;
    append->end= array->buffer + array->max_element * size_of_element;
    memcpy(buffer, element, size_of_element);
  }
  else
  {
    array->elements++;
    memcpy(append->pos, element, size_of_element);
    append->pos+= size_of_element;
  }
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
         In this scenario, the buffer is statically preallocated,
         so we have to create an all-new malloc since we overflowed
       */
       if (!(new_ptr= (uchar *) my_malloc(array->m_psi_key, size *
                                          array->size_of_element,
                                          MYF(array->malloc_flags | MY_WME))))
         DBUG_RETURN(TRUE);
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
  if (unlikely(idx >= array->elements))
  {
    DBUG_PRINT("warning",("To big array idx: %zu, array size is %zu",
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
  if (array->buffer && !(array->malloc_flags & MY_INIT_BUFFER_USED))
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

int mem_root_dynamic_array_resize_not_allowed(MEM_ROOT_DYNAMIC_ARRAY *array)
{
  return (array->malloc_flags & MY_BUFFER_NO_RESIZE);
}

int mem_root_dynamic_array_init(MEM_ROOT *current_mem_root,
                                PSI_memory_key psi_key,
                                MEM_ROOT_DYNAMIC_ARRAY *array,
                                size_t element_size, void *init_buffer,
                                size_t init_alloc, size_t alloc_increment,
                                myf my_flags)
{
  DBUG_ENTER("init_mem_root_dynamic_array");

  array->elements=0;
  array->max_element=init_alloc;
  array->alloc_increment=alloc_increment;
  array->size_of_element=element_size;
  array->m_psi_key= psi_key;
  array->malloc_flags= my_flags;
  array->mem_root= current_mem_root;

  if ((array->buffer= (uchar*)init_buffer))
  {
    array->malloc_flags|= MY_INIT_BUFFER_USED;
    DBUG_RETURN(FALSE);
  }

  if (!alloc_increment && !(my_flags & MY_BUFFER_NO_RESIZE))
  {
    alloc_increment=MY_MAX((8192-MALLOC_OVERHEAD)/element_size,16);
    if (init_alloc > 8 && alloc_increment > init_alloc * 2)
      alloc_increment=init_alloc*2;
  }

  /*
    Since the dynamic array is usable even if allocation fails here malloc
    should not throw an error
  */
  if (init_alloc &&
      !(array->buffer= (uchar*) alloc_root(array->mem_root,
                                           array->size_of_element*
                                                 array->max_element)))
    array->max_element=0;
  memset(array->buffer, 0, array->size_of_element*array->max_element);

  DBUG_RETURN(FALSE);
}

void mem_root_dynamic_array_reset(MEM_ROOT_DYNAMIC_ARRAY *array)
{
  memset(array->buffer, 0, (array->size_of_element)*(array->max_element));
}


int mem_root_allocate_dynamic(MEM_ROOT *mem_root,
                              MEM_ROOT_DYNAMIC_ARRAY *array,
                              size_t idx)
{
  DBUG_ENTER("allocate_dynamic");

  if (idx >= array->max_element)
  {
    size_t size;
    uchar *new_ptr;

    size= (idx + array->alloc_increment);
    if (array->malloc_flags & MY_INIT_BUFFER_USED)
    {
       /*
         In this senerio, the buffer is statically preallocated,
         so we have to create an all-new malloc since we overflowed
       */
       if (!(new_ptr= (uchar *) alloc_root(mem_root,
                                           size * array->size_of_element)))
         DBUG_RETURN(0);
       array->malloc_flags&= ~MY_INIT_BUFFER_USED;
    }
    else if (!(new_ptr= (uchar*) alloc_root(mem_root, size*array->size_of_element)))
      DBUG_RETURN(TRUE);
    /* copy old elements first. */
    memcpy(new_ptr, array->buffer,
              array->max_element * array->size_of_element);
    /* set the remainging memory to 0. */
    memset(new_ptr+((array->max_element) * array->size_of_element), 0,
           array->alloc_increment*array->size_of_element);
    array->buffer= new_ptr;
    array->max_element= size;
  }
  DBUG_RETURN(FALSE);
}


inline int mem_root_dynamic_array_set_val(MEM_ROOT_DYNAMIC_ARRAY *array,
                                   const void *element, size_t idx)
{
  if (array->malloc_flags & MY_BUFFER_NO_RESIZE)
    return TRUE;

  if (idx >= array->max_element)
  {
    if (mem_root_allocate_dynamic(array->mem_root, array, idx))
      return 1;
    array->elements++;
  }

  /*
     Ensure the array size has increased and the index is
     now well within the array bounds.
  */
  DBUG_ASSERT(idx < array->max_element);

  memcpy(array->buffer+(idx * array->size_of_element), element,
         array->size_of_element);

  return FALSE;
}

/*
  Note: If these two are merged, the resultant function will have to have
  a conditional block of code. Now, these are called in recursive functions.
  and although in non-recursive case it would probably not matter much,
  calling functions with conditional block recursively introduces performance
  delay.

  Hence, to avoid the performance issue, where we know for sure
  that there will be no need to resize, directly get value. And
  in other places where there might be a need to resize the array,
  use the one with resize.
*/
inline void* mem_root_dynamic_array_get_val(MEM_ROOT_DYNAMIC_ARRAY *array, size_t idx)
{
  void* element_ptr;

  DBUG_ASSERT(idx < array->max_element);

  // Calculate the pointer to the desired element in the array
  element_ptr = array->buffer + (idx * array->size_of_element);
  return element_ptr;
}

inline void* mem_root_dynamic_array_resize_and_get_val(MEM_ROOT_DYNAMIC_ARRAY *array, size_t idx)
{
  if (array->malloc_flags & MY_BUFFER_NO_RESIZE)
    return NULL;

  if (idx >= array->max_element)
  {
    if (mem_root_allocate_dynamic(array->mem_root, array, idx))
      return NULL;
  }

  /*
     Ensure the array size has increased and the index is
     now well within the array bounds.
  */
  DBUG_ASSERT(idx < array->max_element);

  return mem_root_dynamic_array_get_val(array, idx);
}
