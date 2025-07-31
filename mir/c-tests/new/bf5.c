int printf (const char *, ...);
struct S0 {
  int f0;
  unsigned char f1;
  signed f2 : 12;
};
struct {
  struct S0 f0;
} g_1541 = {0xB98D53C7L, 4169L, 9};
int main () { printf ("%d,%d\n", g_1541.f0.f2, sizeof (g_1541)); }
