#include <stdio.h>

struct string {
  char* begin;
  char* end;
};

unsigned long size (struct string a) { return (a.end - a.begin) / sizeof (char); }

int main (void) {
  char c[3] = "foo";
  struct string s = {c, c + 3};
  printf ("%lu\n", size (s));
  return 0;
}
