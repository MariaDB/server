#define eq(v, p) __builtin_prop_eq (v, p)
void printf (const char *, ...);
static int f (int n) {
  printf ("checking case %d\n", n);
  return 1;
}
int main (void) {
  int i1 = 1, i2 = 3;
  __builtin_prop_set (i1, 0);
  __builtin_prop_set (i2, 0);
  if (eq (i1, 1) && eq (i2, 1) || eq (i1, 0) && eq (i2, 0) && f (0) && (i1 & i2 & 1) == 1) {
    /* both 0 bits are 1 */
    printf ("bit 0\n");
    return 0;
  } else if (eq (i1, 2) && eq (i2, 2) || eq (i1, 0) && eq (i2, 0) && f (1) && (i1 & i2 & 3) == 2) {
    /* both (1,0) bits are 2 */
    printf ("bit 1\n");
  } else {
    printf ("mix\n");
  }
  return 1;
}
