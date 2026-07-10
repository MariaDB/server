/*
  Unit tests for fiber_context — the coroutine / fiber primitives
  used by DuckDB cross-engine predicate pushdown.

  Build (standalone, from the runtime/ directory):
    cc -c fiber_context.c -o fiber_context.o
    c++ -std=c++17 fiber_context_test.cc fiber_context.o -o fiber_context_test
    ./fiber_context_test

  Or via CMake:  see the ADD_EXECUTABLE block in CMakeLists.txt
*/

#include "fiber_context.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#define FIBER_STACK_SIZE (64 * 1024)

/* ----------------------------------------------------------------
   Helpers
   ---------------------------------------------------------------- */

static int test_count = 0;
static int pass_count = 0;

#define TEST(name) \
  static void test_##name(); \
  static struct Register_##name { \
    Register_##name() { tests.push_back({#name, test_##name}); } \
  } reg_##name; \
  static void test_##name()

#define EXPECT(cond) do { \
    test_count++; \
    if (!(cond)) { \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } else { \
      pass_count++; \
    } \
  } while(0)

struct TestEntry {
  const char *name;
  void (*func)();
};
static std::vector<TestEntry> tests;

/* ----------------------------------------------------------------
   Test 1: init / destroy — basic lifecycle
   ---------------------------------------------------------------- */
