/* Copyright (c) 2012, 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#pragma once

#include <utility>
#include "my_global.h"
#include "my_atomic.h"

template <typename Ty> class Smart_ptr_base;
template <typename Ty> class Reference_counter;

/*
  Shared_ptr is a smart pointer that retains shared ownership of an object
  through a pointer. Several Shared_ptr objects may own the same object.
  The object is destroyed and its memory deallocated when either
  of the following happens:
    - the last remaining Shared_ptr owning the object is destroyed;
    - the last remaining Shared_ptr owning the object is assigned
      another pointer via operator= or reset().

  All member functions (including copy constructor and copy assignment) can be
  called by multiple threads on different instances of Shared_ptr without
  additional synchronization even if these instances are copies and share
  ownership of the same object.

  This implementation is inspired by the STL's std::shared_ptr
  and has similar (though less advanced) interface.
*/
template <typename Type>
class Shared_ptr : public Smart_ptr_base<Type>
{
public:
  /* Construct an empty Shared_ptr */
  Shared_ptr() : Smart_ptr_base<Type>() {}

  /* Construct a Shared_ptr that owns a null pointer */
  Shared_ptr(std::nullptr_t) : Smart_ptr_base<Type>(nullptr) {}

  /* Construct Shared_ptr object that owns object obj */
  explicit Shared_ptr(Type *obj) : Smart_ptr_base<Type> (obj) {}

  /*
    Construct Shared_ptr object that owns same resource as other
    (copy-construct)
  */
  Shared_ptr(const Shared_ptr &other) :
    Smart_ptr_base<Type>(other)
  {}

  /*
    Construct Shared_ptr object that takes resource from right
    (move-construct)
  */
  Shared_ptr(Shared_ptr &&other) :
    Smart_ptr_base<Type>(std::move(other))
  {}

  ~Shared_ptr() { this->dec_ref_count(); }

  Shared_ptr &operator=(const Shared_ptr &right)
  {
    Shared_ptr(right).swap(*this);
    return *this;
  }

  template <class Type2>
  Shared_ptr &operator=(const Shared_ptr<Type2> &right)
  {
    Shared_ptr(right).swap(*this);
    return *this;
  }

  Shared_ptr &operator=(Shared_ptr &&right)
  {
    Shared_ptr(std::move(right)).swap(*this);
    return *this;
  }

  template <class Type2>
  Shared_ptr &operator=(Shared_ptr<Type2> &&right)
  {
    Shared_ptr(std::move(right)).swap(*this);
    return *this;
  }

  void swap(Shared_ptr &other) { Smart_ptr_base<Type>::swap(other); }

  /* Release resource and convert to empty Shared_ptr object */
  void reset() { Shared_ptr().swap(*this); }

  /* Release, take ownership of obj */
  template <typename Type2> void reset(Type2 *obj)
  {
    Shared_ptr(obj).swap(*this);
  }

  using Smart_ptr_base<Type>::get;

  template <typename Type2= Type> Type2 &operator*() const
  {
    return *get();
  }

  template <typename Type2= Type> Type2 *operator->() const
  {
    return get();
  }

  explicit operator bool() const { return get() != nullptr; }
};

/* Shared_ptr comparisons */
template<typename Type1, typename Type2>
bool operator==(const Shared_ptr<Type1>& a, const Shared_ptr<Type2>& b)
{
  return a.get() == b.get();
}

template<typename Type1>
bool operator==(const Shared_ptr<Type1>& a, std::nullptr_t)
{
  return a.get() == nullptr;
}

template<typename Type1>
bool operator==(std::nullptr_t, const Shared_ptr<Type1>& b)
{
  return nullptr == b.get();
}

template<typename Type1, typename Type2>
bool operator!=(const Shared_ptr<Type1>& a, const Shared_ptr<Type2>& b)
{
  return a.get() != b.get();
}

template<typename Type1>
bool operator!=(const Shared_ptr<Type1>& a, std::nullptr_t)
{
  return a.get() != nullptr;
}

template<typename Type1>
bool operator!=(std::nullptr_t, const Shared_ptr<Type1>& b)
{
  return nullptr != b.get();
}


/* Base class for reference counting */
class Reference_counter_base
{
protected:
  Reference_counter_base() = default;

public:
  /* Forbid copying and moving */
  Reference_counter_base(const Reference_counter_base &)= delete;
  Reference_counter_base(Reference_counter_base &&)= delete;
  Reference_counter_base &operator=(const Reference_counter_base &)= delete;
  Reference_counter_base &operator=(Reference_counter_base &&)= delete;

  virtual ~Reference_counter_base() {}

  /* Increment use count */
  void inc_ref_count()
  {
    my_atomic_add32(&uses, 1);
  }

  /* Decrement use count */
  void dec_ref_count()
  {
    int32 prev_value= 0;
    if ((prev_value= my_atomic_add32(&uses, -1)) == 1)
    {
      destroy();
      delete_this();
    }
  }

  unsigned long use_count() { return my_atomic_load32(&uses); }

private:
  virtual void destroy() = 0; /* destroy managed resource */
  virtual void delete_this()  = 0; /* destroy self */

  volatile int32 uses= 1;
};


template <typename Type>
class Reference_counter : public Reference_counter_base
{
public:
  explicit Reference_counter(Type *obj) :
    Reference_counter_base(), managed_obj(obj)
  {}

private:
  virtual void destroy() override
  {
    /* destroy managed resource */
    delete managed_obj;
  }

  void delete_this() override
  {
    /* destroy self */
    delete this;
  }

  Type *managed_obj;
};


/* Base class for Shared_ptr and Weak_ptr (to be implemented later)*/
template <typename Type>
class Smart_ptr_base
{
public:
  Smart_ptr_base() = default;

  /* Forbid copy and move assignment operators */
  Smart_ptr_base &operator=(const Smart_ptr_base &)= delete;
  Smart_ptr_base &operator=(Smart_ptr_base &&)= delete;

  long use_count() const
  {
    return ref_counter ? ref_counter->use_count() : 0;
  }

protected:
  Smart_ptr_base(std::nullptr_t) {}

  explicit Smart_ptr_base(Type *obj) :
    managed_obj(obj),
    ref_counter(new Reference_counter<Type>(obj))
  {}

  Smart_ptr_base(const Smart_ptr_base &other)
  {
    other.inc_ref_count();

    managed_obj= other.managed_obj;
    ref_counter= other.ref_counter;
  }

  Smart_ptr_base(Smart_ptr_base &&other)
  {
    managed_obj= other.managed_obj;
    ref_counter= other.ref_counter;

    other.managed_obj= nullptr;
    other.ref_counter= nullptr;
  }

  Type *get() const { return managed_obj; }

  void inc_ref_count() const
  {
    if (ref_counter)
      ref_counter->inc_ref_count();
  }

  void dec_ref_count()
  {
    if (ref_counter)
      ref_counter->dec_ref_count();
  }

  void swap(Smart_ptr_base &other)
  {
    std::swap(this->managed_obj, other.managed_obj);
    std::swap(this->ref_counter, other.ref_counter);
  }

protected:
  /*
    We intentionally made the destructor non-virtual to avoid creation of
    the vtable for Smart_ptr_base. Though the class has children it does not
    have virtual functions so there is no need for the vtable. To avoid common
    mistake with non-virtual destructors like this one:
      Smart_ptr_base<T> a= new Shared_ptr<T>;
      delete a;
    this destructor is placed in the protected section
  */
  ~Smart_ptr_base()= default;

private:
  Type *managed_obj= nullptr;
  Reference_counter_base *ref_counter= nullptr;
};
