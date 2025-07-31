#include <stdio.h>

#define EMPTY_STRUCT_DECLARATION

struct abc {
  // EMPTY_STRUCT_DECLARATION  //without semicolon, can run with -ei/-eg
  EMPTY_STRUCT_DECLARATION;  // syntax error on struct (expected '<declarator>')
};

int main (int argc, char** argv) {
  printf ("from main\n");
  return 0;
}
