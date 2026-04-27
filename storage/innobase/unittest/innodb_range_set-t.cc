/* Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "tap.h"
#include "fil0fil.h"

#include <vector>
#include <utility>

/* Stubs for InnoDB symbols pulled in via univ.i. */
const size_t alloc_max_retries= 0;
void ut_dbg_assertion_failed(const char *, const char *, unsigned)
{ abort(); }
#ifdef UNIV_PFS_MEMORY
PSI_memory_key mem_key_other, mem_key_std;
PSI_memory_key ut_new_get_key_by_file(uint32_t) { return mem_key_std; }
#endif

/** @return a vector of (first, last) pairs representing the ranges in set. */
static std::vector<std::pair<uint32_t, uint32_t>> dump(range_set &set)
{
  std::vector<std::pair<uint32_t, uint32_t>> out;
  for (auto it= set.begin(); it != set.end(); ++it)
    out.emplace_back(it->first, it->last);
  return out;
}

/** @return true iff set contains exactly the ranges in expected. */
static bool equals(range_set &set,
                   std::vector<std::pair<uint32_t, uint32_t>> expected)
{
  return dump(set) == expected;
}

static void test_empty()
{
  range_set s;
  ok(s.empty(), "empty");
  ok(s.size() == 0, "size=0");
  ok(s.begin() == s.end(), "begin==end");
  ok(!s.contains(0), "empty: !contains(0)");
  ok(!s.contains(UINT32_MAX), "empty: !contains(UINT32_MAX)");
}

static void test_add_value()
{
  range_set s;
  s.add_value(5);
  ok(!s.empty(), "add_value: !empty");
  ok(s.size() == 1, "add_value: size=1");
  ok(s.contains(5), "add_value: contains(5)");
  ok(!s.contains(4), "add_value: !contains(4)");
  ok(!s.contains(6), "add_value: !contains(6)");
  ok(equals(s, {{5, 5}}), "add_value: [5,5]");

  /* Adjacent after -> merge */
  s.add_value(6);
  ok(equals(s, {{5, 6}}), "add_value adjacent after: [5,6]");

  /* Adjacent before -> merge */
  s.add_value(4);
  ok(equals(s, {{4, 6}}), "add_value adjacent before: [4,6]");

  /* Duplicate -> no change */
  s.add_value(5);
  ok(equals(s, {{4, 6}}), "add_value duplicate: [4,6]");

  /* Gap -> new range */
  s.add_value(10);
  ok(equals(s, {{4, 6}, {10, 10}}), "add_value gap: [4,6],[10,10]");

  /* Closes gap with single value -> merge */
  s.add_value(7);
  ok(equals(s, {{4, 7}, {10, 10}}), "add_value shrinks gap");
  s.add_value(9);
  ok(equals(s, {{4, 7}, {9, 10}}), "add_value shrinks gap again");
  s.add_value(8);
  ok(equals(s, {{4, 10}}), "add_value bridges gap: [4,10]");
}

static void test_add_range_disjoint()
{
  range_set s;
  s.add_range({10, 20});
  s.add_range({30, 40});
  s.add_range({50, 60});
  ok(equals(s, {{10, 20}, {30, 40}, {50, 60}}),
     "disjoint insertion preserves all ranges");
}

static void test_add_range_extending_existing()
{
  /* This is the exact bug that was fixed: [64,281] + [281,282] must
  produce [64,282], not [64,281]. */
  range_set s;
  s.add_range({64, 281});
  s.add_range({281, 282});
  ok(equals(s, {{64, 282}}),
     "regression: [64,281] + [281,282] -> [64,282]");

  s.clear();
  s.add_range({64, 281});
  s.add_range({100, 400});
  ok(equals(s, {{64, 400}}), "overlap extending last: [64,400]");

  s.clear();
  s.add_range({100, 200});
  s.add_range({50, 150});
  ok(equals(s, {{50, 200}}), "overlap extending first: [50,200]");

  s.clear();
  s.add_range({100, 200});
  s.add_range({50, 250});
  ok(equals(s, {{50, 250}}), "new range contains existing: [50,250]");

  s.clear();
  s.add_range({50, 250});
  s.add_range({100, 200});
  ok(equals(s, {{50, 250}}), "existing contains new: no change");
}

static void test_add_range_adjacent()
{
  range_set s;
  s.add_range({10, 20});
  s.add_range({21, 30});
  ok(equals(s, {{10, 30}}), "adjacent after: [10,30]");

  s.clear();
  s.add_range({21, 30});
  s.add_range({10, 20});
  ok(equals(s, {{10, 30}}), "adjacent before: [10,30]");
}

static void test_add_range_absorbs_multiple()
{
  /* A single new_range spanning several existing ranges must absorb them all. */
  range_set s;
  s.add_range({10, 12});
  s.add_range({20, 22});
  s.add_range({30, 32});
  s.add_range({40, 42});
  ok(equals(s, {{10, 12}, {20, 22}, {30, 32}, {40, 42}}),
     "setup: 4 disjoint ranges");
  s.add_range({11, 41});
  ok(equals(s, {{10, 42}}), "absorbs all four into [10,42]");

  /* Adjacency-only absorption across multiple ranges. */
  s.clear();
  s.add_range({10, 14});
  s.add_range({20, 24});
  s.add_range({30, 34});
  s.add_range({15, 29});
  ok(equals(s, {{10, 34}}), "adjacency bridges three ranges into [10,34]");
}

