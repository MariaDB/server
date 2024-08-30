#ifndef SQL_ARRAY_INCLUDED
#define SQL_ARRAY_INCLUDED

/* Copyright (c) 2003, 2005-2007 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.
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

#include <my_sys.h>

/**
   A wrapper class which provides array bounds checking.
   We do *not* own the array, we simply have a pointer to the first element,
   and a length.

   @remark
   We want the compiler-generated versions of:
   - the copy CTOR (memberwise initialization)
   - the assignment operator (memberwise assignment)

   @param Element_type The type of the elements of the container.
 */
template <typename Element_type> class Bounds_checked_array
{
public:
  Bounds_checked_array()= default;

  Bounds_checked_array(Element_type *el, size_t size_arg)
    : m_array(el), m_size(size_arg)
  {}

  void reset() { m_array= NULL; m_size= 0; }
 
  void reset(Element_type *array_arg, size_t size_arg)
  {
    m_array= array_arg;
    m_size= size_arg;
  }

  /**
    Set a new bound on the array. Does not resize the underlying
    array, so the new size must be smaller than or equal to the
    current size.
   */
  void resize(size_t new_size)
  {
    DBUG_ASSERT(new_size <= m_size);
    m_size= new_size;
  }

  Element_type &operator[](size_t n)
  {
    DBUG_ASSERT(n < m_size);
    return m_array[n];
  }

  const Element_type &operator[](size_t n) const
  {
    DBUG_ASSERT(n < m_size);
    return m_array[n];
  }

  size_t element_size() const { return sizeof(Element_type); }
  size_t size() const         { return m_size; }

  bool is_null() const { return m_array == NULL; }

  void pop_front()
  {
    DBUG_ASSERT(m_size > 0);
    m_array+= 1;
    m_size-= 1;
  }

  Element_type *array() const { return m_array; }

  Element_type *begin() const { return array(); }
  Element_type *end() const { return array() + m_size; }


  bool operator==(const Bounds_checked_array<Element_type>&rhs) const
  {
    return m_array == rhs.m_array && m_size == rhs.m_size;
  }
  bool operator!=(const Bounds_checked_array<Element_type>&rhs) const
  {
    return m_array != rhs.m_array || m_size != rhs.m_size;
  }

private:
  Element_type *m_array= nullptr;
  size_t        m_size= 0;
};

/*
  A typesafe wrapper around DYNAMIC_ARRAY

  TODO: Change creator to take a THREAD_SPECIFIC option.
*/

template <class Elem> class Dynamic_array
{
  DYNAMIC_ARRAY array;
public:
  Dynamic_array(PSI_memory_key psi_key, size_t prealloc=16, size_t increment=16)
  {
    init(psi_key, prealloc, increment);
  }

  Dynamic_array(MEM_ROOT *root, size_t prealloc=16, size_t increment=16)
  {
    void *init_buffer= alloc_root(root, sizeof(Elem) * prealloc);
    init_dynamic_array2(root->psi_key, &array, sizeof(Elem), init_buffer,
                        prealloc, increment, MYF(0));
  }

  void init(PSI_memory_key psi_key, size_t prealloc=16, size_t increment=16)
  {
    init_dynamic_array2(psi_key, &array, sizeof(Elem), 0, prealloc, increment, MYF(0));
  }

  /**
     @note Though formally this could be declared "const" it would be
     misleading at it returns a non-const pointer to array's data.
  */
  Elem& at(size_t idx)
  {
    DBUG_ASSERT(idx < array.elements);
    return *(((Elem*)array.buffer) + idx);
  }

  /// Const variant of at(), which cannot change data
  const Elem& at(size_t idx) const
  {
    return *(((Elem*)array.buffer) + idx);
  }

  Elem& operator[](size_t idx)
  {
    return at(idx);
  }

  /// Const variant of operator[]
  const Elem& operator[](size_t idx) const
  {
    return at(idx);
  }

  /// @returns pointer to first element
  Elem *front()
  {
    return (Elem*)array.buffer;
  }

  /// @returns pointer to first element
  const Elem *front() const
  {
    return (const Elem*)array.buffer;
  }

  /// @returns pointer to last element
  Elem *back()
  {
    return ((Elem*)array.buffer) + array.elements - 1;
  }

  /// @returns pointer to last element
  const Elem *back() const
  {
    return ((const Elem*)array.buffer) + array.elements - 1;
  }

  size_t size() const { return array.elements; }

  const Elem *end() const
  {
    return back() + 1;
  }

  /// @returns pointer to n-th element
  Elem *get_pos(size_t idx)
  {
    return ((Elem*)array.buffer) + idx;
  }

  /// @returns pointer to n-th element
  const Elem *get_pos(size_t idx) const
  {
    return ((const Elem*)array.buffer) + idx;
  }

  /**
     @retval false ok
     @retval true  OOM, @c my_error() has been called.
  */
  bool append(const Elem &el)
  {
    return insert_dynamic(&array, &el);
  }

  bool append_val(Elem el)
  {
    return (insert_dynamic(&array, (uchar*)&el));
  }

  bool push(Elem &el)
  {
    return append(el);
  }

  /// Pops the last element. Does nothing if array is empty.
  Elem& pop()
  {
    return *((Elem*)pop_dynamic(&array));
  }

  void del(size_t idx)
  {
    DBUG_ASSERT(idx <= array.max_element);
    delete_dynamic_element(&array, idx);
  }

  size_t elements() const
  {
    return array.elements;
  }

  void elements(size_t num_elements)
  {
    DBUG_ASSERT(num_elements <= array.max_element);
    array.elements= num_elements;
  }

  void clear()
  {
    elements(0);
  }

  void set(size_t idx, const Elem &el)
  {
    set_dynamic(&array, &el, idx);
  }

  void freeze()
  {
    freeze_size(&array);
  }

  bool reserve(size_t new_size)
  {
    return allocate_dynamic(&array, new_size);
  }


  bool resize(size_t new_size, Elem default_val)
  {
    size_t old_size= elements();
    if (reserve(new_size))
      return true;
    
    if (new_size > old_size)
    {
      set_dynamic(&array, (uchar*)&default_val, new_size - 1);
      /*for (size_t i= old_size; i != new_size; i++)
      {
        at(i)= default_val;
      }*/
    }
    return false;
  }

  ~Dynamic_array()
  {
    delete_dynamic(&array);
  }

  void free_memory()
  {
    delete_dynamic(&array);
  }

  typedef int (*CMP_FUNC)(const Elem *el1, const Elem *el2);

  void sort(CMP_FUNC cmp_func)
  {
    my_qsort(array.buffer, array.elements, sizeof(Elem), (qsort_cmp)cmp_func);
  }

  typedef int (*CMP_FUNC2)(void *, const Elem *el1, const Elem *el2);
  void sort(CMP_FUNC2 cmp_func, void *data)
  {
    my_qsort2(array.buffer, array.elements, sizeof(Elem), (qsort2_cmp)cmp_func, data);
  }
};

typedef Bounds_checked_array<Item*> Ref_ptr_array;

#endif /* SQL_ARRAY_INCLUDED */
