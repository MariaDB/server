extern int printf (const char *, ...);
int a, c;
int *b = &a;
void d (int e, ...) {
  *b = e;
  for (; 0;)
    for (;; e++)
      for (; 0;)
        ;
}
int main (void) {
  d (1, c);
  printf ("%d\n", a);
  return 0;
}
