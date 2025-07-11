#include <assert.h>

struct foo {
  char c;
  int y;
#ifdef _WIN32
  long long x;
#else
  long x;
#endif
};

struct s2 {
  int foo, bar;
  int b;
};

struct s3 {
  char *c;
  int i;
};

struct s4 {
  char foo[7];
  int l;
};

struct s5 {
  char c;
};

struct s6 {
  char c[9];
};

struct s7 {
  int x;
  int y;
  char *name;
  char tag[3];
};

struct s8 {
  char s[3];
  char c[5];
};

struct s9 {
  short s;
  char c[3];
};

struct s10 {
  short s;
  int i[2];
};

int main () {
  assert (sizeof (struct foo) == 16);
  assert (sizeof (struct s2) == 12);
  assert (sizeof (struct s3) == 16);
  assert (sizeof (struct s4) == 12);
  assert (sizeof (struct s5) == 1);
  assert (sizeof (struct s6) == 9);
  assert (sizeof (struct s7) == 24);
  assert (sizeof (struct s8) == 8);
  assert (sizeof (struct s9) == 6);
  assert (sizeof (struct s10) == 12);

  return 0;
}
