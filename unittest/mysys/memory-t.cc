/* Copyright (c) 2003, 2006, 2007 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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

#include <tap.h>
#include <my_global.h>
#include <my_sys.h>
#include "memory_helpers.h"


int main(int argc __attribute__((unused)), char *argv[])
{
  MY_INIT(argv[0]);

  plan(43);

  /* Tests for Shared_ptr class */

  // Empty Shared pointer
  Shared_ptr<int> p1;
  ok(static_cast<bool>(p1) == false, "p1 is empty");
  ok(p1.use_count() == 0, "p1.use_count() == 0");
  ok(p1.get() == nullptr, "p1 is NULL");

  Shared_ptr<int> p2(new int(12345));
  ok(static_cast<bool>(p2) == true, "p2 is not empty");
  ok(p2.use_count() == 1, "p2.use_count() == 1");
  ok(*p2.get() == 12345, "p2 == 12345, obtaining value with .get()");
  ok(*p2 == 12345, "p2 == 12345, obtaining value with operator*");

  // Copy-construction, same value shared among p2 and p3
  auto p3(p2);
  ok(*p3.get() == 12345, "p3 == 12345");
  ok(p2.use_count() == 2, "p2.use_count() == 2");
  ok(p3.use_count() == 2, "p3.use_count() == 2");
  *p3= 888;
  // Value has changed for both p2 and p3
  ok(*p2 == 888, "p2 == 888 after value change");
  ok(*p3== 888, "p3 == 888 after value change");

  // Reset to an empty shared pointer
  p2.reset();
  ok(p2.get() == nullptr, "p2 is NULL");
  // p3 now owns the object
  ok(*p3 == 888, "p3 == 888");
  ok(p3.use_count() == 1, "p3.use_count() == 1");

  // Copy-construction using assignment
  auto p4= p3;
  ok(*p4 == 888, "p4 == 888");
  ok(p4.use_count() == 2, "p4.use_count() == 2");
  ok(p3 == p4, "p3 == p4");
  ok(p4 == p3, "p4 == p3");

  Shared_ptr<int> p5(new int(98765));
  // Re-assignment
  p4= p5;
  ok(p3.use_count() == 1, "p3.use_count() == 1");
  ok(p4.use_count() == 2, "p4.use_count() == 2");
  ok(*p4 == 98765, "p4 == 98765");
  ok(*p5 == 98765, "p5 == 98765");

  // Move-construction
  Shared_ptr<int> p6(std::move(p4));
  ok(*p6.get() == 98765, "p6 == 98765");

  // Move-assignment
  Shared_ptr<int> p7;
  p7= std::move(p3);
  ok(p7.use_count() == 1, "p7.use_count() == 1");
  ok(*p7.get() == 888, "p7 == 888");

  // Reset to new value
  p7.reset(new int(777));
  ok(*p7 == 777, "p7 == 777 after reset to new value");
  ok(p7 != p6, "p7 != p6");

  Shared_ptr<char> p8(new char('a')), p9(new char('b'));
  p8.swap(p9);
  ok(*p8 == 'b', "p8 == 'b' after swap");
  ok(*p9 == 'a', "p9 == 'a' after swap");

  Shared_ptr<long> p10(new long(10));
  {
    // Make copy of p10
    Shared_ptr<long> p11(p10);
    ok(p10.use_count() == 2, "p10.use_count() == 2");
    ok(p11.use_count() == 2, "p11.use_count() == 2");
    // Now p11 will be destroyed
  }
  ok(p10.use_count() == 1, "p10.use_count() == 1 after p11 destruction");

  {
    // Test for LeakSanitizer: memory must be freed upon the shared pointer
    // destruction
    Shared_ptr<long long> p12(new long long(123123123123));
  }

  // Shared pointers owning nullptr's
  {
    Shared_ptr<long> p13(nullptr);
    ok(!p13, "!p13");
    ok(p13 == nullptr, "p13 == nullptr");
    ok(p13.get() == nullptr, "p13.get() == nullptr");
    auto p14= p13; // copy-construction
    ok(p13 == p14, "p13 == p14");
    Shared_ptr<long> p15(nullptr);
    p14= p15; // copy assignment
    ok(!p14, "!p14");
    ok(p14.get() == nullptr, "p14.get() == nullptr");
    auto p16(std::move(p14)); // move-construction
    ok(!p16, "!p16");
    ok(p16.get() == nullptr, "p16.get() == nullptr");
    p14= std::move(p16); // move-assignment
    ok(!p14, "!p14");
    ok(p14.get() == nullptr, "p14.get() == nullptr");
  } // Destructors work correctly

  my_end(0);
  return exit_status();
}
