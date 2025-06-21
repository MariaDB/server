#include <stdio.h>
struct Boo {
  char data[5];
};
struct Boo boo = {"test"};
int main (int argc, char **argv) {
  puts (boo.data);
  return 0;
}
