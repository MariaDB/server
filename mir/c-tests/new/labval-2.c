extern void printf (const char *, ...);
int r, b;
int main (void) {
  void *a = &&lab;
  void *a2[] = {&&lab};
  void *a3[] = {(char *) &&lab + 20};
  static void *sa = &&lab;
  static void *sa2[] = {&&lab};
  static void *sa3[] = {(char *) &&lab + 20};
  goto *sa2[0];
  return 1;
lab:
  printf ("ok\n");
lab2:
  return 0;
}
