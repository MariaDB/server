#include <stdint.h>
int printf (const char *, ...);
static int64_t safe_mul_func_int64_t_s_s (int64_t si1, int64_t si2) {
  return (((si1 > 0) && (si2 > 0) && (si1 > (INT64_MAX / si2)))
          || ((si1 > 0) && (si2 <= 0) && (si2 < (INT64_MIN / si1)))
          || ((si1 <= 0) && (si2 > 0) && (si1 < (INT64_MIN / si2)))
          || ((si1 <= 0) && (si2 <= 0) && (si1 != 0) && (si2 < (INT64_MAX / si1))))
           ? (si1)
           : si1 * si2;
}
struct {
  uint8_t a;
} b[], e;
uint8_t c[][8];
int32_t d = 5;
uint64_t f (g) {
  printf ("%d,%d,%d,%d\n", g, c[0][1], g, e.a);
  if (safe_mul_func_int64_t_s_s (g, c[0][1])) e = b[8];
  printf ("%d,%d,%d,%d\n", g, c[0][1], g, e.a);
  return g;
}
uint8_t h () {
  int32_t *i = &d;
  return *i;
}
int main () {
  f (h ());
  printf ("%d\n", e.a);
}