TEST(init_destroy)
{
  struct fiber_context ctx;
  memset(&ctx, 0xAB, sizeof(ctx));   /* poison */

  int rc = fiber_context_init(&ctx, FIBER_STACK_SIZE);
  EXPECT(rc == 0);

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   Test 2: spawn a fiber that runs to completion without yielding
   ---------------------------------------------------------------- */
static int simple_run_flag = 0;

static void simple_func(void *arg)
{
  simple_run_flag = *(int *)arg;
}

TEST(spawn_no_yield)
{
  struct fiber_context ctx;
  EXPECT(fiber_context_init(&ctx, FIBER_STACK_SIZE) == 0);

  simple_run_flag = 0;
  int val = 42;

  /* spawn should return 0 when the user function completes without yielding */
  int rc = fiber_context_spawn(&ctx, simple_func, &val);
  EXPECT(rc == 0);
  EXPECT(simple_run_flag == 42);

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   Test 3: spawn + single yield + continue → completion
   ---------------------------------------------------------------- */
struct single_yield_data {
  struct fiber_context *ctx;
  int phase;
};

static void single_yield_func(void *arg)
{
  auto *d = (single_yield_data *)arg;
  d->phase = 1;
  fiber_context_yield(d->ctx);
  d->phase = 2;
}

TEST(single_yield)
{
  struct fiber_context ctx;
  EXPECT(fiber_context_init(&ctx, FIBER_STACK_SIZE) == 0);

  single_yield_data d{&ctx, 0};

  int rc = fiber_context_spawn(&ctx, single_yield_func, &d);
  EXPECT(rc == 1);         /* 1 = suspended */
  EXPECT(d.phase == 1);

  rc = fiber_context_continue(&ctx);
  EXPECT(rc == 0);         /* 0 = completed */
  EXPECT(d.phase == 2);

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   Test 4: multiple yields — simulate chunk-based streaming
   ---------------------------------------------------------------- */
struct multi_yield_data {
  struct fiber_context *ctx;
  int chunks_produced;
  static constexpr int TOTAL_CHUNKS = 5;
};

static void multi_yield_func(void *arg)
{
  auto *d = (multi_yield_data *)arg;
  for (int i = 0; i < multi_yield_data::TOTAL_CHUNKS; i++)
  {
    d->chunks_produced++;
    fiber_context_yield(d->ctx);
  }
}

TEST(multi_yield)
{
  struct fiber_context ctx;
  EXPECT(fiber_context_init(&ctx, FIBER_STACK_SIZE) == 0);

  multi_yield_data d{&ctx, 0};

  int rc = fiber_context_spawn(&ctx, multi_yield_func, &d);
  EXPECT(rc == 1);
  EXPECT(d.chunks_produced == 1);

  for (int i = 2; i <= multi_yield_data::TOTAL_CHUNKS; i++)
  {
    rc = fiber_context_continue(&ctx);
    EXPECT(rc == 1);         /* still suspended */
    EXPECT(d.chunks_produced == i);
  }

  /* One more continue — fiber function returns */
  rc = fiber_context_continue(&ctx);
  EXPECT(rc == 0);           /* completed */
  EXPECT(d.chunks_produced == multi_yield_data::TOTAL_CHUNKS);

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   Test 5: continue on a completed context returns 0
   ---------------------------------------------------------------- */
TEST(continue_after_done)
{
  struct fiber_context ctx;
  EXPECT(fiber_context_init(&ctx, FIBER_STACK_SIZE) == 0);

  int val = 1;
  int rc = fiber_context_spawn(&ctx, simple_func, &val);
  EXPECT(rc == 0);

  /* Calling continue on a finished fiber should return 0 (not crash). */
  rc = fiber_context_continue(&ctx);
  EXPECT(rc == 0);

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   Test 6: large stack usage — verify the fiber stack is adequate
   ---------------------------------------------------------------- */
static void deep_stack_func(void *arg)
{
  auto *d = (single_yield_data *)arg;
  /* Allocate ~16KB on the fiber stack. */
  volatile char buf[16384];
  memset((char *)buf, 0xCC, sizeof(buf));
  d->phase = (buf[0] == (char)0xCC && buf[16383] == (char)0xCC) ? 1 : -1;
  fiber_context_yield(d->ctx);
  d->phase = 2;
}

TEST(large_stack)
{
  struct fiber_context ctx;
  EXPECT(fiber_context_init(&ctx, FIBER_STACK_SIZE) == 0);

  single_yield_data d{&ctx, 0};

  int rc = fiber_context_spawn(&ctx, deep_stack_func, &d);
  EXPECT(rc == 1);
  EXPECT(d.phase == 1);

  rc = fiber_context_continue(&ctx);
  EXPECT(rc == 0);
  EXPECT(d.phase == 2);

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   Test 7: two independent fibers — verify no cross-contamination
   ---------------------------------------------------------------- */
struct two_fiber_data {
  struct fiber_context *ctx;
  int id;
  int phase;
};

static void two_fiber_func(void *arg)
{
  auto *d = (two_fiber_data *)arg;
  d->phase = d->id * 10 + 1;
  fiber_context_yield(d->ctx);
  d->phase = d->id * 10 + 2;
}

TEST(two_fibers)
{
  struct fiber_context ctx_a, ctx_b;
  EXPECT(fiber_context_init(&ctx_a, FIBER_STACK_SIZE) == 0);
  EXPECT(fiber_context_init(&ctx_b, FIBER_STACK_SIZE) == 0);

  two_fiber_data a{&ctx_a, 1, 0};
  two_fiber_data b{&ctx_b, 2, 0};

  /* Spawn A, it yields at phase 11 */
  int rc = fiber_context_spawn(&ctx_a, two_fiber_func, &a);
  EXPECT(rc == 1);
  EXPECT(a.phase == 11);

  /* Spawn B, it yields at phase 21 */
  rc = fiber_context_spawn(&ctx_b, two_fiber_func, &b);
  EXPECT(rc == 1);
  EXPECT(b.phase == 21);

  /* A's state is not corrupted by B */
  EXPECT(a.phase == 11);

  /* Resume B first */
  rc = fiber_context_continue(&ctx_b);
  EXPECT(rc == 0);
  EXPECT(b.phase == 22);

  /* Resume A — should still work */
  rc = fiber_context_continue(&ctx_a);
  EXPECT(rc == 0);
  EXPECT(a.phase == 12);

  fiber_context_destroy(&ctx_a);
  fiber_context_destroy(&ctx_b);
}

/* ----------------------------------------------------------------
   Test 8: reuse context — spawn again after completion
   ---------------------------------------------------------------- */
TEST(reuse_after_completion)
{
  struct fiber_context ctx;
  EXPECT(fiber_context_init(&ctx, FIBER_STACK_SIZE) == 0);

  single_yield_data d1{&ctx, 0};
  int rc = fiber_context_spawn(&ctx, single_yield_func, &d1);
  EXPECT(rc == 1 && d1.phase == 1);
  rc = fiber_context_continue(&ctx);
  EXPECT(rc == 0 && d1.phase == 2);

  /* Spawn again on the same context */
  single_yield_data d2{&ctx, 0};
  rc = fiber_context_spawn(&ctx, single_yield_func, &d2);
  EXPECT(rc == 1 && d2.phase == 1);
  rc = fiber_context_continue(&ctx);
  EXPECT(rc == 0 && d2.phase == 2);

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   Test 9: simulate DuckDB scan pattern — chunked row streaming
   ---------------------------------------------------------------- */
struct scan_sim_data {
  struct fiber_context *ctx;
  int total_rows;
  int chunk_size;
  int rows_produced;
  int yields;
};

static void scan_sim_func(void *arg)
{
  auto *d = (scan_sim_data *)arg;
  int count = 0;
  for (int i = 0; i < d->total_rows; i++)
  {
    d->rows_produced++;
    count++;
    if (count >= d->chunk_size)
    {
      d->yields++;
      fiber_context_yield(d->ctx);
      count = 0;
    }
  }
  /* Final partial chunk — no yield, just return */
}

TEST(scan_simulation)
{
  struct fiber_context ctx;
  EXPECT(fiber_context_init(&ctx, FIBER_STACK_SIZE) == 0);

  scan_sim_data d{&ctx, 1000, 128, 0, 0};

  int rc = fiber_context_spawn(&ctx, scan_sim_func, &d);
  int continues = 0;
  while (rc == 1)
  {
    continues++;
    rc = fiber_context_continue(&ctx);
  }
  EXPECT(rc == 0);
  EXPECT(d.rows_produced == 1000);
  EXPECT(d.yields == 7);       /* 1000/128 = 7 full chunks, remainder returns */
  EXPECT(continues == 7);

  fiber_context_destroy(&ctx);
}

/* ----------------------------------------------------------------
   main
   ---------------------------------------------------------------- */
int main()
{
  printf("fiber_context unit tests\n");
  printf("========================\n");

#ifdef FIBER_CONTEXT_DISABLE
  printf("SKIPPED: fiber context is disabled on this platform.\n");
  return 0;
#endif

  int failures = 0;
  for (auto &t : tests)
  {
    int before = test_count;
    int before_pass = pass_count;
    printf("  %-30s ", t.name);
    t.func();
    int ran = test_count - before;
    int passed = pass_count - before_pass;
    if (passed == ran)
      printf("OK (%d checks)\n", ran);
    else
    {
      printf("FAILED (%d/%d)\n", passed, ran);
      failures++;
    }
  }

  printf("------------------------\n");
  printf("%d/%d tests passed, %d/%d checks passed\n",
         (int)tests.size() - failures, (int)tests.size(),
         pass_count, test_count);

  return failures ? 1 : 0;
}
