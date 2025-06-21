#include <stdio.h>
void* print_identity (void* x) {
  printf ("in print_identity: %p\n", x);
  return x;
}

int print_and_return_zero (void* t) {
  printf ("in print_and_return_zero: %p\n", t);
  return 0;
}

extern void* iteration (void*);
void* (*v) (void*) = iteration;

int main (void) {
  v ((void*) 0xdeadbeaf);
  return 0;
}
