#pragma once

#include <string.h>
#include <my_dbug.h>
#include <m_string.h>



template <typename key_trait, typename value_trait> class open_address_hash
{
public:
  using T= typename value_trait::elem_type;
  using find_type= typename value_trait::elem_type;
  using erase_type= typename value_trait::elem_type;
  using hash_value_type= typename key_trait::hash_value_type;
  using key_type= typename key_trait::key_type;

  const key_type *get_key(const T &elem)
  {
    return value_trait::template get_key<key_type>(elem);
  }
  bool is_empty(const T &el) { return value_trait::is_empty(el); }
  void set_null(T &el) { value_trait::set_null(el); }

  open_address_hash()
  {
    first.set_mark(true);
    first.set_ptr(nullptr);
    second= nullptr;
  }     

  ~open_address_hash()
  {
    if (!first.mark())
    {
      DBUG_ASSERT(hash_array);
      free(hash_array);
    }
  }

private:
  hash_value_type to_index(const hash_value_type &hash_value)
  {
    return hash_value & (capacity - 1);
  }

  hash_value_type hash_from_value(const T &value)
  {
    return key_trait::get_hash_value(get_key(value));
  }

  bool insert_helper(const T &value)
  {

    auto hash_val= to_index(hash_from_value(value));

    while (hash_array[hash_val] != nullptr)
    {
      if (hash_array[hash_val] == value)
        return false;
      hash_val= to_index(hash_val + 1);
    }

    hash_array[hash_val]= value;
    return true;
  };

  uint rehash_subsequence(uint i)
  {
    for (uint j= to_index(i + 1); hash_array[j] != nullptr; j= to_index(j + 1))
    {
      auto temp_el= hash_array[j];
      hash_array[j]= nullptr;
      insert_helper(temp_el);
      /*auto key= to_index(key_trait::get_hash_value(trait::get_key(hash_array[j])));
      if (key <= i || key > j)
      {
        hash_array[i]= hash_array[j];
        i= j;
      }*/
    }

    return i;
  }

  bool erase_helper(const erase_type &value)
  {
    for (auto key= to_index(key_trait::get_hash_value(get_key(value)));
         hash_array[key] != nullptr; key= to_index(key + 1))
    {
      if (value_trait::is_equal(hash_array[key], value))
      {
        hash_array[key]= nullptr;
        rehash_subsequence(key);
        return true;
      }
    }

    return false;
  }

  void grow(const uint _capacity)
  {
    uint past_capacity= capacity;
    capacity= _capacity;
    hash_array= (T *) realloc(hash_array, capacity * sizeof(T));
    bzero(hash_array + past_capacity, (capacity - past_capacity)*sizeof(T*));
   /* for (uint i= past_capacity; i < capacity; i++)
    {
      hash_array[i]= nullptr;
    }*/

    for (uint i= 0; i < capacity; i++)
    {
      if (hash_array[i] && i != to_index(hash_from_value(hash_array[i])))
      {
        auto temp_el= hash_array[i];
        hash_array[i]= nullptr;
        insert_helper(temp_el);
      }
    }
  }

  void shrink(const uint _capacity)
  {
    uint past_capacity= capacity;
    capacity= _capacity;

    for (uint i= capacity; i < past_capacity; i++)
    {
      if (hash_array[i])
      {
        auto temp_el= hash_array[i];
        insert_helper(temp_el);
      }
    }

    hash_array= (T *) realloc(hash_array, capacity * sizeof(T));
  }


  bool init_hash_array()
  {
    T _first= first.ptr();
    T _second= second;

    capacity= CAPACITY_INITIAL;
    hash_array= (T*)calloc(capacity, sizeof (T*));
    _size= 0;

    if (insert_helper(_first))
      _size++;
    else
      return false;
    if (insert_helper(_second))
      _size++;
    else
      return false;

    return true;
  }

public:
  T find(const T &elem)
  {
    return find(key_trait::get_key(elem),
                [&elem](const T &rhs) { return rhs == elem; });
  }

  template <typename Func>
  T find(const key_type &key, const Func &elem_suits)
  {
    if (first.mark())
    {
      if (first.ptr() && elem_suits(first.ptr()))
        return first.ptr();
      if (second != nullptr && elem_suits(second))
        return second;

      return nullptr;
    }

    for (auto idx= to_index(key_trait::get_hash_value(&key));
         hash_array[idx] != nullptr; idx= to_index(idx + 1))
    {
      if (elem_suits(hash_array[idx]))
        return hash_array[idx];
    }

    return nullptr;
  };

  bool erase(const erase_type &value)
  {
    if (first.mark())
    {
      if (first.ptr() != nullptr && value_trait::is_equal(first.ptr(), value))
      {
        first.set_ptr(nullptr);
        return true;
      }
      else if (second && value_trait::is_equal(second, value))
      {
        second= nullptr;
        return true;
      }
      else
      {
        return false;
      }
    }

    if (unlikely(capacity > 7 && (_size - 1) * LOW_LOAD_FACTOR < capacity))
      shrink(capacity >> 1);

    if (erase_helper(value))
    {
      _size--;
      return true;
    }
    else
    {
      return false;
    }
  }

  bool insert(const T &value)
  {
    if (first.mark())
    {
      if (!first.ptr())
      {
        first.set_ptr(value);
        return true;
      }
      else if (!second)
      {
        second= value;
        return true;
      }
      else
      {
        first.set_mark(false);
        if (!init_hash_array())
          return false;
      }
    }

    if (unlikely((_size + 1) * MAX_LOAD_FACTOR > capacity))
      grow(capacity << 1);

    if (insert_helper(value))
    {
      _size++;
      return true;
    }
    else
    {
      return false;
    }
    //return insert_helper(value);
  };

  bool clear()
  {
    if (first.mark())
    {
      first.set_ptr(nullptr);
      second= nullptr;
      return true;
    }
    if (!hash_array)
      return false;

    delete hash_array;
    capacity= CAPACITY_INITIAL;

    first.set_mark(true);
    first.set_ptr(nullptr);
    second= nullptr;

    return true;
  }

  uint32 size()
  { 
    if (first.mark())
    {
      uint32 ret_size{};
      if (first.ptr())
        ret_size++;
      if (second)
        ret_size++;
      return ret_size;
    }
    else
    {
      return _size; 
    }
  }
  uint32 buffer_size(){ return first.mark() ? 0 : capacity; }

private:
  static constexpr uint power2_start= 2;
  static constexpr uint CAPACITY_INITIAL= 1 << power2_start;
  static constexpr int MAX_LOAD_FACTOR= 2;
  static constexpr int LOW_LOAD_FACTOR= 10;

  class markable_reference
  {
  public:
    void set_ptr(T ptr) { p= reinterpret_cast<uintptr_t>(ptr); }
    T ptr() { return reinterpret_cast<T>(p); }

    void set_mark(bool mark) { low_type= mark; }
    bool mark() { return low_type; };

  private:
    bool low_type : 1;
    uintptr_t p : 63;
  };

  union
  {
    struct
    {
      markable_reference first;
      T second;
    };
    struct
    {
      T *hash_array;
      uint32 _size;
      uint32 capacity;
    };
  };
};
