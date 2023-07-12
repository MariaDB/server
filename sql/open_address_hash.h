#ifndef OPEN_ADDRESS_HASH
#define OPEN_ADDRESS_HASH

#include <string.h>

class key_type_pair
{
public:
  key_type_pair(MDL_key *_mdl_key, enum_mdl_type _type)
  {
    mdl_key= _mdl_key;
    type= _type;
  }
  MDL_key *mdl_key;
  enum_mdl_type type;
};

template <typename trait, typename key_trait> class open_address_hash
{
public:
  using T= typename trait::elem_type;
  using find_type= typename trait::find_type;
  using erase_type= typename trait::erase_type;
  using hash_value_type= typename key_trait::hash_value_type;
  using key_type= typename key_trait::key_type;

  MDL_key *get_key(const T &elem) { return key_trait::get_key(elem); }
  bool is_empty(const T &el) { return trait::is_empty(el); }
  void set_null(T &el) { trait::set_null(el); }

  open_address_hash()
  {
    first.set_mark(true);
    first.set_ptr(nullptr);
    second= nullptr;
  }

  ~open_address_hash()
  {
    if (hash_array && !first.mark())
      delete[] hash_array;
  }

private:
  hash_value_type to_index(const hash_value_type &hash_value)
  {
    return hash_value & (capacity - 1);
  }

  bool insert_helper(MDL_key *mdl_key, T value)
  {
    auto key= to_index(mdl_key->tc_hash_value());

    while (hash_array[key] != nullptr)
    {
      if (hash_array[key] == value)
        return false;
      key= to_index(key + 1);
    }

    hash_array[key]= value;
    size++;
    return true;
  };

  uint rehash_subsequence(uint i)
  {
    for (uint j= to_index(i + 1); hash_array[j] != nullptr; j= to_index(j + 1))
    {
      auto key= to_index(get_key(hash_array[j])->tc_hash_value());
      if (key <= i || key > j)
      {
        hash_array[i]= hash_array[j];
        i= j;
      }
    }

    return i;
  }

  bool erase_helper(const erase_type &value)
  {
    for (auto key= to_index(key_trait::get_key(value)->tc_hash_value());
         hash_array[key] != nullptr; key= to_index(key + 1))
    {
      if (trait::is_equal(hash_array[key], value))
      {
        hash_array[rehash_subsequence(key)]= nullptr;
        size--;
        return true;
      }
    }

    return false;
  }

  void rehash(const uint _capacity)
  {
    uint past_capacity= capacity;
    capacity= _capacity;
    auto temp_hash_array= hash_array;
    hash_array= new T[capacity]{};
    size= 0;

    for (uint i= 0; i < past_capacity; i++)
    {
      if (temp_hash_array[i])
      {
        insert_helper(get_key(temp_hash_array[i]), temp_hash_array[i]);
      }
    }

    delete[] temp_hash_array;
    return;
  }

  void init_hash_array()
  {
    T _first= first.ptr();
    T _second= second;

    capacity= CAPACITY_INITIAL;
    size= 0;
    hash_array= new T[capacity]{};

    insert_helper(get_key(_first), _first);
    insert_helper(get_key(_second), _second);
  }

public:
  T find(const T &elem)
  {
    return find(key_trait::get_key(elem),
                [&elem](const T &rhs) { return rhs == elem; });
  }

  template <typename Func>
  T find(const MDL_key *mdl_key, const Func &elem_suits)
  {
    if (first.mark())
    {
      if (first.ptr() && elem_suits(first.ptr()))
        return first.ptr();
      if (second != nullptr && elem_suits(second))
        return second;

      return nullptr;
    }

    for (auto key= to_index(mdl_key->tc_hash_value());
         hash_array[key] != nullptr; key= to_index(key + 1))
    {
      if (elem_suits(hash_array[key]))
        return hash_array[key];
    }

    return nullptr;
  };

  bool erase(const erase_type &value)
  {
    if (first.mark())
    {
      if (first.ptr() != nullptr && trait::is_equal(first.ptr(), value))
      {
        first.set_ptr(nullptr);
        return true;
      }
      else if (second && trait::is_equal(second, value))
      {
        second= nullptr;
        return true;
      }
      else
      {
        return false;
      }
    }

    if (capacity > 7 && static_cast<double>(size - 1) <
                            LOW_LOAD_FACTOR * static_cast<double>(capacity))
      rehash(0.5 * capacity);

    return erase_helper(value);
  }

  bool insert(key_type *mdl_key, T value)
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
        init_hash_array();
      }
    }

    if (size + 1 > MAX_LOAD_FACTOR * capacity)
      rehash(capacity << 1);

    return insert_helper(mdl_key, value);
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

    for (uint i= 0; i < capacity; i++)
    {
      hash_array[i]= nullptr;
    }

    capacity= CAPACITY_INITIAL;
    return true;
  }

private:
  static constexpr uint power2_start= 2;
  static constexpr uint CAPACITY_INITIAL= 1 << power2_start;
  static constexpr double MAX_LOAD_FACTOR= 0.5f;
  static constexpr double LOW_LOAD_FACTOR= 0.1f;

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
      uint32 size;
      uint32 capacity;
    };
  };
};

#endif