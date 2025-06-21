#include <stdint.h>
int printf (const char *, ...);
struct {
  signed d : 5;
  uint8_t e;
  signed f : 6;
} g = {2, 1, 6};
int main () { printf ("%d\n", g.f); }
