double d[10];
int main (void) {
  int i;
  __builtin_prop_set (i, 1);
  __builtin_prop_set (d[5], 1);
  return 0;
}
