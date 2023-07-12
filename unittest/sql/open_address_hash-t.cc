#include <my_global.h>
#include <tap.h>

#include "open_address_hash.h"

struct identity_key_trait
{
  using Hash_value_type= uint32;
  using Key_type= Hash_value_type;

  static Key_type *get_key(Key_type *elem) { return elem; }
  static Hash_value_type get_hash_value(const Key_type* elem) { return *elem; }
};

uint32 data[4][16]= {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
};

static void test_pointer_hash_table_with_pointer_equality()
{
  Open_address_hash<uint32, uint32*, identity_key_trait> hashie;

  auto *found= hashie.find(data[0]);
  ok(found == nullptr, "something found in a empty hash!");

  // Insert/delete into
  ok(!hashie.erase(data[0]), "deletion unexpectedly worked out!");
  ok(hashie.insert(data[0] + 1), "insertion into empty table failed");
  ok(!hashie.erase(data[0]), "deletion unexpectedly worked out!");
  ok(hashie.erase(data[0] + 1), "deletion failed");
  ok(!hashie.erase(data[0] + 1), "deletion unexpectedly worked out!");
  ok(hashie.insert(data[0] + 1), "insertion into empty table failed");
  ok(hashie.insert(data[0] + 2), "insertion failed");
  ok(hashie.find(data[0] + 1) == data[0] + 1, "find failed");
  ok(hashie.erase(data[0] + 1), "deletion failed");
  ok(hashie.find(data[0] + 1) == nullptr, "find after delete succeeded");
  ok(hashie.find(data[0] + 2) == data[0] + 2, "find failed");

  ok(hashie.insert(data[0] + 1), "insertion failed");
  ok(hashie.size() == 2, "wrong size");
  ok(hashie.erase(data[0] + 1), "deletion failed");
  ok(hashie.find(data[0] + 2) == data[0] + 2,
                 "find find of second element after delete of first failed");

  ok(hashie.insert(data[0] + 1), "insertion into empty table failed");
  found= hashie.find(data[1] + 1);
  ok(found == nullptr, "wrong val with key=1 is found");
  ok(hashie.erase(data[0] + 2), "deletion failed");

  found= hashie.find(data[0] + 1);
  ok(found && *found == 1, "1 is not found");


  // Expand
  hashie.insert(data[0]+4);
  ok(hashie.size() == 2, "wrong size");
  ok(hashie.buffer_size() == 0, "two elements, why buffer?");
  hashie.insert(data[0]+5);
  ok(hashie.size() == 3, "wrong size, %lu", hashie.size());

  // Collision
  hashie.insert(data[1] + 1); // 1
  ok(!hashie.insert(data[1] + 1), "collision is not detected.");
  auto found2= hashie.find(data[1] + 1);
  ok(found2 != found && *found == *found2, "collision misbehavior");


  // Expand on special occasion (offset elements to the beginning)
  hashie.clear();
  hashie.insert(data[0] + 14);
  hashie.insert(data[0] + 15);
  hashie.insert(data[1] + 15);
  hashie.insert(data[1] + 14);
  hashie.insert(data[2] + 15);
  hashie.insert(data[2] + 14);
  hashie.insert(data[0] + 1);
  hashie.insert(data[3] + 14);
  hashie.insert(data[0] + 2);
  hashie.insert(data[0] + 3);
  ok(hashie.find(data[0] + 14) != nullptr, "expand misbehavior");
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

struct pointer_value_equality_trait:
        public traits::Open_address_hash_value_trait<uint32*>
{
  static bool is_equal(const uint32 *lhs, const uint32 *rhs)
  {
    return lhs == rhs || (lhs != nullptr && rhs != nullptr && *lhs == *rhs);
  }
};

static void test_hash_table_with_value_equality()
{
  Open_address_hash<uint32, uint32*,
                    identity_key_trait,
                    pointer_value_equality_trait> hashie;
  ok(hashie.size() == 0, "hashie is not empty!");
  ok(hashie.insert(data[0]), "insert to empty hash failed");
  ok(!hashie.insert(data[0]), "collision insert succeeded");
  ok(!hashie.insert(data[1]), "insert of the same value succeeded");
  ok(hashie.find(data[0]) != nullptr, "item not found");
  ok(hashie.insert(data[0] + 2), "insert to hash failed");
  ok(hashie.insert(data[0] + 3), "insert to hash failed");
  ok(hashie.insert(data[0] + 4), "insert to hash failed");
  ok(hashie.insert(data[0] + 5), "insert to hash failed");
  ok(hashie.insert(data[0] + 6), "insert to hash failed");
  ok(hashie.insert(data[0] + 7), "insert to hash failed");
  ok(hashie.find(data[0] + 2) != nullptr, "item not found");
  ok(hashie.find(data[0] + 3) != nullptr, "item not found");
  ok(hashie.find(data[0] + 4) != nullptr, "item not found");
  ok(hashie.find(data[0] + 8) == nullptr, "item unexpectedly found");
}


int main(int argc __attribute__((unused)),char *argv[])
{
  plan(50);

  test_pointer_hash_table_with_pointer_equality();
  test_hash_table_with_value_equality();

  return 0;
}
