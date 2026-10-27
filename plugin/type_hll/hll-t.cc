/* Copyright (c) 2026, drrtuy@gmail.com
   Copyright (c) 2026, MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/* Unit tests for the HLL v1 codec and algorithm (plan sections 13.1, 13.2) */

#include "hll_codec.h"
#include <tap.h>
#include <cmath>
#include <vector>

typedef std::vector<uchar> Bytes;

static uchar buf[HLL_MAX_ENCODED_LENGTH];

static Bytes encode(const Hll_state &s)
{
  size_t len= Hll_codec::encode(s, HLL_FORMAT_V1, buf);
  return Bytes(buf, buf + len);
}

static bool rejects(const Bytes &b)
{
  return Hll_codec::decode(b.data(), b.size(), NULL);
}

static bool round_trips(const Bytes &b)
{
  static Hll_state s;
  return !Hll_codec::decode(b.data(), b.size(), &s) && encode(s) == b;
}

static uint64 splitmix64(uint64 *x)
{
  uint64 z= (*x+= 0x9E3779B97F4A7C15ULL);
  z= (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z= (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static Hll_state *build(uint64 seed, uint count)
{
  Hll_state *s= new Hll_state();
  for (uint i= 0; i < count; i++)
    s->add(splitmix64(&seed));
  return s;
}

static Bytes merged(const Hll_state &a, const Hll_state &b)
{
  Hll_state *m= new Hll_state(a);
  m->merge(b);
  Bytes r= encode(*m);
  delete m;
  return r;
}

static Bytes be64(uint64 v)
{
  Bytes b(8);
  for (int i= 7; i >= 0; i--, v>>= 8)
    b[i]= (uchar) v;
  return b;
}

static Bytes explicit_value(const std::vector<uint64> &hashes)
{
  Bytes b(1, 0x12);
  for (uint64 h : hashes)
  {
    Bytes e= be64(h);
    b.insert(b.end(), e.begin(), e.end());
  }
  return b;
}


static void test_empty()
{
  static Hll_state s;
  ok(encode(s) == Bytes{0x11}, "EMPTY encodes as 0x11");
  ok(round_trips(Bytes{0x11}), "EMPTY round trip");
  ok(s.cardinality() == 0, "EMPTY cardinality is 0");
}


static void test_explicit()
{
  static Hll_state s;
  s.add(2);
  s.add(1);
  s.add(2);
  ok(encode(s) == explicit_value({1, 2}),
     "EXPLICIT: sorted unique big-endian hashes");
  ok(s.cardinality() == 2, "EXPLICIT cardinality is exact");

  Hll_state *one= build(1, 1);
  ok(encode(*one).size() == 9 && round_trips(encode(*one)),
     "EXPLICIT with 1 hash: 9 bytes, round trip");
  Hll_state *full= build(1, 256);
  ok(full->repr == Hll_state::EXPLICIT && encode(*full).size() == 2049 &&
     round_trips(encode(*full)) && full->cardinality() == 256,
     "EXPLICIT with 256 hashes: 2049 bytes, round trip, exact");
  delete one;
  delete full;
}


static void test_promotion()
{
  Hll_state *s= build(7, 257);
  Bytes b= encode(*s);
  ok(s->repr == Hll_state::FULL && b.size() == 12289 && b[0] == 0x13,
     "257th hash promotes to FULL (12289 bytes)");
  ok(round_trips(b), "FULL round trip");

  /* Promotion replays the explicit hashes into registers */
  Hll_state *direct= new Hll_state();
  direct->repr= Hll_state::FULL;
  memset(direct->regs, 0, sizeof(direct->regs));
  uint64 seed= 7;
  for (uint i= 0; i < 257; i++)
    direct->add(splitmix64(&seed));
  ok(encode(*direct) == b, "promoted FULL equals FULL built directly");
  delete direct;
  delete s;
}


static void test_packing()
{
  Hll_state *s= new Hll_state();
  s->repr= Hll_state::FULL;
  memset(s->regs, 0, sizeof(s->regs));
  s->regs[0]= 51;
  s->regs[1]= 1;
  s->regs[2]= 2;
  s->regs[3]= 3;
  s->regs[HLL_M - 1]= 51;
  Bytes b= encode(*s);
  ok(b[1] == 0x73 && b[2] == 0x20 && b[3] == 0x0c && b[4] == 0,
     "FULL packing across byte boundaries (registers 0..3)");
  ok(b[12288] == 0xcc, "FULL packing of the last register");
  ok(round_trips(b), "FULL packing round trip");
  delete s;
}


static void test_add_rank()
{
  Hll_state *s= new Hll_state();
  s->repr= Hll_state::FULL;
  memset(s->regs, 0, sizeof(s->regs));
  s->add(5);                                     // index 5, tail 0 -> 51
  s->add((1ULL << 14) | 6);                      // index 6, tail 1 -> 1
  s->add((8ULL << 14) | 7);                      // index 7, tail 8 -> 4
  ok(s->regs[5] == 51 && s->regs[6] == 1 && s->regs[7] == 4,
     "register index and rank (zero tail gives rank 51)");
  delete s;
}


static void test_negative()
{
  Bytes full(12289, 0);
  full[0]= 0x13;
  Bytes bad_reg= full;
  bad_reg[1]= 52;                                // register 0 = 52
  std::vector<uint64> h257;
  for (uint64 i= 1; i <= 257; i++)
    h257.push_back(i);

  ok(rejects(Bytes{}), "reject zero-length value");
  ok(rejects(Bytes{0x21}), "reject unknown version");
  ok(rejects(Bytes{0x01}), "reject version 0");
  ok(rejects(Bytes{0x10}), "reject reserved representation 0");
  ok(rejects(Bytes{0x14}), "reject reserved representation 4");
  ok(rejects(Bytes{0x11, 0x00}), "reject EMPTY with payload");
  ok(rejects(Bytes{0x12}), "reject EXPLICIT without payload");
  ok(rejects(Bytes{0x12, 1, 2, 3, 4, 5, 6, 7}),
     "reject EXPLICIT with length not multiple of 8");
  ok(rejects(explicit_value({5, 5})), "reject EXPLICIT duplicate hashes");
  ok(rejects(explicit_value({6, 5})), "reject EXPLICIT descending hashes");
  ok(rejects(explicit_value(h257)), "reject EXPLICIT with 257 hashes");
  ok(rejects(Bytes(full.begin(), full.end() - 1)), "reject short FULL");
  Bytes long_full= full;
  long_full.push_back(0);
  ok(rejects(long_full), "reject long FULL");
  ok(rejects(bad_reg), "reject FULL register > 51");
  ok(!rejects(full), "accept FULL with registers in range");
}


static void test_merge()
{
  static const Hll_state empty;
  /* Sizes around the promotion threshold, with overlap */
  static const uint sizes[][2]= {{0, 0}, {0, 10}, {100, 155}, {128, 129},
                                 {200, 200}, {10, 300}, {300, 3000}};
  bool all_ok= true;
  for (auto &sz : sizes)
  {
    Hll_state *a= build(11, sz[0]);
    Hll_state *b= build(11, sz[1]);              // shares a prefix with a
    Hll_state *c= build(99, 77);
    Hll_state *u= build(11, std::max(sz[0], sz[1]));
    Bytes ab= merged(*a, *b);
    all_ok&= ab == merged(*b, *a);               // commutative
    all_ok&= merged(*a, *a) == encode(*a);       // idempotent
    all_ok&= ab == encode(*u);                   // build(A u B) = A merge B
    Hll_state *ab_s= new Hll_state(*a);
    ab_s->merge(*b);
    Hll_state *bc_s= new Hll_state(*b);
    bc_s->merge(*c);
    all_ok&= merged(*ab_s, *c) == merged(*a, *bc_s);   // associative
    all_ok&= merged(*a, empty) == encode(*a);    // identity
    delete a; delete b; delete c; delete u; delete ab_s; delete bc_s;
  }
  ok(all_ok, "merge is commutative, associative, idempotent and "
             "equals build of the union");
}


static void test_cardinality()
{
  static const uint sizes[]= {1000, 10000, 100000, 1000000};
  for (uint n : sizes)
  {
    Hll_state *s= build(42, n);
    double err= std::fabs(s->cardinality() - n) / n;
    /* 4 standard errors of 1.04/sqrt(2^14) */
    ok(err < 4 * 0.008125, "cardinality %u within 4 sigma (error %.4f)",
       n, err);
    delete s;
  }
}


int main()
{
  plan(34);
  test_empty();
  test_explicit();
  test_promotion();
  test_packing();
  test_add_rank();
  test_negative();
  test_merge();
  test_cardinality();
  return exit_status();
}
