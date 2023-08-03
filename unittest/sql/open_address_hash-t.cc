#include <my_global.h>
#include <tap.h>

#include "open_address_hash.h"
uint32 unary_hf(uint32 key)
{
  return key;
}

struct test_key_trait
{
  using hash_value_type= uint32;
  using key_type= hash_value_type;

  static uint32 get_key(const key_type *elem) { return *elem; }
  static hash_value_type get_hash_value(const key_type* elem) { return *elem; }
};

template<typename T>
struct pointer_trait
{
  using elem_type= T*;
  using find_type= T*;
  using erase_type= T*;
  static bool is_equal(const T *lhs, const T *rhs)
  {
    return lhs == rhs;
  }
  static bool is_empty(const elem_type el) { return el == nullptr; }
  static void set_null(elem_type &el) { el= nullptr; }
};

struct test_value_trait: public pointer_trait<uint32>
{
  template <typename key_type>
  static const key_type *get_key(const uint32 *elem) { return elem; }
  static uint32 get_hash_value(const uint32* elem) { return *elem; }
};

open_address_hash<test_key_trait, test_value_trait> hashie;

uint32 data[4][16]= {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
};

int main(int argc __attribute__((unused)),char *argv[])
{
  plan(123); // xz 4to eto

  auto *found= hashie.find(data[0]);
  ok(found == nullptr, "Something found in a empty hash!");
  hashie.insert(data[0] + 1); // 1

  found= hashie.find(data[1] + 1);
  ok(found == nullptr, "wrong val with key=1 is found");

  found= hashie.find(data[0] + 1);
  ok(*found == 1, "1 is not found");


  // expand
  hashie.insert(data[0]+4);
  ok(hashie.size() == 2, "wrong size");
  ok(hashie.buffer_size() == 0, "two elements, why buffer?");
  hashie.insert(data[0]+5);
  ok(hashie.size() == 3, "wrong size, %u", hashie.size());

  // collision
  hashie.insert(data[1] + 1); // 1
  auto found2= hashie.find(data[1] + 1);
  ok(found2 != found && *found == *found2, "collision misbehavior");


  //expand on special occasion (offset elements to the beginning)
  hashie.clear();
  hashie.insert(data[0]+14);
  hashie.insert(data[0] + 15);
  hashie.insert(data[1] + 15);
  hashie.insert(data[1] + 14);
  hashie.insert(data[2] + 15);
  hashie.insert(data[2] + 14);
  hashie.insert(data[0] + 1);
  hashie.insert(data[3] + 14);
  hashie.insert(data[0]+2);
  hashie.insert(data[0] + 3);
  ok(hashie.find(data[0]+14) != nullptr, "expand misbehavior");
  ok(hashie.find(data[0] + 15) != nullptr, "expand misbehavior");
  ok(hashie.find(data[1] + 15) != nullptr, "expand misbehavior");
  ok(hashie.find(data[1] + 14) != nullptr, "expand misbehavior");
  ok(hashie.find(data[2] + 15) != nullptr, "expand misbehavior");
  ok(hashie.find(data[2] + 14) != nullptr, "expand misbehavior");
  ok(hashie.find(data[3] + 14) != nullptr, "expand misbehavior");
  ok(hashie.find(data[0] + 1) != nullptr, "expand misbehavior");
  ok(hashie.find(data[0] + 2) != nullptr, "expand misbehavior");
  ok(hashie.find(data[0] + 3) != nullptr, "expand misbehavior");
}