static void test_add_range_at_first_position()
{
  range_set s;
  s.add_range({100, 200});
  s.add_range({10, 20});
  ok(equals(s, {{10, 20}, {100, 200}}), "new range before everything");

  s.clear();
  s.add_range({100, 200});
  s.add_range({10, 99});
  ok(equals(s, {{10, 200}}), "adjacent-before merges first range");
}

static void test_add_range_at_last_position()
{
  range_set s;
  s.add_range({10, 20});
  s.add_range({100, 200});
  ok(equals(s, {{10, 20}, {100, 200}}), "new range after everything");

  s.clear();
  s.add_range({10, 20});
  s.add_range({21, 30});
  s.add_range({31, 40});
  ok(equals(s, {{10, 40}}), "chain of adjacent additions: [10,40]");
}

static void test_add_range_edge_values()
{
  range_set s;
  s.add_range({0, 0});
  ok(equals(s, {{0, 0}}), "add {0,0}");
  s.add_range({1, 5});
  ok(equals(s, {{0, 5}}), "adjacent to zero: [0,5]");

  s.clear();
  s.add_range({UINT32_MAX - 2, UINT32_MAX});
  ok(equals(s, {{UINT32_MAX - 2, UINT32_MAX}}), "add near UINT32_MAX");
  /* Adjacent before (must not overflow) */
  s.add_range({UINT32_MAX - 10, UINT32_MAX - 3});
  ok(equals(s, {{UINT32_MAX - 10, UINT32_MAX}}), "merge near UINT32_MAX");
}

static void test_remove_value()
{
  range_set s;
  s.add_range({10, 20});

  /* Remove at first boundary */
  s.remove_value(10);
  ok(equals(s, {{11, 20}}), "remove_value first: [11,20]");

  /* Remove at last boundary */
  s.remove_value(20);
  ok(equals(s, {{11, 19}}), "remove_value last: [11,19]");

  /* Remove in the middle splits */
  s.remove_value(15);
  ok(equals(s, {{11, 14}, {16, 19}}), "remove_value middle splits");

  /* Remove non-existent value is a no-op */
  s.remove_value(15);
  ok(equals(s, {{11, 14}, {16, 19}}), "remove_value non-existent no-op");

  /* Remove last value in singleton range erases it */
  s.clear();
  s.add_value(7);
  s.remove_value(7);
  ok(s.empty(), "remove_value from singleton empties set");
}

static void test_remove_if_exists()
{
  range_set s;
  s.add_range({10, 20});
  ok(s.remove_if_exists(15), "remove_if_exists present returns true");
  ok(!s.remove_if_exists(100), "remove_if_exists absent returns false");
  ok(equals(s, {{10, 14}, {16, 20}}), "state after remove_if_exists");
}

static void test_contains()
{
  range_set s;
  s.add_range({10, 20});
  ok(!s.contains(9), "!contains before");
  ok(s.contains(10), "contains first");
  ok(s.contains(15), "contains middle");
  ok(s.contains(20), "contains last");
  ok(!s.contains(21), "!contains after");
  s.add_range({30, 40});
  ok(s.contains(10), "after 2nd add, contains first");
  ok(s.contains(15), "after 2nd add, contains middle");
  ok(s.contains(20), "after 2nd add, contains last");
  ok(!s.contains(9), "after 2nd add, !contains before");
  ok(!s.contains(25), "after 2nd add, !contains gap");
  ok(s.contains(35), "after 2nd add, contains second range");
  ok(!s.contains(41), "after 2nd add, !contains after");
}

static void test_clear()
{
  range_set s;
  s.add_range({10, 20});
  s.add_range({30, 40});
  s.clear();
  ok(s.empty(), "clear makes it empty");
  ok(s.size() == 0, "clear: size=0");
}

static void test_large_workload()
{
  /* Insert many scattered values, then bridge them with a big range. */
  range_set s;
  for (uint32_t v= 0; v < 1000; v+= 3)
    s.add_value(v);
  ok(s.size() == 334, "scattered single-value inserts stay un-merged");

  /* Fill the gaps with single values and verify everything collapses
  into a single range. */
  for (uint32_t v= 1; v < 1000; v+= 3)
    s.add_value(v);
  for (uint32_t v= 2; v < 1000; v+= 3)
    s.add_value(v);
  ok(equals(s, {{0, 999}}), "filling gaps collapses into one range");
}

int main()
{
  plan(61);
  test_empty();
  test_add_value();
  test_add_range_disjoint();
  test_add_range_extending_existing();
  test_add_range_adjacent();
  test_add_range_absorbs_multiple();
  test_add_range_at_first_position();
  test_add_range_at_last_position();
  test_add_range_edge_values();
  test_remove_value();
  test_remove_if_exists();
  test_contains();
  test_clear();
  test_large_workload();
  return exit_status();
}
