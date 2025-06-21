extern void printf (const char *, ...);
int main (void) {
  void *addr = &&lab;
  goto *addr;
  return 1;
lab:
  printf ("ok\n");
  return 0;
}
